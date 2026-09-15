#include "NativeAgendaHeader.h"
#include "NativeLineFactory.h"
#include "NativeObjectAccess.h"
#include "NativeStrokeSampling.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDate>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainterPath>
#include <QRawFont>
#include <QRegularExpression>
#include <QThread>
#include <QTimer>
#include <QTransform>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <new>
#if defined(REPAPER_WITH_NATIVE_ABI) && defined(__aarch64__)
#include "rm_Line.hpp"
#include "rm_SceneItem.hpp"
#endif

namespace {
constexpr int MaximumStrokes = 128;
const char Source[] = "/usr/share/remarkable/templates/P Day.template";
const QByteArray SourceHash("c2ca342a59e2140f012986fa208dc472109135e4887ae07070627d467247e626");
bool fieldsForHeader(const QVariantMap &fields, QStringList *values) {
    for (const auto *key : {"day", "date", "time"}) {
        const auto value = fields.value(key);
        if (value.metaType().id() != QMetaType::QString || value.toString().size() > 64
                || value.toString().contains(QChar::Null)) return false;
        values->append(value.toString().simplified().normalized(QString::NormalizationForm_C));
    }
    (*values)[1].remove(' '); (*values)[2].remove(' ');
    return !values->at(0).isEmpty() && values->at(0).size() <= 16
        && QDate::fromString(values->at(1), QStringLiteral("dd/MM/yyyy")).isValid()
        && (values->at(2).isEmpty() || QRegularExpression(QStringLiteral(
            "^(?:[01][0-9]|2[0-3]):[0-5][0-9][–-](?:[01][0-9]|2[0-3]):[0-5][0-9]$"))
                .match(values->at(2)).hasMatch());
}
QRectF strokeBounds(const QVector<PaperDrawing::Stroke> &strokes) {
    QRectF result; bool first = true;
    for (const auto &stroke : strokes) for (const auto &point : stroke.points) {
        const QRectF part(point - QPointF(stroke.width / 2, stroke.width / 2), QSizeF(stroke.width, stroke.width));
        result = first ? part : result.united(part); first = false;
    }
    return result;
}
QByteArray strokeBytes(const PaperDrawing::Stroke &stroke) {
    QByteArray bytes; QDataStream out(&bytes, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_6_0);
    out << qint32(19) << quint32(stroke.color.rgba()) << qint32(qRound(stroke.width * 4))
        << qint32(stroke.points.size());
    for (const auto &point : stroke.points) out << double(float(point.x())) << double(float(point.y()));
    return bytes;
}
QVector<PaperDrawing::Stroke> lettering(const QByteArray &data, const QString &text, qreal size, QPointF baseline) {
    QRawFont font(data, size, QFont::PreferNoHinting);
    if (!font.isValid()) return {};
    const auto glyphs = font.glyphIndexesForString(text);
    const auto advances = font.advancesForGlyphIndexes(glyphs);
    if (glyphs.size() != advances.size()) return {};
    QPainterPath path;
    for (qsizetype i = 0; i < glyphs.size(); ++i) {
        if (!glyphs[i]) return {};
        path.addPath(font.pathForGlyph(glyphs[i]).translated(baseline)); baseline += advances[i];
    }
    QVector<PaperDrawing::Stroke> result;
    const qreal width = qRound(qBound(qreal(.5), size / 24, qreal(2)) * 4) / 4.;
    for (const auto &polygon : path.toSubpathPolygons()) {
        if (polygon.size() < 2) continue;
        const auto sampled = RePaperNative::sampleStrokeForNativeSelection(polygon);
        if (!sampled.valid()) return {};
        PaperDrawing::Stroke stroke; stroke.width = width; stroke.color = Qt::black;
        for (const auto &point : sampled.points) stroke.points.append(QPointF(float(point.x()), float(point.y())));
        result.append(std::move(stroke));
    }
    return result;
}
// Recovery requires all exact contours; occupied fields are never overwritten.
QString inspectHeader(const NativeAgendaHeader::Plan &plan,
                      const RePaperNative::NativeObjectSnapshot &snapshot,
                      bool *existing, QVector<quint64> *matchedIds = nullptr) {
    if (!plan.valid()) return QStringLiteral("agenda-header-plan-invalid");
    if (!snapshot.complete || !snapshot.sceneIdentity) return QStringLiteral("agenda-header-scene-unconfirmed");
    if (!snapshot.pendingEditIdentity) return QStringLiteral("agenda-header-pending-edit");
    if (!snapshot.nativeSelectionExact || !snapshot.selectedIds.isEmpty())
        return QStringLiteral("agenda-header-selection-active");
    if (!snapshot.history.valid) return QStringLiteral("agenda-header-history-unavailable");
    if (snapshot.unsupportedItemCount) return QStringLiteral("agenda-header-unsupported-content");
    const auto baselineError = RePaperNative::ObjectAccessDetail::insertionBaselineError(snapshot, snapshot);
    if (!baselineError.isEmpty()) return baselineError;
    QMap<QByteArray, int> expected;
    for (const auto &stroke : plan.strokes) ++expected[strokeBytes(stroke)];
    int matched = 0;
    for (const auto &line : snapshot.lines) {
        const auto bytes = strokeBytes(line.stroke);
        if (line.tool == 19 && line.uniformPointWidth && line.stroke.color == Qt::black
                && line.maximumPointWidth == qRound(line.stroke.width * 4) && expected.value(bytes) > 0) {
            --expected[bytes]; ++matched;
            if (matchedIds) matchedIds->append(line.id);
            continue;
        }
        const auto bounds = strokeBounds({line.stroke});
        for (const auto &region : plan.fieldRegions)
            if (bounds.intersects(region.toRectF())) return QStringLiteral("agenda-header-fields-occupied");
    }
    if (matched && matched != plan.strokes.size()) return QStringLiteral("agenda-header-partial-content");
    *existing = matched == plan.strokes.size();
    return {};
}
}

