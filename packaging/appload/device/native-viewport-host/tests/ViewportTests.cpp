#include "NativeViewportHost.h"
#include "AppLoadViewport.h"
#include "qtfb/FBController.h"
#include "qtfb/fbmanagement.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QPainter>
#include <QProcess>
#include <QProcessEnvironment>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>

static std::map<int, QPointer<FBController>> controllers;
namespace qtfb::management {
void registerController(FBKey key, QPointer<FBController> value) { controllers[key] = value; }
void unregisterController(FBKey key) { controllers.erase(key); }
bool isControllerAssociated(FBKey key) { return controllers.count(key) && controllers[key]; }
void forwardUserInput(FBKey, const UserInputContents &) {}
void sendDeviceStateChange(FBKey, const DeviceStateChangedContents &) {}
void start() {}
}

struct Fixture {
    const int key = 87654;
    QImage image{1620, 2160, QImage::Format_RGB32};
    QQuickWindow hostWindow, clientWindow;
    FBController framebuffer;
    NativeViewportHost host;
    AppLoadViewport client{&clientWindow};
    Fixture(QSize size = {1200, 550}, int rotation = 0) {
        image.fill(Qt::white);
        clientWindow.resize(540, 720);
        hostWindow.resize(size);
        framebuffer.setParentItem(hostWindow.contentItem());
        framebuffer.setSize(size);
        framebuffer.setProperty("allowScaling", true);
        framebuffer.setProperty("fillMode", FBController::Stretch);
        framebuffer.setFramebufferID(key);
        framebuffer.setFbRotation(static_cast<FBController::Rotation>(rotation));
        framebuffer.associateSHM(&image);
        host.setFramebufferItem(&framebuffer);
        host.setFramebufferID(key);
    }
    void connectClient() { client.setEndpoint(host.socketPath(), key); }
    QImage paintHost() {
        QImage output(qRound(framebuffer.width()), qRound(framebuffer.height()), QImage::Format_RGB32);
        output.fill(Qt::magenta);
        QPainter painter(&output);
        framebuffer.paint(&painter);
        return output;
    }
};

static QImage pattern(QSize size) {
    QImage result(size, QImage::Format_RGB32);
    result.fill(Qt::white);
    QPainter painter(&result);
    painter.fillRect(0, 0, 55, 55, Qt::black);
    painter.fillRect(size.width()-65, 0, 65, 45, Qt::red);
    painter.fillRect(0, size.height()-45, 75, 45, Qt::blue);
    painter.fillRect(size.width()-55, size.height()-55, 55, 55, Qt::green);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::black);
    painter.drawEllipse(QRect(size.width()/2-40, size.height()/2-40, 80, 80));
    return result;
}

