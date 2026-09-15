#include "AgendaController.h"
#include "KeyboardController.h"
#include "ClipboardBridge.h"
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QStandardPaths>
#include <QTimer>

class ProbeClipboard:public repaper::ClipboardBridge {
public:
    int calls=0;
    const QString value=QString::fromUtf8("collage-éè-😀");
    void request() override{++calls;emit ready(value);}
};
class KeyTrace:public QObject {
public:
    int controls=0,pastes=0;
    bool eventFilter(QObject *,QEvent *event) override {
        if(event->type()==QEvent::KeyPress){const auto key=static_cast<QKeyEvent *>(event);if(key->key()==Qt::Key_Control)++controls;if(key->key()==Qt::Key_V&&key->modifiers().testFlag(Qt::ControlModifier))++pastes;}
        return false;
    }
};
int main(int argc,char **argv) {
    QGuiApplication app(argc,argv);app.setOrganizationName("RePaperTests");app.setApplicationName("keyboard-qtfb-probe");QStandardPaths::setTestModeEnabled(true);
    if(qEnvironmentVariable("REPAPER_PC_EMULATOR")!="1"||app.arguments().size()!=2)return 2;
    const auto resultPath=app.arguments().at(1);
    QQuickStyle::setStyle("Basic");repaper::registerKeyboardTypes();AgendaController agenda;QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("agenda",&agenda);engine.rootContext()->setContextProperty("previewPage",QString());engine.load(QUrl("qrc:/reagenda/Main.qml"));
    if(engine.rootObjects().isEmpty())return 3;
    auto window=qobject_cast<QQuickWindow *>(engine.rootObjects().first());auto keyboard=window->findChild<repaper::KeyboardController *>("textKeyboard");
    auto username=window->findChild<QQuickItem *>("loginUsername"),password=window->findChild<QQuickItem *>("loginPassword");auto dialog=window->findChild<QObject *>("loginDialog");
    if(!keyboard||!username||!password||!dialog)return 4;
    ProbeClipboard clipboard;keyboard->setClipboardBridge(&clipboard);KeyTrace trace;app.installEventFilter(&trace);
    int stage=0;
    auto save=[&]{QFile file(resultPath);if(file.open(QIODevice::WriteOnly))file.write(QJsonDocument(QJsonObject{{"plainPaste",stage>=1},{"passwordPaste",stage>=2},{"clipboardRequests",clipboard.calls},{"controlPresses",trace.controls},{"ctrlVPresses",trace.pastes}}).toJson());};
    QTimer::singleShot(300,&app,[&]{QMetaObject::invokeMethod(dialog,"open");username->setProperty("text","ancien utilisateur");username->forceActiveFocus();QMetaObject::invokeMethod(username,"selectAll");save();});
    QTimer check;check.setInterval(30);QObject::connect(&check,&QTimer::timeout,&app,[&]{
        if(stage==0&&username->property("text").toString()==clipboard.value){stage=1;password->setProperty("text","ancien mot de passe");password->forceActiveFocus();QMetaObject::invokeMethod(password,"selectAll");save();}
        else if(stage==1&&password->property("text").toString()==clipboard.value){stage=2;save();}
    });check.start();
    QTimer::singleShot(8000,&app,[&]{save();app.exit(stage==2&&clipboard.calls==2&&trace.pastes>=2?0:1);});
    return app.exec();
}
