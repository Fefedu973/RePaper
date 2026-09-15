#pragma once
#include <QString>
#include <QVector>
namespace rmchat {
struct MathSpan { QString marker, source, literal; bool display = false; bool incomplete = false; };
struct ScannedMarkdown { QString markdown; QVector<MathSpan> math; bool limited = false; };
// Identifies math delimiters; actual TeX syntax is parsed only by MicroTeX.
ScannedMarkdown scanMarkdown(const QString &source);
}
