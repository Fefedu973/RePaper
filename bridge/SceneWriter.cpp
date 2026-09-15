#include "SceneWriter.h"
#include <QDataStream>
#include <QIODevice>
#include <QJsonArray>
#include <QUuid>
#include <algorithm>
#include <cmath>

namespace paper {
namespace {
QByteArray integer(quint32 value) {
    QByteArray b;
    QDataStream s(&b, QIODevice::WriteOnly);
    s.setByteOrder(QDataStream::LittleEndian);
    s << value;
    return b;
}
QByteArray variable(quint64 value) {
    QByteArray b;
    do {
        quint8 byte = value & 127;
        value >>= 7;
        b.append(char(byte | (value ? 128 : 0)));
    } while (value);
    return b;
}
QByteArray id(int tag, int author, quint64 counter) {
    return variable((tag << 4) | 15) + QByteArray(1, char(author)) + variable(counter);
}
QByteArray sub(int tag, const QByteArray &b) {
    return variable((tag << 4) | 12) + integer(b.size()) + b;
}
QByteArray number(int tag, quint32 value) {
    return variable((tag << 4) | 4) + integer(value);
}
QByteArray boolean(int tag, bool v) {
    return variable((tag << 4) | 1) + QByteArray(1, char(v));
}
QByteArray real(int tag, double value, bool single = false) {
    QByteArray b;
    QDataStream s(&b, QIODevice::WriteOnly);
    s.setByteOrder(QDataStream::LittleEndian);
    s.setFloatingPointPrecision(single ? QDataStream::SinglePrecision : QDataStream::DoublePrecision);
    if (single)
        s << float(value);
    else
        s << value;
    return variable((tag << 4) | (single ? 4 : 8)) + b;
}
QByteArray block(int type, const QByteArray &b, int minimum = 1, int current = 1) {
    return integer(b.size()) + QByteArray(1, 0) + QByteArray(1, char(minimum)) +
           QByteArray(1, char(current)) + QByteArray(1, char(type)) + b;
}
QByteArray string(int tag, const QString &text) {
    const auto b = text.toUtf8();
    return sub(tag, variable(b.size()) + QByteArray(1, 1) + b);
}
QByteArray group(int node, const QString &name) {
    return block(
        2, id(1, 0, node) + sub(2, id(1, 0, 12) + string(2, name)) + sub(3, id(1, 0, 0) + boolean(2, true)),
        1, 2);
}
} // namespace
bool validateScene(const QJsonObject &scene, QString *error) {
    auto fail = [error](const QString &s) {
        if (error)
            *error = s;
        return false;
    };
    if (scene["schemaVersion"].toInt() != 1 || !scene["strokes"].isArray())
        return fail("Format de scène inconnu");
    auto page = scene["page"].toObject();
    const double w = page["width"].toDouble(), h = page["height"].toDouble();
    if (w != 1404 || h != 1872)
        return fail("Profil de page non pris en charge (1404 × 1872 requis)");
    auto strokes = scene["strokes"].toArray();
    if (strokes.size() > 10000)
        return fail("Trop de traits");
    int total = 0;
    for (auto item : strokes) {
        auto stroke = item.toObject();
        double width = stroke["width"].toDouble();
        if (!std::isfinite(width) || width <= 0 || width > 50)
            return fail("Épaisseur invalide");
        const auto color = stroke["color"].toString().toLower();
        if (color != "black" && color != "#000000" && color != "#000" && color != "#ff000000")
            return fail("Seuls les traits noirs sont pris en charge par cet adaptateur natif");
        auto points = stroke["points"].toArray();
        if (points.size() < 2 || points.size() > 100000)
            return fail("Points invalides");
        total += points.size();
        if (total > 250000)
            return fail("Scène trop volumineuse");
        for (auto point : points) {
            auto p = point.toArray();
            if (p.size() != 2 || !p[0].isDouble() || !p[1].isDouble())
                return fail("Point invalide");
            auto x = p[0].toDouble(), y = p[1].toDouble();
            if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || x > w || y < 0 || y > h)
                return fail("Point hors de la page");
        }
    }
    return true;
}
QByteArray writeScene(const QJsonObject &scene) {
    if (!validateScene(scene))
        return {};
    QByteArray result("reMarkable .lines file, version=6          ");
    // Author UUID uses the little-endian UUID wire representation.
    auto uuid = QUuid::createUuid().toRfc4122();
    std::reverse(uuid.begin(), uuid.begin() + 4);
    std::reverse(uuid.begin() + 4, uuid.begin() + 6);
    std::reverse(uuid.begin() + 6, uuid.begin() + 8);
    result += block(9, variable(1) + sub(0, variable(16) + uuid + QByteArray("\x01\x00", 2)));
    result += block(0, id(1, 1, 1) + boolean(2, true) + boolean(3, false));
    result += block(10, number(1, 1) + number(2, 0) + number(3, 0) + number(4, 0) + number(5, 0), 0, 1);
    result += block(1, id(1, 0, 11) + id(2, 0, 0) + boolean(3, true) + sub(4, id(1, 0, 1)));
    result += group(1, "") + group(11, "RePaper");
    result += block(4, id(1, 0, 1) + id(2, 0, 13) + id(3, 0, 0) + id(4, 0, 0) + number(5, 0) +
                           sub(6, QByteArray(1, 2) + id(2, 0, 11)));
    result += block(13,
                    sub(1, id(1, 1, 2) + id(2, 0, 11)) + sub(2, id(1, 1, 3) + boolean(2, true)) +
                        sub(3, id(1, 1, 4) + boolean(2, true)) + sub(5, integer(1404) + integer(1872)),
                    0, 1);
    quint64 counter = 20, previous = 0;
    for (auto item : scene["strokes"].toArray()) {
        auto stroke = item.toObject();
        QByteArray points;
        QDataStream data(&points, QIODevice::WriteOnly);
        data.setByteOrder(QDataStream::LittleEndian);
        data.setFloatingPointPrecision(QDataStream::SinglePrecision);
        for (auto value : stroke["points"].toArray()) {
            auto p = value.toArray();
            data << float(p[0].toDouble() - 702.0) << float(p[1].toDouble()) << quint16(0)
                 << quint16(qRound(stroke["width"].toDouble() * 4)) << quint8(0) << quint8(255);
        }
        const auto line =
            number(1, 17) + number(2, 0) + real(3, 1.0) + real(4, 0.0, true) + sub(5, points) + id(6, 0, 1);
        result += block(5,
                        id(1, 0, 11) + id(2, 1, counter) + id(3, previous ? 1 : 0, previous) + id(4, 0, 0) +
                            number(5, 0) + sub(6, QByteArray(1, 3) + line),
                        2, 2);
        previous = counter++;
    }
    return result;
}
} // namespace paper
