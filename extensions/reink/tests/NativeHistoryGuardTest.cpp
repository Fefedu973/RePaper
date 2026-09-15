#include "NativeHistoryGuard.h"
#include <QtTest>
#include <cstring>
#include <limits>
#include <map>
#include <vector>

namespace {
using Word = quintptr;
using RePaperNative::HistoryDetail::commandCannotMerge;
using RePaperNative::HistoryDetail::commandCannotMergeNativeLineInsertion;

// Separate mapped regions make invalid pointers fail exactly as /proc/self/maps
// validation does. The production traversal runs unchanged against these bytes.
struct Memory {
    std::map<Word, QByteArray> regions;
    Word next = 0x100000;
    Memory() {
        regions.emplace(0x1680820, QByteArray(0x30, '\0'));
        write<Word>(0x1680848, 0xe4efc0);
    }
    bool contains(Word address, Word length) const {
        if (!address || !length || length > std::numeric_limits<Word>::max() - address) return false;
        auto region = regions.upper_bound(address);
        if (region == regions.begin()) return false;
        --region;
        return address >= region->first && address + length <= region->first + Word(region->second.size());
    }
    template<class T> bool read(Word address, T &value) const {
        if (!contains(address, sizeof(T))) return false;
        auto region = regions.upper_bound(address); --region;
        std::memcpy(&value, region->second.constData() + address - region->first, sizeof(T));
        return true;
    }
    template<class T> void write(Word address, T value) {
        Q_ASSERT(contains(address, sizeof(T)));
        auto region = regions.upper_bound(address); --region;
        std::memcpy(region->second.data() + address - region->first, &value, sizeof(T));
    }
    Word allocate(qsizetype bytes) {
        const Word address = next;
        next += (Word(bytes) + 31) & ~Word(15);
        regions.emplace(address, QByteArray(bytes, '\0'));
        return address;
    }
    Word leaf(Word merge = 0x496520) {
        const Word table = allocate(0x30), command = allocate(8);
        write<Word>(table + 0x28, merge);
        write<Word>(command, table);
        return command;
    }
    Word macro(const std::vector<Word> &children = {}) {
        const Word command = allocate(0x20);
        write<Word>(command, 0x1680820);
        if (!children.empty()) {
            const Word array = allocate(16), data = allocate(qsizetype(children.size()) * 16);
            write<Word>(command + 8, array);
            write<Word>(command + 0x10, data);
            for (size_t i = 0; i < children.size(); ++i) write<Word>(data + i * 16, children[i]);
        }
        write<qint64>(command + 0x18, qint64(children.size()));
        return command;
    }
    Word textInsert() {
        regions.emplace(0x1683188, QByteArray(0x30, '\0'));
        write<Word>(0x16831b0, 0xef64c0);
        const Word command = allocate(8);
        write<Word>(command, 0x1683188);
        return command;
    }
    Word childData(Word macro) const {
        Word data = 0; const bool readOk = read(macro + 0x10, data); Q_ASSERT(readOk); return data;
    }
};
}

