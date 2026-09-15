#include "NativeViewportHost.h"

#include <QDir>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLocalSocket>
#include <QMetaProperty>
#include <cmath>

namespace {
constexpr qsizetype MaxFrameBytes = 8192;
constexpr qint64 MaxSequence = (qint64(1) << 53) - 1;
constexpr qreal MaxDimension = 32768;

bool exactInteger(const QJsonValue &value, qint64 minimum, qint64 maximum, qint64 *out) {
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number
            || number < minimum || number > maximum) return false;
    *out = qint64(number);
    return true;
}
}

NativeViewportHost::NativeViewportHost(QQuickItem *parent) : QQuickItem(parent) {
    setAcceptedMouseButtons(Qt::NoButton);
    setAcceptTouchEvents(false);
    m_geometryTimer.setSingleShot(true);
    m_geometryTimer.setInterval(20);
    connect(&m_geometryTimer, &QTimer::timeout, this, &NativeViewportHost::sendGeometry);
    connect(&m_server, &QLocalServer::newConnection, this, &NativeViewportHost::acceptConnection);
}

NativeViewportHost::~NativeViewportHost() { closeServer(); }

void NativeViewportHost::setFramebufferID(int key) {
    if (m_key == key) return;
    closeServer();
    m_key = key;
    if (key >= 0) openServer();
    emit framebufferIDChanged();
}

void NativeViewportHost::setFramebufferItem(QQuickItem *item) {
    if (m_framebuffer == item) return;
    for (const auto &connection : m_geometryConnections) disconnect(connection);
    m_geometryConnections.clear();
    m_framebuffer = item;
    m_lastSize = {};
    m_lastRotation = -1;
    if (!item) dropPeer();
    if (item) {
        m_geometryConnections.append(connect(item, &QQuickItem::widthChanged, this, &NativeViewportHost::scheduleGeometry));
        m_geometryConnections.append(connect(item, &QQuickItem::heightChanged, this, &NativeViewportHost::scheduleGeometry));
        m_geometryConnections.append(connect(item, &QObject::destroyed, this, [this] { dropPeer(); }));
        const int index = item->metaObject()->indexOfProperty("fbRotation");
        if (index >= 0) {
            const auto property = item->metaObject()->property(index);
            if (property.hasNotifySignal())
                m_geometryConnections.append(connect(item, property.notifySignal(), this,
                    metaObject()->method(metaObject()->indexOfSlot("scheduleGeometry()"))));
        }
    }
    scheduleGeometry();
    emit framebufferItemChanged();
}

void NativeViewportHost::openServer() {
    m_directory = std::make_unique<QTemporaryDir>(QDir::tempPath()
        + QStringLiteral("/repaper-viewport-%1-XXXXXX").arg(m_key));
    if (!m_directory->isValid()) { m_directory.reset(); return; }
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    const QString path = m_directory->filePath(QStringLiteral("viewport.sock"));
    if (!m_server.listen(path)) { m_directory.reset(); return; }
    m_path = path;
    emit socketPathChanged();
}

void NativeViewportHost::closeServer() {
    m_geometryTimer.stop();
    dropPeer();
    m_server.close();
    m_directory.reset();
    if (!m_path.isEmpty()) { m_path.clear(); emit socketPathChanged(); }
}

void NativeViewportHost::dropPeer() {
    const bool wasReady = m_ready;
    m_ready = false;
    m_buffer.clear();
    m_sequence = 0;
    m_lastSize = {};
    m_lastRotation = -1;
    if (m_socket) {
        auto *peer = m_socket.data();
        m_socket.clear();
        disconnect(peer, nullptr, this, nullptr);
        peer->abort();
        peer->deleteLater();
    }
    if (wasReady) emit connectedChanged();
}

void NativeViewportHost::acceptConnection() {
    while (auto *peer = m_server.nextPendingConnection()) {
        if (m_socket) { peer->abort(); peer->deleteLater(); continue; }
        m_socket = peer;
        m_ready = false;
        m_buffer.clear();
        m_sequence = 0;
        m_lastSize = {};
        m_lastRotation = -1;
        peer->setReadBufferSize(MaxFrameBytes + 1);
        connect(peer, &QLocalSocket::readyRead, this, &NativeViewportHost::readFrames);
        connect(peer, &QLocalSocket::disconnected, this, [this, peer] {
            if (m_socket == peer) dropPeer();
            else peer->deleteLater();
        });
        if (peer->bytesAvailable()) readFrames();
    }
}

void NativeViewportHost::readFrames() {
    if (!m_socket) return;
    m_buffer += m_socket->readAll();
    for (;;) {
        const qsizetype newline = m_buffer.indexOf('\n');
        if (newline < 0) break;
        if (newline > MaxFrameBytes) { dropPeer(); return; }
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(m_buffer.left(newline), &error);
        m_buffer.remove(0, newline + 1);
        if (error.error != QJsonParseError::NoError || !document.isObject()) { dropPeer(); return; }
        const auto frame = document.object();
        qint64 version = 0, key = -1;
        if (m_ready || !exactInteger(frame.value("v"), 1, 1, &version)
                || frame.value("type").toString() != QStringLiteral("hello")
                || !exactInteger(frame.value("key"), 0, 2147483647, &key) || key != m_key) {
            dropPeer(); return;
        }
        m_ready = true;
        emit connectedChanged();
        if (!m_socket || !m_ready) return;
        sendGeometry();
        if (!m_socket) return;
    }
    if (m_buffer.size() > MaxFrameBytes) dropPeer();
}

void NativeViewportHost::scheduleGeometry() {
    // Restarting a trailing-edge timer throughout a drag can starve updates.
    if (!m_geometryTimer.isActive()) m_geometryTimer.start();
}

bool NativeViewportHost::send(const QJsonObject &frame) {
    if (!m_socket || m_socket->state() != QLocalSocket::ConnectedState) return false;
    const QByteArray bytes = QJsonDocument(frame).toJson(QJsonDocument::Compact) + '\n';
    if (bytes.size() > MaxFrameBytes || m_socket->bytesToWrite() + bytes.size() > 2 * MaxFrameBytes
            || m_socket->write(bytes) != bytes.size()) {
        dropPeer(); return false;
    }
    return true;
}

void NativeViewportHost::sendGeometry() {
    if (!m_ready || !m_framebuffer) return;
    const QSizeF size(m_framebuffer->width(), m_framebuffer->height());
    const QVariant property = m_framebuffer->property("fbRotation");
    bool rotationValid = false;
    const int rotation = property.isValid() ? property.toInt(&rotationValid) : 0;
    if (!property.isValid()) rotationValid = true;
    if (!std::isfinite(size.width()) || !std::isfinite(size.height())
            || size.width() < 1 || size.height() < 1
            || size.width() > MaxDimension || size.height() > MaxDimension
            || !rotationValid || rotation < 0 || rotation > 3) return;
    if (size == m_lastSize && rotation == m_lastRotation) return;
    if (m_sequence >= MaxSequence) { dropPeer(); return; }
    if (send({{"v", 1}, {"type", "geometry"}, {"seq", ++m_sequence},
              {"width", size.width()}, {"height", size.height()}, {"rotation", rotation}})) {
        m_lastSize = size;
        m_lastRotation = rotation;
    }
}
