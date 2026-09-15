// Include the implementation to exercise private lifetime/receipt state without
// enabling the native ABI or injecting any mutation backend into production.
#include "NativeSelectionColor.cpp"
#include <QtTest>
#include <stdexcept>

class DocumentWorker : public QObject {
    Q_OBJECT
signals:
    void jobCompleted(qulonglong jobId);
};
class SceneController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject* worker READ worker)
    Q_PROPERTY(QString pageId READ pageId)
public:
    mutable int reads = 0;
    QObject *workerObject = nullptr;
    QString page = QStringLiteral("page-a");
    QObject *worker() const { ++reads; return workerObject; }
    QString pageId() const { ++reads; return page; }
};

namespace {
using namespace RePaperNative::ColorDetail;
struct FakeNativeScene {
    struct State {
        bool exists = true;
        QColor color = Qt::black;
        bool operator==(const State &other) const { return exists == other.exists && color == other.color; }
    };
    struct Command { State before, after; };
    State current;
    QVector<Command> history{{State{}, State{}}}, redo;
    bool cancelled = false, rejectDelete = false, rejectInsert = false;
    bool cancelAfterDelete = false, verify = true, omitDeleteHistory = false;
    bool omitInsertHistory = false, throwDuringInsert = false, throwDuringVerify = false;
    bool badGrouping = false;
    int deletes = 0, inserts = 0, undos = 0, groups = 0;
    void undo() {
        ++undos;
        const auto command = history.takeLast();
        current = command.before;
        redo.append(command);
    }
    void redoOnce() {
        const auto command = redo.takeLast();
        current = command.after;
        history.append(command);
    }
    RecordedEditOperations operations() {
        return {
            [this] { return cancelled; },
            [this] { return history.size(); },
            [this] {
                ++deletes;
                if (rejectDelete) return false;
                const auto before = current;
                current.exists = false;
                if (!omitDeleteHistory) history.append({before, current});
                if (cancelAfterDelete) cancelled = true;
                return true;
            },
            [this] {
                ++inserts;
                if (rejectInsert) return false;
                const auto before = current;
                current = {true, QColor(Qt::red)};
                if (throwDuringInsert) throw std::runtime_error("native partial allocation failure");
                if (!omitInsertHistory) history.append({before, current});
                return true;
            },
            [this] {
                if (throwDuringVerify) throw std::runtime_error("observation allocation failure");
                return verify;
            },
            [this](int count) {
                ++groups;
                if (count != 2) throw std::logic_error("must group only the pair");
                if (badGrouping) return;
                const auto insertion = history.takeLast();
                const auto deletion = history.takeLast();
                history.append({deletion.before, insertion.after});
            },
            [this] { undo(); }
        };
    }
};
}

