#include "PdfLibrary.h"

#include <QCollator>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QSet>
#include <QUuid>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>

namespace {
constexpr qint64 MaxJsonBytes = 256 * 1024;
constexpr int MaxEntries = 10000;

QString normalizedRoot(const QString &path) {
    if (path.isEmpty()) return {};
    const QFileInfo info(path);
    const auto canonical = info.canonicalFilePath();
    return QDir::cleanPath(QDir::fromNativeSeparators(canonical.isEmpty() ? info.absoluteFilePath() : canonical));
}
bool inRoot(const QString &path, const QString &root) {
#ifdef Q_OS_WIN
    constexpr auto sensitivity = Qt::CaseInsensitive;
#else
    constexpr auto sensitivity = Qt::CaseSensitive;
#endif
    const auto prefix = root.endsWith('/') ? root : root + '/';
    return path.compare(root, sensitivity) == 0 || path.startsWith(prefix, sensitivity);
}
bool rootAvailable(const QString &root) {
    const QFileInfo info(root);
    return info.isDir() && info.isReadable() && info.canonicalFilePath() == root;
}
QString nativeId(const QString &value) {
    const QUuid uuid(value);
    const auto id = uuid.toString(QUuid::WithoutBraces);
    return !uuid.isNull() && id.compare(value, Qt::CaseInsensitive) == 0 ? id : QString{};
}
enum class JsonState { Missing, Invalid, Valid };
JsonState readJson(const QString &path, const QString &root, QJsonObject *object) {
    const QFileInfo info(path);
    if (info.isSymLink()) return JsonState::Invalid;
    if (!info.exists()) return JsonState::Missing;
    if (!info.isFile() || info.size() > MaxJsonBytes || !inRoot(info.canonicalFilePath(), root)) return JsonState::Invalid;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return JsonState::Invalid;
    const auto bytes = file.read(MaxJsonBytes + 1);
    if (bytes.size() > MaxJsonBytes || !file.atEnd()) return JsonState::Invalid;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return JsonState::Invalid;
    *object = document.object();
    return JsonState::Valid;
}
QString nativePdfPath(const QString &path, const QString &root) {
    const QFileInfo info(path);
    if (!info.isFile() || info.isSymLink() || !info.isReadable()) return {};
    const auto canonical = info.canonicalFilePath();
    return inRoot(canonical, root) ? canonical : QString{};
}
}

