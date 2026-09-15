#include "NativeObjectBindings.h"
#include "NativeStrokeSampling.h"
#include "NativeArrowStroke.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>

namespace RePaperNative {
namespace {
constexpr int MaxObjects = 3000, MaxObjectLines = 128, MaxPorts = 128;
constexpr qsizetype MaxObjectPoints = 200000, MaxStoredPoints = 2000000;
constexpr qsizetype MaxFileBytes = 32 * 1024 * 1024;
constexpr quint32 MaxReferenceBytes = 4 + MaxObjectLines * 4 + MaxObjectPoints * 8;
constexpr qreal GeometryTolerance = .2;
bool bounded(QPointF point) {
    return std::isfinite(point.x()) && std::isfinite(point.y())
        && std::abs(point.x()) <= 1000000 && std::abs(point.y()) <= 1000000;
}
bool identifier(const QString &text) {
    if (text.isEmpty() || text.size() > 512) return false;
    for (auto c : text) if (c.isNull() || c.category() == QChar::Other_Control) return false;
    return true;
}
QString hexId(quint64 id) { return QStringLiteral("%1").arg(id, 16, 16, QLatin1Char('0')); }
quint64 readId(const QJsonValue &value) {
    if (!value.isString() || value.toString().size() != 16) return 0;
    bool ok = false;
    const quint64 id = value.toString().toULongLong(&ok, 16);
    return ok ? id : 0;
}
qreal distanceSquared(QPointF point) { return QPointF::dotProduct(point, point); }
bool close(QPointF a, QPointF b) {
    return std::abs(a.x() - b.x()) <= GeometryTolerance
        && std::abs(a.y() - b.y()) <= GeometryTolerance;
}
bool translatedGeometryMatches(const NativeObjectLine &before, const NativeObjectLine &after, QPointF offset) {
    if (before.tool != after.tool || before.stroke.points.size() != after.stroke.points.size()
        || before.stroke.color != after.stroke.color || !std::isfinite(before.stroke.width)
        || !std::isfinite(after.stroke.width) || std::abs(before.stroke.width - after.stroke.width) > .001) return false;
    for (qsizetype i = 0; i < before.stroke.points.size(); ++i)
        if (!bounded(after.stroke.points[i]) || !close(before.stroke.points[i] + offset, after.stroke.points[i])) return false;
    return true;
}
bool snapshotIndex(const NativeObjectSnapshot &snapshot,
                   QHash<quint64, const NativeObjectLine*> *byId,
                   QHash<quint64, QVector<const NativeObjectLine*>> *byLineage = nullptr) {
    if (!snapshot.complete || !snapshot.pendingEditIdentity || !identifier(snapshot.documentId)
        || !identifier(snapshot.pageId) || snapshot.layer < 0 || !snapshot.layerId
        || snapshot.lines.size() > 100000 || snapshot.fingerprint.isEmpty()) return false;
    for (const auto &line : snapshot.lines) {
        if (!line.id || !line.parentId || !line.lineageId || byId->contains(line.id)) return false;
        byId->insert(line.id, &line);
        if (byLineage) (*byLineage)[line.lineageId].append(&line);
    }
    return true;
}
bool commonTransform(const QVector<PaperDrawing::Polyline> &references,
                     const QVector<const NativeObjectLine*> &current, QTransform *result) {
    if (references.isEmpty() || references.size() != current.size()) return false;
    qsizetype total = 0;
    QPointF p0, p1, p2, q0, q1, q2;
    bool first = true;
    qreal furthest = 0;
    for (qsizetype i = 0; i < references.size(); ++i) {
        const auto &a = references[i], &b = current[i]->stroke.points;
        if (a.size() < 2 || a.size() != b.size() || a.size() > MaxObjectPoints - total) return false;
        total += a.size();
        for (qsizetype j = 0; j < a.size(); ++j) {
            if (!bounded(a[j]) || !bounded(b[j])) return false;
            if (first) { p0 = a[j]; q0 = b[j]; first = false; }
            const qreal distance = distanceSquared(a[j] - p0);
            if (distance > furthest) { furthest = distance; p1 = a[j]; q1 = b[j]; }
        }
    }
    if (furthest <= 1e-12) {
        result->reset(); result->translate(q0.x() - p0.x(), q0.y() - p0.y());
    } else {
        const auto u = p1 - p0, uq = q1 - q0;
        // Float-rounded samples of a straight diagonal are not perfectly
        // collinear. Prefer a similarity map if every known sample agrees;
        // fitting their tiny perpendicular noise would invent a large scale.
        const qreal similarityA = QPointF::dotProduct(uq, u) / furthest;
        const qreal similarityB = (uq.y() * u.x() - uq.x() * u.y()) / furthest;
        const QTransform similarity(similarityA, similarityB, -similarityB, similarityA,
            q0.x() - similarityA * p0.x() + similarityB * p0.y(),
            q0.y() - similarityB * p0.x() - similarityA * p0.y());
        bool similarityMatches = std::isfinite(similarityA) && std::isfinite(similarityB)
            && similarityA * similarityA + similarityB * similarityB >= 1e-12;
        for (qsizetype i = 0; similarityMatches && i < references.size(); ++i)
            for (qsizetype j = 0; j < references[i].size(); ++j)
                if (!close(similarity.map(references[i][j]), current[i]->stroke.points[j])) {
                    similarityMatches = false; break;
                }
        if (similarityMatches) { *result = similarity; return true; }
        qreal largestCross = 0;
        for (qsizetype i = 0; i < references.size(); ++i)
            for (qsizetype j = 0; j < references[i].size(); ++j) {
                const auto v = references[i][j] - p0;
                const qreal cross = std::abs(u.x() * v.y() - u.y() * v.x());
                if (cross > largestCross) {
                    largestCross = cross; p2 = references[i][j]; q2 = current[i]->stroke.points[j];
                }
            }
        qreal a = 0, b = 0, c = 0, d = 0;
        if (largestCross > furthest * 1e-8) {
            const auto v = p2 - p0, vq = q2 - q0;
            const qreal det = u.x() * v.y() - u.y() * v.x();
            a = (uq.x() * v.y() - vq.x() * u.y()) / det;
            b = (uq.y() * v.y() - vq.y() * u.y()) / det;
            c = (vq.x() * u.x() - uq.x() * v.x()) / det;
            d = (vq.y() * u.x() - uq.y() * v.x()) / det;
        } else {
            // A straight wire has no perpendicular extent. Its endpoints and
            // path lie on this axis; a similarity map is sufficient for them.
            a = QPointF::dotProduct(uq, u) / furthest;
            b = (uq.y() * u.x() - uq.x() * u.y()) / furthest;
            c = -b; d = a;
        }
        if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(c) || !std::isfinite(d)
            || std::abs(a * d - b * c) < 1e-12) return false;
        *result = QTransform(a, b, c, d, q0.x() - a * p0.x() - c * p0.y(),
                             q0.y() - b * p0.x() - d * p0.y());
    }
    for (qsizetype i = 0; i < references.size(); ++i)
        for (qsizetype j = 0; j < references[i].size(); ++j)
            if (!close(result->map(references[i][j]), current[i]->stroke.points[j])) return false;
    return true;
}
QJsonArray pointArray(const PaperDrawing::Polyline &points) {
    QJsonArray result;
    for (auto point : points) result.append(QJsonArray{point.x(), point.y()});
    return result;
}
bool readPoints(const QJsonValue &value, qsizetype limit, PaperDrawing::Polyline *points) {
    if (!value.isArray() || value.toArray().size() > limit) return false;
    for (const auto &entry : value.toArray()) {
        if (!entry.isArray()) return false;
        const auto pair = entry.toArray();
        if (pair.size() != 2 || !pair[0].isDouble() || !pair[1].isDouble()) return false;
        const QPointF point(pair[0].toDouble(), pair[1].toDouble());
        if (!bounded(point)) return false;
        points->append(point);
    }
    return true;
}
QJsonObject encodeModel(const repaper::drawing::Item &item) {
    const auto attachment=[](const repaper::drawing::Attachment &ref){
        return QJsonObject{{"objectId",ref.objectId},{"portId",ref.portId},{"wirePosition",ref.wirePosition}};
    };
    QJsonObject result{{"points",pointArray(item.sourcePoints)},{"style",item.style},{"width",item.width},
        {"headSize",item.headSize},{"arrowDirection",item.arrowDirection},{"patternScale",item.patternScale},
        {"cornerRadius",item.cornerRadius},{"wireBend",item.wireBend},{"horizontalFirst",item.horizontalFirst},
        {"wireAxis",pointArray({item.wireAxis})},{"wireHasBend",item.wireHasBend},{"symbolId",item.symbolId},
        {"wireRoute",pointArray(item.wireRoute)},{"wireRouteMode",item.wireRouteMode},
        {"startAttachment",attachment(item.startAttachment)},{"endAttachment",attachment(item.endAttachment)}};
    if(!item.stencilParameters.isEmpty())result.insert("stencilParameters",QJsonObject::fromVariantMap(item.stencilParameters));
    if(item.kind=="symbol") {
        result.insert("voltageArrow",item.voltageArrow);
        result.insert("voltageArrowReversed",item.voltageArrowReversed);
        result.insert("voltageArrowOtherSide",item.voltageArrowOtherSide);
    }
    return result;
}
bool decodeModel(const QJsonObject &json, repaper::drawing::Item &item) {
    const auto attachment=[&](const char *name,repaper::drawing::Attachment &ref){
        if (!json.contains(name)) return true;
        if (!json.value(name).isObject()) return false;
        const auto value=json.value(name).toObject();
        if (!value.value("objectId").isString() || !value.value("portId").isString()) return false;
        ref.objectId=value.value("objectId").toString();ref.portId=value.value("portId").toString();
        if (value.contains("wirePosition")) {
            if (!value.value("wirePosition").isDouble()) return false;
            ref.wirePosition=value.value("wirePosition").toDouble();
        }
        if (ref.empty()) return ref.portId.isEmpty() && ref.wirePosition==-1;
        if (!identifier(ref.objectId) || !identifier(ref.portId) || ref.objectId==item.id) return false;
        return ref.portId=="route" ? std::isfinite(ref.wirePosition) && ref.wirePosition>=0 && ref.wirePosition<=1
                                   : ref.wirePosition==-1;
    };
    if(!readPoints(json.value("points"),20000,&item.sourcePoints))return false;
    item.style=json.value("style").toString();item.arrowDirection=json.value("arrowDirection").toString();
    item.symbolId=json.value("symbolId").toString();item.horizontalFirst=json.value("horizontalFirst").toBool();
    for(const auto *key:{"voltageArrow","voltageArrowReversed","voltageArrowOtherSide"})
        if(json.contains(key)&&!json.value(key).isBool())return false;
    item.voltageArrow=json.value("voltageArrow").toBool();
    item.voltageArrowReversed=json.value("voltageArrowReversed").toBool();
    item.voltageArrowOtherSide=json.value("voltageArrowOtherSide").toBool();
    if(json.contains("stencilParameters")) {
        if(!json.value("stencilParameters").isObject())return false;
        item.stencilParameters=json.value("stencilParameters").toObject().toVariantMap();
        if((item.kind!="symbol"||!PaperDrawing::isConfigurableStencil(item.symbolId))&&!item.stencilParameters.isEmpty())return false;
    }
    item.wireHasBend=json.value("wireHasBend").toBool();
    for(const auto *name:{"width","headSize","patternScale","cornerRadius","wireBend"})if(!json.value(name).isDouble())return false;
    item.width=json.value("width").toDouble();item.headSize=json.value("headSize").toDouble();
    item.patternScale=json.value("patternScale").toDouble();item.cornerRadius=json.value("cornerRadius").toDouble();
    item.wireBend=json.value("wireBend").toDouble();
    if (json.contains("wireRoute") && !readPoints(json.value("wireRoute"),1024,&item.wireRoute)) return false;
    item.wireRouteMode=json.contains("wireRouteMode") ? json.value("wireRouteMode").toString()
        : (item.wireHasBend || std::abs(item.wireBend)>1e-9 ? QStringLiteral("legacy") : QStringLiteral("auto"));
    if (!QStringList{"auto","manual","legacy"}.contains(item.wireRouteMode)
        || !attachment("startAttachment",item.startAttachment) || !attachment("endAttachment",item.endAttachment)) return false;
    if (item.kind!="wire" && (!item.startAttachment.empty() || !item.endAttachment.empty())) return false;
    if (item.kind=="wire" && !item.wireRoute.isEmpty() && item.wireRouteMode!="legacy"
        && (item.sourcePoints.size()!=2 || item.wireRoute.size()<2
            || QLineF(item.wireRoute.first(),item.sourcePoints.first()).length()>1e-6
            || QLineF(item.wireRoute.last(),item.sourcePoints.last()).length()>1e-6)) return false;
    PaperDrawing::Polyline axis;
    if(!readPoints(json.value("wireAxis"),1,&axis)||axis.size()!=1)return false;
    item.wireAxis=axis.first();
    if(!QStringList{"line","arrow","wire","rectangle","ellipse","symbol"}.contains(item.kind)
        ||!QStringList{"start","end","both","none"}.contains(item.arrowDirection))return false;
    return rebuildNativeObject(item)&&item.strokes.size()<=MaxObjectLines;
}
bool equivalentPath(const PaperDrawing::Polyline &a,const PaperDrawing::Polyline &b){
    if(a.size()<2||b.size()<2||a.size()>MaxObjectPoints||b.size()>MaxObjectPoints)return false;
    const auto lengths=[](const auto &path){
        QVector<qreal> result;result.reserve(path.size());result.append(0);
        for(qsizetype i=1;i<path.size();++i)result.append(result.last()+QLineF(path[i-1],path[i]).length());
        return result;
    };
    const auto al=lengths(a),bl=lengths(b);const auto length=al.last();
    if(!std::isfinite(length)||!std::isfinite(bl.last())||std::abs(length-bl.last())>GeometryTolerance)return false;
    const auto matches=[](const auto &source,const auto &sourceLengths,const auto &target,const auto &targetLengths){
        if(sourceLengths.last()<1e-9)return std::all_of(source.cbegin(),source.cend(),[&](auto point){return close(point,target.first());});
        qsizetype segment=1;
        for(qsizetype i=0;i<source.size();++i){
            const qreal distance=sourceLengths[i]/sourceLengths.last()*targetLengths.last();
            while(segment<target.size()-1&&targetLengths[segment]<distance)++segment;
            const auto span=targetLengths[segment]-targetLengths[segment-1];
            const auto point=span>1e-9?target[segment-1]+(target[segment]-target[segment-1])*((distance-targetLengths[segment-1])/span):target[segment];
            if(!close(source[i],point))return false;
        }
        return true;
    };
    return matches(a,al,b,bl)&&matches(b,bl,a,al);
}
bool modelMatches(const repaper::drawing::Item &item,const QVector<const NativeObjectLine*> &lines) {
    if(item.strokes.size()!=lines.size())return false;
    qsizetype remaining=MaxObjectPoints;
    for(qsizetype i=0;i<lines.size();++i){
        if(lines[i]->tool!=19||!lines[i]->uniformPointWidth)return false;
        remaining-=lines[i]->stroke.points.size();
        if(remaining<0||!equivalentPath(item.strokes[i].points,lines[i]->stroke.points))return false;
        if(lines[i]->stroke.color!=item.strokes[i].color||!std::isfinite(lines[i]->stroke.width)
            ||!std::isfinite(item.strokes[i].width)||std::abs(lines[i]->stroke.width-item.strokes[i].width)>.126)return false;
    }
    return true;
}
bool transformObservedModel(repaper::drawing::Item &item, const QTransform &observed) {
    if (!repaper::drawing::hasBox(item)) return repaper::drawing::transform(item, observed);
    if (item.sourcePoints.size() != 3) return false;
    const auto origin = item.sourcePoints[0];
    const auto oldX = item.sourcePoints[1] - origin, oldY = item.sourcePoints[2] - origin;
    const auto mappedOrigin = observed.map(origin);
    const auto x = observed.map(item.sourcePoints[1]) - mappedOrigin;
    const auto y = observed.map(item.sourcePoints[2]) - mappedOrigin;
    const qreal width = std::hypot(x.x(), x.y()), height = std::hypot(y.x(), y.y());
    if (!std::isfinite(width) || !std::isfinite(height) || width < 1 || height < 1) return false;
    auto perpendicular = QPointF(-x.y(), x.x()) * (height / width);
    if (QPointF::dotProduct(perpendicular, y) < 0) perpendicular = -perpendicular;
    // Native coordinates are float32. A pure rotation can therefore produce a
    // slightly sheared fitted basis. Repair only sub-tolerance rounding here;
    // actual ownership and the complete regenerated rendering are still checked.
    if (QLineF(y, perpendicular).length() > GeometryTolerance) return false;
    const auto correctedOrigin = mappedOrigin + (y - perpendicular) * .5;
    bool invertible = false;
    const QTransform source(oldX.x(), oldX.y(), oldY.x(), oldY.y(), origin.x(), origin.y());
    const auto inverse = source.inverted(&invertible);
    if (!invertible) return false;
    const QTransform target(x.x(), x.y(), perpendicular.x(), perpendicular.y(), correctedOrigin.x(), correctedOrigin.y());
    return repaper::drawing::transform(item, inverse * target);
}
bool recoverLegacyModel(const QString &id,const QString &kind,const QVector<PaperDrawing::Polyline> &references,
        const QVector<QPointF> &ports,const QStringList &portIds,const QTransform &transform,
        const QVector<const NativeObjectLine*> &lines,repaper::drawing::Item *result){
    if(lines.isEmpty())return false;
    repaper::drawing::Item base;base.id=id;base.kind=kind;base.width=lines.first()->stroke.width;
    base.strokes={PaperDrawing::Stroke{{},base.width,lines.first()->stroke.color}};
    const auto accept=[&](repaper::drawing::Item candidate){
        if(!repaper::drawing::rebuild(candidate)||!transformObservedModel(candidate,transform))return false;
        candidate.strokes=wholeNativeArrowStrokes(candidate);
        if(!modelMatches(candidate,lines))return false;
        *result=std::move(candidate);return true;
    };
    if(QStringList{"line","arrow","wire"}.contains(kind)){
        const auto start=portIds.indexOf("start"),end=portIds.indexOf("end");
        if(start<0||end<0)return false;
        base.sourcePoints={ports[start],ports[end]};
    }else if(kind=="rectangle"||kind=="ellipse"){
        const int north=portIds.indexOf("north"),east=portIds.indexOf("east"),south=portIds.indexOf("south"),west=portIds.indexOf("west");
        if(north<0||east<0||south<0||west<0)return false;
        const auto center=(ports[north]+ports[south])*.5;
        base.sourcePoints={ports[north]+ports[west]-center,ports[north]+ports[east]-center,ports[south]+ports[west]-center};
    }else if(kind=="symbol"){
        // 0.6 recorded exact members and their birth geometry, but omitted the
        // symbol ID. Recover only a catalogue model whose complete rendering
        // reproduces those same attributed members; never discover a group.
        QVector<PaperDrawing::Stroke> birth;
        for(const auto &path:references)birth.append({path,0,Qt::black});
        const auto actual=PaperDrawing::bounds(birth);
        if(actual.width()<.01||actual.height()<.01)return false;
        for(const auto &symbol:PaperDrawing::electronicsCatalogue()){
            if(symbol.strokes.size()!=references.size())continue;
            const auto nominal=PaperDrawing::bounds(symbol.strokes);
            if(nominal.width()<.01||nominal.height()<.01)continue;
            const qreal sx=actual.width()/nominal.width(),sy=actual.height()/nominal.height();
            const auto origin=actual.topLeft()-QPointF(nominal.left()*sx,nominal.top()*sy);
            auto candidate=base;candidate.symbolId=symbol.id;
            candidate.sourcePoints={origin,origin+QPointF(120*sx,0),origin+QPointF(0,80*sy)};
            if(accept(candidate))return true;
        }
        return false;
    }else return false;
    for(const auto &style:{QString("solid"),QString("dashed"),QString("dotted")}){
        base.style=style;
        if(kind=="arrow"){
            for(const auto &direction:{QString("end"),QString("start"),QString("both"),QString("none")}){
                auto candidate=base;candidate.arrowDirection=direction;if(accept(candidate))return true;
            }
        }else if(kind=="wire"){
            base.horizontalFirst=true;if(accept(base))return true;
            base.horizontalFirst=false;if(accept(base))return true;
        }else if(accept(base))return true;
    }
    return false;
}
}

