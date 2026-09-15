#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace repaper::drawing {
// Supplied by a future verified native adapter. "id" is an opaque native identity,
// NEVER a pointer or geometry hash. "version" identifies the observed content:
// it must change on edits and match again when Undo restores that content.
struct NativeItemVersion {
    QString id, version;
    bool operator==(const NativeItemVersion &other) const { return id==other.id&&version==other.version; }
};
struct SemanticObject {
    QString id;
    QJsonObject parameters; // Already validated by the drawing model.
    bool operator==(const SemanticObject &other) const { return id==other.id&&parameters==other.parameters; }
};
struct NativeBinding {
    QString objectId;
    QVector<NativeItemVersion> items;
    bool operator==(const NativeBinding &other) const { return objectId==other.objectId&&items==other.items; }
};
struct RegistryTicket {
    QString epoch, request;
    bool valid() const { return !epoch.isEmpty()&&!request.isEmpty(); }
    bool operator==(const RegistryTicket &other) const { return epoch==other.epoch&&request==other.request; }
};

// Local, fail-closed bookkeeping only. It neither reads/writes .rm files nor
// validates an ABI. Loading a sidecar never enables editing without a fresh
// verified observation for the active page. All calls belong to one owner thread.
class NativeObjectRegistry {
public:
    explicit NativeObjectRegistry(QString directory);
    bool activate(const QString &documentId,const QString &pageId);
    QString epoch() const { return m_epoch; }
    bool editable() const { return m_confirmed&&!m_storageBlocked&&!m_pending.valid(); }
    bool pending() const { return m_pending.valid(); }
    QString error() const { return m_error; }
    QString storagePath() const;
    QVector<SemanticObject> objects() const;
    QVector<NativeBinding> bindings() const;

    // A proposal describes the COMPLETE managed object set after a transaction.
    // Confirm only after the native operation is observed, with its exact items.
    RegistryTicket begin(const QVector<SemanticObject> &proposed);
    bool confirm(const RegistryTicket &ticket,const QVector<NativeBinding> &bindings);
    bool cancel(const RegistryTicket &ticket);
    // Unattributed observations while a command is pending cancel that command.
    // The adapter must route its attributed completion to confirm(), not observe().
    // Unknown content/partial deletion/ambiguous histories stay locked.
    bool observe(const QString &epoch,const QVector<NativeItemVersion> &items);

private:
    struct Snapshot {
        QVector<SemanticObject> objects;
        QVector<NativeBinding> bindings;
        bool operator==(const Snapshot &other) const { return objects==other.objects&&bindings==other.bindings; }
    };
    bool load();
    bool persist(const QVector<Snapshot> &history,int current);
    static bool validateObjects(const QVector<SemanticObject> &objects);
    static bool validateBindings(const QVector<SemanticObject> &objects,const QVector<NativeBinding> &bindings);
    static QJsonObject encodeSnapshot(const Snapshot &snapshot);
    static bool decodeSnapshot(const QJsonObject &json,Snapshot *snapshot);
    QString m_directory,m_document,m_page,m_epoch,m_error;
    QVector<Snapshot> m_history;
    int m_current=0;
    bool m_confirmed=false,m_storageBlocked=false;
    RegistryTicket m_pending;
    QVector<SemanticObject> m_proposed;
    QByteArray m_fileDigest;
};
}
