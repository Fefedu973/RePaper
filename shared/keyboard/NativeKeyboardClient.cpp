#include "NativeKeyboardClient.h"
#include <QColor>
#include <QCoreApplication>
#include <QInputMethodEvent>
#include <QInputMethodQueryEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QMetaProperty>
#include <QTextCharFormat>
#include <QUuid>
#include <algorithm>
#include <cmath>

namespace repaper {
namespace {
constexpr qsizetype MaxPacket = 1024 * 1024;
constexpr qsizetype MaxText = 262144;
QJsonArray rectangle(const QRectF &rect) {
    return {rect.x(), rect.y(), rect.width(), rect.height()};
}
bool decodeRectangle(const QJsonValue &value, QRectF &rect) {
    const auto array = value.toArray();
    if (array.size() != 4) return false;
    for (const auto &number : array)
        if (!number.isDouble() || !std::isfinite(number.toDouble())) return false;
    rect = QRectF(array[0].toDouble(), array[1].toDouble(), array[2].toDouble(), array[3].toDouble());
    return rect.width() >= 0 && rect.height() >= 0;
}
}

NativeKeyboardClient::NativeKeyboardClient(QObject *parent) : QObject(parent) {
    m_socket.setReadBufferSize(MaxPacket + 1);
    m_timeout.setSingleShot(true);
    m_timeout.setInterval(1200);
    m_stateTimer.setSingleShot(true);
    m_stateTimer.setInterval(0);
    connect(&m_timeout, &QTimer::timeout, this, &NativeKeyboardClient::fail);
    connect(&m_stateTimer, &QTimer::timeout, this, &NativeKeyboardClient::sendState);
    connect(&m_socket, &QLocalSocket::connected, this, [this] {
        send({{"v", 1}, {"type", "hello"}, {"key", m_key}});
    });
    connect(&m_socket, &QLocalSocket::readyRead, this, &NativeKeyboardClient::receive);
    connect(&m_socket, &QLocalSocket::disconnected, this, &NativeKeyboardClient::fail);
    connect(&m_socket, &QLocalSocket::errorOccurred, this, [this](auto) { fail(); });
    bool validKey = false;
    const int key = qEnvironmentVariable("QTFB_KEY").toInt(&validKey);
    setEndpoint(qEnvironmentVariable("REPAPER_NATIVE_KEYBOARD_SOCKET"), validKey ? key : -1);
}

void NativeKeyboardClient::setEndpoint(const QString &path, int key) {
    blur();
    m_socket.abort();
    m_ready = false;
    m_path = path;
    m_key = key;
}

NativeKeyboardClient::~NativeKeyboardClient() {
    disconnect(&m_socket, nullptr, this, nullptr);
    for (const auto &connection : m_connections) disconnect(connection);
    m_socket.abort();
}

void NativeKeyboardClient::clearTarget() {
    for (const auto &connection : m_connections) disconnect(connection);
    m_connections.clear();
    m_target = nullptr;
    m_window = nullptr;
    m_session.clear();
    m_ack = 0;
    m_stateTimer.stop();
    m_timeout.stop();
    m_available = m_waiting = m_visible = false;
    m_keyboardRectangle = {};
}

void NativeKeyboardClient::blur() {
    if (!m_session.isEmpty() && m_ready)
        send({{"v", 1}, {"type", "blur"}, {"session", m_session}});
    clearTarget();
    emit changed();
}

void NativeKeyboardClient::focus(QQuickWindow *window, QQuickItem *target) {
    if (m_window == window && m_target == target && !m_session.isEmpty()) {
        scheduleState();
        return;
    }
    blur();
    if (!window || !target || m_path.isEmpty() || m_key < 0) return;
    m_window = window;
    m_target = target;
    m_session = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_waiting = true;
    const int slot = metaObject()->indexOfSlot("scheduleState()");
    for (const char *name : {"text", "cursorPosition", "selectionStart", "selectionEnd", "inputMethodHints",
                            "cursorRectangle", "x", "y", "width", "height", "echoMode", "maximumLength"}) {
        const int index = target->metaObject()->indexOfProperty(name);
        if (index < 0) continue;
        const auto property = target->metaObject()->property(index);
        if (property.hasNotifySignal())
            m_connections.append(connect(target, property.notifySignal(), this, metaObject()->method(slot)));
    }
    m_connections.append(connect(target, &QObject::destroyed, this, &NativeKeyboardClient::blur));
    m_connections.append(connect(window, &QObject::destroyed, this, &NativeKeyboardClient::blur));
    m_connections.append(connect(window, &QWindow::widthChanged, this, &NativeKeyboardClient::scheduleState));
    m_connections.append(connect(window, &QWindow::heightChanged, this, &NativeKeyboardClient::scheduleState));
    // Scroll containers can move the field without changing its own geometry.
    for (QQuickItem *ancestor = target->parentItem(); ancestor; ancestor = ancestor->parentItem()) {
        m_connections.append(connect(ancestor, &QQuickItem::xChanged, this, &NativeKeyboardClient::scheduleState));
        m_connections.append(connect(ancestor, &QQuickItem::yChanged, this, &NativeKeyboardClient::scheduleState));
    }
    m_timeout.start();
    if (m_ready && m_socket.state() == QLocalSocket::ConnectedState) send(state("focus"));
    else {
        m_ready = false;
        m_buffer.clear();
        if (m_socket.state() == QLocalSocket::UnconnectedState) m_socket.connectToServer(m_path);
    }
    emit changed();
}

QJsonObject NativeKeyboardClient::state(const QString &type) const {
    if (!m_target || !m_window) return {};
    const QString text = m_target->property("text").toString();
    const int cursor = std::clamp(m_target->property("cursorPosition").toInt(), 0, int(text.size()));
    const int start = m_target->property("selectionStart").toInt();
    const int end = m_target->property("selectionEnd").toInt();
    const int anchor = std::clamp(start == end ? cursor : (cursor == start ? end : start), 0, int(text.size()));
    QInputMethodQueryEvent query(Qt::ImCursorRectangle | Qt::ImAnchorRectangle);
    QCoreApplication::sendEvent(m_target, &query);
    auto cursorRect = query.value(Qt::ImCursorRectangle).toRectF();
    auto anchorRect = query.value(Qt::ImAnchorRectangle).toRectF();
    if (cursorRect.isNull()) cursorRect = m_target->property("cursorRectangle").toRectF();
    if (anchorRect.isNull()) anchorRect = cursorRect;
    int hints = m_target->property("inputMethodHints").toInt();
    if (m_target->property("echoMode").toInt() != 0)
        hints |= Qt::ImhHiddenText | Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase;
    QJsonObject result{{"v", 1}, {"type", type}, {"session", m_session}, {"ack", m_ack},
                       {"text", text}, {"cursor", cursor}, {"anchor", anchor},
                       {"hints", hints},
                       {"cursorRect", rectangle(m_target->mapRectToScene(cursorRect))},
                       {"anchorRect", rectangle(m_target->mapRectToScene(anchorRect))},
                       {"clientSize", QJsonArray{m_window->width(), m_window->height()}}};
    for (const char *name : {"maximumLength", "echoMode"})
        if (m_target->property(name).isValid()) result[QLatin1String(name)] = m_target->property(name).toInt();
    return result;
}

void NativeKeyboardClient::scheduleState() {
    if (m_target && m_ready) m_stateTimer.start();
}

void NativeKeyboardClient::sendState() {
    if (m_target && m_ready) send(state("state"));
}

void NativeKeyboardClient::send(const QJsonObject &message) {
    if (message.isEmpty() || m_socket.state() != QLocalSocket::ConnectedState) return;
    if (message.value("text").toString().size() > MaxText) { fail(); return; }
    const QByteArray bytes = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    if (bytes.size() > MaxPacket || m_socket.bytesToWrite() + bytes.size() > 2 * MaxPacket) { fail(); return; }
    if (m_socket.write(bytes) != bytes.size()) fail();
}

void NativeKeyboardClient::receive() {
    m_buffer += m_socket.readAll();
    for (;;) {
        const auto end = m_buffer.indexOf('\n');
        if (end < 0) break;
        if (end > MaxPacket) { fail(); return; }
        const auto line = m_buffer.left(end);
        m_buffer.remove(0, end + 1);
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(line, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) { fail(); return; }
        handle(document.object());
        if (m_socket.state() != QLocalSocket::ConnectedState) return;
    }
    if (m_buffer.size() > MaxPacket) fail();
}

void NativeKeyboardClient::handle(const QJsonObject &message) {
    if (message.value("v").toInt() != 1) { fail(); return; }
    const auto type = message.value("type").toString();
    if (type == "ready") {
        m_ready = true;
        if (m_target) send(state("focus"));
        return;
    }
    if (!m_target || m_session.isEmpty() || message.value("session").toString() != m_session) return;
    if (type == "error") { fail(); return; }
    if (type == "status") {
        QRectF rect;
        if (!message.value("visible").isBool() || !decodeRectangle(message.value("keyboardRect"), rect)) { fail(); return; }
        m_timeout.stop();
        m_waiting = false;
        m_available = true;
        m_visible = message.value("visible").toBool();
        m_keyboardRectangle = rect;
        emit changed();
        return;
    }
    if (type != "inputMethod" && type != "key") return;
    const qint64 sequence = message.value("seq").toInteger();
    if (sequence <= m_ack) return;
    if (sequence != m_ack + 1) { fail(); return; }
    if (!m_target->isVisible() || !m_target->isEnabled() || !m_target->hasActiveFocus()
        || m_target->property("readOnly").toBool()) { blur(); return; }
    const QString session = m_session;
    if (type == "inputMethod") {
        const auto commit = message.value("commit").toString();
        const auto preedit = message.value("preedit").toString();
        const auto attributes = message.value("attributes").toArray();
        if (commit.size() > MaxText || preedit.size() > MaxText || attributes.size() > 256) { fail(); return; }
        QList<QInputMethodEvent::Attribute> decoded;
        for (const auto &value : attributes) {
            const auto attribute = value.toObject();
            const int kind = attribute.value("type").toInt(-1);
            if (kind < QInputMethodEvent::TextFormat || kind > QInputMethodEvent::Selection) { fail(); return; }
            QVariant content = attribute.value("value").toVariant();
            if (kind == QInputMethodEvent::TextFormat) {
                const auto format = attribute.value("value").toObject();
                QTextCharFormat textFormat;
                const int underline = format.value("underlineStyle").toInt();
                if (underline < QTextCharFormat::NoUnderline || underline > QTextCharFormat::SpellCheckUnderline) { fail(); return; }
                textFormat.setUnderlineStyle(static_cast<QTextCharFormat::UnderlineStyle>(underline));
                const QColor color(format.value("underlineColor").toString());
                if (color.isValid()) textFormat.setUnderlineColor(color);
                content = textFormat;
            }
            decoded.append({static_cast<QInputMethodEvent::AttributeType>(kind),
                            attribute.value("start").toInt(), attribute.value("length").toInt(), content});
        }
        QInputMethodEvent event(preedit, decoded);
        event.setCommitString(commit, message.value("replacementStart").toInt(), message.value("replacementLength").toInt());
        QCoreApplication::sendEvent(m_target, &event);
    } else {
        const auto eventType = message.value("event").toString();
        if (eventType != "press" && eventType != "release") { fail(); return; }
        const QString text = message.value("text").toString();
        if (text.size() > MaxText) { fail(); return; }
        QKeyEvent event(eventType == "press" ? QEvent::KeyPress : QEvent::KeyRelease,
                        message.value("key").toInt(), Qt::KeyboardModifiers(message.value("modifiers").toInt()),
                        text, message.value("autorepeat").toBool(),
                        ushort(std::clamp(message.value("count").toInt(1), 1, 65535)));
        QCoreApplication::sendEvent(m_target, &event);
    }
    if (m_session == session && m_target) {
        m_ack = sequence;
        m_stateTimer.stop();
        sendState();
    }
}

void NativeKeyboardClient::fail() {
    m_ready = m_available = m_waiting = m_visible = false;
    m_keyboardRectangle = {};
    m_buffer.clear();
    m_timeout.stop();
    m_stateTimer.stop();
    if (m_target) {
        QInputMethodEvent clearPreedit;
        QCoreApplication::sendEvent(m_target, &clearPreedit);
    }
    if (m_socket.state() != QLocalSocket::UnconnectedState) m_socket.abort();
    emit changed();
}
}
