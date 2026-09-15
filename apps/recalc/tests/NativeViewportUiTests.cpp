#include "app/Calculator.hpp"
#include "layout/MathCanvas.hpp"
#include "AppLoadViewport.h"
#include "NativeViewportHost.h"
#include "PaperFonts.h"
#include "qtfb/FBController.h"
#include "qtfb/fbmanagement.h"
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>

static std::map<int,QPointer<FBController>> controllers;
namespace qtfb::management {
void registerController(FBKey key,QPointer<FBController> value){controllers[key]=value;}
void unregisterController(FBKey key){controllers.erase(key);}
bool isControllerAssociated(FBKey key){return controllers.count(key)&&controllers[key];}
void forwardUserInput(FBKey,const UserInputContents&){}
void sendDeviceStateChange(FBKey,const DeviceStateChangedContents&){}
void start(){}
}

namespace {
QList<QQuickItem*> visualItems(QQuickItem *root){
    QList<QQuickItem*> items;
    for(auto *child:root->childItems()){items.append(child);items.append(visualItems(child));}
    return items;
}
bool shown(QQuickItem *item){
    for(auto *ancestor=item;ancestor;ancestor=ancestor->parentItem())if(!ancestor->isVisible())return false;
    return item&&item->width()>0&&item->height()>0;
}
QQuickItem *button(QQuickWindow *window,const QString &text){
    for(auto *item:visualItems(window->contentItem()))
        if(shown(item)&&item->isEnabled()&&item->property("text").toString()==text
            &&item->metaObject()->indexOfSignal("clicked()")>=0)return item;
    return nullptr;
}
QQuickItem *sheet(QQuickWindow *window,const QString &title){
    for(auto *item:visualItems(window->contentItem()))
        if(shown(item)&&item->property("title").toString()==title)return item;
    return nullptr;
}
bool inside(QRectF outer,QRectF inner){return outer.adjusted(-.5,-.5,.5,.5).contains(inner);}

struct Fixture {
    const int key=98765;
    QQuickWindow hostWindow;
    FBController framebuffer;
    NativeViewportHost host;
    AppLoadViewport viewport;
    recalc::Calculator calculator;
    QQmlApplicationEngine engine;
    QQuickWindow *window=nullptr;
    QQuickItem *surface=nullptr;
    QImage framebufferImage;
    QStringList errors;QString inputError;
    Fixture(QSize size,int rotation){
        framebuffer.setParentItem(hostWindow.contentItem());framebuffer.setSize(size);
        framebuffer.setProperty("allowScaling",true);framebuffer.setProperty("fillMode",FBController::Stretch);
        framebuffer.setFramebufferID(key);framebuffer.setFbRotation(static_cast<FBController::Rotation>(rotation));
        host.setFramebufferItem(&framebuffer);host.setFramebufferID(key);
        QObject::connect(&engine,&QQmlEngine::warnings,[this](const QList<QQmlError> &warnings){
            for(const auto &warning:warnings)errors.append(warning.toString());
        });
        engine.rootContext()->setContextProperty("calc",&calculator);
        engine.rootContext()->setContextProperty("appLoadViewport",&viewport);
        engine.load(QUrl("qrc:/recalc/qml/Main.qml"));
        if(engine.rootObjects().isEmpty())return;
        window=qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        if(!window)return;
        window->resize(900,1280);viewport.setWindow(window);
        surface=window->findChild<QQuickItem*>("calculatorSurface");
        viewport.setEndpoint(host.socketPath(),key);
    }
    ~Fixture(){viewport.setEndpoint({},-1);if(window)window->hide();}
    bool refreshImage(){
        if(!window)return false;
        const auto image=window->grabWindow();if(image.isNull())return false;
        framebufferImage=image.convertToFormat(QImage::Format_RGB32);
        framebuffer.associateSHM(&framebufferImage);QCoreApplication::processEvents();return framebuffer.active();
    }
    QPoint inputPoint(QPointF visibleHostPoint){
        const auto point=framebuffer.convertPointToQTFBPixels(visibleHostPoint);
        return point?QPoint(qRound(point->x()/window->devicePixelRatio()),qRound(point->y()/window->devicePixelRatio())):QPoint(-100,-100);
    }
    bool click(QQuickItem *item){
        if(!shown(item)||!refreshImage()){inputError="Control or framebuffer unavailable";return false;}
        const auto rect=item->mapRectToItem(surface,QRectF(0,0,item->width(),item->height()));
        if(!inside(QRectF(0,0,surface->width(),surface->height()),rect)){
            inputError=QString("Control outside host: %1,%2 %3x%4").arg(rect.x()).arg(rect.y()).arg(rect.width()).arg(rect.height());return false;
        }
        const auto visible=item->mapToItem(surface,QPointF(item->width()/2,item->height()/2));
        const auto point=inputPoint(visible);
        const auto expected=item->mapToScene(QPointF(item->width()/2,item->height()/2));
        if(QLineF(QPointF(point),expected).length()>2.5){inputError=QString("Input error %1 at %2,%3; expected %4,%5")
            .arg(QLineF(QPointF(point),expected).length()).arg(point.x()).arg(point.y()).arg(expected.x()).arg(expected.y());return false;}
        QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,point,10);return true;
    }
    bool click(const QString &text){return click(button(window,text));}
    bool capture(const QString &name){
        const auto folder=qEnvironmentVariable("RECALC_NATIVE_UI_OUTPUT");
        if(folder.isEmpty())return true;
        window->requestUpdate();QTest::qWait(40);
        if(!refreshImage()||!QDir().mkpath(folder))return false;
        QImage output(qRound(framebuffer.width()),qRound(framebuffer.height()),QImage::Format_RGB32);output.fill(Qt::magenta);
        {QPainter painter(&output);framebuffer.paint(&painter);}
        for(const auto &point:{QPoint(2,2),QPoint(output.width()-3,2),QPoint(2,output.height()-3),QPoint(output.width()-3,output.height()-3)})
            if(output.pixelColor(point)==QColor(Qt::magenta))return false;
        return output.save(QDir(folder).filePath(name+".png"));
    }
    bool scrollTo(QQuickItem *target){
        if(!target||!refreshImage())return false;
        QStringList chain;
        const auto mouse=[&](QPointF hostPoint,QEvent::Type type,Qt::MouseButton button,Qt::MouseButtons buttons,ulong timestamp){
            const auto local=QPointF(inputPoint(hostPoint));const auto global=QPointF(window->mapToGlobal(local.toPoint()));
            QMouseEvent event(type,local,local,global,button,buttons,Qt::NoModifier,Qt::MouseEventSynthesizedByApplication);
            event.setTimestamp(timestamp);QCoreApplication::sendEvent(window,&event);QTest::qWait(15);
        };
        for(auto *parent=target->parentItem();parent;parent=parent->parentItem())
            if(parent->metaObject()->indexOfProperty("contentY")>=0&&parent->metaObject()->indexOfProperty("contentHeight")>=0){
                for(int attempt=0;attempt<5;++attempt){
                    const auto rect=target->mapRectToItem(parent,QRectF(0,0,target->width(),target->height()));
                    if(inside(QRectF(0,0,parent->width(),parent->height()),rect))return true;
                    const auto start=parent->mapToItem(surface,QPointF(parent->width()/2,parent->height()*.95));
                    const auto end=parent->mapToItem(surface,QPointF(parent->width()/2,parent->height()*.05));
                    const ulong clock=1000+attempt*1000;
                    mouse(start,QEvent::MouseButtonPress,Qt::LeftButton,Qt::LeftButton,clock);
                    for(int step=1;step<=8;++step)mouse(start+(end-start)*(step/8.),QEvent::MouseMove,Qt::NoButton,Qt::LeftButton,clock+step*20);
                    mouse(end,QEvent::MouseButtonRelease,Qt::LeftButton,Qt::NoButton,clock+200);QTest::qWait(100);
                    for(int wait=0;wait<40&&parent->property("moving").toBool();++wait)QTest::qWait(25);
                }
                inputError=QString("Scroll did not reveal target: height=%1 content=%2 y=%3 interactive=%4")
                    .arg(parent->height()).arg(parent->property("contentHeight").toDouble()).arg(parent->property("contentY").toDouble()).arg(parent->property("interactive").toBool());return false;
            }else chain.append(QString::fromLatin1(parent->metaObject()->className()));
        inputError="No flickable ancestor: "+chain.join(" > ");
        return false;
    }
};
}