QVector<quint64> selectedNativeLineages(const NativeObjectSnapshot &snapshot,
                                      const QVector<quint64> &ids, QString *error) {
    if (error) error->clear();
    QHash<quint64, const NativeObjectLine*> byId;
    const auto fail = [&](const char *message) {
        if (error) *error = QString::fromLatin1(message);
        return QVector<quint64>{};
    };
    if (!snapshotIndex(snapshot, &byId) || ids.isEmpty() || ids.size() > MaxObjectLines)
        return fail("The native selection snapshot is not ready.");
    QSet<quint64> seenIds, seenLineages;
    QVector<quint64> result;
    for (auto id : ids) {
        const auto *line = byId.value(id);
        if (!line || seenIds.contains(id) || seenLineages.contains(line->lineageId))
            return fail("The native selection contains missing or ambiguous lineages.");
        seenIds.insert(id); seenLineages.insert(line->lineageId); result.append(line->lineageId);
    }
    return result;
}

NativeSelectionExpansion resolveNativeLineages(const NativeObjectSnapshot &snapshot,
                                               const QVector<quint64> &lineages) {
    QHash<quint64, const NativeObjectLine*> byId;
    QHash<quint64, QVector<const NativeObjectLine*>> byLineage;
    if (!snapshotIndex(snapshot, &byId, &byLineage) || lineages.isEmpty() || lineages.size() > MaxObjectLines)
        return {false, {}, QStringLiteral("The native page snapshot is not ready.")};
    QSet<quint64> seen;
    QVector<quint64> ids;
    for (auto lineage : lineages) {
        const auto matches = byLineage.value(lineage);
        if (!lineage || seen.contains(lineage) || matches.size() != 1)
            return {false, {}, QStringLiteral("A selected native lineage is missing or ambiguous.")};
        seen.insert(lineage); ids.append(matches.first()->id);
    }
    return {true, ids, {}};
}

