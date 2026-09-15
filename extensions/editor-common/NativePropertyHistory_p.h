#pragma once
#include "NativeHistorySnapshot.h"
#include "NativeHistoryGuard.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>
#include <limits>
#include <set>

namespace RePaperNative::HistoryDetail {
constexpr qsizetype MaxSnapshotCommands = 32768;

// Exact ferrari/3.28.0.169 only. Call under the native Page lock, after the
// executable gate. All lists contain 16-byte native shared_ptr entries.
template<class Reader>
bool readSnapshot(const Reader &memory, quintptr scene, NativeHistorySnapshot &out) {
    out = {};
    quintptr history = 0, owner = 0; quint8 executing = 0;
    if (!memory.read(scene + 0x348, history) || history % 8
        || !memory.contains(history, 0x50, true)
        || !memory.read(history + 0x10, owner) || owner != scene
        || !memory.read(history + 0x48, executing) || executing) return false;
    NativeHistorySnapshot snapshot;
    snapshot.historyIdentity = history;
    qsizetype remaining = MaxSnapshotCommands;
    qsizetype remainingNodes = MaxSnapshotCommands * 2;
    std::set<quintptr> rootIdentities;
    const auto list = [&](quintptr address, qint64 limit, quintptr &data, qint64 &count) {
        quintptr allocation = 0;
        return memory.read(address, allocation) && memory.read(address + 8, data)
            && memory.read(address + 16, count) && count >= 0 && count <= limit
            && (!allocation || (!(allocation % 8) && memory.contains(allocation, 16)))
            && (!count || (!(data % 8) && memory.contains(data, quintptr(count) * 16)));
    };
    const auto retain = [&](quintptr entry, NativeHistoryCommand &command) {
        qint32 strong = 0, weak = 0; quintptr controlTable = 0;
        if (!memory.read(entry, command.identity) || !memory.read(entry + 8, command.control)
            || !command.identity || command.identity % 8 || !command.control || command.control % 8
            || !memory.read(command.identity, command.type) || command.type % 8
            || !memory.contains(command.type, 0x30)
            || !memory.read(command.control, controlTable) || !memory.contains(controlTable, 0x20)
            || !memory.read(command.control + 8, strong) || !memory.read(command.control + 12, weak)
            || strong < 1 || weak < 1 || strong >= std::numeric_limits<qint32>::max() - MaxSnapshotCommands)
            return false;
        command.retained = *reinterpret_cast<const std::shared_ptr<void>*>(entry);
        return command.retained && quintptr(command.retained.get()) == command.identity;
    };
    const auto readCommands = [&](quintptr address, QVector<NativeHistoryCommand> &commands) {
        quintptr data = 0; qint64 count = 0;
        if (!list(address, remaining, data, count)) return false;
        remaining -= count; commands.reserve(count);
        for (qint64 i = 0; i < count; ++i) {
            NativeHistoryCommand command;
            if (!retain(data + quintptr(i) * 16, command)
                || !rootIdentities.insert(command.identity).second) return false;
            // Grouping can replace a macro's children without replacing its
            // root. Record the complete bounded tree, not only the tail ptr.
            QByteArray structure; QDataStream stream(&structure, QIODevice::WriteOnly);
            QVector<QPair<quintptr, qsizetype>> pending{{data + quintptr(i) * 16, 0}};
            std::set<quintptr> seen;
            for (qsizetype n = 0; n < pending.size(); ++n) {
                if (!remainingNodes-- || pending.size() > MaxMacroChildren
                    || pending[n].second > qsizetype(MaxMacroDepth)) return false;
                NativeHistoryCommand node;
                if (!retain(pending[n].first, node) || !seen.insert(node.identity).second) return false;
                if (n) command.retainedChildren.append(node.retained);
                stream << node.identity << node.control << node.type << qint64(pending[n].second);
                if (node.type != MacroVtable) continue;
                quintptr children = 0; qint64 childCount = 0;
                if (!list(node.identity + 8, MaxMacroChildren - pending.size(), children, childCount)) return false;
                stream << childCount;
                for (qint64 child = 0; child < childCount; ++child)
                    pending.append({children + quintptr(child) * 16, pending[n].second + 1});
            }
            command.structure = QCryptographicHash::hash(structure, QCryptographicHash::Sha256);
            command.cannotMerge = commandCannotMerge(memory, command.identity);
            commands.append(std::move(command));
        }
        return true;
    };
    if (!readCommands(history + 0x18, snapshot.undo) || !readCommands(history + 0x30, snapshot.redo)) return false;
    snapshot.valid = true;
    snapshot.appendIsolated = snapshot.undo.isEmpty() || snapshot.undo.last().cannotMerge;
    out = std::move(snapshot);
    return true;
}
}
