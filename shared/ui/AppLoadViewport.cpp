#include "AppLoadViewport.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <algorithm>
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

bool dimension(const QJsonValue &value, qreal *out) {
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || number < 1 || number > MaxDimension) return false;
    *out = number;
    return true;
}
}

AppLoadViewport::AppLoadViewport(QQuickWindow *window, QObject *parent) : QObject(parent) {
    m_socket.setReadBufferSize(MaxFrameBytes + 1);
    m_retryTimer.setSingleShot(true);
    m_timeout.setSingleShot(true);
    m_timeout.setInterval(1500);
    connect(&m_retryTimer, &QTimer::timeout, this, &AppLoadViewport::connectEndpoint);
    connect(&m_timeout, &QTimer::timeout, this, &AppLoadViewport::fail);
    connect(&m_socket, &QLocalSocket::connected, this, [this] {
        const QByteArray hello = QJsonDocument(QJsonObject{{"v", 1}, {"type", "hello"}, {"key", m_key}})
            .toJson(QJsonDocument::Compact) + '\n';
        if (m_socket.write(hello) != hello.size()) { fail(); return; }
        m_timeout.start();
    });
    connect(&m_socket, &QLocalSocket::readyRead, this, &AppLoadViewport::receive);
    connect(&m_socket, &QLocalSocket::disconnected, this, &AppLoadViewport::fail);
    connect(&m_socket, &QLocalSocket::errorOccurred, this, [this](auto) { fail(); });
    setWindow(window);
    bool keyValid = false;
    const int key = qEnvironmentVariable("QTFB_KEY").toInt(&keyValid);
    setEndpoint(qEnvironmentVariable("REPAPER_VIEWPORT_SOCKET"), keyValid ? key : -1);
}

AppLoadViewport::~AppLoadViewport() {
    m_resetting = true;
    disconnect(&m_socket, nullptr, this, nullptr);
    m_socket.abort();
}

void AppLoadViewport::setWindow(QQuickWindow *window) {
    if (m_window == window) return;
    for (const auto &connection : m_windowConnections) disconnect(connection);
    m_windowConnections.clear();
    m_window = window;
    if (window) {
        m_windowConnections.append(connect(window, &QWindow::widthChanged, this, [this] { emit changed(); }));
        m_windowConnections.append(connect(window, &QWindow::heightChanged, this, [this] { emit changed(); }));
        m_windowConnections.append(connect(window, &QObject::destroyed, this, [this] {
            m_window.clear(); emit changed();
        }));
    }
    emit changed();
}

void AppLoadViewport::setEndpoint(const QString &path, int key) {
    m_resetting = true;
    m_retryTimer.stop();
    m_timeout.stop();
    m_socket.abort();
    m_buffer.clear();
    m_receivedGeometry = false;
    m_sequence = 0;
    m_path = path;
    m_key = key;
    m_retryDelay = 200;
    m_resetting = false;
    emit changed();
    if (!m_path.isEmpty() && m_key >= 0) m_retryTimer.start(0);
}

void AppLoadViewport::connectEndpoint() {
    if (m_resetting || m_path.isEmpty() || m_key < 0
            || m_socket.state() != QLocalSocket::UnconnectedState) return;
    m_buffer.clear();
    m_sequence = 0;
    m_receivedGeometry = false;
    m_timeout.start();
    m_socket.connectToServer(m_path);
}

void AppLoadViewport::fail() {
    if (m_resetting) return;
    m_resetting = true;
    m_timeout.stop();
    m_socket.abort();
    m_buffer.clear();
    m_receivedGeometry = false;
    m_sequence = 0;
    m_resetting = false;
    emit changed();
    if (!m_path.isEmpty() && m_key >= 0 && !m_retryTimer.isActive()) {
        m_retryTimer.start(m_retryDelay);
        m_retryDelay = std::min(m_retryDelay * 2, 3000);
    }
}

