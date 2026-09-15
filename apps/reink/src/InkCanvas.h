#pragma once
#include "Geometry.h"
#include "../../../shared/drawing/DrawingModel.h"
#include "../../../shared/drawing/AlignmentGuide.h"
#include <QQuickPaintedItem>
#include <QJsonObject>
#include <QVariantList>

namespace repaper { class BridgeClient; }

class InkCanvas : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(QString tool READ tool WRITE setTool NOTIFY settingsChanged)
    Q_PROPERTY(QString lineStyle READ lineStyle WRITE setLineStyle NOTIFY settingsChanged)
    Q_PROPERTY(qreal lineWidth READ lineWidth WRITE setLineWidth NOTIFY settingsChanged)
    Q_PROPERTY(QString lineColor READ lineColor NOTIFY settingsChanged)
    Q_PROPERTY(QString selectedLineColor READ selectedLineColor NOTIFY documentChanged)
    Q_PROPERTY(bool snapping READ snapping WRITE setSnapping NOTIFY settingsChanged)
    Q_PROPERTY(bool gridVisible READ gridVisible WRITE setGridVisible NOTIFY settingsChanged)
    Q_PROPERTY(bool horizontalFirst READ horizontalFirst WRITE setHorizontalFirst NOTIFY settingsChanged)
    Q_PROPERTY(QString symbolId READ symbolId WRITE setSymbolId NOTIFY settingsChanged)
    Q_PROPERTY(QVariantList symbols READ symbols CONSTANT)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY documentChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY documentChanged)
    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY documentChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(int itemCount READ itemCount NOTIFY documentChanged)
    Q_PROPERTY(bool selectionHasEndpoints READ selectionHasEndpoints NOTIFY documentChanged)
    Q_PROPERTY(bool selectionCanChangeStyle READ selectionCanChangeStyle NOTIFY documentChanged)
    Q_PROPERTY(bool selectionIsArrow READ selectionIsArrow NOTIFY documentChanged)
    Q_PROPERTY(bool selectionIsWire READ selectionIsWire NOTIFY documentChanged)
    Q_PROPERTY(qreal selectedLineWidth READ selectedLineWidth NOTIFY documentChanged)
    Q_PROPERTY(QString selectedLineStyle READ selectedLineStyle NOTIFY documentChanged)
    Q_PROPERTY(QString selectedArrowDirection READ selectedArrowDirection NOTIFY documentChanged)
    Q_PROPERTY(bool selectedHorizontalFirst READ selectedHorizontalFirst NOTIFY documentChanged)
    Q_PROPERTY(QString selectedObjectId READ selectedObjectId NOTIFY documentChanged)
    Q_PROPERTY(QString selectionKind READ selectionKind NOTIFY documentChanged)
    Q_PROPERTY(bool selectionCanResize READ selectionCanResize NOTIFY documentChanged)
    Q_PROPERTY(qreal selectedShapeWidth READ selectedShapeWidth NOTIFY documentChanged)
    Q_PROPERTY(qreal selectedShapeHeight READ selectedShapeHeight NOTIFY documentChanged)
    Q_PROPERTY(qreal selectedCornerRadius READ selectedCornerRadius NOTIFY documentChanged)
    Q_PROPERTY(qreal selectedWireBend READ selectedWireBend NOTIFY documentChanged)
    Q_PROPERTY(QVariantList selectedWireRoute READ selectedWireRoute NOTIFY documentChanged)
    Q_PROPERTY(bool selectedWireAutomatic READ selectedWireAutomatic NOTIFY documentChanged)
    Q_PROPERTY(QVariantList selectedPorts READ selectedPorts NOTIFY documentChanged)
    Q_PROPERTY(QVariantList selectionHandlePoints READ selectionHandlePoints NOTIFY documentChanged)
    Q_PROPERTY(QString selectedStencilId READ selectedStencilId NOTIFY documentChanged)
    Q_PROPERTY(QVariantMap selectedStencilParameters READ selectedStencilParameters NOTIFY documentChanged)
    Q_PROPERTY(QVariantList alignmentGuides READ alignmentGuides NOTIFY alignmentGuidesChanged)
    Q_PROPERTY(QString activeStencilId READ symbolId NOTIFY settingsChanged)
    Q_PROPERTY(QStringList recentStencils READ recentStencils NOTIFY settingsChanged)
    Q_PROPERTY(bool stencilVertical READ stencilVertical NOTIFY settingsChanged)
    Q_PROPERTY(bool stencilSupportsVoltage READ stencilSupportsVoltage NOTIFY settingsChanged)
    Q_PROPERTY(bool stencilVoltageArrow READ stencilVoltageArrow NOTIFY settingsChanged)
    Q_PROPERTY(bool stencilVoltageReversed READ stencilVoltageReversed NOTIFY settingsChanged)
    Q_PROPERTY(bool stencilVoltageOtherSide READ stencilVoltageOtherSide NOTIFY settingsChanged)
    Q_PROPERTY(bool selectionSupportsVoltage READ selectionSupportsVoltage NOTIFY documentChanged)
    Q_PROPERTY(bool selectedVoltageArrow READ selectedVoltageArrow NOTIFY documentChanged)
    Q_PROPERTY(bool selectedVoltageReversed READ selectedVoltageReversed NOTIFY documentChanged)
    Q_PROPERTY(bool selectedVoltageOtherSide READ selectedVoltageOtherSide NOTIFY documentChanged)
