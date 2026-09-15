#pragma once
#include "NativeHistorySnapshot.h"
#include <QColor>
#include <QByteArray>
#include <QObject>
#include <QString>
#include <QVector>
#include <functional>
#include <memory>

// Uses one exact-target worker callback and one native history macro. A native
// completion plus fresh selected geometry/color is an in-memory receipt, not a
// disk-write receipt or physical Undo/Redo certification.
class NativeSelectionColor : public QObject {
    Q_OBJECT
public:
    explicit NativeSelectionColor(QObject *parent = nullptr);
    ~NativeSelectionColor() override;
    virtual bool dispatch(QObject *controller, int layer, const QColor &color,
                  const RePaperNative::NativeHistorySnapshot &expectedHistory={});
    virtual bool busy() const;
    virtual QString reason() const;
    virtual RePaperNative::NativeHistorySnapshot beforeHistory() const;
    virtual RePaperNative::NativeHistorySnapshot afterHistory() const;
    virtual void cancel();
signals:
    void finished(bool success, const QString &message);
    void statusChanged();
private slots:
    void jobCompleted(qulonglong jobId);
    void verifyCompletion();
private:
    friend class NativeSelectionColorTest;
    struct Private;
    std::unique_ptr<Private> d;
    void finish(bool success, const QString &message);
};

namespace RePaperNative::ColorDetail {
bool nativeInsertAcceptsTool(int tool);
// Native Copy removes source IDs and relocation origins. Match its private
// world-coordinate clones to an exact multiset of observed native line bytes
// before restoring each source's persisted relocation lineage for recoloring.
QVector<quint64> matchedCloneLineages(const QVector<QByteArray> &sourceDigests,
                                    const QVector<quint64> &sourceLineages,
                                    const QVector<QByteArray> &cloneDigests);
// The same sequencing is exercised with deterministic native-operation fakes
// on the host. Every function runs within ONE already locked worker callback.
struct RecordedEditOperations {
    std::function<bool()> cancelled;
    std::function<qsizetype()> historyCount;
    std::function<bool()> eraseSelected;
    std::function<bool()> insertSelected;
    std::function<bool()> verifyInserted;
    std::function<void(int)> groupHistory;
    std::function<void()> undo;
};
struct RecordedEditResult {
    bool committed = false;
    bool compensated = false;
    QString reason;
    bool touchedScene = false;
};
RecordedEditResult runRecordedEdit(const RecordedEditOperations &operations);
}
