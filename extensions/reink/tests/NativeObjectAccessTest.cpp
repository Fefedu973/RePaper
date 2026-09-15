#include "NativeObjectAccess.cpp"
#include <QtTest>
#include <stdexcept>
#include <map>

class DocumentWorker : public QObject {
    Q_OBJECT
signals:
    void jobCompleted(qulonglong id);
};
class SceneController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject* worker READ worker)
    Q_PROPERTY(QString pageId READ pageId)
    Q_PROPERTY(QTransform pendingEdit READ pendingEdit)
public:
    QObject *workerObject = nullptr;
    QString page = QStringLiteral("page-a");
    QTransform transform;
    QObject *worker() const { return workerObject; }
    QString pageId() const { return page; }
    QTransform pendingEdit() const { return transform; }
};

using namespace RePaperNative;
using namespace RePaperNative::ObjectAccessDetail;
namespace {
NativeObjectSnapshot baseline() {
    NativeObjectSnapshot snapshot;
    snapshot.documentId = "document-a"; snapshot.pageId = "page-a";
    snapshot.layer = 0; snapshot.layerId = 10;
    snapshot.complete = true; snapshot.pendingEditIdentity = true; snapshot.fingerprint = "snapshot-a";
    snapshot.nativeSelectionExact = true;
    for (quint64 id : {21, 22, 99}) {
        NativeObjectLine line;
        line.id = id; line.lineageId = id; line.parentId = 10; line.version = "version"; line.contentVersion = "content";
        snapshot.lines.append(line);
    }
    snapshot.selectedIds = {21};
    return snapshot;
}
struct FakeSelection {
    NativeObjectSnapshot actual = baseline();
    bool cancel = false, cancelInPreparation = false, prepareOk = true, verifyOk = true;
    bool throwApply = false, throwRestore = false;
    QStringList calls;
    SelectionOperations operations() {
        return {
            [this] { return cancel; },
            [this] { calls << "observe"; return actual; },
            [this] { calls << "prepare"; if (cancelInPreparation) cancel = true; return prepareOk; },
            [this] { calls << "apply"; if (throwApply) throw std::runtime_error("mutation interrupted"); },
            [this] { calls << "verify"; return verifyOk; },
            [this] { calls << "restore"; if (throwRestore) throw std::runtime_error("restoration interrupted"); }
        };
    }
};
NativeHistoryCommand command(quint64 id) {
    NativeHistoryCommand value;
    value.identity = id; value.control = id + 10000; value.type = 77;
    value.structure = QByteArray::number(id); value.cannotMerge = true;
    return value;
}
NativeObjectSnapshot historyBaseline() {
    auto snapshot = baseline();
    snapshot.history.valid = true; snapshot.history.appendIsolated = true;
    snapshot.history.historyIdentity = 100;
    snapshot.history.undo = {command(1), command(2)};
    return snapshot;
}
NativeObjectSnapshot insertionBaseline() {
    auto snapshot = historyBaseline();
    snapshot.sceneIdentity = 0x8000;
    return snapshot;
}
struct FakePropertyHistory {
    NativeObjectSnapshot initial = historyBaseline(), actual;
    QVector<NativeObjectSnapshot> states;
    QVector<quint64> undone;
    int observes = 0, clears = 0;
    bool cancel = false, clearOk = true, undoOk = true, badHistoryAfterUndo = false;
    bool badContentAfterUndo = false, freshIds = false, cancelAfterUndo = false, throwUndo = false;
    std::function<void(int)> onObserve;
    explicit FakePropertyHistory(int edits = 2) {
        states.append(initial);
        for (int i = 0; i < edits; ++i) {
            auto state = states.last();
            state.history.undo.append(command(3 + i)); state.history.redo.clear();
            state.lines[0].version = QByteArray::number(i + 1);
            state.lines[0].contentVersion = "content-" + QByteArray::number(i + 1);
            state.fingerprint = "edit-" + QByteArray::number(i + 1);
            states.append(state);
        }
        actual = states.last();
    }
    PropertyCancelOperations operations() {
        return {
            [this] { return cancel; },
            [this] { ++observes; if (onObserve) onObserve(observes); return actual; },
            [this] { ++clears; if (!clearOk) return false;
                actual.selectedIds.clear(); actual.nativeSelectionExact = true;
                actual.fingerprint = "selection-cleared"; return true; },
            [this](const NativeHistoryCommand &expected) {
                if (throwUndo) throw std::runtime_error("undo interrupted");
                if (!undoOk || actual.history.undo.isEmpty() || !(actual.history.undo.last() == expected)) return false;
                undone.append(expected.identity);
                auto history = actual.history;
                history.redo.append(history.undo.takeLast());
                history.appendIsolated = history.undo.isEmpty() || history.undo.last().cannotMerge;
                actual = states[history.undo.size() - initial.history.undo.size()];
                actual.history = history;
                if (badHistoryAfterUndo) actual.history.undo.append(command(500));
                if (badContentAfterUndo) actual.lines[0].contentVersion = "corrupted";
                if (freshIds) {
                    for (auto &line : actual.lines) { line.id += 1000; line.version = "relocated-version"; }
                }
                if (cancelAfterUndo) cancel = true;
                return true;
            }
        };
    }
};
struct HistoryMemory {
    std::map<quintptr, QByteArray> ranges;
    void record(quintptr address, qsizetype size) {
        ranges[address] = QByteArray(reinterpret_cast<const char*>(address), size);
    }
    bool contains(quintptr address, quintptr size, bool = false) const {
        if (!address || !size) return false;
        for (const auto &range : ranges)
            if (address >= range.first && address - range.first <= quintptr(range.second.size())
                && size <= quintptr(range.second.size()) - (address - range.first)) return true;
        return false;
    }
    template<class T> bool read(quintptr address, T &value) const {
        for (const auto &range : ranges)
            if (address >= range.first && address - range.first <= quintptr(range.second.size())
                && sizeof(T) <= quintptr(range.second.size()) - (address - range.first)) {
                std::memcpy(&value, range.second.constData() + address - range.first, sizeof(T)); return true;
            }
        return false;
    }
};
struct NativeTestCommand {
    quintptr table = 0x7000;
    QList<std::shared_ptr<void>> children;
};
struct NativeHistoryFixture {
    QByteArray scene = QByteArray(0x368, 0), history = QByteArray(0x50, 0);
    QList<std::shared_ptr<void>> undo, redo;
    bool executing = false;
    qint64 undoCountOverride = -1;
    static void word(QByteArray &bytes, qsizetype offset, quintptr value) {
        std::memcpy(bytes.data() + offset, &value, 8);
    }
    HistoryMemory memory() {
        static_assert(sizeof(std::shared_ptr<void>) == 16);
        static_assert(sizeof(undo) == 24 && sizeof(NativeTestCommand) == 32);
        HistoryMemory memory;
        word(scene, 0x348, quintptr(history.data())); word(history, 0x10, quintptr(scene.data()));
        std::memcpy(history.data() + 0x18, &undo, 24); std::memcpy(history.data() + 0x30, &redo, 24);
        history[0x48] = executing;
        if (undoCountOverride >= 0) word(history, 0x28, undoCountOverride);
        memory.record(quintptr(scene.constData()), scene.size());
        memory.record(quintptr(history.constData()), history.size());
        QByteArray plainTable(0x30, 0), macroTable(0x30, 0);
        word(plainTable, 0x28, HistoryDetail::NonMergingCommand);
        word(macroTable, 0x28, HistoryDetail::MacroMerge);
        memory.ranges[0x7000] = plainTable; memory.ranges[HistoryDetail::MacroVtable] = macroTable;
        std::set<quintptr> visited;
        std::function<void(const QList<std::shared_ptr<void>>&)> recordList;
        recordList = [&](const auto &items) {
            quintptr header[3]{}; std::memcpy(header, &items, 24);
            if (header[0]) memory.record(header[0], 16);
            if (header[2]) memory.record(header[1], header[2] * 16);
            for (const auto &item : items) {
                quintptr pair[2]{}; std::memcpy(pair, &item, 16);
                if (!visited.insert(pair[0]).second) continue;
                memory.record(pair[0], 32); memory.record(pair[1], 16);
                quintptr table = 0; std::memcpy(&table, reinterpret_cast<const void*>(pair[1]), 8);
                memory.record(table, 0x20);
                recordList(static_cast<NativeTestCommand*>(item.get())->children);
            }
        };
        recordList(undo); recordList(redo);
        return memory;
    }
    bool read(NativeHistorySnapshot &snapshot) {
        const auto maps = memory();
        return HistoryDetail::readSnapshot(maps, quintptr(scene.constData()), snapshot);
    }
};
}

