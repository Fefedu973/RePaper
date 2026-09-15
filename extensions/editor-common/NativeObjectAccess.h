#pragma once
#include "NativeObjectSnapshot.h"
#include <QObject>
#include <QPointF>
#include <QVariantMap>
#include <functional>
#include <memory>

// Exact-target access to actual active lines. Each operation owns one native
// DocumentWorker callback; a completed job is an in-memory receipt, not a save
// receipt. The caller keeps its input gate closed until finished/cancel.
class NativeObjectAccess : public QObject {
    Q_OBJECT
public:
    explicit NativeObjectAccess(QObject *parent = nullptr);
    ~NativeObjectAccess() override;
    virtual bool inspect(QObject *controller, const QVariantMap &context);
    virtual bool insert(QObject *controller, const QVariantMap &context,
                const QVariant &nativeItems, QPointF center);
    // Requires a complete inspection with exact selection and valid history.
    // Rechecks that baseline inside the native Page lock before any mutation.
    virtual bool insertIfUnchanged(QObject *controller, const QVariantMap &context,
                const RePaperNative::NativeObjectSnapshot &baseline,
                const QVariant &nativeItems, QPointF center);
    virtual bool replace(QObject *controller, const QVariantMap &context,
                const RePaperNative::NativeObjectSnapshot &baseline,
                const QVector<quint64> &exactIds, const QVariant &nativeItems, QPointF center);
    virtual bool select(QObject *controller, const QVariantMap &context,
                const RePaperNative::NativeObjectSnapshot &baseline,
                const QVector<quint64> &exactIds);
    virtual bool cancelPropertySession(QObject *controller, const QVariantMap &context,
                const RePaperNative::NativeObjectSnapshot &baseline,
                const RePaperNative::NativeObjectSnapshot &expectedCurrent);
    virtual bool busy() const;
    virtual QString reason() const;
    virtual QString operation() const;
    virtual RePaperNative::NativeObjectSnapshot result() const;
    virtual QVector<quint64> insertedIds() const;
    virtual void cancel();
signals:
    void finished(bool success);
    void statusChanged();
private slots:
    void jobCompleted(qulonglong jobId);
    void checkCompletion();
    void workerUnavailable();
    void controllerUnavailable();
private:
    friend class NativeObjectAccessTest;
    struct Private;
    std::unique_ptr<Private> d;
    bool dispatch(QObject *controller, const QVariantMap &context, const QString &operation,
                  const QVariant &items, QPointF center,
                  const RePaperNative::NativeObjectSnapshot &baseline,
                  const QVector<quint64> &exactIds,
                  const RePaperNative::NativeObjectSnapshot &expectedCurrent = {},
                  bool requireUnchanged = false);
    void finish(bool success, const QString &reason);
};

namespace RePaperNative::ObjectAccessDetail {
// A history change can represent native text or Undo even when all observed
// lines are unchanged. Guarded insertion therefore requires both proofs.
QString insertionBaselineError(const NativeObjectSnapshot &expected,
                               const NativeObjectSnapshot &actual);
// The same gate is used inside the locked callback and by host tests. It checks
// a complete immutable baseline before any native selection list is changed.
QString selectionRequestError(const NativeObjectSnapshot &expected,
                              const NativeObjectSnapshot &actual,
                              const QVector<quint64> &exactIds);
struct SelectionOperations {
    std::function<bool()> cancelled;
    std::function<NativeObjectSnapshot()> observe;
    std::function<bool()> prepare;
    std::function<void()> apply;
    std::function<bool()> verify;
    std::function<void()> restore;
};
struct SelectionResult {
    bool applied = false;
    bool touched = false;
    QString reason;
};
SelectionResult runSelection(const NativeObjectSnapshot &expected,
                             const QVector<quint64> &exactIds,
                             const SelectionOperations &operations);
constexpr int MaxPropertyCancelCommands = 512;
struct PropertyCancelOperations {
    std::function<bool()> cancelled;
    std::function<NativeObjectSnapshot()> observe;
    std::function<bool()> clearSelection;
    std::function<bool(const NativeHistoryCommand&)> undo;
};
struct PropertyCancelResult {
    bool cancelled = false, touched = false;
    int undone = 0;
    QString reason;
    NativeObjectSnapshot snapshot;
};
PropertyCancelResult runPropertyCancel(const NativeObjectSnapshot &baseline,
                                      const NativeObjectSnapshot &expectedCurrent,
                                      const PropertyCancelOperations &operations);
}
