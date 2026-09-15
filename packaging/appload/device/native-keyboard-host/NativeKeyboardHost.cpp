#include "NativeKeyboardHost.h"
#include "NativePaperFonts.h"
#include "qtfb/FBController.h"

#include <QCoreApplication>
#include <QDir>
#include <QFont>
#include <QGuiApplication>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QInputMethodQueryEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QKeyEvent>
#include <QLocalSocket>
#include <QLocale>
#include <QQuickWindow>
#include <QTextBoundaryFinder>
#include <QTextCharFormat>
#include <QTransform>
#include <algorithm>
#include <cmath>

namespace {
constexpr qsizetype MaxFrameBytes = 1024 * 1024;
constexpr qsizetype MaxTextUnits = 262144;
constexpr qint64 MaxSequence = (qint64(1) << 53) - 1;
QPointer<NativeKeyboardHost> inputOwner;

bool integer(const QJsonValue &value, qint64 minimum, qint64 maximum, qint64 *result) {
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number
            || number < minimum || number > maximum) return false;
    *result = qint64(number);
    return true;
}

bool rectangle(const QJsonValue &value, QRectF *result) {
    const QJsonArray data = value.toArray();
    if (data.size() != 4) return false;
    for (const auto part : data)
        if (!part.isDouble() || !std::isfinite(part.toDouble())
                || std::abs(part.toDouble()) > 10000000) return false;
    if (data[2].toDouble() < 0 || data[3].toDouble() < 0) return false;
    *result = QRectF(data[0].toDouble(), data[1].toDouble(),
                    data[2].toDouble(), data[3].toDouble());
    return true;
}

QJsonArray rectangleJson(const QRectF &rect) {
    return {rect.x(), rect.y(), rect.width(), rect.height()};
}

QJsonValue attributeValue(const QInputMethodEvent::Attribute &attribute) {
    if (attribute.type == QInputMethodEvent::TextFormat) {
        const auto format = qvariant_cast<QTextFormat>(attribute.value).toCharFormat();
        QJsonObject value{{"underlineStyle", int(format.underlineStyle())}};
        if (format.underlineColor().isValid())
            value.insert("underlineColor", format.underlineColor().name(QColor::HexArgb));
        return value;
    }
    if (attribute.type == QInputMethodEvent::Language)
        return attribute.value.value<QLocale>().name();
    if (attribute.value.metaType().id() == QMetaType::QColor)
        return attribute.value.value<QColor>().name(QColor::HexArgb);
    switch (attribute.value.metaType().id()) {
    case QMetaType::QString: return attribute.value.toString();
    case QMetaType::Bool: return attribute.value.toBool();
    case QMetaType::Int: return attribute.value.toInt();
    case QMetaType::Double: return attribute.value.toDouble();
    default: return QJsonValue();
    }
}
}

NativeKeyboardHost::NativeKeyboardHost(QQuickItem *parent) : QQuickItem(parent) {
    repaper::exposeNativePaperFonts();
    setFlag(ItemAcceptsInputMethod);
    setAcceptedMouseButtons(Qt::NoButton);
    setAcceptTouchEvents(false);
    connect(&m_server, &QLocalServer::newConnection, this, &NativeKeyboardHost::acceptConnection);
    auto *input = QGuiApplication::inputMethod();
    connect(input, &QInputMethod::visibleChanged, this, &NativeKeyboardHost::sendStatus);
    connect(input, &QInputMethod::keyboardRectangleChanged, this, &NativeKeyboardHost::sendStatus);
    connect(this, &QQuickItem::activeFocusChanged, this, [this] {
        if (!hasActiveFocus() && inputOwner == this) {
            inputOwner.clear();
            m_wantsFocus = false;
            // Let Qt finish assigning the next focus object before deciding
            // whether the current input panel has become orphaned.
            QMetaObject::invokeMethod(this, [] {
                if (inputOwner) return;
                QInputMethodQueryEvent query(Qt::ImEnabled);
                if (auto *focused = QGuiApplication::focusObject())
                    QCoreApplication::sendEvent(focused, &query);
                if (!query.value(Qt::ImEnabled).toBool()) QGuiApplication::inputMethod()->hide();
            }, Qt::QueuedConnection);
        }
        sendStatus();
    });
    connect(this, &QQuickItem::visibleChanged, this, [this] {
        if (!isVisible()) releaseFocus(false);
        else if (m_wantsFocus && !inputOwner) activateSession();
    });
    connect(this, &QQuickItem::enabledChanged, this, [this] {
        if (!isEnabled()) releaseFocus(false);
    });
    connect(this, &QQuickItem::windowChanged, this, [this] { geometryChanged(); });
}