public:
    explicit InkCanvas(QQuickItem *parent=nullptr);
    QString tool() const { return m_tool; }
    QString lineStyle() const { return m_style; }
    qreal lineWidth() const { return m_width; }
    QString lineColor() const { return m_color.name(); }
    QString selectedLineColor() const;
    bool setLineColor(const QString &color);
    bool setSelectionLineColor(const QString &color);
    bool snapping() const { return m_snapping; }
    bool gridVisible() const { return m_grid; }
    bool horizontalFirst() const { return m_horizontalFirst; }
    QString symbolId() const { return m_symbolId; }
    QVariantList symbols() const;
    bool canUndo() const { return !m_undo.isEmpty(); }
    bool canRedo() const { return !m_redo.isEmpty(); }
    bool hasSelection() const { return m_selection>=0 && m_selection<m_items.size(); }
    QString status() const { return m_status; }
    int itemCount() const { return m_items.size(); }
    bool selectionHasEndpoints() const;
    bool selectionCanChangeStyle() const;
    bool selectionIsArrow() const;
    bool selectionIsWire() const;
    qreal selectedLineWidth() const;
    QString selectedLineStyle() const;
    QString selectedArrowDirection() const;
    bool selectedHorizontalFirst() const;
    QString selectedObjectId() const;
    QString selectionKind() const;
    bool selectionCanResize() const;
    qreal selectedShapeWidth() const;
    qreal selectedShapeHeight() const;
    qreal selectedCornerRadius() const;
    qreal selectedWireBend() const;
    QVariantList selectedWireRoute() const;
    bool selectedWireAutomatic() const;
    QVariantList selectedPorts() const;
    QVariantList selectionHandlePoints() const;
    QString selectedStencilId() const;
    QVariantMap selectedStencilParameters() const;
    QVariantList alignmentGuides() const;
    QStringList recentStencils() const { return m_recentStencils; }
    bool stencilVertical() const { return m_stencilVertical; }
    bool stencilSupportsVoltage() const { return PaperDrawing::supportsVoltageArrow(m_symbolId); }
    bool stencilVoltageArrow() const { return m_stencilVoltageArrow; }
    bool stencilVoltageReversed() const { return m_stencilVoltageReversed; }
    bool stencilVoltageOtherSide() const { return m_stencilVoltageOtherSide; }
    bool selectionSupportsVoltage() const;
    bool selectedVoltageArrow() const;
    bool selectedVoltageReversed() const;
    bool selectedVoltageOtherSide() const;
    void setTool(const QString &value);
    void setLineStyle(const QString &value);
    void setLineWidth(qreal value);
    void setSnapping(bool value);
    void setGridVisible(bool value);
    void setHorizontalFirst(bool value);
    void setSymbolId(const QString &value);
    void paint(QPainter *painter) override;
    Q_INVOKABLE void begin(qreal x,qreal y);
    Q_INVOKABLE void move(qreal x,qreal y);
    Q_INVOKABLE void end(qreal x,qreal y);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void clear();
    Q_INVOKABLE void removeSelection();
    Q_INVOKABLE void scaleSelection(qreal factor);
    Q_INVOKABLE void rotateSelection();
    Q_INVOKABLE void duplicateSelection();
    Q_INVOKABLE void setSelectionLineWidth(qreal width);
    Q_INVOKABLE void setSelectionLineStyle(const QString &style);
    Q_INVOKABLE void setSelectionArrowDirection(const QString &direction);
    Q_INVOKABLE void setSelectionHorizontalFirst(bool horizontalFirst);
    Q_INVOKABLE void insertSymbol(const QString &id, qreal x = 702, qreal y = 936);
    Q_INVOKABLE bool selectObject(const QString &id);
    Q_INVOKABLE bool resizeSelection(qreal width, qreal height);
    Q_INVOKABLE bool setSelectionCornerRadius(qreal radius);
    Q_INVOKABLE bool setSelectionWireBend(qreal offset);
    Q_INVOKABLE bool setSelectionWireRoute(const QVariantList &points);
    Q_INVOKABLE bool beginStencil(const QString &symbolId);
    Q_INVOKABLE bool beginConfiguredStencil(const QString &symbolId, const QVariantMap &parameters);
    Q_INVOKABLE bool setSelectedStencilParameters(const QVariantMap &parameters);
    Q_INVOKABLE bool setStencilVertical(bool vertical);
    Q_INVOKABLE bool setVoltageArrow(bool enabled);
    Q_INVOKABLE bool setVoltageReversed(bool reversed);
    Q_INVOKABLE bool setVoltageOtherSide(bool otherSide);
    Q_INVOKABLE void duplicateObjects(const QStringList &ids);
    Q_INVOKABLE QVariantMap documentSnapshot() const;
    Q_INVOKABLE QVariantList renderedStrokes(bool includePreview = false) const;
    Q_INVOKABLE void exportDrawing(const QString &format);
    Q_INVOKABLE void importDrawing();
