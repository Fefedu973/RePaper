#include "PowerPointConverter.h"
#include "PowerPointPackage.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QUrl>

namespace {
constexpr qint64 MaxBytes = 64 * 1024 * 1024;
}
PowerPointConverter::PowerPointConverter(QObject *parent, int timeoutMs) : QObject(parent) {
    m_timeout.setSingleShot(true);
    m_timeout.setInterval(qMax(1, timeoutMs));
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        m_timedOut = true;
        m_process.kill();
    });
    connect(&m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            &PowerPointConverter::finish);
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && m_job && !m_cancelled)
            reject("Le moteur de conversion PowerPoint ne peut pas démarrer.");
    });
    // Documents and diagnostics can contain private course content. Neither is
    // sent to a shell, server, log, or the application's output stream.
    m_process.setStandardInputFile(QProcess::nullDevice());
    m_process.setStandardOutputFile(QProcess::nullDevice());
    m_process.setStandardErrorFile(QProcess::nullDevice());
}
PowerPointConverter::~PowerPointConverter() { cancel(); }
QString PowerPointConverter::extension(const QString &filename, const QString &mime) {
    const auto suffix = QFileInfo(filename).suffix().toLower();
    if (suffix == "ppt" || suffix == "pptx")
        return suffix;
    if (mime == "application/vnd.ms-powerpoint")
        return "ppt";
    if (mime == "application/vnd.openxmlformats-officedocument.presentationml.presentation")
        return "pptx";
    return {};
}
bool PowerPointConverter::matchesContent(const QByteArray &header, const QString &extension) {
    return (extension == "pptx" && header.startsWith("PK\003\004")) ||
           (extension == "ppt" && header.startsWith(QByteArray::fromHex("d0cf11e0a1b11ae1")));
}
QString PowerPointConverter::executable() {
    const auto override = qEnvironmentVariable("REPAPER_OFFICE_CONVERTER");
    const QStringList candidates = override.isEmpty()
        ? QStringList{QCoreApplication::applicationDirPath() + "/../lib/repaper-office/convert",
                      "/opt/repaper/lib/repaper-office/convert"}
        : QStringList{override};
    for (const auto &candidate : candidates) {
        QFileInfo file(candidate);
        if (file.isAbsolute() && file.isFile() && file.isExecutable())
            return file.canonicalFilePath();
    }
    return {};
}
bool PowerPointConverter::convert(const QString &source, const QString &suffix,
                                  const QString &destination) {
    if (busy())
        return false;
    QFile original(source);
    if (!original.open(QIODevice::ReadOnly) || original.size() > MaxBytes ||
        !matchesContent(original.peek(16), suffix)) {
        emit failed("Le fichier reçu n’est pas une présentation PowerPoint valide.");
        return false;
    }
    original.close();
    if (suffix == "pptx") {
        const auto problem = validatePowerPointPackage(source);
        if (!problem.isEmpty()) {
            emit failed(problem);
            return false;
        }
    }
    const auto program = executable();
    if (program.isEmpty()) {
        emit failed("Le module de conversion PowerPoint n’est pas installé.");
        return false;
    }
    m_job = std::make_unique<QTemporaryDir>(QDir::tempPath() + "/repaper-ppt-XXXXXX");
    m_cancelled = m_timedOut = m_restarted = false;
    m_destination = destination;
    if (!m_job->isValid() || !QFile::copy(source, m_job->filePath("input." + suffix)) ||
        !QDir().mkpath(m_job->filePath("profile/user")) ||
        !QDir().mkpath(m_job->filePath("output"))) {
        reject("L’espace temporaire est insuffisant pour convertir cette présentation.");
        return false;
    }
    QFile policy(m_job->filePath("profile/user/registrymodifications.xcu"));
    const QByteArray settings =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<oor:items xmlns:oor=\"http://openoffice.org/2001/registry\">"
        "<item oor:path=\"/org.openoffice.Office.Common/Security/Scripting\">"
        "<prop oor:name=\"MacroSecurityLevel\" oor:op=\"fuse\"><value>3</value></prop>"
        "</item></oor:items>";
    if (!policy.open(QIODevice::WriteOnly) || policy.write(settings) != settings.size()) {
        policy.close();
        reject("Impossible de préparer la conversion PowerPoint.");
        return false;
    }
    policy.close();
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.remove("LD_PRELOAD");
    environment.remove("LD_LIBRARY_PATH");
    environment.remove("QTFB_KEY");
    environment.insert("SAL_USE_VCLPLUGIN", "svp");
    environment.insert("SAL_DISABLE_OPENCL", "1");
    environment.insert("SAL_DISABLE_JAVA", "1");
    m_process.setProcessEnvironment(environment);
    m_process.setWorkingDirectory(m_job->path());
    m_process.setProgram(program);
    m_process.setArguments({"-env:UserInstallation=" + QUrl::fromLocalFile(m_job->filePath("profile")).toString(),
        "--headless", "--nologo", "--nodefault", "--nofirststartwizard", "--norestore", "--nolockcheck",
        suffix == "pptx" ? "--infilter=Impress MS PowerPoint 2007 XML" : "--infilter=MS PowerPoint 97",
        "--convert-to", "pdf:impress_pdf_Export", "--outdir", m_job->filePath("output"),
        m_job->filePath("input." + suffix)});
    m_timeout.start();
    m_process.start();
    return true;
}
void PowerPointConverter::cancel() {
    m_cancelled = true;
    m_timeout.stop();
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(1000);
    }
    m_job.reset();
    m_destination.clear();
}
void PowerPointConverter::reject(const QString &message) {
    m_timeout.stop();
    m_job.reset();
    m_destination.clear();
    emit failed(message);
}
void PowerPointConverter::finish(int exitCode, QProcess::ExitStatus status) {
    if (!m_job || m_cancelled)
        return;
    if (!m_timedOut && status == QProcess::NormalExit && exitCode == 81 && !m_restarted) {
        // LibreOffice asks for one restart after initializing a fresh profile.
        m_restarted = true;
        m_process.start();
        return;
    }
    if (m_timedOut) {
        reject("Cette présentation prend trop de temps à convertir. Essayez une version PDF.");
        return;
    }
    QFile pdf(m_job->filePath("output/input.pdf"));
    if (status != QProcess::NormalExit || exitCode != 0 || !pdf.open(QIODevice::ReadOnly) ||
        pdf.size() < 8 || pdf.size() > MaxBytes || !pdf.peek(8).startsWith("%PDF-")) {
        pdf.close();
        reject("Conversion impossible : présentation protégée, endommagée ou trop volumineuse.");
        return;
    }
    QSaveFile destination(m_destination);
    if (!destination.open(QIODevice::WriteOnly)) {
        pdf.close();
        reject("Impossible d’enregistrer le PDF converti. Vérifiez le stockage local.");
        return;
    }
    while (!pdf.atEnd()) {
        const auto bytes = pdf.read(1024 * 1024);
        if (bytes.isEmpty() || destination.write(bytes) != bytes.size()) {
            pdf.close();
            reject("L’enregistrement du PDF converti a été interrompu.");
            return;
        }
    }
    if (!destination.commit()) {
        pdf.close();
        reject("Impossible de finaliser le PDF converti.");
        return;
    }
    pdf.close();
    const auto path = m_destination;
    m_timeout.stop();
    m_job.reset();
    m_destination.clear();
    emit converted(path);
}
