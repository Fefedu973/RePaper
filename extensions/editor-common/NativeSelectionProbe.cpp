#include "NativeSelectionProbe.h"
#include "TargetProfile.h"
#include <QFile>
#include <QList>
#include <QMetaProperty>
#include <QMetaType>
#include <QObject>
#include "NativeSceneObserver.h"
#include <QThread>
#include <QVariantList>
#include <QCryptographicHash>
#include <QStringView>
#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <set>
#include <utility>

#if (defined(__aarch64__) && defined(REPAPER_WITH_NATIVE_ABI)) || defined(REPAPER_SELECTION_READER_TEST)
#include <pthread.h>
// These are the actual global class names in the pinned executable. Incomplete
// types suffice for owning copies of its existing std::shared_ptr instances.
class Page;
class SceneItem;
class SceneTreeNode;
namespace {
using Word=quintptr;
using ItemPtr=std::shared_ptr<SceneItem>;
using NodePtr=std::shared_ptr<SceneTreeNode>;
constexpr qsizetype MaxItems=128, MaxNodes=4096, MaxBuckets=32768, MaxPages=65536;
static_assert(sizeof(Word)==8 && sizeof(std::shared_ptr<Page>)==16);
static_assert(sizeof(QList<ItemPtr>)==24);
#if defined(__aarch64__)
static_assert(sizeof(pthread_mutex_t)==0x30);
#endif

struct Region {Word first,last; bool write,execute;};
class Mappings {
    QList<Region> regions;
public:
    Mappings() {
        QFile file("/proc/self/maps");
        if(!file.open(QIODevice::ReadOnly)) return;
        for(const auto &line:file.readAll().split('\n')) {
            const auto fields=line.simplified().split(' ');
            if(fields.size()<2 || fields[1].size()<3 || fields[1][0]!='r') continue;
            const auto range=fields[0].split('-'); if(range.size()!=2)continue;
            bool firstOk=false,lastOk=false;
            const Word first=range[0].toULongLong(&firstOk,16),last=range[1].toULongLong(&lastOk,16);
            if(firstOk && lastOk && first<last)regions.append({first,last,fields[1][1]=='w',fields[1][2]=='x'});
        }
    }
    bool contains(Word address,Word length,bool write=false,bool execute=false) const {
        if(!address || !length || length>std::numeric_limits<Word>::max()-address)return false;
        return std::any_of(regions.cbegin(),regions.cend(),[=](const auto &r){
            return address>=r.first && address+length<=r.last && (!write||r.write) && (!execute||r.execute);
        });
    }
    template<class T> bool read(Word address,T &value) const {
        if(!contains(address,sizeof(T)))return false;
        std::memcpy(&value,reinterpret_cast<const void*>(address),sizeof(T));return true;
    }
};
class TryLock {
    pthread_mutex_t *mutex=nullptr;
public:
    TryLock(const Mappings &maps,Word address) {
        if(maps.contains(address,sizeof(pthread_mutex_t),true) && address%alignof(pthread_mutex_t)==0) {
            auto *candidate=reinterpret_cast<pthread_mutex_t*>(address);
            if(pthread_mutex_trylock(candidate)==0)mutex=candidate;
        }
    }
    ~TryLock(){if(mutex)pthread_mutex_unlock(mutex);}
    explicit operator bool()const{return mutex!=nullptr;}
    TryLock(const TryLock&)=delete;TryLock&operator=(const TryLock&)=delete;
};
bool stringView(const Mappings &maps,Word address,QStringView &value) {
    Word chars=0;qsizetype count=0;
    if(!maps.read(address+8,chars)||!maps.read(address+16,count)||count<0||count>128)return false;
    if(count && (!maps.contains(chars,Word(count)*2)||chars%alignof(QChar)))return false;
    value=count?QStringView(reinterpret_cast<const QChar*>(chars),count):QStringView{};return true;
}
bool activePage(const Mappings &maps,Word worker,const QString &pageId) {
    Word node=0;std::set<Word> visited;
    if(!maps.read(worker+0x208,node))return false;
    while(node) {
        if(visited.size()>=size_t(MaxPages)||!visited.insert(node).second)return false;
        QStringView key;
        if(!stringView(maps,node+0x20,key))return false;
        const int order=key.compare(QStringView(pageId),Qt::CaseSensitive);
        if(!order)return true;
        if(!maps.read(node+(order<0?0x18:0x10),node))return false;
    }
    return false;
}
Word existingPageData(const Mappings &maps,Word worker,const QString &pageId) {
    Word begin=0,end=0;
    if(!maps.read(worker+0x1e0,begin)||!maps.read(worker+0x1e8,end)||!begin||end<begin
       ||(end-begin)%16||(end-begin)/16>Word(MaxPages)||!maps.contains(begin,end-begin))return 0;
    for(Word entry=begin;entry<end;entry+=16) {
        Word item=0;if(!maps.read(entry,item)||!item)return 0;
        QStringView key;if(!stringView(maps,item+0x40,key))return 0;
        if(key==QStringView(pageId)) {
            Word vtable=0;
            // __si_class_type_info establishes PageData's unadjusted ListItem
            // base; this is the exact PageData vtable, not a guessed cast.
            return maps.read(item,vtable)&&vtable==0x15568d8&&maps.contains(item,0xe0)?item:0;
        }
    }
    return 0;
}
struct HashPair {quint64 key;Word value;};
bool hashPairs(const Mappings &maps,Word address,QList<HashPair> &pairs) {
    Word hash=0,buckets=0,spans=0,size=0;
    pairs.clear();
    if(!maps.read(address,hash))return false;
    if(!hash)return true;
    // Qt6.10 QHash data: ref, size, numBuckets, seed, spans. Helpers below
    // use the same 128-bucket, 0x90-byte spans in the exact executable.
    if(!maps.read(hash+8,size)||!maps.read(hash+0x10,buckets)||!maps.read(hash+0x20,spans))return false;
    if(!(size<=Word(MaxNodes) && buckets>=128 && buckets<=Word(MaxBuckets)
        && (buckets&(buckets-1))==0 && size<=buckets
        && maps.contains(spans,(buckets/128)*0x90)))return false;
    std::set<quint64> keys;
    for(Word spanIndex=0;spanIndex<buckets/128;++spanIndex) {
        const Word span=spans+spanIndex*0x90;Word entries=0;quint8 allocated=0;
        if(!maps.read(span+0x80,entries)||!maps.read(span+0x88,allocated)||allocated>128)return false;
        if(allocated && !maps.contains(entries,Word(allocated)*24))return false;
        std::set<quint8> seen;
        for(Word bucket=0;bucket<128;++bucket) {
            quint8 index=0;if(!maps.read(span+bucket,index))return false;
            if(index==0xff)continue;
            if(index>=allocated||!seen.insert(index).second||pairs.size()>=MaxNodes)return false;
            HashPair pair{};
            if(!maps.read(entries+Word(index)*24,pair.key)||!maps.read(entries+Word(index)*24+8,pair.value)
               ||!pair.value||!keys.insert(pair.key).second)return false;
            pairs.append(pair);
        }
    }
    return Word(pairs.size())==size;
}
bool itemListHeader(const Mappings &maps,Word address,qsizetype &count) {
    Word array=0,data=0;
    if(!maps.read(address,array)||!maps.read(address+8,data)||!maps.read(address+0x10,count))return false;
    if(count<0 || count>MaxItems)return false;
    if(array && !maps.contains(array,16))return false;
    return count==0 || maps.contains(data,Word(count)*sizeof(ItemPtr));
}
}
#endif