void AppLoadViewport::receive() {
    m_buffer += m_socket.readAll();
    for (;;) {
        const qsizetype newline = m_buffer.indexOf('\n');
        if (newline < 0) break;
        if (newline > MaxFrameBytes) { fail(); return; }
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(m_buffer.left(newline), &error);
        m_buffer.remove(0, newline + 1);
        if (error.error != QJsonParseError::NoError || !document.isObject()) { fail(); return; }
        handle(document.object());
        if (m_socket.state() != QLocalSocket::ConnectedState) return;
    }
    if (m_buffer.size() > MaxFrameBytes) fail();
}

void AppLoadViewport::handle(const QJsonObject &frame) {
    qint64 version = 0, rotation = 0, sequence = 0;
    qreal width = 0, height = 0;
    if (!exactInteger(frame.value("v"), 1, 1, &version)
            || frame.value("type").toString() != QStringLiteral("geometry")
            || !exactInteger(frame.value("seq"), 1, MaxSequence, &sequence)
            || !dimension(frame.value("width"), &width) || !dimension(frame.value("height"), &height)
            || !exactInteger(frame.value("rotation"), 0, 3, &rotation)) { fail(); return; }
    if (sequence <= m_sequence) return;
    m_sequence = sequence;
    m_hostWidth = width;
    m_hostHeight = height;
    m_rotation = int(rotation);
    m_receivedGeometry = true;
    m_retryDelay = 200;
    m_timeout.stop();
    emit changed();
}

bool AppLoadViewport::available() const {
    return m_receivedGeometry && m_socket.state() == QLocalSocket::ConnectedState
        && m_window && m_window->width() > 0 && m_window->height() > 0;
}

qreal AppLoadViewport::width() const { return available() ? m_hostWidth : (m_window ? m_window->width() : 0); }
qreal AppLoadViewport::height() const { return available() ? m_hostHeight : (m_window ? m_window->height() : 0); }

QTransform AppLoadViewport::contentToWindow() const {
    if (!available()) return {};
    // Reproduce FBController's Stretch destination rectangle, including its
    // integer rounding and rotation translation. Its source here is the Qt
    // window's logical rectangle; LinuxFB's pixel ratio cancels on both sides.
    const bool quarterTurn = m_rotation == 1 || m_rotation == 2;
    QRect target(0, 0, int(quarterTurn ? m_hostHeight : m_hostWidth),
                       int(quarterTurn ? m_hostWidth : m_hostHeight));
    if (m_rotation == 1) target.translate(-int(m_hostHeight), 0);
    else if (m_rotation == 2) target.translate(0, -int(m_hostWidth));
    else if (m_rotation == 3) target.translate(-int(m_hostWidth), -int(m_hostHeight));
    QTransform windowToHost;
    if (m_rotation == 1) windowToHost.rotate(-90);
    else if (m_rotation == 2) windowToHost.rotate(90);
    else if (m_rotation == 3) windowToHost.rotate(180);
    windowToHost.translate(target.x(), target.y());
    windowToHost.scale(qreal(target.width()) / m_window->width(), qreal(target.height()) / m_window->height());
    return windowToHost.inverted();
}

QMatrix4x4 AppLoadViewport::contentTransform() const { return QMatrix4x4(contentToWindow()); }

QRectF AppLoadViewport::mapRectFromWindow(const QRectF &rectangle) const {
    if (!std::isfinite(rectangle.x()) || !std::isfinite(rectangle.y())
            || !std::isfinite(rectangle.width()) || !std::isfinite(rectangle.height())
            || rectangle.width() < 0 || rectangle.height() < 0) return {};
    if (!available()) return rectangle;
    const auto mapped = contentToWindow().inverted().mapRect(rectangle);
    if (!std::isfinite(mapped.x()) || !std::isfinite(mapped.y())
            || !std::isfinite(mapped.width()) || !std::isfinite(mapped.height())) return {};
    return mapped;
}