NativeKeyboardHost::~NativeKeyboardHost() {
    closeServer();
    if (m_framebuffer) m_framebuffer->removeEventFilter(this);
}

void NativeKeyboardHost::setFramebufferID(int key) {
    if (m_key == key) return;
    closeServer();
    m_key = key;
    if (key >= 0) openServer();
    emit framebufferIDChanged();
}

void NativeKeyboardHost::setFramebufferItem(QQuickItem *item) {
    if (m_framebuffer == item) return;
    if (m_framebuffer) m_framebuffer->removeEventFilter(this);
    m_framebuffer = item;
    if (item) item->installEventFilter(this);
    reconnectGeometry();
    emit framebufferItemChanged();
}

void NativeKeyboardHost::openServer() {
    m_directory = std::make_unique<QTemporaryDir>(QDir::tempPath()
        + QStringLiteral("/repaper-native-keyboard-%1-XXXXXX").arg(m_key));
    if (!m_directory->isValid()) { m_directory.reset(); return; }
    // QTemporaryDir creates its directory with owner-only permissions. The
    // socket is restricted too, and no pre-existing endpoint is ever unlinked.
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    const QString path = m_directory->filePath(QStringLiteral("ime.sock"));
    if (!m_server.listen(path)) { m_directory.reset(); return; }
    m_path = path;
    emit socketPathChanged();
}

void NativeKeyboardHost::closeServer() {
    m_closing = true;
    endSession(true);
    m_ready = false;
    if (m_socket) {
        disconnect(m_socket, nullptr, this, nullptr);
        m_socket->abort();
        m_socket->deleteLater();
        m_socket.clear();
    }
    m_server.close();
    m_directory.reset();
    m_buffer.clear();
    m_rejecting = false;
    if (!m_path.isEmpty()) { m_path.clear(); emit socketPathChanged(); }
    m_closing = false;
}

void NativeKeyboardHost::acceptConnection() {
    while (auto *peer = m_server.nextPendingConnection()) {
        if (m_socket) { peer->abort(); peer->deleteLater(); continue; }
        m_socket = peer;
        m_buffer.clear();
        m_ready = false;
        m_rejecting = false;
        peer->setReadBufferSize(MaxFrameBytes + 1);
        connect(peer, &QLocalSocket::readyRead, this, &NativeKeyboardHost::readFrames);
        connect(peer, &QLocalSocket::disconnected, this, [this, peer] {
            if (m_socket == peer) {
                endSession(true);
                m_ready = false;
                m_socket.clear();
                m_buffer.clear();
                m_rejecting = false;
            }
            peer->deleteLater();
        });
        if (peer->bytesAvailable()) readFrames();
    }
}

void NativeKeyboardHost::readFrames() {
    if (!m_socket || m_rejecting) return;
    m_buffer += m_socket->readAll();
    while (!m_rejecting && m_socket) {
        const qsizetype newline = m_buffer.indexOf('\n');
        if (newline < 0) break;
        if (newline > MaxFrameBytes) { reject("packet-too-large"); return; }
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(m_buffer.left(newline), &error);
        m_buffer.remove(0, newline + 1);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            reject("invalid-json"); return;
        }
        processFrame(document.object());
    }
    if (m_buffer.size() > MaxFrameBytes) reject("packet-too-large");
}

