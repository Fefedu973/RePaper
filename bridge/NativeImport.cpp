#include "NativeImport.h"
#include "Bundle.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>
#include <QLockFile>
#include <QPainter>
#include <QPdfWriter>
#include <QRegularExpression>
#include <QTemporaryFile>
#include <pdfio.h>
#include <stdexcept>

namespace paper {
namespace {
constexpr qint64 MaxBytes = 64 * 1024 * 1024;
void require(bool condition, const char *code) {
    if (!condition) throw std::runtime_error(code);
}
QByteArray hash(const QByteArray &bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex(); }
QByteArray json(const QJsonObject &value) { return QJsonDocument(value).toJson(QJsonDocument::Compact); }
void privateDirectory(const QString &path) {
    const QFileInfo info(path);
    require(info.isAbsolute() && !info.isSymLink() && (!info.exists() || info.isDir()), "NATIVE_STAGING_INVALID");
    require(QDir().mkpath(path) && QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner),
            "NATIVE_STAGING_UNAVAILABLE");
}
void privateWrite(const QString &path, const QByteArray &bytes) {
    require(!QFileInfo(path).isSymLink(), "NATIVE_STAGING_INVALID");
    writeDurable(path, bytes);
    require(QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner), "NATIVE_STAGING_UNAVAILABLE");
}
QByteArray readBounded(const QString &path, qint64 limit = MaxBytes) {
    const QFileInfo info(path);
    require(!info.isSymLink() && info.isFile(), "NATIVE_STAGING_INVALID");
    QFile file(path);
    require(file.open(QIODevice::ReadOnly) && file.size() > 0 && file.size() <= limit, "NATIVE_STAGING_INVALID");
    const auto bytes = file.read(limit + 1);
    require(bytes.size() <= limit && file.atEnd(), "NATIVE_STAGING_INVALID");
    return bytes;
}
int validatePdf(const QByteArray &bytes, const QString &directory) {
    QTemporaryFile temporary(directory + "/validate-XXXXXX.pdf");
    require(temporary.open() && temporary.write(bytes) == bytes.size() && temporary.flush(), "NATIVE_STAGING_UNAVAILABLE");
    auto *pdf = pdfioFileOpen(QFile::encodeName(temporary.fileName()).constData(), nullptr, nullptr,
        [](pdfio_file_t *, const char *, void *) { return false; }, nullptr);
    require(pdf != nullptr, "INVALID_OR_ENCRYPTED_PDF");
    const auto count = pdfioFileGetNumPages(pdf);
    pdfioFileClose(pdf);
    require(count >= 1 && count <= 10000, "PDF_PAGE_COUNT_INVALID");
    return int(count);
}
QByteArray imagePdf(QByteArray bytes) {
    QBuffer input(&bytes);
    require(input.open(QIODevice::ReadOnly), "IMAGE_DECODE_FAILED");
    QImageReader::setAllocationLimit(96);
    QImageReader reader(&input);
    const auto dimensions = reader.size();
    require(dimensions.isValid() && dimensions.width() <= 12000 && dimensions.height() <= 12000 &&
            qint64(dimensions.width()) * dimensions.height() <= 16000000, "UNSUPPORTED_CONTENT");
    const auto image = reader.read();
    require(!image.isNull(), "IMAGE_DECODE_FAILED");
    QByteArray output;
    QBuffer buffer(&output);
    require(buffer.open(QIODevice::WriteOnly), "NATIVE_STAGING_UNAVAILABLE");
    {
        QPdfWriter writer(&buffer);
        writer.setPageSize(QPageSize(QSizeF(157, 209), QPageSize::Millimeter));
        writer.setResolution(226);
        QPainter painter;
        require(painter.begin(&writer), "IMAGE_PDF_FAILED");
        const auto fit = image.size().scaled(writer.width(), writer.height(), Qt::KeepAspectRatio);
        painter.drawImage(QRect(QPoint((writer.width() - fit.width()) / 2, (writer.height() - fit.height()) / 2), fit), image);
        require(painter.end(), "IMAGE_PDF_FAILED");
    }
    require(!output.isEmpty() && output.size() <= MaxBytes, "INPUT_SIZE_INVALID");
    return output;
}
}

QJsonObject prepareNativeImport(const QJsonObject &request, const QString &allowedInputRoot,
                                const QString &stagingRoot) {
    const auto title = request["displayName"].toString().trimmed();
    const auto key = request["idempotencyKey"].toString();
    const auto expected = request["sha256"].toString();
    require(!title.isEmpty() && title.size() <= 250 && !key.isEmpty() && key.size() <= 512,
            "INVALID_IMPORT_REQUEST");
    static const QRegularExpression digestPattern("^[0-9a-f]{64}$");
    require(digestPattern.match(expected).hasMatch(), "INPUT_HASH_MISMATCH");
    const QFileInfo source(request["path"].toString());
    const auto canonical = source.canonicalFilePath();
    const auto allowed = QDir(allowedInputRoot).absolutePath() + '/';
    require(!canonical.isEmpty() && canonical.startsWith(allowed) && canonical == source.absoluteFilePath() &&
            !source.isSymLink() && source.isFile(), "INPUT_PATH_NOT_ALLOWED");
    QFile input(canonical);
    require(input.open(QIODevice::ReadOnly) && input.size() > 0 && input.size() <= MaxBytes, "INPUT_SIZE_INVALID");
    const auto bytes = input.read(MaxBytes + 1);
    require(bytes.size() <= MaxBytes && input.atEnd(), "INPUT_SIZE_INVALID");
    require(hash(bytes) == expected.toLatin1(), "INPUT_HASH_MISMATCH");

    privateDirectory(stagingRoot);
    const auto directory = stagingRoot + '/' + QString::fromLatin1(hash(key.toUtf8()));
    privateDirectory(directory);
    QLockFile lock(directory + "/prepare.lock");
    lock.setStaleLockTime(0);
    require(lock.tryLock(1000), "NATIVE_PREPARATION_BUSY");
    const auto signature = hash(json({{"sha256", expected}, {"displayName", title}}));
    const auto manifestPath = directory + "/prepared.json";
    const auto pdfPath = directory + "/prepared.pdf";
    QJsonObject manifest;
    if (QFileInfo::exists(manifestPath) || QFileInfo(manifestPath).isSymLink()) {
        manifest = QJsonDocument::fromJson(readBounded(manifestPath, 65536)).object();
        require(manifest["version"].toInt() == 1, "NATIVE_STAGING_INVALID");
        require(manifest["requestHash"].toString().toLatin1() == signature, "IDEMPOTENCY_CONFLICT");
        const auto existing = readBounded(pdfPath);
        require(hash(existing) == manifest["sha256"].toString().toLatin1() && existing.startsWith("%PDF-") &&
                manifest["pages"].toInt() >= 1 && manifest["pages"].toInt() <= 10000, "NATIVE_STAGING_INVALID");
    } else {
        const auto pdf = bytes.startsWith("%PDF-") ? bytes : imagePdf(bytes);
        const int pages = validatePdf(pdf, directory);
        // Commit the snapshot and then its receipt before contacting the native
        // host. Image conversion timestamps cannot change the bytes on retry.
        privateWrite(pdfPath, pdf);
        manifest = {{"version", 1}, {"requestHash", QString::fromLatin1(signature)},
            {"sourceSha256", expected}, {"sha256", QString::fromLatin1(hash(pdf))}, {"pages", pages}};
        privateWrite(manifestPath, json(manifest));
    }
    return {{"path", pdfPath}, {"sha256", manifest["sha256"]}, {"displayName", title}, {"idempotencyKey", key}};
}
}
