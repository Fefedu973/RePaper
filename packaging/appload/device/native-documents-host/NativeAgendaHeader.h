#pragma once

#include "NativeObjectSnapshot.h"
#include <QObject>
#include <QPointer>
#include <QSizeF>
#include <QVariantMap>

class NativeObjectAccess;

// Writes the P Day header as ordinary native ink, without library entries.
class NativeAgendaHeader : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString reason READ reason NOTIFY changed)
public:
    struct Plan {
        QVector<PaperDrawing::Stroke> strokes;
        QRectF paperBounds, inkBounds;
        QVariantMap fieldBounds, fieldRegions;
        QString hash, reason;
        bool valid() const { return reason.isEmpty() && !strokes.isEmpty() && hash.size() == 64; }
    };
    explicit NativeAgendaHeader(QObject *parent = nullptr);
    // Directory is retained for source compatibility and is never accessed.
    NativeAgendaHeader(QString source, QString directory, QByteArray sourceHash, QObject *parent = nullptr);
    ~NativeAgendaHeader() override;
    bool busy() const;
    QString reason() const { return m_reason; }
    Q_INVOKABLE QVariantMap ensure(const QVariantMap &fields);
    Q_INVOKABLE bool prepare(const QString &request, QObject *controller,
                            const QVariantMap &context, const QVariantMap &fields, const QSizeF &pageSize);
    Q_INVOKABLE bool commit(const QString &request);
    Q_INVOKABLE void cancel(const QString &request);
    static Plan createPlan(const QVariantMap &fields, const QRectF &paper, const QByteArray &fontData);
signals:
    void prepared(const QString &request, const QVariantMap &receipt);
    void finished(const QString &request, bool success, const QVariantMap &receipt);
    void changed();
protected:
    NativeAgendaHeader(NativeObjectAccess *access, QByteArray fontData,
                       QString source, QByteArray sourceHash, QObject *parent = nullptr);
    virtual QVariant nativeItems(const Plan &plan);
private:
    enum class Phase { Idle, Inspecting, Prepared, VerifyingExisting, Inserting };
    NativeObjectAccess *m_access = nullptr;
    QPointer<QObject> m_controller;
    QMetaObject::Connection m_destroyed;
    QString m_source, m_request, m_reason;
    QByteArray m_sourceHash, m_fontData;
    QVariantMap m_context;
    RePaperNative::NativeObjectSnapshot m_baseline;
    Plan m_plan;
    Phase m_phase = Phase::Idle;
    bool m_existing = false;
    int m_rootTextLength = 0, m_completionRetries = 0;
    quint64 m_epoch = 0;
    bool current() const;
    bool refuse(const QString &reason);
    void reset();
    void accessed(bool success);
    void complete(bool success, const QString &reason = {});
    QVariantMap receipt(const QString &state) const;
};
