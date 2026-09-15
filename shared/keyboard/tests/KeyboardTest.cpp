#include "KeyboardController.h"
#include "ClipboardBridge.h"
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QSignalSpy>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

class FakeClipboard : public repaper::ClipboardBridge {
public:
    int requests=0;QString value;bool delayed=false;
    void request() override {++requests;if(!delayed)QTimer::singleShot(0,this,[this]{emit ready(value);});}
    void finish(){emit ready(value);}
};

class KeyboardTest : public QObject {
    Q_OBJECT
    std::unique_ptr<QQmlApplicationEngine> engine;
    QQuickWindow *window=nullptr;
    repaper::KeyboardController *keyboard=nullptr;
    FakeClipboard clipboard;
    static QQuickItem *findItem(QQuickItem *root,const QString &name){if(root->objectName()==name)return root;for(auto child:root->childItems())if(auto found=findItem(child,name))return found;return nullptr;}
    QQuickItem *field(const char *name){return findItem(window->contentItem(),QString::fromUtf8(name));}
    void focus(const char *name){field(name)->forceActiveFocus();QCoreApplication::processEvents();}
    void select(const char *name,int start,int end){QVERIFY(QMetaObject::invokeMethod(field(name),"select",Q_ARG(int,start),Q_ARG(int,end)));}
    QString text(const char *name){return field(name)->property("text").toString();}
    void setText(const char *name,const QString &text){field(name)->setProperty("text",text);field(name)->setProperty("cursorPosition",text.size());}
    void click(const char *name){auto item=field(name);QVERIFY(item);QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,item->mapToScene({item->width()/2,item->height()/2}).toPoint());}