class ViewportTests : public QObject {
    Q_OBJECT
private slots:
    void fillsEveryShapeWithoutDistortion_data() {
        QTest::addColumn<QSize>("size"); QTest::addColumn<int>("rotation");
        for (QSize size : {QSize(400,300), QSize(1200,550), QSize(600,1000), QSize(1620,700)})
            for (int rotation=0; rotation<4; ++rotation) {
                const auto name = QString("%1x%2-r%3").arg(size.width()).arg(size.height()).arg(rotation).toLatin1();
                QTest::newRow(name.constData()) << size << rotation;
            }
    }
    void fillsEveryShapeWithoutDistortion() {
        QFETCH(QSize,size); QFETCH(int,rotation);
        Fixture f(size,rotation); f.connectClient();
        QTRY_VERIFY(f.client.available());
        QCOMPARE(f.client.width(),qreal(size.width())); QCOMPARE(f.client.height(),qreal(size.height()));
        const QImage expected=pattern(size);
        {
            QPainter painter(&f.image);
            painter.scale(3,3);
            painter.setTransform(f.client.contentTransform().toTransform(),true);
            painter.drawImage(0,0,expected);
        }
        const QImage actual=f.paintHost();
        int checked=0;
        for(int y=3;y<size.height()-3;y+=7)for(int x=3;x<size.width()-3;x+=7) {
            const auto color=expected.pixel(x,y);
            bool flat=true;
            for(int dy=-3;dy<=3 && flat;++dy)for(int dx=-3;dx<=3;++dx)
                if(expected.pixel(x+dx,y+dy)!=color){flat=false;break;}
            if(flat){QCOMPARE(actual.pixel(x,y),color);++checked;}
        }
        QVERIFY(checked>1500);
        // Actual framebuffer input mapping followed by Qt's device-pixel
        // conversion and the surface inverse must return the visible point.
        const auto fromWindow=f.client.contentTransform().toTransform().inverted();
        for(QPointF point : {QPointF(20,20), QPointF(size.width()-20,20),
                            QPointF(20,size.height()-20), QPointF(size.width()/2,size.height()/2)}) {
            const auto framebufferPoint=f.framebuffer.convertPointToQTFBPixels(point);
            QVERIFY(framebufferPoint.has_value());
            const QPointF recovered=fromWindow.map(QPointF(*framebufferPoint)/3.0);
            QVERIFY2(QLineF(point,recovered).length()<2.5,qPrintable(QString("Pointer error %1").arg(QLineF(point,recovered).length())));
        }
        // Native IME reports its rectangle in the Qt window coordinates.
        // The surface must recover its actual bottom occlusion for all rotations.
        const QRectF keyboardRect(0,size.height()-100,size.width(),100);
        const QRectF windowRect=f.client.contentTransform().toTransform().mapRect(keyboardRect);
        const QRectF recovered=f.client.mapRectFromWindow(windowRect);
        QVERIFY(QLineF(recovered.topLeft(),keyboardRect.topLeft()).length()<0.01);
        QVERIFY(QLineF(recovered.bottomRight(),keyboardRect.bottomRight()).length()<0.01);
    }
    void updatesAndReconnectsWithoutRestart() {
        Fixture f; f.connectClient(); QTRY_VERIFY(f.client.available());
        f.framebuffer.setSize({1000,400});
        QTRY_COMPARE(f.client.width(),qreal(1000)); QTRY_COMPARE(f.client.height(),qreal(400));
        f.framebuffer.setFbRotation(FBController::Deg90R);
        QTRY_VERIFY(std::abs(f.client.contentTransform()(0,0))<0.001);
        f.client.setEndpoint(QString(),-1);
        QTRY_VERIFY(!f.host.connected()); QVERIFY(!f.client.available());
        QCOMPARE(f.client.mapRectFromWindow(QRectF(10,20,30,40)),QRectF(10,20,30,40));
        f.client.setEndpoint(f.host.socketPath(),f.key);
        QTRY_VERIFY(f.client.available()); QTRY_VERIFY(f.host.connected());
        QCOMPARE(f.client.width(),qreal(1000));
    }
    void rejectsWrongKeyAndOversizedFrames() {
        Fixture f;
        QLocalSocket invalid;
        invalid.connectToServer(f.host.socketPath());
        QTRY_COMPARE(invalid.state(),QLocalSocket::ConnectedState);
        invalid.write(QJsonDocument(QJsonObject{{"v",1},{"type","hello"},{"key",f.key+1}}).toJson(QJsonDocument::Compact)+'\n');
        QTRY_COMPARE(invalid.state(),QLocalSocket::UnconnectedState);
        QVERIFY(!f.host.connected());
        invalid.connectToServer(f.host.socketPath());
        QTRY_COMPARE(invalid.state(),QLocalSocket::ConnectedState);
        invalid.write(QByteArray(16384,'x'));
        QTRY_COMPARE(invalid.state(),QLocalSocket::UnconnectedState);
        QVERIFY(!f.host.connected());
        f.connectClient(); QTRY_VERIFY(f.client.available());
    }
    void realReaderCapture_data() {
        QTest::addColumn<QSize>("size"); QTest::addColumn<int>("rotation"); QTest::addColumn<bool>("reading");
        QTest::newRow("wide-normal") << QSize(1200,550) << 0 << false;
        QTest::newRow("wide-reading") << QSize(1200,550) << 0 << true;
        QTest::newRow("rotated-reading") << QSize(1200,550) << 2 << true;
        QTest::newRow("small-normal") << QSize(500,350) << 0 << false;
    }
    void realReaderCapture() {
        const QString executable=qEnvironmentVariable("REPDF_GUI_EXECUTABLE");
        if(executable.isEmpty())QSKIP("Real GUI capture requested in the separate Xvfb visual run");
        QFETCH(QSize,size);QFETCH(int,rotation);QFETCH(bool,reading);
        Fixture f(size,rotation);
        QTemporaryDir temp; QVERIFY(temp.isValid());
        const QString capture=temp.filePath("framebuffer.png");
        QProcess process;
        auto environment=QProcessEnvironment::systemEnvironment();
        environment.insert("REPAPER_VIEWPORT_SOCKET",f.host.socketPath());
        environment.insert("QTFB_KEY",QString::number(f.key));
        environment.insert("REPAPER_APPLOAD_TABLET_INPUT","1");
        environment.insert("QT_QPA_PLATFORM","xcb");
        process.setProcessEnvironment(environment);
        QStringList args{"--size","540x720","--open",qEnvironmentVariable("REPDF_GUI_FIXTURE"),"--screenshot",capture};
        if(reading)args<<"--reading-mode";
        process.start(executable,args);
        QVERIFY(process.waitForStarted());
        QTRY_VERIFY_WITH_TIMEOUT(process.state()==QProcess::NotRunning,25000);
        const QByteArray errors=process.readAllStandardError();
        QVERIFY2(process.exitStatus()==QProcess::NormalExit && process.exitCode()==0,errors.constData());
        const QImage image(capture); QCOMPARE(image.size(),QSize(1620,2160));
        f.image=image.convertToFormat(QImage::Format_RGB32);
        const QImage composed=f.paintHost();
        const QString folder=qEnvironmentVariable("REPDF_GUI_OUTPUT");
        QVERIFY(QDir().mkpath(folder));
        QVERIFY(composed.save(QDir(folder).filePath(QString::fromLatin1(QTest::currentDataTag())+".png")));
        QVERIFY2(!errors.contains("rePDF:") && !errors.contains("Binding loop") && !errors.contains("ReferenceError"),errors.constData());
    }
};
QTEST_MAIN(ViewportTests)
#include "ViewportTests.moc"
