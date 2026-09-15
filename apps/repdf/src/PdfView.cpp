#include "PdfView.h"

#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QLibrary>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QQuickWindow>
#include <QUrl>
#include <QtConcurrent/QtConcurrentRun>
#include <fpdfview.h>
#include <atomic>
#include <cmath>

namespace {
// PDFium's public API is serialized across every viewer and worker. Keep the
// loaded library alive until process exit, including late worker completions.
struct Pdfium {
    QMutex mutex;
    QLibrary library;
    bool attempted=false, ready=false;
    decltype(&FPDF_InitLibrary) init=nullptr;
    decltype(&FPDF_LoadDocument) loadDocument=nullptr;
    decltype(&FPDF_GetLastError) lastError=nullptr;
    decltype(&FPDF_GetPageCount) pageCount=nullptr;
    decltype(&FPDF_LoadPage) loadPage=nullptr;
    decltype(&FPDF_GetPageWidthF) pageWidth=nullptr;
    decltype(&FPDF_GetPageHeightF) pageHeight=nullptr;
    decltype(&FPDF_ClosePage) closePage=nullptr;
    decltype(&FPDF_CloseDocument) closeDocument=nullptr;
    decltype(&FPDFBitmap_CreateEx) createBitmap=nullptr;
    decltype(&FPDFBitmap_Destroy) destroyBitmap=nullptr;
    decltype(&FPDF_RenderPageBitmap) render=nullptr;

