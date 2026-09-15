#include "NativeObjectRegistry.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <algorithm>

namespace repaper::drawing {
namespace {
constexpr qsizetype MaxFile=32*1024*1024;
constexpr int MaxObjects=3000,MaxItems=100000,MaxHistory=16;
bool identifier(const QString &value) {
    if(value.isEmpty()||value.size()>512)return false;
    for(auto c:value)if(c.isNull()||c.category()==QChar::Other_Control)return false;
    return true;
}
QString nonce(){return QUuid::createUuid().toString(QUuid::WithoutBraces);}
}
NativeObjectRegistry::NativeObjectRegistry(QString directory):m_directory(std::move(directory)){}
QString NativeObjectRegistry::storagePath() const {
    if(m_document.isEmpty()||m_page.isEmpty()||m_directory.isEmpty())return {};
    const auto key=QJsonDocument(QJsonArray{m_document,m_page}).toJson(QJsonDocument::Compact);
    return QDir(m_directory).filePath(QString::fromLatin1(QCryptographicHash::hash(key,QCryptographicHash::Sha256).toHex())+".registry.json");
}
bool NativeObjectRegistry::activate(const QString &documentId,const QString &pageId) {
    m_epoch=nonce();m_pending={};m_proposed.clear();m_confirmed=false;m_storageBlocked=false;m_error.clear();
    m_document=documentId;m_page=pageId;m_history={Snapshot{}};m_current=0;m_fileDigest.clear();
    if(m_directory.isEmpty()||!identifier(documentId)||!identifier(pageId)){m_storageBlocked=true;m_error="Invalid registry page or directory.";return false;}
    return load();
}
QVector<SemanticObject> NativeObjectRegistry::objects() const {return m_history.isEmpty()?QVector<SemanticObject>{}:m_history[m_current].objects;}
QVector<NativeBinding> NativeObjectRegistry::bindings() const {return m_history.isEmpty()?QVector<NativeBinding>{}:m_history[m_current].bindings;}
bool NativeObjectRegistry::validateObjects(const QVector<SemanticObject> &objects) {
    if(objects.size()>MaxObjects)return false;QSet<QString> ids;qsizetype bytes=0;
    for(const auto &object:objects) {
        if(!identifier(object.id)||ids.contains(object.id))return false;
        ids.insert(object.id);bytes+=QJsonDocument(object.parameters).toJson(QJsonDocument::Compact).size();
        if(bytes>8*1024*1024)return false;
    }
    return true;
}
bool NativeObjectRegistry::validateBindings(const QVector<SemanticObject> &objects,const QVector<NativeBinding> &bindings) {
    if(bindings.size()!=objects.size())return false;
    QSet<QString> expected,actual,nativeIds;
    for(const auto &object:objects)expected.insert(object.id);
    for(const auto &binding:bindings) {
        if(!expected.contains(binding.objectId)||actual.contains(binding.objectId)||binding.items.isEmpty())return false;
        actual.insert(binding.objectId);
        for(const auto &item:binding.items) {
            if(!identifier(item.id)||!identifier(item.version)||nativeIds.contains(item.id))return false;
            nativeIds.insert(item.id);if(nativeIds.size()>MaxItems)return false;
        }
    }
    return expected==actual;
}
RegistryTicket NativeObjectRegistry::begin(const QVector<SemanticObject> &proposed) {
    if(!editable()||!validateObjects(proposed)){m_error="The mapping is not confirmed, or the proposal is invalid.";return {};}
    m_pending={m_epoch,nonce()};m_proposed=proposed;m_error.clear();return m_pending;
}
bool NativeObjectRegistry::cancel(const RegistryTicket &ticket) {
    if(!ticket.valid()||!(ticket==m_pending))return false;
    m_pending={};m_proposed.clear();m_confirmed=false;
    m_error="Confirmation canceled; a fresh native observation is required.";return true;
}
bool NativeObjectRegistry::confirm(const RegistryTicket &ticket,const QVector<NativeBinding> &bindings) {
    if(!ticket.valid()||!(ticket==m_pending)||ticket.epoch!=m_epoch||m_storageBlocked)return false;
    if(!validateBindings(m_proposed,bindings)) {
        m_pending={};m_proposed.clear();m_confirmed=false;m_error="Native confirmation is incomplete or ambiguous.";return false;
    }
    Snapshot next{m_proposed,bindings};
    // Normalize ordering; iteration order from native containers is not identity.
    std::sort(next.objects.begin(),next.objects.end(),[](const auto &a,const auto &b){return a.id<b.id;});
    std::sort(next.bindings.begin(),next.bindings.end(),[](const auto &a,const auto &b){return a.objectId<b.objectId;});
    for(auto &binding:next.bindings)std::sort(binding.items.begin(),binding.items.end(),[](const auto &a,const auto &b){return a.id<b.id;});
    auto history=m_history;history.resize(m_current+1);
    if(!(history.last()==next))history.append(next);
    while(history.size()>MaxHistory)history.removeFirst();
    m_pending={};m_proposed.clear();m_confirmed=false;
    if(!persist(history,history.size()-1))return false;
    m_history=std::move(history);m_current=m_history.size()-1;m_confirmed=true;m_error.clear();return true;
}
bool NativeObjectRegistry::observe(const QString &epoch,const QVector<NativeItemVersion> &items) {
    if(epoch!=m_epoch||m_storageBlocked||m_epoch.isEmpty())return false;
    m_confirmed=false;
    if(m_pending.valid()){m_pending={};m_proposed.clear();m_error="Unattributed native change during a pending operation.";return false;}
    if(items.size()>MaxItems){m_error="Native observation exceeds the registry limit.";return false;}
    QHash<QString,QString> observed;
    for(const auto &item:items) {
        if(!identifier(item.id)||!identifier(item.version)||observed.contains(item.id)){m_error="Ambiguous native observation.";return false;}
        observed.insert(item.id,item.version);
    }
    // Consider only identities that this registry has actually managed. Unrelated
    // handwriting on the same page does not acquire a fabricated semantic record.
    QSet<QString> known,present;
    for(const auto &snapshot:m_history)for(const auto &binding:snapshot.bindings)for(const auto &item:binding.items)known.insert(item.id);
    for(const auto &id:known)if(observed.contains(id))present.insert(id);
    int match=-1;
    for(int i=0;i<m_history.size();++i) {
        QSet<QString> required;bool versionsMatch=true;
        for(const auto &binding:m_history[i].bindings)for(const auto &item:binding.items) {
            required.insert(item.id);if(observed.value(item.id)!=item.version)versionsMatch=false;
        }
        if(!versionsMatch||required!=present)continue;
        if(match>=0&&!(m_history[match]==m_history[i])){m_error="Multiple semantic states match the native observation.";return false;}
        match=i;
    }
    if(match<0){m_error="Unknown native edit or partial deletion; semantic editing remains locked.";return false;}
    if(match!=m_current&&!persist(m_history,match))return false;
    m_current=match;m_confirmed=true;m_error.clear();return true;
}
QJsonObject NativeObjectRegistry::encodeSnapshot(const Snapshot &snapshot) {
    QJsonArray objects,bindings;
    for(const auto &object:snapshot.objects)objects.append(QJsonObject{{"id",object.id},{"parameters",object.parameters}});
    for(const auto &binding:snapshot.bindings) {
        QJsonArray items;for(const auto &item:binding.items)items.append(QJsonObject{{"id",item.id},{"version",item.version}});
        bindings.append(QJsonObject{{"objectId",binding.objectId},{"items",items}});
    }
    return {{"objects",objects},{"bindings",bindings}};
}
bool NativeObjectRegistry::decodeSnapshot(const QJsonObject &json,Snapshot *snapshot) {
    if(!json["objects"].isArray()||!json["bindings"].isArray())return false;
    if(json["objects"].toArray().size()>MaxObjects||json["bindings"].toArray().size()>MaxObjects)return false;
    for(auto value:json["objects"].toArray()) {
        if(!value.isObject())return false;const auto object=value.toObject();
        if(!object["id"].isString()||!object["parameters"].isObject())return false;
        snapshot->objects.append({object["id"].toString(),object["parameters"].toObject()});
    }
    int count=0;
    for(auto value:json["bindings"].toArray()) {
        if(!value.isObject())return false;const auto object=value.toObject();
        if(!object["objectId"].isString()||!object["items"].isArray())return false;
        NativeBinding binding;binding.objectId=object["objectId"].toString();
        for(auto itemValue:object["items"].toArray()) {
            if(++count>MaxItems||!itemValue.isObject())return false;const auto item=itemValue.toObject();
            if(!item["id"].isString()||!item["version"].isString())return false;
            binding.items.append({item["id"].toString(),item["version"].toString()});
        }
        snapshot->bindings.append(binding);
    }
    return validateObjects(snapshot->objects)&&validateBindings(snapshot->objects,snapshot->bindings);
}
bool NativeObjectRegistry::persist(const QVector<Snapshot> &history,int current) {
    QJsonArray states;for(const auto &snapshot:history)states.append(encodeSnapshot(snapshot));
    const auto bytes=QJsonDocument(QJsonObject{{"schemaVersion",1},{"documentId",m_document},{"pageId",m_page},{"current",current},{"history",states}}).toJson(QJsonDocument::Compact);
    if(bytes.size()>MaxFile){m_storageBlocked=true;m_error="Registry storage limit reached; existing state preserved.";return false;}
    if(!QDir().mkpath(m_directory)){m_storageBlocked=true;m_error="Cannot create the registry directory.";return false;}
    QLockFile lock(storagePath()+".lock");
    if(!lock.tryLock(0)){m_storageBlocked=true;m_error="Another registry writer is active; semantic editing remains locked.";return false;}
    QFile previous(storagePath());QByteArray digest;
    if(previous.exists()) {
        if(previous.size()>MaxFile||!previous.open(QIODevice::ReadOnly)){m_storageBlocked=true;m_error="Cannot verify the previous registry state.";return false;}
        digest=QCryptographicHash::hash(previous.readAll(),QCryptographicHash::Sha256);
    }
    if(digest!=m_fileDigest){m_storageBlocked=true;m_error="Registry changed outside this session; reload and verify the native scene.";return false;}
    QSaveFile file(storagePath());file.setDirectWriteFallback(false);
    if(!file.open(QIODevice::WriteOnly)||!file.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner)||file.write(bytes)!=bytes.size()||!file.commit()) {
        m_storageBlocked=true;m_error="Registry save failed; semantic editing remains locked.";return false;
    }
    m_fileDigest=QCryptographicHash::hash(bytes,QCryptographicHash::Sha256);
    return true;
}
bool NativeObjectRegistry::load() {
    QFile file(storagePath());if(!file.exists())return true;
    m_storageBlocked=true;m_error="Invalid registry file preserved; semantic editing remains locked.";
    if(file.size()>MaxFile||!file.open(QIODevice::ReadOnly))return false;
    const auto bytes=file.readAll();QJsonParseError error;const auto doc=QJsonDocument::fromJson(bytes,&error);
    if(error.error!=QJsonParseError::NoError||!doc.isObject())return false;
    const auto root=doc.object();
    if(root["schemaVersion"]!=QJsonValue(1)||root["documentId"].toString()!=m_document||root["pageId"].toString()!=m_page||!root["history"].isArray())return false;
    const auto states=root["history"].toArray();const auto cursor=root["current"].toDouble(-1);
    if(states.isEmpty()||states.size()>MaxHistory||cursor<0||cursor>=states.size()||cursor!=int(cursor))return false;
    QVector<Snapshot> history;
    for(auto state:states){Snapshot snapshot;if(!state.isObject()||!decodeSnapshot(state.toObject(),&snapshot))return false;history.append(snapshot);}
    m_history=std::move(history);m_current=int(cursor);m_fileDigest=QCryptographicHash::hash(bytes,QCryptographicHash::Sha256);m_storageBlocked=false;m_error.clear();return true;
}
}