class NativeObjectAccessTest : public QObject {
    Q_OBJECT
    static void pending(NativeObjectAccess &access, SceneController &controller, DocumentWorker &worker) {
        controller.workerObject = &worker;
        access.d->controller = &controller; access.d->worker = &worker;
        access.d->context = {{"documentId", "document-a"}, {"pageId", "page-a"}, {"layer", 0}};
        access.d->busy = true; access.d->jobId = 42;
        access.d->pending = std::make_shared<NativeObjectAccess::Private::Pending>();
        access.d->pending->ran = true; access.d->pending->success = true;
        access.d->pending->result = baseline();
        access.d->elapsed.start();
        access.d->completionConnection = QObject::connect(&worker, &DocumentWorker::jobCompleted,
            &access, &NativeObjectAccess::jobCompleted, Qt::QueuedConnection);
        access.d->workerDestroyed = QObject::connect(&worker, &QObject::destroyed,
            &access, &NativeObjectAccess::workerUnavailable);
        access.d->controllerDestroyed = QObject::connect(&controller, &QObject::destroyed,
            &access, &NativeObjectAccess::controllerUnavailable);
    }
private slots:
    void memoryRangesKeepBoundsAndPermissionsWhileAvoidingLinearScans() {
        QVector<MemoryRegion> regions;
        for(quintptr i=0;i<4096;++i)regions.append({0x10000+i*0x2000,0x11000+i*0x2000,true,i%2==0});
        MemoryRanges ranges(std::move(regions));
        qsizetype probes=0;
        for(quintptr i=0;i<4096;++i){
            const auto address=0x10000+i*0x2000;
            QVERIFY(ranges.contains(address,0x1000,true,false,&probes));
            QVERIFY(!ranges.contains(address,0x1001));
            QVERIFY(!ranges.contains(address+0x1000,1));
            QCOMPARE(ranges.contains(address,4,false,true),i%2==0);
        }
        QVERIFY(probes<4096*15); // logarithmic lookup, not millions of comparisons
        qsizetype repeated=0;
        for(int i=0;i<100000;++i)QVERIFY(ranges.contains(0x10000+4095*0x2000+14,14,true,false,&repeated));
        QCOMPARE(repeated,qsizetype(100000));
        QVERIFY(!ranges.contains(std::numeric_limits<quintptr>::max()-2,8));
        QVERIFY(!ranges.contains(0x10000,0));
        QVERIFY(!MemoryRanges({{100,300,true,false},{200,400,true,false}}).contains(250,1));
    }
    void exactSelectionRejectsAReplacedSceneWithMatchingContent() {
        auto expected=baseline(),actual=expected;
        expected.sceneIdentity=0x1000;actual.sceneIdentity=0x2000;
        QCOMPARE(selectionRequestError(expected,actual,expected.selectedIds),QStringLiteral("snapshot-changed"));
    }
    void guardedInsertionAcceptsUnchangedCompleteNativeObservation() {
        const auto expected = insertionBaseline();
        QVERIFY(insertionBaselineError(expected, expected).isEmpty());
        auto empty = expected;
        empty.lines.clear(); empty.selectedIds.clear();
        empty.history.undo.clear(); empty.history.redo.clear();
        QVERIFY(insertionBaselineError(empty, empty).isEmpty());
    }
    void guardedInsertionRejectsMissingProof_data() {
        QTest::addColumn<int>("missing");
        QTest::newRow("incomplete-snapshot") << 0;
        QTest::newRow("pending-edit") << 1;
        QTest::newRow("scene-identity") << 2;
        QTest::newRow("fingerprint") << 3;
        QTest::newRow("history-validity") << 4;
        QTest::newRow("history-identity") << 5;
        QTest::newRow("negative-item-count") << 6;
        QTest::newRow("document-id") << 7;
        QTest::newRow("page-id") << 8;
        QTest::newRow("layer-id") << 9;
        QTest::newRow("line-version") << 10;
    }
    void guardedInsertionRejectsMissingProof() {
        QFETCH(int, missing);
        const auto valid = insertionBaseline();
        auto incomplete = valid;
        switch (missing) {
        case 0: incomplete.complete = false; break;
        case 1: incomplete.pendingEditIdentity = false; break;
        case 2: incomplete.sceneIdentity = 0; break;
        case 3: incomplete.fingerprint.clear(); break;
        case 4: incomplete.history.valid = false; break;
        case 5: incomplete.history.historyIdentity = 0; break;
        case 6: incomplete.unsupportedItemCount = -1; break;
        case 7: incomplete.documentId.clear(); break;
        case 8: incomplete.pageId.clear(); break;
        case 9: incomplete.layerId = 0; break;
        case 10: incomplete.lines[0].version.clear(); break;
        }
        QCOMPARE(insertionBaselineError(incomplete, valid), QString("insertion-baseline-incomplete"));
        QCOMPARE(insertionBaselineError(valid, incomplete), QString("insertion-baseline-incomplete"));
    }
    void guardedInsertionRejectsDifferentSceneEvenWithMatchingContentAndHistory() {
        const auto expected = insertionBaseline();
        auto changed = expected;
        ++changed.sceneIdentity;
        QVERIFY(sameNativeObjectContent(expected, changed));
        QVERIFY(sameNativeHistory(expected.history, changed.history));
        QCOMPARE(insertionBaselineError(expected, changed), QString("insertion-scene-changed"));
    }
    void guardedInsertionRejectsChangedContent_data() {
        QTest::addColumn<int>("change");
        QTest::newRow("document") << 0;
        QTest::newRow("page") << 1;
        QTest::newRow("layer-index") << 2;
        QTest::newRow("layer-id") << 3;
        QTest::newRow("line-id") << 4;
        QTest::newRow("line-parent") << 5;
        QTest::newRow("line-lineage") << 6;
        QTest::newRow("line-version") << 7;
        QTest::newRow("fingerprint") << 8;
        QTest::newRow("unsupported-active-object") << 9;
        QTest::newRow("removed-line") << 10;
    }
    void guardedInsertionRejectsChangedContent() {
        QFETCH(int, change);
        const auto expected = insertionBaseline();
        auto changed = expected;
        switch (change) {
        case 0: changed.documentId = "another-document"; break;
        case 1: changed.pageId = "another-page"; break;
        case 2: ++changed.layer; break;
        case 3: ++changed.layerId; break;
        case 4: ++changed.lines.last().id; break;
        case 5: ++changed.lines[0].parentId; break;
        case 6: ++changed.lines[0].lineageId; break;
        case 7: changed.lines[0].version = "new-ink"; break;
        case 8: changed.fingerprint = "other-selection-records"; break;
        case 9: ++changed.unsupportedItemCount; break;
        case 10: changed.lines.removeLast(); break;
        }
        QCOMPARE(insertionBaselineError(expected, changed), QString("insertion-snapshot-changed"));
    }
    void guardedInsertionRequiresUnchangedExactSelection() {
        const auto expected = insertionBaseline();
        auto changed = expected;
        changed.selectedIds = {22};
        QCOMPARE(insertionBaselineError(expected, changed), QString("insertion-selection-changed"));
        changed = expected; changed.nativeSelectionExact = false;
        QCOMPARE(insertionBaselineError(expected, changed), QString("insertion-selection-changed"));
        QCOMPARE(insertionBaselineError(changed, expected), QString("insertion-selection-changed"));
    }
    void guardedInsertionRejectsTextUndoAndReplacedHistoryCommands_data() {
        QTest::addColumn<int>("change");
        QTest::newRow("native-text-appended") << 0;
        QTest::newRow("undo") << 1;
        QTest::newRow("redo-cleared") << 2;
        QTest::newRow("same-depth-command-replaced") << 3;
        QTest::newRow("same-command-different-control") << 4;
        QTest::newRow("macro-child-mutated") << 5;
        QTest::newRow("history-object-replaced") << 6;
        QTest::newRow("merge-isolation-changed") << 7;
    }
    void guardedInsertionRejectsTextUndoAndReplacedHistoryCommands() {
        QFETCH(int, change);
        auto expected = insertionBaseline();
        expected.history.redo = {command(8)};
        auto changed = expected;
        switch (change) {
        case 0: changed.history.undo.append(command(3)); break;
        case 1: changed.history.redo.append(changed.history.undo.takeLast()); break;
        case 2: changed.history.redo.clear(); break;
        case 3: changed.history.undo.last() = command(7); break;
        case 4: ++changed.history.undo.last().control; break;
        case 5: changed.history.undo.first().structure = "mutated-macro"; break;
        case 6: ++changed.history.historyIdentity; break;
        case 7: changed.history.appendIsolated = false; break;
        }
        QVERIFY(sameNativeObjectContent(expected, changed));
        QCOMPARE(expected.fingerprint, changed.fingerprint);
        QCOMPARE(insertionBaselineError(expected, changed), QString("insertion-history-changed"));
    }
    void guardedInsertionCannotFallBackToAnUnguardedInsert() {
        NativeObjectAccess access;
        SceneController controller;
        const QVariantMap context{{"documentId", "document-a"}, {"pageId", "page-a"}, {"layer", 0}};
        QVERIFY(!access.insertIfUnchanged(&controller, context, {}, {}, {}));
        QCOMPARE(access.reason(), QString("insertion-baseline-incomplete"));
        QVERIFY(!access.busy());
        // Existing insertion remains available without a baseline and reaches
        // the host ABI gate, rather than acquiring the new baseline requirement.
        QVERIFY(!access.insert(&controller, context, {}, {}));
        QCOMPARE(access.reason(), QString("native-object-access-unavailable-on-host"));
    }
    void nativeHistoryReaderRetainsTheExactCommandAndControlIdentity() {
        NativeHistoryFixture native;
        auto original = std::make_shared<NativeTestCommand>();
        const auto identity = quintptr(original.get()); std::weak_ptr<void> alive = original;
        native.undo.append(original); original.reset();
        NativeHistorySnapshot snapshot;
        QVERIFY(native.read(snapshot)); QVERIFY(snapshot.valid); QVERIFY(snapshot.appendIsolated);
        QCOMPARE(snapshot.undo.size(), 1); QCOMPARE(snapshot.undo.first().identity, quint64(identity));
        QVERIFY(snapshot.undo.first().control); QVERIFY(snapshot.undo.first().retained);
        native.undo.clear(); QVERIFY(!alive.expired());
        snapshot = {}; QVERIFY(alive.expired());
    }
    void nativeHistoryReaderRetainsAndFingerprintsEveryMacroChild() {
        NativeHistoryFixture native;
        auto macro = std::make_shared<NativeTestCommand>(); macro->table = HistoryDetail::MacroVtable;
        auto oldChild = std::make_shared<NativeTestCommand>(); std::weak_ptr<void> childAlive = oldChild;
        macro->children.append(oldChild); oldChild.reset(); native.undo.append(macro);
        NativeHistorySnapshot before, after;
        QVERIFY(native.read(before)); QVERIFY(before.appendIsolated);
        QCOMPARE(before.undo.first().retainedChildren.size(), 1);
        macro->children.clear(); macro->children.append(std::make_shared<NativeTestCommand>());
        QVERIFY(!childAlive.expired()); QVERIFY(native.read(after));
        QCOMPARE(before.undo.first().identity, after.undo.first().identity);
        QVERIFY(!sameNativeHistory(before, after));
        before = {}; QVERIFY(childAlive.expired());
    }
    void nativeHistoryReaderRejectsExecutingDuplicateAndOversizedStacks() {
        for (int malformed = 0; malformed < 3; ++malformed) {
            NativeHistoryFixture native;
            auto command = std::make_shared<NativeTestCommand>(); native.undo.append(command);
            if (malformed == 0) native.executing = true;
            if (malformed == 1) native.redo.append(command);
            if (malformed == 2) native.undoCountOverride = HistoryDetail::MaxSnapshotCommands + 1;
            NativeHistorySnapshot snapshot;
            QVERIFY(!native.read(snapshot)); QVERIFY(!snapshot.valid); QVERIFY(snapshot.undo.isEmpty());
        }
    }
    void nativeHistoryReaderRejectsCyclesAndIncompleteControlOwnership() {
        NativeHistoryFixture native;
        auto macro = std::make_shared<NativeTestCommand>(); macro->table = HistoryDetail::MacroVtable;
        macro->children.append(macro); native.undo.append(macro);
        NativeHistorySnapshot snapshot;
        QVERIFY(!native.read(snapshot)); QVERIFY(!snapshot.valid);
        macro->children.clear();
        auto memory = native.memory();
        quintptr pair[2]{}; std::memcpy(pair, &native.undo.first(), 16);
        memory.ranges.erase(pair[1]);
        QVERIFY(!HistoryDetail::readSnapshot(memory, quintptr(native.scene.constData()), snapshot));
        QVERIFY(!snapshot.valid);
    }
    void propertiesCancelConsumesOnlyTheAttributedSuffix() {
        FakePropertyHistory native;
        const auto result = runPropertyCancel(native.initial, native.actual, native.operations());
        QVERIFY(result.cancelled); QVERIFY(result.touched); QCOMPARE(result.undone, 2);
        QCOMPARE(native.undone, QVector<quint64>({4, 3}));
        QCOMPARE(result.snapshot.history.undo, native.initial.history.undo);
        QCOMPARE(result.snapshot.history.redo, QVector<NativeHistoryCommand>({command(4), command(3)}));
        QVERIFY(result.snapshot.nativeSelectionExact); QVERIFY(result.snapshot.selectedIds.isEmpty());
        QVERIFY(nativeObjectContentRestored(native.initial, result.snapshot));
    }
    void propertiesCancelAcceptsVerifiedNativeRelocationButNotDifferentContent() {
        FakePropertyHistory native; native.freshIds = true;
        const auto result = runPropertyCancel(native.initial, native.actual, native.operations());
        QVERIFY(result.cancelled); QVERIFY(!sameNativeObjectContent(native.initial, result.snapshot));
        QVERIFY(nativeObjectContentRestored(native.initial, result.snapshot));
        for (int mismatch = 0; mismatch < 3; ++mismatch) {
            auto changed = result.snapshot;
            if (mismatch == 0) changed.lines[0].contentVersion = "different-pressure-bytes";
            if (mismatch == 1) changed.lines[0].lineageId += 1;
            if (mismatch == 2) changed.lines[0].parentId += 1;
            QVERIFY(!nativeObjectContentRestored(native.initial, changed));
        }
    }
    void propertiesCancelWithoutEditsOnlyClearsSelectionAndPreservesRedo() {
        FakePropertyHistory native(0);
        native.initial.history.redo = {command(8)}; native.actual = native.initial;
        const auto result = runPropertyCancel(native.initial, native.actual, native.operations());
        QVERIFY(result.cancelled); QCOMPARE(result.undone, 0); QCOMPARE(native.clears, 1);
        QVERIFY(native.undone.isEmpty()); QVERIFY(result.snapshot.selectedIds.isEmpty());
        QCOMPARE(result.snapshot.history.redo, native.initial.history.redo);
    }
    void propertiesCancelRejectsUnownedHistoryEvenAtTheSameDepth() {
        for (int change = 0; change < 6; ++change) {
            FakePropertyHistory native; const auto expected = native.actual;
            if (change == 0) native.actual.history.undo.last() = command(90);
            if (change == 1) ++native.actual.history.undo.last().control;
            if (change == 2) native.actual.history.undo.first().structure = "different-macro-child";
            if (change == 3) ++native.actual.history.historyIdentity;
            if (change == 4) native.actual.history.redo.append(command(91));
            if (change == 5) native.actual.history.valid = false;
            const auto result = runPropertyCancel(native.initial, expected, native.operations());
            QVERIFY(!result.cancelled); QVERIFY(!result.touched); QVERIFY(native.undone.isEmpty());
        }
    }
    void propertiesCancelRejectsForeignContentPageAndCheckpointPrefix() {
        for (int change = 0; change < 5; ++change) {
            FakePropertyHistory native; const auto expected = native.actual;
            if (change == 0) native.actual.lines[0].version = "foreign-edit";
            if (change == 1) native.actual.pageId = "other-page";
            if (change == 2) native.actual.pendingEditIdentity = false;
            if (change == 3) native.initial.history.undo.first() = command(999);
            if (change == 4) native.initial.layerId = 999;
            const auto result = runPropertyCancel(native.initial, expected, native.operations());
            QVERIFY(!result.cancelled); QVERIFY(!result.touched); QVERIFY(native.undone.isEmpty());
        }
    }
    void propertiesCancelRechecksTheConcreteCommandImmediatelyBeforeUndo() {
        FakePropertyHistory native; const auto expected = native.actual;
        native.onObserve = [&](int count) { if (count == 3) native.actual.history.undo.last() = command(999); };
        const auto result = runPropertyCancel(native.initial, expected, native.operations());
        QVERIFY(!result.cancelled); QVERIFY(result.touched); QVERIFY(native.undone.isEmpty());
        QCOMPARE(result.reason, QStringLiteral("property-session-history-changed-during-cancel"));
    }
    void propertiesCancelStopsAfterAnUnconfirmedUndo() {
        for (int failure = 0; failure < 4; ++failure) {
            FakePropertyHistory native;
            native.badHistoryAfterUndo = failure == 0; native.undoOk = failure != 1;
            native.throwUndo = failure == 2; native.clearOk = failure != 3;
            const auto result = runPropertyCancel(native.initial, native.actual, native.operations());
            QVERIFY(!result.cancelled); QVERIFY(native.undone.size() <= 1);
        }
    }
    void propertiesCancelNeverClaimsAnUnverifiedContentRestoration() {
        FakePropertyHistory native; native.badContentAfterUndo = true;
        const auto result = runPropertyCancel(native.initial, native.actual, native.operations());
        QVERIFY(!result.cancelled); QCOMPARE(native.undone.size(), 2);
        QCOMPARE(result.reason, QStringLiteral("property-session-restoration-unconfirmed"));
    }
    void propertiesCancelHonorsInterruptionAndTheCommandBudget() {
        FakePropertyHistory native; native.cancel = true;
        auto result = runPropertyCancel(native.initial, native.actual, native.operations());
        QVERIFY(!result.cancelled); QVERIFY(!result.touched); QVERIFY(native.undone.isEmpty());
        native.cancel = false; native.cancelAfterUndo = true;
        result = runPropertyCancel(native.initial, native.actual, native.operations());
        QVERIFY(!result.cancelled); QCOMPARE(native.undone.size(), 1);
        FakePropertyHistory oversized(MaxPropertyCancelCommands + 1);
        result = runPropertyCancel(oversized.initial, oversized.actual, oversized.operations());
        QVERIFY(!result.cancelled); QVERIFY(!result.touched); QVERIFY(oversized.undone.isEmpty());
        QCOMPARE(result.reason, QStringLiteral("property-session-history-budget"));
    }
    void historyAttributionRequiresOneNewIsolatedCommandAndAnUnchangedPrefix() {
        FakePropertyHistory native(1);
        QVERIFY(nativeHistoryAppended(native.initial.history, native.actual.history));
        auto before = native.initial.history; before.appendIsolated = false;
        QVERIFY(!nativeHistoryAppended(before, native.actual.history));
        auto after = native.actual.history; after.undo.first().structure = "macro-grouped-in-place";
        QVERIFY(!nativeHistoryAppended(native.initial.history, after));
        after = native.actual.history; after.undo.append(command(5));
        QVERIFY(!nativeHistoryAppended(native.initial.history, after));
        after = native.actual.history; after.redo.append(command(6));
        QVERIFY(!nativeHistoryAppended(native.initial.history, after));
    }
    void exactSelectionCanIncludeSeparatedSiblingsWithoutUnrelatedInk() {
        FakeSelection native;
        const auto result = runSelection(baseline(), {21, 22}, native.operations());
        QVERIFY(result.applied); QVERIFY(result.touched);
        QCOMPARE(native.calls, QStringList({"observe", "prepare", "apply", "verify"}));
    }
    void changedContentFingerprintRejectsBeforeMutation() {
        FakeSelection native; native.actual.fingerprint = "modified-by-another-job";
        const auto result = runSelection(baseline(), {21, 22}, native.operations());
        QVERIFY(!result.applied); QVERIFY(!result.touched);
        QCOMPARE(result.reason, QStringLiteral("snapshot-changed"));
        QCOMPARE(native.calls, QStringList({"observe"}));
    }
    void partialBindingAndDuplicateIdsAreRejected() {
        for (const auto ids : {QVector<quint64>{21, 404}, QVector<quint64>{21, 21}}) {
            FakeSelection native;
            const auto result = runSelection(baseline(), ids, native.operations());
            QVERIFY(!result.applied); QVERIFY(!result.touched);
            QCOMPARE(native.calls, QStringList({"observe"}));
        }
    }
    void pendingTransformAndIncompleteObservationAreRejected() {
        FakeSelection native; native.actual.complete = false;
        QVERIFY(!runSelection(baseline(), {21}, native.operations()).touched);
        native.actual = baseline(); native.actual.pendingEditIdentity = false;
        QVERIFY(!runSelection(baseline(), {21}, native.operations()).touched);
    }
    void exactEmptySelectionIsAnExplicitClear() {
        FakeSelection native;
        QVERIFY(runSelection(baseline(), {}, native.operations()).applied);
        QCOMPARE(native.calls, QStringList({"observe", "prepare", "apply", "verify"}));
    }
    void cancelAfterPreparationDoesNotTouchSelection() {
        FakeSelection native; native.cancelInPreparation = true;
        const auto result = runSelection(baseline(), {21, 22}, native.operations());
        QVERIFY(!result.applied); QVERIFY(!result.touched);
        QCOMPARE(native.calls, QStringList({"observe", "prepare"}));
    }
    void failedVerificationRestoresPreviousSelection() {
        FakeSelection native; native.verifyOk = false;
        const auto result = runSelection(baseline(), {21, 22}, native.operations());
        QVERIFY(!result.applied); QVERIFY(result.touched);
        QCOMPARE(native.calls, QStringList({"observe", "prepare", "apply", "verify", "restore"}));
    }
    void nativeExceptionAttemptsRestoration() {
        FakeSelection native; native.throwApply = true;
        const auto result = runSelection(baseline(), {21, 22}, native.operations());
        QVERIFY(!result.applied); QVERIFY(result.touched);
        QCOMPARE(native.calls, QStringList({"observe", "prepare", "apply", "restore"}));
    }
    void restorationExceptionDoesNotSwapPriorStateBackOut() {
        FakeSelection native; native.verifyOk = false; native.throwRestore = true;
        const auto result = runSelection(baseline(), {21, 22}, native.operations());
        QVERIFY(!result.applied); QVERIFY(result.touched);
        QCOMPARE(native.calls, QStringList({"observe", "prepare", "apply", "verify", "restore"}));
    }
    void anotherJobCannotCompleteReceipt() {
        NativeObjectAccess access; SceneController controller; DocumentWorker worker;
        pending(access, controller, worker); QSignalSpy finished(&access, &NativeObjectAccess::finished);
        emit worker.jobCompleted(41); QCoreApplication::processEvents();
        QVERIFY(access.busy()); QCOMPARE(finished.size(), 0);
        emit worker.jobCompleted(42); QTRY_COMPARE(finished.size(), 1);
        QVERIFY(finished.first().first().toBool()); QVERIFY(!access.busy());
        QCOMPARE(access.result().fingerprint, QByteArray("snapshot-a"));
        QVERIFY(access.result().nativeSelectionExact);
    }
    void inspectionReceiptPreservesMissingSelectionProof() {
        NativeObjectAccess access; SceneController controller; DocumentWorker worker;
        pending(access, controller, worker); access.d->pending->result.nativeSelectionExact = false;
        QSignalSpy finished(&access, &NativeObjectAccess::finished);
        emit worker.jobCompleted(42); QTRY_COMPARE(finished.size(), 1);
        QVERIFY(finished.first().first().toBool());
        QVERIFY(access.result().complete); QVERIFY(!access.result().nativeSelectionExact);
    }
    void noCallbackIsNotSuccess() {
        NativeObjectAccess access; SceneController controller; DocumentWorker worker;
        pending(access, controller, worker); access.d->pending->ran = false;
        QSignalSpy finished(&access, &NativeObjectAccess::finished);
        emit worker.jobCompleted(42); QTRY_COMPARE(finished.size(), 1);
        QVERIFY(!finished.first().first().toBool());
    }
    void pageChangeFailsReceiptAndDropsObsoleteResult() {
        NativeObjectAccess access; SceneController controller; DocumentWorker worker;
        pending(access, controller, worker); controller.page = "other-page";
        QSignalSpy finished(&access, &NativeObjectAccess::finished);
        emit worker.jobCompleted(42); QCoreApplication::processEvents();
        QVERIFY(!access.busy()); QCOMPARE(finished.size(), 1); QVERIFY(!access.result().complete);
        QVERIFY(!finished.first().first().toBool());
        QCOMPARE(access.reason(), QStringLiteral("native-page-context-changed"));
    }
    void workerDestructionFailsPendingReceipt() {
        NativeObjectAccess access; SceneController controller;
        auto worker = std::make_unique<DocumentWorker>();
        pending(access, controller, *worker);
        QSignalSpy finished(&access, &NativeObjectAccess::finished);
        worker.reset();
        QVERIFY(!access.busy()); QCOMPARE(finished.size(), 1);
        QVERIFY(!finished.first().first().toBool());
        QCOMPARE(access.reason(), QStringLiteral("native-worker-destroyed"));
    }
    void controllerDestructionFailsPendingReceipt() {
        NativeObjectAccess access; DocumentWorker worker;
        auto controller = std::make_unique<SceneController>();
        pending(access, *controller, worker);
        QSignalSpy finished(&access, &NativeObjectAccess::finished);
        controller.reset();
        QVERIFY(!access.busy()); QCOMPARE(finished.size(), 1);
        QVERIFY(!finished.first().first().toBool());
        QCOMPARE(access.reason(), QStringLiteral("native-controller-destroyed"));
    }
    void workerReplacementFailsPendingReceipt() {
        NativeObjectAccess access; SceneController controller; DocumentWorker worker, replacement;
        pending(access, controller, worker); controller.workerObject = &replacement;
        QSignalSpy finished(&access, &NativeObjectAccess::finished);
        access.checkCompletion();
        QVERIFY(!access.busy()); QCOMPARE(finished.size(), 1);
        QVERIFY(!finished.first().first().toBool());
        QCOMPARE(access.reason(), QStringLiteral("native-worker-replaced"));
    }
    void explicitCancelDoesNotPublishReceipt() {
        NativeObjectAccess access; SceneController controller; DocumentWorker worker;
        pending(access, controller, worker);
        QSignalSpy finished(&access, &NativeObjectAccess::finished);
        access.cancel(); emit worker.jobCompleted(42); QCoreApplication::processEvents();
        QVERIFY(!access.busy()); QCOMPARE(finished.size(), 0);
    }
    void transformStartedDuringCallbackRefusesConsumption() {
        NativeObjectAccess access; SceneController controller; DocumentWorker worker;
        pending(access, controller, worker); controller.transform.translate(5, 7);
        QSignalSpy finished(&access, &NativeObjectAccess::finished);
        emit worker.jobCompleted(42); QTRY_COMPARE(finished.size(), 1);
        QVERIFY(!finished.first().first().toBool()); QVERIFY(!access.result().pendingEditIdentity);
    }
};
QTEST_GUILESS_MAIN(NativeObjectAccessTest)
#include "NativeObjectAccessTest.moc"
