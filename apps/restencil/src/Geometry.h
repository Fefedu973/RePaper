#pragma once

#include <QColor>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>

class QPainter;

namespace PaperDrawing {
using Polyline = QVector<QPointF>;
struct Stroke {
    Polyline points;
    qreal width = 3.0;
    QColor color = Qt::black;
};
struct Symbol {
    QString id;
    QString name;
    QString category;
    QString standard;
    QVector<Stroke> strokes;
    QVector<QPointF> anchors;
    // Stable catalogue port names, paired by index with anchors.
    QStringList portIds;
};

constexpr qreal PageWidth = 1404;
constexpr qreal PageHeight = 1872;
QVector<Symbol> electronicsCatalogue();
// Two-terminal electrical components support one unlabeled voltage arrow.
bool supportsVoltageArrow(const Symbol &symbol);
bool supportsVoltageArrow(const QString &symbolId);
// Strokes use the catalogue's local coordinates and follow the component's box
// transform. Reversing direction does not change the selected side.
QVector<Stroke> voltageArrowStrokes(const Symbol &symbol, bool reversed = false,
                                  bool otherSide = false);
// Configurable stencils use the same nominal 120 x 80 coordinates as the catalogue.
bool isConfigurableStencil(QString id);
QVariantMap stencilDefaults(QString id);
QVariantList stencilParameterSchema(QString id);
// Validates a partial parameter map and supplies defaults; never clamps invalid input.
bool normalizeStencilParameters(QString id, const QVariantMap &input,
                                QVariantMap *output, QString *error = nullptr);
// Invalid parameters or an unknown id return an empty symbol. Background is rendered by the model.
// Graph sine: offset + amplitude * sin(2*pi*frequencyHz*t + phaseDegrees*pi/180).
Symbol configuredStencil(QString id, const QVariantMap &parameters = {});
// Closed contours inside the normalized rectangle. Invalid/empty bounds return {}.
// Radius is clamped to [0, min(width, height) / 2].
Polyline roundedRectangle(QRectF rect, qreal radius);
Polyline ellipse(QRectF rect);
// Open radical with an overbar. Width extends the overbar while the hook's
// proportions follow height; all points stay inside the normalized rectangle.
Polyline squareRoot(QRectF rect);
QVector<Polyline> dashByArcLength(const Polyline &points, const QVector<qreal> &pattern,
                                qreal phase = 0);
Polyline orthogonalRoute(QPointF start, QPointF end, bool horizontalFirst);
struct OccupiedRouteSegment {
    QPointF from, to;
    // Only explicitly connected junctions may share a collinear stub. A point
    // must belong to the actual overlapping interval to grant an exception.
    QVector<QPointF> sharedJunctions;
};
struct OrthogonalRouteOptions {
    QVector<QRectF> obstacles;
    QVector<OccupiedRouteSegment> occupiedSegments;
    // Cardinal vectors pointing OUT of their ports; zero means unconstrained.
    QPointF startDirection, endDirection;
    Polyline preferredRoute;
    qreal clearance = 12;
    qreal grid = 8;
    bool horizontalFirst = true;
};
// Endpoints remain exact. Intermediate bends use stable grid/obstacle lanes.
// Raw obstacle rectangles are expanded by clearance. A port may escape the
// rectangle containing it along its outward lead. Returns {} when no safe route
// exists or input/search complexity is invalid; never returns a crossing fallback.
Polyline orthogonalRoute(QPointF start, QPointF end, const OrthogonalRouteOptions &options);
QPointF portExitDirection(const Symbol &symbol, const QString &portId, int quarterTurns = 0);
QPointF snapPoint(QPointF point, const QVector<QPointF> &anchors, qreal radius,
                 qreal grid = 0);
QVector<Stroke> arrowHead(QPointF from, QPointF to, qreal size, qreal width);
QVector<Stroke> transformStrokes(const QVector<Stroke> &strokes, QPointF origin,
                               qreal scale, int quarterTurns = 0);
QRectF bounds(const QVector<Stroke> &strokes);
void paintStrokes(QPainter &painter, const QVector<Stroke> &strokes);
bool exportSvg(const QString &path, const QVector<Stroke> &strokes, QString *error);
bool exportPdf(const QString &path, const QVector<Stroke> &strokes,
               const QString &title, QString *error);
bool exportScene(const QString &path, const QVector<Stroke> &strokes,
                 const QString &title, QString *error);
QString exportDirectory(const QString &appName);
}