NativeAgendaHeader::NativeAgendaHeader(QObject *parent)
    : NativeAgendaHeader(new NativeObjectAccess, {}, QString::fromLatin1(Source), SourceHash, parent) {}
NativeAgendaHeader::NativeAgendaHeader(QString source, QString directory, QByteArray hash, QObject *parent)
    : NativeAgendaHeader(new NativeObjectAccess, {}, std::move(source), std::move(hash), parent) { Q_UNUSED(directory); }
NativeAgendaHeader::NativeAgendaHeader(NativeObjectAccess *access, QByteArray fontData,
                                     QString source, QByteArray hash, QObject *parent)
    : QObject(parent), m_access(access), m_source(std::move(source)), m_sourceHash(std::move(hash)), m_fontData(std::move(fontData)) {
    Q_ASSERT(m_access); m_access->setParent(this);
    connect(m_access, &NativeObjectAccess::finished, this, &NativeAgendaHeader::accessed);
}
NativeAgendaHeader::~NativeAgendaHeader() { m_access->cancel(); }
bool NativeAgendaHeader::busy() const { return m_phase != Phase::Idle; }
QVariantMap NativeAgendaHeader::ensure(const QVariantMap &fields) {
    QStringList values;
    if (!fieldsForHeader(fields, &values)) return {{"error", "agenda-header-invalid-fields"}};
    QFile file(m_source);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 256 * 1024) return {{"error", "agenda-header-source-unavailable"}};
    const auto original = file.readAll();
    if (QCryptographicHash::hash(original, QCryptographicHash::Sha256).toHex() != m_sourceHash)
        return {{"error", "agenda-header-template-unsupported"}};
    const auto document = QJsonDocument::fromJson(original).object();
    const auto items = document.value("items").toArray();
    const auto children = items.isEmpty() ? QJsonArray{} : items[0].toObject().value("children").toArray();
    if (document.value("orientation") != "portrait" || children.size() != 4
            || children[0].toObject().value("text") != "Day:" || children[1].toObject().value("text") != "TIME"
            || children[2].toObject().value("id") != "group-date-item"
            || children[3].toObject().value("id") != "two-h-lines") return {{"error", "agenda-header-template-unsupported"}};
    return {{"templateName", "P Day"}, {"storage", "native-ink"}, {"sha256", QString::fromLatin1(m_sourceHash)}};
}

