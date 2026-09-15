#include "MarkdownScanner.h"
#include <QUuid>

namespace rmchat {
namespace {
bool escaped(const QString &source, qsizetype position) {
    int count = 0;
    while (position > 0 && source.at(--position) == '\\') ++count;
    return count % 2;
}
qsizetype runLength(const QString &source, qsizetype position, QChar ch) {
    qsizetype end = position;
    while (end < source.size() && source.at(end) == ch) ++end;
    return end - position;
}
}
ScannedMarkdown scanMarkdown(const QString &source) {
    ScannedMarkdown out;
    const auto prefix = "RMCHATMATH" + QUuid::createUuid().toString(QUuid::Id128).toUpper() + "X";
    QChar fence;
    qsizetype fenceLength = 0, codeTicks = 0;
    bool lineStart = true, indented = false;
    for (qsizetype i = 0; i < source.size();) {
        if (lineStart) {
            qsizetype p = i;
            int spaces = 0;
            while (p < source.size() && source.at(p) == ' ') { ++p; ++spaces; }
            while (p < source.size() && source.at(p) == '>') {
                ++p; if (p < source.size() && source.at(p) == ' ') ++p;
            }
            // Fences can start inside a list item, including a quoted list.
            qsizetype list = p;
            if (list + 1 < source.size() && QStringLiteral("-+*").contains(source.at(list)) && source.at(list + 1) == ' ') list += 2;
            else {
                while (list < source.size() && source.at(list).isDigit()) ++list;
                if (list > p && list + 1 < source.size() && (source.at(list) == '.' || source.at(list) == ')') && source.at(list + 1) == ' ') list += 2;
                else list = p;
            }
            if (list > p) { p = list; while (p < source.size() && source.at(p) == ' ') ++p; }
            indented = spaces >= 4 || (p < source.size() && source.at(p) == '\t');
            if (p < source.size() && (source.at(p) == '`' || source.at(p) == '~')) {
                const auto length = runLength(source, p, source.at(p));
                if (length >= 3 && (fence.isNull() || (source.at(p) == fence && length >= fenceLength))) {
                    if (fence.isNull()) { fence = source.at(p); fenceLength = length; }
                    else { fence = {}; fenceLength = 0; }
                    const auto end = source.indexOf('\n', p);
                    const auto next = end < 0 ? source.size() : end + 1;
                    out.markdown += source.mid(i, next - i); i = next; lineStart = true; codeTicks = 0; continue;
                }
            }
            lineStart = false;
        }
        const auto ch = source.at(i);
        if (ch == '\n') { out.markdown += ch; ++i; lineStart = true; continue; }
        if (!fence.isNull() || indented) { out.markdown += ch; ++i; continue; }
        if (ch == '`' && !escaped(source, i)) {
            const auto count = runLength(source, i, ch);
            if (codeTicks == 0) codeTicks = count;
            else if (codeTicks == count) codeTicks = 0;
            out.markdown += source.mid(i, count); i += count; continue;
        }
        if (codeTicks) { out.markdown += ch; ++i; continue; }
        QString closing;
        qsizetype openingLength = 0;
        bool display = false;
        if (ch == '\\' && !escaped(source, i) && i + 1 < source.size() && (source.at(i + 1) == '(' || source.at(i + 1) == '[')) {
            display = source.at(i + 1) == '['; closing = display ? "\\]" : "\\)"; openingLength = 2;
        } else if (ch == '$' && !escaped(source, i)) {
            display = i + 1 < source.size() && source.at(i + 1) == '$';
            openingLength = display ? 2 : 1; closing = display ? "$$" : "$";
            if (!display && (i + 1 >= source.size() || source.at(i + 1).isSpace())) openingLength = 0;
        }
        if (!openingLength) { out.markdown += ch; ++i; continue; }
        qsizetype end = i + openingLength;
        for (;;) {
            end = source.indexOf(closing, end);
            if (end < 0) break;
            const bool badSingleDollar = closing == "$" && (end == i + 1 || source.at(end - 1).isSpace() ||
                (end + 1 < source.size() && source.at(end + 1).isDigit()));
            if (!escaped(source, end) && !badSingleDollar) break;
            end += closing.size();
        }
        if (end < 0 && ch == '$' && !display) { out.markdown += ch; ++i; continue; } // Currency and unmatched prose dollar.
        const bool incomplete = end < 0;
        const auto next = incomplete ? source.size() : end + closing.size();
        if (out.math.size() >= 128) {
            out.limited = true;
            out.markdown += "\n\n" + QStringLiteral("[Suite du message affichée comme texte : limite de formules atteinte.]\n\n");
            out.markdown += source.mid(i); break;
        }
        const auto marker = prefix + QString::number(out.math.size()) + "END";
        out.math.append({marker, source.mid(i + openingLength, (incomplete ? source.size() : end) - i - openingLength), source.mid(i, next - i), display, incomplete});
        if (display) out.markdown += "\n\n";
        out.markdown += marker;
        if (display) out.markdown += "\n\n";
        i = next;
        lineStart = i > 0 && source.at(i - 1) == '\n';
    }
    return out;
}
}
