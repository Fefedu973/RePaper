#include "NativeSelectionColor.h"
#include "NativeHistoryGuard.h"
#include "NativePropertyHistory_p.h"
#include "NativeStrokeColor.h"
#include "TargetProfile.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QElapsedTimer>
#include <QFile>
#include <QMetaMethod>
#include <QMetaProperty>
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
#include <utility>

bool RePaperNative::ColorDetail::nativeInsertAcceptsTool(int tool) {
    // Exact native append f3bb80 silently refuses these eraser-like tools.
    return !(tool >= 8 && tool <= 11) && tool != 22;
}

QVector<quint64> RePaperNative::ColorDetail::matchedCloneLineages(
        const QVector<QByteArray> &sourceDigests, const QVector<quint64> &sourceLineages,
        const QVector<QByteArray> &cloneDigests) {
    constexpr quint64 clockMask = 0x0000ffffffffffffULL;
    if (sourceDigests.isEmpty() || sourceDigests.size() > 128
        || sourceDigests.size() != sourceLineages.size()
        || sourceDigests.size() != cloneDigests.size()) return {};
    for (qsizetype i = 0; i < sourceDigests.size(); ++i)
        if (sourceDigests[i].size() != 32 || !(sourceLineages[i] & clockMask)) return {};
    QVector<bool> used(sourceDigests.size(), false);
    QVector<quint64> result;
    result.reserve(cloneDigests.size());
    for (const auto &digest : cloneDigests) {
        qsizetype match = -1;
        for (qsizetype i = 0; i < sourceDigests.size(); ++i) {
            if (!used[i] && sourceDigests[i] == digest) { match = i; break; }
        }
        if (match < 0) return {};
        used[match] = true;
        result.append(sourceLineages[match]);
    }
    return result;
}

RePaperNative::ColorDetail::RecordedEditResult
RePaperNative::ColorDetail::runRecordedEdit(const RecordedEditOperations &op) {
    if (op.cancelled()) return {false, false, QStringLiteral("cancelled-before-edit")};
    const qsizetype before = op.historyCount();
    if (before < 0) return {false, false, QStringLiteral("history-unavailable")};
    bool erased = false, inserted = false, nativeCallInProgress = false, groupingStarted = false;
    bool editStarted = false;
    const auto compensate = [&] {
        // Undo only the exact commands this callback has observed being added.
        // Unknown deltas never consume an unrelated native history entry.
        const int known = int(erased) + int(inserted);
        if (!known || op.historyCount() != before + known) return false;
        for (int i = 0; i < known; ++i) op.undo();
        return op.historyCount() == before;
    };
    try {
        editStarted = true;
        nativeCallInProgress = true;
        const bool deleted = op.eraseSelected();
        nativeCallInProgress = false;
        if (!deleted)
            return {false, false, QStringLiteral("delete-refused")};
        if (op.historyCount() != before + 1)
            return {false, false, QStringLiteral("delete-history-unconfirmed"), true};
        erased = true;
        // Cancellation after deletion must finish the pair. The GUI can drop
        // the obsolete receipt; leaving only the deletion would lose content.
        nativeCallInProgress = true;
        const bool added = op.insertSelected();
        nativeCallInProgress = false;
        if (!added)
            return {false, compensate(), QStringLiteral("insert-refused"), true};
        if (op.historyCount() != before + 2)
            return {false, false, QStringLiteral("insert-history-unconfirmed"), true};
        inserted = true;
        if (!op.verifyInserted())
            return {false, compensate(), QStringLiteral("insert-observation-mismatch"), true};
        groupingStarted = true;
        op.groupHistory(2);
        if (op.historyCount() != before + 1)
            return {false, false, QStringLiteral("history-group-unconfirmed"), true};
        return {true, false, {}, true};
    } catch (...) {
        // Native allocation faults are not an atomic rollback contract. Only
        // compensate a known recorded delta; never guess about unrecorded work.
        bool restored = false;
        if (!nativeCallInProgress && !groupingStarted) {
            try { restored = compensate(); } catch (...) {}
        }
        return {false, restored, QStringLiteral("native-edit-exception"), editStarted};
    }
}

#if defined(__aarch64__) && defined(REPAPER_WITH_NATIVE_ABI)
#include "rm_Line.hpp"
#include "rm_SceneItem.hpp"
#include <pthread.h>

