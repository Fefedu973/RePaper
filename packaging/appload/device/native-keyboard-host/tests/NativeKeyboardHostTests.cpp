#include "NativeKeyboardHost.h"
#include "qtfb/FBController.h"
#include "qtfb/fbmanagement.h"
#include <QFileInfo>
#include <QGuiApplication>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QTextCharFormat>
#include <QtTest>
#include <sys/stat.h>

static std::map<int, QPointer<FBController>> controllers;
namespace qtfb::management {
void registerController(FBKey key, QPointer<FBController> value) { controllers[key] = value; }
void unregisterController(FBKey key) { controllers.erase(key); }
bool isControllerAssociated(FBKey key) { return controllers.count(key) && controllers[key]; }
void forwardUserInput(FBKey, const UserInputContents &) {}
void sendDeviceStateChange(FBKey, const DeviceStateChangedContents &) {}
void start() {}
}

class Peer : public QObject {
public:
    QLocalSocket socket;
    QList<QJsonObject> frames;
    QByteArray buffer;
    Peer() {
        connect(&socket, &QLocalSocket::readyRead, this, [this] {
            buffer += socket.readAll();
            for (;;) {
                const auto end = buffer.indexOf('\n');
                if (end < 0) break;
                frames.append(QJsonDocument::fromJson(buffer.left(end)).object());
                buffer.remove(0, end + 1);
            }
        });
    }
    void send(QJsonObject frame) {
        frame.insert("v", 1);
        socket.write(QJsonDocument(frame).toJson(QJsonDocument::Compact) + '\n');
        socket.flush();
    }
    int count(const QString &type) const {
        return std::count_if(frames.begin(), frames.end(), [&](const auto &frame) {
            return frame.value("type").toString() == type;
        });
    }
    QJsonObject last(const QString &type) const {
        for (auto it = frames.crbegin(); it != frames.crend(); ++it)
            if (it->value("type").toString() == type) return *it;
        return {};
    }
};

class Fixture {
public:
    QImage image{1620, 2160, QImage::Format_RGB32};
    QQuickWindow window;
    FBController *framebuffer = new FBController(window.contentItem());
    NativeKeyboardHost *host = new NativeKeyboardHost(framebuffer);
    Peer peer;
    explicit Fixture(int key = 35100, QSize size = {540, 720}) {
        window.resize(size);
        framebuffer->setSize(size);
        framebuffer->setProperty("allowScaling", true);
        framebuffer->setProperty("fillMode", FBController::PreserveAspectFit);
        framebuffer->setFramebufferID(key);
        image.fill(Qt::white);
        framebuffer->associateSHM(&image);
        host->setSize(size);
        host->setFramebufferItem(framebuffer);
        host->setFramebufferID(key);
        window.show();
        window.requestActivate();
        framebuffer->forceActiveFocus();
        QCoreApplication::processEvents();
    }
    QJsonObject state(const QString &session = "field-a", const QString &type = "focus",
                      const QString &text = QString::fromUtf8("abé😀cd"), qint64 ack = 0) const {
        return {{"type", type}, {"session", session}, {"ack", ack}, {"text", text},
                {"cursor", int(text.size())}, {"anchor", int(text.size())}, {"hints", 0},
                {"cursorRect", QJsonArray{120, 90, 2, 24}},
                {"anchorRect", QJsonArray{120, 90, 2, 24}},
                {"clientSize", QJsonArray{810, 1080}}};
    }
};

#define CONNECT_PEER(fixture) \
    fixture.peer.socket.connectToServer(fixture.host->socketPath()); \
    QTRY_COMPARE(fixture.peer.socket.state(), QLocalSocket::ConnectedState); \
    fixture.peer.send({{"type", "hello"}, {"key", fixture.host->framebufferID()}}); \
    QTRY_COMPARE(fixture.peer.count("ready"), 1)

#define FOCUS_PEER(fixture) \
    fixture.peer.send(fixture.state()); \
    QTRY_VERIFY(fixture.host->hasActiveFocus()); \
    QTRY_VERIFY(fixture.peer.count("status") > 0)

