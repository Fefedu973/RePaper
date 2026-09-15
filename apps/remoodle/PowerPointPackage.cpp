#include "PowerPointPackage.h"
#include <QFile>
#include <QHash>
#include <QSet>
#include <QVector>
#include <QXmlStreamReader>
#include <QtEndian>
#include <zlib.h>

namespace {
constexpr qint64 MaxXmlBytes = 8 * 1024 * 1024;
constexpr qint64 MaxExpandedBytes = 256 * 1024 * 1024;
const QString Invalid = "La présentation PowerPoint est endommagée, protégée ou trop volumineuse.";
quint16 u16(const QByteArray &data, int offset) {
    return qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(data.constData() + offset));
}
quint32 u32(const QByteArray &data, int offset) {
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(data.constData() + offset));
}
struct Entry {
    quint16 flags, method;
    quint32 crc, packed, unpacked, offset;
};
QByteArray xmlPart(QFile &file, const Entry &entry) {
    if ((entry.flags & 1) || (entry.method != 0 && entry.method != 8) ||
        entry.packed > MaxXmlBytes || entry.unpacked > MaxXmlBytes || !file.seek(entry.offset))
        return {};
    const auto header = file.read(30);
    if (header.size() != 30 || !header.startsWith("PK\003\004"))
        return {};
    const qint64 offset = qint64(entry.offset) + 30 + u16(header, 26) + u16(header, 28);
    if (offset + entry.packed > file.size() || !file.seek(offset))
        return {};
    auto packed = file.read(entry.packed);
    if (packed.size() != entry.packed)
        return {};
    QByteArray xml;
    if (entry.method == 0) {
        if (entry.packed != entry.unpacked) return {};
        xml = packed;
    } else {
        xml.resize(int(entry.unpacked));
        z_stream stream{};
        stream.next_in = reinterpret_cast<Bytef *>(packed.data());
        stream.avail_in = packed.size();
        stream.next_out = reinterpret_cast<Bytef *>(xml.data());
        stream.avail_out = xml.size();
        if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) return {};
        const int status = inflate(&stream, Z_FINISH);
        const bool valid = status == Z_STREAM_END && stream.total_out == entry.unpacked && stream.total_in == entry.packed;
        inflateEnd(&stream);
        if (!valid) return {};
    }
    if (quint32(crc32(0, reinterpret_cast<const Bytef *>(xml.constData()), xml.size())) != entry.crc)
        return {};
    return xml;
}
bool incompatibleMath(const QByteArray &xml, bool &wellFormed) {
    struct Alternative { int depth; bool office2010Choice = false, math = false, preview = false; int fallbackDepth = -1; };
    QVector<Alternative> alternatives;
    QVector<QHash<QString, QString>> namespaces;
    QXmlStreamReader reader(xml);
    int depth = 0;
    bool unsupported = false;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) {
            ++depth;
            namespaces.append(namespaces.isEmpty() ? QHash<QString, QString>() : namespaces.last());
            for (const auto &declaration : reader.namespaceDeclarations())
                namespaces.last().insert(declaration.prefix().toString(), declaration.namespaceUri().toString());
            const auto ns = reader.namespaceUri().toString();
            const auto name = reader.name().toString();
            if (ns == "http://schemas.openxmlformats.org/markup-compatibility/2006" && name == "AlternateContent")
                alternatives.append(Alternative{depth});
            if (alternatives.isEmpty()) continue;
            auto &alternative = alternatives.last();
            if (ns == "http://schemas.openxmlformats.org/markup-compatibility/2006" && name == "Choice") {
                const auto required = reader.attributes().value("Requires").toString().split(' ');
                for (const auto &prefix : required)
                    if (namespaces.last().value(prefix) == "http://schemas.microsoft.com/office/drawing/2010/main")
                        alternative.office2010Choice = true;
            }
            if (ns == "http://schemas.openxmlformats.org/markup-compatibility/2006" && name == "Fallback")
                alternative.fallbackDepth = depth;
            if (ns == "http://schemas.openxmlformats.org/officeDocument/2006/math" &&
                (name == "oMath" || name == "oMathPara") && alternative.fallbackDepth < 0)
                alternative.math = true;
            if (ns == "http://schemas.openxmlformats.org/drawingml/2006/main" && name == "blip" &&
                alternative.fallbackDepth >= 0 &&
                !reader.attributes().value("http://schemas.openxmlformats.org/officeDocument/2006/relationships", "embed").isEmpty())
                alternative.preview = true;
        } else if (reader.isEndElement()) {
            if (!alternatives.isEmpty()) {
                auto &alternative = alternatives.last();
                if (alternative.fallbackDepth == depth) alternative.fallbackDepth = -1;
                if (alternative.depth == depth) {
                    unsupported |= alternative.office2010Choice && alternative.math && !alternative.preview;
                    alternatives.removeLast();
                }
            }
            namespaces.removeLast();
            --depth;
        }
    }
    wellFormed = !reader.hasError();
    return unsupported;
}
} // namespace
QString validatePowerPointPackage(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() < 22 || file.size() > 64 * 1024 * 1024)
        return Invalid;
    file.seek(qMax(qint64(0), file.size() - 65557));
    const auto tail = file.readAll();
    const int end = tail.lastIndexOf("PK\005\006");
    if (end < 0 || end + 22 > tail.size() || end + 22 + u16(tail, end + 20) != tail.size() ||
        u16(tail, end + 4) || u16(tail, end + 6) || u16(tail, end + 8) != u16(tail, end + 10))
        return Invalid;
    const int count = u16(tail, end + 10);
    const qint64 bytes = u32(tail, end + 12), offset = u32(tail, end + 16);
    if (!count || count > 20000 || bytes > MaxXmlBytes || offset + bytes > file.size() || !file.seek(offset))
        return Invalid;
    const auto directory = file.read(bytes);
    if (directory.size() != bytes) return Invalid;
    QSet<QString> names;
    qint64 totalExpanded = 0;
    int cursor = 0;
    bool presentation = false, types = false;
    for (int index = 0; index < count; ++index) {
        if (cursor + 46 > directory.size() || directory.mid(cursor, 4) != "PK\001\002") return Invalid;
        const int nameLength = u16(directory, cursor + 28);
        const int length = 46 + nameLength + u16(directory, cursor + 30) + u16(directory, cursor + 32);
        if (cursor + length > directory.size() || u16(directory, cursor + 34)) return Invalid;
        const auto name = QString::fromUtf8(directory.mid(cursor + 46, nameLength));
        Entry entry{u16(directory, cursor + 8), u16(directory, cursor + 10), u32(directory, cursor + 16),
                    u32(directory, cursor + 20), u32(directory, cursor + 24), u32(directory, cursor + 42)};
        cursor += length;
        if (names.contains(name) || (entry.flags & 1)) return Invalid;
        names.insert(name);
        totalExpanded += entry.unpacked;
        if (totalExpanded > MaxExpandedBytes) return Invalid;
        if (name == "ppt/presentation.xml") presentation = true;
        const bool slide = name.startsWith("ppt/slides/slide") && name.endsWith(".xml") && name.count('/') == 2;
        if (name != "[Content_Types].xml" && !slide) continue;
        const auto xml = xmlPart(file, entry);
        if (xml.isEmpty()) return Invalid;
        if (name == "[Content_Types].xml") {
            types = xml.contains("application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml");
            continue;
        }
        bool wellFormed = false;
        const bool unsupported = incompatibleMath(xml, wellFormed);
        if (!wellFormed) return Invalid;
        if (unsupported)
            return "Cette présentation contient une équation Office sans aperçu compatible. Exportez-la en PDF depuis PowerPoint pour conserver la formule.";
    }
    return presentation && types && cursor == directory.size() ? QString() : Invalid;
}