void NativeKeyboardHost::processFrame(const QJsonObject &frame) {
    if (frame.value("v").toInt(-1) != 1) { reject("protocol-version"); return; }
    const QString type = frame.value("type").toString();
    if (!m_ready) {
        qint64 key = -1;
        if ((!type.isEmpty() && type != QStringLiteral("hello"))
                || !integer(frame.value("key"), 0, 2147483647, &key) || key != m_key) {
            reject("invalid-hello"); return;
        }
        m_ready = true;
        send({{"type", "ready"}});
        return;
    }
    const QString session = frame.value("session").toString();
    if (session.isEmpty() || session.size() > 128) { reject("invalid-session"); return; }
    if (type == QStringLiteral("blur")) {
        if (session == m_session) endSession(true);
        return;
    }
    if (type != QStringLiteral("focus") && type != QStringLiteral("state")) {
        reject("unknown-message"); return;
    }
    if (type == QStringLiteral("state") && session != m_session) return;
    qint64 ack = 0;
    if (frame.contains("ack") && !integer(frame.value("ack"), 0, MaxSequence, &ack)) {
        reject("invalid-ack"); return;
    }
    const bool newSession = type == QStringLiteral("focus") && session != m_session;
    if (!newSession && ack < m_sequence) return;
    if (!newSession && ack > m_sequence) { reject("invalid-ack"); return; }
    State state;
    if (!parseState(frame, &state)) { reject("invalid-state"); return; }
    if (newSession) {
        endSession(true);
        m_session = session;
        m_sequence = ack;
    }
    m_state = std::move(state);
    if (type == QStringLiteral("focus")) {
        m_wantsFocus = true;
        activateSession();
    }
    if (ownsInputFocus()) QGuiApplication::inputMethod()->update(Qt::ImQueryAll);
    sendStatus();
}

bool NativeKeyboardHost::parseState(const QJsonObject &frame, State *state) const {
    if (!frame.value("text").isString()) return false;
    state->text = frame.value("text").toString();
    if (state->text.size() > MaxTextUnits) return false;
    qint64 number;
    if (!integer(frame.value("cursor"), 0, state->text.size(), &number)) return false;
    state->cursor = int(number);
    if (!integer(frame.value("anchor"), 0, state->text.size(), &number)) return false;
    state->anchor = int(number);
    if (!integer(frame.value("hints"), -2147483648LL, 2147483647, &number)) return false;
    state->hints = int(number);
    if (frame.contains("maximumLength")) {
        if (!integer(frame.value("maximumLength"), -1, 2147483647, &number)) return false;
        state->maximumLength = number < 0 ? 2147483647 : int(number);
    }
    if (frame.contains("echoMode")) {
        if (!integer(frame.value("echoMode"), 0, 3, &number)) return false;
        state->echoMode = int(number);
    }
    if (!rectangle(frame.value("cursorRect"), &state->cursorRect)) return false;
    if (frame.contains("anchorRect")) {
        if (!rectangle(frame.value("anchorRect"), &state->anchorRect)) return false;
    } else state->anchorRect = state->cursorRect;
    const QJsonArray size = frame.value("clientSize").toArray();
    if (size.size() != 2) return false;
    for (const auto value : size)
        if (!value.isDouble() || !std::isfinite(value.toDouble())
                || value.toDouble() <= 0 || value.toDouble() > 10000000) return false;
    state->clientSize = QSizeF(size[0].toDouble(), size[1].toDouble());
    return true;
}