    bool initialize() {
        if(attempted)return ready;
        attempted=true;
        library.setFileName(qEnvironmentVariable("REPAPER_PDFIUM_LIBRARY",QStringLiteral("pdfium")));
        library.setLoadHints(QLibrary::ResolveAllSymbolsHint|QLibrary::PreventUnloadHint);
        if(!library.load())return false;
#define PDF_SYMBOL(member, symbol) \
        member=reinterpret_cast<decltype(member)>(library.resolve(#symbol)); \
        if(!member)return false
        PDF_SYMBOL(init,FPDF_InitLibrary);
        PDF_SYMBOL(loadDocument,FPDF_LoadDocument);
        PDF_SYMBOL(lastError,FPDF_GetLastError);
        PDF_SYMBOL(pageCount,FPDF_GetPageCount);
        PDF_SYMBOL(loadPage,FPDF_LoadPage);
        PDF_SYMBOL(pageWidth,FPDF_GetPageWidthF);
        PDF_SYMBOL(pageHeight,FPDF_GetPageHeightF);
        PDF_SYMBOL(closePage,FPDF_ClosePage);
        PDF_SYMBOL(closeDocument,FPDF_CloseDocument);
        PDF_SYMBOL(createBitmap,FPDFBitmap_CreateEx);
        PDF_SYMBOL(destroyBitmap,FPDFBitmap_Destroy);
        PDF_SYMBOL(render,FPDF_RenderPageBitmap);
#undef PDF_SYMBOL
        init();ready=true;return true;
    }
};
Pdfium &pdfium() { static auto *api=new Pdfium;return *api; }

QString loadError(unsigned long code) {
    switch(code){
    case FPDF_ERR_FILE:return QStringLiteral("Impossible de lire ce fichier PDF.");
    case FPDF_ERR_FORMAT:return QStringLiteral("Ce fichier n’est pas un PDF valide ou il est endommagé.");
    case FPDF_ERR_PASSWORD:return QStringLiteral("Ce PDF est protégé par un mot de passe.");
    case FPDF_ERR_SECURITY:return QStringLiteral("La protection de ce PDF n’est pas prise en charge.");
    default:return QStringLiteral("Impossible d’ouvrir ce fichier PDF.");
    }
}

QSize renderSize(qreal aspect,qreal desiredWidth) {
    constexpr double maxDimension=4096.,maxPixels=8000000.;
    double width=qBound(1.,double(desiredWidth),maxDimension),height=width/aspect;
    const double longest=qMax(width,height);
    if(longest>maxDimension){const double scale=maxDimension/longest;width*=scale;height*=scale;}
    if(width*height>maxPixels){const double scale=std::sqrt(maxPixels/(width*height));width*=scale;height*=scale;}
    return QSize(qMax(1,int(std::floor(width))),qMax(1,int(std::floor(height))));
}
}

struct PdfRenderGate { std::atomic<quint64> generation{0}; };
struct PdfDocument {
    FPDF_DOCUMENT handle=nullptr;
    ~PdfDocument() {
        // Closing a viewer must never wait behind another PDF render's mutex.
        // Workers retain their own shared handle until their render is done.
        const auto closing=handle;
        if(closing)(void)QtConcurrent::run([closing]{
            auto &api=pdfium();QMutexLocker guard(&api.mutex);api.closeDocument(closing);
        });
    }
};

namespace {
struct RenderResult {
    std::shared_ptr<PdfDocument> document;
    QImage image;
    QString error;
    int pageNumber=0,pageCount=0;
    qreal aspect=1.;
};

RenderResult renderPage(std::shared_ptr<PdfDocument> document,const QString &path,
                        int number,int rotation,qreal desiredWidth,
                        const std::shared_ptr<PdfRenderGate> &gate,quint64 generation) {
    RenderResult result;
    if(gate->generation!=generation)return result;
    auto &api=pdfium();QMutexLocker guard(&api.mutex);
    if(gate->generation!=generation)return result;
    if(!api.initialize()){result.error=QStringLiteral("Le moteur PDF est indisponible.");return result;}
    if(!document){
        const QByteArray filename=QFile::encodeName(path);
        auto handle=api.loadDocument(filename.constData(),nullptr);
        if(!handle){result.error=loadError(api.lastError());return result;}
        document=std::make_shared<PdfDocument>();document->handle=handle;
    }
    if(gate->generation!=generation)return result;
    const int count=api.pageCount(document->handle);
    if(count<1){result.error=QStringLiteral("Ce PDF ne contient aucune page lisible.");return result;}
    auto page=api.loadPage(document->handle,number-1);
    if(!page){result.error=QStringLiteral("Cette page PDF ne peut pas être ouverte.");return result;}
    const qreal width=api.pageWidth(page),height=api.pageHeight(page);
    qreal aspect=width/height;
    if(rotation==90||rotation==270)aspect=1./aspect;
    if(!std::isfinite(width)||!std::isfinite(height)||width<=0||height<=0||!std::isfinite(aspect)||aspect<=0){
        api.closePage(page);result.error=QStringLiteral("Les dimensions de cette page PDF sont invalides.");return result;
    }
    if(gate->generation!=generation){api.closePage(page);return result;}
    QImage image(renderSize(aspect,desiredWidth),QImage::Format_ARGB32);
    if(image.isNull()){
        api.closePage(page);result.error=QStringLiteral("Mémoire insuffisante pour afficher cette page PDF.");return result;
    }
    image.fill(Qt::white);
    auto bitmap=api.createBitmap(image.width(),image.height(),FPDFBitmap_BGRA,image.bits(),image.bytesPerLine());
    if(!bitmap){api.closePage(page);result.error=QStringLiteral("Impossible d’afficher cette page PDF.");return result;}
    // Only page contents and static annotations are rendered. No form-fill,
    // JavaScript, document-open or page-action APIs are initialized or invoked.
    api.render(bitmap,page,0,0,image.width(),image.height(),rotation/90,FPDF_ANNOT);
    api.destroyBitmap(bitmap);api.closePage(page);
    result.document=std::move(document);result.image=std::move(image);
    result.pageNumber=number;result.pageCount=count;result.aspect=aspect;
    return result;
}
}

PdfView::PdfView(QQuickItem *parent):QQuickPaintedItem(parent),m_gate(std::make_shared<PdfRenderGate>()) {
    setOpaquePainting(true);setTextureSize(QSize(1,1));
    m_resizeTimer.setSingleShot(true);m_resizeTimer.setInterval(80);
    connect(&m_resizeTimer,&QTimer::timeout,this,&PdfView::requestRender);
    connect(this,&QQuickItem::widthChanged,this,&PdfView::scheduleResize);
}

PdfView::~PdfView() { ++m_gate->generation;m_resizeTimer.stop(); }

void PdfView::setBusy(bool value) { if(m_busy!=value){m_busy=value;emit busyChanged();} }
void PdfView::setError(const QString &value) { if(m_error!=value){m_error=value;emit errorChanged();} }

void PdfView::clearState() {
    ++m_gate->generation;m_resizeTimer.stop();m_document.reset();
    m_source.clear();m_title.clear();m_image={};m_pageNumber=m_pageCount=m_rotation=0;m_aspectRatio=1.;
    setTextureSize(QSize(1,1));
    setError({});setBusy(false);emit documentChanged();emit viewChanged();update();
}

bool PdfView::openFile(QString path,QString displayName) {
    clearState();
    if(QUrl(path).isLocalFile())path=QUrl(path).toLocalFile();
    QFileInfo info(path);
    if(path.isEmpty()||!info.isFile()){
        setError(QStringLiteral("Ce fichier PDF est introuvable ou n’est pas un fichier."));return false;
    }
    if(!info.isReadable()){setError(QStringLiteral("Impossible de lire ce fichier PDF."));return false;}
    m_source=info.canonicalFilePath();
    if(m_source.isEmpty())m_source=info.absoluteFilePath(); // Let the worker report a file removed after the check.
    m_title=displayName.trimmed().isEmpty()?info.completeBaseName():displayName;
    emit documentChanged();requestRender();return true;
}

void PdfView::closeDocument() { clearState(); }

void PdfView::setPageNumber(int number) {
    if(!m_document||number<1||number>m_pageCount||number==m_pageNumber)return;
    m_pageNumber=number;m_image={};emit viewChanged();update();requestRender();
}

void PdfView::rotateClockwise() {
    if(!m_document)return;
    m_rotation=(m_rotation+90)%360;m_aspectRatio=1./m_aspectRatio;m_image={};
    emit viewChanged();update();requestRender();
}

void PdfView::scheduleResize() {
    if(!m_document&&!m_busy)return;
    ++m_gate->generation;setBusy(true);m_resizeTimer.start();
}

void PdfView::requestRender() {
    if(m_source.isEmpty())return;
    m_resizeTimer.stop();const auto generation=++m_gate->generation;
    const auto document=m_document;
    const auto gate=m_gate;
    const auto path=m_source;const int page=m_pageNumber>0?m_pageNumber:1,rotation=m_rotation;
    const qreal ratio=window()?window()->devicePixelRatio():1.;
    const qreal requestedWidth=std::isfinite(width())&&width()>0?width()*ratio:800.;
    setBusy(true);setError({});
    auto *watcher=new QFutureWatcher<RenderResult>(this);
    connect(watcher,&QFutureWatcher<RenderResult>::finished,this,[this,watcher,generation]{
        const auto result=watcher->result();watcher->deleteLater();
        if(m_gate->generation!=generation)return;
        if(!result.error.isEmpty()){
            m_document.reset();m_image={};m_pageNumber=m_pageCount=m_rotation=0;m_aspectRatio=1.;setError(result.error);
        }else{
            m_document=result.document;m_image=result.image;m_pageNumber=result.pageNumber;
            m_pageCount=result.pageCount;m_aspectRatio=result.aspect;
        }
        // QQuickPaintedItem otherwise allocates a backing texture from the
        // item's logical size, which can be very tall for long PDF pages.
        setTextureSize(m_image.isNull()?QSize(1,1):m_image.size());
        emit documentChanged();emit viewChanged();update();setBusy(false);
    });
    watcher->setFuture(QtConcurrent::run([document,path,page,rotation,requestedWidth,gate,generation]{
        return renderPage(document,path,page,rotation,requestedWidth,gate,generation);
    }));
}

void PdfView::paint(QPainter *painter) {
    painter->fillRect(boundingRect(),Qt::white);
    if(m_image.isNull())return;
    QSizeF size=m_image.size();size.scale(boundingRect().size(),Qt::KeepAspectRatio);
    const QRectF target((width()-size.width())/2.,(height()-size.height())/2.,size.width(),size.height());
    painter->setRenderHint(QPainter::SmoothPixmapTransform,true);painter->drawImage(target,m_image);
}
