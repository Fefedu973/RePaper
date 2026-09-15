#include "NativeObjectAccess.h"
#include "NativeHistoryGuard.h"
#include "NativePropertyHistory_p.h"
#include "NativeMemoryRanges_p.h"
#include "NativeSelectionColor.h"
#include "NativeStrokeColor.h"
#include "TargetProfile.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QElapsedTimer>
#include <QFile>
#include <QMetaMethod>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QTransform>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <set>
#include <unordered_set>
#include <utility>
#include <vector>

using RePaperNative::NativeObjectLine;
using RePaperNative::NativeObjectSnapshot;

QString RePaperNative::ObjectAccessDetail::insertionBaselineError(
    const NativeObjectSnapshot &expected, const NativeObjectSnapshot &actual) {
    if (!expected.complete || !actual.complete || !expected.pendingEditIdentity || !actual.pendingEditIdentity
        || !expected.sceneIdentity || !actual.sceneIdentity
        || expected.fingerprint.isEmpty() || actual.fingerprint.isEmpty()
        || expected.unsupportedItemCount < 0 || actual.unsupportedItemCount < 0
        || !sameNativeObjectContent(expected, expected) || !sameNativeObjectContent(actual, actual)
        || !expected.history.valid || !actual.history.valid
        || !expected.history.historyIdentity || !actual.history.historyIdentity)
        return QStringLiteral("insertion-baseline-incomplete");
    if (expected.sceneIdentity != actual.sceneIdentity)
        return QStringLiteral("insertion-scene-changed");
    if (!expected.nativeSelectionExact || !actual.nativeSelectionExact
        || expected.selectedIds != actual.selectedIds)
        return QStringLiteral("insertion-selection-changed");
    if (!sameNativeObjectContent(expected, actual) || expected.fingerprint != actual.fingerprint
        || expected.unsupportedItemCount != actual.unsupportedItemCount)
        return QStringLiteral("insertion-snapshot-changed");
    if (!sameNativeHistory(expected.history, actual.history))
        return QStringLiteral("insertion-history-changed");
    return {};
}

QString RePaperNative::ObjectAccessDetail::selectionRequestError(
    const NativeObjectSnapshot &expected, const NativeObjectSnapshot &actual,
    const QVector<quint64> &exactIds) {
    if (!expected.complete || !actual.complete || !expected.pendingEditIdentity || !actual.pendingEditIdentity
        || expected.fingerprint.isEmpty() || expected.documentId.isEmpty() || expected.pageId.isEmpty()
        || expected.layer < 0 || !expected.layerId)
        return QStringLiteral("incomplete-baseline");
    if (expected.documentId != actual.documentId || expected.pageId != actual.pageId
        || expected.layer != actual.layer || expected.layerId != actual.layerId
        || (expected.sceneIdentity && expected.sceneIdentity != actual.sceneIdentity)
        || expected.fingerprint != actual.fingerprint)
        return QStringLiteral("snapshot-changed");
    if (exactIds.size() > 128) return QStringLiteral("selection-limit");
    std::set<quint64> live, requested;
    for (const auto &line : actual.lines) {
        if (!line.id || !line.parentId || !line.lineageId || line.version.isEmpty()
            || !live.insert(line.id).second) return QStringLiteral("invalid-active-identities");
    }
    for (const auto id : exactIds)
        if (!id || !requested.insert(id).second || live.find(id) == live.end())
            return QStringLiteral("missing-or-duplicate-selection-id");
    return {};
}

RePaperNative::ObjectAccessDetail::SelectionResult
RePaperNative::ObjectAccessDetail::runSelection(const NativeObjectSnapshot &expected,
    const QVector<quint64> &exactIds, const SelectionOperations &op) {
    bool touched = false, restorationStarted = false;
    try {
        if (op.cancelled()) return {false, false, QStringLiteral("cancelled-before-selection")};
        const auto problem = selectionRequestError(expected, op.observe(), exactIds);
        if (!problem.isEmpty()) return {false, false, problem};
        if (!op.prepare()) return {false, false, QStringLiteral("selection-preparation-failed")};
        if (op.cancelled()) return {false, false, QStringLiteral("cancelled-before-selection")};
        touched = true;
        op.apply();
        if (!op.verify()) {
            restorationStarted = true;
            op.restore();
            return {false, true, QStringLiteral("selection-verification-failed")};
        }
        return {true, true, {}};
    } catch (...) {
        // Selection has no content-history command. Restoration uses retained
        // native lists and prebuilt records; allocation failure is not claimed
        // to be a universal atomic rollback contract.
        if (touched && !restorationStarted) { try { op.restore(); } catch (...) {} }
        return {false, touched, QStringLiteral("selection-exception")};
    }
}

RePaperNative::ObjectAccessDetail::PropertyCancelResult
RePaperNative::ObjectAccessDetail::runPropertyCancel(const NativeObjectSnapshot &baseline,
    const NativeObjectSnapshot &expectedCurrent, const PropertyCancelOperations &op) {
    PropertyCancelResult result;
    const auto fail = [&](const char *reason) {
        result.reason = QString::fromLatin1(reason); return result;
    };
    try {
        if (op.cancelled()) return fail("property-session-cancelled-before-access");
        auto current = op.observe();
        if (!sameNativeObjectContent(expectedCurrent, current)
            || expectedCurrent.fingerprint.isEmpty() || expectedCurrent.fingerprint != current.fingerprint)
            return fail("property-session-content-changed");
        if (!sameNativeHistory(expectedCurrent.history, current.history))
            return fail("property-session-current-history-changed");
        const auto &checkpoint = baseline.history;
        if (!sameNativeObjectContent(baseline, baseline) || !checkpoint.valid
            || checkpoint.historyIdentity != current.history.historyIdentity
            || baseline.documentId != current.documentId || baseline.pageId != current.pageId
            || baseline.layer != current.layer || baseline.layerId != current.layerId
            || checkpoint.undo.size() > current.history.undo.size())
            return fail("property-session-checkpoint-invalid");
        const auto count = current.history.undo.size() - checkpoint.undo.size();
        if (count > MaxPropertyCancelCommands) return fail("property-session-history-budget");
        for (qsizetype i = 0; i < checkpoint.undo.size(); ++i)
            if (!(checkpoint.undo[i] == current.history.undo[i])) return fail("property-session-prefix-changed");
        if ((!count && !sameNativeHistory(checkpoint, current.history))
            || (count && (!checkpoint.appendIsolated || !current.history.redo.isEmpty())))
            return fail("property-session-history-changed");
        const auto clearSelection = [&]() {
            if (current.nativeSelectionExact && current.selectedIds.isEmpty()) return true;
            const auto before = current;
            result.touched = true;
            if (!op.clearSelection()) return false;
            current = op.observe();
            return sameNativeObjectContent(before, current) && sameNativeHistory(before.history, current.history)
                && current.nativeSelectionExact && current.selectedIds.isEmpty();
        };
        for (qsizetype remaining = count; ; --remaining) {
            if (op.cancelled()) return fail("property-session-cancel-interrupted");
            if (!clearSelection()) return fail("property-session-selection-clear-failed");
            if (!remaining) break;
            // Reobserve immediately before each call. A matching depth alone
            // never grants permission to consume the native history tail.
            const auto verified = op.observe();
            if (!sameNativeObjectContent(current, verified)
                || !sameNativeHistory(current.history, verified.history)
                || !verified.nativeSelectionExact || !verified.selectedIds.isEmpty())
                return fail("property-session-history-changed-during-cancel");
            auto nextHistory = verified.history;
            const auto command = nextHistory.undo.takeLast();
            nextHistory.redo.append(command);
            nextHistory.appendIsolated = nextHistory.undo.isEmpty() || nextHistory.undo.last().cannotMerge;
            if (op.cancelled()) return fail("property-session-cancel-interrupted");
            result.touched = true;
            if (!op.undo(command)) return fail("property-session-undo-unconfirmed");
            ++result.undone;
            current = op.observe();
            if (!sameNativeHistory(nextHistory, current.history)
                || !sameNativeObjectContent(current, current)
                || current.documentId != baseline.documentId || current.pageId != baseline.pageId
                || current.layer != baseline.layer || current.layerId != baseline.layerId)
                return fail("property-session-undo-verification-failed");
        }
        if (!nativeObjectContentRestored(baseline, current)
            || !current.nativeSelectionExact || !current.selectedIds.isEmpty()
            || current.history.undo != checkpoint.undo)
            return fail("property-session-restoration-unconfirmed");
        result.cancelled = true; result.snapshot = std::move(current);
        return result;
    } catch (...) { return fail("property-session-cancel-exception"); }
}

