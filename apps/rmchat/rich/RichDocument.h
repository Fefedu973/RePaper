#pragma once
#include "MathEngine.h"
#include <QTextDocument>
#include <QTextObjectInterface>
#include <QVector>

namespace rmchat {
class MathObject : public QObject, public QTextObjectInterface {
    Q_OBJECT
    Q_INTERFACES(QTextObjectInterface)
public:
    explicit MathObject(QObject *parent = nullptr) : QObject(parent) {}
    QVector<MathImage> images;
    QSizeF intrinsicSize(QTextDocument *, int, const QTextFormat &format) override;
    void drawObject(QPainter *painter, const QRectF &rect, QTextDocument *, int, const QTextFormat &format) override;
};

class RichDocument : public QTextDocument {
    Q_OBJECT
public:
    explicit RichDocument(QObject *parent = nullptr);
    void setSource(const QString &source, qreal width, qreal fontPixels, const QColor &color, qreal devicePixelRatio = 1);
    bool hasMath() const { return !m_math->images.isEmpty(); }
    int mathErrorCount() const { return m_mathErrors; }
    bool truncated() const { return m_truncated; }
    int blockedResourceCount() const { return m_blockedResources; }
    static bool isSafeLink(const QUrl &url);
protected:
    QVariant loadResource(int type, const QUrl &name) override;
private:
    void styleDocument(qreal fontPixels, const QColor &color);
    void limitHeight();
    MathObject *m_math;
    int m_mathErrors = 0;
    int m_blockedResources = 0;
    bool m_truncated = false;
    qreal m_heightLimit = 6000;
};
}
