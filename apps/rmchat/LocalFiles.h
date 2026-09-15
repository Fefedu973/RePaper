#pragma once
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QUrl>
#include <QVariantList>

namespace rmchat {
class LocalFiles : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString mode READ mode WRITE setMode NOTIFY changed)
    Q_PROPERTY(QUrl folder READ folder WRITE openFolder NOTIFY changed)
    Q_PROPERTY(QVariantList entries READ entries NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(bool canGoUp READ canGoUp NOTIFY changed)
    Q_PROPERTY(bool pcIntegration READ pcIntegration CONSTANT)
public:
    explicit LocalFiles(QObject *parent = nullptr);
    ~LocalFiles() override;
    QString mode() const { return m_mode; }
    QUrl folder() const { return QUrl::fromLocalFile(m_folder); }
    QVariantList entries() const { return m_entries; }
    QString message() const { return m_message; }
    bool canGoUp() const;
    bool pcIntegration() const;
    void setMode(const QString &mode);
    Q_INVOKABLE void openFolder(const QUrl &folder);
    Q_INVOKABLE void home();
    Q_INVOKABLE void up();
    Q_INVOKABLE void openBrowser(bool sessionPage = false);
    Q_INVOKABLE void openLink(const QUrl &url);
    Q_INVOKABLE void openConversation(const QString &id);
    Q_INVOKABLE void copyText(const QString &text);
signals:
    void changed();
private:
    void refresh();
    void launchUrl(const QUrl &url);
    QString m_mode = "session", m_folder, m_home, m_message;
    QVariantList m_entries;
    QPointer<QProcess> m_browser;
    QPointer<QProcess> m_clipboard;
};
}
