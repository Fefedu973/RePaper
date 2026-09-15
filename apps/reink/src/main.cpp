#include "PaperFonts.h"
#include "InkCanvas.h"
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>
#include <cstdio>

int main(int argc,char *argv[]) {
    qInstallMessageHandler([](QtMsgType,const QMessageLogContext &,const QString &message){std::fprintf(stderr,"%s\n",qPrintable(message));});
    QGuiApplication app(argc,argv);
    repaper::registerPaperFonts();app.setOrganizationName("RePaper");app.setApplicationName("reink");
    QQuickStyle::setStyle("Basic");qmlRegisterType<InkCanvas>("RePaper.Drawing",1,0,"InkCanvas");
    QQmlApplicationEngine engine;
    QObject::connect(&engine,&QQmlApplicationEngine::warnings,[](const QList<QQmlError> &errors){for(const auto &error:errors)std::fprintf(stderr,"%s\n",qPrintable(error.toString()));});
    const int screenshot=app.arguments().indexOf("--screenshot");
    const QString path=screenshot>=0&&screenshot+1<app.arguments().size()?app.arguments()[screenshot+1]:QString();
    QObject::connect(&engine,&QQmlApplicationEngine::objectCreated,&app,[&app,path](QObject *root,const QUrl &){
        if(!root){app.exit(1);return;}
        if(!path.isEmpty())QTimer::singleShot(1000,&app,[&app,root,path]{
            auto window=qobject_cast<QQuickWindow*>(root);app.exit(window&&window->grabWindow().save(path)?0:2);
        });
    },Qt::QueuedConnection);
    engine.load(QUrl("qrc:/reink/Main.qml"));
    return app.exec();
}
