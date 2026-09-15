#pragma once
#include <QByteArray>
#include <QVector>
#include <memory>

namespace RePaperNative {
// Ephemeral, process-local identities. Retaining the native shared ownership
// prevents an erased command/control block from being reused during a popup.
// These records are never serialized or used outside the exact native process.
struct NativeHistoryCommand {
    quint64 identity = 0, control = 0, type = 0;
    QByteArray structure;
    bool cannotMerge = false;
    std::shared_ptr<void> retained;
    QVector<std::shared_ptr<void>> retainedChildren;
    friend bool operator==(const NativeHistoryCommand &a, const NativeHistoryCommand &b) {
        return a.identity == b.identity && a.control == b.control && a.type == b.type
            && a.structure == b.structure && a.cannotMerge == b.cannotMerge;
    }
};
struct NativeHistorySnapshot {
    bool valid = false, appendIsolated = false;
    quint64 historyIdentity = 0;
    QVector<NativeHistoryCommand> undo, redo;
};
inline bool sameNativeHistory(const NativeHistorySnapshot &a, const NativeHistorySnapshot &b) {
    return a.valid && b.valid && a.historyIdentity && a.historyIdentity == b.historyIdentity
        && a.appendIsolated == b.appendIsolated && a.undo == b.undo && a.redo == b.redo;
}
inline bool nativeHistoryAppended(const NativeHistorySnapshot &before,
                                 const NativeHistorySnapshot &after, int expectedCount = 1) {
    if (!before.valid || !after.valid || !before.appendIsolated || !before.historyIdentity
        || before.historyIdentity != after.historyIdentity || expectedCount < 1 || expectedCount > 512
        || after.undo.size() != before.undo.size() + expectedCount || !after.redo.isEmpty()) return false;
    for (qsizetype i = 0; i < before.undo.size(); ++i)
        if (!(before.undo[i] == after.undo[i])) return false;
    return true;
}
}