private slots:
    void initTestCase(){QQuickStyle::setStyle("Basic");repaper::registerKeyboardTypes();}
    void init(){
        engine=std::make_unique<QQmlApplicationEngine>();engine->load(QUrl("qrc:/keyboard-test/KeyboardFixture.qml"));
        QVERIFY(!engine->rootObjects().isEmpty());window=qobject_cast<QQuickWindow *>(engine->rootObjects().first());QVERIFY(window);
        keyboard=window->findChild<repaper::KeyboardController *>("controller");QVERIFY(keyboard);keyboard->setClipboardBridge(&clipboard);
        clipboard.requests=0;clipboard.value.clear();clipboard.delayed=false;QVERIFY(QTest::qWaitForWindowExposed(window));
    }
    void cleanup(){engine.reset();window=nullptr;keyboard=nullptr;}
    void focusControlsFallbackAndReadonlyNeverGetsKeyboard(){
        QVERIFY(!keyboard->fallbackVisible());focus("plain");QTRY_VERIFY(keyboard->fallbackVisible());QVERIFY(!keyboard->platformVisible());
        click("keyboardKey_a");QCOMPARE(text("plain"),QString("a"));QVERIFY(field("plain")->hasActiveFocus());
        field("plain")->setProperty("readOnly",true);QTRY_VERIFY(!keyboard->fallbackVisible());
        focus("readonly");QVERIFY(!keyboard->target());keyboard->insertText("interdit");QCOMPARE(text("readonly"),QString("Lecture seule"));
        focus("password");QTRY_VERIFY(keyboard->fallbackVisible());click("outside");QTRY_VERIFY(!keyboard->fallbackVisible());
    }
    void insertionReplacesSelectionAndBackspaceDeletesGraphemes(){
        focus("plain");setText("plain",QString::fromUtf8("abéçcd"));select("plain",2,4);keyboard->insertText("Z");QCOMPARE(text("plain"),QString("abZcd"));
        setText("plain",QString::fromUtf8("A😀e\u0301👩‍💻"));
        keyboard->backspace();QCOMPARE(text("plain"),QString::fromUtf8("A😀e\u0301"));
        keyboard->backspace();QCOMPARE(text("plain"),QString::fromUtf8("A😀"));
        keyboard->backspace();QCOMPARE(text("plain"),QString("A"));
        setText("plain","abcdef");select("plain",1,4);keyboard->backspace();QCOMPARE(text("plain"),QString("aef"));
        focus("multiline");setText("multiline",QString::fromUtf8("ligne\nété😀"));keyboard->backspace();QCOMPARE(text("multiline"),QString::fromUtf8("ligne\nété"));
    }
    void returnAndArrowKeysHaveRealTextControlSemantics(){
        focus("plain");setText("plain","ac");field("plain")->setProperty("cursorPosition",1);keyboard->insertText("b");keyboard->moveCursor(-1);keyboard->insertText("x");QCOMPARE(text("plain"),QString("axbc"));
        keyboard->enter();QCOMPARE(window->property("acceptedCount").toInt(),1);
        focus("multiline");setText("multiline","un");keyboard->enter();keyboard->insertText("deux");QCOMPARE(text("multiline"),QString("un\ndeux"));
    }
    void ctrlVPastesOnlyAfterExplicitActionIntoPasswordAndSelection(){
        QCOMPARE(clipboard.requests,0);focus("plain");QTRY_VERIFY(keyboard->fallbackVisible());QCOMPARE(clipboard.requests,0);
        setText("plain","avant ancien après");select("plain",6,12);clipboard.value=QString::fromUtf8("été 😀");
        QTest::keyClick(window,Qt::Key_V,Qt::ControlModifier);QTRY_COMPARE(text("plain"),QString::fromUtf8("avant été 😀 après"));QCOMPARE(clipboard.requests,1);
        focus("password");setText("password","ancien secret");select("password",0,13);clipboard.value=QString::fromUtf8("sûr-éè😀");
        QTest::keyClick(window,Qt::Key_V,Qt::ControlModifier);QTRY_COMPARE(text("password"),clipboard.value);QCOMPARE(clipboard.requests,2);
        QTRY_VERIFY(keyboard->fallbackVisible());clipboard.value="X";click("keyboardPaste");QTRY_VERIFY(text("password").endsWith("X"));QCOMPARE(clipboard.requests,3);
        keyboard->dismiss();QTRY_VERIFY(!keyboard->fallbackVisible());QTest::keyClick(window,Qt::Key_V,Qt::ControlModifier);QCOMPARE(clipboard.requests,3);
    }
    void asynchronousPasteCannotLeakIntoAnotherFieldOrEditedSelection(){
        focus("plain");setText("plain","texte");clipboard.value="secret";clipboard.delayed=true;keyboard->paste();QCOMPARE(clipboard.requests,1);
        focus("password");keyboard->paste();QCOMPARE(clipboard.requests,1);clipboard.finish();QCOMPARE(text("plain"),QString("texte"));QCOMPARE(text("password"),QString());
        focus("plain");keyboard->paste();keyboard->insertText("!");clipboard.finish();QCOMPARE(text("plain"),QString("texte!"));
        select("plain",3,0);keyboard->paste();select("plain",4,0);clipboard.finish();QCOMPARE(text("plain"),QString("texte!"));
    }
    void windowsClipboardAdapterUsesExplicitBoundedUtf8Pipe(){
#ifdef Q_OS_WIN
        QSKIP("The WSL pipe adapter is Linux-specific.");
#else
        QTemporaryDir directory;QVERIFY(directory.isValid());const auto executable=directory.filePath("powershell.exe");
        QFile script(executable);QVERIFY(script.open(QIODevice::WriteOnly));script.write("#!/bin/sh\nprintf '%s' 'élève 😀'\n");script.close();QVERIFY(script.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner));
        const auto oldPath=qgetenv("PATH"),oldMode=qgetenv("REPAPER_PC_EMULATOR");
        qputenv("PATH",directory.path().toUtf8());qputenv("REPAPER_PC_EMULATOR","1");
        repaper::ClipboardBridge bridge;QSignalSpy ready(&bridge,&repaper::ClipboardBridge::ready);QSignalSpy failed(&bridge,&repaper::ClipboardBridge::failed);
        QCOMPARE(ready.size(),0);bridge.request();
        // Restore immediately: the child command was already resolved and
        // spawned; no real Windows clipboard command is reachable in this test.
        qputenv("PATH",oldPath);if(oldMode.isNull())qunsetenv("REPAPER_PC_EMULATOR");else qputenv("REPAPER_PC_EMULATOR",oldMode);
        QTRY_COMPARE(ready.size(),1);QCOMPARE(failed.size(),0);QCOMPARE(ready.first().first().toString(),QString::fromUtf8("élève 😀"));
#endif
    }
    void fallbackButtonsStayInsideSmallViewport(){
        window->resize(800,800);focus("plain");QTRY_VERIFY(keyboard->fallbackVisible());QTest::qWait(40);
        for(const auto &name:{"keyboardPaste","keyboardKey_a","keyboardKey_ç","keyboardBackspace","keyboardEnter"}){
            auto item=field(name);QVERIFY(item);QVERIFY(QRectF(0,0,800,800).contains(item->mapRectToScene(item->boundingRect())));
        }
    }
};
QTEST_MAIN(KeyboardTest)
#include "KeyboardTest.moc"