bool NativeKeyboardHost::send(QJsonObject frame) {
    if (!m_socket || m_socket->state() != QLocalSocket::ConnectedState) return false;
    frame.insert("v", 1);
    const QByteArray bytes = QJsonDocument(frame).toJson(QJsonDocument::Compact) + '\n';
    if (bytes.size() > MaxFrameBytes || m_socket->bytesToWrite() > MaxFrameBytes * 2) {
        m_socket->abort();
        return false;
    }
    return m_socket->write(bytes) == bytes.size();
}

void NativeKeyboardHost::reject(const char *code) {
    if (m_rejecting) return;
    m_rejecting = true;
    send({{"type", "error"}, {"session", m_session}, {"code", QString::fromLatin1(code)}});
    endSession(true);
    if (m_socket) m_socket->disconnectFromServer();
}

bool NativeKeyboardHost::ownsInputFocus() const {
    return !m_session.isEmpty() && m_ready && m_socket && inputOwner == this
        && hasActiveFocus() && isVisible() && isEnabled();
}

void NativeKeyboardHost::activateSession() {
    if (m_session.isEmpty() || !m_ready || !window() || !isVisible() || !isEnabled()) return;
    bool geometryValid = false;
    clientToFramebufferItem(&geometryValid);
    if (!geometryValid) return;
    if (inputOwner && inputOwner != this) {
        inputOwner->m_wantsFocus = false;
        inputOwner->releaseFocus(false);
    }
    if (!hasActiveFocus()) m_previousFocus = window()->activeFocusItem();
    inputOwner = this;
    forceActiveFocus(Qt::OtherFocusReason);
    if (hasActiveFocus()) {
        QGuiApplication::inputMethod()->update(Qt::ImQueryAll);
        QGuiApplication::inputMethod()->show();
    }
    sendStatus();
}

void NativeKeyboardHost::releaseFocus(bool restoreFocus) {
    const bool owned = inputOwner == this;
    const bool focused = hasActiveFocus();
    if (owned) inputOwner.clear();
    if (focused) {
        setFocus(false, Qt::OtherFocusReason);
        if (restoreFocus && m_previousFocus && m_previousFocus != this
                && m_previousFocus->isVisible() && m_previousFocus->isEnabled())
            m_previousFocus->forceActiveFocus(Qt::OtherFocusReason);
    }
    // Never hide a panel that now belongs to another native editor.
    if (owned && focused) {
        auto *focusedItem = window() ? window()->activeFocusItem() : nullptr;
        if (!focusedItem || !focusedItem->flags().testFlag(ItemAcceptsInputMethod))
            QGuiApplication::inputMethod()->hide();
    }
    m_previousFocus.clear();
    sendStatus();
}

void NativeKeyboardHost::endSession(bool restoreFocus) {
    m_wantsFocus = false;
    releaseFocus(restoreFocus);
    m_session.clear();
    m_state = State();
    m_sequence = 0;
}

void NativeKeyboardHost::sendStatus() {
    if (!m_ready || m_session.isEmpty()) return;
    const bool visible = ownsInputFocus() && QGuiApplication::inputMethod()->isVisible();
    send({{"type", "status"}, {"session", m_session}, {"visible", visible},
          {"keyboardRect", rectangleJson(visible
            ? keyboardRectForClient(QGuiApplication::inputMethod()->keyboardRectangle()) : QRectF())}});
}

