#include "Launcher.h"
#include <QFile>
#include <QQmlComponent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QtTest>

class CalculatorLibraryFixture : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList applications READ applications CONSTANT)
public:
    using QObject::QObject;
    static inline QStringList launches;
    static inline QVariantList lastArguments;
    static inline QVariantMap lastEnvironment;
    QVariantList applications() const {
        QVariantList result;
        for (const auto &id : {QString("external::recalc"), QString("external::reagenda"), QString("external::repdf")})
            result.append(QVariantMap{{"id", id}, {"name", id}, {"icon", ""}, {"externalType", 2},
                {"canHaveMultipleFrontends", true}, {"disablesWindowedMode", id.endsWith("reagenda")},
                {"supportsScaling", false}, {"aspectRatio", .75}, {"width", 0}, {"virtualKeyboardLayout", QVariant()}});
        return result;
    }
    Q_INVOKABLE bool isFrontendRunningFor(const QString &) const { return false; }
    Q_INVOKABLE int launchExternal(const QString &id, int, const QVariantList &args, const QVariantMap &env) {
        launches.append(id); lastArguments=args; lastEnvironment=env; return 100+launches.size();
    }
    Q_INVOKABLE int reloadList() { return 3; }
    Q_INVOKABLE void terminateExternal(qint64) {}
signals:
    void pidDied(qint64 pid);
};