#if defined(__aarch64__) && defined(REPAPER_WITH_NATIVE_ABI)
#include "rm_Line.hpp"
#include "rm_SceneItem.hpp"
#include <pthread.h>
class Scene;
namespace {
using Word = quintptr;
using ItemPtr = std::shared_ptr<SceneItem>;
using Items = QList<ItemPtr>;
using IdSet = std::unordered_set<quint64>;
constexpr qsizetype MaxItems = 128, MaxLines = 8192, MaxPoints = 1000000, MaxEntries = 200000;
constexpr qsizetype MaxNodes = 4096, MaxBuckets = 32768, MaxPages = 65536, MaxHistory = 1000000;
constexpr quint64 CounterMask = 0xffffffffffffULL;
static_assert(sizeof(Word) == 8 && sizeof(Items) == 24 && sizeof(Line) == 0x58);
static_assert(sizeof(LinePoint) == 0xe && sizeof(std::function<bool(Scene*)>) == 32);
static_assert(sizeof(pthread_mutex_t) == 0x30 && sizeof(IdSet) == 0x38);
#include "NativeObjectAccessMemory_p.h"

bool finite(QPointF p) {
    return std::isfinite(p.x()) && std::isfinite(p.y())
        && std::abs(p.x()) <= 1000000 && std::abs(p.y()) <= 1000000;
}
bool finite(QRectF r) {
    return finite(r.topLeft()) && finite(r.bottomRight()) && r.width() >= 0 && r.height() >= 0;
}
bool insertable(int tool) { return !(tool >= 8 && tool <= 11) && tool != 22; }

struct AccessContext {
    QString documentId, pageId;
    int layer = -1;
    quint64 layerId = 0;
    Word scene = 0;
};
struct LiveLine { NativeObjectLine value; Word address = 0; };
struct LiveNode {
    quint64 id = 0;
    Word address = 0;
    QPointF offset;
    QVector<quint64> selected;
};
struct ActiveSnapshot {
    NativeObjectSnapshot value;
    QVector<LiveLine> lines;
    QVector<LiveNode> nodes;
    bool foreignSelection = false;
};

bool resolveContext(QObject *controller, const QVariantMap &supplied,
                    AccessContext &context, QString &reason) {
    const auto reject = [&](const char *code) { reason = QString::fromLatin1(code); return false; };
    context.documentId = supplied.value("documentId").toString();
    context.pageId = supplied.value("pageId").toString();
    bool layerOk = false;
    context.layer = supplied.value("layer").toInt(&layerOk);
    if (!layerOk || context.layer < 0 || context.documentId.isEmpty() || context.documentId.size() > 128
        || context.pageId.isEmpty() || context.pageId.size() > 128
        || controller->property("pageId").toString() != context.pageId)
        return reject("page-context-mismatch");
    const Mappings maps;
    const Word controllerAddress = reinterpret_cast<Word>(controller);
    qsizetype layerCount = 0; Word layers = 0;
    // This is the controller's GUI-owned layer cache, not the native Scene.
    // Capture only the selected layer identity so a queued layer reorder is
    // rejected on the worker. No PageData traversal or Page lock occurs here.
    if (!maps.read(controllerAddress + 0x1a8, layerCount) || layerCount <= context.layer
        || layerCount > MaxPages || !maps.read(controllerAddress + 0x1a0, layers)
        || !maps.contains(layers, Word(layerCount) * 0x28)
        || !maps.read(layers + Word(context.layer) * 0x28 + 0x20, context.layerId)
        || !(context.layerId & CounterMask)) return reject("invalid-layer");
    return true;
}

// The exact DocumentWorker type-27 callback supplies this Scene while retaining
// its Page and owning the native Page lock. Resolve it here rather than racing
// the worker's page cache from the GUI. Every later observation in this callback
// retains the same concrete scene/layer identities.
bool resolveWorkerScene(AccessContext &context, Scene *scene, QString &reason) {
    const Mappings maps;
    const Word address = reinterpret_cast<Word>(scene);
    Word table = 0;
    if (!maps.contains(address, 0x368, true) || !maps.read(address, table) || table != 0x167f620) {
        reason = QStringLiteral("invalid-page-scene"); return false;
    }
    using LayerId = quint64 (*)(Scene*, int);
    if (reinterpret_cast<LayerId>(Word(0xe46220))(scene, context.layer) != context.layerId) {
        reason = QStringLiteral("layer-changed"); return false;
    }
    context.scene = address;
    return true;
}

bool readLine(const Mappings &maps, Word address, QPointF offset,
              qsizetype &totalPoints, NativeObjectLine &out, const quint64 *versionOrigin = nullptr) {
    Word table = 0, array = 0, data = 0; qsizetype count = 0;
    quint8 tag = 0; quint64 origin = 0, timestamp = 0, renderGroup = 0;
    if (!maps.contains(address, 0xb0) || !maps.read(address, table) || table != 0x1682940
        || !maps.read(address + 8, tag) || tag != 3
        || !maps.read(address + 0x10, out.id) || !maps.read(address + 0x18, out.parentId)
        || !maps.read(address + 0x20, origin) || !maps.read(address + 0xa0, timestamp)
        || !maps.read(address + 0xa8, renderGroup)
        || !maps.read(address + 0x58, array) || !maps.read(address + 0x60, data)
        || !maps.read(address + 0x68, count) || count < 1 || count > MaxPoints - totalPoints
        || (array && !maps.contains(array, 16))
        || !maps.contains(data, Word(count) * sizeof(LinePoint))) return false;
    const auto *line = reinterpret_cast<const Line*>(address + 0x48);
    if (!std::isfinite(line->maskScale) || !std::isfinite(line->thickness)
        || !finite(line->bounds) || !finite(offset)) return false;
    out.tool = line->tool;
    out.lineageId = (origin & CounterMask) ? origin : out.id;
    out.stroke.color = nativeStrokeColor(line->color, line->rgba);
    if (!out.stroke.color.isValid()) return false;
    out.stroke.width = 0;
    out.uniformPointWidth = true;
    quint16 firstWidth = 0;
    out.stroke.points.reserve(count);
    for (qsizetype i = 0; i < count; ++i) {
        LinePoint point{};
        // The complete immutable QList point allocation was range-checked
        // above while the native Page lock is owned. Rechecking process maps
        // for every 14-byte sample added work proportional to points × maps.
        std::memcpy(&point,reinterpret_cast<const void*>(data + Word(i) * sizeof(LinePoint)),sizeof(point));
        const QPointF position = QPointF(point.x, point.y) + offset;
        if(i==0)firstWidth=point.width;
        if(point.width!=firstWidth||!point.width)out.uniformPointWidth=false;
        if (!finite(position)) return false;
        out.stroke.points.append(position);
        out.maximumPointWidth = qMax(out.maximumPointWidth, point.width);
        if (out.stroke.width == 0 && point.width) out.stroke.width = double(point.width) / 4.0;
    }
    // Version contains full native point/style/relocation data, excluding the
    // mutable bounds cache. Group movement is included through the scene offset.
    const auto version = [&](quint64 normalizedOrigin) {
        QByteArray fields; QDataStream stream(&fields, QIODevice::WriteOnly);
        stream << qint32(line->tool) << qint32(line->color) << quint32(line->rgba)
               << line->maskScale << line->thickness << normalizedOrigin
               << timestamp << renderGroup << offset;
        QCryptographicHash hash(QCryptographicHash::Sha256);
        hash.addData(fields);
        hash.addData(QByteArrayView(reinterpret_cast<const char*>(data), count * qsizetype(sizeof(LinePoint))));
        return hash.result();
    };
    out.version = version(versionOrigin ? *versionOrigin : origin);
    out.contentVersion = version(out.lineageId);
    totalPoints += count;
    return true;
}

QByteArray snapshotFingerprint(const NativeObjectSnapshot &snapshot, const QByteArray &selectedEntryIdentity) {
    QByteArray bytes; QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream << snapshot.documentId << snapshot.pageId << qint32(snapshot.layer) << snapshot.layerId
           << snapshot.pendingEditIdentity << snapshot.nativeSelectionExact << qint64(snapshot.lines.size())
           << qint32(snapshot.unsupportedItemCount);
    for (const auto &line : snapshot.lines)
        stream << line.id << line.parentId << line.lineageId << line.version;
    stream << snapshot.selectedIds << selectedEntryIdentity;
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}

bool selectionRecordsExact(const Mappings &maps, Word scene, ActiveSnapshot &snapshot);

bool observeActive(const AccessContext &context, Scene *nativeScene, ActiveSnapshot &snapshot) {
    const auto reject = [&](const char *code) {
        snapshot.value.complete = false; snapshot.value.reason = QString::fromLatin1(code); return false;
    };
    snapshot = {};
    auto &out = snapshot.value;
    out.documentId = context.documentId; out.pageId = context.pageId;
    out.layer = context.layer; out.layerId = context.layerId; out.pendingEditIdentity = true;
    bool selectedEntriesExact = true;
    const Mappings maps;
    const Word scene = reinterpret_cast<Word>(nativeScene);
    Word table = 0;
    if (scene != context.scene || !maps.contains(scene, 0x368, true)
        || !maps.read(scene, table) || table != 0x167f620) return reject("scene-changed");
    out.sceneIdentity = scene;
    using LayerId = quint64 (*)(Scene*, int);
    if (reinterpret_cast<LayerId>(Word(0xe46220))(nativeScene, context.layer) != context.layerId)
        return reject("layer-changed");
    QList<HashPair> registry;
    if (!hashPairs(maps, scene + 0x248, registry)) return reject("invalid-node-registry");
    Word root = 0;
    for (const auto &node : registry) if (node.key == context.layerId) root = node.value;
    if (!root) return reject("layer-node-missing");
    QVector<HashPair> pending{{context.layerId, root}};
    std::set<Word> seenNodes, seenEntries;
    std::set<quint64> liveIds, lineIds, selectedIds;
    qsizetype points = 0;
    QByteArray selectedEntryIdentity;
    QDataStream selectedIdentity(&selectedEntryIdentity, QIODevice::WriteOnly);
    for (qsizetype index = 0; index < pending.size(); ++index) {
        if (pending.size() > MaxNodes) return reject("node-budget");
        const auto candidate = pending[index];
        LiveNode node; node.id = candidate.key; node.address = candidate.value;
        quint64 actualId = 0, marker = 0; Word tree = 0;
        if (!seenNodes.insert(node.address).second || !maps.contains(node.address, 0x1e8, true)
            || !maps.read(node.address + 0x18, actualId) || actualId != node.id
            || !maps.read(node.address + 0x190, tree) || tree != scene + 0x188
            || !maps.read(node.address + 0x138, marker)) return reject("invalid-active-node");
        if ((marker & CounterMask) && !maps.read(node.address + 0x1b8, node.offset))
            return reject("invalid-node-offset");
        if (!finite(node.offset)) return reject("invalid-node-offset");
        const Word sentinel = node.address + 0x20;
        Word entry = 0, previous = sentinel;
        if (!maps.read(sentinel, entry)) return reject("invalid-crdt-head");
        while (entry != sentinel) {
            if (seenEntries.size() >= size_t(MaxEntries) || !seenEntries.insert(entry).second
                || !maps.contains(entry, 0x40)) return reject("invalid-crdt-chain");
            Word next = 0, back = 0; quint32 deleted = 0;
            if (!maps.read(entry, next) || !maps.read(entry + 8, back) || back != previous
                || !maps.read(entry + 0x38, deleted)) return reject("invalid-crdt-links");
            if (!deleted) {
                Word item = 0; quint64 id = 0, parent = 0, entryId = 0; quint8 tag = 0;
                if (!maps.read(entry + 0x28, item) || !maps.contains(item, 0x48)
                    || !maps.read(entry + 0x10, entryId) || !maps.read(item + 0x10, id)
                    || id != entryId || !(id & CounterMask) || !liveIds.insert(id).second
                    || !maps.read(item + 0x18, parent) || parent != node.id
                    || !maps.read(item + 8, tag)) return reject("invalid-active-item");
                if (tag == 3) {
                    LiveLine line; line.address = item;
                    if (snapshot.lines.size() >= MaxLines || !readLine(maps, item, node.offset, points, line.value))
                        return reject("invalid-or-oversized-line");
                    lineIds.insert(id); snapshot.lines.append(std::move(line));
                } else if (tag == 2) {
                    Word child = 0, childTable = 0; quint64 childId = 0;
                    if (!maps.read(item, childTable) || childTable != 0x16804a8
                        || !maps.read(item + 0x48, childId) || !(childId & CounterMask)
                        || !maps.read(item + 0x50, child) || !child)
                        return reject("invalid-child-group");
                    const auto found = std::find_if(registry.cbegin(), registry.cend(),
                        [&](const auto &registered) { return registered.key == childId && registered.value == child; });
                    if (found == registry.cend()) return reject("unregistered-child-group");
                    pending.append({childId, child});
                } else ++out.unsupportedItemCount;
            }
            previous = entry; entry = next;
        }
        Word tail = 0;
        if (!maps.read(sentinel + 8, tail) || tail != previous) return reject("invalid-crdt-tail");
        qsizetype selectedCount = 0; Word selected = 0;
        if (!listHeader(maps, node.address + 0x1d0, MaxItems - out.selectedIds.size(), selectedCount, selected))
            return reject("invalid-selected-list");
        selectedIdentity << node.id << qint64(selectedCount);
        for (qsizetype i = 0; i < selectedCount; ++i) {
            Word item = 0; quint64 id = 0, parent = 0;
            if (!maps.read(selected + Word(i) * 16, item) || !maps.read(item + 0x10, id)
                || !maps.read(item + 0x18, parent)) return reject("invalid-selected-item");
            selectedIdentity << quint64(item) << id << parent;
            // Undo/affine work can leave old selected pointers. They do not
            // invalidate the complete active traversal and are never reported
            // as live membership. A later exact-ID select repairs the records.
            if (parent != node.id || !lineIds.count(id)) { selectedEntriesExact = false; continue; }
            const auto live = std::find_if(snapshot.lines.cbegin(), snapshot.lines.cend(),
                [&](const auto &line) { return line.value.id == id && line.address == item; });
            if (live == snapshot.lines.cend()) { selectedEntriesExact = false; continue; }
            if (!selectedIds.insert(id).second) return reject("duplicate-selected-id");
            node.selected.append(id); out.selectedIds.append(id);
        }
        snapshot.nodes.append(std::move(node));
    }
    std::sort(snapshot.lines.begin(), snapshot.lines.end(), [](const auto &a, const auto &b) { return a.value.id < b.value.id; });
    for (const auto &line : std::as_const(snapshot.lines)) out.lines.append(line.value);
    std::sort(out.selectedIds.begin(), out.selectedIds.end());
    const bool recordsExact = selectionRecordsExact(maps, scene, snapshot);
    out.nativeSelectionExact = selectedEntriesExact && recordsExact;
    out.complete = true; out.fingerprint = snapshotFingerprint(out, selectedEntryIdentity);
    // A bounded unavailable history disables popup rollback, not ordinary
    // object observation or editing. Content and history share this Page lock.
    RePaperNative::HistoryDetail::readSnapshot(maps, scene, out.history);
    return true;
}

struct SelectionRecord { quint64 nodeId = 0; IdSet ids; Items clones; QRectF bounds; };
static_assert(sizeof(SelectionRecord) == 0x78);
static_assert(offsetof(SelectionRecord, ids) == 8 && offsetof(SelectionRecord, clones) == 0x40
              && offsetof(SelectionRecord, bounds) == 0x58);
using Records = std::vector<SelectionRecord>;

bool selectionRecordsExact(const Mappings &maps, Word scene, ActiveSnapshot &snapshot) {
    Word state = 0, entry = 0, mapSize = 0;
    if (!maps.read(scene + 0x360, state) || !maps.contains(state, 0x1c8)
        || !maps.read(state + 0x1a0, entry) || !maps.read(state + 0x1a8, mapSize)
        || mapSize > Word(MaxNodes)) return false;
    std::set<Word> entries;
    std::set<quint64> layers, nodes, selected;
    qsizetype points = 0;
    while (entry) {
        Word next = 0, begin = 0, end = 0, capacity = 0; quint64 layer = 0;
        if (!entries.insert(entry).second || entries.size() > size_t(MaxNodes)
            || !maps.read(entry, next) || !maps.read(entry + 8, layer) || !layers.insert(layer).second
            || !maps.read(entry + 0x10, begin) || !maps.read(entry + 0x18, end)
            || !maps.read(entry + 0x20, capacity) || end < begin || capacity < end
            || (end - begin) % sizeof(SelectionRecord)
            || (end - begin) / sizeof(SelectionRecord) > Word(MaxNodes)
            || (end != begin && !maps.contains(begin, end - begin))) return false;
        // A line-only overlay for this layer cannot hide another layer's
        // independently selected content safely.
        if (layer != snapshot.value.layerId && begin != end) {
            snapshot.foreignSelection = true; return false;
        }
        for (Word record = begin; record < end; record += sizeof(SelectionRecord)) {
            quint64 nodeId = 0; Word idEntry = 0, idCount = 0, clones = 0; qsizetype cloneCount = 0;
            if (!maps.read(record, nodeId) || !nodes.insert(nodeId).second
                || !maps.read(record + 0x18, idEntry) || !maps.read(record + 0x20, idCount)
                || !idCount || idCount > Word(MaxItems)
                || !listHeader(maps, record + 0x40, MaxItems, cloneCount, clones)
                || Word(cloneCount) != idCount) return false;
            const auto node = std::find_if(snapshot.nodes.cbegin(), snapshot.nodes.cend(),
                [&](const auto &candidate) { return candidate.id == nodeId; });
            if (node == snapshot.nodes.cend()) return false;
            std::set<Word> idEntries;
            std::set<quint64> ids;
            while (idEntry) {
                Word idNext = 0; quint64 id = 0;
                if (!idEntries.insert(idEntry).second || idEntries.size() > size_t(MaxItems)
                    || !maps.read(idEntry, idNext) || !maps.read(idEntry + 8, id)
                    || !ids.insert(id).second || !selected.insert(id).second) return false;
                idEntry = idNext;
            }
            if (ids.size() != idCount || selected.size() > size_t(MaxItems)
                || ids != std::set<quint64>(node->selected.cbegin(), node->selected.cend())) return false;
            std::set<quint64> matched;
            for (qsizetype i = 0; i < cloneCount; ++i) {
                Word clone = 0; quint64 origin = 0;
                if (!maps.read(clones + Word(i) * 16, clone) || !maps.read(clone + 0x20, origin)) return false;
                bool found = false;
                for (const auto &line : snapshot.lines) {
                    if (!ids.count(line.value.id) || matched.count(line.value.id) || origin != line.value.lineageId) continue;
                    quint64 sourceOrigin = 0; NativeObjectLine copied;
                    if (!maps.read(line.address + 0x20, sourceOrigin)
                        || !readLine(maps, clone, node->offset, points, copied, &sourceOrigin)) return false;
                    if (copied.version != line.value.version) continue;
                    matched.insert(line.value.id); found = true; break;
                }
                if (!found) return false;
            }
        }
        entry = next;
    }
    return entries.size() == mapSize
        && selected == std::set<quint64>(snapshot.value.selectedIds.cbegin(), snapshot.value.selectedIds.cend());
}

struct NodePlan { Word address = 0; IdSet ids; Items previous; };
struct PreparedSelection { std::vector<NodePlan> nodes; Records records; QRectF bounds; };

bool prepareSelection(const ActiveSnapshot &active, const QVector<quint64> &requested,
                      Scene *scene, PreparedSelection &prepared) {
    if (active.foreignSelection) return false;
    const Mappings maps;
    const std::set<quint64> wanted(requested.cbegin(), requested.cend());
    using Clone = ItemPtr (*)(SceneItem*);
    using Bounds = QRectF (*)(SceneItem*);
    using MapBounds = QRectF (*)(Scene*, quint64, QRectF);
    using Selectable = bool (*)(SceneItem*);
    prepared.nodes.reserve(size_t(active.nodes.size()));
    prepared.records.reserve(size_t(active.nodes.size()));
    for (const auto &node : active.nodes) {
        NodePlan plan; plan.address = node.address;
        plan.previous = *reinterpret_cast<const Items*>(node.address + 0x1d0);
        SelectionRecord record; record.nodeId = node.id;
        for (const auto &line : active.lines) {
            if (line.value.parentId != node.id || !wanted.count(line.value.id)) continue;
            auto *original = reinterpret_cast<SceneItem*>(line.address);
            if (!reinterpret_cast<Selectable>(Word(0xed71d0))(original)) return false;
            auto clone = reinterpret_cast<Clone>(Word(0xed7220))(original);
            const Word address = reinterpret_cast<Word>(clone.get());
            Word table = 0;
            if (!address || address == line.address || !maps.read(address, table) || table != 0x1682940) {
                // Native allocation can extend /proc/self/maps. Validate against
                // a fresh mapping before accepting a newly allocated clone.
                const Mappings fresh;
                if (!address || address == line.address || !fresh.read(address, table) || table != 0x1682940) return false;
            }
            const Mappings fresh;
            if (!fresh.contains(address, 0xb0, true)) return false;
            std::memset(reinterpret_cast<void*>(address + 0x10), 0, 16);
            std::memcpy(reinterpret_cast<void*>(address + 0x20), &line.value.lineageId, 8);
            const QRectF bounds = reinterpret_cast<Bounds>(Word(0x6111e0))(clone.get());
            if (!finite(bounds)) return false;
            record.bounds = record.bounds.united(bounds);
            record.clones.append(std::move(clone));
            record.ids.insert(line.value.id); plan.ids.insert(line.value.id);
        }
        prepared.nodes.push_back(std::move(plan));
        if (!record.ids.empty()) {
            const QRectF world = reinterpret_cast<MapBounds>(Word(0xe44fb0))(scene, node.id, record.bounds);
            if (!finite(world)) return false;
            prepared.bounds = prepared.bounds.united(world);
            prepared.records.push_back(std::move(record));
        }
    }
    return true;
}

void installRecords(Scene *scene, const AccessContext &context, Records &records, QRectF bounds, QRectF dirty) {
    auto *state = *reinterpret_cast<void**>(reinterpret_cast<Word>(scene) + 0x360);
    using Install = void (*)(void*, quint64, Records*);
    const bool present = !records.empty();
    if (!present) {
        // edf930 deliberately ignores an empty vector. Its underlying setter
        // f695f0 has the verified erase-this-layer branch at f699a0.
        reinterpret_cast<Install>(Word(0xf695f0))(reinterpret_cast<void*>(reinterpret_cast<Word>(state) + 0x190),
                                                context.layerId, &records);
    } else reinterpret_cast<Install>(Word(0xedf930))(state, context.layerId, &records);
    *reinterpret_cast<quint8*>(reinterpret_cast<Word>(state) + 0x1c8) = 0;
    using Area = void (*)(Scene*, int, QRectF);
    using Presence = void (*)(Scene*, bool);
    using Dirty = void (*)(Scene*, quint64, QRectF);
    reinterpret_cast<Area>(Word(0xe251d0))(scene, context.layer, bounds);
    reinterpret_cast<Presence>(Word(0xe24dc0))(scene, present);
    reinterpret_cast<Dirty>(Word(0xe46b00))(scene, 0, dirty.united(bounds));
}

bool historyReady(const Mappings &maps, Word scene, Word &history, qsizetype &count,
                  bool insertingNativeLines) {
    Word owner = 0, data = 0; quint8 readOnly = 0; unsigned char author[16]{};
    if (!maps.read(scene + 0x348, history) || !maps.contains(history, 0x50, true)
        || !maps.read(history + 0x10, owner) || owner != scene
        || !maps.read(0x1a676b0, readOnly) || (readOnly & 1)
        || !maps.read(scene + 0x26a, author)
        || std::all_of(std::begin(author), std::end(author), [](auto byte) { return byte == 0; })
        || !listHeader(maps, history + 0x18, MaxHistory - 1, count, data)) return false;
    if (count) {
        Word command = 0;
        if (!maps.read(data + Word(count - 1) * 16, command)) return false;
        const bool isolated = insertingNativeLines
            ? RePaperNative::HistoryDetail::commandCannotMergeNativeLineInsertion(maps, command)
            : RePaperNative::HistoryDetail::commandCannotMerge(maps, command);
        if (!isolated) return false;
    }
    return true;
}
}
#endif

