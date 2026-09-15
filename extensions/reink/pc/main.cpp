#include "EditorAdapter.h"
#include "NativePreviewItem.h"
#include "NativeInputScheduler.h"
#include "InkCanvas.h"
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>
#include <cstdio>

static void populateDemo(InkCanvas *page) {
    if(!page)return;
    if(page->itemCount()!=0){
        const auto items=page->documentSnapshot()["items"].toList();
        if(!items.isEmpty())page->selectObject(items.first().toMap()["id"].toString());
        return;
    }
    auto viewPoint=[page](QPointF p) {
        const qreal scale=std::min(page->width()/1404.,page->height()/1872.);
        return QPointF((page->width()-1404*scale)/2,(page->height()-1872*scale)/2)+p*scale;
    };
    auto draw=[&](const QString &tool,QPointF from,QPointF to) {
        page->setTool(tool);const auto a=viewPoint(from),b=viewPoint(to);
        page->begin(a.x(),a.y());page->move(b.x(),b.y());page->end(b.x(),b.y());
        const auto items=page->documentSnapshot()["items"].toList();
        if(!items.isEmpty())page->selectObject(items.last().toMap()["id"].toString());
    };
    page->insertSymbol("resistor-iec",400,650);
    const QString resistor=page->selectedObjectId();
    page->insertSymbol("capacitor",960,650);
    draw("wire",QPointF(520,650),QPointF(840,650));
    draw("arrow",QPointF(320,350),QPointF(1040,350));
    page->setSelectionArrowDirection("both");
    draw("rectangle",QPointF(250,1080),QPointF(650,1370));
    page->setSelectionCornerRadius(35);
    draw("ellipse",QPointF(850,1080),QPointF(1150,1370));
    draw("wire",QPointF(650,1225),QPointF(850,1225));
    page->selectObject(resistor);
}

int main(int argc,char **argv) {
    qInstallMessageHandler([](QtMsgType,const QMessageLogContext &,const QString &message){std::fprintf(stderr,"%s\n",qPrintable(message));});
    QGuiApplication app(argc,argv);
    Q_INIT_RESOURCE(editor_panels);
    Q_INIT_RESOURCE(preview);
    const auto args=app.arguments();
    app.setOrganizationName("RePaper");
    app.setApplicationName(args.contains("--demo")?"editor-demo":"editor-preview");
    QQuickStyle::setStyle("Basic");
    qmlRegisterType<EditorAdapter>("RePaper.Editor",1,0,"EditorAdapter");
    qmlRegisterType<NativePreviewItem>("RePaper.Editor",1,0,"NativePreviewItem");
    qmlRegisterType<NativeInputScheduler>("RePaper.Editor",1,0,"NativeInputScheduler");
    qmlRegisterType<InkCanvas>("RePaper.Drawing",1,0,"InkCanvas");
    QQmlApplicationEngine engine;engine.load(QUrl("qrc:/repaper/editor/Preview.qml"));
    if(engine.rootObjects().isEmpty()) {
        std::fprintf(stderr,"Could not create the PC preview window.\n");
        return 1;
    }
    const int paletteOption=args.indexOf("--palette");
    const int stencilOption=args.indexOf("--stencil");
    if(args.contains("--demo")||paletteOption>=0||stencilOption>=0)QTimer::singleShot(100,&app,[&]{
        auto root=engine.rootObjects().first();
        if(args.contains("--demo"))populateDemo(root->findChild<InkCanvas*>("editorPreviewPage"));
        if(paletteOption>=0&&paletteOption+1<args.size())
            QMetaObject::invokeMethod(root,"showPalette",Q_ARG(QVariant,QVariant(args[paletteOption+1])));
        if(stencilOption>=0&&stencilOption+1<args.size()) {
            QMetaObject::invokeMethod(root,"showPalette",Q_ARG(QVariant,QVariant("restencil")));
            if(auto sidebar=root->findChild<QObject*>("editorSidebar"))
                QMetaObject::invokeMethod(sidebar,"configureStencil",Q_ARG(QVariant,QVariant(args[stencilOption+1])),Q_ARG(QVariant,QVariant(false)));
        }
    });
    const int option=args.indexOf("--screenshot");
    if(option>=0&&option+1<args.size())QTimer::singleShot(700,&app,[&]{
        auto window=qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        const auto capture=window?window->grabWindow():QImage{};
        if(capture.isNull()) {
            std::fprintf(stderr,"The window rendered no screenshot on platform '%s'. For headless Linux, use xvfb-run -a with QT_QPA_PLATFORM=xcb.\n",qPrintable(QGuiApplication::platformName()));
            app.exit(2);
        } else if(!capture.save(args[option+1])) {
            std::fprintf(stderr,"Could not save the screenshot: %s\n",qPrintable(args[option+1]));
            app.exit(2);
        } else app.exit(0);
    });
    return app.exec();
}