NativeObjectBindings::NativeObjectBindings(QString directory) : m_directory(std::move(directory)) {}
void NativeObjectBindings::invalidate() {
    m_ready = false; m_resolved.clear(); m_invalidManagedIds.clear(); m_snapshot = {};
    m_models.clear();m_resolvedIndex.clear();m_memberIndex.clear();m_availableIds.clear();m_invalidIds.clear();
    m_dependents.clear();m_connectionsReady=false;m_snapExcludedObject.clear();m_snapBlockedObjects.clear();
}
void NativeObjectBindings::clear() {
    invalidate(); m_document.clear(); m_page.clear(); m_layerId = 0;
    m_objects.clear(); m_fileDigest.clear(); m_storageBlocked = false; m_reason.clear();
}
QString NativeObjectBindings::storagePath() const {
    if (m_directory.isEmpty() || m_document.isEmpty() || m_page.isEmpty() || !m_layerId) return {};
    const auto key = QJsonDocument(QJsonArray{m_document, m_page, hexId(m_layerId)}).toJson(QJsonDocument::Compact);
    return QDir(m_directory).filePath(QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha256).toHex())
                                     + QStringLiteral(".objects.json"));
}
bool NativeObjectBindings::activate(const NativeObjectSnapshot &snapshot) {
    if (m_document == snapshot.documentId && m_page == snapshot.pageId && m_layerId == snapshot.layerId)
        return !m_storageBlocked;
    clear(); m_document = snapshot.documentId; m_page = snapshot.pageId; m_layerId = snapshot.layerId;
    if (m_directory.isEmpty()) { m_storageBlocked = true; m_reason = "Object binding storage is unavailable."; return false; }
    return load();
}
bool NativeObjectBindings::observe(const NativeObjectSnapshot &snapshot) {
    // Value snapshots share immutable Qt arrays. This shortcut is valid only
    // for the exact same line storage and selection, not merely an unchanged
    // page name or a producer-supplied fingerprint.
    if(m_ready&&snapshot.complete&&snapshot.pendingEditIdentity
        &&m_snapshot.lines.constData()==snapshot.lines.constData()&&m_snapshot.lines.size()==snapshot.lines.size()
        &&m_snapshot.documentId==snapshot.documentId&&m_snapshot.pageId==snapshot.pageId
        &&m_snapshot.layer==snapshot.layer&&m_snapshot.layerId==snapshot.layerId
        &&m_snapshot.sceneIdentity==snapshot.sceneIdentity&&m_snapshot.fingerprint==snapshot.fingerprint
        &&m_snapshot.nativeSelectionExact==snapshot.nativeSelectionExact&&m_snapshot.selectedIds==snapshot.selectedIds){
        m_snapshot=snapshot;m_snapshot.history={};return true;
    }
    invalidate();
    QHash<quint64, const NativeObjectLine*> byId;
    if (!snapshotIndex(snapshot, &byId)) {
        m_reason = snapshot.reason.isEmpty() ? QStringLiteral("The native page snapshot is incomplete.") : snapshot.reason;
        return false;
    }
    if (!activate(snapshot)) return false;
    return resolve(snapshot);
}
bool NativeObjectBindings::resolve(const NativeObjectSnapshot &snapshot) {
    m_resolved.clear(); m_invalidManagedIds.clear();
    QHash<quint64, const NativeObjectLine*> byId;
    QHash<quint64, QVector<const NativeObjectLine*>> byLineage;
    if (!snapshotIndex(snapshot, &byId, &byLineage)) return false;
    for (const auto &object : m_objects) {
        QVector<const NativeObjectLine*> lines;
        QVector<quint64> present;
        bool complete = true;
        for (auto lineage : object.lineages) {
            const auto matches = byLineage.value(lineage);
            if (matches.size() != 1) complete = false;
            for (const auto *line : matches) present.append(line->id);
            if (matches.size() == 1) lines.append(matches.first());
        }
        if (present.isEmpty()) continue; // Deleted objects can return through native Undo.
        QTransform transform;
        if (!complete || !commonTransform(object.references, lines, &transform)) {
            m_invalidManagedIds += present;
            continue;
        }
        ResolvedObject resolved;
        resolved.id = object.logicalId; resolved.kind = object.kind; resolved.portIds = object.portIds;
        resolved.placementAttachedEndpoints=object.placementAttachedEndpoints;
        for (const auto *line : lines) resolved.ids.append(line->id);
        for (auto point : object.ports) resolved.ports.append(transform.map(point));
        for (auto point : object.wirePath) resolved.wirePath.append(transform.map(point));
        if(object.hasModel){
            auto model=object.model; model.id=object.logicalId;
            const qsizetype foreground=repaper::drawing::isBackgroundStroke(model,0)?1:0;
            if(foreground>=lines.size()){m_invalidManagedIds+=present;continue;}
            // The configurable catalogue starts with a full-width foreground
            // stroke. Its preceding white mask has independent paint and width.
            model.width=lines[foreground]->stroke.width;
            repaper::drawing::setForegroundColor(model,lines[foreground]->stroke.color);
            // Only expose semantic controls after regeneration reproduces all
            // actual native members. Nonparametric or independently edited ink
            // retains the ordinary exact-ID selection path.
            if(transformObservedModel(model,transform))model.strokes=wholeNativeArrowStrokes(model);
            else model.strokes.clear();
            if(modelMatches(model,lines)){
                resolved.model=std::move(model);resolved.hasModel=true;
            }
        }else if(std::any_of(present.cbegin(),present.cend(),[&](auto id){return snapshot.selectedIds.contains(id);})){ 
            resolved.hasModel=recoverLegacyModel(object.logicalId,object.kind,object.references,object.ports,object.portIds,transform,lines,&resolved.model);
        }
        const bool validPoints = std::all_of(resolved.ports.cbegin(), resolved.ports.cend(), bounded)
            && std::all_of(resolved.wirePath.cbegin(), resolved.wirePath.cend(), bounded);
        if (!validPoints) { m_invalidManagedIds += present; continue; }
        m_resolved.append(std::move(resolved));
    }
    // A history revision is resolved by its explicit native lineages. If two
    // revisions of one logical object are active, neither can own connections.
    QHash<QString, int> activeRevisions;
    for (const auto &object : m_resolved) ++activeRevisions[object.id];
    for (qsizetype i=m_resolved.size(); i-- > 0;) if (activeRevisions.value(m_resolved[i].id)>1) {
        m_invalidManagedIds += m_resolved[i].ids; m_resolved.removeAt(i);
    }
    m_snapshot = snapshot; m_snapshot.history={};m_ready = true; m_reason.clear();
    rebuildIndexes();
    return true;
}
void NativeObjectBindings::rebuildIndexes(){
    m_models.clear();m_resolvedIndex.clear();m_memberIndex.clear();m_availableIds.clear();m_invalidIds.clear();
    m_dependents.clear();m_snapExcludedObject.clear();m_snapBlockedObjects.clear();
    for(qsizetype i=0;i<m_resolved.size();++i){
        const auto &object=m_resolved[i];m_resolvedIndex.insert(object.id,i);
        for(auto id:object.ids)m_memberIndex.insert(id,i);
    }
    QSet<quint64> active;for(const auto &line:std::as_const(m_snapshot.lines)){active.insert(line.lineageId);m_availableIds.insert(line.id);}
    for(auto id:m_invalidManagedIds)m_invalidIds.insert(id);
    QSet<QString> knownActive,validated;
    for(const auto &object:m_objects)
        if(std::any_of(object.lineages.cbegin(),object.lineages.cend(),[&](auto id){return active.contains(id);}))knownActive.insert(object.logicalId);
    for(const auto &object:m_resolved)if(object.hasModel){validated.insert(object.id);m_models.append(activeModel(object));}
    m_connectionsReady=true;
    for(const auto &object:m_resolved)if(object.hasModel&&object.kind=="wire")
        for(const auto &ref:{object.model.startAttachment,object.model.endAttachment})
            if(!ref.empty()&&knownActive.contains(ref.objectId)&&!validated.contains(ref.objectId))m_connectionsReady=false;
    for(const auto &model:m_models)if(model.kind=="wire")
        for(const auto &ref:{model.startAttachment,model.endAttachment})
            if(!ref.empty())m_dependents[ref.objectId].append(model.id);
}