// QObject lifetime and the worker receipt are defined below the native-only
// implementation so unsupported host builds still exercise completion races.
struct NativeObjectAccess::Private {
    QPointer<QObject> controller, worker;
    QVariantMap context;
    QString operation, reason;
    NativeObjectSnapshot result;
    QVector<quint64> inserted;
    bool busy = false, completed = false, destroying = false;
    qulonglong jobId = 0;
    QTimer timer;
    QElapsedTimer elapsed;
    QMetaObject::Connection completionConnection, workerDestroyed, controllerDestroyed;
    struct Pending {
        std::atomic_bool cancelled{false};
        std::mutex mutex;
        bool ran = false, success = false;
        QString reason;
        NativeObjectSnapshot result;
        QVector<quint64> inserted;
#if defined(__aarch64__) && defined(REPAPER_WITH_NATIVE_ABI)
        AccessContext context;
        bool requireUnchanged = false;
        NativeObjectSnapshot baseline;
        NativeObjectSnapshot expectedCurrent;
        QVector<quint64> exactIds;
        Items items;
        QPointF center;
#endif
    };
    std::shared_ptr<Pending> pending;
};

NativeObjectAccess::NativeObjectAccess(QObject *parent) : QObject(parent), d(new Private) {
    d->timer.setInterval(50);
    connect(&d->timer, &QTimer::timeout, this, &NativeObjectAccess::checkCompletion);
}
NativeObjectAccess::~NativeObjectAccess() { d->destroying = true; NativeObjectAccess::cancel(); }
bool NativeObjectAccess::busy() const { return d->busy; }
QString NativeObjectAccess::reason() const { return d->reason; }
QString NativeObjectAccess::operation() const { return d->operation; }
NativeObjectSnapshot NativeObjectAccess::result() const { return d->result; }
QVector<quint64> NativeObjectAccess::insertedIds() const { return d->inserted; }
void NativeObjectAccess::cancel() {
    const bool wasBusy = d->busy;
    if (d->pending) d->pending->cancelled.store(true, std::memory_order_release);
    d->timer.stop();
    QObject::disconnect(d->completionConnection);
    QObject::disconnect(d->workerDestroyed);
    QObject::disconnect(d->controllerDestroyed);
    d->completionConnection = {}; d->workerDestroyed = {}; d->controllerDestroyed = {};
    d->controller.clear(); d->worker.clear(); d->pending.reset();
    d->busy = false; d->completed = false; d->jobId = 0;
    if (wasBusy && !d->destroying) emit statusChanged();
}
void NativeObjectAccess::finish(bool success, const QString &reason) {
    d->reason = reason;
    NativeObjectAccess::cancel();
    emit finished(success);
}
bool NativeObjectAccess::inspect(QObject *controller, const QVariantMap &context) {
    return dispatch(controller, context, QStringLiteral("inspect"), {}, {}, {}, {});
}
bool NativeObjectAccess::insert(QObject *controller, const QVariantMap &context, const QVariant &items, QPointF center) {
    return dispatch(controller, context, QStringLiteral("insert"), items, center, {}, {});
}
bool NativeObjectAccess::insertIfUnchanged(QObject *controller, const QVariantMap &context,
    const NativeObjectSnapshot &baseline, const QVariant &items, QPointF center) {
    return dispatch(controller, context, QStringLiteral("insert"), items, center, baseline, {}, {}, true);
}
bool NativeObjectAccess::replace(QObject *controller, const QVariantMap &context,
    const NativeObjectSnapshot &baseline, const QVector<quint64> &exactIds, const QVariant &items, QPointF center) {
    return dispatch(controller, context, QStringLiteral("replace"), items, center, baseline, exactIds);
}
bool NativeObjectAccess::select(QObject *controller, const QVariantMap &context,
    const NativeObjectSnapshot &baseline, const QVector<quint64> &exactIds) {
    return dispatch(controller, context, QStringLiteral("select"), {}, {}, baseline, exactIds);
}
bool NativeObjectAccess::cancelPropertySession(QObject *controller, const QVariantMap &context,
    const NativeObjectSnapshot &baseline, const NativeObjectSnapshot &expectedCurrent) {
    return dispatch(controller, context, QStringLiteral("cancel-properties"), {}, {}, baseline, {}, expectedCurrent);
}

