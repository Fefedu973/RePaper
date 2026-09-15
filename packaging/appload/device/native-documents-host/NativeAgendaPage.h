#pragma once
#include "NativeAgendaLayout.h"
#include <QObject>
#include <QPointer>
#include <QSizeF>

class NativeObjectAccess;

class NativeAgendaPage : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString reason READ reason NOTIFY changed)
public:
    explicit NativeAgendaPage(QObject *parent = nullptr);
    ~NativeAgendaPage() override;
    bool busy() const;
    QString reason() const { return m_reason; }
    Q_INVOKABLE bool prepare(const QString &request, QObject *controller,
                            const QVariantMap &context, const QVariantMap &fields, const QSizeF &pageSize);
    Q_INVOKABLE bool commit(const QString &request);
    Q_INVOKABLE void cancel(const QString &request);
signals:
    void prepared(const QString &request, const QVariantMap &receipt);
    void finished(const QString &request, bool success, const QVariantMap &receipt);
    void changed();
protected:
    // Test seams substitute observation/allocation, never enable a foreign ABI.
    NativeAgendaPage(NativeObjectAccess *access, QByteArray fontData, QObject *parent);
    virtual QVariant nativeItems(const NativeAgendaLayout::Plan &plan);
private:
    enum class Phase { Idle, Inspecting, Prepared, VerifyingExisting, Inserting };
    NativeObjectAccess *m_access = nullptr;
    QPointer<QObject> m_controller;
    QMetaObject::Connection m_destroyed;
    QByteArray m_fontData;
    QVariantMap m_context;
    RePaperNative::NativeObjectSnapshot m_baseline;
    NativeAgendaLayout::Plan m_plan;
    Phase m_phase = Phase::Idle;
    QString m_request, m_reason;
    bool m_existing = false;
    quint64 m_epoch = 0;
    int m_completionRetries = 0;
    void accessed(bool success);
    bool current() const;
    bool refuse(const QString &code);
    void complete(bool success, const QString &code = {});
    QVariantMap receipt(const QString &state) const;
    void reset();
};