signals:
    void settingsChanged();
    void documentChanged();
    void statusChanged();
    void alignmentGuidesChanged();
private:
    using DrawingItem = repaper::drawing::Item;
    using Document = repaper::drawing::Document;
    QPointF toPage(qreal x,qreal y) const;
    qreal pageScale() const;
    QPointF pageOffset() const;
    QVector<QPointF> anchors(int excludedItem=-1) const;
    QVector<PaperDrawing::Stroke> draftStrokes() const;
    QVector<PaperDrawing::Stroke> allStrokes() const;
    void checkpoint();
    void finishMutation();
    void save();
    void load();
    void setStatus(const QString &status);
    void transformSelection(const QTransform &transform);
    static bool hasEditableEndpoints(const DrawingItem &item);
    static bool hasSourcePath(const DrawingItem &item);
    static bool rebuildItem(DrawingItem &item);
    static void inferLegacyGeometry(DrawingItem &item);
    bool fitsDocumentBudget(const DrawingItem &item, int replacing=-1) const;
    bool applySelectionItem(const DrawingItem &item);
    bool commitDocument(Document next, const QString &message = {});
    bool previewItem(const DrawingItem &item);
    DrawingItem draftItem() const;
    static QJsonObject encodeDocument(const Document &document);
    int endpointAt(const QPointF &point) const;
    int boxHandleAt(const QPointF &point) const;
    int wireSegmentAt(const QPointF &point) const;
    void setAlignmentGuides(const QVector<repaper::drawing::AlignmentGuide> &guides);
    void configureStencilItem(DrawingItem &item,QPointF center) const;
    bool setVoltageOption(bool DrawingItem::*member,bool &preference,bool value);
    QString m_tool="pen",m_style="solid",m_symbolId="resistor-iec",m_status;
    QVariantMap m_stencilParameters;
    QStringList m_recentStencils{"resistor-iec","capacitor","voltage-source","square-root"};
    qreal m_width=3;
    QColor m_color=Qt::black;
    bool m_snapping=true,m_grid=true,m_horizontalFirst=true,m_drawing=false;
    bool m_stencilVertical=false,m_stencilVoltageArrow=false,m_stencilVoltageReversed=false,m_stencilVoltageOtherSide=false;
    bool m_persistenceBlocked=false;
    int m_selection=-1,m_dragEndpoint=-1,m_dragCorner=-1,m_dragSegment=-1;
    bool m_dragChanged=false;
    QPointF m_start,m_last,m_handleOffset;
    PaperDrawing::Polyline m_draft;
    mutable PaperDrawing::Polyline m_wireDraftRoute;
    Document m_items,m_beforeDrag;
    DrawingItem m_original;
    DrawingItem m_alignmentPrototype;
    mutable repaper::drawing::StencilPlacementResult m_stencilPlacement;
    QRectF m_stencilCenterRange;
    QVector<repaper::drawing::AlignmentGuide> m_alignmentGuides;
    QVector<Document> m_undo,m_redo;
    repaper::BridgeClient *m_bridge;
};