void NativeKeyboardHost::reconnectGeometry() {
    for (const auto &connection : m_geometryConnections) disconnect(connection);
    m_geometryConnections.clear();
    for (QQuickItem *item = m_framebuffer; item; item = item->parentItem()) {
        for (auto signal : {&QQuickItem::xChanged, &QQuickItem::yChanged,
                            &QQuickItem::widthChanged, &QQuickItem::heightChanged,
                            &QQuickItem::rotationChanged, &QQuickItem::scaleChanged})
            m_geometryConnections.append(connect(item, signal, this, &NativeKeyboardHost::geometryChanged));
    }
    if (auto *framebuffer = qobject_cast<FBController *>(m_framebuffer.data())) {
        m_geometryConnections.append(connect(framebuffer, &FBController::framebufferSizeChanged,
                                              this, &NativeKeyboardHost::geometryChanged));
        m_geometryConnections.append(connect(framebuffer, &FBController::fbRotationChanged,
                                              this, &NativeKeyboardHost::geometryChanged));
        m_geometryConnections.append(connect(framebuffer, &QQuickItem::activeFocusChanged, this, [this, framebuffer] {
            if (!framebuffer->hasActiveFocus() || m_session.isEmpty()) return;
            const QString session = m_session;
            QMetaObject::invokeMethod(this, [this, session] {
                if (session == m_session && m_ready && (!inputOwner || inputOwner == this)) {
                    m_wantsFocus = true;
                    activateSession();
                }
            }, Qt::QueuedConnection);
        }));
    }
    geometryChanged();
}

void NativeKeyboardHost::geometryChanged() {
    if (ownsInputFocus()) QGuiApplication::inputMethod()->update(
        Qt::ImCursorRectangle | Qt::ImAnchorRectangle | Qt::ImInputItemClipRectangle);
    else if (m_wantsFocus && !inputOwner) activateSession();
    sendStatus();
}

QTransform NativeKeyboardHost::clientToFramebufferItem(bool *ok) const {
    if (ok) *ok = false;
    auto *framebuffer = qobject_cast<FBController *>(m_framebuffer.data());
    if (!framebuffer || m_state.clientSize.isEmpty() || framebuffer->framebufferSize().isEmpty()
            || framebuffer->width() <= 0 || framebuffer->height() <= 0) return {};
    const int rotation = framebuffer->property("fbRotation").toInt();
    QRect target;
    if (framebuffer->property("allowScaling").toBool()
            && framebuffer->property("fillMode").toInt() != FBController::Pad) {
        target = framebuffer->translateToCounteractRotation(framebuffer->convertQTFBRectToScreen(
            QRect(QPoint(), framebuffer->framebufferSize())));
    } else {
        float width = framebuffer->width(), height = framebuffer->height();
        if (rotation == FBController::Deg90L || rotation == FBController::Deg90R) std::swap(width, height);
        target = framebuffer->translateToCounteractRotation(QRect(
            (width - framebuffer->framebufferSize().width()) / 2,
            (height - framebuffer->framebufferSize().height()) / 2,
            framebuffer->width(), framebuffer->height()));
    }
    if (target.isEmpty()) return {};
    QTransform transform;
    switch (rotation) {
    case FBController::Deg90L: transform.rotate(-90); break;
    case FBController::Deg90R: transform.rotate(90); break;
    case FBController::Deg180: transform.rotate(180); break;
    default: break;
    }
    transform.translate(target.x(), target.y());
    transform.scale(target.width() / m_state.clientSize.width(), target.height() / m_state.clientSize.height());
    if (ok) *ok = transform.isInvertible();
    return transform;
}

QRectF NativeKeyboardHost::clientRectToHost(const QRectF &rect) const {
    bool valid;
    const auto transform = clientToFramebufferItem(&valid);
    return valid ? mapRectFromItem(m_framebuffer, transform.mapRect(rect)) : QRectF();
}

QRectF NativeKeyboardHost::keyboardRectForClient(const QRectF &windowRectangle) const {
    bool valid;
    const auto transform = clientToFramebufferItem(&valid);
    if (!valid || !m_framebuffer) return {};
    const auto panel = m_framebuffer->mapRectFromScene(windowRectangle);
    const auto overlap = panel.intersected(m_framebuffer->boundingRect())
        .intersected(transform.mapRect(QRectF(QPointF(), m_state.clientSize)));
    if (overlap.isEmpty()) return {};
    return transform.inverted().mapRect(overlap).intersected(QRectF(QPointF(), m_state.clientSize));
}