class Page;
class Scene;
namespace {
using Word = quintptr;
using ItemPtr = std::shared_ptr<SceneItem>;
using Items = QList<ItemPtr>;
constexpr qsizetype MaxItems = 128, MaxPoints = 200000, MaxNodes = 4096;
constexpr qsizetype MaxBuckets = 32768, MaxPages = 65536, MaxHistory = 1000000;
static_assert(sizeof(Word) == 8 && sizeof(Items) == 24 && sizeof(Line) == 0x58);
static_assert(sizeof(LinePoint) == 0xe && sizeof(std::function<bool(Scene*)>) == 32);
static_assert(sizeof(pthread_mutex_t) == 0x30);

struct Region { Word first, last; bool write, execute; };
class Mappings {
    QList<Region> regions;
public:
    Mappings() {
        QFile file(QStringLiteral("/proc/self/maps"));
        if (!file.open(QIODevice::ReadOnly)) return;
        for (const auto &line : file.readAll().split('\n')) {
            const auto fields = line.simplified().split(' ');
            if (fields.size() < 2 || fields[1].size() < 3 || fields[1][0] != 'r') continue;
            const auto range = fields[0].split('-');
            if (range.size() != 2) continue;
            bool firstOk = false, lastOk = false;
            const Word first = range[0].toULongLong(&firstOk, 16);
            const Word last = range[1].toULongLong(&lastOk, 16);
            if (firstOk && lastOk && first < last)
                regions.append({first, last, fields[1][1] == 'w', fields[1][2] == 'x'});
        }
    }
    bool contains(Word address, Word length, bool write = false, bool execute = false) const {
        if (!address || !length || length > std::numeric_limits<Word>::max() - address) return false;
        return std::any_of(regions.cbegin(), regions.cend(), [=](const auto &r) {
            return address >= r.first && address + length <= r.last
                && (!write || r.write) && (!execute || r.execute);
        });
    }
    template<class T> bool read(Word address, T &value) const {
        if (!contains(address, sizeof(T))) return false;
        std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
        return true;
    }
};
class TryLock {
    pthread_mutex_t *mutex = nullptr;
public:
    TryLock(const Mappings &maps, Word address) {
        if (maps.contains(address, sizeof(pthread_mutex_t), true)
            && address % alignof(pthread_mutex_t) == 0) {
            auto *candidate = reinterpret_cast<pthread_mutex_t*>(address);
            if (pthread_mutex_trylock(candidate) == 0) mutex = candidate;
        }
    }
    ~TryLock() { if (mutex) pthread_mutex_unlock(mutex); }
    explicit operator bool() const { return mutex != nullptr; }
    TryLock(const TryLock&) = delete;
    TryLock &operator=(const TryLock&) = delete;
};
bool stringView(const Mappings &maps, Word address, QStringView &value) {
    Word chars = 0; qsizetype count = 0;
    if (!maps.read(address + 8, chars) || !maps.read(address + 16, count)
        || count < 0 || count > 128) return false;
    if (count && (!maps.contains(chars, Word(count) * 2) || chars % alignof(QChar))) return false;
    value = count ? QStringView(reinterpret_cast<const QChar*>(chars), count) : QStringView{};
    return true;
}
bool activePage(const Mappings &maps, Word worker, const QString &pageId) {
    Word node = 0; std::set<Word> visited;
    if (!maps.read(worker + 0x208, node)) return false;
    while (node) {
        if (visited.size() >= size_t(MaxPages) || !visited.insert(node).second) return false;
        QStringView key;
        if (!stringView(maps, node + 0x20, key)) return false;
        const int order = key.compare(QStringView(pageId), Qt::CaseSensitive);
        if (!order) return true;
        if (!maps.read(node + (order < 0 ? 0x18 : 0x10), node)) return false;
    }
    return false;
}
Word existingPageData(const Mappings &maps, Word worker, const QString &pageId) {
    Word begin = 0, end = 0;
    if (!maps.read(worker + 0x1e0, begin) || !maps.read(worker + 0x1e8, end) || !begin
        || end < begin || (end - begin) % 16 || (end - begin) / 16 > Word(MaxPages)
        || !maps.contains(begin, end - begin)) return 0;
    for (Word entry = begin; entry < end; entry += 16) {
        Word item = 0;
        if (!maps.read(entry, item) || !item) return 0;
        QStringView key;
        if (!stringView(maps, item + 0x40, key)) return 0;
        if (key == QStringView(pageId)) {
            Word vtable = 0;
            return maps.read(item, vtable) && vtable == 0x15568d8 && maps.contains(item, 0xe0) ? item : 0;
        }
    }
    return 0;
}
struct HashPair { quint64 key; Word value; };
bool hashPairs(const Mappings &maps, Word address, QList<HashPair> &pairs) {
    Word hash = 0, buckets = 0, spans = 0, size = 0;
    pairs.clear();
    if (!maps.read(address, hash)) return false;
    if (!hash) return true;
    if (!maps.read(hash + 8, size) || !maps.read(hash + 0x10, buckets)
        || !maps.read(hash + 0x20, spans)) return false;
    if (!(size <= Word(MaxNodes) && buckets >= 128 && buckets <= Word(MaxBuckets)
          && (buckets & (buckets - 1)) == 0 && size <= buckets
          && maps.contains(spans, (buckets / 128) * 0x90))) return false;
    std::set<quint64> keys;
    for (Word s = 0; s < buckets / 128; ++s) {
        const Word span = spans + s * 0x90;
        Word entries = 0; quint8 allocated = 0;
        if (!maps.read(span + 0x80, entries) || !maps.read(span + 0x88, allocated) || allocated > 128) return false;
        if (allocated && !maps.contains(entries, Word(allocated) * 24)) return false;
        std::set<quint8> seen;
        for (Word bucket = 0; bucket < 128; ++bucket) {
            quint8 index = 0;
            if (!maps.read(span + bucket, index)) return false;
            if (index == 0xff) continue;
            if (index >= allocated || !seen.insert(index).second || pairs.size() >= MaxNodes) return false;
            HashPair pair{};
            if (!maps.read(entries + Word(index) * 24, pair.key)
                || !maps.read(entries + Word(index) * 24 + 8, pair.value)
                || !pair.value || !keys.insert(pair.key).second) return false;
            pairs.append(pair);
        }
    }
    return Word(pairs.size()) == size;
}
bool listHeader(const Mappings &maps, Word address, qsizetype maximum, qsizetype &count, Word &data) {
    Word array = 0;
    if (!maps.read(address, array) || !maps.read(address + 8, data)
        || !maps.read(address + 16, count) || count < 0 || count > maximum) return false;
    if (array && !maps.contains(array, 16)) return false;
    return !count || maps.contains(data, Word(count) * 16);
}
bool lineDigest(const Mappings &maps, Word address, qsizetype &totalPoints,
                QByteArray &digest, QRectF *bounds = nullptr, QColor *color = nullptr,
                const QPointF *translation = nullptr) {
    Word table = 0, array = 0, data = 0; quint8 tag = 0; qsizetype count = 0;
    if (!maps.contains(address, 0xb0) || !maps.read(address, table) || table != 0x1682940
        || !maps.read(address + 8, tag) || tag != 3
        || !maps.read(address + 0x58, array) || !maps.read(address + 0x60, data)
        || !maps.read(address + 0x68, count) || count < 2 || count > MaxPoints - totalPoints
        || (array && !maps.contains(array, 16)) || !maps.contains(data, Word(count) * sizeof(LinePoint))) return false;
    const auto *line = std::launder(reinterpret_cast<const Line*>(address + 0x48));
    const QColor decoded = nativeStrokeColor(line->color, line->rgba);
    if (!decoded.isValid() || !RePaperNative::ColorDetail::nativeInsertAcceptsTool(line->tool)
        || !std::isfinite(line->maskScale) || !std::isfinite(line->thickness)
        || !std::isfinite(line->bounds.x()) || !std::isfinite(line->bounds.y())
        || !std::isfinite(line->bounds.width()) || !std::isfinite(line->bounds.height())
        || line->bounds.width() < 0 || line->bounds.height() < 0) return false;
    QByteArray translated;
    float dx = 0, dy = 0;
    if (translation) {
        dx = float(translation->x()); dy = float(translation->y());
        if (!std::isfinite(dx) || !std::isfinite(dy)) return false;
        translated.resize(count * qsizetype(sizeof(LinePoint)));
    }
    for (qsizetype i = 0; i < count; ++i) {
        LinePoint point{};
        if (!maps.read(data + Word(i) * sizeof(LinePoint), point)
            || !std::isfinite(point.x) || !std::isfinite(point.y)
            || std::abs(point.x) > 1000000 || std::abs(point.y) > 1000000) return false;
        if (translation) {
            // SceneLineItem::translate converts both offsets to float before
            // adding them (ed7190 -> f79640), exactly as native Copy does.
            point.x += dx; point.y += dy;
            if (!std::isfinite(point.x) || !std::isfinite(point.y)
                || std::abs(point.x) > 1000000 || std::abs(point.y) > 1000000) return false;
            std::memcpy(translated.data() + i * qsizetype(sizeof(LinePoint)), &point, sizeof(point));
        }
    }
    QByteArray fields;
    QDataStream stream(&fields, QIODevice::WriteOnly);
    // Bounds are only a cache. Native translation invalidates that cache even
    // for zero offset; subsequent selection bounds calculation can rebuild it
    // differently without changing any stroke content.
    stream << qint32(line->tool) << qint32(line->color) << quint32(line->rgba)
           << line->maskScale << line->thickness;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(fields);
    if (translation) hash.addData(translated);
    else hash.addData(QByteArrayView(reinterpret_cast<const char*>(data), count * qsizetype(sizeof(LinePoint))));
    digest = hash.result();
    totalPoints += count;
    if (bounds) *bounds = line->bounds;
    if (color) *color = decoded;
    return true;
}
struct ItemObservation {
    quint64 id = 0, parent = 0, origin = 0;
    Word address = 0;
    QByteArray digest, worldDigest;
    bool operator==(const ItemObservation &other) const {
        return id == other.id && parent == other.parent && origin == other.origin
            && address == other.address && digest == other.digest && worldDigest == other.worldDigest;
    }
};
struct NodeObservation {
    quint64 key = 0, groupMarker = 0, offsetXBits = 0, offsetYBits = 0;
    Word address = 0;
    QList<quint64> selectedIds;
    bool operator==(const NodeObservation &other) const {
        return key == other.key && address == other.address && groupMarker == other.groupMarker
            && offsetXBits == other.offsetXBits && offsetYBits == other.offsetYBits
            && selectedIds == other.selectedIds;
    }
};
struct SelectionSnapshot {
    Word scene = 0;
    quint64 layerId = 0;
    QList<ItemObservation> items;
    QList<NodeObservation> nodes;
    std::shared_ptr<Page> retainedPage;
    bool same(const SelectionSnapshot &other) const {
        return scene == other.scene && layerId == other.layerId && items == other.items && nodes == other.nodes;
    }
};
bool sceneSelection(const Mappings &maps, Word scene, quint64 layerId, SelectionSnapshot &snapshot) {
    Word vtable = 0;
    if (!maps.contains(scene, 0x368, true) || !maps.read(scene, vtable) || vtable != 0x167f620) return false;
    QList<HashPair> index, nodes;
    Word root = 0;
    if (!hashPairs(maps, scene + 0x248, index)) return false;
    for (const auto &entry : index) if (entry.key == layerId) root = entry.value;
    if (!root || !maps.contains(root, 0x1e8) || !hashPairs(maps, root + 0x198, nodes)
        || nodes.size() >= MaxNodes) return false;
    nodes.prepend({layerId, root});
    snapshot.scene = scene; snapshot.layerId = layerId; snapshot.items.clear(); snapshot.nodes.clear();
    std::set<Word> seenNodes;
    std::set<quint64> identities;
    qsizetype totalPoints = 0, worldPoints = 0;
    for (const auto &node : std::as_const(nodes)) {
        qsizetype count = 0; Word data = 0;
        if (!seenNodes.insert(node.value).second || !maps.contains(node.value, 0x1e8)
            || !listHeader(maps, node.value + 0x1d0, MaxItems - snapshot.items.size(), count, data)) return false;
        NodeObservation nodeObservation;
        nodeObservation.key = node.key; nodeObservation.address = node.value;
        // Exact native Copy at 8994ec checks node+138's low 48 bits, then
        // translates the clone by the QPointF at node+1b8 (899500–899510).
        // Capture those fields and selected membership as well as local Line
        // data, so a group-only move cannot pass an unchanged-item check.
        if (count && (!maps.read(node.value + 0x138, nodeObservation.groupMarker)
            || !maps.read(node.value + 0x1b8, nodeObservation.offsetXBits)
            || !maps.read(node.value + 0x1c0, nodeObservation.offsetYBits))) return false;
        for (qsizetype i = 0; i < count; ++i) {
            ItemObservation item;
            if (!maps.read(data + Word(i) * 16, item.address)
                || !maps.read(item.address + 0x10, item.id) || !maps.read(item.address + 0x18, item.parent)
                || !maps.read(item.address + 0x20, item.origin)
                || !item.id || !item.parent || !identities.insert(item.id).second
                || !lineDigest(maps, item.address, totalPoints, item.digest)) return false;
            item.worldDigest = item.digest;
            if (nodeObservation.groupMarker & 0x0000ffffffffffffULL) {
                double x = 0, y = 0;
                std::memcpy(&x, &nodeObservation.offsetXBits, sizeof(x));
                std::memcpy(&y, &nodeObservation.offsetYBits, sizeof(y));
                const QPointF translation(x, y);
                if (!lineDigest(maps, item.address, worldPoints, item.worldDigest,
                                nullptr, nullptr, &translation)) return false;
            }
            snapshot.items.append(item);
            nodeObservation.selectedIds.append(item.id);
        }
        if (count) {
            std::sort(nodeObservation.selectedIds.begin(), nodeObservation.selectedIds.end());
            snapshot.nodes.append(std::move(nodeObservation));
        }
    }
    std::sort(snapshot.items.begin(), snapshot.items.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
    std::sort(snapshot.nodes.begin(), snapshot.nodes.end(), [](const auto &a, const auto &b) { return a.key < b.key; });
    return true;
}
bool controllerSnapshot(QObject *controller, QObject *worker, const QString &pageId,
                        int layer, SelectionSnapshot &snapshot, QString &reason) {
    auto reject = [&](const char *code) { reason = QString::fromLatin1(code); return false; };
    if (!controller || !worker || controller->thread() != QThread::currentThread()
        || worker->thread() != QThread::currentThread()) return reject("wrong-thread");
    if (controller->property("pageId").toString() != pageId
        || controller->property("worker").value<QObject*>() != worker) return reject("page-changed");
    const int expected = controller->property("selectionItemCount").toInt();
    if (expected < 1 || expected > MaxItems) return reject("selection-limit");
    const Mappings maps;
    const Word workerAddress = reinterpret_cast<Word>(worker);
    const Word controllerAddress = reinterpret_cast<Word>(controller);
    if (!maps.contains(workerAddress, 0x218, true)) return reject("worker-mapping");
    qsizetype layerCount = 0; Word layers = 0; quint64 layerId = 0;
    if (layer < 0 || !maps.read(controllerAddress + 0x1a8, layerCount) || layer >= layerCount
        || layerCount > MaxPages || !maps.read(controllerAddress + 0x1a0, layers)
        || !maps.contains(layers, Word(layerCount) * 0x28)
        || !maps.read(layers + Word(layer) * 0x28 + 0x20, layerId) || !layerId) return reject("invalid-layer");
    std::shared_ptr<Page> page;
    {
        TryLock guard(maps, workerAddress + 0x118);
        if (!guard) return reject("worker-busy");
        if (!activePage(maps, workerAddress, pageId)) return reject("page-not-loaded");
        const Word pageData = existingPageData(maps, workerAddress, pageId);
        Word payload = 0, control = 0, vtable = 0;
        qint32 strong = 0, weak = 0;
        if (!pageData || !maps.read(pageData + 0xd0, payload) || !maps.read(pageData + 0xd8, control)
            || !payload || !control || control % alignof(std::shared_ptr<Page>) || payload != control + 0x10
            || !maps.contains(control, 0xb0, true) || !maps.read(control, vtable) || vtable != 0x1548af8
            || !maps.read(control + 8, strong) || !maps.read(control + 12, weak) || strong <= 0 || weak <= 0
            || strong == std::numeric_limits<qint32>::max()) return reject("invalid-page-ownership");
        page = *reinterpret_cast<const std::shared_ptr<Page>*>(pageData + 0xd0);
    }
    {
        const Word pageAddress = reinterpret_cast<Word>(page.get());
        TryLock guard(maps, pageAddress + 0x68);
        if (!guard) return reject("page-busy");
        Word scene = 0, owner = 0;
        if (!maps.read(pageAddress, scene) || !maps.read(pageAddress + 0x98, owner) || owner
            || !sceneSelection(maps, scene, layerId, snapshot)) return reject("selection-unavailable");
    }
    if (controller->property("pageId").toString() != pageId
        || controller->property("worker").value<QObject*>() != worker
        || controller->property("selectionItemCount").toInt() != expected
        || snapshot.items.size() != expected) return reject("selection-changed");
    snapshot.retainedPage = std::move(page);
    reason.clear();
    return true;
}
qsizetype historyCount(const Mappings &maps, Word history) {
    qsizetype count = 0; Word data = 0;
    return listHeader(maps, history + 0x18, MaxHistory, count, data) ? count : -1;
}
bool historyPreflight(const Mappings &maps, Word scene, quint64 layerId, Word &history) {
    Word owner = 0; quint8 readOnly = 0;
    if (!maps.read(scene + 0x348, history) || !maps.contains(history, 0x50, true)
        || !maps.read(history + 0x10, owner) || owner != scene
        || !maps.read(0x1a676b0, readOnly) || (readOnly & 1)) return false;
    unsigned char author[16]{};
    if (!maps.read(scene + 0x26a, author)
        || std::all_of(std::begin(author), std::end(author), [](auto byte) { return byte == 0; })) return false;
    QList<HashPair> nodes;
    if (!hashPairs(maps, scene + 0x248, nodes)
        || std::none_of(nodes.cbegin(), nodes.cend(), [=](const auto &node) { return node.key == layerId; })) return false;
    qsizetype count = 0; Word data = 0;
    if (!listHeader(maps, history + 0x18, MaxHistory - 2, count, data)) return false;
    if (count) {
        Word command = 0;
        if (!maps.read(data + Word(count - 1) * 16, command)
            || !RePaperNative::HistoryDetail::commandCannotMerge(maps, command)) return false;
    }
    for (const Word entry : {Word(0xad71d0), Word(0xec4180), Word(0xec7130), Word(0x6111e0),
                             Word(0xe51880), Word(0xe50b20), Word(0xe46220)})
        if (!maps.contains(entry, 4, false, true)) return false;
    return true;
}
bool expectedClones(const Mappings &maps, const Items &clones, const SelectionSnapshot &observed) {
    if (clones.size() != observed.items.size()) return false;
    QList<ItemObservation> expected;
    qsizetype total = 0;
    for (const auto &clone : clones) {
        ItemObservation item;
        item.address = reinterpret_cast<Word>(clone.get());
        if (!maps.read(item.address + 0x10, item.id) || !maps.read(item.address + 0x18, item.parent)
            || !maps.read(item.address + 0x20, item.origin)
            || !item.id || !item.parent || !lineDigest(maps, item.address, total, item.digest)) return false;
        item.worldDigest = item.digest;
        expected.append(item);
    }
    std::sort(expected.begin(), expected.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
    return expected == observed.items;
}
}
#endif

struct NativeSelectionColor::Private {
    QPointer<QObject> controller, worker;
    QString pageId, reason;
    int layer = -1, verificationAttempts = 0;
    qulonglong jobId = 0;
    bool busy = false, completed = false, finishing = false, destroying = false;
    qint64 warningAfterMs = 30000;
    QTimer timer;
    QElapsedTimer elapsed;
    QMetaObject::Connection completionConnection, workerDestroyed, controllerDestroyed;
    RePaperNative::NativeHistorySnapshot beforeHistory,afterHistory;
    struct Pending {
        std::atomic_bool cancelled{false};
        std::mutex mutex;
        bool callbackRan = false, committed = false;
        QString reason;
        RePaperNative::NativeHistorySnapshot expectedHistory,beforeHistory,afterHistory;
#if defined(__aarch64__) && defined(REPAPER_WITH_NATIVE_ABI)
        SelectionSnapshot baseline, expected;
        Items clones;
        QVector<quint64> cloneLineages;
#endif
    };
    std::shared_ptr<Pending> pending;
};

NativeSelectionColor::NativeSelectionColor(QObject *parent) : QObject(parent), d(new Private) {
    d->timer.setInterval(50);
    connect(&d->timer, &QTimer::timeout, this, &NativeSelectionColor::verifyCompletion);
}
NativeSelectionColor::~NativeSelectionColor() { d->destroying = true; cancel(); }
bool NativeSelectionColor::busy() const { return d->busy; }
QString NativeSelectionColor::reason() const { return d->reason; }
RePaperNative::NativeHistorySnapshot NativeSelectionColor::beforeHistory() const { return d->beforeHistory; }
RePaperNative::NativeHistorySnapshot NativeSelectionColor::afterHistory() const { return d->afterHistory; }
void NativeSelectionColor::cancel() {
    const bool wasBusy = d->busy;
    if (d->pending) d->pending->cancelled.store(true, std::memory_order_release);
    d->timer.stop();
    QObject::disconnect(d->completionConnection);
    QObject::disconnect(d->workerDestroyed);
    QObject::disconnect(d->controllerDestroyed);
    d->completionConnection = {}; d->workerDestroyed = {}; d->controllerDestroyed = {};
    d->controller.clear(); d->worker.clear(); d->pending.reset();
    d->busy = false; d->completed = false; d->jobId = 0;
    if(!d->finishing){d->beforeHistory={};d->afterHistory={};}
    if (wasBusy && !d->finishing)
        d->reason = QStringLiteral("Le suivi du changement de couleur a été arrêté pour cette page.");
    if (wasBusy && !d->destroying) emit statusChanged();
}
void NativeSelectionColor::finish(bool success, const QString &message) {
    d->reason = message;
    d->finishing = true;
    cancel();
    d->finishing = false;
    emit finished(success, message);
}

bool NativeSelectionColor::dispatch(QObject *controller, int layer, const QColor &color,
                                   const RePaperNative::NativeHistorySnapshot &expectedHistory) {
    const auto refuse = [&](const QString &message) { d->reason = message; return false; };
    if (d->busy) return refuse(QStringLiteral("Un changement de couleur est déjà en cours."));
    d->beforeHistory={};d->afterHistory={};
    if (thread() != QThread::currentThread() || !controller || controller->thread() != QThread::currentThread())
        return refuse(QStringLiteral("Le contrôleur de la sélection est indisponible."));
    if (layer < 0 || !color.isValid() || color.alpha() != 255)
        return refuse(QStringLiteral("La couche ou la couleur choisie est invalide."));
    if (QString::fromLatin1(controller->metaObject()->className()) != QStringLiteral("SceneController"))
        return refuse(QStringLiteral("Le contrôleur natif de la sélection n’est pas reconnu."));
#if !defined(__aarch64__) || !defined(REPAPER_WITH_NATIVE_ABI)
    Q_UNUSED(expectedHistory);
    return refuse(QStringLiteral("Le changement de couleur natif est indisponible sur cet hôte."));
#else
    static const bool exactTarget = RePaperNative::matchesRunningXochitl();
    if (!exactTarget) return refuse(QStringLiteral("Le firmware ne correspond pas au profil de couleur natif."));
    const auto listType = QMetaType::fromName("QList<std::shared_ptr<SceneItem>>");
    if (!listType.isValid() || listType.sizeOf() != sizeof(Items)
        || QMetaType::fromName("Line").sizeOf() != sizeof(Line))
        return refuse(QStringLiteral("Les types de la sélection native ne correspondent pas au profil."));
    const auto *meta = controller->metaObject();
    for (const auto property : {"worker", "pageId", "selectionItemCount", "pendingEdit"})
        if (meta->indexOfProperty(property) < 0)
            return refuse(QStringLiteral("Les propriétés de la sélection native sont incomplètes."));
    const auto pendingEdit = controller->property("pendingEdit");
    if (!pendingEdit.canConvert<QTransform>() || !pendingEdit.value<QTransform>().isIdentity())
        return refuse(QStringLiteral("Terminez le déplacement de la sélection avant de changer sa couleur."));
    auto *worker = controller->property("worker").value<QObject*>();
    const QString pageId = controller->property("pageId").toString();
    if (!worker || worker->thread() != QThread::currentThread()
        || QString::fromLatin1(worker->metaObject()->className()) != QStringLiteral("DocumentWorker")
        || pageId.isEmpty() || pageId.size() > 128)
        return refuse(QStringLiteral("La page native est indisponible."));
    const int completedSignal = worker->metaObject()->indexOfSignal("jobCompleted(qulonglong)");
    const int completedSlot = metaObject()->indexOfSlot("jobCompleted(qulonglong)");
    if (completedSignal < 0 || completedSlot < 0)
        return refuse(QStringLiteral("Le suivi du travail natif est indisponible."));
    auto pending = std::make_shared<Private::Pending>();
    pending->expectedHistory=expectedHistory;
    QString detail;
    if (!controllerSnapshot(controller, worker, pageId, layer, pending->baseline, detail))
        return refuse(QStringLiteral("La sélection native ne peut pas être vérifiée (%1).").arg(detail));
    // Native Copy applies each group's world transform to its own genuine
    // clones and clears their identities. Never flatten groups by guessing
    // coordinates or clone/mutate the original live instances ourselves.
    const double copyScale = 1.0;
    if (!QMetaObject::invokeMethod(controller, "cloneSelectedItems", Qt::DirectConnection,
            QGenericReturnArgument(listType.name(), &pending->clones),
            QGenericArgument("int", &layer), QGenericArgument("double", &copyScale))
        || pending->clones.size() != pending->baseline.items.size())
        return refuse(QStringLiteral("La copie native de la sélection a été refusée."));
    SelectionSnapshot afterCopy;
    if (!controllerSnapshot(controller, worker, pageId, layer, afterCopy, detail)
        || !pending->baseline.same(afterCopy))
        return refuse(QStringLiteral("La sélection a changé pendant la préparation de la couleur."));
    {
        const Mappings maps;
        std::set<Word> unique;
        qsizetype total = 0;
        bool noChange = true;
        QVector<QByteArray> cloneDigests, sourceDigests;
        QVector<quint64> sourceLineages;
        for (const auto &item : std::as_const(pending->baseline.items)) {
            sourceDigests.append(item.worldDigest);
            sourceLineages.append(item.origin & 0x0000ffffffffffffULL ? item.origin : item.id);
        }
        for (const auto &clone : std::as_const(pending->clones)) {
            const Word address = reinterpret_cast<Word>(clone.get());
            quint64 id = 0, parent = 0, origin = 0; QByteArray digest; QColor oldColor;
            if (!address || !unique.insert(address).second || !maps.contains(address, 0xb0, true)
                || !maps.read(address + 0x10, id) || !maps.read(address + 0x18, parent) || id || parent
                || !maps.read(address + 0x20, origin) || origin
                || std::any_of(pending->baseline.items.cbegin(), pending->baseline.items.cend(),
                               [=](const auto &item) { return item.address == address; })
                || !lineDigest(maps, address, total, digest, nullptr, &oldColor))
                return refuse(QStringLiteral("La copie native ne contient pas uniquement des traits privés valides."));
            cloneDigests.append(digest);
            // The same native bounds getter used by insertion may normalize a
            // zero-size cache using tool/scale padding. Only the private clone
            // is touched. Obtain the same bounds insertion will use for its
            // placement center; mutable cache bytes are excluded from digests.
            using Bounds = QRectF (*)(SceneItem*);
            const QRectF nativeBounds = reinterpret_cast<Bounds>(Word(0x6111e0))(clone.get());
            if (!std::isfinite(nativeBounds.x()) || !std::isfinite(nativeBounds.y())
                || !std::isfinite(nativeBounds.width()) || !std::isfinite(nativeBounds.height())
                || nativeBounds.width() <= 0 || nativeBounds.height() <= 0)
                return refuse(QStringLiteral("Les limites du trait natif ne peuvent pas être vérifiées."));
            noChange = noChange && oldColor.rgba() == color.rgba();
        }
        if (noChange) return refuse(QStringLiteral("La sélection utilise déjà cette couleur."));
        pending->cloneLineages = RePaperNative::ColorDetail::matchedCloneLineages(
            sourceDigests, sourceLineages, cloneDigests);
        if (pending->cloneLineages.size() != pending->clones.size())
            return refuse(QStringLiteral("La correspondance des traits copiés ne peut pas être vérifiée."));
        for (qsizetype i = 0; i < pending->clones.size(); ++i) {
            const auto &clone = pending->clones[i];
            // RM field 7 is SceneItem+20. Native transforms preserve this root
            // origin when they replace a line with a newly identified clone.
            // Recoloring preserves the same ownership; ordinary Copy remains
            // independent because it still leaves the clone origin at zero.
            std::memcpy(reinterpret_cast<unsigned char*>(clone.get()) + 0x20,
                        &pending->cloneLineages[i], sizeof(quint64));
            auto *line = std::launder(reinterpret_cast<Line*>(reinterpret_cast<unsigned char*>(clone.get()) + 0x48));
            line->color = 9; line->rgba = color.rgba();
        }
    }
    d->controller = controller; d->worker = worker; d->pageId = pageId; d->layer = layer;
    d->pending = pending; d->busy = true; d->completed = false; d->verificationAttempts = 0;
    d->completionConnection = QObject::connect(worker, worker->metaObject()->method(completedSignal),
        this, metaObject()->method(completedSlot), Qt::QueuedConnection);
    d->workerDestroyed = connect(worker, &QObject::destroyed, this, &NativeSelectionColor::cancel);
    d->controllerDestroyed = connect(controller, &QObject::destroyed, this, &NativeSelectionColor::cancel);
    if (!d->completionConnection) { cancel(); return refuse(QStringLiteral("Le suivi de la couleur native n’a pas pu être connecté.")); }
    const std::function<bool(Scene*)> callback = [pending, layer](Scene *nativeScene) {
        const auto complete = [&](bool committed, const QString &reason, bool notifyScene = false) {
            std::lock_guard<std::mutex> lock(pending->mutex);
            pending->callbackRan = true; pending->committed = committed; pending->reason = reason;
            // The worker's bool controls dirty-region publication, separately
            // from our successful-edit receipt. Compensating Undo also changes
            // scene state and must allow that publication.
            return notifyScene || committed;
        };
        try {
            if (pending->cancelled.load(std::memory_order_acquire)) return complete(false, QStringLiteral("cancelled-before-edit"));
            const Mappings maps;
            const Word scene = reinterpret_cast<Word>(nativeScene);
            SelectionSnapshot current;
            if (scene != pending->baseline.scene
                || !sceneSelection(maps, scene, pending->baseline.layerId, current)
                || !pending->baseline.same(current)) return complete(false, QStringLiteral("selection-changed"));
            Word history = 0;
            if (!historyPreflight(maps, scene, pending->baseline.layerId, history))
                return complete(false, QStringLiteral("native-history-preflight"));
            if(pending->expectedHistory.valid){
                if(!RePaperNative::HistoryDetail::readSnapshot(maps,scene,pending->beforeHistory)
                    ||!RePaperNative::sameNativeHistory(pending->expectedHistory,pending->beforeHistory))
                    return complete(false,QStringLiteral("properties-color-history-changed"));
            }
            using LayerId = quint64 (*)(Scene*, int);
            if (reinterpret_cast<LayerId>(Word(0xe46220))(nativeScene, layer) != pending->baseline.layerId)
                return complete(false, QStringLiteral("layer-changed"));
            // The standard insertion preprocessor ec82b0 forwards every
            // non-image item unchanged and in order. All entries here have the
            // exact line RTTI/tag, so copying the native list is equivalent.
            Items prepared = pending->clones;
            QRectF bounds; bool first = true;
            QList<QByteArray> wanted;
            qsizetype total = 0;
            for (const auto &clone : std::as_const(pending->clones)) {
                QByteArray digest;
                const Word address = reinterpret_cast<Word>(clone.get());
                quint64 id = 0, parent = 0, origin = 0;
                if (!maps.read(address + 0x10, id) || !maps.read(address + 0x18, parent)
                    || !maps.read(address + 0x20, origin) || id || parent
                    || origin != pending->cloneLineages.value(wanted.size())
                    || !lineDigest(maps, address, total, digest))
                    return complete(false, QStringLiteral("private-line-invalid"));
                wanted.append(digest);
            }
            total = 0;
            for (qsizetype i = 0; i < prepared.size(); ++i) {
                QByteArray digest; QRectF lineBounds;
                if (prepared[i].get() != pending->clones[i].get()
                    || !lineDigest(maps, reinterpret_cast<Word>(prepared[i].get()), total, digest, &lineBounds)
                    || digest != wanted[i]) return complete(false, QStringLiteral("clone-preparation-changed"));
                bounds = first ? lineBounds : bounds.united(lineBounds); first = false;
            }
            const QPointF placementCenter = bounds.center();
            if (!std::isfinite(bounds.x()) || !std::isfinite(bounds.y())
                || !std::isfinite(bounds.width()) || !std::isfinite(bounds.height())
                || !std::isfinite(placementCenter.x()) || !std::isfinite(placementCenter.y()))
                return complete(false, QStringLiteral("placement-bounds-invalid"));
            using Edit = bool (*)(Scene*, int);
            using Insert = bool (*)(Scene*, int, Items*, QPointF);
            using Group = void (*)(void*, int);
            using Undo = void (*)(void*);
            const auto erase = reinterpret_cast<Edit>(Word(0xec4180));
            const auto insert = reinterpret_cast<Insert>(Word(0xec7130));
            const auto group = reinterpret_cast<Group>(Word(0xe51880));
            const auto undo = reinterpret_cast<Undo>(Word(0xe50b20));
            RePaperNative::ColorDetail::RecordedEditOperations operations;
            operations.cancelled = [pending] { return pending->cancelled.load(std::memory_order_acquire); };
            operations.historyCount = [&] {
                // Native QList/QHash allocations may extend brk or add an mmap
                // after the previous operation. Re-read mappings before every
                // post-edit container check; stale ranges can mimic refusal.
                const Mappings currentMaps;
                return historyCount(currentMaps, history);
            };
            operations.eraseSelected = [&] { return erase(nativeScene, layer); };
            operations.insertSelected = [&] { return insert(nativeScene, layer, &prepared, placementCenter); };
            operations.verifyInserted = [&] {
                const Mappings currentMaps;
                SelectionSnapshot observed;
                if (!sceneSelection(currentMaps, scene, pending->baseline.layerId, observed)
                    || !expectedClones(currentMaps, prepared, observed)) return false;
                // Check against the immutable pre-insertion Line fingerprints,
                // not just against clones that native insertion could change.
                qsizetype points = 0;
                for (qsizetype i = 0; i < prepared.size(); ++i) {
                    QByteArray digest;
                    quint64 origin = 0;
                    if (!lineDigest(currentMaps, reinterpret_cast<Word>(prepared[i].get()), points, digest)
                        || !currentMaps.read(reinterpret_cast<Word>(prepared[i].get()) + 0x20, origin)
                        || origin != pending->cloneLineages[i]
                        || digest != wanted[i]) return false;
                }
                pending->expected = std::move(observed);
                return true;
            };
            operations.groupHistory = [&](int count) { group(reinterpret_cast<void*>(history), count); };
            operations.undo = [&] { undo(reinterpret_cast<void*>(history)); };
            const auto result = RePaperNative::ColorDetail::runRecordedEdit(operations);
            if(result.committed&&pending->expectedHistory.valid){
                const Mappings currentMaps;
                // The edit owns exactly one known native macro. The later UI
                // refresh must match this precise worker-side history receipt.
                RePaperNative::HistoryDetail::readSnapshot(currentMaps,scene,pending->afterHistory);
            }
            return complete(result.committed, result.reason, result.touchedScene);
        } catch (...) {
            return complete(false, QStringLiteral("native-preparation-exception"));
        }
    };
    using Request = quint64 (*)(QObject*, const QString*, const std::function<bool(Scene*)>*);
    try {
        d->jobId = reinterpret_cast<Request>(Word(0xad71d0))(worker, &d->pageId, &callback);
    } catch (...) {
        cancel(); return refuse(QStringLiteral("Le travail de couleur natif n’a pas pu être envoyé."));
    }
    if (!d->jobId) { cancel(); return refuse(QStringLiteral("Le travail de couleur natif n’a pas reçu d’identifiant.")); }
    d->reason = QStringLiteral("Changement de couleur…");
    d->elapsed.start(); d->timer.start();
    return true;
#endif
}

void NativeSelectionColor::jobCompleted(qulonglong jobId) {
    if (!d->busy || !d->pending || sender() != d->worker.data() || jobId != d->jobId) return;
    d->completed = true;
    verifyCompletion();
}
void NativeSelectionColor::verifyCompletion() {
    if (!d->busy || !d->pending) return;
    if (!d->controller || !d->worker || d->controller->property("pageId").toString() != d->pageId
        || d->controller->property("worker").value<QObject*>() != d->worker.data()) { cancel(); return; }
    if (!d->completed) {
        const QString waiting = QStringLiteral("Le travail de couleur natif est toujours en cours ; attente de sa fin.");
        if (d->elapsed.elapsed() > d->warningAfterMs && d->reason != waiting) {
            // A timer cannot cancel an already executing worker edit. Retain
            // the busy lock until this exact job completes or its page leaves.
            d->reason = waiting;
            emit statusChanged();
        }
        return;
    }
    bool ran = false, committed = false; QString detail;
    {
        std::lock_guard<std::mutex> lock(d->pending->mutex);
        ran = d->pending->callbackRan; committed = d->pending->committed; detail = d->pending->reason;
        d->beforeHistory=d->pending->beforeHistory;d->afterHistory=d->pending->afterHistory;
    }
    if (!ran || !committed) {
        finish(false, QStringLiteral("Le changement de couleur natif n’est pas confirmé (%1).")
            .arg(ran ? detail : QStringLiteral("page-unavailable")));
        return;
    }
#if defined(__aarch64__) && defined(REPAPER_WITH_NATIVE_ABI)
    SelectionSnapshot observed;
    if (controllerSnapshot(d->controller, d->worker, d->pageId, d->layer, observed, detail)
        && observed.same(d->pending->expected)) {
        finish(true, QStringLiteral("Couleur de la sélection modifiée."));
        return;
    }
    if (++d->verificationAttempts < 20) return;
#endif
    finish(false, QStringLiteral("Le travail natif est terminé, mais la sélection recolorée n’a pas été confirmée."));
}
