#include "NativeAgendaLayout.h"
#include "NativeStrokeSampling.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>
#include <QPainterPath>
#include <QRawFont>
#include <QRegularExpression>
#include <QTextBoundaryFinder>
#include <algorithm>
#include <cmath>

namespace {
bool finite(qreal value) { return std::isfinite(value); }
bool rectangle(const QRectF &r) {
    return finite(r.x()) && finite(r.y()) && finite(r.width()) && finite(r.height())
        && r.width() >= 1800 && r.width() <= 5000 && r.height() >= 1000
        && r.height() < r.width() && qAbs(r.x()) < 10000 && qAbs(r.y()) < 10000;
}
struct TextInk {
    QVector<PaperDrawing::Stroke> strokes;
    QRectF bounds;
    bool valid = false;
};
TextInk lettering(const QByteArray &data, const QString &text, qreal size, QPointF baseline) {
    TextInk ink;
    QRawFont font(data, size, QFont::PreferNoHinting);
    if (!font.isValid()) return ink;
    const auto glyphs = font.glyphIndexesForString(text);
    const auto advances = font.advancesForGlyphIndexes(glyphs);
    if (glyphs.size() != advances.size()) return ink;
    QPainterPath path;
    QPointF position = baseline;
    for (qsizetype i = 0; i < glyphs.size(); ++i) {
        if (!glyphs[i]) return ink;
        const auto glyph = font.pathForGlyph(glyphs[i]);
        path.addPath(glyph.translated(position));
        position += advances[i];
    }
    const qreal width = qRound(qBound(qreal(.5), size / 24, qreal(2)) * 4) / 4.0;
    for (const auto &polygon : path.toSubpathPolygons()) {
        if (polygon.size() < 2) continue;
        const auto sampled = RePaperNative::sampleStrokeForNativeSelection(polygon);
        if (!sampled.valid()) return {};
        PaperDrawing::Stroke stroke;
        stroke.width = width; stroke.color = Qt::black;
        for (const auto &point : sampled.points)
            stroke.points.append(QPointF(float(point.x()), float(point.y())));
        ink.strokes.append(std::move(stroke));
    }
    ink.bounds = NativeAgendaLayout::bounds(ink.strokes);
    ink.valid = text.trimmed().isEmpty() || !ink.strokes.isEmpty();
    return ink;
}
QByteArray strokeBytes(const PaperDrawing::Stroke &stroke) {
    QByteArray bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_6_0);
    out << qint32(19) << quint32(stroke.color.rgba()) << qint32(qRound(stroke.width * 4))
        << qint32(stroke.points.size());
    // Serialize the exact float coordinates passed to the native Line writer.
    for (const auto &p : stroke.points) out << double(float(p.x())) << double(float(p.y()));
    return bytes;
}
QList<QByteArray> canonical(const QVector<PaperDrawing::Stroke> &strokes) {
    QList<QByteArray> result;
    for (const auto &stroke : strokes) result.append(strokeBytes(stroke));
    std::sort(result.begin(), result.end());
    return result;
}
}

QRectF NativeAgendaLayout::bounds(const QVector<PaperDrawing::Stroke> &strokes) {
    QRectF result;
    bool first = true;
    for (const auto &stroke : strokes) for (const auto &point : stroke.points) {
        const QRectF part(point - QPointF(stroke.width / 2, stroke.width / 2),
                          QSizeF(stroke.width, stroke.width));
        result = first ? part : result.united(part); first = false;
    }
    return result;
}