QVariant NativeKeyboardHost::inputMethodQuery(Qt::InputMethodQuery query) const {
    const bool active = ownsInputFocus();
    switch (query) {
    case Qt::ImEnabled: return active;
    case Qt::ImReadOnly: return !active;
    case Qt::ImCursorRectangle: return clientRectToHost(m_state.cursorRect);
    case Qt::ImAnchorRectangle: return clientRectToHost(m_state.anchorRect);
    case Qt::ImCursorPosition: case Qt::ImAbsolutePosition: return m_state.cursor;
    case Qt::ImAnchorPosition: return m_state.anchor;
    case Qt::ImSurroundingText: return m_state.text;
    case Qt::ImTextBeforeCursor: return m_state.text.left(m_state.cursor);
    case Qt::ImTextAfterCursor: return m_state.text.mid(m_state.cursor);
    case Qt::ImCurrentSelection: return m_state.text.mid(std::min(m_state.cursor, m_state.anchor),
                                                        std::abs(m_state.cursor - m_state.anchor));
    case Qt::ImHints: return m_state.hints | (m_state.echoMode != 0
        ? int(Qt::ImhHiddenText | Qt::ImhSensitiveData | Qt::ImhNoPredictiveText) : 0);
    case Qt::ImMaximumTextLength: return m_state.maximumLength;
    case Qt::ImPreferredLanguage: return QLocale().name();
    case Qt::ImEnterKeyType: return int(Qt::EnterKeyDefault);
    case Qt::ImFont: return QFont();
    case Qt::ImInputItemClipRectangle:
        return m_framebuffer ? mapRectFromItem(m_framebuffer, m_framebuffer->boundingRect()) : QRectF();
    default: return QQuickItem::inputMethodQuery(query);
    }
}

QVariant NativeKeyboardHost::inputMethodQuery(Qt::InputMethodQuery query, const QVariant &argument) const {
    // QInputMethod::queryFocusObject first tries this public invokable overload.
    // Honor bounded surrounding-text requests without inventing hit-test data
    // for the remote field, whose layout remains owned by the client process.
    bool validLength = false;
    const int length = argument.toInt(&validLength);
    if (validLength && length >= 0) {
        if (query == Qt::ImTextBeforeCursor) return m_state.text.left(m_state.cursor).right(length);
        if (query == Qt::ImTextAfterCursor) return m_state.text.mid(m_state.cursor).left(length);
    }
    return inputMethodQuery(query);
}

void NativeKeyboardHost::inputMethodEvent(QInputMethodEvent *event) {
    if (!ownsInputFocus()) { event->ignore(); return; }
    if (event->commitString().size() > MaxTextUnits || event->preeditString().size() > MaxTextUnits
            || event->attributes().size() > 256 || m_sequence == MaxSequence) {
        reject("input-too-large"); event->ignore(); return;
    }
    QJsonArray attributes;
    for (const auto &attribute : event->attributes()) {
        // The plain commit covers MimeData too; transport no arbitrary objects.
        if (attribute.type > QInputMethodEvent::Selection) continue;
        attributes.append(QJsonObject{{"type", int(attribute.type)}, {"start", attribute.start},
            {"length", attribute.length}, {"value", attributeValue(attribute)}});
    }
    const bool sent = send({{"type", "inputMethod"}, {"session", m_session}, {"seq", ++m_sequence},
        {"commit", event->commitString()}, {"preedit", event->preeditString()},
        {"replacementStart", event->replacementStart()}, {"replacementLength", event->replacementLength()},
        {"attributes", attributes}});
    if (sent) applyInputToShadow(event);
    event->setAccepted(sent);
}

