#include "PdfLibrary.h"
#include "PdfView.h"
#include "AppLoadViewport.h"
#include "KeyboardController.h"
#include "TabletMouseAdapter.h"
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QTimer>
#include <cstdio>

int main(int argc, char **argv) {
    // The fixed framebuffer stays at three pixels per Qt window unit.
    // AppLoadViewport maps the responsive surface to that framebuffer using
    // the live host geometry, keeping controls and PDF text undistorted.
    if(qEnvironmentVariableIntValue("REPAPER_APPLOAD_TABLET_INPUT") == 1)
        qputenv("QT_SCALE_FACTOR", "3");
    QCoreApplication::setOrganizationName("RePaper");
    QCoreApplication::setApplicationName("repdf");
    QCoreApplication::setApplicationVersion("0.1.0");
    QQuickStyle::setStyle("Basic");
    QGuiApplication app(argc,argv);
    QCommandLineParser parser;
    parser.setApplicationDescription("rePDF — visionneuse PDF en lecture seule pour AppLoad");
    parser.addHelpOption(); parser.addVersionOption();
    parser.addOptions({
        {"open","Ouvrir un PDF local au démarrage.","pdf"},
        {"library-root","Dossier de la bibliothèque reMarkable.","directory",QDir::homePath()+"/.local/share/remarkable/xochitl"},
        {"files-root","Racine du sélecteur de fichiers.","directory",QDir::homePath()},
        {"screenshot","Capturer le banc PC puis quitter.","png"},
        {"reading-mode","Ouvrir le PDF en mode lecture dans la fenêtre."},
        {"size","Dimensions du banc PC.","widthxheight"}
    });
    parser.process(app);
    repaper::installTabletMouseAdapter();
    repaper::registerKeyboardTypes();
    qmlRegisterType<PdfView>("RePdf",1,0,"PdfView");
    PdfLibrary library(parser.value("library-root"),parser.value("files-root"));
    AppLoadViewport appLoadViewport(nullptr);
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("pdfLibrary",&library);
    engine.rootContext()->setContextProperty("appLoadViewport",&appLoadViewport);
    QObject::connect(&engine,&QQmlEngine::warnings,&app,[](const QList<QQmlError> &errors){
        for(const auto &error:errors)std::fprintf(stderr,"rePDF: %s\n",qPrintable(error.toString()));
    });
    engine.load(QUrl("qrc:/repdf/qml/Main.qml"));
    if(engine.rootObjects().isEmpty())return 1;
    auto window=qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    if(!window)return 1;
    QObject::connect(&appLoadViewport,&AppLoadViewport::changed,window,[window]{
        QTimer::singleShot(0,window,[window]{
            if(auto keyboard=window->findChild<repaper::KeyboardController*>("pdfKeyboard"))
                keyboard->refreshGeometry();
        });
    });
    appLoadViewport.setWindow(window);
    if(parser.isSet("size")) {
        const auto match=QRegularExpression("^([0-9]{3,4})x([0-9]{3,4})$").match(parser.value("size"));
        if(!match.hasMatch())return 2;
        window->resize(qBound(320,match.captured(1).toInt(),4000),qBound(240,match.captured(2).toInt(),4000));
    }
    if(parser.isSet("open"))QTimer::singleShot(0,window,[window,&parser]{
        if(parser.isSet("reading-mode")) {
            if(auto view=window->findChild<PdfView*>("pdfView")) {
                QObject::connect(view,&PdfView::documentChanged,window,[window,view]{
                    if(view->pageCount()>0)window->setProperty("readingMode",true);
                });
            }
        }
        const auto path=QFileInfo(parser.value("open")).absoluteFilePath();
        QMetaObject::invokeMethod(window,"openDocument",Q_ARG(QVariant,QVariant(path)),
                                  Q_ARG(QVariant,QVariant(QFileInfo(path).completeBaseName())));
    });
    QTimer capture;
    int attempts=0;
    if(parser.isSet("screenshot")) {
        capture.setInterval(250);
        QObject::connect(&capture,&QTimer::timeout,&app,[&]{
            auto view=window->findChild<PdfView*>("pdfView");
            if(++attempts<60 && (attempts<5 || library.property("busy").toBool() || (view&&view->property("busy").toBool())))return;
            capture.stop();
            const auto image=window->grabWindow();
            if(image.isNull() && QGuiApplication::platformName()=="offscreen") {
                const auto grab=window->contentItem()->grabToImage();
                if(grab) {
                    const auto path=parser.value("screenshot");
                    QObject::connect(grab.data(),&QQuickItemGrabResult::ready,&app,[grab,path,&app]{
                        if(grab->image().isNull()||!grab->saveToFile(path)) {
                            std::fprintf(stderr,"rePDF: capture du contenu indisponible.\n");app.exit(2);
                        } else app.exit(0);
                    },Qt::SingleShotConnection);
                    QTimer::singleShot(10000,&app,[&app]{
                        std::fprintf(stderr,"rePDF: délai de capture dépassé.\n");app.exit(2);
                    });
                    return;
                }
            }
            if(image.isNull()||!image.save(parser.value("screenshot"))) {
                std::fprintf(stderr,"rePDF: capture indisponible. En mode sans écran, utiliser Xvfb et QT_QPA_PLATFORM=xcb.\n");
                app.exit(2);
            } else app.exit(0);
        });
        capture.start();
    }
    return app.exec();
}