bool RePaperNative::attachExistingSceneObserver(QObject *controller, const QString &pageId,
                                                NativeSceneObserver *observer, QString *reason) {
    auto reject = [&](const char *code) { if (reason) *reason = QString::fromLatin1(code); return false; };
    if (!controller || !observer || controller->thread() != QThread::currentThread()
        || observer->thread() != QThread::currentThread()) return reject("wrong-thread-or-controller");
    if (QString::fromLatin1(controller->metaObject()->className()) != "SceneController")
        return reject("wrong-controller-type");
    if (pageId.isEmpty() || pageId.size() > 128) return reject("page-unavailable");
#if !defined(__aarch64__) || !defined(REPAPER_WITH_NATIVE_ABI)
    return reject("unsupported-build");
#else
    static const bool exactTarget = matchesRunningXochitl();
    if (!exactTarget) return reject("wrong-executable-profile");
    const auto *meta = controller->metaObject();
    const int workerProperty = meta->indexOfProperty("worker"), pageProperty = meta->indexOfProperty("pageId");
    if (workerProperty < 0 || pageProperty < 0) return reject("missing-properties");
    if (meta->property(pageProperty).read(controller).toString() != pageId) return reject("page-changed");
    QObject *worker = meta->property(workerProperty).read(controller).value<QObject*>();
    if (!worker || worker->thread() != QThread::currentThread()
        || QString::fromLatin1(worker->metaObject()->className()) != "DocumentWorker") return reject("worker-unavailable");
    const Mappings maps;
    const Word workerAddress = reinterpret_cast<Word>(worker);
    if (!maps.contains(workerAddress, 0x218, true)) return reject("worker-mapping");
    std::shared_ptr<Page> page;
    {
        TryLock guard(maps, workerAddress + 0x118);
        if (!guard) return reject("worker-busy");
        if (!activePage(maps, workerAddress, pageId)) return reject("page-not-loaded");
        const Word pageData = existingPageData(maps, workerAddress, pageId);
        Word payload = 0, control = 0, vtable = 0;
        if (!pageData || !maps.read(pageData + 0xd0, payload) || !maps.read(pageData + 0xd8, control))
            return reject("page-data-unavailable");
        if (!payload || !control) return reject("page-not-created");
        if (control % alignof(std::shared_ptr<Page>) || payload != control + 0x10
            || !maps.contains(control, 0xb0, true) || !maps.read(control, vtable) || vtable != 0x1548af8)
            return reject("wrong-page-control-type");
        qint32 strong = 0, weak = 0;
        if (!maps.read(control + 8, strong) || !maps.read(control + 12, weak)
            || strong <= 0 || weak <= 0 || strong == std::numeric_limits<qint32>::max())
            return reject("invalid-page-ownership");
        page = *reinterpret_cast<const std::shared_ptr<Page>*>(pageData + 0xd0);
    }
    {
        const Word pageAddress = reinterpret_cast<Word>(page.get());
        TryLock guard(maps, pageAddress + 0x68);
        if (!guard) return reject("page-busy");
        Word scene = 0, owner = 0, vtable = 0, qobjectData = 0;
        if (!maps.read(pageAddress, scene) || !maps.read(pageAddress + 0x98, owner) || owner)
            return reject("page-lock-state");
        if (!maps.contains(scene, 0x250, true) || !maps.read(scene, vtable) || vtable != 0x167f620
            || !maps.read(scene + 8, qobjectData) || !maps.contains(qobjectData, 0x40, true))
            return reject("wrong-scene-object");
        // Exact constructor passes this unchanged to QObjectC2 and installs
        // this vtable at +0. Its connectNotify/disconnectNotify slots are the
        // inherited QObject implementations, with no Page-lock re-entry.
        auto *object = reinterpret_cast<QObject*>(scene);
        if (QString::fromLatin1(object->metaObject()->className()) != "Scene"
            || !observer->connectScene(object)) return reject("missing-scene-change-signal");
    }
    // Only after native locks are released, recheck the QObject-origin page.
    if (meta->property(pageProperty).read(controller).toString() != pageId
        || meta->property(workerProperty).read(controller).value<QObject*>() != worker) {
        observer->disconnectScene();
        return reject("page-or-controller-changed");
    }
    if (reason) reason->clear();
    return true;
#endif
}

