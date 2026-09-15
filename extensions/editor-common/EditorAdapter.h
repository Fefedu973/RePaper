#pragma once
#include <QObject>
#include <QPointer>
#include <QVariantMap>
#include <QRectF>
class NativeScene;

// One UI contract, with separate real Xochitl and PC test-page backends.
class EditorAdapter : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(QString backend READ backend NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QVariantMap state READ state NOTIFY changed)
    Q_PROPERTY(QVariantMap evidence READ evidence NOTIFY changed)
    Q_PROPERTY(QVariantList stencils READ stencils CONSTANT)
    Q_PROPERTY(QVariantMap overlayState READ overlayState NOTIFY previewChanged)
    Q_PROPERTY(QVariant previewFrame READ previewFrame NOTIFY previewChanged)
    Q_PROPERTY(QVariantList previewStrokes READ previewStrokes NOTIFY previewChanged)
    Q_PROPERTY(bool captureEnabled READ captureEnabled NOTIFY changed)
public:
    explicit EditorAdapter(QObject *parent=nullptr);
    bool available() const;
    QString backend() const;
    QString status() const;
    QVariantMap state() const;
    QVariantMap overlayState() const;
    QVariant previewFrame() const;
    QVariantMap evidence() const;
    QVariantList stencils() const;
    QVariantList previewStrokes() const;
    bool captureEnabled() const;
    Q_INVOKABLE bool attachPcPage(QObject *page);
    Q_INVOKABLE bool inspectNativeObject(QObject *object);
    Q_INVOKABLE bool attachNativePage(QObject *controller,QObject *view,QObject *commandHost);
    Q_INVOKABLE void refreshNativeState(bool objectsMayHaveChanged=false);
    Q_INVOKABLE void setNativeToolActive(bool active);
    Q_INVOKABLE void nativeAreaSelected(int layer,const QRectF &rect={});
    Q_INVOKABLE void nativeContentChanged();
    Q_INVOKABLE void nativeViewTransformChanged();
    Q_INVOKABLE void nativePreviewPresented(qulonglong token);
    Q_INVOKABLE bool pointerBegin(qreal x,qreal y,const QString &kind);
    Q_INVOKABLE bool pointerMove(qreal x,qreal y);
    Q_INVOKABLE bool pointerMoveBatch(const QVariantList &points);
    Q_INVOKABLE bool pointerEnd(qreal x,qreal y);
    Q_INVOKABLE void pointerCancel();
    Q_INVOKABLE bool chooseTool(const QString &tool);
    Q_INVOKABLE bool setStrokeStyle(const QString &style);
    Q_INVOKABLE bool setStrokeWidth(qreal width);
    Q_INVOKABLE bool setStrokeColor(const QString &color);
    Q_INVOKABLE bool setArrowDirection(const QString &direction);
    Q_INVOKABLE bool setWireOrientation(bool horizontalFirst);
    Q_INVOKABLE bool setStencilVertical(bool vertical);
    Q_INVOKABLE bool setVoltageArrow(bool enabled);
    Q_INVOKABLE bool setVoltageReversed(bool reversed);
    Q_INVOKABLE bool setVoltageOtherSide(bool otherSide);
    Q_INVOKABLE bool insertStencil(const QString &symbolId);
    Q_INVOKABLE bool beginStencil(const QString &symbolId);
    Q_INVOKABLE bool beginConfiguredStencil(const QString &symbolId, const QVariantMap &parameters);
    Q_INVOKABLE QVariantList stencilSchema(const QString &symbolId) const;
    Q_INVOKABLE QVariantMap stencilDefaults(const QString &symbolId) const;
    Q_INVOKABLE QVariantMap stencilPreview(const QString &symbolId, const QVariantMap &parameters) const;
    Q_INVOKABLE bool setStencilParameters(const QVariantMap &parameters);
    Q_INVOKABLE bool resize(qreal width,qreal height);
    Q_INVOKABLE bool setCornerRadius(qreal radius);
    Q_INVOKABLE bool setWireBend(qreal offset);
    Q_INVOKABLE bool selectObject(const QString &id);
    Q_INVOKABLE bool undo();
    Q_INVOKABLE bool redo();
    Q_INVOKABLE bool rotate();
    Q_INVOKABLE bool scale(qreal factor);
    Q_INVOKABLE bool duplicate();
    Q_INVOKABLE bool remove();
    Q_INVOKABLE bool beginPropertiesSession();
    Q_INVOKABLE bool acceptPropertiesSession();
    Q_INVOKABLE bool cancelPropertiesSession();
signals:
    void changed();
    void previewChanged();
private:
    friend class PcAdapterTest;
    QVariantMap readState() const;
    void notifyStateChanged();
    void notifyPreviewChanged();
    enum class Action { Tool, Style, Width, Direction, Wire, Stencil, Undo, Redo, Rotate, Scale, Duplicate, Remove, Color };
    bool act(Action action,const QVariant &argument={});
    bool stencilOption(const char *method,const QString &action,bool value);
    QPointer<QObject> m_page;
    QVariantMap m_evidence;
    NativeScene *m_native;
    mutable QVariantMap m_cachedState,m_cachedOverlayState;
    mutable bool m_stateValid=false,m_overlayStateValid=false,m_readingState=false;
    mutable quint64 m_stateBuildCount=0;
    quint64 m_stateRevision=0,m_overlayRevision=0;
};