class NativeKeyboardHostTests : public QObject {
    Q_OBJECT
private slots:
    void socketPrivacyAndCleanup() {
        QString path;
        {
            Fixture f;
            path = f.host->socketPath();
            QVERIFY(!path.isEmpty());
            struct stat info{};
            QVERIFY(::stat(QFile::encodeName(QFileInfo(path).absolutePath()).constData(), &info) == 0);
            QCOMPARE(info.st_mode & 0777, mode_t(0700));
            QVERIFY(::stat(QFile::encodeName(path).constData(), &info) == 0);
            QCOMPARE(info.st_mode & 0077, mode_t(0));
            CONNECT_PEER(f);
        }
        QVERIFY(!QFileInfo::exists(path));
        QVERIFY(!QFileInfo::exists(QFileInfo(path).absolutePath()));
    }
    void publicInputMethodFocusAndQueries() {
        Fixture f;
        CONNECT_PEER(f);
        auto state = f.state();
        state.insert("cursor", 5);
        state.insert("anchor", 2);
        state.insert("echoMode", 2);
        state.insert("maximumLength", 100);
        state.insert("hints", int(Qt::ImhEmailCharactersOnly));
        f.peer.send(state);
        QTRY_VERIFY(f.host->hasActiveFocus());
        QTRY_VERIFY(f.peer.count("status") > 0);
        QCOMPARE(f.window.activeFocusItem(), f.host);
        QCOMPARE(QGuiApplication::inputMethod()->queryFocusObject(Qt::ImSurroundingText, {}).toString(), state.value("text").toString());
        QVERIFY(f.host->inputMethodQuery(Qt::ImEnabled).toBool());
        QCOMPARE(f.host->inputMethodQuery(Qt::ImCurrentSelection).toString(), QString::fromUtf8("é😀"));
        QCOMPARE(f.host->inputMethodQuery(Qt::ImCursorPosition).toInt(), 5);
        QCOMPARE(f.host->inputMethodQuery(Qt::ImAnchorPosition).toInt(), 2);
        QCOMPARE(f.host->inputMethodQuery(Qt::ImMaximumTextLength).toInt(), 100);
        QCOMPARE(QInputMethod::queryFocusObject(Qt::ImTextBeforeCursor, 2).toString(), QString::fromUtf8("😀"));
        QVERIFY(f.host->inputMethodQuery(Qt::ImHints).toInt() & Qt::ImhHiddenText);
        QVERIFY(f.host->inputMethodQuery(Qt::ImHints).toInt() & Qt::ImhSensitiveData);
        QVERIFY(f.host->inputMethodQuery(Qt::ImHints).toInt() & Qt::ImhEmailCharactersOnly);
        const QRectF rect = f.host->inputMethodQuery(Qt::ImCursorRectangle).toRectF();
        QCOMPARE(rect, QRectF(80, 60, qreal(4) / 3, 16));
        // Offscreen has no native panel: the bridge must report that honestly.
        QVERIFY(!f.peer.last("status").value("visible").toBool());
        QCOMPARE(f.peer.last("status").value("keyboardRect").toArray(), QJsonArray({0, 0, 0, 0}));
    }
    void unicodeCompositionSelectionAndAck() {
        Fixture f;
        CONNECT_PEER(f);
        auto state = f.state();
        state.insert("cursor", 5);
        state.insert("anchor", 2);
        f.peer.send(state);
        QTRY_VERIFY(f.host->hasActiveFocus());
        QTextCharFormat format;
        format.setUnderlineStyle(QTextCharFormat::DashUnderline);
        format.setUnderlineColor(QColor("#123456"));
        QList<QInputMethodEvent::Attribute> attributes{
            {QInputMethodEvent::TextFormat, 0, 2, QVariant::fromValue(QTextFormat(format))},
            {QInputMethodEvent::Cursor, 2, 1, QVariant()},
        };
        QInputMethodEvent event(QString::fromUtf8("â"), attributes);
        event.setCommitString(QString::fromUtf8("Ça"));
        QCoreApplication::sendEvent(f.host, &event);
        QVERIFY(event.isAccepted());
        QTRY_COMPARE(f.peer.count("inputMethod"), 1);
        const auto frame = f.peer.last("inputMethod");
        QCOMPARE(frame.value("commit").toString(), QString::fromUtf8("Ça"));
        QCOMPARE(frame.value("preedit").toString(), QString::fromUtf8("â"));
        QCOMPARE(frame.value("seq").toInteger(), 1);
        const auto encodedFormat = frame.value("attributes").toArray()[0].toObject().value("value").toObject();
        QCOMPARE(encodedFormat.value("underlineStyle").toInt(), int(QTextCharFormat::DashUnderline));
        QCOMPARE(QColor(encodedFormat.value("underlineColor").toString()), QColor("#123456"));
        QCOMPARE(f.host->inputMethodQuery(Qt::ImSurroundingText).toString(), QString::fromUtf8("abÇacd"));
        // The old snapshot cannot overwrite the optimistic native query state.
        state.insert("type", "state");
        f.peer.send(state);
        QTest::qWait(10);
        QCOMPARE(f.host->inputMethodQuery(Qt::ImSurroundingText).toString(), QString::fromUtf8("abÇacd"));
        f.peer.send(f.state("field-a", "state", QString::fromUtf8("client validé"), 1));
        QTRY_COMPARE(f.host->inputMethodQuery(Qt::ImSurroundingText).toString(), QString::fromUtf8("client validé"));
    }
    void keysKeepUnicodeAndSequence() {
        Fixture f;
        CONNECT_PEER(f);
        FOCUS_PEER(f);
        QKeyEvent press(QEvent::KeyPress, Qt::Key_E, Qt::ShiftModifier, QString::fromUtf8("É"), true, 2);
        QKeyEvent release(QEvent::KeyRelease, Qt::Key_E, Qt::ShiftModifier, QString::fromUtf8("É"), true, 2);
        QCoreApplication::sendEvent(f.host, &press);
        QCoreApplication::sendEvent(f.host, &release);
        QTRY_COMPARE(f.peer.count("key"), 2);
        const auto frame = f.peer.last("key");
        QCOMPARE(frame.value("seq").toInteger(), 2);
        QCOMPARE(frame.value("event").toString(), QStringLiteral("release"));
        QCOMPARE(frame.value("text").toString(), QString::fromUtf8("É"));
        QCOMPARE(frame.value("modifiers").toInt(), int(Qt::ShiftModifier));
        QCOMPARE(frame.value("count").toInt(), 2);
        QVERIFY(frame.value("autorepeat").toBool());
    }
    void nativeAccentReplacementUsesRelativeOffsets() {
        Fixture f;
        CONNECT_PEER(f);
        f.peer.send(f.state("field-a", "focus", "cafe"));
        QTRY_VERIFY(f.host->hasActiveFocus());
        QInputMethodEvent removePrevious;
        removePrevious.setCommitString(QString(), -1, 1);
        QCoreApplication::sendEvent(f.host, &removePrevious);
        QTRY_COMPARE(f.peer.count("inputMethod"), 1);
        QCOMPARE(f.peer.last("inputMethod").value("replacementStart").toInt(), -1);
        QCOMPARE(f.peer.last("inputMethod").value("replacementLength").toInt(), 1);
        QCOMPARE(f.host->inputMethodQuery(Qt::ImSurroundingText).toString(), QStringLiteral("caf"));
        f.peer.send(f.state("field-a", "state", "caf", 1));
        QInputMethodEvent accent;
        accent.setCommitString(QString::fromUtf8("é"));
        QCoreApplication::sendEvent(f.host, &accent);
        QTRY_COMPARE(f.peer.count("inputMethod"), 2);
        QCOMPARE(f.host->inputMethodQuery(Qt::ImSurroundingText).toString(), QString::fromUtf8("café"));
    }
    void staleSessionAndBlurCleanup() {
        Fixture f;
        CONNECT_PEER(f);
        FOCUS_PEER(f);
        f.peer.send(f.state("field-b", "focus", "new target"));
        QTRY_COMPARE(f.host->inputMethodQuery(Qt::ImSurroundingText).toString(), QStringLiteral("new target"));
        f.peer.send({{"type", "blur"}, {"session", "field-a"}});
        f.peer.send(f.state("field-a", "state", "stale"));
        QTest::qWait(10);
        QCOMPARE(f.host->inputMethodQuery(Qt::ImSurroundingText).toString(), QStringLiteral("new target"));
        f.peer.send({{"type", "blur"}, {"session", "field-b"}});
        QTRY_VERIFY(!f.host->hasActiveFocus());
        QCOMPARE(f.host->inputMethodQuery(Qt::ImSurroundingText).toString(), QString());
        QVERIFY(!f.host->inputMethodQuery(Qt::ImEnabled).toBool());
        QTRY_COMPARE(f.peer.last("status").value("session").toString(), QStringLiteral("field-b"));
        QVERIFY(!f.peer.last("status").value("visible").toBool());
    }
    void disconnectReleasesFocusAndCanReconnect() {
        Fixture f;
        CONNECT_PEER(f);
        FOCUS_PEER(f);
        f.peer.socket.abort();
        QTRY_VERIFY(!f.host->hasActiveFocus());
        QCOMPARE(f.host->inputMethodQuery(Qt::ImSurroundingText).toString(), QString());
        f.peer.frames.clear();
        CONNECT_PEER(f);
        FOCUS_PEER(f);
    }
    void invalidKeyCannotClaimFocus() {
        Fixture f;
        f.peer.socket.connectToServer(f.host->socketPath());
        QTRY_COMPARE(f.peer.socket.state(), QLocalSocket::ConnectedState);
        f.peer.send({{"type", "hello"}, {"key", f.host->framebufferID() + 1}});
        QTRY_COMPARE(f.peer.socket.state(), QLocalSocket::UnconnectedState);
        QVERIFY(!f.host->hasActiveFocus());
        QCOMPARE(f.peer.count("ready"), 0);
    }
    void invalidStateAndOversizeCannotClaimFocus() {
        Fixture f;
        CONNECT_PEER(f);
        auto state = f.state();
        state.insert("cursor", -1);
        f.peer.send(state);
        QTRY_COMPARE(f.peer.socket.state(), QLocalSocket::UnconnectedState);
        QVERIFY(!f.host->hasActiveFocus());
        f.peer.frames.clear();
        CONNECT_PEER(f);
        f.peer.socket.write(QByteArray(1024 * 1024 + 2, 'x'));
        QTRY_COMPARE(f.peer.socket.state(), QLocalSocket::UnconnectedState);
    }
    void separateWindowsAndPeerIsolation() {
        Fixture a(35101), b(35102);
        QVERIFY(a.host->socketPath() != b.host->socketPath());
        CONNECT_PEER(a);
        CONNECT_PEER(b);
        a.window.requestActivate();
        FOCUS_PEER(a);
        b.window.requestActivate();
        FOCUS_PEER(b);
        QInputMethodEvent event;
        event.setCommitString("B");
        QCoreApplication::sendEvent(b.host, &event);
        QTRY_COMPARE(b.peer.count("inputMethod"), 1);
        QCOMPARE(a.peer.count("inputMethod"), 0);
        // Closing the inactive window's session leaves the active proxy alone.
        a.peer.socket.abort();
        QTest::qWait(10);
        QVERIFY(b.host->inputMethodQuery(Qt::ImEnabled).toBool());
        Peer third;
        third.socket.connectToServer(b.host->socketPath());
        QTRY_COMPARE(third.socket.state(), QLocalSocket::UnconnectedState);
        QVERIFY(b.host->inputMethodQuery(Qt::ImEnabled).toBool());
    }
    void rotatedGeometry_data() {
        QTest::addColumn<int>("rotation");
        for (int rotation = 0; rotation < 4; ++rotation)
            QTest::newRow(qPrintable(QString::number(rotation))) << rotation;
    }
    void rotatedGeometry() {
        QFETCH(int, rotation);
        Fixture f(35100, {640, 600});
        f.framebuffer->setFbRotation(static_cast<FBController::Rotation>(rotation));
        CONNECT_PEER(f);
        auto state = f.state();
        // A 1-pixel point in client logical coords corresponds to source (405,1620).
        state.insert("cursorRect", QJsonArray{202.5, 810, 0, 0});
        f.peer.send(state);
        QTRY_VERIFY(f.host->hasActiveFocus());
        const QList<QPointF> expected{{207.5, 450}, {480, 420}, {160, 180}, {432.5, 150}};
        const QPointF actual = f.host->inputMethodQuery(Qt::ImCursorRectangle).toRectF().topLeft();
        QVERIFY(QLineF(actual, expected[rotation]).length() < 0.001);
    }
    void occlusionRespectsWindowOffsetAndLetterbox() {
        Fixture f(35100, {900, 700});
        CONNECT_PEER(f);
        FOCUS_PEER(f);
        // Fit draws into x=[187,712], y=[0,700]. A panel below y=500
        // obscures 200/700 of the 1080-unit client surface, not 200 units.
        const QRectF overlap = f.host->keyboardRectForClient(QRectF(0, 500, 900, 200));
        QVERIFY(std::abs(overlap.x()) < 0.001);
        QVERIFY(std::abs(overlap.y() - 5400.0 / 7) < 0.001);
        QVERIFY(std::abs(overlap.width() - 810) < 0.001);
        QVERIFY(std::abs(overlap.height() - 2160.0 / 7) < 0.001);
        QVERIFY(f.host->keyboardRectForClient(QRectF(0, 0, 100, 700)).isEmpty());
        f.framebuffer->setY(-200);
        QVERIFY(f.host->keyboardRectForClient(QRectF(0, 500, 900, 200)).isEmpty());
    }
    void backgroundStateDoesNotStealNativeFocus() {
        Fixture f;
        CONNECT_PEER(f);
        FOCUS_PEER(f);
        QQuickItem otherEditor(f.window.contentItem());
        otherEditor.setFlag(QQuickItem::ItemAcceptsInputMethod);
        otherEditor.forceActiveFocus();
        QTRY_VERIFY(otherEditor.hasActiveFocus());
        f.framebuffer->setX(1);
        f.peer.send(f.state("field-a", "state", "background"));
        QTest::qWait(10);
        QVERIFY(otherEditor.hasActiveFocus());
        QInputMethodEvent rejected;
        rejected.setCommitString("must not route");
        QCoreApplication::sendEvent(f.host, &rejected);
        QVERIFY(!rejected.isAccepted());
        QCOMPARE(f.peer.count("inputMethod"), 0);
        // The actual framebuffer receiving a new user focus can reclaim its
        // existing session without requiring the remote field to change.
        f.framebuffer->forceActiveFocus();
        QTRY_VERIFY(f.host->hasActiveFocus());
    }
    void qmlBindingsPublishSocketBeforeApplicationLaunch() {
        qmlRegisterType<FBController>("RePaper.NativeKeyboardHost.Tests", 1, 0, "TestFramebuffer");
        qmlRegisterType<NativeKeyboardHost>("RePaper.NativeKeyboardHost.Tests", 1, 0, "TestKeyboardHost");
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData(R"QML(
import QtQuick 2.15
import RePaper.NativeKeyboardHost.Tests 1.0
Item {
    property int framebufferKey: -1
    readonly property string endpoint: nativeHost.socketPath
    TestFramebuffer {
        id: canvas
        TestKeyboardHost {
            id: nativeHost
            anchors.fill: parent
            framebufferID: framebufferKey
            framebufferItem: canvas
        }
    }
}
)QML", QUrl("qrc:/native-keyboard-host-fixture.qml"));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> root(component.create());
        QVERIFY2(root != nullptr, qPrintable(component.errorString()));
        QVERIFY(root->property("endpoint").toString().isEmpty());
        root->setProperty("framebufferKey", 35130);
        // AppLoad assigns its key then immediately reads this property to put
        // the endpoint in the environment before launching the child process.
        QVERIFY(!root->property("endpoint").toString().isEmpty());
        QVERIFY(QFileInfo::exists(root->property("endpoint").toString()));
    }
};

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    NativeKeyboardHostTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "NativeKeyboardHostTests.moc"
