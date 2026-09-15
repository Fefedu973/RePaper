#pragma once

#include <QObject>
#include <QMetaMethod>
#include <QPointer>
#include <QRectF>
#include <QSizeF>
#include <QTimer>
#include <QVariantMap>

// Prefills the native text document. Xochitl owns the text, history and storage.
class NativeAgendaText : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString reason READ reason NOTIFY changed)
public:
    explicit NativeAgendaText(QObject *parent = nullptr);
    bool busy() const { return m_phase != Phase::Idle; }
    QString reason() const { return m_reason; }
    Q_INVOKABLE bool prepare(const QString &request, QObject *controller,
                            const QVariantMap &context, const QVariantMap &fields, const QSizeF &pageSize);
    Q_INVOKABLE bool commit(const QString &request);
    Q_INVOKABLE void cancel(const QString &request);
signals:
    void prepared(const QString &request, const QVariantMap &receipt);
    void finished(const QString &request, bool success, const QVariantMap &receipt);
    void changed();
private slots:
    void advance();
private:
    enum class Phase { Idle, Prepared, Creating, Inserting, ReadingExisting, Verifying, RestoringForPrepare, RestoringForFinish };
    Phase m_phase = Phase::Idle;
    QPointer<QObject> m_controller, m_sceneView;
    QMetaObject::Connection m_destroyedConnection;
    QTimer m_poll;
    QVariantMap m_context;
    QRectF m_paper;
    QString m_request, m_reason, m_text, m_title, m_hash, m_legacyText, m_expectedBefore;
    qreal m_width = 0;
    bool m_existing = false;
    bool m_replacingLegacy = false, m_replacedLegacy = false;
    int m_ticks = 0;
    int m_readLength = 0;
    int m_savedCursor = 0;
    bool m_selectionRequested = false;
    QString m_readError;

    bool current() const;
    bool idleWorker() const;
    bool emptyPage() const;
    bool readExactText(QString *text) const;
    bool beginRead(Phase phase);
    void restoreSelection();
    void finishRead(const QString &error = {});
    bool titleStyle(int *value, QMetaMethod *create) const;
    bool refuse(const QString &code);
    void complete(bool success, const QString &code = {});
    QVariantMap receipt(const QString &state) const;
    void logState(const QString &event) const;
    void reset();
};
