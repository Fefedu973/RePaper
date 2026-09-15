#include "LocalFiles.h"
#include <QDesktopServices>
#include <QClipboard>
#include <QGuiApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>

namespace rmchat {
LocalFiles::LocalFiles(QObject *parent) : QObject(parent) {
    m_home = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (pcIntegration()) {
        const auto match = QRegularExpression("^(/mnt/[a-z]/Users/[^/]+)/")
            .match(qEnvironmentVariable("REPAPER_PC_HANDOFF_HELPER"));
        if (match.hasMatch() && QDir(match.captured(1) + "/Downloads").exists())
            m_home = match.captured(1) + "/Downloads";
    }
    if (!QDir(m_home).exists()) m_home = QDir::homePath();
    m_folder = m_home;
    // List only when the user opens the picker; startup never enumerates files.
}
LocalFiles::~LocalFiles() {
    if (m_browser) { m_browser->kill(); m_browser->waitForFinished(1500); }
    if (m_clipboard) { m_clipboard->kill(); m_clipboard->waitForFinished(1500); }
}
bool LocalFiles::pcIntegration() const {
    const QFileInfo helper(qEnvironmentVariable("REPAPER_PC_HANDOFF_HELPER"));
    return qEnvironmentVariable("REPAPER_PC_EMULATOR") == "1" && helper.isAbsolute() && helper.isFile();
}
bool LocalFiles::canGoUp() const { return !m_folder.isEmpty() && !QDir(m_folder).isRoot(); }
void LocalFiles::setMode(const QString &mode) {
    if (mode != "session" && mode != "pdf") return;
    m_mode = mode;
    refresh();
}
void LocalFiles::home() { openFolder(QUrl::fromLocalFile(m_home)); }
void LocalFiles::up() {
    QDir directory(m_folder);
    if (directory.cdUp()) openFolder(QUrl::fromLocalFile(directory.absolutePath()));
}
void LocalFiles::openFolder(const QUrl &folder) {
    if (!folder.isLocalFile()) return;
    const QFileInfo info(folder.toLocalFile());
    if (!info.isDir() || !info.isReadable()) { m_message = "Dossier inaccessible."; emit changed(); return; }
    m_folder = info.canonicalFilePath();
    refresh();
}
void LocalFiles::refresh() {
    m_entries.clear();
    m_message.clear();
    const QDir directory(m_folder);
    const auto files = directory.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::Readable,
                                                QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
    for (const auto &file : files) {
        if (file.isSymLink()) continue;
        if (!file.isDir() && file.suffix().compare(m_mode == "pdf" ? "pdf" : "json", Qt::CaseInsensitive) != 0) continue;
        if (m_entries.size() >= 1000) { m_message = "Les 1 000 premiers éléments sont affichés."; break; }
        m_entries.append(QVariantMap{{"name", file.fileName()}, {"url", QUrl::fromLocalFile(file.absoluteFilePath())},
                                     {"isDirectory", file.isDir()}, {"size", file.isDir() ? 0 : file.size()}});
    }
    if (m_entries.isEmpty()) m_message = m_mode == "pdf" ? "Aucun PDF dans ce dossier." : "Aucun fichier JSON dans ce dossier.";
    emit changed();
}
void LocalFiles::openBrowser(bool sessionPage) {
    const QUrl url(sessionPage ? "https://chatgpt.com/api/auth/session" : "https://chatgpt.com/");
    launchUrl(url);
}
void LocalFiles::openConversation(const QString &id) {
    if (!id.isEmpty() && !QRegularExpression("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$").match(id).hasMatch()) return;
    launchUrl(QUrl(id.isEmpty() ? "https://chatgpt.com/" : "https://chatgpt.com/c/" + id));
}
void LocalFiles::openLink(const QUrl &url) {
    const auto encoded = url.toString(QUrl::FullyEncoded);
    if (!url.isValid() || url.scheme() != "https" || url.host().isEmpty() ||
        !url.userName().isEmpty() || !url.password().isEmpty() || encoded.size() > 4096) return;
    launchUrl(url);
}
void LocalFiles::copyText(const QString &text) {
    const auto bytes = text.toUtf8();
    if (bytes.size() > 256 * 1024 || m_clipboard) return;
#ifndef Q_OS_WIN
    if (pcIntegration()) {
        const auto executable = QStandardPaths::findExecutable("powershell.exe");
        if (executable.isEmpty()) { m_message = "Le presse-papiers PC est indisponible."; emit changed(); return; }
        auto *process = new QProcess(this); m_clipboard = process;
        auto environment = QProcessEnvironment::systemEnvironment();
        for (const auto &key : environment.keys())
            if (key == "LD_PRELOAD" || key.startsWith("QTFB")) environment.remove(key);
        process->setProcessEnvironment(environment);
        process->setStandardOutputFile(QProcess::nullDevice()); process->setStandardErrorFile(QProcess::nullDevice());
        connect(process, &QProcess::started, this, [process, bytes] { process->write(bytes); process->closeWriteChannel(); });
        connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
            m_message = "Le texte n’a pas pu être copié sur le PC."; emit changed();
            if (error == QProcess::FailedToStart) { if (m_clipboard == process) m_clipboard = nullptr; process->deleteLater(); }
        });
        connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this, process](int code, QProcess::ExitStatus status) {
            m_message = code == 0 && status == QProcess::NormalExit ? "Texte copié sur le PC." : "Le texte n’a pas pu être copié sur le PC.";
            if (m_clipboard == process) m_clipboard = nullptr;
            process->deleteLater(); emit changed();
        });
        // The command is constant. User text travels only through the child's stdin.
        const QString script = QStringLiteral("$ErrorActionPreference='Stop'; [Console]::InputEncoding=New-Object System.Text.UTF8Encoding($false); $v=[Console]::In.ReadToEnd(); if($v.Length -gt 262144){throw 'Text too large'}; Set-Clipboard -Value $v");
        process->start(executable, {"-NoLogo", "-NoProfile", "-NonInteractive", "-WindowStyle", "Hidden", "-Command", script});
        QTimer::singleShot(5000, process, [process] { if (process->state() != QProcess::NotRunning) process->kill(); });
        return;
    }