bool NativeObjectBindings::registerInserted(const repaper::drawing::Item &item,
                                           const NativeObjectSnapshot &snapshot,
                                           const QVector<quint64> &insertedIds,
                                           const repaper::drawing::StencilPlacementResult *placement) {
    if (!observe(snapshot)) return false;
    for (const auto &existing : m_objects) if (existing.logicalId == item.id) {
        m_reason = "The generated object identifier was already registered."; return false;
    }
    BoundObject object;
    if (!insertedBinding(item, snapshot, insertedIds, m_objects, &object)) return false;
    auto objects = m_objects; objects.append(std::move(object));
    if(placement) {
        auto models=documentModels();models.append(item);
        if(repaper::drawing::attachCoincidentWireEndpoints(models,item,placement)>0) {
            const auto changed=std::find_if(models.cbegin(),models.cend(),[&](const auto &model){return model.id==placement->targetAttachment.objectId;});
            const auto active=std::find_if(m_resolved.cbegin(),m_resolved.cend(),[&](const auto &resolved){return resolved.id==placement->targetAttachment.objectId&&resolved.hasModel;});
            if(changed==models.cend()||active==m_resolved.cend())return false;
            QSet<quint64> activeLineages;
            for(const auto &line:snapshot.lines)if(active->ids.contains(line.id))activeLineages.insert(line.lineageId);
            int modified=0;
            for(auto &stored:objects)if(stored.logicalId==changed->id&&stored.hasModel&&stored.lineages.size()==activeLineages.size()&&
                std::all_of(stored.lineages.cbegin(),stored.lineages.cend(),[&](auto lineage){return activeLineages.contains(lineage);})) {
                // Mutate only the active binding's attachment metadata. Its
                // references, geometry and native lineages are unchanged.
                const auto endpoint=placement->targetAttachment.portId;
                if(endpoint=="start")stored.model.startAttachment=changed->startAttachment;
                else stored.model.endAttachment=changed->endAttachment;
                if(!stored.placementAttachedEndpoints.contains(endpoint))stored.placementAttachedEndpoints.append(endpoint);
                ++modified;
            }
            if(modified!=1){m_reason="The snapped wire's active binding is ambiguous.";return false;}
        }
    }
    if (!persist(objects)) return false;
    m_objects = std::move(objects);
    return resolve(snapshot);
}
bool NativeObjectBindings::insertedBinding(const repaper::drawing::Item &item,
                                           const NativeObjectSnapshot &snapshot,
                                           const QVector<quint64> &insertedIds,
                                           const QVector<BoundObject> &existingObjects,
                                           BoundObject *result) {
    const auto fail = [&](const char *message) { m_reason = QString::fromLatin1(message); return false; };
    if (!identifier(item.id) || item.strokes.isEmpty() || item.strokes.size() > MaxObjectLines
        || insertedIds.size() != item.strokes.size() || item.anchors.size() != item.portIds.size()
        || item.anchors.size() > MaxPorts || existingObjects.size() >= MaxObjects || !result)
        return fail("The generated object exceeds the binding contract.");
    QHash<quint64, const NativeObjectLine*> byId;
    if (!snapshotIndex(snapshot, &byId)) return false;
    QSet<quint64> owned;
    qsizetype storedPoints = 0;
    for (const auto &existing : existingObjects) {
        for (auto id : existing.lineages) owned.insert(id);
        for (const auto &path : existing.references) storedPoints += path.size();
    }
    BoundObject object;
    object.id = item.id; object.logicalId = item.id; object.kind = item.kind; object.portIds = item.portIds; object.ports = item.anchors;
    for (const auto &existing : existingObjects) if (existing.id == object.id) {
        object.id = QUuid::createUuid().toString(QUuid::WithoutBraces); break;
    }
    object.hasModel=repaper::drawing::hasEndpoints(item)||repaper::drawing::hasBox(item);
    if(object.hasModel)object.model=item;
    qsizetype remaining = MaxObjectPoints;
    QSet<quint64> inserted;
    QVector<const NativeObjectLine*> observed;
    for (qsizetype i = 0; i < insertedIds.size(); ++i) {
        const auto *line = byId.value(insertedIds[i]);
        if (!line || line->lineageId != line->id || inserted.contains(line->id) || owned.contains(line->lineageId))
            return fail("The insertion did not produce distinct new native lineages.");
        const auto sampled = sampleStrokeForNativeSelection(item.strokes[i].points, remaining);
        if (!sampled.valid() || sampled.points.size() != line->stroke.points.size())
            return fail("The inserted native object does not match its expected geometry.");
        for (qsizetype j = 0; j < sampled.points.size(); ++j)
            if (!bounded(line->stroke.points[j]) || !close(sampled.points[j], line->stroke.points[j]))
                return fail("The inserted native object does not match its expected geometry.");
        remaining -= sampled.points.size(); storedPoints += sampled.points.size();
        if (storedPoints > MaxStoredPoints) return fail("The object binding geometry limit was reached.");
        inserted.insert(line->id); object.lineages.append(line->lineageId);
        object.references.append(line->stroke.points);
        observed.append(line);
    }
    QSet<QString> ports;
    for (qsizetype i = 0; i < object.ports.size(); ++i) {
        if (!bounded(object.ports[i]) || !identifier(object.portIds[i]) || ports.contains(object.portIds[i]))
            return fail("The generated object contains invalid ports.");
        ports.insert(object.portIds[i]);
    }
    if (item.kind == "wire") {
        auto solid = item; solid.style = "solid";
        if (!repaper::drawing::rebuild(solid) || solid.strokes.size() != 1)
            return fail("The generated wire path is unavailable.");
        object.wirePath = solid.strokes.first().points;
    }
    if (object.hasModel && !modelMatches(item,observed))
        return fail("The inserted native object's paint or geometry differs from its model.");
    // The same bounded decoder validates in-memory attachment/route values
    // before persisting them; malformed references never poison a sidecar.
    if (object.hasModel) {
        repaper::drawing::Item validated;validated.id=item.id;validated.kind=item.kind;
        if (!decodeModel(encodeModel(item),validated)) return fail("The generated model contains invalid parameters or connections.");
    }
    *result = std::move(object); return true;
}