PdfLibrary::PdfLibrary(QString libraryRoot, QString filesRoot, QObject *parent)
    : QAbstractListModel(parent), m_libraryRoot(normalizedRoot(libraryRoot)), m_filesRoot(normalizedRoot(filesRoot)) {
    startScan();
}
PdfLibrary::~PdfLibrary() {
    if (m_cancelled) m_cancelled->store(true, std::memory_order_relaxed);
}
int PdfLibrary::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : count(); }
QVariant PdfLibrary::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.column() != 0 || index.row() < 0 || index.row() >= m_rows.size()) return {};
    const auto &entry = m_rows[index.row()];
    switch (role) {
    case EntryIdRole: return entry.id;
    case Qt::DisplayRole:
    case TitleRole: return entry.title;
    case SubtitleRole: return entry.subtitle;
    case FilePathRole: return entry.filePath;
    case FolderRole: return entry.folder;
    case AvailableRole: return entry.available;
    default: return {};
    }
}
QHash<int, QByteArray> PdfLibrary::roleNames() const {
    return {{EntryIdRole,"entryId"},{TitleRole,"title"},{SubtitleRole,"subtitle"},
        {FilePathRole,"filePath"},{FolderRole,"folder"},{AvailableRole,"available"}};
}
bool PdfLibrary::canGoBack() const {
    return m_mode == "library" ? !m_location.isEmpty() : !m_location.isEmpty() && m_location != m_filesRoot;
}
void PdfLibrary::setFilter(const QString &filter) {
    if (m_filter == filter) return;
    m_filter = filter;
    applyFilter();
    emit filterChanged();
}
void PdfLibrary::clearFilter() {
    if (m_filter.isEmpty()) return;
    m_filter.clear();
    emit filterChanged();
}
void PdfLibrary::updateLocationTitle() {
    if (m_mode == "library")
        m_locationTitle = m_location.isEmpty() ? QStringLiteral("Bibliothèque")
            : m_nativeFolders.value(m_location).title;
    else m_locationTitle = m_location == m_filesRoot ? QStringLiteral("Fichiers") : QFileInfo(m_location).fileName();
    if (m_locationTitle.isEmpty()) m_locationTitle = QStringLiteral("Dossier");
}
void PdfLibrary::showLibrary() {
    m_mode = QStringLiteral("library");m_location.clear();clearFilter();updateLocationTitle();startScan();
}
void PdfLibrary::showFiles() {
    m_mode = QStringLiteral("files");m_location = m_filesRoot;clearFilter();updateLocationTitle();startScan();
}
void PdfLibrary::goUp() {
    if (!canGoBack()) return;
    if (m_mode == "library") m_location = m_nativeFolders.value(m_location).parent;
    else {
        QDir parent(m_location);parent.cdUp();
        const auto path = parent.canonicalPath();
        m_location = !path.isEmpty() && inRoot(path,m_filesRoot) ? path : m_filesRoot;
    }
    clearFilter();updateLocationTitle();startScan();
}
void PdfLibrary::refresh() { startScan(); }
void PdfLibrary::openFolder(const QString &entryId) {
    const auto found = std::find_if(m_rows.cbegin(),m_rows.cend(),[&](const Entry &entry) {
        return entry.id == entryId && entry.folder && entry.available;
    });
    if (found == m_rows.cend()) return;
    if (m_mode == "files") {
        const QFileInfo info(entryId);
        if (!info.isDir() || info.canonicalFilePath() != entryId || !inRoot(entryId,m_filesRoot)) {
            m_error = QStringLiteral("Ce dossier n’est plus disponible.");emit stateChanged();return;
        }
    }
    m_location = found->id;
    clearFilter();updateLocationTitle();startScan();
}

void PdfLibrary::startScan() {
    if (m_cancelled) m_cancelled->store(true,std::memory_order_relaxed);
    const auto cancelled = m_cancelled = std::make_shared<std::atomic_bool>(false);
    const quint64 generation = ++m_generation;
    const auto mode = m_mode, location = m_location;
    const auto libraryRoot = m_libraryRoot, filesRoot = m_filesRoot;
    m_busy = true;m_error.clear();m_cache.clear();applyFilter();emit stateChanged();
    auto *watcher = new QFutureWatcher<ScanResult>(this);
    connect(watcher,&QFutureWatcher<ScanResult>::finished,this,[this,watcher,generation,mode,location] {
        auto result = watcher->result();watcher->deleteLater();
        if (generation != m_generation || mode != m_mode || location != m_location) return;
        m_cache = std::move(result.entries);m_error = result.error;
        if (m_mode == "library") {
            m_nativeFolders.clear();
            for (const auto &entry : m_cache) if (entry.folder) m_nativeFolders.insert(entry.id,entry);
            if (!m_location.isEmpty() && !m_nativeFolders.contains(m_location)) {
                m_location.clear();
                if (m_error.isEmpty()) m_error = QStringLiteral("Ce dossier n’est plus disponible.");
            }
        }
        m_busy = false;updateLocationTitle();applyFilter();emit stateChanged();
    });
    watcher->setFuture(QtConcurrent::run([mode,location,libraryRoot,filesRoot,cancelled] {
        return mode == "library" ? scanLibrary(libraryRoot,cancelled) : scanFiles(filesRoot,location,cancelled);
    }));
}
void PdfLibrary::applyFilter() {
    QVector<Entry> rows;
    const auto query = m_filter.trimmed();
    for (const auto &entry : m_cache) {
        if (!query.isEmpty()) {
            if (!entry.title.contains(query,Qt::CaseInsensitive)) continue;
        } else if (m_mode == "library" && entry.parent != m_location) continue;
        rows.append(entry);
    }
    QCollator collator(QLocale(QLocale::French,QLocale::France));
    collator.setCaseSensitivity(Qt::CaseInsensitive);collator.setNumericMode(true);
    std::sort(rows.begin(),rows.end(),[&](const Entry &a,const Entry &b) {
        if (a.folder != b.folder) return a.folder;
        const auto order = collator.compare(a.title,b.title);
        return order != 0 ? order < 0 : a.id < b.id;
    });
    const auto previous = m_rows.size();
    beginResetModel();m_rows = std::move(rows);endResetModel();
    if (previous != m_rows.size()) emit countChanged();
}

