#include "NativeAgendaText.h"
#include <QtTest>
#include <QTransform>
#include <functional>

namespace scene {
class ParagraphStyle {
    Q_GADGET
public:
    enum class Type { Paragraph = 1, Title = 2 };
    Q_ENUM(Type)
};
}

class Worker : public QObject {
    Q_OBJECT
    Q_PROPERTY(int jobQueueSize MEMBER pending)
public:
    int pending = 0;
};

class Controller : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject *worker READ worker)
    Q_PROPERTY(QString pageId MEMBER pageId)
    Q_PROPERTY(int currentLayer MEMBER layer)
    Q_PROPERTY(bool working MEMBER working)
    Q_PROPERTY(bool hasAnnotations MEMBER annotations)
    Q_PROPERTY(bool hasTextSelection MEMBER selection)
    Q_PROPERTY(int selectionItemCount MEMBER itemSelection)
    Q_PROPERTY(int textCursorIndex MEMBER cursor)
    Q_PROPERTY(QRectF paperPortraitBounds MEMBER paper)
    Q_PROPERTY(QRectF paperNoteBounds READ noteBounds)
    Q_PROPERTY(QTransform pendingEdit MEMBER transform)
    Q_PROPERTY(bool hasRootDocument MEMBER hasRoot)
    Q_PROPERTY(int rootDocumentLength READ length)
    Q_PROPERTY(double rootDocumentTextWidth READ width WRITE setWidth)
    Q_PROPERTY(QRectF rootDocumentBoundingRect READ bounds)
public:
    Worker jobs;
    QString pageId = "page-a", text;
    int layer = 0, insertions = 0, creations = 0, itemSelection = 0;
    int cursor = 0, anchor = 0;
    bool working = false, annotations = false, selection = false, hasRoot = false;
    bool publishUpdates = true, truncateInsert = false;
    QRectF paper{-702, 0, 1404, 1872};
    QTransform transform;
    double textWidth = 936;
    QObject *worker() { return &jobs; }
    int length() const { return text.size(); }
    double width() const { return textWidth; }
    QRectF noteBounds() const { return QRectF(-paper.height() / 2, 0, paper.height(), paper.width()); }
    QRectF bounds() const { return QRectF(-textWidth / 2, 234, textWidth, 180); }
    void enqueue(std::function<void()> operation) {
        ++jobs.pending;
        working = true;
        QTimer::singleShot(20, this, [this, operation] {
            operation();
            --jobs.pending;
            if (publishUpdates) emit updated(); // Controller clears working after the completion signal.
            QTimer::singleShot(0, this, [this] { working = jobs.pending != 0; });
        });
    }
    void setWidth(double value) { enqueue([this, value] { textWidth = value; }); }
    Q_INVOKABLE void createRootDocument(scene::ParagraphStyle::Type style) {
        ++creations;
        enqueue([this, style] { hasRoot = style == scene::ParagraphStyle::Type::Title; });
    }
    Q_INVOKABLE void focusRootDocument() { enqueue([] {}); }
    Q_INVOKABLE void selectTextRange(int start, int end) {
        enqueue([this, start, end] { anchor = start; cursor = end; selection = start != end; });
    }
    Q_INVOKABLE void clearSelectedText() {
        enqueue([this] { anchor = cursor; selection = false; });
    }
    Q_INVOKABLE void setCursorIndex(int index) {
        enqueue([this, index] { cursor = anchor = index; selection = false; });
    }
    Q_INVOKABLE void replaceText(QString value) {
        ++insertions;
        enqueue([this, value] {
            const int start = qMin(cursor, anchor);
            const auto replacement = truncateInsert ? value.left(5) : value;
            text.replace(start, qAbs(cursor - anchor), replacement);
            cursor = anchor = start + replacement.size();
            selection = false;
        });
    }
signals:
    void updated();
};

class SceneView : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject *controller READ controller)
    Q_PROPERTY(int status MEMBER status)
public:
    explicit SceneView(Controller *value) : scene(value) {}
    Controller *scene;
    bool partial = false;
    int status = 2;
    QObject *controller() { return scene; }
    Q_INVOKABLE QVariant inputMethodQuery(Qt::InputMethodQuery query, QVariant) {
        if (status != 2) return {};
        if (query == Qt::ImSurroundingText) return scene->text.section('\n', -1);
        if (query != Qt::ImCurrentSelection) return {};
        const auto selected = scene->text.mid(qMin(scene->anchor, scene->cursor), qAbs(scene->anchor - scene->cursor));
        return partial ? selected.left(3) : selected;
    }
};

