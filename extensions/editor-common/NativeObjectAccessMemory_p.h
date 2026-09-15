// Exact-target mapped-memory and Page ownership helpers. Included only by NativeObjectAccess.cpp.
// Kept separate from the public snapshot; no pointer escapes through that API.
using Region = RePaperNative::ObjectAccessDetail::MemoryRegion;
class Mappings {
    RePaperNative::ObjectAccessDetail::MemoryRanges ranges;
public:
    Mappings() {
        QVector<Region> regions;
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
        ranges=RePaperNative::ObjectAccessDetail::MemoryRanges(std::move(regions));
    }
    bool contains(Word address, Word length, bool write = false, bool execute = false) const {
        return ranges.contains(address,length,write,execute);
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