class NativeHistoryGuardTest : public QObject {
    Q_OBJECT
private slots:
    void titleInsertionAllowsHeaderInkWithoutWeakeningGenericHistoryGuard() {
        Memory memory;
        const auto title = memory.textInsert();
        QVERIFY(!commandCannotMerge(memory, title));
        QVERIFY(commandCannotMergeNativeLineInsertion(memory, title));
        // Native text replacement may group removal and insertion. Only its
        // final text command decides whether the incoming line edit merges.
        const auto groupedTitle = memory.macro({memory.leaf(), memory.macro({title})});
        QVERIFY(!commandCannotMerge(memory, groupedTitle));
        QVERIFY(commandCannotMergeNativeLineInsertion(memory, groupedTitle));
        const auto header = memory.leaf();
        QVERIFY(commandCannotMerge(memory, header));
        QVERIFY(commandCannotMergeNativeLineInsertion(memory, header));
    }
    void nativeLineInsertionRequiresExactTextTypeAndMergeImplementation() {
        Memory memory;
        const auto title = memory.textInsert();
        const auto unknown = memory.leaf(0xef64c0);
        QVERIFY(!commandCannotMergeNativeLineInsertion(memory, unknown));
        QVERIFY(!commandCannotMergeNativeLineInsertion(memory, memory.macro({title, unknown})));
        memory.write<Word>(0x16831b0, 0x123456);
        QVERIFY(!commandCannotMergeNativeLineInsertion(memory, title));
        memory.regions.erase(0x1683188);
        QVERIFY(!commandCannotMergeNativeLineInsertion(memory, title));
    }
    void directKnownFalseAndUnknownCommands() {
        Memory memory;
        QVERIFY(commandCannotMerge(memory, memory.leaf()));
        QVERIFY(!commandCannotMerge(memory, memory.leaf(0x123456)));
        QVERIFY(!commandCannotMerge(memory, 0));
        QVERIFY(!commandCannotMerge(memory, 0x800000));
    }
    void consecutiveGroupedReplacementsRemainAdmissible() {
        Memory memory;
        Word historyTail = memory.leaf();
        for (int edit = 0; edit < 4; ++edit) {
            // Real replacement first checks the old history tail, then groups
            // its two known nonmerging native delete/insert commands.
            QVERIFY2(commandCannotMerge(memory, historyTail), "a previous replacement must not block the next edit");
            historyTail = memory.macro({memory.leaf(), memory.leaf()});
        }
        QVERIFY(commandCannotMerge(memory, historyTail));
        QVERIFY(commandCannotMerge(memory, memory.macro({memory.leaf(), memory.macro({historyTail})})));
    }
    void onlyTheLastChildControlsMerge() {
        Memory memory;
        const Word noMerge = memory.leaf(), unknown = memory.leaf(0x123456);
        QVERIFY(commandCannotMerge(memory, memory.macro({unknown, noMerge})));
        QVERIFY(!commandCannotMerge(memory, memory.macro({noMerge, unknown})));
        QVERIFY(!commandCannotMerge(memory, memory.macro({noMerge, 0})));
    }
    void emptyMacroAndNestedEmptyMacroCannotMerge() {
        Memory memory;
        const Word empty = memory.macro();
        QVERIFY(commandCannotMerge(memory, empty));
        QVERIFY(commandCannotMerge(memory, memory.macro({empty})));
    }
    void macroVtableAndDelegationFunctionMustBothMatch() {
        Memory memory;
        const Word macro = memory.macro({memory.leaf()});
        const Word impostor = memory.allocate(0x30);
        memory.write<Word>(impostor + 0x28, 0xe4efc0);
        memory.write<Word>(macro, impostor);
        QVERIFY(!commandCannotMerge(memory, macro));
        memory.write<Word>(macro, 0x1680820);
        memory.write<Word>(0x1680848, 0x123456);
        QVERIFY(!commandCannotMerge(memory, macro));
    }
    void cyclesAreRejected() {
        Memory memory;
        const Word a = memory.macro({memory.leaf()});
        memory.write<Word>(memory.childData(a), a);
        QVERIFY(!commandCannotMerge(memory, a));
        const Word b = memory.macro({a});
        memory.write<Word>(memory.childData(a), b);
        QVERIFY(!commandCannotMerge(memory, a));
    }
    void negativeAndOversizedCountsAreRejected() {
        for (const qint64 count : {qint64(-1), qint64(4097), std::numeric_limits<qint64>::max()}) {
            Memory memory;
            const Word macro = memory.macro({memory.leaf()});
            memory.write<qint64>(macro + 0x18, count);
            QVERIFY(!commandCannotMerge(memory, macro));
        }
    }
    void listAndCommandMappingsMustBeComplete() {
        for (int broken = 0; broken < 6; ++broken) {
            Memory memory;
            const Word leaf = memory.leaf(), macro = memory.macro({leaf});
            if (broken == 0) memory.write<Word>(macro + 8, 0x800000);
            if (broken == 1) memory.write<Word>(macro + 0x10, 0x800000);
            if (broken == 2) memory.write<Word>(macro + 0x10, std::numeric_limits<Word>::max() - 7);
            if (broken == 3) memory.write<qint64>(macro + 0x18, 2); // only one entry is mapped
            if (broken == 4) memory.regions.erase(leaf);
            if (broken == 5) memory.regions[macro].resize(0x18); // count word is unreadable
            QVERIFY(!commandCannotMerge(memory, macro));
        }
    }
    void depthLimitAllows64MacroLinks() {
        Memory memory;
        Word command = memory.leaf();
        for (int depth = 0; depth < 64; ++depth) command = memory.macro({command});
        QVERIFY(commandCannotMerge(memory, command));
        QVERIFY(!commandCannotMerge(memory, memory.macro({command})));
    }
    void childBudgetIsCumulativeAcrossNestedLists() {
        Memory memory;
        const Word leaf = memory.leaf();
        const Word wide = memory.macro(std::vector<Word>(4096, leaf));
        QVERIFY(commandCannotMerge(memory, wide));
        QVERIFY(!commandCannotMerge(memory, memory.macro({wide})));
        const Word inner = memory.macro(std::vector<Word>(2048, leaf));
        std::vector<Word> outer(2048, leaf); outer.back() = inner;
        QVERIFY(commandCannotMerge(memory, memory.macro(outer)));
        outer.insert(outer.begin(), leaf);
        QVERIFY(!commandCannotMerge(memory, memory.macro(outer)));
    }
};
QTEST_APPLESS_MAIN(NativeHistoryGuardTest)
#include "NativeHistoryGuardTest.moc"
