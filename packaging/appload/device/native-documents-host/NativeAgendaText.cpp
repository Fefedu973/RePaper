#include "NativeAgendaText.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QRegularExpression>
#include <QDebug>
#include <QIODevice>
#include <QMetaEnum>
#include <QMetaMethod>
#include <QThread>
#include <QTransform>
#include <cmath>
#include <cstdio>

namespace {
QMetaMethod method(QObject *object, const char *signature) {
    if (!object) return {};
    const auto *meta = object->metaObject();
    const int index = meta->indexOfMethod(signature);
    return index >= 0 ? meta->method(index) : QMetaMethod{};
}
bool invoke(QObject *object, const char *name) {
    return QMetaObject::invokeMethod(object, name, Qt::DirectConnection);
}
bool integerProperty(QObject *object, const char *name, qlonglong expected) {
    bool valid = false;
    const auto value = object->property(name);
    return value.isValid() && value.toLongLong(&valid) == expected && valid;
}
}

NativeAgendaText::NativeAgendaText(QObject *parent) : QObject(parent) {
    m_poll.setInterval(25);
    connect(&m_poll, &QTimer::timeout, this, &NativeAgendaText::advance);
}

bool NativeAgendaText::refuse(const QString &code) {
    m_reason = code;
    emit changed();
    return false;
}

void NativeAgendaText::reset() {
    m_poll.stop();
    disconnect(m_destroyedConnection);
    m_phase = Phase::Idle;
    m_controller.clear();
    m_sceneView.clear();
    m_context.clear();
    m_request.clear();
    m_ticks = 0;
    m_selectionRequested = false;
    m_readError.clear();
    m_expectedBefore.clear();
    m_replacingLegacy = m_replacedLegacy = false;
    m_readLength = 0;
}

bool NativeAgendaText::current() const {
    if (!m_controller || m_controller->thread() != QThread::currentThread()
        || m_controller->property("pageId").toString() != m_context.value("pageId").toString()
        || m_controller->property("currentLayer") != m_context.value("layer")
        || m_controller->property("paperPortraitBounds").toRectF() != m_paper) return false;
    const auto pending = m_controller->property("pendingEdit");
    return pending.canConvert<QTransform>() && pending.value<QTransform>().isIdentity();
}

bool NativeAgendaText::idleWorker() const {
    if (!m_controller || !m_controller->property("working").isValid()
        || m_controller->property("working").toBool()) return false;
    const auto worker = m_controller->property("worker");
    if (auto *object = worker.value<QObject *>())
        return integerProperty(object, "jobQueueSize", 0);
    return false;
}

bool NativeAgendaText::emptyPage() const {
    return current() && integerProperty(m_controller, "rootDocumentLength", 0)
        && m_controller->property("hasAnnotations").isValid()
        && !m_controller->property("hasAnnotations").toBool()
        && integerProperty(m_controller, "selectionItemCount", 0)
        && m_controller->property("hasTextSelection").isValid()
        && !m_controller->property("hasTextSelection").toBool();
}

bool NativeAgendaText::readExactText(QString *text) const {
    // Xochitl's surrounding-text cache covers the current paragraphs only.
    // Selecting the entire root expands that cache before ImCurrentSelection.
    if (!m_sceneView || m_sceneView->thread() != QThread::currentThread()
        || m_sceneView->property("controller").value<QObject *>() != m_controller.data()) return false;
    QVariant result;
    const auto query = method(m_sceneView, "inputMethodQuery(Qt::InputMethodQuery,QVariant)");
    const bool invoked = query.isValid() && query.invoke(m_sceneView, Qt::DirectConnection,
            Q_RETURN_ARG(QVariant, result), Q_ARG(Qt::InputMethodQuery, Qt::ImCurrentSelection),
            Q_ARG(QVariant, QVariant{}));
    const auto value = result.toString();
    if (!invoked || result.metaType().id() != QMetaType::QString) return false;
    if (!integerProperty(m_controller, "rootDocumentLength", value.size())) return false;
    *text = value;
    return true;
}

