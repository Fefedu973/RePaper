#include "EditorAdapter.h"
#include "InkCanvas.h"
#include <QFile>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QtTest>

class EditorWorkflowTest : public QObject {
    Q_OBJECT
    std::unique_ptr<QQmlApplicationEngine> engine;
    QQuickWindow *window = nullptr;
    InkCanvas *page = nullptr;

    static QQuickItem *descendant(QQuickItem *root, const QString &name) {
        if (!root) return nullptr;
        if (root->objectName() == name) return root;
        for (auto child : root->childItems())
            if (auto result = descendant(child, name)) return result;
        return nullptr;
    }
    QQuickItem *item(const QString &name) const {
        return descendant(window->contentItem(), name);
    }
    QObject *popup() const {
        return window->findChild<QObject *>("nativeEditorPalette");
    }
    void click(const QString &name) {
        // A retained popup can change modes before Qt polishes its layouts.
        // Compute the hit point from a submitted frame, as a user would see it.
        QSignalSpy frames(window,&QQuickWindow::frameSwapped);
        window->update();
        QTRY_VERIFY(!frames.isEmpty());
        auto control = item(name);
        QVERIFY2(control, qPrintable(name));
        QVERIFY2(control->isVisible() && control->isEnabled(), qPrintable(name));
        const QPoint point = control->mapToScene(QPointF(control->width()/2, control->height()/2)).toPoint();
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point);
    }
    QPoint pagePoint(qreal x, qreal y) const {
        const qreal scale = std::min(page->width()/1404., page->height()/1872.);
        const QPointF offset((page->width()-1404*scale)/2, (page->height()-1872*scale)/2);
        return page->mapToScene(offset + QPointF(x,y)*scale).toPoint();
    }
private slots:
    void initTestCase() {
        Q_INIT_RESOURCE(editor_panels);
        Q_INIT_RESOURCE(preview);
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName("RePaperTests");
        QCoreApplication::setApplicationName("editor-workflow");
        QQuickStyle::setStyle("Basic");
        qmlRegisterType<EditorAdapter>("RePaper.Editor", 1, 0, "EditorAdapter");
        qmlRegisterType<InkCanvas>("RePaper.Drawing", 1, 0, "InkCanvas");
    }
    void init() {
        QFile::remove(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)+"/drawing.json");
        engine = std::make_unique<QQmlApplicationEngine>();
        engine->load(QUrl("qrc:/repaper/editor/Preview.qml"));
        QVERIFY(!engine->rootObjects().isEmpty());
        window = qobject_cast<QQuickWindow *>(engine->rootObjects().first());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        page = window->findChild<InkCanvas *>("editorPreviewPage");
        QVERIFY(page);
        QTRY_VERIFY(page->width() > 100 && page->height() > 100);
        QVERIFY(popup());
    }
    void cleanup() {
        engine.reset();
        page = nullptr;
        window = nullptr;
        QFile::remove(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)+"/drawing.json");
    }
    void rectangleFromPaletteToPropertiesAndUndo() {
        click("openInkPalette");
        QTRY_VERIFY(popup()->property("opened").toBool());
        click("editorTool_rectangle");
        QTRY_COMPARE(page->tool(), QString("rectangle"));
        QTRY_VERIFY(!popup()->property("visible").toBool());
        const auto start = pagePoint(300,500), end = pagePoint(800,900);
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(window, end, 20);
        QCOMPARE(page->itemCount(), 0);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, end);
        QTRY_COMPARE(page->itemCount(), 1);
        click("openInkPalette");
        QTRY_VERIFY(popup()->property("opened").toBool());
        click("editorTool_select");
        QTRY_VERIFY(!popup()->property("visible").toBool());
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, pagePoint(550,700));
        QVERIFY(page->hasSelection());
        const auto rectangleId = page->selectedObjectId();
        click("openSelectionPalette");
        QTRY_VERIFY(popup()->property("opened").toBool());
        QCOMPARE(page->property("selectionKind").toString(), QString("rectangle"));
        auto width = item("editorShapeWidth");
        QVERIFY(width);
        QVERIFY(width->isEnabled());
        const qreal oldWidth = page->property("selectedShapeWidth").toReal();
        width->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_Up);
        QTRY_VERIFY(page->property("selectedShapeWidth").toReal() > oldWidth);
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!popup()->property("visible").toBool());
        page->undo();
        QVERIFY(page->selectObject(rectangleId));
        QCOMPARE(page->property("selectedShapeWidth").toReal(), oldWidth);
        page->undo();
        QCOMPARE(page->itemCount(), 0);
        page->redo();
        QCOMPARE(page->itemCount(), 1);
    }
    void stencilPaletteActivatesPlacementRatherThanInsertingAtDefaultPoint() {
        click("openStencilPalette");
        QTRY_VERIFY(popup()->property("opened").toBool());
        click("editorStencil_resistor-iec");
        QTRY_COMPARE(page->tool(), QString("symbol"));
        QCOMPARE(page->itemCount(), 0);
        QTRY_VERIFY(!popup()->property("visible").toBool());
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, pagePoint(600,800));
        QTRY_COMPARE(page->itemCount(), 1);
        click("openInkPalette");
        QTRY_VERIFY(popup()->property("opened").toBool());
        click("editorTool_select");
        QTRY_VERIFY(!popup()->property("visible").toBool());
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, pagePoint(600,800));
        QCOMPARE(page->property("selectionKind").toString(), QString("symbol"));
    }
    void secondFingerCancelsPreview() {
        page->setTool("arrow");
        auto device = QTest::createTouchDevice(QInputDevice::DeviceType::TouchScreen);
        const QPoint start = pagePoint(300,500), finish = pagePoint(700,800);
        QTest::touchEvent(window,device).press(0,start,window).commit();
        QTest::touchEvent(window,device).move(0,finish,window).commit();
        QCOMPARE(page->itemCount(),0);
        QTest::touchEvent(window,device).stationary(0).press(1,pagePoint(950,1000),window).commit();
        QTest::touchEvent(window,device).release(0,finish,window).release(1,pagePoint(950,1000),window).commit();
        QTest::qWait(30);
        QCOMPARE(page->itemCount(),0);
        QVERIFY(!page->canUndo());
    }
};
QTEST_MAIN(EditorWorkflowTest)
#include "EditorWorkflowTest.moc"