#endif
    if (auto *clipboard = QGuiApplication::clipboard()) { clipboard->setText(text); m_message = "Texte copié."; emit changed(); }
}
void LocalFiles::launchUrl(const QUrl &url) {
    if (m_browser) return;
    if (!pcIntegration()) {
        if (!QDesktopServices::openUrl(url)) { m_message = "Ouvrez chatgpt.com sur votre PC."; emit changed(); }
        return;
    }
    auto *process = new QProcess(this);
    m_browser = process;
    auto environment = QProcessEnvironment::systemEnvironment();
    for (const auto &key : environment.keys())
        if (key == "LD_PRELOAD" || key.startsWith("QTFB")) environment.remove(key);
    process->setProcessEnvironment(environment);
    process->setStandardOutputFile(QProcess::nullDevice());
    process->setStandardErrorFile(QProcess::nullDevice());
    connect(process, &QProcess::started, this, [process, url] {
        process->write(QJsonDocument(QJsonObject{{"url", url.toString()}}).toJson(QJsonDocument::Compact));
        process->closeWriteChannel();
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        m_message = "Le navigateur PC n’a pas pu être ouvert."; emit changed();
        if (error == QProcess::FailedToStart) {
            if (m_browser == process) m_browser = nullptr;
            process->deleteLater();
        }
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, process](int code, QProcess::ExitStatus status) {
        if (code || status != QProcess::NormalExit) m_message = "Le navigateur PC n’a pas pu être ouvert.";
        else m_message = "Page ouverte dans le navigateur PC.";
        if (m_browser == process) m_browser = nullptr;
        process->deleteLater(); emit changed();
    });
    process->start("python3", {qEnvironmentVariable("REPAPER_PC_HANDOFF_HELPER"), "open"});
    QTimer::singleShot(20000, process, [process] { if (process->state() != QProcess::NotRunning) process->kill(); });
}
}
