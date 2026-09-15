#include <QQmlError>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickView>
#include <QtTest>
#include <memory>

// Built by tools/validate-native-toolbar-protocol.py with user-supplied,
// locally validated firmware sources. No firmware source is redistributed.
class NativeToolbarProtocolTest : public QObject {
    Q_OBJECT
    std::unique_ptr<QQuickView> view;
    QQuickItem *toolbar = nullptr;

    QObject *tool(const char *name) const {
        return toolbar->findChild<QObject *>(QString::fromLatin1(name));
    }
    QObject *selected(const char *property) const {
        return toolbar->property(property).value<QObject *>();
    }
    int highlightedPens() const {
        int count = 0;
        for (const auto name : {"brush", "RePaperInkTool", "RePaperStencilTool", "RePaperSelectTool", "lasso"})
            if(tool(name))count += tool(name)->property("highlighted").toBool();
        return count;
    }
    void load(const char *version) {
        view = std::make_unique<QQuickView>();
        view->setSource(QUrl::fromLocalFile(QStringLiteral(HARNESS_DIR)
                                          + "/" + version + "/Toolbar.qml"));
        QStringList errors;
        for (const auto &error : view->errors()) errors.append(error.toString());
        QVERIFY2(view->status() == QQuickView::Ready, qPrintable(errors.join('\n')));
        toolbar = view->rootObject();
        QVERIFY(toolbar);
        view->show();
        QVERIFY(QTest::qWaitForWindowExposed(view.get()));
    }
    void click(const char *name) {
        auto item = qobject_cast<QQuickItem *>(tool(name));
        QVERIFY(item);
        QVERIFY(item->isVisible());
        QTest::mouseClick(view.get(), Qt::LeftButton, Qt::NoModifier,
                         item->mapToScene(QPointF(item->width()/2, item->height()/2)).toPoint());
    }
    void expectPen(const char *name, bool custom) {
        QCOMPARE(selected("selectedPen"), tool(name));
        QCOMPARE(selected("selectedTool"), tool(name));
        QCOMPARE(highlightedPens(), 1);
        QCOMPARE(toolbar->property("repaperToolActive").toBool(), custom);
    }
private slots:
    void cleanup() { view.reset(); toolbar = nullptr; }

    void legacyGenericToolReproducesDoubleSelection() {
        load("legacy");
        QVERIFY(toolbar);
        click("brush");
        QCOMPARE(highlightedPens(), 1);
        click("RePaperInkTool");
        QCOMPARE(selected("selectedTool"), tool("RePaperInkTool"));
        QCOMPARE(selected("selectedPen"), tool("brush"));
        QCOMPARE(highlightedPens(), 2); // The previously observed defect.
    }

    void brushInkStencilLassoAreExclusive() {
        load("current");
        QVERIFY(toolbar);
        click("brush"); expectPen("brush", false);
        click("RePaperInkTool"); expectPen("RePaperInkTool", true);
        QVERIFY(tool("RePaperInkTool")->property("foldoutVisible").toBool());
        click("RePaperStencilTool"); expectPen("RePaperStencilTool", true);
        QVERIFY(!tool("RePaperInkTool")->property("foldoutVisible").toBool());
        QVERIFY(tool("RePaperStencilTool")->property("foldoutVisible").toBool());
        click("lasso"); expectPen("lasso", false);
        QVERIFY(!tool("RePaperStencilTool")->property("foldoutVisible").toBool());
        click("brush"); expectPen("brush", false);
    }

    void layersPreservePenAndReturnToIt() {
        load("current");
        QVERIFY(toolbar);
        for (const auto name : {"brush", "RePaperInkTool", "RePaperStencilTool", "RePaperSelectTool", "lasso"}) {
            click(name);
            const bool custom = QByteArray(name).startsWith("RePaper");
            click("layers");
            QCOMPARE(selected("selectedPen"), tool(name));
            QCOMPARE(selected("selectedTool"), tool("layers"));
            QCOMPARE(highlightedPens(), 1);
            QCOMPARE(toolbar->property("repaperToolActive").toBool(), custom);
            QVERIFY(tool("layers")->property("foldoutVisible").toBool());
            click("layers");
            expectPen(name, custom);
            QVERIFY(!tool("layers")->property("foldoutVisible").toBool());
        }
    }