bool NativeObjectAccess::dispatch(QObject *controller, const QVariantMap &context, const QString &operation,
    const QVariant &items, QPointF center, const NativeObjectSnapshot &baseline, const QVector<quint64> &exactIds,
    const NativeObjectSnapshot &expectedCurrent, bool requireUnchanged) {
    const auto reject = [&](const QString &reason) { d->reason = reason; return false; };
    if (d->busy) return reject(QStringLiteral("native-object-operation-busy"));
    if (thread() != QThread::currentThread() || !controller || controller->thread() != QThread::currentThread()
        || QString::fromLatin1(controller->metaObject()->className()) != QStringLiteral("SceneController"))
        return reject(QStringLiteral("native-controller-unavailable"));
    if (requireUnchanged) {
        const auto error = RePaperNative::ObjectAccessDetail::insertionBaselineError(baseline, baseline);
        if (!error.isEmpty()) return reject(error);
    }
#if !defined(__aarch64__) || !defined(REPAPER_WITH_NATIVE_ABI)
    Q_UNUSED(context); Q_UNUSED(operation); Q_UNUSED(items); Q_UNUSED(center); Q_UNUSED(baseline); Q_UNUSED(exactIds); Q_UNUSED(expectedCurrent);
    return reject(QStringLiteral("native-object-access-unavailable-on-host"));
#else
    static const bool target = RePaperNative::matchesRunningXochitl();
    if (!target) return reject(QStringLiteral("native-object-target-mismatch"));
    const auto itemType = QMetaType::fromName("QList<std::shared_ptr<SceneItem>>");
    if (!itemType.isValid() || itemType.sizeOf() != sizeof(Items)
        || QMetaType::fromName("Line").sizeOf() != sizeof(Line))
        return reject(QStringLiteral("native-object-metatype-mismatch"));
    for (const auto *property : {"pageId", "worker", "pendingEdit", "selectionItemCount"})
        if (controller->metaObject()->indexOfProperty(property) < 0)
            return reject(QStringLiteral("native-object-properties-missing"));
    const auto pendingEdit = controller->property("pendingEdit");
    if (!pendingEdit.canConvert<QTransform>() || !pendingEdit.value<QTransform>().isIdentity())
        return reject(QStringLiteral("native-transform-pending"));
    auto *worker = controller->property("worker").value<QObject*>();
    if (!worker || worker->thread() != QThread::currentThread()
        || QString::fromLatin1(worker->metaObject()->className()) != QStringLiteral("DocumentWorker"))
        return reject(QStringLiteral("native-worker-unavailable"));
    const int signal = worker->metaObject()->indexOfSignal("jobCompleted(qulonglong)");
    const int slot = metaObject()->indexOfSlot("jobCompleted(qulonglong)");
    if (signal < 0 || slot < 0) return reject(QStringLiteral("native-job-receipt-unavailable"));
    auto pending = std::make_shared<Private::Pending>();
    QString problem;
    if (!resolveContext(controller, context, pending->context, problem)) return reject(problem);
    if (operation == QStringLiteral("select") || operation == QStringLiteral("replace")
        || operation == QStringLiteral("cancel-properties") || requireUnchanged) {
        if (baseline.documentId != pending->context.documentId || baseline.pageId != pending->context.pageId
            || baseline.layer != pending->context.layer || baseline.layerId != pending->context.layerId)
            return reject(requireUnchanged ? QStringLiteral("insertion-context-mismatch")
                                           : QStringLiteral("selection-context-mismatch"));
        pending->baseline = baseline; pending->exactIds = exactIds;
        pending->expectedCurrent = expectedCurrent;
        pending->requireUnchanged = requireUnchanged;
    }
    if (operation == QStringLiteral("insert") || operation == QStringLiteral("replace")) {
        if (!finite(center) || items.metaType() != itemType) return reject(QStringLiteral("invalid-native-item-batch"));
        pending->items = *reinterpret_cast<const Items*>(items.constData());
        pending->center = center;
        if (pending->items.isEmpty() || pending->items.size() > MaxItems)
            return reject(QStringLiteral("native-insertion-budget"));
    }
    {
        const Mappings maps;
        for (const Word address : {Word(0xad71d0), Word(0xe46220), Word(0xf3f250), Word(0xedf930),
                Word(0xf695f0), Word(0xed7220), Word(0x6111e0), Word(0xe44fb0), Word(0xe251d0),
                Word(0xe24dc0), Word(0xe46b00), Word(0xec7130), Word(0xed7190), Word(0xed71d0), Word(0xe50b20), Word(0xec4180), Word(0xe51880)})
            if (!maps.contains(address, 4, false, true)) return reject(QStringLiteral("native-entrypoint-unavailable"));
    }
    d->controller = controller; d->worker = worker; d->context = context; d->operation = operation;
    d->result = {}; d->inserted.clear(); d->pending = pending; d->busy = true; d->completed = false;
    d->completionConnection = QObject::connect(worker, worker->metaObject()->method(signal),
        this, metaObject()->method(slot), Qt::QueuedConnection);
    d->workerDestroyed = connect(worker, &QObject::destroyed, this, &NativeObjectAccess::workerUnavailable);
    d->controllerDestroyed = connect(controller, &QObject::destroyed, this, &NativeObjectAccess::controllerUnavailable);
    if (!d->completionConnection) { cancel(); return reject(QStringLiteral("native-receipt-connect-failed")); }
    const std::function<bool(Scene*)> callback = [pending, operation](Scene *scene) {
        bool touched = false;
        bool insertionCommandOwned = false;
        Word insertionHistory = 0, insertionCommand = 0;
        qsizetype insertionBefore = 0;
        ActiveSnapshot current;
        PreparedSelection originalSelection;
        const auto compensateInsertion = [&] {
            if (!insertionCommandOwned) return true;
            insertionCommandOwned = false;
            try {
                // Recheck the precise command before Undo. Selection updates
                // have no history entry, so this must still be our own tail.
                const Mappings maps;
                qsizetype count = 0; Word data = 0, tail = 0;
                if (!listHeader(maps, insertionHistory + 0x18, MaxHistory, count, data)
                    || count != insertionBefore + 1
                    || !maps.read(data + Word(count - 1) * 16, tail) || tail != insertionCommand)
                    return false;
                using Undo = void (*)(void*);
                reinterpret_cast<Undo>(Word(0xe50b20))(reinterpret_cast<void*>(insertionHistory));
                for (auto &node : originalSelection.nodes)
                    reinterpret_cast<Items*>(node.address + 0x1d0)->swap(node.previous);
                installRecords(scene, pending->context, originalSelection.records, originalSelection.bounds, {});
                ActiveSnapshot restored;
                return observeActive(pending->context, scene, restored)
                    && restored.value.fingerprint == current.value.fingerprint;
            } catch (...) { return false; }
        };
        const auto complete = [&](bool success, const QString &reason, const ActiveSnapshot *snapshot = nullptr) {
            const bool compensated = success || compensateInsertion();
            if (success) insertionCommandOwned = false;
            std::lock_guard<std::mutex> lock(pending->mutex);
            pending->ran = true; pending->success = success;
            pending->reason = compensated ? reason : reason + QStringLiteral("-compensation-unconfirmed");
            if (success && snapshot) pending->result = snapshot->value;
            // This return publishes dirty regions; it is separate from our
            // successful receipt. Read-only inspection must return false.
            return touched;
        };
        try {
            if (pending->cancelled.load(std::memory_order_acquire))
                return complete(false, QStringLiteral("cancelled-before-access"));
            QString contextError;
            if (!resolveWorkerScene(pending->context, scene, contextError)) return complete(false, contextError);
            if (pending->baseline.sceneIdentity && pending->baseline.sceneIdentity != pending->context.scene)
                return complete(false, pending->requireUnchanged ? QStringLiteral("insertion-scene-changed")
                                                                : QStringLiteral("selection-scene-changed"));
            if (!observeActive(pending->context, scene, current))
                return complete(false, current.value.reason, &current);
            if (pending->requireUnchanged) {
                // This callback still owns the native Page lock. Reject any
                // change since preparation before touching selection or ink.
                const auto error = RePaperNative::ObjectAccessDetail::insertionBaselineError(
                    pending->baseline, current.value);
                if (!error.isEmpty()) return complete(false, error);
            }
            if (operation == QStringLiteral("inspect")) return complete(true, {}, &current);
            const bool replacing = operation == QStringLiteral("replace");
            if (replacing) {
                const auto error = RePaperNative::ObjectAccessDetail::selectionRequestError(pending->baseline, current.value, pending->exactIds);
                auto wanted = pending->exactIds; std::sort(wanted.begin(), wanted.end());
                if (!error.isEmpty() || wanted.isEmpty() || !current.value.nativeSelectionExact || wanted != current.value.selectedIds)
                    return complete(false, error.isEmpty() ? QStringLiteral("replacement-selection-changed") : error);
                if (pending->baseline.history.valid
                    && !RePaperNative::sameNativeHistory(pending->baseline.history, current.value.history))
                    return complete(false, QStringLiteral("replacement-history-changed"));
            }
            const Mappings maps;
            Word state = 0;
            if (!maps.read(reinterpret_cast<Word>(scene) + 0x360, state) || !maps.contains(state, 0x1c9, true))
                return complete(false, QStringLiteral("native-selection-state-unavailable"));
            if (operation == QStringLiteral("cancel-properties")) {
                RePaperNative::ObjectAccessDetail::PropertyCancelOperations ops;
                ActiveSnapshot finalSnapshot;
                ops.cancelled = [&] { return pending->cancelled.load(std::memory_order_acquire); };
                ops.observe = [&] {
                    ActiveSnapshot observed;
                    observeActive(pending->context, scene, observed);
                    return observed.value;
                };
                ops.clearSelection = [&] {
                    ActiveSnapshot observed;
                    PreparedSelection selection;
                    if (!observeActive(pending->context, scene, observed)
                        || !prepareSelection(observed, {}, scene, selection)) return false;
                    QRectF dirty;
                    for (const auto &line : observed.lines)
                        for (const auto &point : line.value.stroke.points)
                            dirty = dirty.united(QRectF(point, QSizeF(1, 1)));
                    using Select = void (*)(void*, const IdSet*);
                    touched = true;
                    for (const auto &node : selection.nodes)
                        reinterpret_cast<Select>(Word(0xf3f250))(reinterpret_cast<void*>(node.address), &node.ids);
                    installRecords(scene, pending->context, selection.records, {}, dirty);
                    return true;
                };
                ops.undo = [&](const RePaperNative::NativeHistoryCommand &command) {
                    const Mappings fresh;
                    RePaperNative::NativeHistorySnapshot before;
                    quint8 readOnly = 0; unsigned char author[16]{};
                    if (!RePaperNative::HistoryDetail::readSnapshot(fresh, reinterpret_cast<Word>(scene), before)
                        || before.historyIdentity != pending->expectedCurrent.history.historyIdentity
                        || before.undo.isEmpty() || !(before.undo.last() == command)
                        || !fresh.read(0x1a676b0, readOnly) || (readOnly & 1)
                        || !fresh.read(reinterpret_cast<Word>(scene) + 0x26a, author)
                        || std::all_of(std::begin(author), std::end(author), [](auto byte) { return byte == 0; }))
                        return false;
                    using Undo = void (*)(void*);
                    touched = true;
                    reinterpret_cast<Undo>(Word(0xe50b20))(reinterpret_cast<void*>(before.historyIdentity));
                    return true;
                };
                const auto cancellation = RePaperNative::ObjectAccessDetail::runPropertyCancel(
                    pending->baseline, pending->expectedCurrent, ops);
                touched = touched || cancellation.touched;
                if (cancellation.cancelled) finalSnapshot.value = cancellation.snapshot;
                return complete(cancellation.cancelled, cancellation.reason,
                                cancellation.cancelled ? &finalSnapshot : nullptr);
            }
            if (operation == QStringLiteral("select")) {
                PreparedSelection prepared, restore;
                ActiveSnapshot result;
                RePaperNative::ObjectAccessDetail::SelectionOperations ops;
                ops.cancelled = [&] { return pending->cancelled.load(std::memory_order_acquire); };
                ops.observe = [&] { return current.value; };
                ops.prepare = [&] {
                    return prepareSelection(current, pending->exactIds, scene, prepared)
                        && prepareSelection(current, current.value.selectedIds, scene, restore);
                };
                ops.apply = [&] {
                    using Select = void (*)(void*, const IdSet*);
                    touched = true;
                    for (const auto &node : prepared.nodes)
                        reinterpret_cast<Select>(Word(0xf3f250))(reinterpret_cast<void*>(node.address), &node.ids);
                    installRecords(scene, pending->context, prepared.records, prepared.bounds, restore.bounds);
                };
                ops.verify = [&] {
                    if (!observeActive(pending->context, scene, result)) return false;
                    auto wanted = pending->exactIds; std::sort(wanted.begin(), wanted.end());
                    if (!result.value.nativeSelectionExact || result.value.selectedIds != wanted
                        || result.value.lines.size() != current.value.lines.size()) return false;
                    for (qsizetype i = 0; i < current.value.lines.size(); ++i) {
                        const auto &a = current.value.lines[i]; const auto &b = result.value.lines[i];
                        if (a.id != b.id || a.parentId != b.parentId || a.version != b.version) return false;
                    }
                    return true;
                };
                ops.restore = [&] {
                    for (auto &node : prepared.nodes)
                        reinterpret_cast<Items*>(node.address + 0x1d0)->swap(node.previous);
                    installRecords(scene, pending->context, restore.records, restore.bounds, prepared.bounds);
                };
                const auto resultCode = RePaperNative::ObjectAccessDetail::runSelection(pending->baseline, pending->exactIds, ops);
                touched = touched || resultCode.touched;
                return complete(resultCode.applied, resultCode.reason, resultCode.applied ? &result : nullptr);
            }
            // One line-only native insert, with actual IDs read from the same
            // retained objects while this exact worker callback still owns the
            // page lock. Neither selection counts nor a later queue wake-up
            // attributes another operation's ink to this insertion.
            Items prepared = pending->items;
            Items expected; expected.reserve(prepared.size());
            QRectF bounds;
            std::set<Word> addresses;
            qsizetype pointCount = 0;
            using Clone = ItemPtr (*)(SceneItem*);
            using Bounds = QRectF (*)(SceneItem*);
            using Translate = void (*)(SceneItem*, QPointF);
            for (const auto &item : std::as_const(prepared)) {
                const Word address = reinterpret_cast<Word>(item.get());
                NativeObjectLine line; quint64 origin = 0, renderGroup = 0;
                if (!addresses.insert(address).second || !maps.contains(address, 0xb0, true)
                    || !readLine(maps, address, {}, pointCount, line) || line.id || line.parentId
                    || !maps.read(address + 0x20, origin) || origin
                    || !maps.read(address + 0xa8, renderGroup) || renderGroup || !insertable(line.tool)
                    || line.stroke.points.size() < 2)
                    return complete(false, QStringLiteral("private-insertion-line-invalid"));
                const QRectF itemBounds = reinterpret_cast<Bounds>(Word(0x6111e0))(item.get());
                if (!finite(itemBounds)) return complete(false, QStringLiteral("private-line-bounds-invalid"));
                bounds = bounds.united(itemBounds);
                auto clone = reinterpret_cast<Clone>(Word(0xed7220))(item.get());
                if (!clone || clone.get() == item.get()) return complete(false, QStringLiteral("expected-line-clone-invalid"));
                expected.append(std::move(clone));
            }
            const QPointF translation = pending->center - bounds.center();
            QVector<QByteArray> expectedVersions; pointCount = 0;
            for (const auto &item : std::as_const(expected)) {
                reinterpret_cast<Translate>(Word(0xed7190))(item.get(), translation);
                const Mappings fresh;
                NativeObjectLine line;
                if (!readLine(fresh, reinterpret_cast<Word>(item.get()), {}, pointCount, line))
                    return complete(false, QStringLiteral("expected-insertion-geometry-invalid"));
                expectedVersions.append(line.version);
            }
            if (!historyReady(maps, reinterpret_cast<Word>(scene), insertionHistory, insertionBefore, !replacing))
                return complete(false, QStringLiteral("native-insertion-history-unavailable"));
            if(replacing&&insertionBefore>MaxHistory-2)
                return complete(false,QStringLiteral("native-replacement-history-budget"));
            if (!prepareSelection(current, current.value.selectedIds, scene, originalSelection))
                return complete(false, QStringLiteral("original-selection-preparation-failed"));
            if (pending->cancelled.load(std::memory_order_acquire))
                return complete(false, QStringLiteral("cancelled-before-insertion"));
            using Insert = bool (*)(Scene*, int, Items*, QPointF);
            ActiveSnapshot finalSnapshot;
            QVector<quint64> ids;
            const auto verifyInsertion = [&]() {
                const Mappings fresh;
                ActiveSnapshot insertedSnapshot;
                std::set<quint64> distinct;
                const auto removedCount = replacing ? pending->exactIds.size() : 0;
                if (!observeActive(pending->context, scene, insertedSnapshot)
                    || insertedSnapshot.lines.size() != current.lines.size() - removedCount + prepared.size()) return false;
                for (const auto &previous : current.lines) {
                    const bool removed = replacing && pending->exactIds.contains(previous.value.id);
                    const auto active = std::find_if(insertedSnapshot.lines.cbegin(), insertedSnapshot.lines.cend(),
                        [&](const auto &line) { return line.value.id == previous.value.id; });
                    if (removed) { if (active != insertedSnapshot.lines.cend()) return false; }
                    else if (active == insertedSnapshot.lines.cend() || active->address != previous.address
                        || active->value.parentId != previous.value.parentId || active->value.version != previous.value.version) return false;
                }
                ids.clear();
                for (qsizetype i = 0; i < prepared.size(); ++i) {
                    const Word address = reinterpret_cast<Word>(prepared[i].get()); quint64 id = 0;
                    if (!fresh.read(address + 0x10, id) || !(id & CounterMask) || !distinct.insert(id).second) return false;
                    for (const auto &previous : current.lines) if (previous.value.id == id) return false;
                    const auto active = std::find_if(insertedSnapshot.lines.cbegin(), insertedSnapshot.lines.cend(),
                        [&](const auto &line) { return line.address == address && line.value.id == id; });
                    if (active == insertedSnapshot.lines.cend() || active->value.parentId != pending->context.layerId
                        || active->value.lineageId != id || active->value.version != expectedVersions[i]) return false;
                    ids.append(id);
                }
                // Finish creation as normal page ink; an edited selection keeps
                // its new exact IDs for the extension's selection overlay.
                const auto selectionIds = replacing ? ids : QVector<quint64>{};
                PreparedSelection selection;
                if (!prepareSelection(insertedSnapshot, selectionIds, scene, selection)) return false;
                using Select = void (*)(void*, const IdSet*);
                for (const auto &node : selection.nodes)
                    reinterpret_cast<Select>(Word(0xf3f250))(reinterpret_cast<void*>(node.address), &node.ids);
                installRecords(scene, pending->context, selection.records, selection.bounds, bounds.translated(translation));
                if (!observeActive(pending->context, scene, finalSnapshot)) return false;
                auto wanted = selectionIds; std::sort(wanted.begin(), wanted.end());
                return finalSnapshot.value.nativeSelectionExact && finalSnapshot.value.selectedIds == wanted;
            };
            if (replacing) {
                using Edit = bool (*)(Scene*, int);
                using Group = void (*)(void*, int);
                using Undo = void (*)(void*);
                RePaperNative::ColorDetail::RecordedEditOperations operations;
                operations.cancelled = [&] { return pending->cancelled.load(std::memory_order_acquire); };
                operations.historyCount = [&] {
                    const Mappings fresh; qsizetype count = 0; Word data = 0;
                    return listHeader(fresh, insertionHistory + 0x18, MaxHistory, count, data) ? count : qsizetype(-1);
                };
                operations.eraseSelected = [&] { return reinterpret_cast<Edit>(Word(0xec4180))(scene, pending->context.layer); };
                operations.insertSelected = [&] { return reinterpret_cast<Insert>(Word(0xec7130))(scene, pending->context.layer, &prepared, pending->center); };
                operations.verifyInserted = verifyInsertion;
                operations.groupHistory = [&](int count) { reinterpret_cast<Group>(Word(0xe51880))(reinterpret_cast<void*>(insertionHistory), count); };
                operations.undo = [&] { reinterpret_cast<Undo>(Word(0xe50b20))(reinterpret_cast<void*>(insertionHistory)); };
                const auto edit = RePaperNative::ColorDetail::runRecordedEdit(operations);
                touched = edit.touchedScene;
                if (!edit.committed) {
                    // Undo restores content; rebuild the original exact records
                    // only after observing that content matches the baseline.
                    ActiveSnapshot restored;
                    if (edit.compensated && observeActive(pending->context, scene, restored)) {
                        bool matches = restored.lines.size() == current.lines.size();
                        for (qsizetype i = 0; matches && i < current.lines.size(); ++i)
                            matches = current.lines[i].value.id == restored.lines[i].value.id
                                && current.lines[i].value.version == restored.lines[i].value.version;
                        PreparedSelection selection;
                        if (matches && prepareSelection(restored, current.value.selectedIds, scene, selection)) {
                            using Select = void (*)(void*, const IdSet*);
                            for (const auto &node : selection.nodes)
                                reinterpret_cast<Select>(Word(0xf3f250))(reinterpret_cast<void*>(node.address), &node.ids);
                            installRecords(scene, pending->context, selection.records, selection.bounds, bounds.translated(translation));
                        }
                    }
                    return complete(false, edit.reason);
                }
                // verifyInsertion observes the two concrete edits before the
                // native macro is formed. Attribute the final grouped command
                // from this same locked callback, not the pre-group snapshot.
                const Mappings groupedHistory;
                RePaperNative::HistoryDetail::readSnapshot(groupedHistory, reinterpret_cast<Word>(scene),
                                                           finalSnapshot.value.history);
                { std::lock_guard<std::mutex> lock(pending->mutex); pending->inserted = ids; }
                return complete(true, {}, &finalSnapshot);
            }
            touched = true;
            const bool inserted = reinterpret_cast<Insert>(Word(0xec7130))(scene, pending->context.layer, &prepared, pending->center);
            const Mappings fresh;
            qsizetype after = 0; Word historyData = 0;
            insertionCommandOwned = listHeader(fresh, insertionHistory + 0x18, MaxHistory, after, historyData)
                && after == insertionBefore + 1
                && fresh.read(historyData + Word(after - 1) * 16, insertionCommand) && insertionCommand;
            if (!inserted || !insertionCommandOwned)
                return complete(false, QStringLiteral("native-insertion-history-unconfirmed"));
            if (!verifyInsertion()) {
                return complete(false, QStringLiteral("inserted-identities-unconfirmed"));
            }
            { std::lock_guard<std::mutex> lock(pending->mutex); pending->inserted = ids; }
            return complete(true, {}, &finalSnapshot);
        } catch (...) {
            return complete(false, QStringLiteral("native-object-access-exception"));
        }
    };
    using Request = quint64 (*)(QObject*, const QString*, const std::function<bool(Scene*)>*);
    try {
        d->jobId = reinterpret_cast<Request>(Word(0xad71d0))(worker, &pending->context.pageId, &callback);
    } catch (...) { cancel(); return reject(QStringLiteral("native-object-request-failed")); }
    if (!d->jobId) { cancel(); return reject(QStringLiteral("native-object-job-id-missing")); }
    d->reason.clear(); d->elapsed.start(); d->timer.start(); emit statusChanged();
    return true;
#endif
}