bool NativeObjectBindings::registerDuplicate(const NativeObjectSnapshot &before,
                                            const QVector<quint64> &beforeIds,
                                            const NativeObjectSnapshot &after,
                                            const QVector<quint64> &afterIds, QPointF offset) {
    const auto fail = [&](const char *message) { m_reason = QString::fromLatin1(message); return false; };
    if (!bounded(offset) || before.documentId != after.documentId || before.pageId != after.pageId
        || before.layerId != after.layerId || beforeIds.isEmpty() || beforeIds.size() > MaxObjectLines
        || beforeIds.size() != afterIds.size()) return fail("The duplicate receipt has an invalid scope or size.");
    QHash<quint64, const NativeObjectLine*> oldById, newById;
    if (!snapshotIndex(before, &oldById) || !snapshotIndex(after, &newById) || !observe(before)) return false;
    const auto expanded = expandSelection(beforeIds);
    if (!expanded.valid || expanded.ids.size() != beforeIds.size())
        return fail("The duplicate did not begin from complete managed objects.");
    const auto sources = m_resolved;
    const auto objectsBefore = m_objects;
    QSet<quint64> oldIds, newIds;
    for (auto id : beforeIds) {
        if (!oldById.contains(id) || oldIds.contains(id)) return fail("The duplicate source is ambiguous.");
        oldIds.insert(id);
        // Copy retains every original in place. A moved/replaced original is
        // not a valid duplicate receipt even if the new selection looks right.
        const auto *retained = newById.value(id);
        if (!retained || retained->lineageId != oldById.value(id)->lineageId
            || !translatedGeometryMatches(*oldById.value(id), *retained, {}))
            return fail("The duplicate source changed before confirmation.");
    }
    for (auto id : afterIds) {
        const auto *line = newById.value(id);
        if (!line || line->lineageId != id || oldById.contains(id) || newIds.contains(id))
            return fail("The duplicate did not create distinct new native lineages.");
        newIds.insert(id);
    }
    QVector<QVector<int>> candidates(beforeIds.size());
    for (qsizetype i = 0; i < beforeIds.size(); ++i)
        for (qsizetype j = 0; j < afterIds.size(); ++j)
            if (translatedGeometryMatches(*oldById.value(beforeIds[i]), *newById.value(afterIds[j]), offset))
                candidates[i].append(int(j));
    QVector<int> owner(afterIds.size(), -1);
    std::function<bool(int, QVector<bool>&)> assign = [&](int source, QVector<bool> &seen) {
        for (int target : candidates[source]) {
            if (seen[target]) continue;
            seen[target] = true;
            if (owner[target] < 0 || assign(owner[target], seen)) { owner[target] = source; return true; }
        }
        return false;
    };
    for (int i = 0; i < beforeIds.size(); ++i) {
        QVector<bool> seen(afterIds.size(), false);
        if (!assign(i, seen)) return fail("The duplicate geometry does not match the complete source selection.");
    }
    QHash<quint64, quint64> replacements;
    for (int target = 0; target < owner.size(); ++target) replacements.insert(beforeIds[owner[target]], afterIds[target]);
    QVector<BoundObject> updated = objectsBefore;
    qsizetype storedPoints = 0;
    QSet<quint64> owned;
    for (const auto &object : updated) {
        for (const auto &path : object.references) storedPoints += path.size();
        for (auto lineage : object.lineages) owned.insert(lineage);
    }
    QHash<QString,QString> copiedLogicalIds;
    for (const auto &source:sources)
        if (std::any_of(source.ids.cbegin(),source.ids.cend(),[&](auto id){return oldIds.contains(id);}))
            copiedLogicalIds.insert(source.id,QUuid::createUuid().toString(QUuid::WithoutBraces));
    for (const auto &source : sources) {
        const bool selected = std::any_of(source.ids.cbegin(), source.ids.cend(), [&](auto id) { return oldIds.contains(id); });
        if (!selected) continue;
        BoundObject copy; copy.id = copiedLogicalIds.value(source.id); copy.logicalId=copy.id; copy.kind = source.kind;
        if(source.hasModel){
            copy.model=activeModel(source);copy.model.id=copy.id;
            for (auto *ref:{&copy.model.startAttachment,&copy.model.endAttachment}) {
                if (copiedLogicalIds.contains(ref->objectId)) ref->objectId=copiedLogicalIds.value(ref->objectId);
                else *ref={};
            }
            QTransform translation;translation.translate(offset.x(),offset.y());
            copy.hasModel=repaper::drawing::transform(copy.model,translation);
        }
        copy.portIds = source.portIds;
        for (auto point : source.ports) copy.ports.append(point + offset);
        for (auto point : source.wirePath) copy.wirePath.append(point + offset);
        for (auto id : source.ids) {
            const auto *line = newById.value(replacements.value(id));
            if (!line || owned.contains(line->lineageId)) return fail("The copied object has an ambiguous native member.");
            owned.insert(line->lineageId); copy.lineages.append(line->lineageId); copy.references.append(line->stroke.points);
            storedPoints += line->stroke.points.size();
        }
        if (updated.size() >= MaxObjects || storedPoints > MaxStoredPoints)
            return fail("The copied object exceeds the binding storage limit.");
        updated.append(std::move(copy));
    }
    if (updated.size() != objectsBefore.size()) {
        if (!persist(updated)) return false;
        m_objects = std::move(updated);
    }
    return resolve(after);
}

