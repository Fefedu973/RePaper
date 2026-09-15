#include "KeyboardController.h"
#include "NativeKeyboardClient.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QTemporaryDir>
#include <QtTest>

class NativeKeyboardClientTest : public QObject {
    Q_OBJECT
    QTemporaryDir directory;
    QLocalServer server;
    QPointer<QLocalSocket> peer;
    QByteArray buffer;
    QList<QJsonObject> messages;
    std::unique_ptr<QQmlApplicationEngine> engine;
    QQuickWindow *window = nullptr;
    repaper::KeyboardController *keyboard = nullptr;
    repaper::NativeKeyboardClient *client = nullptr;

    QQuickItem *findItem(QQuickItem *root, const QString &name) {
        if (root->objectName() == name) return root;
        for (auto child : root->childItems()) if (auto item = findItem(child, name)) return item;
        return nullptr;
    }
    QQuickItem *field(const char *name) { return findItem(window->contentItem(), QString::fromLatin1(name)); }
    QList<QJsonObject> ofType(const QString &type) {
        QList<QJsonObject> result;
        for (const auto &message : messages) if (message.value("type").toString() == type) result.append(message);
        return result;
    }
    void send(QJsonObject message) {
        QVERIFY(peer);
        message["v"] = 1;
        peer->write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
        peer->flush();
    }
    void acknowledge(const QString &session, bool visible = true) {
        send({{"type", "status"}, {"session", session}, {"visible", visible},
              {"keyboardRect", visible ? QJsonArray{0, 800, 936, 448} : QJsonArray{0, 0, 0, 0}}});
    }
    void begin(const char *name = "plain") {
        field(name)->forceActiveFocus();
        QTRY_VERIFY(peer);
        QTRY_COMPARE(ofType("hello").size(), 1);
        QCOMPARE(ofType("hello").last().value("key").toInt(), 4567);
        send({{"type", "ready"}});
        QTRY_VERIFY(!ofType("focus").isEmpty());
    }
    QString session() { return ofType("focus").last().value("session").toString(); }
    void commit(const QString &session, int sequence, const QString &text) {
        send({{"type", "inputMethod"}, {"session", session}, {"seq", sequence},
              {"commit", text}, {"preedit", ""}, {"replacementStart", 0}, {"replacementLength", 0},
              {"attributes", QJsonArray{}}});
    }
private slots:
    void initTestCase() {
        QVERIFY(directory.isValid());
        QQuickStyle::setStyle("Basic");
        repaper::registerKeyboardTypes();
        connect(&server, &QLocalServer::newConnection, this, [this] {
            peer = server.nextPendingConnection();
            connect(peer, &QLocalSocket::readyRead, this, [this] {
                buffer += peer->readAll();
                while (buffer.contains('\n')) {
                    const auto end = buffer.indexOf('\n');
                    messages.append(QJsonDocument::fromJson(buffer.left(end)).object());
                    buffer.remove(0, end + 1);
                }
            });
        });
    }
    void init() {
        QVERIFY(server.listen(directory.filePath("keyboard.sock")));
        engine = std::make_unique<QQmlApplicationEngine>();
        engine->load(QUrl("qrc:/native-keyboard-test/KeyboardFixture.qml"));
        QVERIFY(!engine->rootObjects().isEmpty());
        window = qobject_cast<QQuickWindow *>(engine->rootObjects().first());
        QVERIFY(window);
        keyboard = window->findChild<repaper::KeyboardController *>("controller");
        QVERIFY(keyboard);
        client = keyboard->findChild<repaper::NativeKeyboardClient *>();
        QVERIFY(client);
        client->setEndpoint(server.fullServerName(), 4567);
        QVERIFY(QTest::qWaitForWindowExposed(window));
    }
    void cleanup() {
        engine.reset();
        if (peer) { peer->abort(); delete peer; }
        peer = nullptr;
        server.close();
        buffer.clear();
        messages.clear();
    }
    void nativeAvailabilityAndOcclusionComeFromTheHost() {
        begin();
        QVERIFY(client->waiting());
        QVERIFY(!keyboard->platformVisible());
        QVERIFY(!keyboard->fallbackVisible());
        acknowledge(session(), false);
        QTRY_VERIFY(client->available());
        QVERIFY(!keyboard->platformVisible());
        // Collapsing the real native keyboard must not reveal the fallback.
        QTest::qWait(300);
        QVERIFY(!keyboard->fallbackVisible());
        acknowledge(session());
        QTRY_VERIFY(keyboard->platformVisible());
        QCOMPARE(keyboard->platformHeight(), qreal(448));
        send({{"type", "status"}, {"session", session()}, {"visible", true},
              {"keyboardRect", QJsonArray{0, 1100, 936, 148}}});
        QTRY_COMPARE(keyboard->platformHeight(), qreal(148));
        peer->disconnectFromServer();
        QTRY_VERIFY(!client->available());
        QTRY_VERIFY(keyboard->fallbackVisible());
    }
    void unicodeSelectionAndRealKeyEventsReachTheFocusedControl() {
        auto plain = field("plain");
        plain->setProperty("text", QString::fromUtf8("avant ancien après"));
        begin();
        const QString current = session();
        acknowledge(current);
        QVERIFY(QMetaObject::invokeMethod(plain, "select", Q_ARG(int, 6), Q_ARG(int, 12)));
        QTRY_VERIFY(!ofType("state").isEmpty());
        commit(current, 1, QString::fromUtf8("été 😀"));
        QTRY_COMPARE(plain->property("text").toString(), QString::fromUtf8("avant été 😀 après"));
        QTRY_COMPARE(ofType("state").last().value("ack").toInt(), 1);
        QCOMPARE(ofType("state").last().value("text").toString(), plain->property("text").toString());
        send({{"type", "key"}, {"session", current}, {"seq", 2}, {"event", "press"},
              {"key", Qt::Key_Return}, {"modifiers", 0}, {"text", "\r"}, {"autorepeat", false}, {"count", 1}});
        QTRY_COMPARE(window->property("acceptedCount").toInt(), 1);
        QTRY_COMPARE(ofType("state").last().value("ack").toInt(), 2);
        // A duplicate transport event must never insert twice.
        commit(current, 1, "duplicate");
        QTest::qWait(20);
        QCOMPARE(plain->property("text").toString(), QString::fromUtf8("avant été 😀 après"));
    }
    void passwordAndFocusChangesFenceLateInput() {
        begin();
        const QString oldSession = session();
        acknowledge(oldSession);
        field("password")->forceActiveFocus();
        QTRY_COMPARE(ofType("focus").size(), 2);
        const QString passwordSession = session();
        QVERIFY(passwordSession != oldSession);
        QVERIFY(ofType("focus").last().value("hints").toInt() & Qt::ImhHiddenText);
        acknowledge(passwordSession);
        commit(oldSession, 1, "wrong-field");
        commit(passwordSession, 1, QString::fromUtf8("faux mot-é😀"));
        QTRY_COMPARE(field("password")->property("text").toString(), QString::fromUtf8("faux mot-é😀"));
        QCOMPARE(field("plain")->property("text").toString(), QString());
        field("password")->setProperty("readOnly", true);
        QTRY_VERIFY(!keyboard->target());
        commit(passwordSession, 2, "late");
        QTest::qWait(20);
        QCOMPARE(field("password")->property("text").toString(), QString::fromUtf8("faux mot-é😀"));
    }
    void preeditAndReplacementUseInputMethodSemanticsAndClearOnDisconnect() {
        field("plain")->setProperty("text", "ac");
        begin();
        field("plain")->setProperty("cursorPosition", 1);
        const QString current = session();
        acknowledge(current);
        send({{"type", "inputMethod"}, {"session", current}, {"seq", 1},
              {"commit", ""}, {"preedit", QString::fromUtf8("é")},
              {"replacementStart", 0}, {"replacementLength", 0},
              {"attributes", QJsonArray{QJsonObject{{"type", QInputMethodEvent::Cursor}, {"start", 1}, {"length", 1}, {"value", 0}}}}});
        QTRY_COMPARE(field("plain")->property("preeditText").toString(), QString::fromUtf8("é"));
        QCOMPARE(field("plain")->property("text").toString(), QString("ac"));
        send({{"type", "inputMethod"}, {"session", current}, {"seq", 2},
              {"commit", "b"}, {"preedit", ""}, {"replacementStart", 0}, {"replacementLength", 0},
              {"attributes", QJsonArray{}}});
        QTRY_COMPARE(field("plain")->property("text").toString(), QString("abc"));
        send({{"type", "inputMethod"}, {"session", current}, {"seq", 3},
              {"commit", ""}, {"preedit", "pending"}, {"replacementStart", 0}, {"replacementLength", 0},
              {"attributes", QJsonArray{}}});
        QTRY_COMPARE(field("plain")->property("preeditText").toString(), QString("pending"));
        peer->disconnectFromServer();
        QTRY_VERIFY(!client->available());
        QTRY_COMPARE(field("plain")->property("preeditText").toString(), QString());
        QCOMPARE(field("plain")->property("text").toString(), QString("abc"));
    }
    void missingHostHandshakeFallsBackWithoutClaimingNativeVisibility() {
        field("plain")->forceActiveFocus();
        QTRY_VERIFY(peer);
        QTRY_VERIFY(client->waiting());
        QTRY_VERIFY_WITH_TIMEOUT(!client->waiting(), 2500);
        QVERIFY(!client->available());
        QVERIFY(!keyboard->platformVisible());
        QTRY_VERIFY(keyboard->fallbackVisible());
    }
    void invalidInputSequenceDisconnectsWithoutEditing() {
        begin();
        acknowledge(session());
        QTRY_VERIFY(client->available());
        commit(session(), 2, "missing-prior-event");
        QTRY_VERIFY(!client->available());
        QCOMPARE(field("plain")->property("text").toString(), QString());
        QTRY_VERIFY(keyboard->fallbackVisible());
    }
};
QTEST_MAIN(NativeKeyboardClientTest)
#include "NativeKeyboardClientTest.moc"
