#include "CoreProcess.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcessEnvironment>
#include <QThread>

namespace rmchat {
namespace { constexpr qsizetype frameLimit = 1024 * 1024; }
CoreProcess::~CoreProcess() { stop(); }
bool CoreProcess::fail(const QString &message) {
    m_error = message;
    stop();
    return false;
}
bool CoreProcess::start(const QString &program, const QStringList &uploadRoots) {
    stop();
    m_error.clear();
    m_nextId = 0;
    if (m_cancelled && m_cancelled())
        return fail(QStringLiteral("Opération interrompue."));
    if (!QFileInfo(program).isAbsolute() || !QFileInfo(program).isExecutable())
        return fail(QStringLiteral("Chemin absolu d’un core exécutable requis."));
    m_directory = std::make_unique<QTemporaryDir>(QDir::tempPath() + "/rmchat-XXXXXX");
    if (!m_directory->isValid() ||
        !QFile::setPermissions(m_directory->path(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner))
        return fail(QStringLiteral("Impossible de créer le répertoire IPC privé."));
    const auto socketPath = m_directory->filePath("core.sock");
    m_socket.setReadBufferSize(frameLimit + 1);
    QStringList arguments{"--socket", socketPath};
    for (const auto &root : uploadRoots) {
        const auto canonical = QFileInfo(root).canonicalFilePath();
        if (canonical.isEmpty() || !QFileInfo(canonical).isDir())
            return fail(QStringLiteral("Répertoire de fichiers invalide."));
        arguments << "--upload-root" << canonical;
    }
    auto environment = QProcessEnvironment::systemEnvironment();
    for (const auto &key : environment.keys()) {
        if (key == QStringLiteral("LD_PRELOAD") || key.startsWith(QStringLiteral("QTFB")))
            environment.remove(key);
    }
    m_process.setProcessEnvironment(environment);
    m_process.setStandardInputFile(QProcess::nullDevice());
    m_process.setStandardOutputFile(QProcess::nullDevice());
    m_process.setStandardErrorFile(QProcess::nullDevice());
    m_process.start(program, arguments);
    if (!m_process.waitForStarted(5000))
        return fail(QStringLiteral("Impossible de lancer rmchat-core."));
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 10000) {
        if (m_cancelled && m_cancelled())
            return fail(QStringLiteral("Opération interrompue."));
        m_socket.abort();
        m_socket.connectToServer(socketPath);
        if (m_socket.waitForConnected(100))
            return true;
        m_process.waitForFinished(10);
        if (m_process.state() == QProcess::NotRunning)
            return fail(QStringLiteral("rmchat-core s’est arrêté avant la connexion."));
        QThread::msleep(20);
    }
    return fail(QStringLiteral("Délai de connexion au core dépassé."));
}
QJsonObject CoreProcess::call(const QString &method, const QJsonObject &params,
                             int timeoutMs, Progress progress) {
    m_error.clear();
    if (m_cancelled && m_cancelled()) {
        fail(QStringLiteral("Opération interrompue ; vérifier la conversation avant de renvoyer."));
        return {};
    }
    if (m_socket.state() != QLocalSocket::ConnectedState) {
        fail(QStringLiteral("Le core n’est pas connecté."));
        return {};
    }
    const auto id = QStringLiteral("ui:%1").arg(++m_nextId);
    const auto frame = QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", id},
                                                 {"method", method}, {"params", params}})
                           .toJson(QJsonDocument::Compact) + '\n';
    if (frame.size() > frameLimit || m_socket.write(frame) != frame.size()) {
        fail(QStringLiteral("Requête IPC refusée."));
        return {};
    }
    m_socket.flush();
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (m_cancelled && m_cancelled()) {
            fail(QStringLiteral("Opération interrompue ; vérifier la conversation avant de renvoyer."));
            return {};
        }
        // Never accumulate more than one bounded frame even if a peer floods data.
        m_pending += m_socket.read(qMax<qsizetype>(0, frameLimit + 1 - m_pending.size()));
        for (;;) {
            const auto newline = m_pending.indexOf('\n');
            if (newline < 0)
                break;
            if (newline > frameLimit) {
                fail(QStringLiteral("Réponse IPC trop volumineuse."));
                return {};
            }
            QJsonParseError parse;
            const auto document = QJsonDocument::fromJson(m_pending.left(newline), &parse);
            m_pending.remove(0, newline + 1);
            if (parse.error != QJsonParseError::NoError || !document.isObject()) {
                fail(QStringLiteral("Réponse IPC invalide."));
                return {};
            }
            const auto object = document.object();
            if (object.value("jsonrpc").toString() != QStringLiteral("2.0")) {
                fail(QStringLiteral("Version IPC invalide."));
                return {};
            }
            if (!object.contains("id") && object.value("method").toString() == QStringLiteral("chat.progress")) {
                const auto event = object.value("params").toObject();
                if (event.value("requestId").toString() == id && progress)
                    progress(event);
                continue;
            }
            if (object.value("id").toString() != id ||
                object.contains("result") == object.contains("error")) {
                fail(QStringLiteral("Identifiant ou résultat IPC invalide."));
                return {};
            }
            return object;
        }
        if (m_pending.size() > frameLimit) {
            fail(QStringLiteral("Réponse IPC trop volumineuse."));
            return {};
        }
        if (m_socket.bytesAvailable() > 0)
            continue;
        if (m_socket.state() != QLocalSocket::ConnectedState) {
            fail(QStringLiteral("Connexion au core interrompue ; résultat distant à vérifier."));
            return {};
        }
        m_socket.waitForReadyRead(qMin(250, qMax(1, timeoutMs - int(timer.elapsed()))));
    }
    fail(QStringLiteral("Délai dépassé ; vérifier l’historique avant de renouveler un envoi."));
    return {};
}
void CoreProcess::stop() {
    m_socket.abort();
    m_pending.fill('\0');
    m_pending.clear();
    if (m_process.state() != QProcess::NotRunning) {
        if (!m_process.waitForFinished(300)) {
            m_process.terminate();
            if (!m_process.waitForFinished(1000)) {
                m_process.kill();
                m_process.waitForFinished(1000);
            }
        }
    }
    m_directory.reset();
}
}