bool NativeAgendaText::beginRead(Phase phase) {
    m_phase = phase;
    m_ticks = 0;
    m_readError.clear();
    m_readLength = m_controller->property("rootDocumentLength").toInt();
    if (!m_selectionRequested) {
        bool valid = false;
        m_savedCursor = m_controller->property("textCursorIndex").toInt(&valid);
        if (!valid || m_savedCursor < 0 || m_savedCursor > m_readLength) m_savedCursor = m_readLength;
    }
    m_savedCursor = qMin(m_savedCursor, m_readLength);
    m_selectionRequested = true;
    if (!invoke(m_controller, "focusRootDocument")
        || !QMetaObject::invokeMethod(m_controller, "selectTextRange", Qt::DirectConnection,
                                     Q_ARG(int, 0), Q_ARG(int, m_readLength))) {
        complete(false, QStringLiteral("agenda-text-selection-failed"));
        return false;
    }
    logState(QStringLiteral("reading-selection"));
    m_poll.start();
    return true;
}

void NativeAgendaText::restoreSelection() {
    if (!m_selectionRequested || !current()) return;
    invoke(m_controller, "clearSelectedText");
    QMetaObject::invokeMethod(m_controller, "setCursorIndex", Qt::DirectConnection, Q_ARG(int, m_savedCursor));
    m_selectionRequested = false;
}

void NativeAgendaText::finishRead(const QString &error) {
    const bool preparing = m_phase == Phase::ReadingExisting;
    m_readError = error;
    restoreSelection();
    m_phase = preparing ? Phase::RestoringForPrepare : Phase::RestoringForFinish;
    m_ticks = 0;
}

bool NativeAgendaText::titleStyle(int *value, QMetaMethod *create) const {
    *create = method(m_controller, "createRootDocument(scene::ParagraphStyle::Type)");
    if (!create->isValid()) return false;
    const auto type = create->parameterMetaType(0);
    const auto *meta = type.metaObject();
    if (!meta || type.sizeOf() != sizeof(int)) return false;
    for (int i = 0; i < meta->enumeratorCount(); ++i) {
        const auto enumeration = meta->enumerator(i);
        if (QByteArray(enumeration.name()) != "Type") continue;
        bool valid = false;
        *value = enumeration.keyToValue("Title", &valid);
        return valid;
    }
    return false;
}

QVariantMap NativeAgendaText::receipt(const QString &state) const {
    return {{"state", state}, {"planHash", m_hash}, {"displayedTitle", m_title},
            {"textLength", m_text.size()}, {"textWidth", m_width},
            {"migratedPreviousText", m_replacedLegacy}, {"format", "native-text-P-Day-header-v3"}};
}

void NativeAgendaText::logState(const QString &event) const {
    // Counts and state only; never log ids, names, or document text.
    auto property = [this](const char *name) {
        return m_controller ? m_controller->property(name) : QVariant{};
    };
    const bool hasRoot = property("hasRootDocument").toBool();
    const int length = hasRoot ? property("rootDocumentLength").toInt() : 0;
    const double width = hasRoot ? property("rootDocumentTextWidth").toDouble() : 0;
    // Keep diagnostics independent of the host's QVariant debug operators.
    // Before root creation, do not request root-dependent properties.
    std::fprintf(stderr, "[RePaper agenda] native text event=%s phase=%d root=%d length=%d expectedLength=%lld width=%.1f expectedWidth=%.1f working=%d\n",
                 event.toLatin1().constData(), int(m_phase), int(hasRoot), length,
                 static_cast<long long>(m_text.size()), width, double(m_width),
                 int(property("working").toBool()));
    std::fflush(stderr);
}

void NativeAgendaText::complete(bool success, const QString &code) {
    logState(success ? QStringLiteral("completed") : code);
    restoreSelection();
    const auto request = m_request;
    auto result = success ? receipt(m_existing && !m_replacedLegacy ? "already-present" : "inserted")
                          : QVariantMap{{"code", code}, {"planHash", m_hash}};
    reset();
    m_reason = code;
    emit changed();
    emit finished(request, success, result);
}

