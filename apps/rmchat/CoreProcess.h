#pragma once
#include <QJsonObject>
#include <QLocalSocket>
#include <QProcess>
#include <QTemporaryDir>
#include <functional>
#include <memory>
#include <utility>

namespace rmchat {
// A single owned child, private socket, no daemon. No credentials in argv or logs.
class CoreProcess {
public:
    using Progress = std::function<void(const QJsonObject &)>;
    CoreProcess() = default;
    ~CoreProcess();
    // The callback must be thread-safe; only this worker thread touches Qt I/O.
    void setCancellationCheck(std::function<bool()> check) { m_cancelled = std::move(check); }
    bool start(const QString &program, const QStringList &uploadRoots = {});
    QJsonObject call(const QString &method, const QJsonObject &params = {},
                     int timeoutMs = 120000, Progress progress = {});
    void stop();
    QString error() const { return m_error; }
private:
    bool fail(const QString &message);
    QProcess m_process;
    QLocalSocket m_socket;
    std::unique_ptr<QTemporaryDir> m_directory;
    QByteArray m_pending;
    QString m_error;
    quint64 m_nextId = 0;
    std::function<bool()> m_cancelled;
};
}