    void selectingNativeLassoFromPaletteChangesPenBeforeClosingFoldout() {
        load("current");QVERIFY(toolbar);
        click("RePaperInkTool");expectPen("RePaperInkTool",true);
        QVERIFY(QMetaObject::invokeMethod(toolbar,"selectSelection"));
        expectPen("lasso",false);
        QCOMPARE(toolbar->property("nativeActiveTool").toString(),QString("selection"));
        QVERIFY(QMetaObject::invokeMethod(toolbar,"closeFoldout"));
        expectPen("lasso",false);
    }

    void choosingPaletteToolClosesFoldoutWithoutLosingPen() {
        load("current");
        QVERIFY(toolbar);
        for (const auto name : {"RePaperInkTool", "RePaperStencilTool"}) {
            click(name);
            QVERIFY(tool(name)->property("foldoutVisible").toBool());
            auto loader = tool(name)->property("foldoutContent").value<QObject *>();
            QVERIFY(loader);
            QTRY_VERIFY(loader->property("item").value<QObject *>());
            QVERIFY(QMetaObject::invokeMethod(loader->property("item").value<QObject *>(), "toolChosen"));
            QVERIFY(!tool(name)->property("foldoutVisible").toBool());
            expectPen(name, true);
            click(name);
            QVERIFY(tool(name)->property("foldoutVisible").toBool());
            click(name);
            QVERIFY(!tool(name)->property("foldoutVisible").toBool());
            expectPen(name, true);
        }
    }

    void directSelectActivatesOurToolWithoutOpeningAnyMenu() {
        load("current");QVERIFY(toolbar);
        const auto host=tool("editorHostFixture");QVERIFY(host);
        const auto editor=host->property("editor").value<QObject*>();QVERIFY(editor);
        for(const auto previous:{"brush","RePaperInkTool","RePaperStencilTool","lasso"}){
            click(previous);host->setProperty("inspectorOpen",true);
            editor->setProperty("tool","rectangle");const int activations=host->property("selectionActivations").toInt();
            click("RePaperSelectTool");expectPen("RePaperSelectTool",true);
            QCOMPARE(toolbar->property("nativeActiveTool").toString(),QString("primary"));
            QCOMPARE(editor->property("tool").toString(),QString("select"));
            QCOMPARE(host->property("selectionActivations").toInt(),activations+1);
            QVERIFY(!host->property("inspectorOpen").toBool());
            QVERIFY(!tool("RePaperSelectTool")->property("hasFoldout").toBool());
            QVERIFY(!tool("RePaperInkTool")->property("foldoutVisible").toBool());
            QVERIFY(!tool("RePaperStencilTool")->property("foldoutVisible").toBool());
            click("RePaperSelectTool");expectPen("RePaperSelectTool",true);
            QCOMPARE(host->property("selectionActivations").toInt(),activations+2);
            QVERIFY(!tool("RePaperSelectTool")->property("foldoutVisible").toBool());
        }
        editor->setProperty("available",false);QVERIFY(!tool("RePaperSelectTool")->property("enabled").toBool());
    }

    void customSelectionGuard_data() {
        QTest::addColumn<bool>("available");
        QTest::addColumn<bool>("handled");
        QTest::newRow("custom-selection") << true << true;
        QTest::newRow("native-fallback") << true << false;
        QTest::newRow("host-unloaded") << false << false;
    }

    void customSelectionGuard() {
        QFETCH(bool, available);
        QFETCH(bool, handled);
        view = std::make_unique<QQuickView>();
        view->rootContext()->setContextProperty("Experimental", QVariantMap{
            {"glyphSelection", QVariantMap{{"enabled", false}}}});
        view->rootContext()->setContextProperty("sceneViewLog", QString("private-selection-protocol"));
        view->setSource(QUrl::fromLocalFile(QStringLiteral(HARNESS_DIR) + "/SelectionGuard.qml"));
        QVERIFY(view->status() == QQuickView::Ready);
        toolbar = view->rootObject();
        QVERIFY(toolbar);
        toolbar->setProperty("customHostAvailable", available);
        toolbar->setProperty("customHandlesSelection", handled);
        const QVariant layer = 7;
        const QVariant rect = QRectF(10, 20, 200, 100);
        QVERIFY(QMetaObject::invokeMethod(toolbar, "onAreaSelected", Q_ARG(QVariant, layer), Q_ARG(QVariant, rect)));
        QCOMPARE(toolbar->property("cleanupCount").toInt(), 1);
        QCOMPARE(toolbar->property("repaintCount").toInt(), 1);
        QCOMPARE(toolbar->property("hostCallCount").toInt(), available ? 1 : 0);
        QCOMPARE(toolbar->property("clearCount").toInt(), handled ? 0 : 1);
        QCOMPARE(toolbar->property("endCount").toInt(), handled ? 0 : 1);
        if (available) {
            QCOMPARE(toolbar->property("receivedLayer").toInt(), layer.toInt());
            QCOMPARE(toolbar->property("receivedRect").toRectF(), rect.toRectF());
        }
        QCOMPARE(toolbar->property("events").toString(), QString("clean;paint;")
                 + (available ? "host;" : "") + (handled ? "" : "end;clear;"));
    }