namespace {
QVariantMap fields() {
    return {{"title", QStringLiteral("Présentation 3PSM")}, {"day", "Lundi"},
            {"date", "07 / 09 / 2026"}, {"time", QStringLiteral("09:00 – 10:30")}, {"headerPrepared", true}, {"headerPlanHash", QString(64, 'b')}};
}
QString expected() {
    return fields()["title"].toString();
}
QString legacyExpected() {
    const auto value = fields();
    return value["title"].toString() + '\n' + value["day"].toString() + QStringLiteral(" — ")
        + value["date"].toString() + QStringLiteral(" — ") + value["time"].toString();
}
QVariantMap context(SceneView *view = nullptr) {
    auto result = QVariantMap{{"documentId", "doc-a"}, {"pageId", "page-a"}, {"layer", 0}};
    if (view) result["sceneView"] = QVariant::fromValue(static_cast<QObject *>(view));
    return result;
}
}

class NativeAgendaTextTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { qRegisterMetaType<scene::ParagraphStyle::Type>(); }
    void writesOnlyAfterCommitAndWaitsForNativeCompletion() {
        Controller scene;
        SceneView view(&scene);
        NativeAgendaText page;
        QSignalSpy ready(&page, &NativeAgendaText::prepared), done(&page, &NativeAgendaText::finished);
        // Firmware 3.28 exposes note bounds in landscape even for a portrait page.
        QCOMPARE(scene.property("paperNoteBounds").toRectF(), QRectF(-936, 0, 1872, 1404));
        QVERIFY2(page.prepare("one", &scene, context(&view), fields(), {1620, 2160}), qPrintable(page.reason()));
        QCOMPARE(ready.size(), 1);
        QCOMPARE(ready[0][1].toMap()["state"].toString(), "empty");
        QCOMPARE(scene.creations, 0);
        QVERIFY(page.commit("one"));
        QCOMPARE(done.size(), 0);
        QCOMPARE(scene.insertions, 0);
        QTRY_COMPARE(done.size(), 1);
        QVERIFY2(done[0][1].toBool(), qPrintable(done[0][2].toMap()["code"].toString()));
        QVERIFY(!scene.selection);
        QCOMPARE(scene.cursor, int(expected().size()));
        QCOMPARE(scene.text, expected());
        QCOMPARE(scene.width(), 1032.0);
        QCOMPARE(scene.bounds().left() - scene.paper.left(), 186.0);
        QCOMPARE(done[0][2].toMap()["state"].toString(), "inserted");
    }
    void rejectsLandscapeAndUnsupportedController() {
        Controller scene;
        NativeAgendaText page;
        scene.paper = {-936, 0, 1872, 1404};
        QVERIFY(!page.prepare("one", &scene, context(), fields(), {1620, 2160}));
        QCOMPARE(scene.creations, 0);
        QObject unsupported;
        QVERIFY(!page.prepare("one", &unsupported, context(), fields(), {1620, 2160}));
    }
    void refusesWithoutPreparedTemplateHeader() {
        Controller scene;
        NativeAgendaText page;
        auto values = fields();
        values.remove("headerPrepared");
        QVERIFY(!page.prepare("one", &scene, context(), values, {1620, 2160}));
        QCOMPARE(page.reason(), "agenda-template-header-not-prepared");
        QCOMPARE(scene.creations, 0);
        QCOMPARE(scene.insertions, 0);
        QCOMPARE(scene.jobs.pending, 0);
        values = fields(); values.remove("headerPlanHash");
        QVERIFY(!page.prepare("one", &scene, context(), values, {1620, 2160}));
        QCOMPARE(scene.creations, 0); QCOMPARE(scene.insertions, 0);
    }
    void resolvesNativeSceneViewInsideDocumentView() {
        Controller scene;
        QObject documentView;
        SceneView view(&scene);
        view.setParent(&documentView);
        NativeAgendaText page;
        auto ctx = context();
        ctx["documentView"] = QVariant::fromValue(&documentView);
        scene.text = expected();
        scene.hasRoot = true;
        scene.textWidth = 1032;
        QSignalSpy ready(&page, &NativeAgendaText::prepared);
        QVERIFY(page.prepare("one", &scene, ctx, fields(), {1620, 2160}));
        QTRY_COMPARE(ready.size(), 1);
        QCOMPARE(ready[0][1].toMap()["state"].toString(), "already-present");
        page.cancel("one");
    }
    void pageChangeBeforeCommitAndDuringCreationStopsFurtherWrites() {
        for (bool during : {false, true}) {
            Controller scene;
            NativeAgendaText page;
            QSignalSpy done(&page, &NativeAgendaText::finished);
            QVERIFY(page.prepare("one", &scene, context(), fields(), {1620, 2160}));
            if (during) QVERIFY(page.commit("one"));
            scene.pageId = "page-b";
            if (during) {
                QTRY_COMPARE(done.size(), 1);
                QVERIFY(!done[0][1].toBool());
            } else QVERIFY(!page.commit("one"));
            QCOMPARE(scene.insertions, 0);
        }
    }
    void exactExistingTextDoesNotInsertAgain() {
        Controller scene;
        SceneView view(&scene);
        NativeAgendaText page;
        scene.text = expected();
        scene.hasRoot = true;
        scene.textWidth = 1032;
        QSignalSpy ready(&page, &NativeAgendaText::prepared), done(&page, &NativeAgendaText::finished);
        QVERIFY(page.prepare("one", &scene, context(&view), fields(), {1620, 2160}));
        QTRY_COMPARE(ready.size(), 1);
        QCOMPARE(ready[0][1].toMap()["state"].toString(), "already-present");
        QVERIFY(page.commit("one"));
        QTRY_COMPARE(done.size(), 1);
        QVERIFY(done[0][1].toBool());
        QCOMPARE(scene.creations, 0);
        QCOMPARE(scene.insertions, 0);
        QVERIFY(!scene.selection);
    }
    void exactLegacyTextMigratesOnlyAfterCommitAndPreservesHandwriting() {
        Controller scene;
        SceneView view(&scene);
        NativeAgendaText page;
        scene.text = legacyExpected();
        scene.hasRoot = true;
        scene.textWidth = 1032;
        scene.annotations = true;
        scene.cursor = scene.anchor = 3;
        QSignalSpy ready(&page, &NativeAgendaText::prepared), done(&page, &NativeAgendaText::finished);
        QVERIFY(page.prepare("one", &scene, context(&view), fields(), {1620, 2160}));
        QTRY_COMPARE(ready.size(), 1);
        QCOMPARE(scene.text, legacyExpected());
        QCOMPARE(scene.insertions, 0);
        QCOMPARE(ready[0][1].toMap()["format"].toString(), "native-text-P-Day-header-v3");
        QVERIFY(page.commit("one"));
        QTRY_COMPARE(done.size(), 1);
        QVERIFY(done[0][1].toBool());
        QCOMPARE(scene.text, expected());
        QCOMPARE(scene.insertions, 1);
        QCOMPARE(scene.creations, 0);
        QVERIFY(scene.annotations);
        QVERIFY(!scene.selection);
        QCOMPARE(scene.cursor, 3);
        QVERIFY(done[0][2].toMap()["migratedPreviousText"].toBool());
        QVERIFY(page.prepare("two", &scene, context(&view), fields(), {1620, 2160}));
        QTRY_COMPARE(ready.size(), 2);
        QVERIFY(page.commit("two"));
        QTRY_COMPARE(done.size(), 2);
        QVERIFY(done[1][1].toBool());
        QCOMPARE(scene.insertions, 1);
        QVERIFY(!done[1][2].toMap()["migratedPreviousText"].toBool());
    }
    void changedLegacyTextIsNeverReplaced() {
        for (const bool changeAfterPrepare : {false, true}) {
            Controller scene;
            SceneView view(&scene);
            NativeAgendaText page;
            scene.text = legacyExpected();
            scene.hasRoot = true;
            scene.textWidth = 1032;
            scene.annotations = true;
            if (!changeAfterPrepare) scene.text[0] = 'X';
            QSignalSpy ready(&page, &NativeAgendaText::prepared), done(&page, &NativeAgendaText::finished);
            QVERIFY(page.prepare("one", &scene, context(&view), fields(), {1620, 2160}));
            if (changeAfterPrepare) {
                QTRY_COMPARE(ready.size(), 1);
                scene.text[0] = 'X';
                QVERIFY(page.commit("one"));
            }
            const auto changed = scene.text;
            QTRY_COMPARE(done.size(), 1);
            QVERIFY(!done[0][1].toBool());
            QCOMPARE(scene.text, changed);
            QCOMPARE(scene.insertions, 0);
            QVERIFY(scene.annotations);
            QVERIFY(!scene.selection);
        }
    }
    void matchingLengthOrPartialReaderNeverConfirmsExistingText() {
        Controller scene;
        SceneView view(&scene);
        NativeAgendaText page;
        scene.hasRoot = true;
        scene.textWidth = 1032;
        scene.text = QString(expected().size(), 'x');
        QSignalSpy done(&page, &NativeAgendaText::finished);
        QVERIFY(page.prepare("one", &scene, context(&view), fields(), {1620, 2160}));
        QTRY_COMPARE(done.size(), 1);
        QVERIFY(!done[0][1].toBool());
        QVERIFY(!scene.selection);
        scene.text = expected();
        view.partial = true;
        QVERIFY(page.prepare("two", &scene, context(&view), fields(), {1620, 2160}));
        QTRY_VERIFY(scene.selection && !scene.working);
        for (int i = 0; i <= 320 && done.size() == 1; ++i)
            QVERIFY(QMetaObject::invokeMethod(&page, "advance", Qt::DirectConnection));
        QTRY_COMPARE(done.size(), 2);
        QVERIFY(!done[1][1].toBool());
        QTRY_VERIFY(!scene.selection && !scene.working);
        QVERIFY(!page.prepare("one", &scene, context(), fields(), {1620, 2160}));
        QCOMPARE(scene.insertions, 0);
    }
    void existingTextChangedAfterPrepareIsRejected() {
        Controller scene;
        SceneView view(&scene);
        NativeAgendaText page;
        scene.hasRoot = true;
        scene.textWidth = 1032;
        scene.text = expected();
        QSignalSpy ready(&page, &NativeAgendaText::prepared), done(&page, &NativeAgendaText::finished);
        QVERIFY(page.prepare("one", &scene, context(&view), fields(), {1620, 2160}));
        QTRY_COMPARE(ready.size(), 1);
        scene.text[0] = 'X';
        QVERIFY(page.commit("one"));
        QTRY_COMPARE(done.size(), 1);
        QVERIFY(!done[0][1].toBool());
        QVERIFY(!scene.selection);
        QCOMPARE(scene.insertions, 0);
    }
    void partialInsertionIsReportedAndNeverAppendedOnRetry() {
        Controller scene;
        SceneView view(&scene);
        NativeAgendaText page;
        scene.truncateInsert = true;
        QSignalSpy done(&page, &NativeAgendaText::finished);
        QVERIFY(page.prepare("one", &scene, context(&view), fields(), {1620, 2160}));
        QVERIFY(page.commit("one"));
        QTRY_COMPARE(scene.text.size(), 5);
        QTRY_VERIFY(!scene.working);
        for (int i = 0; i <= 320 && done.isEmpty(); ++i)
            QVERIFY(QMetaObject::invokeMethod(&page, "advance", Qt::DirectConnection));
        QTRY_COMPARE(done.size(), 1);
        QVERIFY(!done[0][1].toBool());
        QCOMPARE(scene.text.size(), 5);
        QVERIFY(!page.prepare("two", &scene, context(&view), fields(), {1620, 2160}));
        QCOMPARE(scene.insertions, 1);
    }
    void cancellationAfterCreationDoesNotSubmitText() {
        Controller scene;
        NativeAgendaText page;
        QSignalSpy done(&page, &NativeAgendaText::finished);
        QVERIFY(page.prepare("one", &scene, context(), fields(), {1620, 2160}));
        QVERIFY(page.commit("one"));
        page.cancel("one");
        QTest::qWait(100);
        QCOMPARE(scene.insertions, 0);
        QCOMPARE(done.size(), 0);
        QVERIFY(!page.busy());
    }
    void cancellationDuringExistingReadClearsSelectionAndRestoresCursor() {
        Controller scene;
        SceneView view(&scene);
        NativeAgendaText page;
        scene.hasRoot = true;
        scene.text = expected();
        scene.textWidth = 1032;
        scene.cursor = scene.anchor = 3;
        view.status = 1;
        QSignalSpy ready(&page, &NativeAgendaText::prepared), done(&page, &NativeAgendaText::finished);
        QVERIFY(page.prepare("one", &scene, context(&view), fields(), {1620, 2160}));
        QTRY_VERIFY(scene.selection && !scene.working);
        page.cancel("one");
        QTRY_VERIFY(!scene.selection && !scene.working);
        QCOMPARE(scene.cursor, 3);
        QCOMPARE(scene.text, expected());
        QCOMPARE(scene.insertions, 0);
        QCOMPARE(scene.creations, 0);
        QCOMPARE(ready.size(), 0);
        QCOMPARE(done.size(), 0);
    }
    void existingUserSelectionIsPreservedAndReaderDestructionIsHandled() {
        Controller scene;
        auto *view = new SceneView(&scene);
        NativeAgendaText page;
        scene.hasRoot = true;
        scene.text = expected();
        scene.textWidth = 1032;
        scene.selection = true;
        scene.anchor = 2;
        scene.cursor = 7;
        QVERIFY(!page.prepare("one", &scene, context(view), fields(), {1620, 2160}));
        QVERIFY(scene.selection);
        QCOMPARE(scene.anchor, 2);
        QCOMPARE(scene.cursor, 7);
        QCOMPARE(scene.jobs.pending, 0);
        scene.selection = false;
        view->status = 1;
        QSignalSpy done(&page, &NativeAgendaText::finished);
        QVERIFY(page.prepare("two", &scene, context(view), fields(), {1620, 2160}));
        QTRY_VERIFY(scene.selection && !scene.working);
        delete view;
        QTRY_COMPARE(done.size(), 1);
        QVERIFY(!done[0][1].toBool());
        QTRY_VERIFY(!scene.selection && !scene.working);
        QCOMPARE(scene.cursor, 7);
        QCOMPARE(scene.text, expected());
        QCOMPARE(scene.insertions, 0);
    }
    void propertyPostconditionsConfirmCompletionWithoutUpdatedSignal() {
        Controller scene;
        NativeAgendaText page;
        scene.publishUpdates = false;
        QSignalSpy done(&page, &NativeAgendaText::finished);
        QVERIFY(page.prepare("one", &scene, context(), fields(), {1620, 2160}));
        QVERIFY(page.commit("one"));
        QTRY_COMPARE(done.size(), 1);
        QVERIFY(scene.hasRoot);
        QVERIFY(!scene.working);
        QCOMPARE(scene.insertions, 1);
        QCOMPARE(scene.text, expected());
        QVERIFY(done[0][1].toBool());
    }
    void rootPostconditionStillWaitsForNativeWorkerQueue() {
        Controller scene;
        NativeAgendaText page;
        QVERIFY(page.prepare("one", &scene, context(), fields(), {1620, 2160}));
        QVERIFY(page.commit("one"));
        scene.hasRoot = true;
        QVERIFY(QMetaObject::invokeMethod(&page, "advance", Qt::DirectConnection));
        QCOMPARE(scene.insertions, 0);
        QTRY_COMPARE(scene.insertions, 1);
        page.cancel("one");
    }
    void waitsForSceneViewToBecomeActiveAfterWorkerFinishes() {
        Controller scene;
        SceneView view(&scene);
        NativeAgendaText page;
        QSignalSpy done(&page, &NativeAgendaText::finished);
        QVERIFY(page.prepare("one", &scene, context(&view), fields(), {1620, 2160}));
        view.status = 1;
        QVERIFY(page.commit("one"));
        QTRY_COMPARE(scene.text, expected());
        QTRY_VERIFY(!scene.working);
        QTest::qWait(75);
        QCOMPARE(done.size(), 0);
        view.status = 2;
        QTRY_COMPARE(done.size(), 1);
        QVERIFY(done[0][1].toBool());
    }
};

QTEST_GUILESS_MAIN(NativeAgendaTextTest)
#include "NativeAgendaTextTest.moc"