void NativeObjectAccess::jobCompleted(qulonglong jobId) {
    if (!d->busy || !d->pending || sender() != d->worker.data() || jobId != d->jobId) return;
    d->completed = true; checkCompletion();
}
void NativeObjectAccess::workerUnavailable() {
    if (d->busy) finish(false, QStringLiteral("native-worker-destroyed"));
}
void NativeObjectAccess::controllerUnavailable() {
    if (d->busy) finish(false, QStringLiteral("native-controller-destroyed"));
}
void NativeObjectAccess::checkCompletion() {
    if (!d->busy || !d->pending) return;
    if (!d->controller) { controllerUnavailable(); return; }
    if (!d->worker) { workerUnavailable(); return; }
    if (d->controller->property("pageId").toString() != d->context.value("pageId").toString()) {
        finish(false, QStringLiteral("native-page-context-changed")); return;
    }
    if (d->controller->property("worker").value<QObject*>() != d->worker.data()) {
        finish(false, QStringLiteral("native-worker-replaced")); return;
    }
    if (!d->completed) {
        if (d->elapsed.elapsed() > 30000 && d->reason.isEmpty()) {
            d->reason = QStringLiteral("native-object-job-still-running"); emit statusChanged();
        }
        return;
    }
    bool ran = false, success = false; QString reason;
    {
        std::lock_guard<std::mutex> lock(d->pending->mutex);
        ran = d->pending->ran; success = d->pending->success; reason = d->pending->reason;
        d->result = d->pending->result; d->inserted = d->pending->inserted;
    }
    if (!ran) { finish(false, QStringLiteral("native-page-callback-did-not-run")); return; }
    // The GUI transform may have changed while the callback ran. Its identity
    // is a necessary gate for consuming this result as a selection baseline.
    const auto pendingEdit = d->controller->property("pendingEdit");
    if (!pendingEdit.canConvert<QTransform>() || !pendingEdit.value<QTransform>().isIdentity()) {
        d->result.pendingEditIdentity = false;
        finish(false, QStringLiteral("native-transform-changed-during-access")); return;
    }
    finish(success, reason);
}
