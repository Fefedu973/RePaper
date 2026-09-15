#pragma once
#include <QtGlobal>
#include <algorithm>
#include <array>

namespace RePaperNative::HistoryDetail {
constexpr quintptr NonMergingCommand = 0x496520;
constexpr quintptr MacroVtable = 0x1680820;
constexpr quintptr MacroMerge = 0xe4efc0;
constexpr quintptr TextInsertVtable = 0x1683188;
constexpr quintptr TextInsertMerge = 0xef64c0;
constexpr size_t MaxMacroDepth = 64;
constexpr qint64 MaxMacroChildren = 4096;

// Exact ferrari/3.28.0.169 only; callers enforce the executable/ABI gate and
// hold the native Page lock. Reader supplies mapped contains() and read().
// Native history asks its previous command to merge the incoming edit. A
// SceneMacroAction delegates that question to its last child, recursively;
// recognizing only the direct false function rejects our own grouped edits.
// Never invoke merge to probe it: unknown implementations can change history.
template<class Reader>
bool commandCannotMergeImpl(const Reader &memory, quintptr command, bool insertingNativeLines) {
    std::array<quintptr, MaxMacroDepth> visited{};
    size_t depth = 0;
    qint64 remainingChildren = MaxMacroChildren;
    for (;;) {
        quintptr table = 0, merge = 0;
        if (command % 8 || !memory.contains(command, 8) || !memory.read(command, table)
            || table % 8 || !memory.contains(table, 0x30)
            || !memory.read(table + 0x28, merge)) return false;
        if (merge == NonMergingCommand) return true;
        // ec7130 submits SceneDeleteAppendItemsCrdt (vtable 0x1680e98).
        // SceneTextInsertAction::merge (ef64c0..ef6528) first dynamic_casts
        // that incoming command to SceneTextInsertAction and returns false
        // before any mutation when the cast fails. The native line command's
        // RTTI chain is SceneDeleteAppendItemsCrdt -> SceneSwapItemsCrdt ->
        // SceneAction, so it cannot pass that cast. This exception is specific
        // to line insertion; text edits and property rollback keep the strict
        // guard because another incoming text command could merge here.
        if (insertingNativeLines && table == TextInsertVtable && merge == TextInsertMerge) return true;
        if (table != MacroVtable || merge != MacroMerge || depth == MaxMacroDepth
            || std::find(visited.begin(), visited.begin() + depth, command) != visited.begin() + depth
            || !memory.contains(command, 0x20)) return false;
        visited[depth++] = command;

        // Macro+8 is the native QList of 16-byte shared_ptr entries. Validate
        // the complete bounded list before following the final entry consumed
        // by e4efc0. An empty macro returns false without delegating.
        quintptr array = 0, data = 0;
        qint64 count = 0;
        if (!memory.read(command + 8, array) || !memory.read(command + 0x10, data)
            || !memory.read(command + 0x18, count) || count < 0 || count > remainingChildren
            || (array && (array % 8 || !memory.contains(array, 16)))) return false;
        if (!count) return true;
        if (data % 8 || !memory.contains(data, quintptr(count) * 16)
            || !memory.read(data + quintptr(count - 1) * 16, command)) return false;
        remainingChildren -= count;
    }
}

template<class Reader>
bool commandCannotMerge(const Reader &memory, quintptr command) {
    return commandCannotMergeImpl(memory, command, false);
}

template<class Reader>
bool commandCannotMergeNativeLineInsertion(const Reader &memory, quintptr command) {
    return commandCannotMergeImpl(memory, command, true);
}
}