NativeAgendaHeader::Plan NativeAgendaHeader::createPlan(const QVariantMap &fields, const QRectF &paper, const QByteArray &fontData) {
    Plan plan; plan.paperBounds = paper;
    const auto fail = [&](const char *reason) { plan.reason = QString::fromLatin1(reason); return plan; };
    if (!std::isfinite(paper.x()) || !std::isfinite(paper.y()) || !std::isfinite(paper.width()) || !std::isfinite(paper.height())
            || paper.width() < 1404 || paper.width() > 3000 || paper.height() <= paper.width() || paper.height() > 5000
            || qAbs(paper.left() + paper.width() / 2) > 1 || qAbs(paper.top()) > 1)
        return fail("agenda-header-portrait-paper-required");
    if (fontData.isEmpty() || fontData.size() > 8 * 1024 * 1024) return fail("agenda-header-native-font-unavailable");
    QStringList values;
    if (!fieldsForHeader(fields, &values)) return fail("agenda-header-invalid-fields");
    const auto date = values[1].split('/');
    const auto add = [&](const char *name, const QString &text, qreal size, QPointF baseline, const QRectF &region) {
        plan.fieldRegions.insert(QString::fromLatin1(name), region);
        if (text.isEmpty()) { plan.fieldBounds.insert(QString::fromLatin1(name), QRectF{}); return true; }
        while (size >= 12) {
            const auto strokes = lettering(fontData, text, size, baseline);
            if (strokes.isEmpty()) return false;
            const auto bounds = strokeBounds(strokes);
            if (region.contains(bounds)) {
                plan.strokes += strokes; plan.fieldBounds.insert(QString::fromLatin1(name), bounds); return true;
            }
            --size;
        }
        return false;
    };
    // P Day constants use the centered scene origin. Preserve the DATE slashes
    // already present in the template at x=590 and x=635, baseline y=205.
    if (!add("day", values[0], 56, {-160, 95}, {-166, 20, paper.right() + 146, 110})
            || !add("time", values[2], 32, {-530, 205}, {-536, 155, 970, 80})
            || !add("dateDay", date[0], 24, {563, 205}, {560, 170, 29, 60})
            || !add("dateMonth", date[1], 24, {608, 205}, {605, 170, 29, 60})
            || !add("dateYear", date[2], 22, {654, 205}, {650, 170, paper.right() - 662, 60}))
        return fail("agenda-header-field-does-not-fit");
    plan.inkBounds = strokeBounds(plan.strokes);
    qsizetype points = 0;
    for (const auto &stroke : plan.strokes) points += stroke.points.size();
    if (plan.strokes.isEmpty() || plan.strokes.size() > MaximumStrokes
            || points > RePaperNative::NativeStrokePointBudget || !paper.contains(plan.inkBounds))
        return fail("agenda-header-stroke-budget");
    QByteArray bytes; QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << QStringLiteral("native-ink-P-Day-header-v3") << paper << values
           << QCryptographicHash::hash(fontData, QCryptographicHash::Sha256);
    QList<QByteArray> strokes;
    for (const auto &stroke : plan.strokes) strokes.append(strokeBytes(stroke));
    std::sort(strokes.begin(), strokes.end());
    for (const auto &stroke : strokes) stream << stroke;
    plan.hash = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    return plan;
}

