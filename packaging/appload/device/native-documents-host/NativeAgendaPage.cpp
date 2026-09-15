#include "NativeAgendaPage.h"
#include "NativeLineFactory.h"
#include "NativeObjectAccess.h"
#include <QCoreApplication>
#include <QFile>
#include <QThread>
#include <QTimer>
#include <QTransform>
#include <cmath>
#include <new>
#if defined(REPAPER_WITH_NATIVE_ABI) && defined(__aarch64__)
#include "rm_Line.hpp"
#include "rm_SceneItem.hpp"
#endif

namespace {
QByteArray nativeFont() {
    QFile file(QStringLiteral(":/reMarkableSans-Regular.ttf"));
    return file.open(QIODevice::ReadOnly) && file.size() <= 8 * 1024 * 1024 ? file.readAll() : QByteArray{};
}
bool emptyText(QObject *controller) {
    bool valid = false;
    const auto value = controller->property("rootDocumentLength");
    const auto length = value.toLongLong(&valid);
    return value.isValid() && valid && length == 0;
}
QStringList ids(const RePaperNative::NativeObjectSnapshot &snapshot) {
    QStringList result;
    for (const auto &line : snapshot.lines) result.append(QString::number(line.id));
    return result;
}
}

NativeAgendaPage::NativeAgendaPage(QObject *parent)
    : NativeAgendaPage(new NativeObjectAccess, {}, parent) {}

NativeAgendaPage::NativeAgendaPage(NativeObjectAccess *access, QByteArray fontData, QObject *parent)
    : QObject(parent), m_access(access), m_fontData(std::move(fontData)) {
    Q_ASSERT(m_access);
    m_access->setParent(this);
    connect(m_access, &NativeObjectAccess::finished, this, &NativeAgendaPage::accessed);
}
NativeAgendaPage::~NativeAgendaPage() { m_access->cancel(); }
bool NativeAgendaPage::busy() const { return m_phase != Phase::Idle; }
bool NativeAgendaPage::refuse(const QString &code) {
    m_reason = code; emit changed(); return false;
}
void NativeAgendaPage::reset() {
    ++m_epoch; m_completionRetries = 0;
    disconnect(m_destroyed);
    m_phase = Phase::Idle; m_controller.clear(); m_context.clear();
    m_baseline = {}; m_request.clear(); m_existing = false;
}
bool NativeAgendaPage::current() const {
    if (!m_controller || m_controller->thread() != QThread::currentThread()
        || m_controller->property("pageId").toString() != m_context.value("pageId").toString()
        || m_controller->property("currentLayer") != m_context.value("layer")
        || m_controller->property("working").toBool() || !emptyText(m_controller)) return false;
    const auto transform = m_controller->property("pendingEdit");
    const auto bounds = m_controller->property("paperNoteBounds");
    return transform.canConvert<QTransform>() && transform.value<QTransform>().isIdentity()
        && bounds.canConvert<QRectF>() && bounds.toRectF() == m_plan.paperBounds;
}

bool NativeAgendaPage::prepare(const QString &request, QObject *controller,
    const QVariantMap &context, const QVariantMap &fields, const QSizeF &pageSize) {
    if (busy() || m_access->busy()) return refuse(QStringLiteral("agenda-busy"));
    if (!QCoreApplication::instance() || QThread::currentThread() != QCoreApplication::instance()->thread()
        || !controller || controller->thread() != QThread::currentThread()
        || request.isEmpty() || request.size() > 128 || request.contains(QChar::Null)
        || context.value("documentId").toString().isEmpty() || context.value("documentId").toString().size() > 128
        || context.value("pageId").toString().isEmpty() || context.value("pageId").toString().size() > 128)
        return refuse(QStringLiteral("agenda-invalid-context"));
    bool layerValid = false;
    const int layer = context.value("layer").toInt(&layerValid);
    const auto nativeBounds = controller->property("paperNoteBounds");
    if (!layerValid || layer < 0 || !controller->property("working").isValid()
        || !controller->property("hasAnnotations").isValid() || !nativeBounds.canConvert<QRectF>()
        || !std::isfinite(pageSize.width()) || !std::isfinite(pageSize.height())
        || pageSize.width() <= 0 || pageSize.height() <= 0)
        return refuse(QStringLiteral("agenda-invalid-context"));
    const auto paper = nativeBounds.toRectF();
    const qreal ratio = qMax(pageSize.width(), pageSize.height()) / qMin(pageSize.width(), pageSize.height());
    if (paper.height() <= 0 || qAbs(paper.width() / paper.height() - ratio) > .02)
        return refuse(QStringLiteral("agenda-paper-size-mismatch"));
    if (m_fontData.isEmpty()) m_fontData = nativeFont();
    m_plan = NativeAgendaLayout::create(fields, paper, m_fontData);
    if (!m_plan.valid()) return refuse(m_plan.reason);
    m_controller = controller; m_context = context; m_context["layer"] = layer; m_request = request;
    if (!current()) { reset(); return refuse(QStringLiteral("agenda-page-not-ready")); }
    m_destroyed = connect(controller, &QObject::destroyed, this, [this] {
        if (!busy()) return;
        const auto request = m_request;
        cancel(request);
        emit finished(request, false, {{"code", "agenda-controller-destroyed"}, {"planHash", m_plan.hash}});
    });
    m_reason.clear(); m_phase = Phase::Inspecting; emit changed();
    if (m_access->inspect(controller, m_context)) return true;
    const auto code = m_access->reason(); reset(); return refuse(code);
}