bool NativeObjectBindings::selectedModel(const QVector<quint64> &ids, repaper::drawing::Item *item) const {
    if(!m_ready||!item||ids.isEmpty())return false;
    auto wanted=ids;std::sort(wanted.begin(),wanted.end());
    const auto index=m_memberIndex.constFind(ids.first());
    if(index!=m_memberIndex.cend()){
        const auto &object=m_resolved[index.value()];
        auto members=object.ids;std::sort(members.begin(),members.end());
        if(wanted==members&&object.hasModel){*item=activeModel(object);return true;}
    }
    return false;
}
bool NativeObjectBindings::registerReplacement(const NativeObjectSnapshot &before,const QVector<quint64> &beforeIds,
        const NativeObjectSnapshot &after,const QVector<quint64> &afterIds,const repaper::drawing::Item &item){
    return registerReplacementsBatch(before,{beforeIds},after,{afterIds},{item});
}
repaper::drawing::Document NativeObjectBindings::documentModels() const {
    return m_ready?m_models:repaper::drawing::Document{};
}
repaper::drawing::Item NativeObjectBindings::activeModel(const ResolvedObject &object) const {
    auto model=object.model;
    if(model.kind!="wire"||!repaper::drawing::hasEndpoints(model))return model;
    for(const auto &endpoint:object.placementAttachedEndpoints) {
        const int end=endpoint=="start"?0:1;
        auto &attachment=end==0?model.startAttachment:model.endAttachment;
        if(attachment.empty())continue;
        bool coincident=false;
        const auto found=m_resolvedIndex.constFind(attachment.objectId);
        if(found!=m_resolvedIndex.cend()&&m_resolved[found.value()].hasModel) {
            const auto &target=m_resolved[found.value()];
            const int port=target.model.portIds.indexOf(attachment.portId);
            if(port>=0&&port<target.model.anchors.size()){
                const auto point=target.model.anchors[port],wirePoint=model.sourcePoints[end];
                const qreal magnitude=std::max({qreal(1),std::abs(point.x()),std::abs(point.y()),std::abs(wirePoint.x()),std::abs(wirePoint.y())});
                const qreal tolerance=std::max(qreal(1e-5),2*magnitude*std::numeric_limits<float>::epsilon());
                coincident=QLineF(point,wirePoint).length()<=tolerance;
            }
        }
        // The persisted candidate survives Undo. A moved native endpoint does
        // not silently reconnect when the old target later reappears or reloads.
        if(!coincident)attachment={};
    }
    return model;
}
bool NativeObjectBindings::connectionModelsReady() const {
    return m_ready&&m_connectionsReady;
}
QVector<quint64> NativeObjectBindings::idsForObject(const QString &logicalId) const {
    const auto found=m_resolvedIndex.constFind(logicalId);
    if(m_ready&&found!=m_resolvedIndex.cend()&&m_resolved[found.value()].hasModel)return m_resolved[found.value()].ids;
    return {};
}
bool NativeObjectBindings::registerReplacementsBatch(const NativeObjectSnapshot &before,
        const QVector<QVector<quint64>> &oldGroups,const NativeObjectSnapshot &after,
        const QVector<QVector<quint64>> &newGroups,const repaper::drawing::Document &models) {
    const auto fail=[&](const char *reason){m_reason=QString::fromLatin1(reason);return false;};
    if (models.isEmpty() || models.size()>MaxObjectLines || oldGroups.size()!=models.size()
        || newGroups.size()!=models.size() || before.documentId!=after.documentId
        || before.pageId!=after.pageId || before.layerId!=after.layerId)
        return fail("The replacement batch has an invalid scope or size.");
    QHash<quint64,const NativeObjectLine*> oldIndex,newIndex;
    if (!snapshotIndex(before,&oldIndex) || !snapshotIndex(after,&newIndex) || !observe(before))
        return fail("The replacement batch needs complete native snapshots.");
    QSet<QString> logicalIds; QSet<quint64> oldIds,newIds,oldLineages;
    for (qsizetype group=0;group<models.size();++group) {
        repaper::drawing::Item original;
        if (!selectedModel(oldGroups[group],&original) || original.id!=models[group].id
            || logicalIds.contains(original.id) || newGroups[group].isEmpty())
            return fail("The replacement source is not one complete logical object.");
        logicalIds.insert(original.id);
        for (auto id:oldGroups[group]) {
            const auto *line=oldIndex.value(id);
            if (!line || oldIds.contains(id)) return fail("The replacement sources overlap.");
            oldIds.insert(id);oldLineages.insert(line->lineageId);
        }
        for (auto id:newGroups[group]) {
            const auto *line=newIndex.value(id);
            if (!line || newIds.contains(id) || oldIndex.contains(id) || line->lineageId!=id)
                return fail("The replacement did not produce distinct fresh native members.");
            newIds.insert(id);
        }
    }
    if (oldIds.size()>MaxObjectLines || newIds.size()>MaxObjectLines
        || after.lines.size()!=before.lines.size()-oldIds.size()+newIds.size())
        return fail("The replacement batch exceeds its native member budget.");
    for (const auto &line:after.lines) if (oldLineages.contains(line.lineageId))
        return fail("A replaced native lineage is still active.");
    for (const auto &line:before.lines) if (!oldIds.contains(line.id)) {
        const auto *retained=newIndex.value(line.id);
        if (!retained || retained->lineageId!=line.lineageId || retained->parentId!=line.parentId
            || retained->version!=line.version)
            return fail("Unrelated native ink changed during replacement.");
    }
    auto objects=m_objects;
    for (qsizetype group=0;group<models.size();++group) {
        BoundObject revision;
        if (!insertedBinding(models[group],after,newGroups[group],objects,&revision)) return false;
        // Retain every old revision for native Undo. Only this revision's new
        // explicit lineages become active after the grouped native command.
        objects.append(std::move(revision));
    }
    if (!persist(objects)) return false;
    m_objects=std::move(objects);return resolve(after);
}
NativeSelectionExpansion NativeObjectBindings::expandSelection(const QVector<quint64> &ids) const {
    if (!m_ready || ids.size() > MaxObjectLines)
        return {false, {}, QStringLiteral("The object bindings need a fresh native page snapshot.")};
    QSet<quint64> selected;
    QVector<quint64> result;
    for (auto id : ids) {
        if (!m_availableIds.contains(id) || selected.contains(id))
            return {false, {}, QStringLiteral("The native selection changed before object expansion.")};
        if (m_invalidIds.contains(id))
            return {false, {}, QStringLiteral("This object's native parts are incomplete or no longer share one transform.")};
        selected.insert(id); result.append(id);
    }
    for (const auto &object : m_resolved) {
        const bool touched = std::any_of(object.ids.cbegin(), object.ids.cend(), [&](auto id) { return selected.contains(id); });
        if (!touched) continue;
        for (auto id : object.ids) if (!selected.contains(id)) {
            if (result.size() >= MaxObjectLines)
                return {false, {}, QStringLiteral("The complete objects exceed the native selection limit.")};
            selected.insert(id); result.append(id);
        }
    }
    return {true, result, {}};
}

