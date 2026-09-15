#pragma once

#include <QImage>
#include <QQuickPaintedItem>
#include <QTimer>
#include <memory>

struct PdfDocument;
struct PdfRenderGate;

class PdfView : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(QString source READ source NOTIFY documentChanged)
    Q_PROPERTY(QString title READ title NOTIFY documentChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(int pageNumber READ pageNumber WRITE setPageNumber NOTIFY viewChanged)
    Q_PROPERTY(int pageCount READ pageCount NOTIFY documentChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(qreal aspectRatio READ aspectRatio NOTIFY viewChanged)
    Q_PROPERTY(int rotation READ rotation NOTIFY viewChanged)
public:
    explicit PdfView(QQuickItem *parent = nullptr);
    ~PdfView() override;

    QString source() const { return m_source; }
    QString title() const { return m_title; }
    QString error() const { return m_error; }
    int pageNumber() const { return m_pageNumber; }
    int pageCount() const { return m_pageCount; }
    bool busy() const { return m_busy; }
    qreal aspectRatio() const { return m_aspectRatio; }
    int rotation() const { return m_rotation; }
    QImage renderedImage() const { return m_image; }

    Q_INVOKABLE bool openFile(QString path, QString displayName = QString());
    Q_INVOKABLE void closeDocument();
    Q_INVOKABLE void rotateClockwise();
    void setPageNumber(int pageNumber);
    void paint(QPainter *painter) override;

signals:
    void documentChanged();
    void viewChanged();
    void busyChanged();
    void errorChanged();

private:
    void requestRender();
    void scheduleResize();
    void setBusy(bool value);
    void setError(const QString &value);
    void clearState();

    QString m_source, m_title, m_error;
    int m_pageNumber = 0, m_pageCount = 0, m_rotation = 0;
    bool m_busy = false;
    qreal m_aspectRatio = 1.;
    QImage m_image;
    QTimer m_resizeTimer;
    std::shared_ptr<PdfDocument> m_document;
    std::shared_ptr<PdfRenderGate> m_gate;
};