PdfLibrary::ScanResult PdfLibrary::scanLibrary(const QString &root,const Cancellation &cancelled) {
    ScanResult result;
    if (!rootAvailable(root)) {result.error = QStringLiteral("La bibliothèque native est indisponible.");return result;}
    struct Node {Entry entry;bool deleted = false, validParent = true, pdf = false;};
    QHash<QString,Node> nodes;QSet<QString> ambiguous;
    QDirIterator iterator(root,{QStringLiteral("*.metadata")},QDir::Files|QDir::NoSymLinks);
    int scanned = 0;
    while (iterator.hasNext()) {
        if (cancelled->load(std::memory_order_relaxed)) return {};
        if (scanned++ >= MaxEntries) {result.error = QStringLiteral("Affichage limité à 10 000 éléments de bibliothèque.");break;}
        iterator.next();const auto info = iterator.fileInfo();
        const auto stem = info.completeBaseName(), id = nativeId(stem);
        if (id.isEmpty()) continue;
        if (ambiguous.contains(id)) continue;
        if (nodes.contains(id)) {nodes.remove(id);ambiguous.insert(id);continue;}
        QJsonObject metadata;
        if (readJson(info.filePath(),root,&metadata) != JsonState::Valid) continue;
        const auto type = metadata.value("type").toString();
        if (type != "CollectionType" && type != "DocumentType") continue;
        Node node;node.entry.id = id;node.entry.folder = type == "CollectionType";
        node.deleted = metadata.value("deleted").toBool();
        const auto parent = metadata.value("parent");
        if (!parent.isUndefined() && !parent.isString()) node.validParent = false;
        else if (parent.toString().compare(QStringLiteral("trash"),Qt::CaseInsensitive) == 0) node.validParent = false;
        else if (!parent.toString().isEmpty()) {
            node.entry.parent = nativeId(parent.toString());node.validParent = !node.entry.parent.isEmpty();
        }
        node.entry.title = metadata.value("visibleName").toString().left(1024).simplified();
        if (node.entry.title.isEmpty()) node.entry.title = node.entry.folder ? QStringLiteral("Dossier sans nom") : QStringLiteral("PDF sans titre");
        node.entry.available = node.entry.folder;
        if (!node.entry.folder) {
            QJsonObject content;
            const auto contentState = readJson(root+'/'+stem+".content",root,&content);
            const auto path = nativePdfPath(root+'/'+stem+".pdf",root);
            const bool hasType = contentState == JsonState::Valid && content.contains("fileType");
            node.pdf = contentState == JsonState::Valid && hasType && content.value("fileType").toString().compare("pdf",Qt::CaseInsensitive) == 0;
            if (!hasType && contentState != JsonState::Invalid && !path.isEmpty()) node.pdf = true;
            node.entry.filePath = path;node.entry.available = !path.isEmpty();
        }
        nodes.insert(id,std::move(node));
    }
    // Memoized iterative ancestry validation stays bounded for deeply nested
    // trees and cycles; a document can never serve as a logical folder.
    QHash<QString,bool> visibility;
    const auto visible = [&](const QString &id) {
        QString cursor = id;QVector<QString> chain;QSet<QString> seen;bool allowed = false;
        while (!cursor.isEmpty()) {
            if (cancelled->load(std::memory_order_relaxed)) return false;
            if (visibility.contains(cursor)) {allowed = visibility.value(cursor);break;}
            const auto found = nodes.constFind(cursor);
            if (found == nodes.cend() || seen.contains(cursor)) break;
            seen.insert(cursor);chain.append(cursor);
            if (found->deleted || !found->validParent) break;
            if (found->entry.parent.isEmpty()) {allowed = true;break;}
            const auto parent = nodes.constFind(found->entry.parent);
            if (parent == nodes.cend() || !parent->entry.folder) break;
            cursor = parent->entry.id;
        }
        for (const auto &part : chain) visibility.insert(part,allowed);
        return allowed;
    };
    for (auto it = nodes.cbegin();it != nodes.cend();++it) {
        if (cancelled->load(std::memory_order_relaxed)) return {};
        if (!visible(it.key()) || (!it->entry.folder && !it->pdf)) continue;
        auto entry = it->entry;
        QStringList ancestors;QString parent = entry.parent;int characters = 0;
        while (!parent.isEmpty() && ancestors.size() < 32 && characters < 2048) {
            const auto ancestor = nodes.constFind(parent);
            if (ancestor == nodes.cend()) break;
            ancestors.prepend(ancestor->entry.title);characters += ancestor->entry.title.size();parent = ancestor->entry.parent;
        }
        if (!parent.isEmpty()) ancestors.prepend(QStringLiteral("…"));
        entry.subtitle = entry.folder ? QStringLiteral("Dossier")
            : entry.available ? QStringLiteral("PDF") : QStringLiteral("À télécharger…");
        if (!ancestors.isEmpty()) entry.subtitle += QStringLiteral(" · ") + ancestors.join(QStringLiteral(" / "));
        result.entries.append(std::move(entry));
    }
    return result;
}