void NativeKeyboardHost::applyInputToShadow(const QInputMethodEvent *event) {
    const int selectionStart = std::min(m_state.cursor, m_state.anchor);
    const int selectionLength = std::abs(m_state.cursor - m_state.anchor);
    // The next acknowledged client state remains authoritative, including
    // validators/maxLength. This shadow only answers immediate native queries.
    if (!event->commitString().isEmpty() || !event->preeditString().isEmpty()
            || event->replacementLength() != 0) {
        if (selectionLength) {
            m_state.text.remove(selectionStart, selectionLength);
            m_state.cursor = selectionStart;
        }
        const int start = int(std::clamp<qint64>(qint64(m_state.cursor) + event->replacementStart(), 0, m_state.text.size()));
        const int length = int(std::clamp<qint64>(event->replacementLength(), 0, m_state.text.size() - start));
        m_state.text.replace(start, length, event->commitString());
        m_state.cursor = start + int(event->commitString().size());
        m_state.anchor = m_state.cursor;
    }
    for (const auto &attribute : event->attributes()) {
        if (attribute.type != QInputMethodEvent::Selection) continue;
        m_state.anchor = int(std::clamp<qint64>(attribute.start, 0, m_state.text.size()));
        m_state.cursor = int(std::clamp<qint64>(qint64(attribute.start) + attribute.length, 0, m_state.text.size()));
    }
}

void NativeKeyboardHost::forwardKey(QKeyEvent *event, bool pressed) {
    if (!ownsInputFocus()) { event->ignore(); return; }
    if (event->text().size() > MaxTextUnits || m_sequence == MaxSequence) {
        reject("input-too-large"); event->ignore(); return;
    }
    const bool sent = send({{"type", "key"}, {"session", m_session}, {"seq", ++m_sequence},
        {"event", pressed ? "press" : "release"}, {"key", event->key()},
        {"modifiers", int(event->modifiers())}, {"text", event->text()},
        {"autorepeat", event->isAutoRepeat()}, {"count", event->count()}});
    if (sent && pressed) applyKeyToShadow(event);
    event->setAccepted(sent);
}

void NativeKeyboardHost::applyKeyToShadow(const QKeyEvent *event) {
    if (event->modifiers().testFlag(Qt::ControlModifier) || event->modifiers().testFlag(Qt::AltModifier)
            || event->modifiers().testFlag(Qt::MetaModifier)) return;
    int start = std::min(m_state.cursor, m_state.anchor);
    int end = std::max(m_state.cursor, m_state.anchor);
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, m_state.text);
    finder.setPosition(m_state.cursor);
    if (event->key() == Qt::Key_Backspace || event->key() == Qt::Key_Delete) {
        if (start == end) {
            if (event->key() == Qt::Key_Backspace) start = std::max(0, int(finder.toPreviousBoundary()));
            else { const auto next = finder.toNextBoundary(); end = next < 0 ? end : int(next); }
        }
        m_state.text.remove(start, end - start);
        m_state.cursor = m_state.anchor = start;
    } else if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right) {
        const auto next = event->key() == Qt::Key_Left ? finder.toPreviousBoundary() : finder.toNextBoundary();
        if (next >= 0) m_state.cursor = int(next);
        if (!event->modifiers().testFlag(Qt::ShiftModifier)) m_state.anchor = m_state.cursor;
    } else if (!event->text().isEmpty() && event->text().front().isPrint()) {
        m_state.text.replace(start, end - start, event->text());
        m_state.cursor = m_state.anchor = start + int(event->text().size());
    }
}

void NativeKeyboardHost::keyPressEvent(QKeyEvent *event) { forwardKey(event, true); }
void NativeKeyboardHost::keyReleaseEvent(QKeyEvent *event) { forwardKey(event, false); }

bool NativeKeyboardHost::eventFilter(QObject *watched, QEvent *event) {
    if (watched == m_framebuffer && !m_session.isEmpty()
            && (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::TouchBegin
                || event->type() == QEvent::TabletPress)) {
        const QString session = m_session;
        QMetaObject::invokeMethod(this, [this, session] {
            if (session == m_session && m_ready) { m_wantsFocus = true; activateSession(); }
        }, Qt::QueuedConnection);
    }
    return QQuickItem::eventFilter(watched, event);
}
