#pragma once
#include <QColor>
#include <QImage>
#include <QSizeF>
#include <QString>

namespace rmchat {
struct MathImage {
    QImage pixels;
    QSizeF size;
    bool valid() const { return !pixels.isNull() && size.width() > 0 && size.height() > 0; }
};
class MathEngine {
public:
    static MathImage render(const QString &source, bool display, qreal fontPixels,
                            qreal width, const QColor &color, int timeBudgetMs = 75);
    static bool accepts(const QString &source);
    static void initialize();
};
}