class CalculatorWindowTests : public QObject {
    Q_OBJECT
    QTemporaryDir directory;
    QScopedPointer<QQmlEngine> engine;
    QScopedPointer<QQuickWindow> surface;
    QScopedPointer<QObject> view;
    QQuickItem *windows=nullptr;
    AppLoadLauncher *launcher=nullptr;
    QList<QQmlError> warnings;
    static bool write(const QString &path, const QByteArray &data) {
        QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(data)==data.size();
    }
    QList<QQuickItem *> openWindows() const { return windows->childItems(); }
    void request(const QString &id="external::recalc", bool windowed=true) {
        launcher->launchApplication(id, {}, {}, windowed);
        QCoreApplication::processEvents();
    }
    void dragResize(QQuickItem *window, const QPoint &delta) {
        QQuickItem *handle=nullptr;
        for(auto item:window->findChildren<QQuickItem *>()) {
            if(item->metaObject()->indexOfProperty("startX")>=0 && item->metaObject()->indexOfProperty("startY")>=0) {
                QVERIFY(!handle); handle=item;
            }
        }
        QVERIFY(handle); QVERIFY(handle->isEnabled());
        const QPoint start=handle->mapToScene(QPointF(50,50)).toPoint();
        QTest::mousePress(surface.data(),Qt::LeftButton,Qt::NoModifier,start);
        QTest::mouseMove(surface.data(),start+delta);
        QTest::mouseRelease(surface.data(),Qt::LeftButton,Qt::NoModifier,start+delta);
        QCoreApplication::processEvents();
    }
private slots:
    void initTestCase() {
        qmlRegisterType<CalculatorLibraryFixture>("net.asivery.AppLoad",1,0,"AppLoadLibrary");
        qmlRegisterSingletonType<AppLoadLauncher>("net.asivery.AppLoad",1,0,"AppLoadLauncher",&AppLoadLauncher::qmlSingleton);
        qmlRegisterModule("net.asivery.Framebuffer",1,0);
        QVERIFY(QFile::copy(QString::fromUtf8(APPLOAD_QML_SOURCE),directory.filePath("appload.qml")));
        QVERIFY(QFile::copy(QString::fromUtf8(APPLOAD_WINDOW_SOURCE),directory.filePath("actual-window.qml")));
        // Full production window.qml is used for pointer-driven resize tests.
        // These fixtures replace only native process/framebuffer boundaries.
        const QMap<QString,QByteArray> nativeTypes{
            {"AppLoadCoordinator",R"(import QtQuick
QtObject {
    property bool loaded: false
    property string applicationQMLRoot: ""
    signal unloading()
    function loadApplication(id) {}
    function close() {}
    function terminate() {}
})"},
            {"NativeKeyboardHost",R"(import QtQuick
Item {
    property int framebufferID: -1
    property Item framebufferItem: null
    readonly property string socketPath: ""
})"},
            {"NativeViewportHost",R"(import QtQuick
Item {
    objectName: "fixtureViewportHost"
    property int framebufferID: -1
    property Item framebufferItem: null
    property bool connected: false
    readonly property string socketPath: framebufferID >= 0 ? "/tmp/repaper-viewport-fixture-" + framebufferID : ""
})"},
            {"FBController",R"(import QtQuick
import QtQuick.Controls
Item {
    objectName: "fixtureFramebuffer"
    enum FillMode { PreserveAspectFit, Stretch }
    property bool allowScaling: false
    property int fillMode: 0
    property int framebufferID: -1
    property bool active: true
    property int equalsClicks: 0
    signal dragDown()
    function virtualKeyboardKeyUp(code) {}
    function virtualKeyboardKeyDown(code) {}
    Button {
        objectName: "fixtureEqualsButton"
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        width: 96
        height: 96
        text: "="
        onClicked: ++parent.equalsClicks
    }
})"},
            {"StylusInputHandler", "import QtQuick\nItem {}"}
        };
        for(auto type=nativeTypes.cbegin();type!=nativeTypes.cend();++type) {
            const auto path=directory.filePath(type.key()+".qml");
            QVERIFY(write(path,type.value()));
            const auto module=type.key()=="FBController" || type.key()=="StylusInputHandler"
                ? "net.asivery.Framebuffer" : "net.asivery.AppLoad";
            QVERIFY(qmlRegisterType(QUrl::fromLocalFile(path),module,1,0,type.key().toUtf8().constData())>=0);
        }
        // The actual patched AppLoad QML handles dispatch and lifetime. Only
        // its process boundary and framebuffer window are replaced here.
        QVERIFY(write(directory.filePath("window.qml"),R"(
import QtQuick
FocusScope {
    property string applicationId: ""
    property string appName: ""
    property bool supportsScaling: false
    property bool disablesWindowedMode: false
    property var virtualKeyboardLayout: null
    property var virtualKeyboardRef: null
    property int globalWidth: 0
    property int globalHeight: 0
    property int minWidth: 0
    property int minHeight: 0
    property int scaledContentWidth: 0
    property int scaledContentHeight: 0
    property int qtfbKey: -1
    property int appPid: -1
    property string nativeKeyboardSocket: ""
    property string nativeViewportSocket: "/tmp/repaper-window-viewport-fixture"
    property bool fullscreen: false
    property bool minimized: false
    property int maximizeCalls: 0
    signal closed()
    z: 100
    function maximize() { ++maximizeCalls; fullscreen = !fullscreen }
}
)"));
    }
    void init() {
        CalculatorLibraryFixture::launches.clear(); warnings.clear();
        CalculatorLibraryFixture::lastArguments.clear();CalculatorLibraryFixture::lastEnvironment.clear();
        engine.reset(new QQmlEngine);
        connect(engine.data(),&QQmlEngine::warnings,this,[this](const QList<QQmlError> &errors){warnings.append(errors);});
        surface.reset(new QQuickWindow);
        surface->resize(1620,2160);
        surface->show();
        windows=new QQuickItem(surface->contentItem());
        QQmlComponent component(engine.data(),QUrl::fromLocalFile(directory.filePath("appload.qml")));
        QVERIFY2(component.isReady(),qPrintable(component.errorString()));
        view.reset(component.create());QVERIFY2(view,qPrintable(component.errorString()));
        auto item=qobject_cast<QQuickItem *>(view.data());QVERIFY(item);
        item->setParentItem(surface->contentItem());item->setWidth(1620);item->setHeight(2160);
        QVERIFY(view->setProperty("absoluteRoot",QVariant::fromValue(windows)));
        launcher=engine->singletonInstance<AppLoadLauncher *>(qmlTypeId("net.asivery.AppLoad",1,0,"AppLoadLauncher"));
        QVERIFY(launcher);
        QCoreApplication::processEvents();
        QCOMPARE(item->width(),qreal(1620));QCOMPARE(item->height(),qreal(2160));
    }
    void cleanup() {
        for(const auto &warning:warnings)qWarning().noquote()<<warning.toString();
        const auto errors=warnings;
        view.reset();surface.reset();engine.reset();windows=nullptr;launcher=nullptr;
        QVERIFY(errors.isEmpty());
    }
    void repeatedRequestsLaunchOneWindowAndKeepBoundedForeground() {
        request();QCOMPARE(CalculatorLibraryFixture::launches.size(),1);QCOMPARE(openWindows().size(),1);
        auto window=openWindows().first();QCOMPARE(window->property("applicationId").toString(),QString("external::recalc"));
        for(int i=0;i<100;++i)request();
        QCOMPARE(CalculatorLibraryFixture::launches.size(),1);QCOMPARE(openWindows().first(),window);
        QCOMPARE(window->z(),qreal(101));QVERIFY(!window->property("fullscreen").toBool());
        QCOMPARE(window->property("maximizeCalls").toInt(),0);QVERIFY(window->hasActiveFocus());
    }
    void restoresExistingFullscreenMinimizedHiddenWindow() {
        request("external::recalc",false);QCOMPARE(openWindows().size(),1);auto window=openWindows().first();
        QVERIFY(window->property("fullscreen").toBool());QCOMPARE(window->z(),qreal(100));
        window->setProperty("minimized",true);window->setVisible(false);
        request();QCOMPARE(CalculatorLibraryFixture::launches.size(),1);QCOMPARE(openWindows().first(),window);
        QVERIFY(!window->property("fullscreen").toBool());QVERIFY(!window->property("minimized").toBool());
        QVERIFY(window->isVisible());QCOMPARE(window->property("maximizeCalls").toInt(),2);
        request();QCOMPARE(window->property("maximizeCalls").toInt(),2);
    }
    void closingWindowAllowsOneFreshLaunch() {
        request();QCOMPARE(openWindows().size(),1);QPointer<QQuickItem> first=openWindows().first();
        QVERIFY(QMetaObject::invokeMethod(first,"closed"));
        QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);QVERIFY(first.isNull());
        request();QCOMPARE(CalculatorLibraryFixture::launches.size(),2);QCOMPARE(openWindows().size(),1);
    }
    void pdfIsReusedAndUtilitiesExchangeForegroundWithoutGrowingZ() {
        request();QVERIFY(!CalculatorLibraryFixture::lastEnvironment.contains("REPAPER_VIEWPORT_SOCKET"));
        request("external::repdf");QCOMPARE(openWindows().size(),2);
        auto calculator=openWindows()[0];auto pdf=openWindows()[1];
        QCOMPARE(pdf->property("applicationId").toString(),QString("external::repdf"));
        QCOMPARE(CalculatorLibraryFixture::lastEnvironment.value("REPAPER_VIEWPORT_SOCKET").toString(),
            pdf->property("nativeViewportSocket").toString());
        QVERIFY(!pdf->property("nativeViewportSocket").toString().isEmpty());
        pdf->setProperty("fullscreen",true);pdf->setProperty("minimized",true);pdf->setVisible(false);
        request("external::repdf");QVERIFY(!pdf->property("fullscreen").toBool());
        QVERIFY(!pdf->property("minimized").toBool());QVERIFY(pdf->isVisible());
        for(int i=0;i<10;++i){
            request();QCOMPARE(calculator->z(),qreal(101));QCOMPARE(pdf->z(),qreal(100));
            request("external::repdf");QCOMPARE(pdf->z(),qreal(101));QCOMPARE(calculator->z(),qreal(100));
        }
        QCOMPARE(CalculatorLibraryFixture::launches.size(),2);QCOMPARE(openWindows().size(),2);
    }
    void otherLaunchPoliciesAndArgumentsStayIntact() {
        launcher->launchApplication("external::reagenda",{"--example"},{{"EXAMPLE","value"}},true);
        QCOMPARE(CalculatorLibraryFixture::lastArguments,QVariantList{"--example"});
        QCOMPARE(CalculatorLibraryFixture::lastEnvironment,QVariantMap({{"EXAMPLE","value"}}));
        request("external::reagenda",true);request("external::recalc",false);request("external::recalc",false);
        QCOMPARE(CalculatorLibraryFixture::launches.size(),4);QCOMPARE(openWindows().size(),4);
        QVERIFY(!CalculatorLibraryFixture::lastEnvironment.contains("REPAPER_VIEWPORT_SOCKET"));
        for(const auto window:openWindows()){QVERIFY(window->property("fullscreen").toBool());QCOMPARE(window->z(),qreal(100));}
        launcher->launchApplication("external::repdf",{"--example"},{{"EXAMPLE","value"}},true);
        QCOMPARE(CalculatorLibraryFixture::lastArguments,QVariantList{"--example"});
        QCOMPARE(CalculatorLibraryFixture::lastEnvironment,QVariantMap({
            {"EXAMPLE","value"},{"REPAPER_VIEWPORT_SOCKET","/tmp/repaper-window-viewport-fixture"}}));
    }
    void actualWindowResizesPdfFreelyAndKeepsCalculatorAspect_data() {
        QTest::addColumn<QString>("applicationId");
        QTest::addColumn<QPoint>("delta");
        QTest::addColumn<QSizeF>("expectedSize");
        QTest::newRow("pdf-width-only")<<QString("external::repdf")<<QPoint(150,0)<<QSizeF(750,931);
        QTest::newRow("pdf-height-only")<<QString("external::repdf")<<QPoint(0,150)<<QSizeF(600,1081);
        QTest::newRow("calculator-width-keeps-aspect")<<QString("external::recalc")<<QPoint(150,0)<<QSizeF(750,1131);
        QTest::newRow("calculator-height-keeps-aspect")<<QString("external::recalc")<<QPoint(0,150)<<QSizeF(600,931);
        QTest::newRow("pdf-minimum-size")<<QString("external::repdf")<<QPoint(-450,-600)<<QSizeF(400,664);
    }
    void actualWindowResizesPdfFreelyAndKeepsCalculatorAspect() {
        QFETCH(QString,applicationId);QFETCH(QPoint,delta);QFETCH(QSizeF,expectedSize);
        QQmlComponent actualWindow(engine.data(),QUrl::fromLocalFile(directory.filePath("actual-window.qml")));
        QVERIFY2(actualWindow.isReady(),qPrintable(actualWindow.errorString()));
        QVERIFY(view->setProperty("windowArchetype",QVariant::fromValue(&actualWindow)));
        windows->setZ(1000);
        request(applicationId);QCOMPARE(openWindows().size(),1);
        auto window=openWindows().first();
        QCOMPARE(window->property("supportsScaling").toBool(),applicationId=="external::repdf");
        auto framebuffer=window->findChild<QQuickItem *>("fixtureFramebuffer");QVERIFY(framebuffer);
        auto viewport=window->findChild<QQuickItem *>("fixtureViewportHost");QVERIFY(viewport);
        QCOMPARE(viewport->parentItem(),framebuffer);
        QCOMPARE(viewport->property("framebufferItem").value<QQuickItem *>(),framebuffer);
        const bool pdf=applicationId=="external::repdf";
        QCOMPARE(viewport->property("framebufferID").toInt(),pdf ? window->property("qtfbKey").toInt() : -1);
        QCOMPARE(window->property("nativeViewportSocket"),viewport->property("socketPath"));
        QCOMPARE(CalculatorLibraryFixture::lastEnvironment.contains("REPAPER_VIEWPORT_SOCKET"),pdf);
        if(pdf)QCOMPARE(CalculatorLibraryFixture::lastEnvironment.value("REPAPER_VIEWPORT_SOCKET"),viewport->property("socketPath"));
        // Exercise the actual production fillMode binding across connection
        // transitions. The framebuffer itself is a paint-free fixture here.
        QCOMPARE(framebuffer->property("fillMode").toInt(),0); // PreserveAspectFit
        QVERIFY(viewport->setProperty("connected",true));
        QCOMPARE(framebuffer->property("fillMode").toInt(),pdf ? 1 : 0); // Stretch only for connected PDF
        QVERIFY(viewport->setProperty("connected",false));
        QCOMPARE(framebuffer->property("fillMode").toInt(),0);
        QVERIFY(viewport->setProperty("connected",true));
        QCOMPARE(framebuffer->property("fillMode").toInt(),pdf ? 1 : 0);
        window->setWidth(600);QVERIFY(window->setProperty("_height",800));
        QCoreApplication::processEvents();QCOMPARE(window->height(),qreal(931));
        dragResize(window,delta);
        QCOMPARE(window->width(),expectedSize.width());QCOMPARE(window->height(),expectedSize.height());
        // A second horizontal drag catches accidental addition of the titlebar
        // height on every resize even when the requested vertical delta is zero.
        if(applicationId=="external::repdf") {
            dragResize(window,QPoint(60,0));
            QCOMPARE(window->width(),expectedSize.width()+60);QCOMPARE(window->height(),expectedSize.height());
        }
    }
    void actualClientCornerEqualsIsOutsideResizeGrip() {
        QQmlComponent actualWindow(engine.data(),QUrl::fromLocalFile(directory.filePath("actual-window.qml")));
        QVERIFY2(actualWindow.isReady(),qPrintable(actualWindow.errorString()));
        QVERIFY(view->setProperty("windowArchetype",QVariant::fromValue(&actualWindow)));
        windows->setZ(1000);request();QCOMPARE(openWindows().size(),1);
        auto window=openWindows().first();
        window->setWidth(600);QVERIFY(window->setProperty("_height",800));
        QCoreApplication::processEvents();
        auto framebuffer=window->findChild<QQuickItem *>("fixtureFramebuffer");QVERIFY(framebuffer);
        auto equals=window->findChild<QQuickItem *>("fixtureEqualsButton");QVERIFY(equals);
        QQuickItem *handle=nullptr;
        for(auto item:window->findChildren<QQuickItem *>())
            if(item->metaObject()->indexOfProperty("startX")>=0 && item->metaObject()->indexOfProperty("startY")>=0)handle=item;
        QVERIFY(handle);QVERIFY(handle->isVisible());QVERIFY(handle->isEnabled());
        QCOMPARE(framebuffer->height(),qreal(800));QCOMPARE(handle->height(),qreal(56));
        QVERIFY(!handle->mapRectToScene(handle->boundingRect()).intersects(framebuffer->mapRectToScene(framebuffer->boundingRect())));
        const auto before=QSizeF(window->width(),window->height());
        const QPoint point=equals->mapToScene(QPointF(equals->width()-20,equals->height()-20)).toPoint();
        QTest::mouseClick(surface.data(),Qt::LeftButton,Qt::NoModifier,point);
        QCOMPARE(framebuffer->property("equalsClicks").toInt(),1);
        QCOMPARE(QSizeF(window->width(),window->height()),before);
        QTest::mousePress(surface.data(),Qt::LeftButton,Qt::NoModifier,point);
        QTest::mouseMove(surface.data(),point+QPoint(8,8));
        QTest::mouseRelease(surface.data(),Qt::LeftButton,Qt::NoModifier,point+QPoint(8,8));
        QCOMPARE(framebuffer->property("equalsClicks").toInt(),2);
        QCOMPARE(QSizeF(window->width(),window->height()),before);
        dragResize(window,QPoint(150,0));
        QCOMPARE(window->width(),qreal(750));QCOMPARE(window->height(),qreal(1131));
        QCOMPARE(framebuffer->property("equalsClicks").toInt(),2);
        QVERIFY(QMetaObject::invokeMethod(window,"maximize"));
        QCoreApplication::processEvents();
        QVERIFY(!handle->isVisible());QVERIFY(!handle->property("enabled").toBool());
        QCOMPARE(framebuffer->height(),qreal(2160));
        QVERIFY(QMetaObject::invokeMethod(window,"maximize"));
        QCoreApplication::processEvents();
        QCOMPARE(window->height(),qreal(1131));QVERIFY(handle->isVisible());
        QVERIFY(window->setProperty("minimized",true));
        QCoreApplication::processEvents();
        QVERIFY(!handle->isVisible());QVERIFY(!handle->property("enabled").toBool());QCOMPARE(window->height(),qreal(75));
        QVERIFY(window->setProperty("minimized",false));
        QCoreApplication::processEvents();
        QVERIFY(handle->isVisible());QCOMPARE(window->height(),qreal(1131));
    }
    void unavailableWindowDoesNotDereferenceUndefined() {
        QQmlComponent unavailable(engine.data()); // Component.Null is a controlled creation failure.
        QVERIFY(view->setProperty("windowArchetype",QVariant::fromValue(&unavailable)));
        request();request("external::reagenda",true);request("external::recalc",false);
        QCOMPARE(CalculatorLibraryFixture::launches.size(),0);QVERIFY(openWindows().isEmpty());
    }
};
QTEST_MAIN(CalculatorWindowTests)
#include "CalculatorWindowTests.moc"
