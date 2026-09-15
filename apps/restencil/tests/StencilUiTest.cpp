#include "ActivePageAdapter.h"
#include "InkCanvas.h"
#include "StencilCatalogue.h"
#include "KeyboardController.h"
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QtTest>

class StencilUiTest:public QObject {
    Q_OBJECT
    static QJsonArray savedItems() {
        QFile file(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)+"/drawing.json");
        if(!file.open(QIODevice::ReadOnly))return {};
        return QJsonDocument::fromJson(file.readAll()).object()["items"].toArray();
    }
    static QPointF firstPoint() {
        auto xy=savedItems()[0].toObject()["strokes"].toArray()[0].toObject()["points"].toArray()[0].toArray();
        return {xy[0].toDouble(),xy[1].toDouble()};
    }
private slots:
    void searchUsesSharedKeyboardAndHidesOnCommandFocus() {
        StencilCatalogue catalogue;ActivePageAdapter adapter;QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("catalogue",&catalogue);engine.rootContext()->setContextProperty("activePageAdapter",&adapter);engine.load(QUrl("qrc:/restencil/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());auto window=qobject_cast<QQuickWindow *>(engine.rootObjects().first());QVERIFY(window);window->resize(936,1248);
        auto search=window->findChild<QQuickItem *>("symbolSearch");auto keyboard=window->findChild<repaper::KeyboardController *>("textKeyboard");QVERIFY(search);QVERIFY(keyboard);
        search->forceActiveFocus();QTRY_VERIFY(keyboard->fallbackVisible());keyboard->insertText("résistance");QCOMPARE(search->property("text").toString(),QString("résistance"));
        QVERIFY(QRectF(0,0,936,1248).contains(search->mapRectToScene(search->boundingRect())));keyboard->dismiss();QTRY_VERIFY(!keyboard->fallbackVisible());
    }
    void actualSidebarClickInsertMoveScaleCopyUndo() {
        const QString path=QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)+"/drawing.json";
        QFile::remove(path);
        StencilCatalogue catalogue;ActivePageAdapter adapter;QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("catalogue",&catalogue);
        engine.rootContext()->setContextProperty("activePageAdapter",&adapter);
        engine.load(QUrl("qrc:/restencil/Main.qml"));
        QTRY_VERIFY_WITH_TIMEOUT(!engine.rootObjects().isEmpty(),5000);
        auto window=qobject_cast<QQuickWindow*>(engine.rootObjects().first());QVERIFY(window);
        auto page=window->findChild<InkCanvas*>("emulatorPage");QVERIFY(page);
        auto insert=window->findChild<QQuickItem*>("previewInsertButton");QVERIFY(insert);
        QTRY_VERIFY(adapter.available());QTRY_VERIFY(insert->isVisible());QVERIFY(insert->isEnabled());
        QTest::qWait(100);
        auto click=[window](QQuickItem *item){QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,item->mapToScene(QPointF(item->width()/2,item->height()/2)).toPoint());};
        click(insert);
        QTRY_COMPARE(page->itemCount(),1);QTRY_VERIFY(page->isVisible());QVERIFY(page->hasSelection());
        const QPointF original=firstPoint();
        QTest::qWait(100);
        auto center=page->mapToScene(QPointF(page->width()/2,page->height()/2)).toPoint();
        QTest::mousePress(window,Qt::LeftButton,Qt::NoModifier,center);
        QTest::mouseMove(window,center+QPoint(32,24),40);
        QTest::mouseRelease(window,Qt::LeftButton,Qt::NoModifier,center+QPoint(32,24));
        QTRY_VERIFY(firstPoint()!=original);
        auto undo=window->findChild<QQuickItem*>("selectionUndo");QVERIFY(undo);click(undo);
        QTRY_COMPARE(firstPoint(),original);
        click(page);
        QTRY_VERIFY(page->hasSelection());
        auto scale=window->findChild<QQuickItem*>("selectionScaleUp");QVERIFY(scale);click(scale);
        QTRY_VERIFY(firstPoint()!=original);
        auto copy=window->findChild<QQuickItem*>("selectionCopy");QVERIFY(copy);click(copy);
        QTRY_COMPARE(page->itemCount(),2);click(undo);QTRY_COMPARE(page->itemCount(),1);
        click(page);QTRY_VERIFY(page->hasSelection());
        const QString screenshot=qEnvironmentVariable("PAPER_UI_EVIDENCE");
        if(!screenshot.isEmpty()){QTest::qWait(150);QVERIFY2(window->grabWindow().save(screenshot),"Use the xcb platform under WSLg for screenshots.");}
        QFile::remove(path);
    }
};

int main(int argc,char **argv) {
    int appArgc=2;char emulator[]="--emulator";char *appArgv[]={argv[0],emulator,nullptr};
    QGuiApplication app(appArgc,appArgv);
    app.setOrganizationName("RePaperTests");app.setApplicationName("restencil-ui-test");
    QStandardPaths::setTestModeEnabled(true);
    QQuickStyle::setStyle("Basic");
    repaper::registerKeyboardTypes();
    qmlRegisterType<SymbolPreview>("RePaper.Drawing",1,0,"SymbolPreview");
    qmlRegisterType<InkCanvas>("RePaper.Drawing",1,0,"InkCanvas");
    StencilUiTest test;return QTest::qExec(&test,argc,argv);
}
#include "StencilUiTest.moc"
