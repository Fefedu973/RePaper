#pragma once
#include "CalendarEngine.h"
#include "SecretStore.h"
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QElapsedTimer>
#include <QHash>
#include <QPointer>
#include <QTimer>
#include <QObject>
#include <functional>

class AgendaController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString selectedDate READ selectedDate NOTIFY calendarChanged)
    Q_PROPERTY(QString periodLabel READ periodLabel NOTIFY calendarChanged)
    Q_PROPERTY(QString view READ view WRITE setView NOTIFY calendarChanged)
    Q_PROPERTY(QVariantList days READ days NOTIFY calendarChanged)
    Q_PROPERTY(QVariantList sources READ sources NOTIFY sourcesChanged)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool noteBusy READ noteBusy NOTIFY noteStateChanged)
    Q_PROPERTY(QString noteMessage READ noteMessage NOTIFY noteStateChanged)
    Q_PROPERTY(bool cpeConnected READ cpeConnected NOTIFY sourcesChanged)
    Q_PROPERTY(QVariantList grades READ grades NOTIFY schoolDataChanged)
    Q_PROPERTY(QVariantList absences READ absences NOTIFY schoolDataChanged)
  public:
    explicit AgendaController(QObject *parent = nullptr, QNetworkAccessManager *network = nullptr);
    QString selectedDate() const {
        return m_date.toString(Qt::ISODate);
    }
    QString periodLabel() const;
    QString view() const {
        return m_view;
    }
    QVariantList days() const;
    QVariantList sources() const;
    QString message() const {
        return m_message + (m_parseWarnings.isEmpty() ? QString() : " " + m_parseWarnings.join(' ')) +
               (m_storageError.isEmpty() ? QString() : " " + m_storageError);
    }
    bool busy() const {
        return m_pending > 0;
    }
    bool noteBusy() const { return m_noteBusy; }
    QString noteMessage() const { return m_noteMessage; }
    bool cpeConnected() const {
        return !m_cpeToken.isEmpty();
    }
    QVariantList grades() const;
    QVariantList absences() const;
    Q_INVOKABLE void setView(const QString &view);
    Q_INVOKABLE void selectDate(const QString &date);
    Q_INVOKABLE void shift(int direction);
    Q_INVOKABLE void today();
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void addIcs(const QString &label, const QString &location);
    Q_INVOKABLE void removeSource(const QString &id);
    Q_INVOKABLE void connectCpe(const QString &username, const QString &password);
    Q_INVOKABLE void disconnectCpe();
    Q_INVOKABLE void requestNote(const QString &date, const QString &eventId, const QString &title,
                                const QVariantMap &eventSnapshot = {});
    Q_INVOKABLE void reportBridgeError(const QString &message) {
        reportNoteProgress(message, false);
        setMessage(message);
    }
    void reportNoteProgress(const QString &message, bool busy);
    void setNoteHandler(std::function<bool(const QString &, const QString &, const QString &, const QJsonObject &)> handler) {
        m_noteHandler = std::move(handler);
    }
  signals:
    void calendarChanged();
    void sourcesChanged();
    void messageChanged();
    void busyChanged();
    void schoolDataChanged();
    void noteRequested(const QString &date, const QString &eventId, const QString &title);
    void noteStateChanged();

  private:
    using Completion = std::function<void(QByteArray, int, QString)>;
    QPair<QDate, QDate> range() const;
    QNetworkReply *request(const QUrl &url, const QByteArray &body, const QByteArray &token, Completion done);
    void scheduleRefresh(const QPair<QDate, QDate> &previousRange);
    void refreshCalendars(bool force);
    void cancelRefresh(const QString &id);
    void finishRefresh(const QString &id, quint64 generation);
    bool recentlyRefreshed(const QString &key) const;
    void refreshSource(const QString &id, bool force = false);
    void refreshCpe(const QString &id, bool force = false);
    void cpeWeek(const QString &id, const QDate &week, const QDate &last, const QDate &first,
                 QJsonArray collected, quint64 refreshGeneration);
    void refreshSchoolDetails();
    void save();
    void rebuild();
    void setMessage(const QString &message);
    void sourceError(const QString &id, const QString &message);
    QJsonObject source(const QString &id) const;
    void putSource(const QJsonObject &source);
    QDate m_date = QDate::currentDate();
    QString m_view = "week";
    QString m_message;
    QStringList m_parseWarnings;
    QString m_storageError;
    QJsonArray m_sources;
    QJsonArray m_grades;
    QJsonObject m_absences;
    QVector<ReAgenda::Event> m_events;
    QByteArray m_cpeToken;
    QJsonObject m_configuration;
    QNetworkAccessManager m_ownedNetwork;
    QNetworkAccessManager *m_network;
    QTimer m_refreshTimer;
    QElapsedTimer m_refreshClock;
    QHash<QString, quint64> m_refreshGenerations;
    QHash<QString, QPointer<QNetworkReply>> m_calendarReplies;
    QHash<QString, QString> m_refreshWindows;
    QHash<QString, qint64> m_lastRefresh;
    repaper::SecretStore m_secrets;
    int m_pending = 0;
    int m_cpeGeneration = 0;
    QString m_statePath;
    bool m_noteBusy = false;
    QString m_noteMessage;
    std::function<bool(const QString &, const QString &, const QString &, const QJsonObject &)> m_noteHandler;
};