    void nativeHistoryGuard_data() {
        QTest::addColumn<bool>("hostAvailable");
        QTest::addColumn<bool>("pending");
        QTest::newRow("native-operation-pending") << true << true;
        QTest::newRow("native-operation-idle") << true << false;
        QTest::newRow("host-unloaded-pending-state") << false << true;
        QTest::newRow("host-unloaded-idle") << false << false;
    }

    void nativeHistoryGuard() {
        QFETCH(bool, hostAvailable);
        QFETCH(bool, pending);
        view = std::make_unique<QQuickView>();
        view->setSource(QUrl::fromLocalFile(QStringLiteral(HARNESS_DIR) + "/HistoryGuard.qml"));
        QStringList errors;
        for (const auto &error : view->errors()) errors.append(error.toString());
        QVERIFY2(view->status() == QQuickView::Ready, qPrintable(errors.join('\n')));
        toolbar = view->rootObject();
        QVERIFY(toolbar);
        toolbar->setProperty("customHostAvailable", hostAvailable);
        toolbar->setProperty("customPending", pending);
        const bool blocked = hostAvailable && pending;
        QCOMPARE(toolbar->property("repaperNativeOperationPending").toBool(), blocked);
        QCOMPARE(toolbar->property("sceneOperationPending").toBool(), blocked);
        for (const auto *name : {"undoAvailable", "redoAvailable", "toolbarUndoEnabled", "toolbarRedoEnabled",
                                 "keyboardUndoEnabled", "keyboardRedoEnabled", "gestureUndoEnabled", "gestureRedoEnabled"})
            QCOMPARE(toolbar->property(name).toBool(), !blocked);
        // Direct toolbar signals and keyboard dispatch must remain guarded
        // even if they bypass the disabled visual buttons.
        for (const auto *method : {"invokeToolbarUndo", "invokeToolbarRedo", "shortcutUndo", "shortcutRedo",
                                   "conversionFailed", "closeAndUndo"})
            QVERIFY(QMetaObject::invokeMethod(toolbar, method));
        QCOMPARE(toolbar->property("undoCalls").toInt(), blocked ? 0 : 4);
        QCOMPARE(toolbar->property("redoCalls").toInt(), blocked ? 0 : 2);
        QCOMPARE(toolbar->property("exitTextCalls").toInt(), blocked ? 0 : 2);
        QCOMPARE(toolbar->property("closeCalls").toInt(), blocked ? 0 : 1);
        QCOMPARE(toolbar->property("newPageVisible").toBool(), blocked);
        if (blocked) {
            toolbar->setProperty("customPending", false);
            QVERIFY(QMetaObject::invokeMethod(toolbar, "invokeToolbarUndo"));
            QCOMPARE(toolbar->property("undoCalls").toInt(), 1);
            QVERIFY(toolbar->property("gestureUndoEnabled").toBool());
            QVERIFY(toolbar->property("keyboardRedoEnabled").toBool());
        }
        // Releasing our lock preserves native history availability.
        toolbar->setProperty("customPending", false);
        toolbar->setProperty("nativeUndoAvailable", false);
        toolbar->setProperty("nativeRedoAvailable", false);
        for (const auto *name : {"undoAvailable", "redoAvailable", "toolbarUndoEnabled", "toolbarRedoEnabled",
                                 "keyboardUndoEnabled", "keyboardRedoEnabled", "gestureUndoEnabled", "gestureRedoEnabled"})
            QVERIFY(!toolbar->property(name).toBool());
    }
};

QTEST_MAIN(NativeToolbarProtocolTest)
#include "NativeToolbarProtocolTest.moc"