NativeWireSnap NativeObjectBindings::snapWirePoint(QPointF point, qreal tolerance,
                                                 const QString &excludedObject) const {
    NativeWireSnap result; result.point = point;
    if (!m_ready || !bounded(point) || !std::isfinite(tolerance) || tolerance <= 0 || tolerance > 10000) return result;
    if(m_snapExcludedObject!=excludedObject){
        m_snapExcludedObject=excludedObject;m_snapBlockedObjects.clear();
        QStringList pending;if(!excludedObject.isEmpty())pending.append(excludedObject);
        while(!pending.isEmpty()){
            const auto id=pending.takeLast();if(m_snapBlockedObjects.contains(id))continue;
            m_snapBlockedObjects.insert(id);pending+=m_dependents.value(id);
        }
    }
    const auto allowed=[&](const ResolvedObject &object){
        return object.id!=excludedObject&&!m_snapBlockedObjects.contains(object.id);
    };
    const auto finish=[&]{
        for (const auto &object:m_resolved) if (object.id==result.objectId && !object.hasModel) {
            // Preserve a legacy geometric snap without inventing a connection
            // to a model that the document cannot currently regenerate.
            result.objectId.clear();result.portId.clear();result.wirePosition=-1;break;
        }
        return result;
    };
    qreal best = tolerance * tolerance;
    // A nearby terminal takes precedence over a segment projection, so drawing
    // near a wire end cannot leave a short accidental gap to that terminal.
    // Persisted object order makes equal-distance choices stable during moves.
    for (const auto &object : m_resolved) if (allowed(object))
        for (qsizetype i = 0; i < object.ports.size(); ++i) {
            const qreal distance = distanceSquared(object.ports[i] - point);
            if (distance <= best && (!result.snapped || distance < best - 1e-12)) {
                best = distance; result = {true, object.ports[i], object.id, object.portIds[i], false};
            }
        }
    if (result.snapped) return finish();
    for (const auto &object : m_resolved) if (object.kind == "wire" && allowed(object)) {
        qreal total=0,travelled=0;
        for (qsizetype i=1;i<object.wirePath.size();++i) total+=QLineF(object.wirePath[i-1],object.wirePath[i]).length();
        if (!std::isfinite(total) || total<1e-9) continue;
        for (qsizetype i = 1; i < object.wirePath.size(); ++i) {
            const auto a = object.wirePath[i - 1], segment = object.wirePath[i] - a;
            const qreal length = distanceSquared(segment);
            if (length <= 1e-12) continue;
            const qreal segmentLength=std::sqrt(length);
            const qreal t = std::clamp(QPointF::dotProduct(point - a, segment) / length, qreal(0), qreal(1));
            const auto nearest = a + t * segment;
            const qreal distance = distanceSquared(nearest - point);
            if (distance <= best && (!result.snapped || distance < best - 1e-12)) {
                best = distance; result = {true, nearest, object.id, QStringLiteral("route"), true,
                    std::clamp((travelled+t*segmentLength)/total,qreal(0),qreal(1))};
            }
            travelled+=segmentLength;
        }
    }
    return finish();
}

