#include "EditorAdapter.h"
#include "TargetProfile.h"
#include "NativeDiagnostics.h"
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QtTest>

class PretendSceneController:public QObject {
    Q_OBJECT
public:
    int calls=0;
    Q_INVOKABLE void addDrawingLine(QVariant){++calls;}
    Q_INVOKABLE void applyPendingEdit(int){++calls;}
    Q_INVOKABLE void deleteSelection(){++calls;}
    Q_INVOKABLE void undo(){++calls;}
};
class NativeGateTest:public QObject {
    Q_OBJECT
private slots:
    void diagnosticFingerprintIsUnambiguousAndContainsNoRawIds() {
        using RePaperNative::contextFingerprint;
        const auto value=contextFingerprint("test-document","test-page");
        QCOMPARE(value.size(),64);
        QCOMPARE(value,contextFingerprint("test-document","test-page"));
        QVERIFY(value!=contextFingerprint("test-page","test-document"));
        QVERIFY(contextFingerprint("a,b","c")!=contextFingerprint("a","b,c"));
        QVERIFY(!value.contains("test-document"));
        QVERIFY(contextFingerprint("","page").isEmpty());
        QVERIFY(contextFingerprint("document","").isEmpty());
    }
    void attachedUnverifiedHostCannotWriteOrRetainDestroyedPage() {
        EditorAdapter adapter;PretendSceneController controller;
        auto view=new QObject;auto host=new QObject;
        QVERIFY(!adapter.attachNativePage(&controller,view,host));
        adapter.refreshNativeState();
        QVERIFY(!adapter.available());QVERIFY(!adapter.captureEnabled());
        adapter.setNativeToolActive(true);
        // The input surface blocks the underlying native pen even when this
        // host is unsupported; it still cannot start or dispatch a custom edit.
        QVERIFY(adapter.captureEnabled());
        QVERIFY(!adapter.pointerBegin(50,60,"pen"));
        QVERIFY(!adapter.pointerMove(80,90));QVERIFY(!adapter.pointerEnd(80,90));
        QVERIFY(!adapter.beginStencil("resistor-iec"));QVERIFY(!adapter.undo());
        QVERIFY(!adapter.duplicate());QVERIFY(!adapter.remove());
        delete host;delete view;
        QVERIFY(!adapter.available());QVERIFY(!adapter.pointerEnd(80,90));
        QCOMPARE(controller.calls,0);
    }
    void metadataCannotEnableNativeMutations() {
        EditorAdapter adapter;PretendSceneController controller;
        QVERIFY(adapter.inspectNativeObject(&controller));
        QVERIFY(adapter.evidence()["methods"].toStringList().contains("addDrawingLine(QVariant)"));
        QVERIFY(!adapter.available());QVERIFY(!adapter.attachPcPage(&controller));
        adapter.setNativeToolActive(true);
        QVERIFY(!adapter.captureEnabled());
        QVERIFY(!adapter.state().contains("nativeCreationArmed"));
        QVERIFY(!adapter.state().value("nativePersistentEditingAvailable").toBool());
        QVERIFY(!adapter.insertStencil("resistor-iec"));QVERIFY(!adapter.chooseTool("arrow"));
        QVERIFY(!adapter.setStrokeStyle("dotted"));QVERIFY(!adapter.setStrokeWidth(8));
        QVERIFY(!adapter.setArrowDirection("both"));QVERIFY(!adapter.setWireOrientation(false));
        QVERIFY(!adapter.undo());QVERIFY(!adapter.redo());QVERIFY(!adapter.remove());
        QVERIFY(!adapter.rotate());QVERIFY(!adapter.scale(1.25));QVERIFY(!adapter.duplicate());
        QCOMPARE(controller.calls,0);
        QVERIFY(!RePaperNative::matchesRunningXochitl());
    }
    void sidebarLoadsWithDisabledNativeActions() {
        Q_INIT_RESOURCE(editor_panels);
        QQuickStyle::setStyle("Basic");QQmlEngine engine;
        QQmlComponent component(&engine,QUrl("qrc:/repaper/editor/ReInkSidebar.qml"));
        EditorAdapter adapter;
        QScopedPointer<QObject> sidebar(component.createWithInitialProperties({{"editor",QVariant::fromValue<QObject*>(&adapter)}}));
        QVERIFY2(sidebar,qPrintable(component.errorString()));
        for(const auto name:{"editorStrokeStyle","selectionDirection"}) {
            auto control=sidebar->findChild<QQuickItem*>(name);QVERIFY(control);QVERIFY(!control->isEnabled());
        }
        sidebar->setProperty("mode","restencil");
        auto search=sidebar->findChild<QQuickItem*>("editorStencilSearch");
        QVERIFY(search);QVERIFY(search->isEnabled()); // local catalogue browsing is read-only
    }
};
QTEST_MAIN(NativeGateTest)
#include "NativeGateTest.moc"