class NativeSelectionColorTest : public QObject {
    Q_OBJECT
    static void installPending(NativeSelectionColor &editor, SceneController &controller,
                               DocumentWorker &worker, qulonglong jobId = 42) {
        controller.workerObject = &worker;
        editor.d->controller = &controller;
        editor.d->worker = &worker;
        editor.d->pageId = controller.page;
        editor.d->jobId = jobId;
        editor.d->busy = true;
        editor.d->pending = std::make_shared<NativeSelectionColor::Private::Pending>();
        editor.d->elapsed.start();
        editor.d->completionConnection = QObject::connect(&worker, &DocumentWorker::jobCompleted,
            &editor, &NativeSelectionColor::jobCompleted, Qt::QueuedConnection);
        editor.d->controllerDestroyed = QObject::connect(&controller, &QObject::destroyed,
            &editor, &NativeSelectionColor::cancel);
        editor.d->workerDestroyed = QObject::connect(&worker, &QObject::destroyed,
            &editor, &NativeSelectionColor::cancel);
    }
private slots:
    void toolsSilentlyRefusedByNativeAppendAreRejectedBeforeEditing() {
        for (const int tool : {8, 9, 10, 11, 22}) QVERIFY(!nativeInsertAcceptsTool(tool));
        for (const int tool : {0, 2, 6, 7, 12, 14, 19, 21, 23}) QVERIFY(nativeInsertAcceptsTool(tool));
    }
    void privateCloneLineagesFollowExactSourceBytesInCloneOrder() {
        const QByteArray a(32, 'a'), b(32, 'b'), c(32, 'c');
        QCOMPARE(matchedCloneLineages({a, b, c}, {101, 202, 303}, {c, a, b}),
                 (QVector<quint64>{303, 101, 202}));
        // Identical overlapping ink still consumes one source entry per clone;
        // legacy native fragments may legitimately share one relocation root.
        QCOMPARE(matchedCloneLineages({a, a, b}, {101, 202, 303}, {a, b, a}),
                 (QVector<quint64>{101, 303, 202}));
        QCOMPARE(matchedCloneLineages({a, b}, {101, 101}, {b, a}),
                 (QVector<quint64>{101, 101}));
    }
    void missingOrChangedSourceLineageRefusesBeforeNativeEditing() {
        const QByteArray a(32, 'a'), b(32, 'b'), c(32, 'c');
        QVERIFY(matchedCloneLineages({}, {}, {}).isEmpty());
        QVERIFY(matchedCloneLineages({a, b}, {101}, {a, b}).isEmpty());
        QVERIFY(matchedCloneLineages({a, b}, {101, 202}, {a}).isEmpty());
        QVERIFY(matchedCloneLineages({a, b}, {101, 202}, {a, a}).isEmpty());
        QVERIFY(matchedCloneLineages({a, b}, {101, 202}, {a, c}).isEmpty());
        QVERIFY(matchedCloneLineages({a}, {0}, {a}).isEmpty());
        QVERIFY(matchedCloneLineages({a}, {0xbeef000000000000ULL}, {a}).isEmpty());
        QVERIFY(matchedCloneLineages({QByteArray("invalid")}, {101}, {QByteArray("invalid")}).isEmpty());
        QVERIFY(matchedCloneLineages(QVector<QByteArray>(129, a), QVector<quint64>(129, 101),
                                     QVector<QByteArray>(129, a)).isEmpty());
    }
    void oneSuccessfulPairIsOneUndoAndRedo() {
        FakeNativeScene scene;
        const auto result = runRecordedEdit(scene.operations());
        QVERIFY(result.committed); QVERIFY(result.touchedScene); QVERIFY(!result.compensated);
        QCOMPARE(scene.history.size(), qsizetype(2)); // previous command + our single macro
        QCOMPARE(scene.groups, 1); QCOMPARE(scene.current.color, QColor(Qt::red));
        scene.undo();
        QVERIFY(scene.current.exists); QCOMPARE(scene.current.color, QColor(Qt::black));
        QCOMPARE(scene.history.size(), qsizetype(1));
        scene.redoOnce();
        QVERIFY(scene.current.exists); QCOMPARE(scene.current.color, QColor(Qt::red));
    }
    void cancellationBeforeEditRefusesButAfterDeletionFinishesPair() {
        FakeNativeScene before; before.cancelled = true;
        const auto refused = runRecordedEdit(before.operations());
        QVERIFY(!refused.committed); QVERIFY(!refused.touchedScene);
        QCOMPARE(before.deletes, 0); QCOMPARE(before.inserts, 0); QCOMPARE(before.history.size(), qsizetype(1));
        FakeNativeScene during; during.cancelAfterDelete = true;
        const auto completed = runRecordedEdit(during.operations());
        QVERIFY(completed.committed); QCOMPARE(during.inserts, 1); QCOMPARE(during.groups, 1);
        QVERIFY(during.current.exists); QCOMPARE(during.current.color, QColor(Qt::red));
    }
    void ordinaryInsertRefusalCompensatesOnlyTheRecordedDeletion() {
        FakeNativeScene scene; scene.rejectInsert = true;
        const auto result = runRecordedEdit(scene.operations());
        QVERIFY(!result.committed); QVERIFY(result.compensated); QVERIFY(result.touchedScene);
        QCOMPARE(scene.undos, 1); QCOMPARE(scene.groups, 0);
        QCOMPARE(scene.history.size(), qsizetype(1));
        QVERIFY(scene.current.exists); QCOMPARE(scene.current.color, QColor(Qt::black));
        FakeNativeScene refused; refused.rejectDelete = true;
        const auto untouched = runRecordedEdit(refused.operations());
        QVERIFY(!untouched.touchedScene); QCOMPARE(refused.inserts, 0); QCOMPARE(refused.undos, 0);
    }
    void observationMismatchUndoesThePairWithoutTouchingPreviousHistory() {
        FakeNativeScene scene; scene.verify = false;
        const auto result = runRecordedEdit(scene.operations());
        QVERIFY(!result.committed); QVERIFY(result.compensated); QVERIFY(result.touchedScene);
        QCOMPARE(scene.undos, 2); QCOMPARE(scene.groups, 0); QCOMPARE(scene.history.size(), qsizetype(1));
        QVERIFY(scene.current.exists); QCOMPARE(scene.current.color, QColor(Qt::black));
    }
    void unknownHistoryDeltasNeverUndoAnUnrelatedCommand() {
        FakeNativeScene deleted; deleted.omitDeleteHistory = true;
        const auto first = runRecordedEdit(deleted.operations());
        QCOMPARE(first.reason, QString("delete-history-unconfirmed"));
        QVERIFY(first.touchedScene); QVERIFY(!first.compensated);
        QCOMPARE(deleted.inserts, 0); QCOMPARE(deleted.undos, 0);
        FakeNativeScene inserted; inserted.omitInsertHistory = true;
        const auto second = runRecordedEdit(inserted.operations());
        QCOMPARE(second.reason, QString("insert-history-unconfirmed"));
        QVERIFY(second.touchedScene); QVERIFY(!second.compensated); QCOMPARE(inserted.undos, 0);
        FakeNativeScene grouping; grouping.badGrouping = true;
        const auto third = runRecordedEdit(grouping.operations());
        QCOMPARE(third.reason, QString("history-group-unconfirmed"));
        QVERIFY(!third.committed); QCOMPARE(grouping.undos, 0);
    }
    void nativePartialExceptionCannotBeClaimedAsAtomicRollback() {
        FakeNativeScene partial; partial.throwDuringInsert = true;
        const auto result = runRecordedEdit(partial.operations());
        QVERIFY(!result.committed); QVERIFY(!result.compensated); QVERIFY(result.touchedScene);
        QCOMPARE(partial.undos, 0); QCOMPARE(result.reason, QString("native-edit-exception"));
        FakeNativeScene observation; observation.throwDuringVerify = true;
        const auto recorded = runRecordedEdit(observation.operations());
        QVERIFY(!recorded.committed); QVERIFY(recorded.compensated); QCOMPARE(observation.undos, 2);
    }
    void hostDispatchNeverReadsNativePropertiesOrSendsAJob() {
        NativeSelectionColor editor; SceneController controller; QObject wrong;
        QSignalSpy finished(&editor, &NativeSelectionColor::finished);
        QVERIFY(!editor.dispatch(nullptr, 0, Qt::red));
        QVERIFY(!editor.dispatch(&wrong, 0, Qt::red));
        QVERIFY(!editor.dispatch(&controller, -1, Qt::red));
        QVERIFY(!editor.dispatch(&controller, 0, QColor()));
        QVERIFY(!editor.dispatch(&controller, 0, QColor(255, 0, 0, 128)));
        QVERIFY(!editor.dispatch(&controller, 0, Qt::red));
        QCOMPARE(controller.reads, 0); QVERIFY(!editor.busy()); QCOMPARE(finished.size(), 0);
        QVERIFY(!editor.reason().isEmpty());
    }
    void timeoutAndUnrelatedJobCannotReleaseAnExecutingEdit() {
        NativeSelectionColor editor; SceneController controller; DocumentWorker worker;
        installPending(editor, controller, worker);
        editor.d->warningAfterMs = -1;
        QSignalSpy finished(&editor, &NativeSelectionColor::finished);
        QSignalSpy status(&editor, &NativeSelectionColor::statusChanged);
        editor.verifyCompletion();
        QVERIFY(editor.busy()); QVERIFY(!editor.d->pending->cancelled.load()); QCOMPARE(status.size(), 1);
        emit worker.jobCompleted(41); QCoreApplication::processEvents();
        QVERIFY(editor.busy()); QCOMPARE(finished.size(), 0);
        editor.verifyCompletion(); QCOMPARE(status.size(), 1);
        emit worker.jobCompleted(42); QCoreApplication::processEvents();
        QVERIFY(!editor.busy()); QCOMPARE(finished.size(), 1); QVERIFY(!finished.first().first().toBool());
    }
    void callbackRefusalRequiresExactJobAndPageCancellationDropsLateResult() {
        NativeSelectionColor editor; SceneController controller; DocumentWorker worker;
        installPending(editor, controller, worker);
        auto pending = editor.d->pending;
        pending->callbackRan = true; pending->committed = false; pending->reason = QStringLiteral("selection-changed");
        QSignalSpy finished(&editor, &NativeSelectionColor::finished);
        emit worker.jobCompleted(999); QCoreApplication::processEvents();
        QVERIFY(editor.busy()); QCOMPARE(finished.size(), 0);
        controller.page = QStringLiteral("page-b");
        editor.verifyCompletion();
        QVERIFY(!editor.busy()); QVERIFY(pending->cancelled.load());
        emit worker.jobCompleted(42); QCoreApplication::processEvents();
        QCOMPARE(finished.size(), 0);
    }
    void destroyedControllerCancelsReceiptWithoutWorkerQObjectCapture() {
        NativeSelectionColor editor; DocumentWorker worker;
        auto controller = std::make_unique<SceneController>();
        installPending(editor, *controller, worker);
        auto pending = editor.d->pending;
        QSignalSpy finished(&editor, &NativeSelectionColor::finished);
        controller.reset();
        QVERIFY(!editor.busy()); QVERIFY(pending->cancelled.load());
        emit worker.jobCompleted(42); QCoreApplication::processEvents();
        QCOMPARE(finished.size(), 0);
    }
};
QTEST_GUILESS_MAIN(NativeSelectionColorTest)
#include "NativeSelectionColorTest.moc"