PdfLibrary::ScanResult PdfLibrary::scanFiles(const QString &root,const QString &directory,const Cancellation &cancelled) {
    ScanResult result;
    const QFileInfo current(directory);
    const auto canonical = current.canonicalFilePath();
    if (!rootAvailable(root) || !current.isDir() || !current.isReadable() || canonical != directory || !inRoot(canonical,root)) {
        result.error = QStringLiteral("Ce dossier de fichiers est indisponible.");return result;
    }
    QDirIterator iterator(directory,QDir::Dirs|QDir::Files|QDir::NoDotAndDotDot);
    const QLocale french(QLocale::French,QLocale::France);int scanned = 0;
    while (iterator.hasNext()) {
        if (cancelled->load(std::memory_order_relaxed)) return {};
        if (scanned++ >= MaxEntries) {result.error = QStringLiteral("Affichage limité à 10 000 fichiers et dossiers.");break;}
        iterator.next();const auto info = iterator.fileInfo();
        if (info.isHidden() || info.fileName().startsWith('.')) continue;
        const auto path = info.canonicalFilePath();
        if (path.isEmpty() || !inRoot(path,root)) continue;
        const bool folder = info.isDir();
        if (!folder && (!info.isFile() || info.suffix().compare("pdf",Qt::CaseInsensitive) != 0)) continue;
        Entry entry;entry.id = path;entry.title = info.fileName();entry.parent = directory;
        entry.folder = folder;entry.available = info.isReadable();
        entry.subtitle = folder ? QStringLiteral("Dossier") : QStringLiteral("PDF · ") + french.formattedDataSize(info.size());
        if (!folder) entry.filePath = path;
        result.entries.append(std::move(entry));
    }
    return result;
}
