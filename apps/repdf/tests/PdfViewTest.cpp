#include "PdfView.h"
#include <QFile>
#include <QFileInfo>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtTest>

namespace {
bool makeDocument(const QString &path,bool single=false) {
    QPdfWriter writer(path);writer.setResolution(72);
    writer.setPageSize(QPageSize(QSizeF(200,300),QPageSize::Point));
    writer.setPageMargins(QMarginsF(),QPageLayout::Point);
    QPainter painter(&writer);
    if(!painter.isActive())return false;
    painter.fillRect(QRectF(0,0,100,150),Qt::red);
    painter.fillRect(QRectF(100,0,100,150),Qt::blue);
    painter.fillRect(QRectF(0,150,100,150),Qt::green);
    painter.fillRect(QRectF(100,150,100,150),Qt::yellow);
    if(!single){
        writer.setPageSize(QPageSize(QSizeF(300,200),QPageSize::Point));
        if(!writer.newPage())return false;
        painter.fillRect(QRectF(0,0,300,200),Qt::blue);
        writer.setPageSize(QPageSize(QSizeF(250,250),QPageSize::Point));
        if(!writer.newPage())return false;
        painter.fillRect(QRectF(0,0,250,250),Qt::green);
    }
    painter.end();return QFileInfo(path).size()>0;
}
QColor sample(const QImage &image,qreal x,qreal y) {
    return image.pixelColor(qBound(0,int(image.width()*x),image.width()-1),
                            qBound(0,int(image.height()*y),image.height()-1));
}
}

