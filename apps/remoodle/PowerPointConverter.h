#pragma once
#include <QObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <memory>

// A private, headless process converts the downloaded original. Only a completed
// PDF replaces the cache file; the resource's Moodle identity stays unchanged.
class PowerPointConverter : public QObject {
    Q_OBJECT
  public:
    explicit PowerPointConverter(QObject *parent = nullptr, int timeoutMs = 180000);
    ~PowerPointConverter() override;
    static QString extension(const QString &filename, const QString &mime = {});
    static bool matchesContent(const QByteArray &header, const QString &extension);
    static QString executable();
    bool busy() const { return bool(m_job); }
    bool convert(const QString &source, const QString &extension, const QString &destination);
    void cancel();
  signals:
    void converted(const QString &path);
    void failed(const QString &message);
  private:
    void finish(int exitCode, QProcess::ExitStatus status);
    void reject(const QString &message);
    QProcess m_process;
    QTimer m_timeout;
    std::unique_ptr<QTemporaryDir> m_job;
    QString m_destination;
    bool m_cancelled = false, m_timedOut = false, m_restarted = false;
};
