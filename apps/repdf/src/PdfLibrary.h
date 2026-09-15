#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <QVector>
#include <atomic>
#include <memory>

// Read-only PDF picker. Native metadata is never modified by this model.
class PdfLibrary : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString mode READ mode NOTIFY stateChanged)
    Q_PROPERTY(QString locationTitle READ locationTitle NOTIFY stateChanged)
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
    Q_PROPERTY(bool canGoBack READ canGoBack NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
public:
    enum Role {
        EntryIdRole = Qt::UserRole + 1, TitleRole, SubtitleRole, FilePathRole,
        FolderRole, AvailableRole
    };
    Q_ENUM(Role)

    explicit PdfLibrary(QString libraryRoot, QString filesRoot, QObject *parent = nullptr);
    ~PdfLibrary() override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString mode() const { return m_mode; }
    QString locationTitle() const { return m_locationTitle; }
    QString filter() const { return m_filter; }
    void setFilter(const QString &filter);
    bool canGoBack() const;
    bool busy() const { return m_busy; }
    QString error() const { return m_error; }
    int count() const { return int(m_rows.size()); }

    Q_INVOKABLE void showLibrary();
    Q_INVOKABLE void showFiles();
    Q_INVOKABLE void goUp();
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void openFolder(const QString &entryId);

signals:
    void stateChanged();
    void filterChanged();
    void countChanged();

private:
    struct Entry {
        QString id, title, subtitle, filePath, parent;
        bool folder = false, available = false;
    };
    struct ScanResult {
        QVector<Entry> entries;
        QString error;
    };
    using Cancellation = std::shared_ptr<std::atomic_bool>;
    static ScanResult scanLibrary(const QString &root, const Cancellation &cancelled);
    static ScanResult scanFiles(const QString &root, const QString &directory, const Cancellation &cancelled);
    void startScan();
    void applyFilter();
    void clearFilter();
    void updateLocationTitle();

    QString m_libraryRoot, m_filesRoot;
    QString m_mode = QStringLiteral("library"), m_location;
    QString m_locationTitle = QStringLiteral("Bibliothèque"), m_filter, m_error;
    bool m_busy = false;
    quint64 m_generation = 0;
    Cancellation m_cancelled;
    QVector<Entry> m_cache, m_rows;
    QHash<QString, Entry> m_nativeFolders;
};