class PdfViewTest : public QObject {
    Q_OBJECT
    QTemporaryDir directory;
    QString document,other;
    QScopedPointer<PdfView> view;
private slots:
    void initTestCase() {
        document=directory.filePath(QString::fromUtf8("document-école.pdf"));
        other=directory.filePath("other.pdf");
        QVERIFY(makeDocument(document));QVERIFY(makeDocument(other,true));
    }
    void init() { view.reset(new PdfView);view->setWidth(240);view->setHeight(360); }
    void cleanup() {
        view.reset();QVERIFY(QThreadPool::globalInstance()->waitForDone(10000));
        QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    }
    void opensActualPdfAndRendersPageContents() {
        QCOMPARE(view->pageNumber(),0);QVERIFY(view->renderedImage().isNull());
        QSignalSpy busy(view.data(),&PdfView::busyChanged);
        QVERIFY(view->openFile(document,QString::fromUtf8("Cours d’électronique")));QVERIFY(view->busy());
        QTRY_VERIFY_WITH_TIMEOUT(!view->busy(),10000);
        QVERIFY2(view->error().isEmpty(),qPrintable(view->error()));
        QCOMPARE(view->source(),QFileInfo(document).canonicalFilePath());
        QCOMPARE(view->title(),QString::fromUtf8("Cours d’électronique"));
        QCOMPARE(view->pageCount(),3);QCOMPARE(view->pageNumber(),1);QCOMPARE(view->rotation(),0);
        QVERIFY(qAbs(view->aspectRatio()-2./3.)<.001);QCOMPARE(view->renderedImage().size(),QSize(240,360));
        QCOMPARE(sample(view->renderedImage(),.25,.25),QColor(Qt::red));
        QCOMPARE(sample(view->renderedImage(),.75,.75),QColor(Qt::yellow));
        QCOMPARE(busy.size(),2);
    }
    void navigatesDifferentPageSizesAndIgnoresOutOfRange() {
        view->setPageNumber(1);QCOMPARE(view->pageNumber(),0);
        QVERIFY(view->openFile(document));QTRY_VERIFY_WITH_TIMEOUT(!view->busy(),10000);
        QCOMPARE(view->pageCount(),3);
        view->setPageNumber(0);view->setPageNumber(-1);view->setPageNumber(4);
        QCOMPARE(view->pageNumber(),1);QVERIFY(!view->busy());
        view->setPageNumber(2);QVERIFY(view->busy());QTRY_VERIFY_WITH_TIMEOUT(!view->busy(),10000);
        QCOMPARE(view->pageNumber(),2);QVERIFY(qAbs(view->aspectRatio()-1.5)<.001);
        QCOMPARE(view->renderedImage().size(),QSize(240,160));
        QCOMPARE(sample(view->renderedImage(),.5,.5),QColor(Qt::blue));
        view->setPageNumber(3);QTRY_VERIFY_WITH_TIMEOUT(!view->busy(),10000);
        QCOMPARE(view->pageNumber(),3);QCOMPARE(view->renderedImage().size(),QSize(240,240));
        QCOMPARE(sample(view->renderedImage(),.5,.5),QColor(Qt::green));
    }
    void rotatesActualRasterAndRestoresOriginalOrientation() {
        QVERIFY(view->openFile(document));QTRY_VERIFY_WITH_TIMEOUT(!view->busy(),10000);
        view->rotateClockwise();QTRY_VERIFY_WITH_TIMEOUT(!view->busy(),10000);
        QCOMPARE(view->rotation(),90);QVERIFY(qAbs(view->aspectRatio()-1.5)<.001);
        QCOMPARE(view->renderedImage().size(),QSize(240,160));
        QCOMPARE(sample(view->renderedImage(),.25,.25),QColor(Qt::green));
        QCOMPARE(sample(view->renderedImage(),.75,.25),QColor(Qt::red));
        for(int i=0;i<3;++i){view->rotateClockwise();QTRY_VERIFY_WITH_TIMEOUT(!view->busy(),10000);}
        QCOMPARE(view->rotation(),0);QCOMPARE(view->renderedImage().size(),QSize(240,360));
        QCOMPARE(sample(view->renderedImage(),.25,.25),QColor(Qt::red));
    }
    void missingDirectoryAndCorruptFilesGiveErrors() {
        QVERIFY(!view->openFile(directory.filePath("missing.pdf")));QVERIFY(!view->error().isEmpty());QVERIFY(!view->busy());
        QVERIFY(!view->openFile(directory.path()));QVERIFY(!view->error().isEmpty());
        const auto corrupt=directory.filePath("corrupt.pdf");QFile file(corrupt);
        QVERIFY(file.open(QIODevice::WriteOnly));file.write("This is not a PDF");file.close();
        QVERIFY(view->openFile(corrupt));QTRY_VERIFY_WITH_TIMEOUT(!view->busy(),10000);
        QVERIFY(view->error().contains(QString::fromUtf8("endommagé")));
        QCOMPARE(view->pageCount(),0);QCOMPARE(view->pageNumber(),0);QVERIFY(view->renderedImage().isNull());
        view->setWidth(500);QTest::qWait(100);QVERIFY(!view->busy());
        view->closeDocument();QVERIFY(view->error().isEmpty());QVERIFY(view->source().isEmpty());
    }
    void rapidOpenNavigationAndCloseRejectStaleResults() {
        for(int i=0;i<20;++i){QVERIFY(view->openFile(document));view->closeDocument();}
        QVERIFY(view->openFile(document,"old"));QVERIFY(view->openFile(other,"current"));
        QTRY_VERIFY_WITH_TIMEOUT(!view->busy(),10000);
        QCOMPARE(view->title(),QString("current"));QCOMPARE(view->pageCount(),1);
        QVERIFY(view->openFile(document));QTRY_VERIFY_WITH_TIMEOUT(!view->busy(),10000);
        view->setPageNumber(2);view->setPageNumber(3);view->setPageNumber(1);view->rotateClockwise();
        QTRY_VERIFY_WITH_TIMEOUT(!view->busy(),10000);
        QCOMPARE(view->pageNumber(),1);QCOMPARE(view->rotation(),90);
        QCOMPARE(sample(view->renderedImage(),.25,.25),QColor(Qt::green));
        view->setWidth(4000);view->closeDocument();
        QVERIFY(QThreadPool::globalInstance()->waitForDone(10000));QCoreApplication::processEvents();
        QCOMPARE(view->pageNumber(),0);QCOMPARE(view->pageCount(),0);QCOMPARE(view->rotation(),0);
        QVERIFY(!view->busy());QVERIFY(view->renderedImage().isNull());QVERIFY(view->source().isEmpty());
    }
    void resizeDebouncesAndCapsRasterAllocation() {
        QVERIFY(view->openFile(document));QTRY_VERIFY_WITH_TIMEOUT(!view->busy(),10000);
        QSignalSpy busy(view.data(),&PdfView::busyChanged);
        for(int width=300;width<=900;width+=100)view->setWidth(width);
        QVERIFY(view->busy());QCOMPARE(view->renderedImage().width(),240);
        QTRY_VERIFY_WITH_TIMEOUT(!view->busy(),10000);
        QCOMPARE(view->renderedImage().size(),QSize(900,1350));QCOMPARE(busy.size(),2);
        view->setWidth(100000);QTRY_VERIFY_WITH_TIMEOUT(!view->busy(),10000);
        const auto image=view->renderedImage();QVERIFY(image.width()<=4096);QVERIFY(image.height()<=4096);
        QVERIFY(qint64(image.width())*image.height()<=8000000);
        QCOMPARE(view->textureSize(),image.size());
        QVERIFY(qAbs(qreal(image.width())/image.height()-view->aspectRatio())<.002);
        view->closeDocument();QCOMPARE(view->textureSize(),QSize(1,1));
    }
    void paintsImageFittedInsideItemBounds() {
        view->setWidth(200);view->setHeight(200);QVERIFY(view->openFile(document));
        QTRY_VERIFY_WITH_TIMEOUT(!view->busy(),10000);
        QImage target(200,200,QImage::Format_ARGB32);target.fill(Qt::black);
        QPainter painter(&target);view->paint(&painter);painter.end();
        QCOMPARE(target.pixelColor(5,100),QColor(Qt::white));
        QCOMPARE(target.pixelColor(60,40),QColor(Qt::red));
        QCOMPARE(target.pixelColor(140,160),QColor(Qt::yellow));
    }
};
QTEST_MAIN(PdfViewTest)
#include "PdfViewTest.moc"
