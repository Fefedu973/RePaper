#include "MathEngine.h"
#include "MathBudget.h"
#include <QCache>
#include <QCryptographicHash>
#include <QGuiApplication>
#include <QPainter>
#include <QSet>
#include <QThread>
#include <QtMath>
#include <cmath>
#include <memory>
#include "latex.h"
#include "core/formula.h"
#include "core/parser.h"
#include "fonts/font_info.h"
#include "platform/qt/graphic_qt.h"

static void initializeMathResources() { Q_INIT_RESOURCE(rmchat_math_fonts); }
namespace rmchat {
namespace {
bool initialized = false;
QCache<QString, MathImage> images(8 * 1024); // KiB, including the rasterized formulas only.
const QSet<QString> forbidden{
    "newcommand", "renewcommand", "providecommand", "def", "gdef", "edef", "xdef", "let", "futurelet", "csname", "endcsname",
    "newenvironment", "renewenvironment", "newcolumntype", "definecolor", "DeclareMathOperator", "makeatletter", "makeatother",
    "input", "include", "includegraphics", "write", "openout", "read", "usepackage", "documentclass", "xml", "externalFont",
    "dynamic", "GeoGebra", "fatalIfCmdConflict", "multicolumn", "multirow", "hdotsfor", "resizebox", "scalebox", "rotatebox",
    "rule", "hspace", "vspace", "kern", "mkern", "hskip", "vskip", "muskip", "newlength", "setlength", "addtolength",
    "DeclareMathSizes", "magnification", "breakEverywhere", "arrayrulecolor", "cornersize", "longdiv", "XML", "roman", "Roman"};
}
void MathEngine::initialize() {
    if (initialized || !qGuiApp || QThread::currentThread() != qGuiApp->thread()) return;
    initializeMathResources();
    tex::LaTeX::init(":/rmchat/math");
    // Qt loads font engines lazily during the first draw. Register every fixed,
    // embedded MicroTeX font before drawing any of them: adding another font
    // later invalidates Qt's font cache. This startup work contains no message
    // input and therefore precedes the untrusted expression's 75 ms budget.
    QVector<QFont> fonts;
    const auto &knownFonts = tex::FontInfo::__infos();
    const auto count = qMin<std::size_t>(knownFonts.size(), 64);
    for (std::size_t i = 0; i < count; ++i) {
        const auto *info = knownFonts.at(i);
        if (!info || info->getPath().rfind(":/rmchat/math/fonts/", 0) != 0) continue;
        const auto *font = static_cast<const tex::Font_qt *>(tex::FontInfo::getFont(int(i)));
        fonts.append(font->getQFont());
    }
    QImage scratch(768, 128, QImage::Format_ARGB32_Premultiplied); scratch.fill(Qt::transparent);
    QPainter painter(&scratch);
    painter.setRenderHint(QPainter::Antialiasing); painter.setRenderHint(QPainter::TextAntialiasing);
    const auto glyphs = QStringLiteral("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz+-=()[]");
    for (const auto &font : fonts) for (qreal scale : {20., 42., 48.}) {
        painter.save(); painter.scale(scale, scale); painter.setFont(font);
        painter.drawText(QPointF(0, 1.5), glyphs); painter.restore();
    }
    painter.end();
    initialized = true;
}
bool MathEngine::accepts(const QString &source) {
    if (source.isEmpty() || source.toUtf8().size() > 8192) return false;
    int depth = 0, commands = 0, cells = 0, lines = 0;
    for (qsizetype i = 0; i < source.size(); ++i) {
        const auto ch = source.at(i);
        if (ch == '\0' || (ch.unicode() < 0x20 && ch != '\n' && ch != '\t' && ch != '\r')) return false;
        if (ch == '%') { while (i + 1 < source.size() && source.at(i + 1) != '\n') ++i; continue; }
        if (ch == '\\') {
            if (++commands > 1024) return false;
            qsizetype end = i + 1;
            while (end < source.size() && (source.at(end).isLetter() || source.at(end) == '@')) ++end;
            if (end > i + 1) {
                const auto command = source.mid(i + 1, end - i - 1);
                if (forbidden.contains(command) || command.contains('@')) return false;
                i = end - 1;
            } else if (end < source.size()) {
                if (source.at(end) == '\\' && ++lines > 128) return false;
                i = end;
            }
        } else if (ch == '{') { if (++depth > 32) return false; }
        else if (ch == '}') { if (--depth < 0) return false; }
        else if (ch == '&' && ++cells > 128) return false;
    }
    return depth == 0;
}
MathImage MathEngine::render(const QString &source, bool display, qreal fontPixels,
                            qreal width, const QColor &color, int budgetMs) {
    if (!accepts(source) || budgetMs <= 0 || !qGuiApp || QThread::currentThread() != qGuiApp->thread()) return {};
    initialize();
    if (!initialized) return {};
    fontPixels = qBound<qreal>(10, fontPixels, 48);
    width = qBound<qreal>(40, width, 1600);
    const auto key = QString::fromLatin1(QCryptographicHash::hash(source.toUtf8(), QCryptographicHash::Sha256).toHex()) +
        QString(":%1:%2:%3:%4").arg(display).arg(fontPixels).arg(qRound(width)).arg(color.rgba());
    if (auto *cached = images.object(key)) return *cached;
    try {
        mathbudget::Session budget(qMin(75, budgetMs));
        tex::Formula formula;
        tex::TeXParser parser(false, source.toStdWString(), &formula);
        parser.parse();
        mathbudget::check();
        tex::TeXRenderBuilder builder;
        builder.setStyle(display ? tex::TexStyle::display : tex::TexStyle::text)
            .setTextSize(float(fontPixels)).setWidth(tex::UnitType::pixel, float(width), tex::Alignment::left)
            .setIsMaxWidth(true).setLineSpace(tex::UnitType::pixel, float(fontPixels * 0.25))
            .setForeground(color.rgba());
        std::unique_ptr<tex::TeXRender> render(builder.build(formula));
        mathbudget::check();
        if (!render || render->getWidth() <= 0 || render->getHeight() <= 0 ||
            render->getWidth() > 16000 || render->getHeight() > 4096) return {};
        const qreal scale = qMin<qreal>(1, width / render->getWidth());
        const QSizeF size(qCeil(render->getWidth() * scale) + 4, qCeil(render->getHeight() * scale) + 4);
        // A fixed 2x raster keeps formulas crisp on both PC and tablet without huge textures.
        const QSize raster(qCeil(size.width() * 2), qCeil(size.height() * 2));
        if (qint64(raster.width()) * raster.height() > 2 * 1024 * 1024) return {};
        QImage pixels(raster, QImage::Format_ARGB32_Premultiplied); pixels.fill(Qt::transparent);
        QPainter painter(&pixels); painter.setRenderHint(QPainter::Antialiasing); painter.setRenderHint(QPainter::TextAntialiasing);
        painter.scale(2 * scale, 2 * scale);
        tex::Graphics2D_qt graphics(&painter);
        render->draw(graphics, 2, 2);
        painter.end();
        mathbudget::check();
        MathImage result{pixels, size};
        images.insert(key, new MathImage(result), qMax(1, int(pixels.sizeInBytes() / 1024)));
        return result;
    } catch (const std::exception &) {
        return {}; // Parse/limit errors are rendered as literal source by the document.
    }
}
}
