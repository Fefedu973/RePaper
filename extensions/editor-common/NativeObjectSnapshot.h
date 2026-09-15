#pragma once
#include "Geometry.h"
#include <QByteArray>
#include <QString>
#include <QVector>
#include "NativeHistorySnapshot.h"
#include <algorithm>

namespace RePaperNative {
struct NativeObjectLine {
    quint64 id = 0;
    quint64 parentId = 0;
    // The real persisted relocation field, or the current ID before the first
    // relocation. This is supplied by the exact native adapter, never invented.
    quint64 lineageId = 0;
    QByteArray version;
    // Full native content with relocation origin normalized to lineageId.
    // Undo reinserts ink under fresh CRDT IDs; strict version remains unchanged.
    QByteArray contentVersion;
    int tool = 0;
    quint16 maximumPointWidth = 0;
    bool uniformPointWidth = false;
    PaperDrawing::Stroke stroke; // Actual active geometry in scene coordinates.
};
struct NativeObjectSnapshot {
    QString documentId, pageId;
    int layer = -1;
    quint64 layerId = 0;
    // Process-local scene identity for guarded two-phase insertions. No native
    // addresses or history records are serialized into notebook metadata.
    quint64 sceneIdentity = 0;
    int unsupportedItemCount = 0;
    bool complete = false;
    bool pendingEditIdentity = false;
    // Every native selected entry and UI record is a supported active line,
    // and both representations agree exactly. Absence of proof stays false.
    bool nativeSelectionExact = false;
    QVector<NativeObjectLine> lines;
    QVector<quint64> selectedIds;
    QByteArray fingerprint;
    QString reason;
    NativeHistorySnapshot history;
};
inline bool sameNativeObjectContent(const NativeObjectSnapshot &a, const NativeObjectSnapshot &b) {
    if (!a.complete || !b.complete || !a.pendingEditIdentity || !b.pendingEditIdentity
        || a.documentId.isEmpty() || a.pageId.isEmpty() || a.layer < 0 || !a.layerId
        || a.documentId != b.documentId || a.pageId != b.pageId || a.layer != b.layer
        || a.layerId != b.layerId || a.lines.size() != b.lines.size()) return false;
    quint64 previous = 0;
    for (qsizetype i = 0; i < a.lines.size(); ++i) {
        const auto &left = a.lines[i]; const auto &right = b.lines[i];
        if (!left.id || left.id <= previous || !left.parentId || !left.lineageId || left.version.isEmpty()
            || left.id != right.id || left.parentId != right.parentId
            || left.lineageId != right.lineageId || left.version != right.version) return false;
        previous = left.id;
    }
    return true;
}
inline bool nativeObjectContentRestored(const NativeObjectSnapshot &a, const NativeObjectSnapshot &b) {
    if (!sameNativeObjectContent(a, a) || !sameNativeObjectContent(b, b)
        || a.documentId != b.documentId || a.pageId != b.pageId || a.layer != b.layer
        || a.layerId != b.layerId || a.lines.size() != b.lines.size()) return false;
    auto left = a.lines, right = b.lines;
    const auto order = [](const NativeObjectLine &x, const NativeObjectLine &y) {
        if (x.parentId != y.parentId) return x.parentId < y.parentId;
        if (x.lineageId != y.lineageId) return x.lineageId < y.lineageId;
        return x.contentVersion < y.contentVersion;
    };
    std::sort(left.begin(), left.end(), order); std::sort(right.begin(), right.end(), order);
    for (qsizetype i = 0; i < left.size(); ++i)
        if (left[i].contentVersion.isEmpty() || left[i].contentVersion != right[i].contentVersion
            || left[i].parentId != right[i].parentId || left[i].lineageId != right[i].lineageId) return false;
    return true;
}
}