QVariantMap NativeAgendaPage::receipt(const QString &state) const {
    return {{"state", state}, {"planHash", m_plan.hash}, {"displayedTitle", m_plan.displayedTitle},
            {"strokeCount", m_plan.strokes.size()}};
}
void NativeAgendaPage::complete(bool success, const QString &code) {
    const auto request = m_request;
    QVariantMap result = success ? receipt(m_existing ? "already-present" : "inserted")
                                : QVariantMap{{"code", code}, {"planHash", m_plan.hash}};
    if (success) result["insertedIds"] = ids(m_access->result());
    reset(); m_reason = code; emit changed(); emit finished(request, success, result);
}

void NativeAgendaPage::accessed(bool success) {
    if (m_phase == Phase::Idle || m_phase == Phase::Prepared) return;
    if (!success) { complete(false, m_access->reason()); return; }
    // DocumentWorker's completion signal can reach us before SceneController's
    // own slot clears working. Let that delivery unwind without interpreting
    // the native job itself as an unrelated active edit. Cancellation fences
    // this bounded retry, including a later request reusing the same key.
    if (m_controller && m_controller->property("working").toBool() && m_completionRetries++ < 20) {
        const auto epoch = m_epoch;
        QTimer::singleShot(25, this, [this, epoch] { if (epoch == m_epoch) accessed(true); });
        return;
    }
    m_completionRetries = 0;
    if (!current()) { complete(false, QStringLiteral("agenda-page-changed")); return; }
    const auto snapshot = m_access->result();
    if (!snapshot.complete || !snapshot.pendingEditIdentity || !snapshot.nativeSelectionExact
        || !snapshot.history.valid || !snapshot.sceneIdentity || snapshot.unsupportedItemCount
        || snapshot.documentId != m_context.value("documentId").toString()
        || snapshot.pageId != m_context.value("pageId").toString()
        || snapshot.layer != m_context.value("layer").toInt() || !snapshot.selectedIds.isEmpty()) {
        complete(false, QStringLiteral("agenda-page-state-unconfirmed")); return;
    }
    if (m_phase == Phase::Inspecting) {
        if (snapshot.lines.isEmpty()) {
            if (m_controller->property("hasAnnotations").toBool()) {
                complete(false, QStringLiteral("agenda-page-has-other-content")); return;
            }
            m_existing = false;
        } else if (NativeAgendaLayout::matches(m_plan, snapshot)) m_existing = true;
        else { complete(false, QStringLiteral("agenda-page-has-other-content")); return; }
        m_baseline = snapshot;
        m_phase = Phase::Prepared; emit changed();
        emit prepared(m_request, receipt(m_existing ? "already-present" : "empty"));
    } else if (m_phase == Phase::VerifyingExisting) {
        const auto error = RePaperNative::ObjectAccessDetail::insertionBaselineError(m_baseline, snapshot);
        if (!error.isEmpty()) { complete(false, error); return; }
        complete(NativeAgendaLayout::matches(m_plan, snapshot), QString{});
    } else if (m_phase == Phase::Inserting) {
        if (!NativeAgendaLayout::matches(m_plan, snapshot)
            || m_access->insertedIds().size() != m_plan.strokes.size()) {
            complete(false, QStringLiteral("agenda-inserted-ink-unconfirmed")); return;
        }
        complete(true);
    }
}

bool NativeAgendaPage::commit(const QString &request) {
    if (m_phase != Phase::Prepared || request != m_request) return refuse(QStringLiteral("agenda-request-not-prepared"));
    if (!current()) { reset(); return refuse(QStringLiteral("agenda-page-changed")); }
    bool accepted = false;
    if (m_existing) {
        m_phase = Phase::VerifyingExisting;
        accepted = m_access->inspect(m_controller, m_context);
    } else {
        const auto items = nativeItems(m_plan);
        if (!items.isValid()) { reset(); return refuse(QStringLiteral("agenda-native-items-unavailable")); }
        if (!current()) { reset(); return refuse(QStringLiteral("agenda-page-changed")); }
        m_phase = Phase::Inserting;
        accepted = m_access->insertIfUnchanged(m_controller, m_context, m_baseline, items, m_plan.inkBounds.center());
    }
    if (!accepted) { const auto code = m_access->reason(); reset(); return refuse(code); }
    emit changed(); return true;
}

void NativeAgendaPage::cancel(const QString &request) {
    if (!busy() || request != m_request) return;
    reset(); m_access->cancel(); m_reason = QStringLiteral("agenda-cancelled"); emit changed();
}

QVariant NativeAgendaPage::nativeItems(const NativeAgendaLayout::Plan &plan) {
#if defined(REPAPER_WITH_NATIVE_ABI) && defined(__aarch64__)
    using Items = QList<std::shared_ptr<SceneItem>>;
    const auto type = QMetaType::fromName("QList<std::shared_ptr<SceneItem>>");
    if (!plan.valid() || plan.strokes.size() > NativeAgendaLayout::MaximumStrokes
        || !type.isValid() || type.sizeOf() != sizeof(Items)) return {};
    Items result;
    for (const auto &stroke : plan.strokes) {
        if (!current()) return {};
        auto item = RePaperNative::createNativeLineItem(&m_reason);
        if (!item) return {};
        QList<LinePoint> points;
        points.reserve(stroke.points.size());
        for (const auto &point : stroke.points)
            points.append({float(point.x()), float(point.y()), 25, quint16(qRound(stroke.width * 4)), 0, 255});
        auto line = Line::fromPoints(std::move(points), NativeAgendaLayout::bounds({stroke}));
        auto *nativeLine = std::launder(reinterpret_cast<Line*>(reinterpret_cast<unsigned char*>(item.get()) + 0x48));
        *nativeLine = std::move(line);
        result.append(std::move(item));
    }
    return QVariant(type, &result);
#else
    Q_UNUSED(plan); return {};
#endif
}