QJsonObject NativeObjectBindings::encode(const BoundObject &object) {
    QJsonArray lineages, ports;
    for (auto lineage : object.lineages) lineages.append(hexId(lineage));
    QByteArray raw;
    QDataStream stream(&raw, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian); stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
    stream << quint32(object.references.size());
    for (const auto &path : object.references) {
        stream << quint32(path.size());
        for (auto point : path) stream << float(point.x()) << float(point.y());
    }
    for (qsizetype i = 0; i < object.ports.size(); ++i)
        ports.append(QJsonArray{object.portIds[i], object.ports[i].x(), object.ports[i].y()});
    QJsonObject encoded{{"id", object.id}, {"logicalId",object.logicalId}, {"kind", object.kind}, {"lineages", lineages},
            {"referencePoints", QString::fromLatin1(raw.toBase64())},
            {"ports", ports}, {"wirePath", pointArray(object.wirePath)}};
    if(object.hasModel)encoded.insert("model",encodeModel(object.model));
    if(!object.placementAttachedEndpoints.isEmpty())encoded.insert("placementAttachedEndpoints",QJsonArray::fromStringList(object.placementAttachedEndpoints));
    return encoded;
}
bool NativeObjectBindings::decode(const QJsonObject &json, BoundObject *object) {
    object->id = json.value("id").toString(); object->kind = json.value("kind").toString();
    object->logicalId = json.contains("logicalId") ? json.value("logicalId").toString() : object->id;
    if (!identifier(object->id) || !identifier(object->logicalId) || !identifier(object->kind) || !json.value("lineages").isArray()
        || !json.value("referencePoints").isString() || !json.value("ports").isArray()) return false;
    const auto lineages = json.value("lineages").toArray();
    if (lineages.isEmpty() || lineages.size() > MaxObjectLines) return false;
    QSet<quint64> seen;
    for (const auto &entry : lineages) {
        const auto id = readId(entry);
        if (!id || seen.contains(id)) return false;
        seen.insert(id); object->lineages.append(id);
    }
    const auto encoded = json.value("referencePoints").toString().toLatin1();
    if (encoded.size() > MaxReferenceBytes * 2) return false;
    // The reference format is fixed-width raw bytes. qUncompress treats its
    // length header as a hint and may allocate beyond it on malformed input;
    // raw base64 keeps every allocation bounded before point decoding.
    const auto raw = QByteArray::fromBase64(encoded, QByteArray::AbortOnBase64DecodingErrors);
    if (raw.size() < 4 || raw.size() > MaxReferenceBytes) return false;
    QDataStream stream(raw);
    stream.setByteOrder(QDataStream::LittleEndian); stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
    quint32 count = 0; stream >> count;
    if (count != quint32(object->lineages.size())) return false;
    qsizetype total = 0;
    for (quint32 i = 0; i < count; ++i) {
        quint32 size = 0; stream >> size;
        if (size < 2 || size > MaxObjectPoints - total) return false;
        total += size; PaperDrawing::Polyline path; path.reserve(size);
        for (quint32 j = 0; j < size; ++j) {
            float x = 0, y = 0; stream >> x >> y;
            if (!bounded({x, y})) return false;
            path.append({x, y});
        }
        object->references.append(std::move(path));
    }
    if (stream.status() != QDataStream::Ok || !stream.atEnd()) return false;
    const auto ports = json.value("ports").toArray();
    if (ports.size() > MaxPorts) return false;
    QSet<QString> names;
    for (const auto &entry : ports) {
        if (!entry.isArray()) return false;
        const auto port = entry.toArray();
        if (port.size() != 3 || !port[0].isString() || !port[1].isDouble() || !port[2].isDouble()
            || !identifier(port[0].toString()) || names.contains(port[0].toString())) return false;
        const QPointF point(port[1].toDouble(), port[2].toDouble());
        if (!bounded(point)) return false;
        names.insert(port[0].toString()); object->portIds.append(port[0].toString()); object->ports.append(point);
    }
    if(!readPoints(json.value("wirePath"), MaxObjectPoints, &object->wirePath))return false;
    if(json.contains("model")){
        if(!json.value("model").isObject())return false;
        object->model.id=object->logicalId;object->model.kind=object->kind;
        if(!decodeModel(json.value("model").toObject(),object->model))return false;
        object->hasModel=true;
    }
    if(json.contains("placementAttachedEndpoints")) {
        const auto value=json.value("placementAttachedEndpoints");
        if(!value.isArray()||value.toArray().size()>2||object->kind!="wire"||!object->hasModel)return false;
        for(const auto &entry:value.toArray()) {
            if(!entry.isString()||(entry.toString()!="start"&&entry.toString()!="end")||object->placementAttachedEndpoints.contains(entry.toString()))return false;
            const auto &attachment=entry.toString()=="start"?object->model.startAttachment:object->model.endAttachment;
            if(attachment.empty())return false;
            object->placementAttachedEndpoints.append(entry.toString());
        }
    }
    return true;
}

bool NativeObjectBindings::load() {
    QFile file(storagePath());
    if (!file.exists()) return true;
    m_storageBlocked = true; m_reason = "The existing object bindings are invalid and were preserved.";
    if (file.size() > MaxFileBytes || !file.open(QIODevice::ReadOnly)) return false;
    const auto bytes = file.readAll(); QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
    const auto root = document.object();
    if ((root.value("schemaVersion") != QJsonValue(2) && root.value("schemaVersion") != QJsonValue(3)
        && root.value("schemaVersion") != QJsonValue(4)) || root.value("documentId").toString() != m_document
        || root.value("pageId").toString() != m_page || readId(root.value("layerId")) != m_layerId
        || !root.value("objects").isArray() || root.value("objects").toArray().size() > MaxObjects) return false;
    QVector<BoundObject> objects; QSet<QString> ids; QSet<quint64> owned; qsizetype total = 0;
    for (const auto &entry : root.value("objects").toArray()) {
        BoundObject object;
        if (root.value("schemaVersion")==QJsonValue(4)
            && (!entry.isObject() || !entry.toObject().contains("logicalId"))) return false;
        if (!entry.isObject() || !decode(entry.toObject(), &object) || ids.contains(object.id)) return false;
        ids.insert(object.id);
        for (auto lineage : object.lineages) { if (owned.contains(lineage)) return false; owned.insert(lineage); }
        for (const auto &path : object.references) total += path.size();
        if (total > MaxStoredPoints) return false;
        objects.append(std::move(object));
    }
    m_objects = std::move(objects); m_fileDigest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
    m_storageBlocked = false; m_reason.clear(); return true;
}
bool NativeObjectBindings::persist(const QVector<BoundObject> &objects) {
    const auto fail = [&](const char *message) {
        m_storageBlocked = true; m_reason = QString::fromLatin1(message); return false;
    };
    if (m_storageBlocked) return false;
    QJsonArray values; for (const auto &object : objects) values.append(encode(object));
    const auto bytes = QJsonDocument(QJsonObject{{"schemaVersion", 4}, {"documentId", m_document},
        {"pageId", m_page}, {"layerId", hexId(m_layerId)}, {"objects", values}}).toJson(QJsonDocument::Compact);
    if (bytes.size() > MaxFileBytes) return fail("The object binding storage limit was reached.");
    if (!QDir().mkpath(m_directory)) return fail("The object binding directory could not be created.");
    QLockFile lock(storagePath() + ".lock");
    if (!lock.tryLock(0)) return fail("Another object binding writer is active.");
    QFile previous(storagePath()); QByteArray digest;
    if (previous.exists()) {
        if (previous.size() > MaxFileBytes || !previous.open(QIODevice::ReadOnly))
            return fail("The previous object bindings could not be verified.");
        digest = QCryptographicHash::hash(previous.readAll(), QCryptographicHash::Sha256);
    }
    if (digest != m_fileDigest) return fail("The object bindings changed outside this session.");
    QSaveFile file(storagePath()); file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)
        || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
        || file.write(bytes) != bytes.size() || !file.commit())
        return fail("The object bindings could not be saved atomically.");
    m_fileDigest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
    return true;
}
}