QVariantMap RePaperNative::observeOriginalSelection(QObject *controller,int layer) {
    QVariantMap result{{"source","original-selection-before-copy"},{"profile","ferrari-3.28.0.169"},
        {"nativeIdentityValidated",false},{"mutationsEnabled",false},{"complete",false}};
    auto reject=[&](const char *code){result.insert("status",QString::fromLatin1(code));return result;};
    if(!controller || controller->thread()!=QThread::currentThread())return reject("wrong-thread-or-controller");
    if(QString::fromLatin1(controller->metaObject()->className())!="SceneController")return reject("wrong-controller-type");
    if(layer<0)return reject("invalid-layer");
#if !defined(__aarch64__) || !defined(REPAPER_WITH_NATIVE_ABI)
    return reject("unsupported-build");
#else
    static const bool exactTarget=matchesRunningXochitl();
    if(!exactTarget)return reject("wrong-executable-profile");
    const auto listType=QMetaType::fromName("QList<std::shared_ptr<SceneItem>>");
    if(!listType.isValid()||listType.sizeOf()!=sizeof(QList<ItemPtr>))return reject("wrong-list-metatype");
    const auto *meta=controller->metaObject();
    const int workerProperty=meta->indexOfProperty("worker"),pageProperty=meta->indexOfProperty("pageId");
    const int countProperty=meta->indexOfProperty("selectionItemCount");
    if(workerProperty<0||pageProperty<0||countProperty<0)return reject("missing-properties");
    // ELF Qt dispatch establishes both getters as weak-handle lookup / QString
    // copy. No worker helper that creates a page is invoked.
    const auto workerValue=meta->property(workerProperty).read(controller);
    auto *worker=workerValue.value<QObject*>();
    const QString pageId=meta->property(pageProperty).read(controller).toString();
    const int expected=meta->property(countProperty).read(controller).toInt();
    if(!worker || worker->thread()!=QThread::currentThread()
       || QString::fromLatin1(worker->metaObject()->className())!="DocumentWorker")return reject("worker-unavailable");
    if(pageId.isEmpty()||pageId.size()>128||expected<0||expected>MaxItems)return reject("page-or-selection-limit");
    result.insert("pageIdSha256",QString::fromLatin1(QCryptographicHash::hash(pageId.toUtf8(),QCryptographicHash::Sha256).toHex()));
    result.insert("layer",layer);
    const Mappings maps;
    std::shared_ptr<Page> page;
    const Word workerAddress=reinterpret_cast<Word>(worker);
    if(!maps.contains(workerAddress,0x218,true))return reject("worker-mapping");
    {
        TryLock guard(maps,workerAddress+0x118);if(!guard)return reject("worker-busy");
        if(!activePage(maps,workerAddress,pageId))return reject("page-not-loaded");
        const Word pageData=existingPageData(maps,workerAddress,pageId);
        Word payload=0,control=0,vtable=0;
        if(!pageData||!maps.read(pageData+0xd0,payload)||!maps.read(pageData+0xd8,control))return reject("page-data-unavailable");
        if(!payload||!control)return reject("page-not-created");
        if(control%alignof(std::shared_ptr<Page>) || payload!=control+0x10 || !maps.contains(control,0xb0,true)
           ||!maps.read(control,vtable)||vtable!=0x1548af8)return reject("wrong-page-control-type");
        qint32 strong=0,weak=0;
        if(!maps.read(control+8,strong)||!maps.read(control+12,weak)||strong<=0||weak<=0
           ||strong==std::numeric_limits<qint32>::max())return reject("invalid-page-ownership");
        // Native RTTI proves std::_Sp_counted_ptr_inplace<::Page>. Retaining
        // the existing control block while worker is locked prevents eviction.
        page=*reinterpret_cast<const std::shared_ptr<Page>*>(pageData+0xd0);
    }
    QVariantList observations;qsizetype total=0,lines=0;
    {
    const Word pageAddress=reinterpret_cast<Word>(page.get());
    TryLock pageGuard(maps,pageAddress+0x68);if(!pageGuard)return reject("page-busy");
    Word scene=0,owner=0;
    if(!maps.read(pageAddress,scene)||!maps.read(pageAddress+0x98,owner)||owner)
        return reject("page-lock-state");
    // Do not call Page::get(), whose owner-thread marker would require a
    // mutation. Existing Scene is read while holding the same native mutex.
    if(!maps.contains(scene,0x250,true))return reject("scene-unavailable");
    qsizetype layerCount=0;Word layerData=0;quint64 layerId=0;
    const Word controllerAddress=reinterpret_cast<Word>(controller);
    if(!maps.read(controllerAddress+0x1a8,layerCount)||layer>=layerCount||layerCount>MaxPages
       ||!maps.read(controllerAddress+0x1a0,layerData)||!maps.contains(layerData,Word(layerCount)*0x28)
       ||!maps.read(layerData+Word(layer)*0x28+0x20,layerId))return reject("layer-bounds");
    if(!layerId)return reject("layer-unavailable");
    QList<HashPair> index,nodes;Word rootAddress=0;
    if(!hashPairs(maps,scene+0x248,index))return reject("scene-index-limit");
    for(const auto &entry:index)if(entry.key==layerId)rootAddress=entry.value;
    if(!rootAddress||!maps.contains(rootAddress,0x1e8)||!hashPairs(maps,rootAddress+0x198,nodes))return reject("root-node-unavailable");
    if(nodes.size()>=MaxNodes)return reject("node-limit");
    nodes.prepend({layerId,rootAddress});
    std::set<quint64> identities;std::set<Word> seenNodes;
    for(const auto &node:std::as_const(nodes)) {
        const Word address=node.value;qsizetype count=0;Word itemData=0;
        if(!seenNodes.insert(address).second||!maps.contains(address,0x1e8)||!itemListHeader(maps,address+0x1d0,count)
           ||!maps.read(address+0x1d8,itemData))return reject("selection-list-bounds");
        if(count>MaxItems-total)return reject("item-limit");
        // Page owns this graph and remains locked throughout the traversal.
        // No native lists or item shared_ptrs need to be copied or retained.
        for(qsizetype i=0;i<count;++i) {
            Word itemAddress=0;if(!maps.read(itemData+Word(i)*16,itemAddress))return reject("item-mapping");
            Word vtable=0;quint64 id=0,parent=0;quint8 tag=0;
            if(!itemAddress||!maps.read(itemAddress,vtable)||!maps.read(itemAddress+8,tag)
               ||!maps.read(itemAddress+0x10,id)||!maps.read(itemAddress+0x18,parent))return reject("item-mapping");
            // This first probe supports only real SceneLineItems. Other native
            // types can have adjusted bases; they are refused, never guessed.
            if(tag!=3||vtable!=0x1682940||!maps.contains(itemAddress,0xb0))return reject("unsupported-item-type");
            if(!id||!parent||!identities.insert(id).second)return reject("ambiguous-native-identity");
            observations.append(QVariantMap{{"idHex",QString::number(id,16).rightJustified(16,'0')},
                {"parentIdHex",QString::number(parent,16).rightJustified(16,'0')},{"type","SceneLineItem"}});
            ++total;++lines;
        }
    }
    }
    // Getters are read only after releasing the private mutex, so no Qt getter
    // can re-enter that lock. Reject a late page/worker/selection change.
    if(meta->property(pageProperty).read(controller).toString()!=pageId
       ||meta->property(workerProperty).read(controller).value<QObject*>()!=worker
       ||meta->property(countProperty).read(controller).toInt()!=expected)return reject("page-or-selection-changed");
    if(total!=expected)return reject("selection-count-mismatch");
    result.insert("status","observed");result.insert("complete",true);result.insert("count",total);
    result.insert("lineCount",lines);result.insert("items",observations);return result;
#endif
}
