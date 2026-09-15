#include "StencilCatalogue.h"
#include "BridgeClient.h"
#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QPainter>
#include <QSettings>

using namespace PaperDrawing;

StencilCatalogue::StencilCatalogue(QObject *parent) : QObject(parent), m_symbols(electronicsCatalogue()),m_bridge(new repaper::BridgeClient(this)) {
    QSettings settings;
    const QStringList favorites = settings.value("favorites").toStringList();
    for (const auto &id : favorites) m_favorites.insert(id);
    m_selectedId = m_symbols.first().id;
    connect(m_bridge,&repaper::BridgeClient::imported,this,[this](const QString &id){m_status="Traits importés dans la bibliothèque native : "+id;emit statusChanged();});
    connect(m_bridge,&repaper::BridgeClient::failed,this,[this](const QString &,const QString &message){m_status=message;emit statusChanged();});
    connect(m_bridge,&repaper::BridgeClient::changed,this,[this]{if(m_bridge->busy()){m_status=m_bridge->message();emit statusChanged();}});
}
QVariantList StencilCatalogue::symbols() const {
    QVariantList result;
    for (const auto &symbol : m_symbols) {
        if (!m_category.isEmpty() && symbol.category != m_category) continue;
        if (m_favoritesOnly && !m_favorites.contains(symbol.id)) continue;
        if (!m_query.trimmed().isEmpty() &&
            !(symbol.name+" "+symbol.id+" "+symbol.standard).contains(m_query.trimmed(), Qt::CaseInsensitive)) continue;
        result.append(QVariantMap{{"symbolId",symbol.id},{"name",symbol.name},
            {"category",symbol.category},{"standard",symbol.standard},{"favorite",m_favorites.contains(symbol.id)}});
    }
    return result;
}
QVariantMap StencilCatalogue::selected() const {
    for (const auto &symbol : m_symbols) if (symbol.id == m_selectedId)
        return {{"symbolId",symbol.id},{"name",symbol.name},{"category",symbol.category},
                {"standard",symbol.standard},{"favorite",m_favorites.contains(symbol.id)},
                {"strokeCount",symbol.strokes.size()},{"anchorCount",symbol.anchors.size()}};
    return {};
}
QString StencilCatalogue::outputDirectory() const { return exportDirectory("restencil"); }
bool StencilCatalogue::emulatorMode() const { return QCoreApplication::arguments().contains("--emulator"); }
QString StencilCatalogue::insertionStatus() const {
    if(emulatorMode())return "Page d’essai de l’émulateur : insertion de vrais traits structurés, déplaçables et redimensionnables. L’intégration Xochitl sur tablette reste à valider.";
    return "Insertion dans la page active indisponible : aucune extension Xochitl validée pour ce firmware. "
           "Les exports de traits restent disponibles. Aucun document ouvert n’est modifié.";
}
void StencilCatalogue::setQuery(const QString &value) { if (m_query!=value) {m_query=value;emit symbolsChanged();} }
void StencilCatalogue::setCategory(const QString &value) { if (m_category!=value) {m_category=value;emit symbolsChanged();} }
void StencilCatalogue::setFavoritesOnly(bool value) { if(m_favoritesOnly!=value){m_favoritesOnly=value;emit symbolsChanged();} }
void StencilCatalogue::setSelectedId(const QString &value) {
    for (const auto &symbol : m_symbols) if (symbol.id == value && m_selectedId != value) {
        m_selectedId=value;emit selectedChanged();return;
    }
}
void StencilCatalogue::toggleFavorite(const QString &id) {
    bool known=false;
    for (const auto &symbol : m_symbols) if (symbol.id==id) {known=true;break;}
    if (!known) return;
    if (m_favorites.contains(id)) m_favorites.remove(id); else m_favorites.insert(id);
    QStringList values;
    for (const auto &value : m_favorites) values.append(value);
    QSettings().setValue("favorites",values);
    emit symbolsChanged();emit selectedChanged();
}
void StencilCatalogue::writeExport(const QVector<Stroke> &strokes, const QString &name, const QString &format) {
    if (format!="svg" && format!="pdf" && format!="scene") return;
    const QString suffix=format=="scene"?"paper-scene.json":format;
    const QString path=outputDirectory()+"/"+name+"-"+QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz")+"."+suffix;
    QString error;
    bool success = format=="svg" ? exportSvg(path,strokes,&error) :
        format=="pdf" ? exportPdf(path,strokes,name,&error) : exportScene(path,strokes,name,&error);
    m_status=success?"Export créé : "+path:"Échec de l’export : "+error;
    emit statusChanged();
}
void StencilCatalogue::exportSelected(const QString &format) {
    for (const auto &symbol:m_symbols) if(symbol.id==m_selectedId) {
        writeExport(transformStrokes(symbol.strokes,{558,840},2.4),symbol.id,format);return;
    }
}
void StencilCatalogue::exportPack(const QString &format) {
    QVector<Stroke> strokes;
    int index=0;
    for(const auto &symbol:m_symbols) {
        const QPointF origin(110+(index%4)*320,160+(index/4)*320);
        strokes+=transformStrokes(symbol.strokes,origin,1.8);
        ++index;
    }
    writeExport(strokes,"reStencil-electronique-1.0",format);
}
void StencilCatalogue::importNative(bool entirePack) {
    if(m_bridge->busy())return;
    QVector<Stroke> strokes;int index=0;
    QString title="reStencil — "+selected().value("name").toString();
    if(entirePack) {
        title="reStencil — Électronique IEC et ANSI — 1.0";
        for(const auto &symbol:m_symbols) {
            strokes+=transformStrokes(symbol.strokes,{qreal(110+(index%4)*320),qreal(160+(index/4)*320)},1.8);++index;
        }
    } else for(const auto &symbol:m_symbols)if(symbol.id==m_selectedId)strokes=transformStrokes(symbol.strokes,{558,840},2.4);
    const QString path=outputDirectory()+"/latest.paper-scene.json";
    QString error;
    if(!exportScene(path,strokes,title,&error)){m_status=error;emit statusChanged();return;}
    QFile file(path);if(!file.open(QIODevice::ReadOnly)){m_status=file.errorString();emit statusChanged();return;}
    const QString key="restencil-"+QString::fromLatin1(QCryptographicHash::hash(file.readAll(),QCryptographicHash::Sha256).toHex());
    m_bridge->importFile(path,title,key);
}

SymbolPreview::SymbolPreview(QQuickItem *parent):QQuickPaintedItem(parent) {setAntialiasing(true);}
void SymbolPreview::setSymbolId(const QString &id) {if(m_symbolId!=id){m_symbolId=id;update();emit symbolIdChanged();}}
void SymbolPreview::setShowAnchors(bool value) {if(m_showAnchors!=value){m_showAnchors=value;update();emit showAnchorsChanged();}}
void SymbolPreview::paint(QPainter *painter) {
    painter->setRenderHint(QPainter::Antialiasing);
    for (const auto &symbol:electronicsCatalogue()) if(symbol.id==m_symbolId) {
        const qreal scale=std::max(qreal(0),std::min((width()-24)/140,(height()-24)/100));
        painter->translate(width()/2-60*scale,height()/2-40*scale);
        painter->scale(scale,scale);
        paintStrokes(*painter,symbol.strokes);
        if(m_showAnchors) {
            painter->setPen(QPen(Qt::darkGray,1,Qt::DashLine));painter->setBrush(Qt::NoBrush);
            for(auto p:symbol.anchors)painter->drawEllipse(p,5,5);
        }
        return;
    }
}
