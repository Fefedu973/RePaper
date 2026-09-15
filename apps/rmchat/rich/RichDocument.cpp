#include "RichDocument.h"
#include "MarkdownScanner.h"
#include <QAbstractTextDocumentLayout>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QPainter>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextTable>
#include <QUrl>
#include <QtMath>

namespace rmchat {
namespace {
constexpr int MathObjectType = QTextFormat::UserObject + 81;
constexpr int MathImageIndex = QTextFormat::UserProperty + 81;
constexpr int MessageByteLimit = 128 * 1024;
constexpr qreal HeightLimit = 6000;
constexpr qint64 ImageByteLimit = 8 * 1024 * 1024;
constexpr qreal SurfacePixelLimit = 4 * 1024 * 1024;
constexpr int MessageMathBudgetMs = 350;
QTextCharFormat literalFormat(qreal size, const QColor &color) {
    QTextCharFormat format;
    auto font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPixelSize(qRound(size));
    format.setFont(font); format.setForeground(color);
    return format;
}
}
QSizeF MathObject::intrinsicSize(QTextDocument *, int, const QTextFormat &format) {
    const int index = format.intProperty(MathImageIndex);
    return index >= 0 && index < images.size() ? images.at(index).size : QSizeF();
}
void MathObject::drawObject(QPainter *painter, const QRectF &rect, QTextDocument *, int, const QTextFormat &format) {
    const int index = format.intProperty(MathImageIndex);
    if (index < 0 || index >= images.size()) return;
    painter->save(); painter->setRenderHint(QPainter::SmoothPixmapTransform);
    painter->drawImage(rect, images.at(index).pixels); painter->restore();
}
RichDocument::RichDocument(QObject *parent) : QTextDocument(parent), m_math(new MathObject(this)) {
    MathEngine::initialize();
    setUndoRedoEnabled(false);
    setDocumentMargin(2);
    documentLayout()->registerHandler(MathObjectType, m_math);
}
QVariant RichDocument::loadResource(int type, const QUrl &) {
    ++m_blockedResources;
    // Deliberately never delegates to QTextDocument: that can read local files.
    if (type == QTextDocument::ImageResource) {
        QImage empty(1, 1, QImage::Format_ARGB32_Premultiplied); empty.fill(Qt::transparent); return empty;
    }
    return {};
}
bool RichDocument::isSafeLink(const QUrl &url) {
    return url.isValid() && url.scheme().compare("https", Qt::CaseInsensitive) == 0 &&
        !url.host().isEmpty() && url.userInfo().isEmpty();
}
void RichDocument::setSource(const QString &source, qreal width, qreal fontPixels, const QColor &color, qreal devicePixelRatio) {
    QElapsedTimer elapsed; elapsed.start();
    m_math->images.clear(); m_mathErrors = 0; m_blockedResources = 0; m_truncated = false;
    clear();
    fontPixels = qBound<qreal>(10, fontPixels, 48);
    QFont font("DejaVu Sans"); font.setPixelSize(qRound(fontPixels)); setDefaultFont(font);
    QTextOption option; option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere); setDefaultTextOption(option);
    setTextWidth(qBound<qreal>(40, width, 1600));
    devicePixelRatio = qBound<qreal>(1, devicePixelRatio, 4);
    m_heightLimit = qMin<qreal>(HeightLimit, SurfacePixelLimit / (textWidth() * devicePixelRatio * devicePixelRatio));
    QString bounded = source;
    if (bounded.toUtf8().size() > MessageByteLimit) {
        auto bytes = bounded.toUtf8().left(MessageByteLimit);
        while (!bytes.isEmpty() && (static_cast<unsigned char>(bytes.back()) & 0xc0) == 0x80) bytes.chop(1);
        if (!bytes.isEmpty() && static_cast<unsigned char>(bytes.back()) >= 0xc0) bytes.chop(1);
        bounded = QString::fromUtf8(bytes); m_truncated = true;
    }
    const auto scanned = scanMarkdown(bounded);
    m_truncated = m_truncated || scanned.limited;
    setMarkdown(scanned.markdown, QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub) | QTextDocument::MarkdownNoHTML);
    QTextCursor editBatch(this);
    editBatch.beginEditBlock();
    styleDocument(fontPixels, color);
    const auto fallbackFormat = literalFormat(fontPixels, color);
    qint64 imageBytes = 0;
    // This deadline bounds math work; Qt Markdown/layout has its own size caps.
    elapsed.restart();
    for (const auto &span : scanned.math) {
        auto cursor = find(span.marker);
        if (cursor.isNull()) continue; // A marker inside Markdown link metadata is never made executable.
        MathImage math;
        const int remaining = MessageMathBudgetMs - int(elapsed.elapsed());
        if (!span.incomplete && remaining > 0)
            math = MathEngine::render(span.source, span.display, fontPixels, qMax<qreal>(40, textWidth() - 24), color, qMin(75, remaining));
        if (math.valid() && imageBytes + math.pixels.sizeInBytes() > ImageByteLimit) math = {};
        if (!math.valid()) {
            cursor.insertText(span.literal, fallbackFormat);
            if (!span.incomplete) ++m_mathErrors;
            continue;
        }
        QTextCharFormat format;
        format.setObjectType(MathObjectType); format.setProperty(MathImageIndex, int(m_math->images.size()));
        format.setVerticalAlignment(QTextCharFormat::AlignMiddle);
        imageBytes += math.pixels.sizeInBytes();
        m_math->images.append(math);
        cursor.insertText(QString(QChar::ObjectReplacementCharacter), format);
        if (span.display) {
            auto block = cursor.blockFormat(); block.setAlignment(Qt::AlignHCenter);
            block.setTopMargin(fontPixels * .4); block.setBottomMargin(fontPixels * .4); cursor.setBlockFormat(block);
        }
    }
    if (m_truncated) {
        QTextCursor cursor(this); cursor.movePosition(QTextCursor::End); cursor.insertBlock();
        cursor.insertText(QStringLiteral("[Affichage limité : le message dépasse la taille prise en charge.]"), fallbackFormat);
    }
    editBatch.endEditBlock();
    limitHeight();
}
void RichDocument::styleDocument(qreal fontPixels, const QColor &color) {
    QTextCursor all(this); all.select(QTextCursor::Document);
    QTextCharFormat foreground; foreground.setForeground(color); all.mergeCharFormat(foreground);
    QVector<QTextCursor> externalImages;
    for (auto block = begin(); block.isValid(); block = block.next()) {
        QTextCursor cursor(block);
        auto format = block.blockFormat();
        format.setBottomMargin(fontPixels * .35);
        if (format.hasProperty(QTextFormat::BlockCodeFence) || format.nonBreakableLines()) {
            format.setBackground(QColor("#eeeeee")); format.setNonBreakableLines(false);
            format.setLeftMargin(8); format.setRightMargin(8);
            // BlockUnderCursor includes the preceding paragraph separator and
            // would apply the code block format to the previous list item too.
            cursor.setPosition(block.position());
            cursor.setPosition(block.position() + block.length() - 1, QTextCursor::KeepAnchor);
            cursor.mergeCharFormat(literalFormat(fontPixels * .90, color));
            cursor.clearSelection();
        }
        if (format.intProperty(QTextFormat::BlockQuoteLevel) > 0) {
            format.setLeftMargin(fontPixels); format.setBackground(QColor("#f1f1f1"));
        }
        cursor.setBlockFormat(format);
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment(); if (!fragment.isValid()) continue;
            const auto chars = fragment.charFormat();
            if (chars.isImageFormat()) {
                QTextCursor imageCursor(this); imageCursor.setPosition(fragment.position());
                imageCursor.setPosition(fragment.position() + fragment.length(), QTextCursor::KeepAnchor); externalImages.append(imageCursor);
            } else if (chars.isAnchor()) {
                QTextCursor anchor(this); anchor.setPosition(fragment.position());
                anchor.setPosition(fragment.position() + fragment.length(), QTextCursor::KeepAnchor);
                QTextCharFormat link; link.setFontUnderline(true); link.setForeground(color); anchor.mergeCharFormat(link);
            }
        }
    }
    for (auto it = externalImages.rbegin(); it != externalImages.rend(); ++it) {
        const auto alt = it->charFormat().stringProperty(QTextFormat::ImageAltText);
        it->insertText(alt.isEmpty() ? QStringLiteral("[Image non chargée]") : QStringLiteral("[Image : %1]").arg(alt), literalFormat(fontPixels * .9, color));
    }
    QList<QTextFrame *> frames = rootFrame()->childFrames();
    for (int i = 0; i < frames.size(); ++i) {
        auto *frame = frames.at(i); frames.append(frame->childFrames());
        if (auto *table = qobject_cast<QTextTable *>(frame)) {
            auto format = table->format(); format.setWidth(QTextLength(QTextLength::PercentageLength, 100));
            // GFM tables always have a header; Qt 6.2's importer can omit this property.
            format.setHeaderRowCount(1);
            format.setCellPadding(6); format.setCellSpacing(0); format.setBorder(1);
            format.setBorderBrush(QColor("#777777")); format.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
            QList<QTextLength> columns;
            for (int column = 0; column < table->columns(); ++column) columns.append(QTextLength(QTextLength::PercentageLength, 100.0 / table->columns()));
            format.setColumnWidthConstraints(columns); table->setFormat(format);
            for (int row = 0; row < table->rows(); ++row) for (int column = 0; column < table->columns(); ++column) {
                auto cell = table->cellAt(row, column);
                auto leading = cell.firstCursorPosition();
                if (leading.block().text().isEmpty() && leading.block().next().isValid() &&
                    leading.block().next().position() < cell.lastCursorPosition().position()) {
                    leading.movePosition(QTextCursor::NextBlock, QTextCursor::KeepAnchor); leading.removeSelectedText();
                }
                auto cellFormat = cell.format().toTableCellFormat();
                cellFormat.setVerticalAlignment(QTextCharFormat::AlignMiddle);
                if (row < format.headerRowCount()) cellFormat.setBackground(QColor("#ededed"));
                cell.setFormat(cellFormat);
                auto cellCursor = cell.firstCursorPosition();
                const auto end = cell.lastCursorPosition().position();
                do {
                    auto blockFormat = cellCursor.blockFormat(); blockFormat.setTopMargin(0); blockFormat.setBottomMargin(2);
                    blockFormat.setLeftMargin(0); blockFormat.setRightMargin(0); cellCursor.setBlockFormat(blockFormat);
                } while (cellCursor.movePosition(QTextCursor::NextBlock) && cellCursor.position() < end);
                if (row < format.headerRowCount()) {
                    cellCursor = cell.firstCursorPosition(); cellCursor.setPosition(end, QTextCursor::KeepAnchor);
                    QTextCharFormat header; header.setFontWeight(QFont::Bold); cellCursor.mergeCharFormat(header);
                }
            }
        }
    }
}
void RichDocument::limitHeight() {
    if (size().height() <= m_heightLimit) return;
    // Bound QQuickPaintedItem's texture and scrolling geometry, with an explicit notice.
    const int position = documentLayout()->hitTest(QPointF(2, m_heightLimit - 180), Qt::FuzzyHit);
    QTextCursor cursor(this); cursor.setPosition(qBound(0, position, characterCount() - 1));
    cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor); cursor.removeSelectedText();
    cursor.insertBlock(); cursor.insertText(QStringLiteral("[Affichage limité : le message est trop long.]"));
    m_truncated = true;
    if (size().height() > m_heightLimit) {
        // Removing a table cell can leave the enclosing frame tall; replace that pathological layout.
        auto excerpt = toPlainText().left(800).remove(QChar::ObjectReplacementCharacter);
        m_math->images.clear();
        setPlainText(excerpt + QStringLiteral("\n\n[Affichage limité : le message est trop long.]"));
        while (size().height() > m_heightLimit && !excerpt.isEmpty()) {
            excerpt.truncate(excerpt.size() / 2);
            setPlainText(excerpt + QStringLiteral("\n\n[Affichage limité : le message est trop long.]"));
        }
        if (size().height() > m_heightLimit) setPlainText(QStringLiteral("[Affichage limité]"));
    }
}
}
