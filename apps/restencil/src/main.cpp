#include "StencilCatalogue.h"
#include "InkCanvas.h"
#include "ActivePageAdapter.h"
#include "KeyboardController.h"
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>
#include <cstdio>

int main(int argc,char *argv[]) {
    qInstallMessageHandler([](QtMsgType,const QMessageLogContext &,const QString &message){std::fprintf(stderr,"%s\n",qPrintable(message));});
    QGuiApplication app(argc,argv);
    app.setOrganizationName("RePaper");app.setApplicationName("restencil");
    QQuickStyle::setStyle("Basic");
    repaper::registerKeyboardTypes();
    qmlRegisterType<SymbolPreview>("RePaper.Drawing",1,0,"SymbolPreview");
    qmlRegisterType<InkCanvas>("RePaper.Drawing",1,0,"InkCanvas");
    StencilCatalogue catalogue;
    ActivePageAdapter activePageAdapter;
    QQmlApplicationEngine engine;
    QObject::connect(&engine,&QQmlApplicationEngine::warnings,[](const QList<QQmlError> &errors){for(const auto &error:errors)std::fprintf(stderr,"%s\n",qPrintable(error.toString()));});
    engine.rootContext()->setContextProperty("catalogue",&catalogue);
    engine.rootContext()->setContextProperty("activePageAdapter",&activePageAdapter);
    const int screenshot=app.arguments().indexOf("--screenshot");
    const QString path=screenshot>=0&&screenshot+1<app.arguments().size()?app.arguments()[screenshot+1]:QString();
    QObject::connect(&engine,&QQmlApplicationEngine::objectCreated,&app,[&app,path](QObject *root,const QUrl &){
        if(!root){app.exit(1);return;}
        if(!path.isEmpty())QTimer::singleShot(1000,&app,[&app,root,path]{
            auto window=qobject_cast<QQuickWindow*>(root);app.exit(window&&window->grabWindow().save(path)?0:2);
        });
    },Qt::QueuedConnection);
    engine.load(QUrl("qrc:/restencil/Main.qml"));
    return app.exec();
}
