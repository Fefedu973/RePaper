#pragma once
#include "NativeObjectSnapshot.h"
#include "DrawingModel.h"
#include <QJsonObject>
#include <QHash>
#include <QSet>
#include <QTransform>

namespace RePaperNative {
struct NativeSelectionExpansion {
    bool valid = false;
    QVector<quint64> ids;
    QString reason;
};
struct NativeWireSnap {
    bool snapped = false;
    QPointF point;
    QString objectId, portId;
    bool wireSegment = false;
    qreal wirePosition = -1;
};

// Stores only explicit generated-object membership in native relocation
// lineages. Geometry revalidation confirms that each complete object still
// underwent one common affine transform; it never discovers ownership.
class NativeObjectBindings {
public:
    explicit NativeObjectBindings(QString directory);
    bool observe(const NativeObjectSnapshot &snapshot);
    bool registerInserted(const repaper::drawing::Item &item,
                          const NativeObjectSnapshot &snapshot,
                          const QVector<quint64> &insertedIds,
                          const repaper::drawing::StencilPlacementResult *placement = nullptr);
    bool registerDuplicate(const NativeObjectSnapshot &before,
                           const QVector<quint64> &beforeIds,
                           const NativeObjectSnapshot &after,
                           const QVector<quint64> &afterIds,
                           QPointF offset = {24, 24});
    bool selectedModel(const QVector<quint64> &ids, repaper::drawing::Item *item) const;
    bool registerReplacement(const NativeObjectSnapshot &before, const QVector<quint64> &beforeIds,
                             const NativeObjectSnapshot &after, const QVector<quint64> &afterIds,
                             const repaper::drawing::Item &item);
    // Only models whose complete native membership still reproduces their
    // semantic geometry are returned. IDs identify logical objects across Undo.
    repaper::drawing::Document documentModels() const;
    // A missing target may be deleted; an active target whose geometry became
    // unrecognizable must not silently detach another validated wire.
    bool connectionModelsReady() const;
    QVector<quint64> idsForObject(const QString &logicalId) const;
    bool registerReplacementsBatch(const NativeObjectSnapshot &before,
                                  const QVector<QVector<quint64>> &oldGroups,
                                  const NativeObjectSnapshot &after,
                                  const QVector<QVector<quint64>> &newGroups,
                                  const repaper::drawing::Document &models);
    NativeSelectionExpansion expandSelection(const QVector<quint64> &ids) const;
    NativeWireSnap snapWirePoint(QPointF point, qreal sceneTolerance,
                                const QString &excludedObject = {}) const;
    void invalidate();
    void clear();
    bool ready() const { return m_ready; }
    QString reason() const { return m_reason; }
    QByteArray fingerprint() const { return m_snapshot.fingerprint; }
    QString storagePath() const;
    int activeObjectCount() const { return m_resolved.size(); }
private:
    struct BoundObject {
        QString id, logicalId, kind;
        QVector<quint64> lineages;
        QVector<PaperDrawing::Polyline> references;
        QStringList portIds;
        QVector<QPointF> ports;
        PaperDrawing::Polyline wirePath;
        repaper::drawing::Item model;
        // These links were added by stencil placement without a new wire
        // lineage. They are active only while native endpoints still coincide.
        QStringList placementAttachedEndpoints;
        bool hasModel = false;
    };
    struct ResolvedObject {
        QString id, kind;
        QVector<quint64> ids;
        QStringList portIds;
        QVector<QPointF> ports;
        PaperDrawing::Polyline wirePath;
        repaper::drawing::Item model;
        QStringList placementAttachedEndpoints;
        bool hasModel = false;
    };
    bool activate(const NativeObjectSnapshot &snapshot);
    bool load();
    bool persist(const QVector<BoundObject> &objects);
    bool insertedBinding(const repaper::drawing::Item &item,
                         const NativeObjectSnapshot &snapshot,
                         const QVector<quint64> &insertedIds,
                         const QVector<BoundObject> &existing,
                         BoundObject *object);
    bool resolve(const NativeObjectSnapshot &snapshot);
    void rebuildIndexes();
    repaper::drawing::Item activeModel(const ResolvedObject &object) const;
    static QJsonObject encode(const BoundObject &object);
    static bool decode(const QJsonObject &json, BoundObject *object);
    QString m_directory, m_document, m_page, m_reason;
    quint64 m_layerId = 0;
    QByteArray m_fileDigest;
    bool m_ready = false, m_storageBlocked = false;
    QVector<BoundObject> m_objects;
    QVector<ResolvedObject> m_resolved;
    QVector<quint64> m_invalidManagedIds;
    NativeObjectSnapshot m_snapshot;
    repaper::drawing::Document m_models;
    QHash<QString, qsizetype> m_resolvedIndex;
    QHash<quint64, qsizetype> m_memberIndex;
    QSet<quint64> m_availableIds, m_invalidIds;
    QHash<QString, QStringList> m_dependents;
    bool m_connectionsReady = false;
    mutable QString m_snapExcludedObject;
    mutable QSet<QString> m_snapBlockedObjects;
};

// These helpers also work for ordinary native ink with no semantic binding.
// They resolve only already observed native relocation identities.
QVector<quint64> selectedNativeLineages(const NativeObjectSnapshot &snapshot,
                                      const QVector<quint64> &ids,
                                      QString *error = nullptr);
NativeSelectionExpansion resolveNativeLineages(const NativeObjectSnapshot &snapshot,
                                               const QVector<quint64> &lineages);
}