bool NativeAgendaText::prepare(const QString &request, QObject *controller,
    const QVariantMap &context, const QVariantMap &fields, const QSizeF &pageSize) {
    if (busy()) return refuse(QStringLiteral("agenda-busy"));
    if (fields.value("headerPrepared").metaType().id() != QMetaType::Bool
        || !fields.value("headerPrepared").toBool()
        || fields.value("headerPlanHash").metaType().id() != QMetaType::QString
        || !QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(fields.value("headerPlanHash").toString()).hasMatch())
        return refuse(QStringLiteral("agenda-template-header-not-prepared"));
    if (!QCoreApplication::instance() || QThread::currentThread() != QCoreApplication::instance()->thread()
        || !controller || controller->thread() != QThread::currentThread()
        || request.isEmpty() || request.size() > 128 || request.contains(QChar::Null)
        || context.value("documentId").toString().isEmpty() || context.value("pageId").toString().isEmpty())
        return refuse(QStringLiteral("agenda-invalid-context"));
    bool validLayer = false;
    const int layer = context.value("layer").toInt(&validLayer);
    // Xochitl exposes both coordinate extents independently of the document's
    // orientation: paperNoteBounds swaps the explicit paper width and height.
    // P Day uses the portrait coordinate extents.
    const auto bounds = controller->property("paperPortraitBounds");
    const auto paper = bounds.toRectF();
    QStringList geometryFailures;
    if (!validLayer || layer < 0) geometryFailures.append(QStringLiteral("layer"));
    if (!bounds.canConvert<QRectF>()) geometryFailures.append(QStringLiteral("bounds-type"));
    if (!std::isfinite(paper.x()) || !std::isfinite(paper.y())
        || !std::isfinite(paper.width()) || !std::isfinite(paper.height()))
        geometryFailures.append(QStringLiteral("bounds-finite"));
    if (paper.width() < 1000 || paper.width() > 3000 || paper.height() > 5000)
        geometryFailures.append(QStringLiteral("bounds-size"));
    if (paper.height() <= paper.width()) geometryFailures.append(QStringLiteral("portrait"));
    if (qAbs(paper.left() + paper.width() / 2) > 1 || qAbs(paper.top()) > 1)
        geometryFailures.append(QStringLiteral("origin"));
    if (!std::isfinite(pageSize.width()) || !std::isfinite(pageSize.height())
        || pageSize.width() <= 0 || pageSize.height() <= 0)
        geometryFailures.append(QStringLiteral("page-size"));
    if (qAbs(paper.width() / paper.height() - qMin(pageSize.width(), pageSize.height())
                                                      / qMax(pageSize.width(), pageSize.height())) > .02)
        geometryFailures.append(QStringLiteral("aspect-ratio"));
    if (!geometryFailures.isEmpty()) {
        // Geometry only: never include document ids, names, or requested text.
        qWarning() << "[RePaper agenda] rejected native page geometry"
                   << "paperPortraitBounds" << paper << "pageSize" << pageSize
                   << "validLayer" << validLayer << "layer" << layer
                   << "boundsType" << bounds.typeName() << "conditions" << geometryFailures;
        return refuse(QStringLiteral("agenda-portrait-paper-required"));
    }
    QStringList values;
    for (const auto *key : {"title", "day", "date", "time"}) {
        const auto value = fields.value(key);
        if (value.metaType().id() != QMetaType::QString || value.toString().size() > 512
            || value.toString().contains(QChar::Null)) return refuse(QStringLiteral("agenda-invalid-fields"));
        values.append(value.toString().simplified().normalized(QString::NormalizationForm_C));
    }
    if (values[0].isEmpty() || values[1].isEmpty() || values[2].isEmpty())
        return refuse(QStringLiteral("agenda-invalid-fields"));
    m_title = values.takeFirst();
    if (values.last().isEmpty()) values.removeLast();
    m_legacyText = m_title + '\n' + values.join(QStringLiteral(" — "));
    m_text = m_title;
    m_paper = paper;
    m_width = paper.width() - 372; // P Day writing area starts 186 units from the left.
    QByteArray plan;
    QDataStream stream(&plan, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << QStringLiteral("native-text-P-Day-header-v3") << m_paper << m_width << m_text
           << fields.value("headerPlanHash").toString();
    m_hash = QString::fromLatin1(QCryptographicHash::hash(plan, QCryptographicHash::Sha256).toHex());
    m_controller = controller;
    m_sceneView = context.value("sceneView").value<QObject *>();
    if (!m_sceneView) {
        // DocumentView exposes its controller but keeps the native SceneView
        // behind a QML id. Find that existing child through QObject metadata.
        auto *documentView = context.value("documentView").value<QObject *>();
        if (documentView && documentView->thread() == QThread::currentThread()) {
            for (auto *child : documentView->findChildren<QObject *>()) {
                if (child->property("controller").value<QObject *>() == controller
                    && method(child, "inputMethodQuery(Qt::InputMethodQuery,QVariant)").isValid()) {
                    m_sceneView = child;
                    break;
                }
            }
        }
    }
    m_context = context;
    m_context["layer"] = layer;
    m_request = request;
    int title = 0;
    QMetaMethod create;
    if (!current() || !idleWorker() || !method(controller, "focusRootDocument()").isValid()
        || !method(controller, "replaceText(QString)").isValid() || !titleStyle(&title, &create)
        || (m_sceneView && (!method(controller, "selectTextRange(int,int)").isValid()
            || !method(controller, "clearSelectedText()").isValid()
            || !method(controller, "setCursorIndex(int)").isValid()))) {
        reset(); return refuse(QStringLiteral("agenda-native-text-unavailable"));
    }
    m_existing = !integerProperty(controller, "rootDocumentLength", 0);
    if (m_existing && controller->property("hasTextSelection").toBool()) {
        reset(); return refuse(QStringLiteral("agenda-existing-selection-active"));
    }
    if (m_existing && (!m_sceneView
        || (!integerProperty(controller, "rootDocumentLength", m_text.size())
            && !integerProperty(controller, "rootDocumentLength", m_legacyText.size()))
        || qAbs(controller->property("rootDocumentTextWidth").toDouble() - m_width) >= .1)) {
        reset(); return refuse(QStringLiteral("agenda-existing-content-unconfirmed"));
    }
    if (!m_existing && !emptyPage()) {
        reset(); return refuse(QStringLiteral("agenda-existing-content-unconfirmed"));
    }
    m_destroyedConnection = connect(controller, &QObject::destroyed, this, [this] {
        if (busy()) complete(false, QStringLiteral("agenda-controller-destroyed"));
    });
    if (m_existing) return beginRead(Phase::ReadingExisting);
    m_phase = Phase::Prepared;
    m_reason.clear();
    emit changed();
    emit prepared(request, receipt(m_existing ? "already-present" : "empty"));
    return true;
}

bool NativeAgendaText::commit(const QString &request) {
    if (m_phase != Phase::Prepared || request != m_request)
        return refuse(QStringLiteral("agenda-request-not-prepared"));
    if (!current() || !idleWorker()) {
        reset(); return refuse(QStringLiteral("agenda-page-changed"));
    }
    if (m_existing) {
        if (!integerProperty(m_controller, "rootDocumentLength", m_expectedBefore.size())
            || qAbs(m_controller->property("rootDocumentTextWidth").toDouble() - m_width) >= .1) {
            reset(); return refuse(QStringLiteral("agenda-existing-content-unconfirmed"));
        }
        return beginRead(Phase::Verifying);
    }
    if (!emptyPage()) { reset(); return refuse(QStringLiteral("agenda-page-has-other-content")); }
    int title = 0;
    QMetaMethod create;
    if (!titleStyle(&title, &create)) { reset(); return refuse(QStringLiteral("agenda-native-text-unavailable")); }
    m_phase = Phase::Creating;
    m_ticks = 0;
    logState(QStringLiteral("creating"));
    const auto typeName = create.parameterTypes().first();
    if (!create.invoke(m_controller, Qt::DirectConnection, QGenericArgument(typeName.constData(), &title))
        || !invoke(m_controller, "focusRootDocument")) {
        complete(false, QStringLiteral("agenda-text-creation-failed")); return false;
    }
    m_poll.start();
    emit changed();
    return true;
}

void NativeAgendaText::advance() {
    if (m_phase == Phase::Idle || m_phase == Phase::Prepared) return;
    if (!current()) { complete(false, QStringLiteral("agenda-page-changed")); return; }
    if (++m_ticks > 320) { complete(false, QStringLiteral("agenda-native-text-timeout")); return; }
    if (m_ticks == 40 || m_ticks == 160) logState(QStringLiteral("waiting"));
    if (!idleWorker()) return;
    if (m_phase == Phase::RestoringForPrepare || m_phase == Phase::RestoringForFinish) {
        if (m_controller->property("hasTextSelection").toBool()) return;
        if (!m_readError.isEmpty()) { const auto error = m_readError; complete(false, error); return; }
        const int expectedLength = m_phase == Phase::RestoringForPrepare ? m_expectedBefore.size() : m_text.size();
        if (!integerProperty(m_controller, "rootDocumentLength", expectedLength)
            || qAbs(m_controller->property("rootDocumentTextWidth").toDouble() - m_width) >= .1) {
            complete(false, QStringLiteral("agenda-text-changed-during-confirmation")); return;
        }
        if (m_phase == Phase::RestoringForPrepare) {
            m_poll.stop();
            m_phase = Phase::Prepared;
            m_reason.clear();
            emit changed();
            emit prepared(m_request, receipt(QStringLiteral("already-present")));
        } else complete(true);
        return;
    }
    if (m_phase == Phase::ReadingExisting || m_phase == Phase::Verifying) {
        if (!m_sceneView) { complete(false, QStringLiteral("agenda-text-reader-destroyed")); return; }
        const auto status = m_sceneView->property("status");
        if ((status.isValid() && status.toInt() != 2)
            || !m_controller->property("hasTextSelection").toBool()) return;
        QString actual;
        if (!readExactText(&actual)) return;
        if (m_phase == Phase::ReadingExisting) {
            if (actual != m_text && actual != m_legacyText) {
                finishRead(QStringLiteral("agenda-text-content-unconfirmed")); return;
            }
            m_expectedBefore = actual;
            m_replacingLegacy = actual == m_legacyText;
            finishRead();
            return;
        }
        const auto expected = m_replacingLegacy && !m_replacedLegacy ? m_expectedBefore : m_text;
        if (actual != expected) { finishRead(QStringLiteral("agenda-text-content-unconfirmed")); return; }
        if (m_replacingLegacy && !m_replacedLegacy) {
            // The complete legacy payload is selected and freshly verified.
            // Xochitl's replaceText replaces that selection in its own history;
            // it does not affect handwriting or any other scene items.
            m_phase = Phase::Inserting;
            m_ticks = 0;
            m_replacedLegacy = true;
            m_savedCursor = qMin(m_savedCursor, int(m_text.size()));
            if (!QMetaObject::invokeMethod(m_controller, "replaceText", Qt::DirectConnection, Q_ARG(QString, m_text))) {
                complete(false, QStringLiteral("agenda-text-migration-failed")); return;
            }
            logState(QStringLiteral("migrating-legacy-text"));
            return;
        }
        finishRead();
        return;
    }
    if (m_phase == Phase::Creating) {
        // Empty root creation can publish property changes without updated().
        // Its observable postcondition and the completed native worker queue
        // are the completion boundary for this operation.
        if (!m_controller->property("hasRootDocument").toBool()) return;
        if (!emptyPage()) {
            complete(false, QStringLiteral("agenda-text-creation-unconfirmed")); return;
        }
        m_phase = Phase::Inserting;
        m_ticks = 0;
        logState(QStringLiteral("inserting"));
        if (!m_controller->setProperty("rootDocumentTextWidth", m_width)
            || !QMetaObject::invokeMethod(m_controller, "replaceText", Qt::DirectConnection, Q_ARG(QString, m_text))) {
            complete(false, QStringLiteral("agenda-text-insertion-failed")); return;
        }
        return;
    }
    if (!integerProperty(m_controller, "rootDocumentLength", m_text.size())
        || !m_controller->property("hasRootDocument").toBool()
        || qAbs(m_controller->property("rootDocumentTextWidth").toDouble() - m_width) >= .1) {
        // Qt can deliver the cached property changes after the worker becomes
        // idle. Wait for the complete postcondition instead of rejecting that
        // intermediate state or relying on the unrelated updated() signal.
        return;
    }
    const auto bounds = m_controller->property("rootDocumentBoundingRect").toRectF();
    if (!bounds.isValid() || !m_paper.contains(bounds)) {
        complete(false, QStringLiteral("agenda-text-outside-page")); return;
    }
    if (m_sceneView) beginRead(Phase::Verifying);
    else complete(true);
}

void NativeAgendaText::cancel(const QString &request) {
    if (!busy() || request != m_request) return;
    // Native jobs already accepted by Xochitl cannot be canceled here. Stop
    // submitting work and let the durable initialization record require review.
    restoreSelection();
    reset();
    m_reason = QStringLiteral("agenda-cancelled");
    emit changed();
}
