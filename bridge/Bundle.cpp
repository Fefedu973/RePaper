#include "Bundle.h"
#include "SceneWriter.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QPdfWriter>
#include <QSaveFile>
#include <QUuid>
#include <pdfio.h>
#include <stdexcept>
#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <unistd.h>
#endif
namespace paper {
static void fail(const char *text) {
    throw std::runtime_error(text);
}
void syncDirectory(const QString &path) {
#ifdef Q_OS_UNIX
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_DIRECTORY);
    if (fd < 0)
        fail("DIRECTORY_OPEN_FAILED");
    const int status = ::fsync(fd);
    ::close(fd);
    if (status < 0)
        fail("DIRECTORY_FSYNC_FAILED");
#else
    Q_UNUSED(path);
#endif
}
void writeDurable(const QString &path, const QByteArray &data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.flush())
        fail("STAGING_WRITE_FAILED");
#ifdef Q_OS_UNIX
    if (::fsync(file.handle()) != 0)
        fail("STAGING_FSYNC_FAILED");
#endif
    if (!file.commit())
        fail("STAGING_COMMIT_FAILED");
    syncDirectory(QFileInfo(path).absolutePath());
}
QJsonObject blankScene() {
    return {{"schemaVersion", 1},
            {"page", QJsonObject{{"width", 1404}, {"height", 1872}}},
            {"strokes", QJsonArray{}}};
}
Bundle prepareBundle(const QString &root, const QString &id, const QString &title, const QString &source) {
    if (QUuid(id).isNull() || title.trimmed().isEmpty() || title.size() > 250)
        fail("INVALID_DOCUMENT");
    Bundle bundle{id, root + "/" + id, {}, "notebook", 1};
    if (QFileInfo::exists(bundle.staging))
        fail("STAGING_ALREADY_EXISTS");
    if (!QDir().mkpath(bundle.staging))
        fail("STAGING_CREATE_FAILED");
    QByteArray native;
    QString input = source;
    if (source.isEmpty())
        native = writeScene(blankScene());
    else {
        QFile file(source);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 64 * 1024 * 1024)
            fail("INPUT_UNREADABLE_OR_TOO_LARGE");
        auto head = file.peek(32);
        if (head.startsWith("%PDF-")) {
            auto pdf = pdfioFileOpen(
                QFile::encodeName(source).constData(), nullptr, nullptr,
                [](pdfio_file_t *, const char *, void *) { return false; }, nullptr);
            if (!pdf)
                fail("INVALID_OR_ENCRYPTED_PDF");
            bundle.pages = int(pdfioFileGetNumPages(pdf));
            pdfioFileClose(pdf);
            if (bundle.pages < 1 || bundle.pages > 10000)
                fail("PDF_PAGE_COUNT_INVALID");
            bundle.type = "pdf";
        } else if (source.endsWith(".paper-scene.json")) {
            auto doc = QJsonDocument::fromJson(file.readAll());
            if (!doc.isObject() || !validateScene(doc.object()))
                fail("SCENE_INVALID");
            native = writeScene(doc.object());
        } else {
            QImageReader::setAllocationLimit(96);
            QImageReader reader(source);
            const auto dimensions = reader.size();
            if (!dimensions.isValid() || dimensions.width() > 12000 || dimensions.height() > 12000 ||
                qint64(dimensions.width()) * dimensions.height() > 16000000)
                fail("UNSUPPORTED_CONTENT");
            QImage image = reader.read();
            if (image.isNull())
                fail("IMAGE_DECODE_FAILED");
            // Use decoded image dimensions/pixels, never trust filename MIME.
            input = bundle.staging + "/converted.pdf";
            QPdfWriter writer(input);
            writer.setPageSize(QPageSize(QSizeF(157, 209), QPageSize::Millimeter));
            writer.setResolution(226);
            {
                QPainter painter(&writer);
                const auto fit = image.size().scaled(writer.width(), writer.height(), Qt::KeepAspectRatio);
                painter.drawImage(
                    QRect(QPoint((writer.width() - fit.width()) / 2, (writer.height() - fit.height()) / 2),
                          fit),
                    image);
            }
            bundle.type = "pdf";
        }
    }
    QJsonArray pages, redirection;
    for (int i = 0; i < bundle.pages; ++i) {
        pages.append(QUuid::createUuid().toString(QUuid::WithoutBraces));
        redirection.append(i);
    }
    QJsonObject content{{"fileType", bundle.type},
                        {"formatVersion", 1},
                        {"pageCount", bundle.pages},
                        {"pages", pages},
                        {"originalPageCount", bundle.type == "pdf" ? bundle.pages : 0},
                        {"redirectionPageMap", redirection},
                        {"orientation", "portrait"},
                        {"textScale", 1},
                        {"lineHeight", -1},
                        {"margins", 125},
                        {"textAlignment", "justify"},
                        {"fontName", ""},
                        {"zoomMode", "bestFit"},
                        {"customZoomCenterX", 0},
                        {"customZoomCenterY", 936},
                        {"customZoomOrientation", "portrait"},
                        {"customZoomPageWidth", 1404},
                        {"customZoomPageHeight", 1872},
                        {"customZoomScale", 1},
                        {"documentMetadata", QJsonObject{}},
                        {"extraMetadata", QJsonObject{}},
                        {"pageTags", QJsonArray{}},
                        {"transform", QJsonObject{}}};
    auto now = QString::number(QDateTime::currentMSecsSinceEpoch());
    QJsonObject metadata{{"visibleName", title.trimmed()},
                         {"parent", ""},
                         {"type", "DocumentType"},
                         {"version", 1},
                         {"deleted", false},
                         {"synced", false},
                         {"modified", true},
                         {"createdTime", now},
                         {"lastModified", now},
                         {"lastOpened", "0"},
                         {"lastOpenedPage", 0},
                         {"pinned", false}};
    auto write = [&](const QString &ext, const QByteArray &bytes) {
        writeDurable(bundle.staging + "/" + id + ext, bytes);
        bundle.entries << id + ext;
    };
    write(".content", QJsonDocument(content).toJson(QJsonDocument::Compact));
    write(".metadata", QJsonDocument(metadata).toJson(QJsonDocument::Compact));
    write(".pagedata", QByteArray(bundle.pages, '\n'));
    if (bundle.type == "pdf") {
        QFile file(input);
        if (!file.open(QIODevice::ReadOnly))
            fail("PDF_READ_FAILED");
        write(".pdf", file.readAll());
    } else {
        const auto dir = bundle.staging + "/" + id;
        QDir().mkpath(dir);
        writeDurable(dir + "/" + pages[0].toString() + ".rm", native);
        bundle.entries << id;
    }
    syncDirectory(bundle.staging);
    return bundle;
}
} // namespace paper
