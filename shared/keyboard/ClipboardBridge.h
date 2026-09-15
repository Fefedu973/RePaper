#pragma once
#include <QObject>
#include <QProcess>
#include <QTimer>

namespace repaper {
// Explicit paste only: never polls, logs, or writes either system clipboard.
class ClipboardBridge : public QObject {
    Q_OBJECT
public:
    explicit ClipboardBridge(QObject *parent=nullptr);
    virtual void request();
signals:
    void ready(const QString &text);
    void failed();
private:
    QProcess m_process;
    QTimer m_timeout;
    QByteArray m_output;
    bool m_pending=false;
    void fail();
};
}