bool NativeAgendaHeader::refuse(const QString &reason) {
    // Codes only: no notebook identifiers or requested content in diagnostics.
    std::fprintf(stderr, "[RePaper agenda] native header refused phase=%d code=%s\n",
                 int(m_phase), reason.toLatin1().constData());
    std::fflush(stderr);
    m_reason = reason; emit changed(); return false;
}
void NativeAgendaHeader::reset() {
    ++m_epoch; m_completionRetries = 0; disconnect(m_destroyed);
    m_phase = Phase::Idle; m_controller.clear(); m_context.clear(); m_request.clear(); m_baseline = {}; m_existing = false;
}
bool NativeAgendaHeader::current() const {
    if (!m_controller || m_controller->thread() != QThread::currentThread()
            || m_controller->property("pageId").toString() != m_context.value("pageId").toString()
            || m_controller->property("currentLayer") != m_context.value("layer")
            || m_controller->property("working").toBool()
            || m_controller->property("paperPortraitBounds").toRectF() != m_plan.paperBounds
            || m_controller->property("rootDocumentLength").toInt() != m_rootTextLength
            || m_controller->property("hasTextSelection").toBool()
            || m_controller->property("selectionItemCount").toInt() != 0) return false;
    auto *worker = m_controller->property("worker").value<QObject *>();
    const auto pending = m_controller->property("pendingEdit");
    return worker && worker->property("jobQueueSize").toInt() == 0
        && pending.canConvert<QTransform>() && pending.value<QTransform>().isIdentity();
}
bool NativeAgendaHeader::prepare(const QString &request, QObject *controller, const QVariantMap &context,
                                 const QVariantMap &fields, const QSizeF &pageSize) {
    if (busy() || m_access->busy()) return refuse(QStringLiteral("agenda-header-busy"));
    if (!QCoreApplication::instance() || QThread::currentThread() != QCoreApplication::instance()->thread()
            || !controller || controller->thread() != QThread::currentThread() || request.isEmpty() || request.size() > 128
            || request.contains(QChar::Null) || context.value("documentId").toString().isEmpty()
            || context.value("pageId").toString().isEmpty()) return refuse(QStringLiteral("agenda-header-invalid-context"));
    bool layerValid = false; const int layer = context.value("layer").toInt(&layerValid);
    for (const auto *property : {"working", "rootDocumentLength", "hasTextSelection", "selectionItemCount", "paperPortraitBounds"})
        if (!controller->property(property).isValid()) return refuse(QStringLiteral("agenda-header-invalid-context"));
    const auto bounds = controller->property("paperPortraitBounds");
    if (!layerValid || layer < 0 || !bounds.canConvert<QRectF>() || !std::isfinite(pageSize.width())
            || !std::isfinite(pageSize.height()) || pageSize.width() <= 0 || pageSize.height() <= 0
            || qAbs(bounds.toRectF().width() / bounds.toRectF().height()
                - qMin(pageSize.width(), pageSize.height()) / qMax(pageSize.width(), pageSize.height())) > .02)
        return refuse(QStringLiteral("agenda-header-invalid-context"));
    const auto source = ensure(fields);
    if (source.contains("error")) return refuse(source.value("error").toString());
    if (m_fontData.isEmpty()) {
        QFile font(QStringLiteral(":/reMarkableSans-Regular.ttf"));
        if (font.open(QIODevice::ReadOnly) && font.size() <= 8 * 1024 * 1024) m_fontData = font.readAll();
    }
    m_plan = createPlan(fields, bounds.toRectF(), m_fontData);
    if (!m_plan.valid()) return refuse(m_plan.reason);
    m_controller = controller; m_context = context; m_context["layer"] = layer; m_request = request;
    m_rootTextLength = controller->property("rootDocumentLength").toInt();
    if (!current()) { reset(); return refuse(QStringLiteral("agenda-header-page-not-ready")); }
    m_destroyed = connect(controller, &QObject::destroyed, this, [this] {
        if (busy()) complete(false, QStringLiteral("agenda-header-controller-destroyed"));
    });
    m_reason.clear(); m_phase = Phase::Inspecting; emit changed();
    if (m_access->inspect(controller, m_context)) return true;
    const auto error = m_access->reason(); reset(); return refuse(error);
}
QVariantMap NativeAgendaHeader::receipt(const QString &state) const {
    return {{"state", state}, {"planHash", m_plan.hash}, {"strokeCount", m_plan.strokes.size()},
            {"fieldBounds", m_plan.fieldBounds}, {"format", "native-ink-P-Day-header-v3"}, {"templateName", "P Day"}};
}
void NativeAgendaHeader::complete(bool success, const QString &reason) {
    std::fprintf(stderr, "[RePaper agenda] native header finished phase=%d success=%d code=%s\n",
                 int(m_phase), int(success), reason.toLatin1().constData());
    std::fflush(stderr);
    const auto request = m_request;
    const auto result = success ? receipt(m_existing ? "already-present" : "inserted")
        : QVariantMap{{"code", reason}, {"planHash", m_plan.hash}};
    reset(); m_reason = reason; emit changed(); emit finished(request, success, result);
}
void NativeAgendaHeader::accessed(bool success) {
    if (m_phase == Phase::Idle || m_phase == Phase::Prepared) return;
    if (!success) { complete(false, m_access->reason()); return; }
    auto *worker = m_controller ? m_controller->property("worker").value<QObject *>() : nullptr;
    if (m_controller && (m_controller->property("working").toBool()
            || (worker && worker->property("jobQueueSize").toInt() != 0)) && m_completionRetries++ < 20) {
        const auto epoch = m_epoch;
        QTimer::singleShot(25, this, [this, epoch] { if (m_epoch == epoch) accessed(true); }); return;
    }
    if (!current()) { complete(false, QStringLiteral("agenda-header-page-changed")); return; }
    const auto snapshot = m_access->result();
    std::fprintf(stderr, "[RePaper agenda] native header inspected phase=%d complete=%d history=%d isolated=%d lines=%lld unsupported=%d selected=%lld\n",
                 int(m_phase), int(snapshot.complete), int(snapshot.history.valid), int(snapshot.history.appendIsolated),
                 static_cast<long long>(snapshot.lines.size()), int(snapshot.unsupportedItemCount),
                 static_cast<long long>(snapshot.selectedIds.size()));
    std::fflush(stderr);
    if (snapshot.documentId != m_context.value("documentId").toString()
            || snapshot.pageId != m_context.value("pageId").toString() || snapshot.layer != m_context.value("layer").toInt()) {
        complete(false, QStringLiteral("agenda-header-context-changed")); return;
    }
    bool existing = false; QVector<quint64> matched;
    const auto error = inspectHeader(m_plan, snapshot, &existing, &matched);
    if (!error.isEmpty()) { complete(false, error); return; }
    if (m_phase == Phase::Inspecting) {
        m_baseline = snapshot; m_existing = existing; m_phase = Phase::Prepared; emit changed();
        emit prepared(m_request, receipt(existing ? "already-present" : "empty"));
    } else if (m_phase == Phase::VerifyingExisting) {
        const auto change = RePaperNative::ObjectAccessDetail::insertionBaselineError(m_baseline, snapshot);
        complete(existing && change.isEmpty(), change.isEmpty() && !existing ? QStringLiteral("agenda-header-content-changed") : change);
    } else {
        auto inserted = m_access->insertedIds();
        std::sort(inserted.begin(), inserted.end()); std::sort(matched.begin(), matched.end());
        auto preserved = snapshot;
        preserved.lines.erase(std::remove_if(preserved.lines.begin(), preserved.lines.end(),
            [&](const auto &line) { return inserted.contains(line.id); }), preserved.lines.end());
        if (!existing || inserted.size() != m_plan.strokes.size() || inserted != matched
                || !RePaperNative::sameNativeObjectContent(m_baseline, preserved)) {
            complete(false, QStringLiteral("agenda-header-inserted-ink-unconfirmed")); return;
        }
        complete(true);
    }
}
bool NativeAgendaHeader::commit(const QString &request) {
    if (m_phase != Phase::Prepared || request != m_request) return refuse(QStringLiteral("agenda-header-request-not-prepared"));
    if (!current()) { reset(); return refuse(QStringLiteral("agenda-header-page-changed")); }
    bool accepted = false;
    if (m_existing) { m_phase = Phase::VerifyingExisting; accepted = m_access->inspect(m_controller, m_context); }
    else {
        const auto items = nativeItems(m_plan);
        if (!items.isValid()) { reset(); return refuse(QStringLiteral("agenda-header-native-items-unavailable")); }
        if (!current()) { reset(); return refuse(QStringLiteral("agenda-header-page-changed")); }
        m_phase = Phase::Inserting;
        accepted = m_access->insertIfUnchanged(m_controller, m_context, m_baseline, items, m_plan.inkBounds.center());
    }
    if (!accepted) { const auto error = m_access->reason(); reset(); return refuse(error); }
    emit changed(); return true;
}
void NativeAgendaHeader::cancel(const QString &request) {
    if (!busy() || request != m_request) return;
    reset(); m_access->cancel(); m_reason = QStringLiteral("agenda-header-cancelled"); emit changed();
}
QVariant NativeAgendaHeader::nativeItems(const Plan &plan) {
#if defined(REPAPER_WITH_NATIVE_ABI) && defined(__aarch64__)
    using Items = QList<std::shared_ptr<SceneItem>>;
    const auto type = QMetaType::fromName("QList<std::shared_ptr<SceneItem>>");
    if (!plan.valid() || plan.strokes.size() > MaximumStrokes || !type.isValid() || type.sizeOf() != sizeof(Items)) return {};
    Items result;
    for (const auto &stroke : plan.strokes) {
        if (!current()) return {};
        auto item = RePaperNative::createNativeLineItem(&m_reason);
        if (!item) return {};
        QList<LinePoint> points; points.reserve(stroke.points.size());
        for (const auto &point : stroke.points)
            points.append({float(point.x()), float(point.y()), 25, quint16(qRound(stroke.width * 4)), 0, 255});
        auto line = Line::fromPoints(std::move(points), strokeBounds({stroke}));
        auto *nativeLine = std::launder(reinterpret_cast<Line *>(reinterpret_cast<unsigned char *>(item.get()) + 0x48));
        *nativeLine = std::move(line); result.append(std::move(item));
    }
    return QVariant(type, &result);
#else
    Q_UNUSED(plan); return {};
#endif
}