class NativeViewportUiTests:public QObject {
    Q_OBJECT
private slots:
    void controlsAndSheetsFollowTheActualFramebuffer_data(){
        QTest::addColumn<QSize>("size");QTest::addColumn<int>("rotation");
        for(auto size:{QSize(1620,2160),QSize(810,1080),QSize(400,700),QSize(1200,550)})
            for(int rotation=0;rotation<4;++rotation)
                QTest::newRow(qPrintable(QString("%1x%2-r%3").arg(size.width()).arg(size.height()).arg(rotation)))<<size<<rotation;
    }
    void controlsAndSheetsFollowTheActualFramebuffer(){
        QFETCH(QSize,size);QFETCH(int,rotation);Fixture fixture(size,rotation);
        QVERIFY2(fixture.window,qPrintable(fixture.errors.join('\n')));QVERIFY(fixture.surface);
        QVERIFY(QTest::qWaitForWindowExposed(fixture.window));QTRY_VERIFY(fixture.viewport.available());
        QTRY_COMPARE(fixture.surface->width(),qreal(size.width()));QTRY_COMPARE(fixture.surface->height(),qreal(size.height()));
        QTest::qWait(40);QVERIFY(fixture.errors.isEmpty());
        QVERIFY(fixture.capture(QString::fromLatin1(QTest::currentDataTag())+"-initial"));
        for(const auto &key:QStringList{"Effacer","1","+","2","="})QVERIFY2(fixture.click(key),qPrintable(key+": "+fixture.inputError));
        QTRY_COMPARE(fixture.calculator.result(),QString("= 3"));
        const bool save=rotation==0||size==QSize(1200,550)&&rotation==2;
        const auto prefix=QString::fromLatin1(QTest::currentDataTag());
        if(save)QVERIFY(fixture.capture(prefix+"-keypad"));
        QVERIFY(fixture.click("Options"));QTRY_VERIFY(sheet(fixture.window,"Options"));
        auto *options=sheet(fixture.window,"Options");
        QVERIFY(inside(QRectF(QPointF(),size),options->mapRectToItem(fixture.surface,QRectF(0,0,options->width(),options->height()))));
        if(save)QVERIFY(fixture.capture(prefix+"-options"));
        auto *help=button(fixture.window,"Mode d’emploi");QVERIFY(help);
        QVERIFY2(fixture.scrollTo(help),qPrintable(fixture.inputError));QVERIFY(fixture.click(help));
        QTRY_VERIFY(sheet(fixture.window,"Mode d’emploi"));QVERIFY(!sheet(fixture.window,"Options"));
        if(save)QVERIFY(fixture.capture(prefix+"-help"));
        QVERIFY(fixture.click("Fermer"));QTRY_VERIFY(!sheet(fixture.window,"Mode d’emploi"));
        QVERIFY(fixture.click("Historique"));QTRY_VERIFY(sheet(fixture.window,"Historique"));
        QVERIFY(fixture.calculator.history().size()>0);
        if(save)QVERIFY(fixture.capture(prefix+"-history"));
        QVERIFY(fixture.click("Fermer"));QTRY_VERIFY(!sheet(fixture.window,"Historique"));
        // The header close target never intercepts the keypad after closing.
        QVERIFY(fixture.click("Effacer"));QVERIFY(fixture.click("7"));QVERIFY(fixture.click("="));
        QTRY_COMPARE(fixture.calculator.result(),QString("= 7"));
        QVERIFY(fixture.click("Options"));QTRY_VERIFY(sheet(fixture.window,"Options"));
        QTest::keyClick(fixture.window,Qt::Key_Escape);QTRY_VERIFY(!sheet(fixture.window,"Options"));
        QVERIFY2(fixture.errors.isEmpty(),qPrintable(fixture.errors.join('\n')));
    }
    void resizingAndRotatingAnOpenSheetKeepsItsControlsReachable(){
        Fixture fixture({1620,2160},0);QVERIFY(fixture.window);QVERIFY(fixture.surface);
        QVERIFY(QTest::qWaitForWindowExposed(fixture.window));QTRY_VERIFY(fixture.viewport.available());QTest::qWait(40);
        QVERIFY(fixture.click("Options"));QTRY_VERIFY(sheet(fixture.window,"Options"));
        fixture.framebuffer.setSize({400,700});fixture.framebuffer.setFbRotation(FBController::Deg90R);
        QTRY_COMPARE(fixture.surface->width(),qreal(400));QTRY_COMPARE(fixture.surface->height(),qreal(700));QTest::qWait(40);
        QCOMPARE(fixture.window->size(),QSize(900,1280));
        auto *help=button(fixture.window,"Mode d’emploi");QVERIFY(help);QVERIFY2(fixture.scrollTo(help),qPrintable(fixture.inputError));
        QVERIFY(fixture.click(help));QTRY_VERIFY(sheet(fixture.window,"Mode d’emploi"));
        QVERIFY(fixture.capture("live-resize-400x700-r2-help"));QVERIFY(fixture.click("Fermer"));
        fixture.framebuffer.setSize({810,1080});fixture.framebuffer.setFbRotation(FBController::Deg180);
        QTRY_COMPARE(fixture.surface->width(),qreal(810));QTRY_COMPARE(fixture.surface->height(),qreal(1080));QTest::qWait(40);
        for(const auto &key:QStringList{"Effacer","9","-","2","="})
            QVERIFY2(fixture.click(key=="-"?QString::fromUtf8("−"):key),qPrintable(key+": "+fixture.inputError));
        QTRY_COMPARE(fixture.calculator.result(),QString("= 7"));QVERIFY(fixture.capture("live-resize-810x1080-r3-keypad"));
        QVERIFY2(fixture.errors.isEmpty(),qPrintable(fixture.errors.join('\n')));
    }
};

int main(int argc,char **argv){
    QTemporaryDir data; if(!data.isValid())return 2;
    qputenv("XDG_DATA_HOME",data.filePath("data").toUtf8());qputenv("XDG_CONFIG_HOME",data.filePath("config").toUtf8());
    qputenv("XDG_CACHE_HOME",data.filePath("cache").toUtf8());
    QCoreApplication::setOrganizationName("RePaperTests");QCoreApplication::setApplicationName("recalc-native-viewport");
    QQuickStyle::setStyle("Basic");QGuiApplication app(argc,argv);repaper::registerPaperFonts();
    qmlRegisterType<recalc::MathCanvas>("ReCalc",1,0,"MathCanvas");
    qmlRegisterUncreatableType<recalc::Calculator>("ReCalc",1,0,"Calculator","Test instance");
    NativeViewportUiTests tests;return QTest::qExec(&tests,argc,argv);
}
#include "NativeViewportUiTests.moc"