NativeAgendaLayout::Plan NativeAgendaLayout::create(
    const QVariantMap &fields, const QRectF &paperBounds, const QByteArray &fontData) {
    Plan plan;
    plan.paperBounds = paperBounds;
    const auto fail = [&](const char *reason) { plan.reason = QString::fromLatin1(reason); return plan; };
    if (!rectangle(paperBounds)) return fail("agenda-native-paper-bounds-unavailable");
    if (fontData.isEmpty() || fontData.size() > 8 * 1024 * 1024)
        return fail("agenda-native-font-unavailable");
    for (const auto &key : {"title", "day", "date", "time"}) {
        const auto value = fields.value(key);
        if (value.metaType().id() != QMetaType::QString || value.toString().size() > 512
            || value.toString().contains(QChar::Null)) return fail("agenda-invalid-fields");
    }
    const QString title = fields.value("title").toString().simplified().normalized(QString::NormalizationForm_C);
    const QString day = fields.value("day").toString().simplified().normalized(QString::NormalizationForm_C);
    const QString time = fields.value("time").toString().simplified().normalized(QString::NormalizationForm_C);
    const QRegularExpression datePattern(QStringLiteral("^([0-9]{2}) / ([0-9]{2}) / ([0-9]{4})$"));
    const auto date = datePattern.match(fields.value("date").toString());
    if (title.isEmpty() || day.isEmpty() || day.size() > 24 || time.size() > 40 || !date.hasMatch())
        return fail("agenda-invalid-fields");
    const qreal w = paperBounds.width();
    const QPointF origin = paperBounds.topLeft();
    const auto add = [&](const char *name, const QString &text, qreal size, QPointF baseline,
                         const QRectF &region, bool shrink) {
        TextInk ink;
        for (;;) {
            ink = lettering(fontData, text, size, baseline + origin);
            if (!ink.valid) return false;
            if (text.isEmpty() || region.translated(origin).contains(ink.bounds)) break;
            if (!shrink || size <= 10) return false;
            size -= 1;
        }
        plan.strokes += ink.strokes;
        plan.fieldBounds.insert(QString::fromLatin1(name), ink.bounds);
        return true;
    };
    // Positions follow the firmware's native LS Dayplanner constants. The
    // title belongs in the first notes row; dates fit around existing slashes.
    if (!add("day", day, 48, QPointF(w / 2 - 410, 95), QRectF(w / 2 - 420, 20, 820, 110), true)
        || !add("dateDay", date.captured(1), 20, QPointF(w / 2 + 784, 205), QRectF(w / 2 + 782, 170, 34, 60), true)
        || !add("dateMonth", date.captured(2), 20, QPointF(w / 2 + 830, 205), QRectF(w / 2 + 828, 170, 32, 60), true)
        || !add("dateYear", date.captured(3), 18, QPointF(w / 2 + 875, 205), QRectF(w / 2 + 873, 170, w / 2 - 879, 60), true)
        || !add("time", time, 20, QPointF(14, 330), QRectF(8, 265, 170, 100), true))
        return fail("agenda-field-does-not-fit");
    QString displayed = title;
    const QRectF titleRegion = QRectF(200, 270, w - 220, 90).translated(origin);
    const auto titleAt = [&](const QString &text) {
        return lettering(fontData, text, 38, origin + QPointF(206, 330));
    };
    const auto fits = [&](const TextInk &ink) {
        return ink.valid && titleRegion.contains(ink.bounds)
            && plan.strokes.size() + ink.strokes.size() <= MaximumStrokes;
    };
    TextInk titleInk = titleAt(displayed);
    if (!fits(titleInk)) {
        // Prefix width/contour count grow monotonically with the native font's
        // nonnegative advances. Binary search bounds work for 512-character
        // titles without hundreds of font/path reconstructions on the GUI.
        QVector<int> ends{0};
        QTextBoundaryFinder boundaries(QTextBoundaryFinder::Grapheme, title);
        for (int end = boundaries.toNextBoundary(); end >= 0; end = boundaries.toNextBoundary()) ends.append(end);
        displayed = QString(QChar(0x2026));
        titleInk = titleAt(displayed);
        if (!fits(titleInk)) return fail("agenda-stroke-budget");
        int low = 1, high = ends.size() - 1;
        while (low <= high) {
            const int middle = low + (high - low) / 2;
            const auto candidate = title.left(ends[middle]) + QChar(0x2026);
            const auto ink = titleAt(candidate);
            if (fits(ink)) { displayed = candidate; titleInk = ink; low = middle + 1; }
            else high = middle - 1;
        }
    }
    plan.strokes += titleInk.strokes;
    plan.fieldBounds.insert("title", titleInk.bounds);
    plan.displayedTitle = displayed;
    plan.inkBounds = NativeAgendaLayout::bounds(plan.strokes);
    qsizetype count = 0;
    for (const auto &stroke : plan.strokes) count += stroke.points.size();
    if (plan.strokes.isEmpty() || plan.strokes.size() > MaximumStrokes
        || count > RePaperNative::NativeStrokePointBudget || !paperBounds.contains(plan.inkBounds))
        return fail("agenda-stroke-budget");
    QByteArray header;
    QDataStream stream(&header, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << QStringLiteral("native-agenda-LS-Dayplanner-v1") << paperBounds
           << QCryptographicHash::hash(fontData, QCryptographicHash::Sha256)
           << title << day << fields.value("date").toString() << time << displayed;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(header);
    for (const auto &stroke : canonical(plan.strokes)) hash.addData(stroke);
    plan.hash = QString::fromLatin1(hash.result().toHex());
    return plan;
}

bool NativeAgendaLayout::matches(const Plan &plan, const RePaperNative::NativeObjectSnapshot &snapshot) {
    if (!plan.valid() || !snapshot.complete || snapshot.lines.size() != plan.strokes.size()) return false;
    QVector<PaperDrawing::Stroke> actual;
    for (const auto &line : snapshot.lines) {
        if (line.tool != 19 || !line.uniformPointWidth || line.maximumPointWidth != qRound(line.stroke.width * 4)
            || line.stroke.color != Qt::black) return false;
        actual.append(line.stroke);
    }
    return canonical(actual) == canonical(plan.strokes);
}
