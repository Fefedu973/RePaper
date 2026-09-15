#include "NativeObjectBindings.h"
#include "NativeStrokeSampling.h"
#include "NativeArrowStroke.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QImage>
#include <QPainter>
#include <QTemporaryDir>
#include <QtTest>
#include <limits>

using namespace RePaperNative;
using namespace repaper::drawing;
namespace {
NativeObjectSnapshot emptyPage() {
    NativeObjectSnapshot snapshot;
    snapshot.documentId = "document-a"; snapshot.pageId = "page-a";
    snapshot.layer = 0; snapshot.layerId = 0x100000000000a;
    snapshot.complete = true; snapshot.pendingEditIdentity = true; snapshot.fingerprint = "snapshot-a";
    return snapshot;
}
Item resistor() {
    Item item; item.kind = "symbol"; item.symbolId = "resistor-iec";
    item.sourcePoints = {{200, 200}, {440, 200}, {200, 360}};
    if (!rebuild(item)) return {};
    return item;
}
Item wire(QPointF a = {100, 500}, QPointF b = {700, 800}, const QString &style = "solid") {
    Item item; item.kind = "wire"; item.sourcePoints = {a, b}; item.style = style;
    if (!rebuild(item)) return {};
    return item;
}
QVector<quint64> addItem(NativeObjectSnapshot &snapshot, const Item &item, quint64 start = 100) {
    QVector<quint64> ids;
    for (const auto &stroke : item.strokes) {
        NativeObjectLine line;
        line.id = 0x1000000000000 + start++; line.parentId = snapshot.layerId; line.lineageId = line.id;
        line.version = QByteArray::number(line.id); line.tool = 19; line.stroke = stroke;line.uniformPointWidth=true;
        line.stroke.points = sampleStrokeForNativeSelection(stroke.points).points;
        for (auto &point : line.stroke.points) point = QPointF(float(point.x()), float(point.y()));
        ids.append(line.id); snapshot.lines.append(std::move(line));
    }
    return ids;
}
NativeObjectSnapshot transformed(NativeObjectSnapshot snapshot, const QTransform &transform, quint64 idOffset = 1000) {
    for (auto &line : snapshot.lines) {
        line.id += idOffset; line.version += "-moved";
        for (auto &point : line.stroke.points) {
            point = transform.map(point); point = QPointF(float(point.x()), float(point.y()));
        }
    }
    snapshot.selectedIds.clear(); snapshot.fingerprint += "-moved";
    return snapshot;
}
QByteArray readFile(const QString &path) {
    QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}
bool writeFile(const QString &path, const QByteArray &bytes) {
    QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
bool close(QPointF a, QPointF b) { return QLineF(a, b).length() < .2; }
}

class NativeObjectBindingsTest : public QObject {
    Q_OBJECT
private slots:
    void semanticCacheDoesNotRetainNativeHistoryCommands() {
        QTemporaryDir directory;NativeObjectBindings bindings(directory.path());auto snapshot=emptyPage();
        std::weak_ptr<void> retained;
        {
            NativeHistoryCommand command;command.retained=std::make_shared<int>(42);retained=command.retained;
            snapshot.history.undo.append(command);
        }
        QVERIFY(bindings.observe(snapshot));snapshot.history={};
        QVERIFY(retained.expired());QVERIFY(bindings.ready());
    }
    void cachedModelsShareStorageAndDetachedGeometryStillRevalidates() {
        QTemporaryDir directory;NativeObjectBindings bindings(directory.path());auto original=emptyPage();
        const auto item=resistor();const auto ids=addItem(original,item);
        QVERIFY(bindings.registerInserted(item,original,ids));const auto models=bindings.documentModels();
        QVERIFY(!models.isEmpty());
        for(int i=0;i<100;++i){
            QCOMPARE(bindings.documentModels().constData(),models.constData());
            QVERIFY(bindings.connectionModelsReady());QCOMPARE(bindings.idsForObject(item.id),ids);
        }
        QVERIFY(bindings.observe(original));QCOMPARE(bindings.documentModels().constData(),models.constData());
        auto changed=original;
        changed.lines[0].stroke.points[0]+=QPointF(31,47);
        // Even an incorrectly reused fingerprint cannot bless a detached
        // snapshot whose actual immutable line allocation has changed.
        QVERIFY(bindings.observe(changed));QVERIFY(bindings.documentModels().isEmpty());
        Item selected;QVERIFY(!bindings.selectedModel(ids,&selected));
        QVERIFY(bindings.observe(original));QCOMPARE(bindings.documentModels().size(),1);
    }
    void stencilPlacementUpdatesOnlyTheChosenWireBindingAndSurvivesUndoRedo() {
        QTemporaryDir directory;NativeObjectBindings bindings(directory.path());
        auto original=emptyPage();const auto trunk=wire({100,500},{300,500});const auto trunkIds=addItem(original,trunk);
        QVERIFY(bindings.registerInserted(trunk,original,trunkIds));const auto before=QJsonDocument::fromJson(readFile(bindings.storagePath())).object();
        auto component=resistor();const auto placement=placeStencilNearPointer(component,{303,500},{trunk},8);QVERIFY(placement.snapped);
        QTransform position;position.translate(placement.delta.x(),placement.delta.y());QVERIFY(transform(component,position));
        auto after=original;const auto componentIds=addItem(after,component,200);
        QVERIFY(bindings.registerInserted(component,after,componentIds,&placement));QCOMPARE(bindings.activeObjectCount(),2);
        Item current;QVERIFY(bindings.selectedModel(trunkIds,&current));QCOMPARE(current.endAttachment.objectId,component.id);
        QCOMPARE(current.endAttachment.portId,QString("left"));QCOMPARE(current.sourcePoints,trunk.sourcePoints);
        const auto bytes=readFile(bindings.storagePath());const auto saved=QJsonDocument::fromJson(bytes).object();
        QCOMPARE(saved["objects"].toArray().size(),2);const auto previous=before["objects"].toArray()[0].toObject();
        const auto updated=saved["objects"].toArray()[0].toObject();
        for(const auto &field:{"id","logicalId","lineages","referencePoints","ports","wirePath"})QCOMPARE(updated[field],previous[field]);
        QCOMPARE(updated["placementAttachedEndpoints"].toArray(),QJsonArray({"end"}));
        QVERIFY(bindings.observe(original));QVERIFY(bindings.selectedModel(trunkIds,&current));QVERIFY(current.endAttachment.empty());
        QCOMPARE(bindings.documentModels().size(),1);QVERIFY(bindings.documentModels()[0].endAttachment.empty());
        QCOMPARE(readFile(bindings.storagePath()),bytes);
        NativeObjectBindings reopened(directory.path());QVERIFY(reopened.observe(after));QVERIFY(reopened.selectedModel(trunkIds,&current));
        QCOMPARE(current.endAttachment.objectId,component.id);QVERIFY(reopened.connectionModelsReady());
        QVERIFY(reopened.observe(original));QVERIFY(reopened.selectedModel(trunkIds,&current));QVERIFY(current.endAttachment.empty());
        QVERIFY(reopened.observe(after));QVERIFY(reopened.selectedModel(trunkIds,&current));QCOMPARE(current.endAttachment.objectId,component.id);
    }
    void movedWireDoesNotReconnectWhenAPlacedTargetReturnsAfterReload() {
        QTemporaryDir directory;NativeObjectBindings bindings(directory.path());auto original=emptyPage();
        const auto trunk=wire({100,500},{300,500});const auto trunkIds=addItem(original,trunk);QVERIFY(bindings.registerInserted(trunk,original,trunkIds));
        auto component=resistor();const auto placement=placeStencilNearPointer(component,{300,500},{trunk},8);
        QTransform position;position.translate(placement.delta.x(),placement.delta.y());QVERIFY(transform(component,position));
        auto after=original;const auto componentIds=addItem(after,component,200);QVERIFY(bindings.registerInserted(component,after,componentIds,&placement));
        const auto bytes=readFile(bindings.storagePath());QTransform movement;movement.translate(0,40);
        auto movedAbsent=transformed(original,movement);QVector<quint64> movedIds;for(const auto &line:movedAbsent.lines)movedIds.append(line.id);
        QVERIFY(bindings.observe(movedAbsent));Item current;QVERIFY(bindings.selectedModel(movedIds,&current));QVERIFY(current.endAttachment.empty());
        auto returned=movedAbsent;for(const auto &line:after.lines)if(componentIds.contains(line.id))returned.lines.append(line);
        returned.fingerprint="moved-wire-target-returned";
        NativeObjectBindings reopened(directory.path());QVERIFY(reopened.observe(returned));QVERIFY(reopened.selectedModel(movedIds,&current));
        QVERIFY(current.endAttachment.empty());QCOMPARE(current.sourcePoints.last(),QPointF(300,540));
        for(const auto &model:reopened.documentModels())if(model.id==trunk.id)QVERIFY(model.endAttachment.empty());
        QVERIFY(reopened.observe(after));QVERIFY(reopened.selectedModel(trunkIds,&current));QCOMPARE(current.endAttachment.objectId,component.id);
        QVERIFY(reopened.observe(returned));QVERIFY(reopened.selectedModel(movedIds,&current));QVERIFY(current.endAttachment.empty());
        QCOMPARE(readFile(bindings.storagePath()),bytes);
    }
    void wireEditedWhilePlacedTargetIsAbsentKeepsItsNewRevisionDetached() {
        QTemporaryDir directory;NativeObjectBindings bindings(directory.path());auto original=emptyPage();
        const auto trunk=wire({100,500},{300,500});const auto oldIds=addItem(original,trunk);QVERIFY(bindings.registerInserted(trunk,original,oldIds));
        auto component=resistor();const auto placement=placeStencilNearPointer(component,{300,500},{trunk},8);
        QTransform position;position.translate(placement.delta.x(),placement.delta.y());QVERIFY(transform(component,position));
        auto withTarget=original;const auto targetIds=addItem(withTarget,component,200);QVERIFY(bindings.registerInserted(component,withTarget,targetIds,&placement));
        QVERIFY(bindings.observe(original));Item edited;QVERIFY(bindings.selectedModel(oldIds,&edited));QVERIFY(edited.endAttachment.empty());
        QTransform move;move.translate(40,0);QVERIFY(transform(edited,move));
        auto replaced=emptyPage();replaced.fingerprint="edited-without-target";const auto newIds=addItem(replaced,edited,400);
        QVERIFY(bindings.registerReplacement(original,oldIds,replaced,newIds,edited));
        auto returned=replaced;for(const auto &line:withTarget.lines)if(targetIds.contains(line.id))returned.lines.append(line);
        NativeObjectBindings reopened(directory.path());QVERIFY(reopened.observe(returned));Item current;QVERIFY(reopened.selectedModel(newIds,&current));
        QVERIFY(current.endAttachment.empty());QCOMPARE(current.sourcePoints.last(),QPointF(340,500));
        QVERIFY(reopened.observe(withTarget));QVERIFY(reopened.selectedModel(oldIds,&current));QCOMPARE(current.endAttachment.objectId,component.id);
        QVERIFY(reopened.observe(returned));QVERIFY(reopened.selectedModel(newIds,&current));QVERIFY(current.endAttachment.empty());
    }
    void stencilPlacementDoesNotAttachNearOrUnchosenEndpoints_data() {
        QTest::addColumn<QString>("condition");
        for(const auto &condition:{"near-not-exact","different-wire","different-local-port","wire-interior","already-connected"})
            QTest::newRow(condition)<<QString(condition);
    }
    void stencilPlacementDoesNotAttachNearOrUnchosenEndpoints() {
        QFETCH(QString,condition);QTemporaryDir directory;NativeObjectBindings bindings(directory.path());auto original=emptyPage();
        auto trunk=wire({100,500},{300,500});
        if(condition=="already-connected")trunk.endAttachment={"existing-target","left"};
        const auto ids=addItem(original,trunk);QVERIFY(bindings.registerInserted(trunk,original,ids));
        auto component=resistor();auto placement=placeStencilNearPointer(component,{300,500},{trunk},8);
        QTransform position;position.translate(placement.delta.x(),placement.delta.y());QVERIFY(transform(component,position));
        if(condition=="near-not-exact"){QTransform slight;slight.translate(.001,0);QVERIFY(transform(component,slight));}
        if(condition=="different-wire")placement.targetAttachment.objectId="another-wire";
        if(condition=="different-local-port")placement.localPortId="right";
        if(condition=="wire-interior"){placement.targetAttachment.portId="route";placement.targetAttachment.wirePosition=1;}
        auto after=original;const auto componentIds=addItem(after,component,200);QVERIFY(bindings.registerInserted(component,after,componentIds,&placement));
        Item current;QVERIFY(bindings.selectedModel(ids,&current));
        QCOMPARE(current.endAttachment.objectId,trunk.endAttachment.objectId);
        const auto saved=QJsonDocument::fromJson(readFile(bindings.storagePath())).object();
        QVERIFY(!saved["objects"].toArray()[0].toObject().contains("placementAttachedEndpoints"));
    }
    void malformedPlacementAttachmentMetadataPreservesTheExistingSidecar() {
        QTemporaryDir directory;NativeObjectBindings bindings(directory.path());auto snapshot=emptyPage();
        auto trunk=wire({100,500},{300,500});const auto ids=addItem(snapshot,trunk);QVERIFY(bindings.registerInserted(trunk,snapshot,ids));
        const auto original=QJsonDocument::fromJson(readFile(bindings.storagePath())).object();
        for(const auto &value:{QJsonValue(true),QJsonValue(QJsonArray({"route"})),QJsonValue(QJsonArray({"end","end"})),QJsonValue(QJsonArray({"end"}))}) {
            auto changed=original;auto objects=changed["objects"].toArray();auto object=objects[0].toObject();
            object["placementAttachedEndpoints"]=value;objects[0]=object;changed["objects"]=objects;
            const auto bytes=QJsonDocument(changed).toJson();QVERIFY(writeFile(bindings.storagePath(),bytes));
            NativeObjectBindings rejected(directory.path());QVERIFY(!rejected.observe(snapshot));QCOMPARE(readFile(bindings.storagePath()),bytes);
        }
    }
    void squareRootWithoutPortsRemainsEditableAfterNativeRotationAndReload() {
        QTemporaryDir directory;NativeObjectBindings bindings(directory.path());
        Item radical;radical.kind="symbol";radical.symbolId="square-root";
        radical.sourcePoints={{200,300},{800,300},{200,460}};QVERIFY(rebuild(radical));
        QVERIFY(radical.anchors.isEmpty());QVERIFY(radical.portIds.isEmpty());QCOMPARE(radical.strokes.size(),1);
        auto original=emptyPage();const auto ids=addItem(original,radical);QVERIFY(bindings.registerInserted(radical,original,ids));
        QCOMPARE(bindings.activeObjectCount(),1);QVERIFY(!bindings.snapWirePoint(radical.sourcePoints[0],20).snapped);
        QTransform change;change.translate(1000,100);change.rotate(90);
        const auto moved=transformed(original,change);NativeObjectBindings reopened(directory.path());QVERIFY(reopened.observe(moved));
        Item restored;QVERIFY(reopened.selectedModel({moved.lines[0].id},&restored));
        QCOMPARE(restored.symbolId,QString("square-root"));QCOMPARE(restored.id,radical.id);
        QVERIFY(restored.anchors.isEmpty());QVERIFY(restored.portIds.isEmpty());
        for(int i=0;i<radical.strokes[0].points.size();++i)QVERIFY(close(restored.strokes[0].points[i],change.map(radical.strokes[0].points[i])));
    }
    void voltageArrowBelongsToTheComponentAcrossReopenAndNativeTransforms_data() {
        QTest::addColumn<bool>("reversed");QTest::addColumn<bool>("otherSide");
        for(bool reversed:{false,true})for(bool otherSide:{false,true})
            QTest::newRow(qPrintable(QString("reverse-%1-side-%2").arg(reversed).arg(otherSide)))<<reversed<<otherSide;
    }
    void voltageArrowBelongsToTheComponentAcrossReopenAndNativeTransforms() {
        QFETCH(bool,reversed);QFETCH(bool,otherSide);
        QTemporaryDir directory;NativeObjectBindings bindings(directory.path());
        auto component=resistor();const auto ports=component.anchors;const auto count=component.strokes.size();
        component.voltageArrow=true;component.voltageArrowReversed=reversed;component.voltageArrowOtherSide=otherSide;
        QVERIFY(rebuild(component));QCOMPARE(component.anchors,ports);QCOMPARE(component.strokes.size(),count+2);
        auto original=emptyPage();const auto ids=addItem(original,component);
        QVERIFY(bindings.registerInserted(component,original,ids));QCOMPARE(bindings.activeObjectCount(),1);
        // Selecting either the arrow shaft or its head expands to the complete
        // component, with no new logical object or additional wire ports.
        for(auto id:{ids[ids.size()-2],ids.last()}) {
            const auto selection=bindings.expandSelection({id});QVERIFY(selection.valid);QCOMPARE(selection.ids.size(),ids.size());
        }
        const auto stored=QJsonDocument::fromJson(readFile(bindings.storagePath())).object()["objects"].toArray();
        QCOMPARE(stored.size(),1);const auto model=stored.first().toObject()["model"].toObject();
        QCOMPARE(model["voltageArrow"].toBool(),true);QCOMPARE(model["voltageArrowReversed"].toBool(),reversed);
        QCOMPARE(model["voltageArrowOtherSide"].toBool(),otherSide);
        QTransform change;change.translate(600,100);change.rotate(90);change.scale(1.25,1.25);
        const auto moved=transformed(original,change);QVector<quint64> movedIds;
        for(const auto &line:moved.lines)movedIds.append(line.id);
        NativeObjectBindings reopened(directory.path());QVERIFY(reopened.observe(moved));QCOMPARE(reopened.activeObjectCount(),1);
        Item restored;QVERIFY(reopened.selectedModel(movedIds,&restored));
        QCOMPARE(restored.id,component.id);QVERIFY(restored.voltageArrow);
        QCOMPARE(restored.voltageArrowReversed,reversed);QCOMPARE(restored.voltageArrowOtherSide,otherSide);
        QCOMPARE(restored.portIds,component.portIds);QCOMPARE(restored.strokes.size(),component.strokes.size());
        for(int i=0;i<component.strokes.size();++i)for(int p=0;p<component.strokes[i].points.size();++p)
            QVERIFY(close(restored.strokes[i].points[p],change.map(component.strokes[i].points[p])));
        QVERIFY(reopened.observe(original));QVERIFY(reopened.selectedModel(ids,&restored));QVERIFY(restored.voltageArrow);
        QVERIFY(reopened.observe(moved));QVERIFY(reopened.selectedModel(movedIds,&restored));
    }
    void malformedVoltageOptionsPreserveTheSidecar() {
        QTemporaryDir directory;NativeObjectBindings bindings(directory.path());
        auto original=emptyPage();const auto component=resistor();const auto ids=addItem(original,component);
        QVERIFY(bindings.registerInserted(component,original,ids));const auto path=bindings.storagePath();
        const auto valid=QJsonDocument::fromJson(readFile(path)).object();
        for(const auto *key:{"voltageArrow","voltageArrowReversed","voltageArrowOtherSide"}) {
            auto json=valid;auto objects=json["objects"].toArray();auto object=objects.first().toObject();
            auto model=object["model"].toObject();model[key]="true";object["model"]=model;objects[0]=object;json["objects"]=objects;
            const auto malformed=QJsonDocument(json).toJson(QJsonDocument::Compact);QVERIFY(writeFile(path,malformed));
            NativeObjectBindings reopened(directory.path());QVERIFY(!reopened.observe(original));QCOMPARE(readFile(path),malformed);
        }
        auto legacy=valid;auto objects=legacy["objects"].toArray();auto object=objects.first().toObject();
        auto model=object["model"].toObject();
        for(const auto *key:{"voltageArrow","voltageArrowReversed","voltageArrowOtherSide"})model.remove(key);
        object["model"]=model;objects[0]=object;legacy["objects"]=objects;QVERIFY(writeFile(path,QJsonDocument(legacy).toJson()));
        NativeObjectBindings reopened(directory.path());QVERIFY(reopened.observe(original));Item restored;
        QVERIFY(reopened.selectedModel(ids,&restored));QVERIFY(!restored.voltageArrow);
        QVERIFY(!restored.voltageArrowReversed);QVERIFY(!restored.voltageArrowOtherSide);
    }
    void completeResistorSelectionUsesOnlyRegisteredMembers() {
        QTemporaryDir directory; NativeObjectBindings bindings(directory.path());
        auto snapshot = emptyPage(); const auto item = resistor();
        QVERIFY(item.strokes.size() > 1);
        const auto ids = addItem(snapshot, item);
        const auto unrelated = addItem(snapshot, wire({210, 205}, {430, 355}), 500);
        QVERIFY(bindings.registerInserted(item, snapshot, ids));
        QCOMPARE(bindings.activeObjectCount(), 1);
        for (auto id : ids) {
            const auto expanded = bindings.expandSelection({id});
            QVERIFY(expanded.valid); QCOMPARE(QSet<quint64>(expanded.ids.cbegin(), expanded.ids.cend()), QSet<quint64>(ids.cbegin(), ids.cend()));
        }
        const auto nativeOnly = bindings.expandSelection(unrelated);
        QVERIFY(nativeOnly.valid); QCOMPARE(nativeOnly.ids, unrelated);
        auto mixed = bindings.expandSelection({ids[0], unrelated[0]});
        QVERIFY(mixed.valid); QCOMPARE(mixed.ids.size(), ids.size() + 1);
        QVERIFY(mixed.ids.contains(unrelated[0]));
    }
    void bindingsSurviveAffineReplacementReopenUndoAndRedo() {
        QTemporaryDir directory; NativeObjectBindings bindings(directory.path());
        auto original = emptyPage(); const auto item = resistor(); const auto ids = addItem(original, item);
        QVERIFY(bindings.registerInserted(item, original, ids));
        const auto path = bindings.storagePath(); QVERIFY(QFile::exists(path));
        QTransform transform; transform.translate(130, -20); transform.rotate(38); transform.scale(1.7, .8);
        const auto moved = transformed(original, transform);
        QVERIFY(bindings.observe(moved));
        const auto expanded = bindings.expandSelection({moved.lines.last().id});
        QVERIFY(expanded.valid); QCOMPARE(expanded.ids.size(), ids.size());
        for (const auto &line : moved.lines) QVERIFY(expanded.ids.contains(line.id));
        const auto port = transform.map(item.anchors.first());
        auto snap = bindings.snapWirePoint(port + QPointF(3, 2), 10);
        QVERIFY(snap.snapped); QVERIFY(close(snap.point, port)); QCOMPARE(snap.portId, item.portIds.first());
        NativeObjectBindings reopened(directory.path());
        QVERIFY(reopened.observe(moved)); QCOMPARE(reopened.storagePath(), path); QCOMPARE(reopened.activeObjectCount(), 1);
        QVERIFY(reopened.expandSelection({moved.lines.first().id}).valid);
        QVERIFY(reopened.observe(original));
        QVERIFY(reopened.snapWirePoint(item.anchors.first() + QPointF(2, 1), 10).snapped);
        QVERIFY(reopened.observe(moved));
        QVERIFY(close(reopened.snapWirePoint(port, 10).point, port));
        auto deleted = moved; deleted.lines.clear(); deleted.fingerprint = "deleted";
        QVERIFY(reopened.observe(deleted)); QCOMPARE(reopened.activeObjectCount(), 0);
        QVERIFY(!reopened.snapWirePoint(port, 10).snapped);
        QVERIFY(reopened.observe(original)); QCOMPARE(reopened.activeObjectCount(), 1);
    }
    void partialErasureAndAmbiguousLineageNeverGrabOtherInk() {
        QTemporaryDir directory; NativeObjectBindings bindings(directory.path());
        auto original = emptyPage(); const auto item = resistor(); const auto ids = addItem(original, item);
        QVERIFY(bindings.registerInserted(item, original, ids));
        auto partial = original; partial.lines.removeLast();
        QVERIFY(bindings.observe(partial)); QCOMPARE(bindings.activeObjectCount(), 0);
        QVERIFY(!bindings.expandSelection({partial.lines.first().id}).valid);
        QVERIFY(!bindings.snapWirePoint(item.anchors.first(), 10).snapped);
        auto ambiguous = original; auto extra = ambiguous.lines.first(); extra.id += 5000;
        ambiguous.lines.append(extra);
        QVERIFY(bindings.observe(ambiguous)); QVERIFY(!bindings.expandSelection({ids.first()}).valid);
        auto split = original; split.lines.first().stroke.points.removeLast();
        QVERIFY(bindings.observe(split)); QVERIFY(!bindings.expandSelection({ids.first()}).valid);
        QVERIFY(bindings.observe(original)); QVERIFY(bindings.expandSelection({ids.first()}).valid);
    }
    void independentlyMovedPartInvalidatesOnlyItsObject() {
        QTemporaryDir directory; NativeObjectBindings bindings(directory.path());
        auto snapshot = emptyPage(); const auto item = resistor(); const auto ids = addItem(snapshot, item);
        QVERIFY(bindings.registerInserted(item, snapshot, ids));
        const auto other = wire(); const auto wireIds = addItem(snapshot, other, 500);
        QVERIFY(bindings.registerInserted(other, snapshot, wireIds));
        for (auto &point : snapshot.lines.first().stroke.points) point += QPointF(20, 0);
        QVERIFY(bindings.observe(snapshot)); QCOMPARE(bindings.activeObjectCount(), 1);
        QVERIFY(!bindings.expandSelection({ids.first()}).valid);
        QVERIFY(bindings.expandSelection(wireIds).valid);
        QVERIFY(bindings.snapWirePoint(other.anchors.first(), 10).snapped);
    }
    void identicalNativeCopiesDoNotAcquireOwnership() {
        QTemporaryDir directory; NativeObjectBindings bindings(directory.path());
        auto snapshot = emptyPage(); const auto item = resistor(); const auto ids = addItem(snapshot, item);
        QVERIFY(bindings.registerInserted(item, snapshot, ids));
        const auto copyIds = addItem(snapshot, item, 500);
        QVERIFY(bindings.observe(snapshot));
        const auto expanded = bindings.expandSelection({copyIds.first()});
        QVERIFY(expanded.valid); QCOMPARE(expanded.ids, QVector<quint64>{copyIds.first()});
        QCOMPARE(bindings.activeObjectCount(), 1);
    }
    void wiresSnapToPortsEndpointsAndSegmentInteriors() {
        QTemporaryDir directory; NativeObjectBindings bindings(directory.path());
        auto snapshot = emptyPage(); const auto component = resistor(); const auto resistorIds = addItem(snapshot, component);
        QVERIFY(bindings.registerInserted(component, snapshot, resistorIds));
        const auto connector = wire(); const auto wireIds = addItem(snapshot, connector, 500);
        QVERIFY(bindings.registerInserted(connector, snapshot, wireIds));
        const auto port = component.anchors.first();
        auto snap = bindings.snapWirePoint(port + QPointF(8, -3), 12);
        QVERIFY(snap.snapped); QCOMPARE(snap.objectId, component.id); QVERIFY(!snap.wireSegment); QCOMPARE(snap.point, port);
        snap = bindings.snapWirePoint({300, 507}, 12);
        QVERIFY(snap.snapped); QVERIFY(snap.wireSegment); QCOMPARE(snap.point, QPointF(300, 500));
        snap = bindings.snapWirePoint({704, 802}, 12);
        QVERIFY(snap.snapped); QVERIFY(!snap.wireSegment); QCOMPARE(snap.point, connector.anchors.last());
        QVERIFY(!bindings.snapWirePoint({300, 520}, 12).snapped);
        QVERIFY(!bindings.snapWirePoint({300, 507}, 12, connector.id).snapped);
        bindings.invalidate(); QVERIFY(!bindings.ready()); QVERIFY(!bindings.snapWirePoint(port, 12).snapped);
        QVERIFY(!bindings.expandSelection({resistorIds.first()}).valid);
    }
    void dashedWireHasLogicalPortsAndContinuousSnapPath() {
        QTemporaryDir directory; NativeObjectBindings bindings(directory.path());
        auto snapshot = emptyPage(); const auto connector = wire({100, 500}, {613, 500}, "dashed");
        const auto ids = addItem(snapshot, connector);
        QVERIFY(ids.size() > 1); QVERIFY(bindings.registerInserted(connector, snapshot, ids));
        auto snap = bindings.snapWirePoint({611, 503}, 8);
        QVERIFY(snap.snapped); QCOMPARE(snap.point, QPointF(613, 500)); QVERIFY(!snap.wireSegment);
        snap = bindings.snapWirePoint({133, 504}, 8); // In a visible dash gap.
        QVERIFY(snap.snapped); QCOMPARE(snap.point, QPointF(133, 500)); QVERIFY(snap.wireSegment);
    }
    void layerAndPageScopeCannotReuseBindings() {
        QTemporaryDir directory; NativeObjectBindings bindings(directory.path());
        auto snapshot = emptyPage(); const auto item = resistor(); const auto ids = addItem(snapshot, item);
        QVERIFY(bindings.registerInserted(item, snapshot, ids)); const auto originalPath = bindings.storagePath();
        auto other = snapshot; other.layer = 2;
        QVERIFY(bindings.observe(other)); QCOMPARE(bindings.activeObjectCount(), 1); QCOMPARE(bindings.storagePath(), originalPath);
        other.layerId += 1;
        QVERIFY(bindings.observe(other)); QCOMPARE(bindings.activeObjectCount(), 0); QVERIFY(bindings.storagePath() != originalPath);
        other = snapshot; other.pageId = "another-page";
        QVERIFY(bindings.observe(other)); QCOMPARE(bindings.activeObjectCount(), 0);
        QVERIFY(bindings.observe(snapshot)); QCOMPARE(bindings.activeObjectCount(), 1);
    }
    void insertionRequiresFreshIdsAndMatchingOrderedGeometry() {
        QTemporaryDir directory; NativeObjectBindings bindings(directory.path());
        auto snapshot = emptyPage(); const auto item = resistor(); const auto ids = addItem(snapshot, item);
        auto reversed = ids; std::reverse(reversed.begin(), reversed.end());
        QVERIFY(!bindings.registerInserted(item, snapshot, reversed));
        QVERIFY(!QFile::exists(bindings.storagePath()));
        auto wrong = snapshot; wrong.lines.first().lineageId += 1000;
        QVERIFY(!bindings.registerInserted(item, wrong, ids));
        wrong = snapshot; wrong.lines.first().stroke.points.first() += QPointF(5, 0);
        QVERIFY(!bindings.registerInserted(item, wrong, ids));
        QVERIFY(bindings.registerInserted(item, snapshot, ids));
        const auto bytes = readFile(bindings.storagePath());
        QVERIFY(!bindings.registerInserted(item, snapshot, ids)); QCOMPARE(readFile(bindings.storagePath()), bytes);
    }
    void corruptAndConcurrentSidecarsArePreserved() {
        QTemporaryDir directory; NativeObjectBindings first(directory.path()), stale(directory.path());
        auto snapshot = emptyPage(); QVERIFY(first.observe(snapshot)); QVERIFY(stale.observe(snapshot));
        const auto item = resistor(); const auto ids = addItem(snapshot, item);
        QVERIFY(first.registerInserted(item, snapshot, ids)); const auto path = first.storagePath(); const auto bytes = readFile(path);
        QVERIFY(!stale.registerInserted(item, snapshot, ids)); QCOMPARE(readFile(path), bytes);
        QVERIFY(writeFile(path, "{broken")); NativeObjectBindings corrupted(directory.path());
        QVERIFY(!corrupted.observe(snapshot)); QCOMPARE(readFile(path), QByteArray("{broken"));
        auto root = QJsonDocument::fromJson(bytes).object(); auto objects = root["objects"].toArray();
        auto object = objects[0].toObject();
        QByteArray bomb(4, char(0xff)); bomb.append('x');
        object["referencePoints"] = QString::fromLatin1(bomb.toBase64()); objects[0] = object; root["objects"] = objects;
        const auto bad = QJsonDocument(root).toJson(QJsonDocument::Compact); QVERIFY(writeFile(path, bad));
        NativeObjectBindings boundedReader(directory.path()); QVERIFY(!boundedReader.observe(snapshot)); QCOMPARE(readFile(path), bad);
    }
    void nativeLineageUtilitiesCoverUnmanagedAffineSelections() {
        auto original = emptyPage(); const auto connector = wire(); const auto ids = addItem(original, connector);
        QString error; const auto lineages = selectedNativeLineages(original, ids, &error);
        QVERIFY(error.isEmpty()); QCOMPARE(lineages, ids);
        QTransform transform; transform.translate(50, 30); transform.rotate(20);
        auto moved = transformed(original, transform); const auto resolved = resolveNativeLineages(moved, lineages);
        QVERIFY(resolved.valid); QCOMPARE(resolved.ids.first(), ids.first() + 1000);
        auto duplicate = moved.lines.first(); duplicate.id += 5000; moved.lines.append(duplicate);
        QVERIFY(!resolveNativeLineages(moved, lineages).valid);
        moved = original; moved.pendingEditIdentity = false;
        QVERIFY(selectedNativeLineages(moved, ids, &error).isEmpty()); QVERIFY(!error.isEmpty());
    }
    void oversizedReferenceBlobIsRejectedBeforePointDecoding() {
        QTemporaryDir directory; NativeObjectBindings bindings(directory.path());
        auto snapshot = emptyPage(); const auto item = resistor(); const auto ids = addItem(snapshot, item);
        QVERIFY(bindings.registerInserted(item, snapshot, ids));
        const auto path = bindings.storagePath();
        auto root = QJsonDocument::fromJson(readFile(path)).object();
        QCOMPARE(root.value("schemaVersion").toInt(), 4);
        auto objects = root.value("objects").toArray(); auto object = objects.first().toObject();
        object.insert("referencePoints", QString::fromLatin1(QByteArray(2 * 1024 * 1024, 'x').toBase64()));
        objects[0] = object; root.insert("objects", objects);
        const auto malformed = QJsonDocument(root).toJson(QJsonDocument::Compact);
        QVERIFY(writeFile(path, malformed));
        NativeObjectBindings reopened(directory.path());
        QVERIFY(!reopened.observe(snapshot)); QVERIFY(!reopened.ready());
        QCOMPARE(readFile(path), malformed);
    }
    void modelsSurviveRotationReopenAndSemanticReplacementUndoRedo(){
        QTemporaryDir directory;NativeObjectBindings bindings(directory.path());
        auto before=emptyPage();auto item=wire();item.wireHasBend=true;item.wireBend=25;QVERIFY(rebuild(item));
        const auto ids=addItem(before,item);QVERIFY(bindings.registerInserted(item,before,ids));
        Item model;QVERIFY(bindings.selectedModel(ids,&model));QCOMPARE(model.sourcePoints,item.sourcePoints);QVERIFY(model.wireHasBend);
        QTransform rotate;rotate.translate(150,80);rotate.rotate(27);auto moved=transformed(before,rotate);
        NativeObjectBindings reopened(directory.path());QVERIFY(reopened.observe(moved));
        QVector<quint64> movedIds;for(const auto &line:moved.lines)movedIds.append(line.id);
        QVERIFY(reopened.selectedModel(movedIds,&model));QVERIFY(close(model.sourcePoints[1],rotate.map(item.sourcePoints[1])));
        model.sourcePoints[1]+=QPointF(80,100);model.style="dashed";QVERIFY(rebuild(model));
        auto after=emptyPage();after.fingerprint="replacement";const auto newIds=addItem(after,model,5000);
        QVERIFY(reopened.registerReplacement(moved,movedIds,after,newIds,model));
        Item current;QVERIFY(reopened.selectedModel(newIds,&current));QCOMPARE(current.style,QString("dashed"));
        QCOMPARE(current.id,model.id);QCOMPARE(reopened.activeObjectCount(),1);
        NativeObjectBindings restored(directory.path());QVERIFY(restored.observe(moved));QVERIFY(restored.selectedModel(movedIds,&current));
        QCOMPARE(current.style,QString("solid"));QVERIFY(restored.observe(after));QVERIFY(restored.selectedModel(newIds,&current));
        QCOMPARE(current.style,QString("dashed"));QCOMPARE(restored.activeObjectCount(),1);
    }
    void semanticControlsRequireEveryActualMemberToMatchTheModel(){
        QTemporaryDir directory;NativeObjectBindings bindings(directory.path());auto snapshot=emptyPage();
        const auto item=resistor();const auto ids=addItem(snapshot,item);QVERIFY(bindings.registerInserted(item,snapshot,ids));
        Item model;QVERIFY(bindings.selectedModel(ids,&model));QVERIFY(!bindings.selectedModel({ids.first()},&model));
        auto invalid=snapshot;invalid.lines.first().stroke.points.first()+=QPointF(30,20);invalid.fingerprint="edited-one-part";
        QVERIFY(bindings.observe(invalid));QVERIFY(!bindings.selectedModel(ids,&model));
        invalid=snapshot;invalid.lines.first().uniformPointWidth=false;invalid.fingerprint="variable-width";
        QVERIFY(bindings.observe(invalid));QVERIFY(!bindings.selectedModel(ids,&model));QVERIFY(bindings.expandSelection(ids).valid);
    }
    void versionSixExplicitBindingsRecoverOnlyAnExactParametricRendering(){
        for(const auto &kind:QStringList{"line","arrow","wire","rectangle","ellipse","symbol"}){
            QTemporaryDir directory;NativeObjectBindings bindings(directory.path());auto before=emptyPage();
            Item item;item.kind=kind;
            if(kind=="symbol")item=resistor();
            else if(kind=="rectangle"||kind=="ellipse")item.sourcePoints={{200,200},{440,200},{200,360}};
            else item.sourcePoints={{100,100},{400,300}};
            item.arrowDirection="both";QVERIFY(rebuildNativeObject(item));
            const auto ids=addItem(before,item);QVERIFY(bindings.registerInserted(item,before,ids));
            auto root=QJsonDocument::fromJson(readFile(bindings.storagePath())).object();root["schemaVersion"]=2;
            auto objects=root["objects"].toArray();auto object=objects.first().toObject();object.remove("model");objects[0]=object;root["objects"]=objects;
            QVERIFY(writeFile(bindings.storagePath(),QJsonDocument(root).toJson(QJsonDocument::Compact)));
            QTransform rotation;rotation.translate(150,80);rotation.rotate(29);auto current=transformed(before,rotation);
            for(const auto &line:current.lines)current.selectedIds.append(line.id);
            NativeObjectBindings reopened(directory.path());QVERIFY(reopened.observe(current));Item model;
            QVERIFY2(reopened.selectedModel(current.selectedIds,&model),qPrintable(kind));QCOMPARE(model.kind,item.kind);
            for(int i=0;i<item.sourcePoints.size();++i)QVERIFY(close(model.sourcePoints[i],rotation.map(item.sourcePoints[i])));
        }
    }
    void smallRotatedBoxesRetainControlsAcrossNativeFloatRounding() {
        for (const auto &kind : QStringList{"rectangle", "ellipse", "symbol"}) {
            QTemporaryDir directory; NativeObjectBindings bindings(directory.path());
            auto before = emptyPage(); Item item; item.kind = kind; item.symbolId = "resistor-iec";
            item.sourcePoints = {{1000, 1400}, {1040, 1400}, {1000, 1430}};
            QVERIFY(rebuildNativeObject(item)); const auto ids = addItem(before, item);
            QVERIFY(bindings.registerInserted(item, before, ids));
            QTransform rotation; rotation.translate(1020, 1415); rotation.rotate(39); rotation.translate(-1020, -1415);
            auto after = transformed(before, rotation);
            for (const auto &line : after.lines) after.selectedIds.append(line.id);
            NativeObjectBindings reopened(directory.path()); QVERIFY(reopened.observe(after)); Item model;
            QVERIFY2(reopened.selectedModel(after.selectedIds, &model), qPrintable(kind));
            QVERIFY(std::abs(boxWidth(model) - 40) < .01); QVERIFY(std::abs(boxHeight(model) - 30) < .01);
            QCOMPARE(model.width, item.width);
            for (int i = 0; i < 3; ++i) QVERIFY(close(model.sourcePoints[i], rotation.map(item.sourcePoints[i])));
            // Real shear must still lose semantic controls while retaining only
            // the explicitly known native membership.
            QTransform shear; shear.shear(.08, 0); auto distorted = transformed(before, shear);
            for (const auto &line : distorted.lines) distorted.selectedIds.append(line.id);
            QVERIFY(reopened.observe(distorted)); QVERIFY(!reopened.selectedModel(distorted.selectedIds, &model));
            QVERIFY(reopened.expandSelection(distorted.selectedIds).valid);
        }
    }
    void rotatedDiagonalPatternedLinesDoNotInferScaleFromRounding() {
        for (const auto &style : QStringList{"dashed", "dotted"}) {
            QTemporaryDir directory; NativeObjectBindings bindings(directory.path());
            auto before = emptyPage(); Item item; item.kind = "line"; item.style = style;
            item.sourcePoints = {{100, 100}, {400, 300}};
            QVERIFY(rebuildNativeObject(item)); const auto ids = addItem(before, item);
            QVERIFY(bindings.registerInserted(item, before, ids));
            QTransform rotation; rotation.rotate(31); auto after = transformed(before, rotation);
            for (const auto &line : after.lines) after.selectedIds.append(line.id);
            NativeObjectBindings reopened(directory.path()); QVERIFY(reopened.observe(after)); Item model;
            QVERIFY2(reopened.selectedModel(after.selectedIds, &model), qPrintable(style));
            QCOMPARE(model.style, style); QVERIFY(std::abs(model.patternScale - item.patternScale) < .001);
            QCOMPARE(model.width, item.width);
            for (int i = 0; i < 2; ++i) QVERIFY(close(model.sourcePoints[i], rotation.map(item.sourcePoints[i])));
        }
    }
    void opaqueStencilMasksInkWithoutChangingForegroundPaint() {
        Item item;item.kind="symbol";item.symbolId="table";item.width=4.6;
        item.sourcePoints={{200,200},{440,200},{200,360}};
        item.stencilParameters={{"rows",4},{"columns",6},{"opaqueBackground",true}};
        setForegroundColor(item,QColor(Qt::red));
        QCOMPARE(foregroundColor(item),QColor(Qt::red)); // Single empty color seed.
        QVERIFY(!isBackgroundStroke(item,0));QVERIFY(rebuild(item));
        QVERIFY(isBackgroundStroke(item,0));QCOMPARE(item.strokes.first().color,QColor(Qt::white));
        QVERIFY(item.strokes.first().width<=24);QCOMPARE(foregroundColor(item),QColor(Qt::red));
        const auto geometry=PaperDrawing::configuredStencil(item.symbolId,item.stencilParameters);
        QCOMPARE(item.strokes.size(),geometry.strokes.size()+1);
        for(qsizetype i=0;i<geometry.strokes.size();++i)
            QCOMPARE(item.strokes[i+1].width,std::max(qreal(.5),item.width*geometry.strokes[i].width/2.3));
        QImage rendered(640,480,QImage::Format_ARGB32);rendered.fill(Qt::black);
        {
            QPainter painter(&rendered);painter.setRenderHint(QPainter::Antialiasing);
            for(const auto &stroke:item.strokes){
                painter.setPen(QPen(stroke.color,stroke.width,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
                painter.drawPolyline(stroke.points.constData(),stroke.points.size());
            }
        }
        // The earlier black ink disappears throughout the interior, including
        // between serpentine passes, while ink outside the box remains intact.
        for(int y=206;y<354;++y)for(int x=206;x<434;++x)
            QVERIFY(rendered.pixelColor(x,y)!=QColor(Qt::black));
        QCOMPARE(rendered.pixelColor(190,220),QColor(Qt::black));
        QCOMPARE(rendered.pixelColor(224,224),QColor(Qt::white));
        setForegroundColor(item,QColor(Qt::blue));QVERIFY(rebuild(item));
        QCOMPARE(item.strokes.first().color,QColor(Qt::white));QCOMPARE(foregroundColor(item),QColor(Qt::blue));
        const auto parameters=item.stencilParameters;
        QTransform transform;transform.translate(180,40);transform.rotate(27);transform.scale(1.3,.8);
        QVERIFY(repaper::drawing::transform(item,transform));
        QCOMPARE(item.stencilParameters,parameters);QCOMPARE(foregroundColor(item),QColor(Qt::blue));
        QCOMPARE(item.strokes.first().color,QColor(Qt::white));
        // Editing parameters retains the chosen foreground via the same seed
        // contract used by native and desktop object-property controls.
        const auto color=foregroundColor(item);item.stencilParameters["opaqueBackground"]=false;
        item.strokes={PaperDrawing::Stroke{{},item.width,color}};QVERIFY(rebuild(item));
        QVERIFY(!isBackgroundStroke(item,0));QCOMPARE(item.strokes.size(),geometry.strokes.size());
        QCOMPARE(foregroundColor(item),QColor(Qt::blue));
        item.strokes.last().color=Qt::red;QVERIFY(!foregroundColor(item).isValid());
    }
    void opaqueStencilSamplingIsBoundedAtRealPageSize() {
        Item item;item.kind="symbol";item.symbolId="table";
        item.sourcePoints={{0,0},{1620,0},{0,2160}};
        item.stencilParameters={{"rows",30},{"columns",30}};
        QVERIFY(rebuild(item));QVERIFY(isBackgroundStroke(item,0));
        qsizetype remaining=NativeStrokePointBudget;
        for(const auto &stroke:item.strokes){
            const auto samples=sampleStrokeForNativeSelection(stroke.points,remaining);
            QVERIFY(samples.valid());remaining-=samples.points.size();
        }
        QVERIFY(remaining>0);
        auto tooLarge=item;tooLarge.sourcePoints={{0,0},{20000,0},{0,20000}};
        QVERIFY(!rebuild(tooLarge));
        auto invalid=item;invalid.stencilParameters["rows"]=0;QVERIFY(!rebuild(invalid));
        invalid=item;invalid.sourcePoints[1].setX(std::numeric_limits<qreal>::infinity());QVERIFY(!rebuild(invalid));
    }
    void configurableStencilParametersSurviveNativeReopenAndRotation_data() {
        QTest::addColumn<QString>("symbolId");QTest::addColumn<bool>("opaque");
        for(const auto &id:QStringList{"table","graph","bode"})for(bool opaque:{false,true})
            QTest::newRow(qPrintable(id+(opaque?"-opaque":"-transparent")))<<id<<opaque;
    }
    void configurableStencilParametersSurviveNativeReopenAndRotation() {
        QFETCH(QString,symbolId);QFETCH(bool,opaque);
        QTemporaryDir directory;NativeObjectBindings bindings(directory.path());
        Item item;item.kind="symbol";item.symbolId=symbolId;item.width=4.6;
        item.sourcePoints={{200,200},{440,200},{200,360}};
        item.stencilParameters={{"opaqueBackground",opaque}};
        if(symbolId=="table"){item.stencilParameters["rows"]=5;item.stencilParameters["columns"]=7;}
        else if(symbolId=="graph"){item.stencilParameters["curveType"]="exponential";item.stencilParameters["tau"]=2.5;}
        else {item.stencilParameters["curve"]=true;item.stencilParameters["damping"]=.7;}
        setForegroundColor(item,QColor(Qt::blue));QVERIFY(rebuildNativeObject(item));
        const auto parameters=item.stencilParameters;
        auto before=emptyPage();const auto ids=addItem(before,item);
        QVERIFY(bindings.registerInserted(item,before,ids));
        Item restored;QVERIFY(bindings.selectedModel(ids,&restored));
        QCOMPARE(restored.stencilParameters,parameters);QCOMPARE(restored.width,item.width);
        QCOMPARE(foregroundColor(restored),QColor(Qt::blue));
        const auto json=QJsonDocument::fromJson(readFile(bindings.storagePath())).object();
        QVERIFY(json["objects"].toArray().first().toObject()["model"].toObject()["stencilParameters"].isObject());
        QTransform rotation;rotation.translate(120,70);rotation.rotate(29);
        const auto moved=transformed(before,rotation);QVector<quint64> movedIds;
        for(const auto &line:moved.lines)movedIds.append(line.id);
        NativeObjectBindings reopened(directory.path());QVERIFY(reopened.observe(moved));
        QVERIFY(reopened.selectedModel(movedIds,&restored));
        QCOMPARE(restored.stencilParameters,parameters);QCOMPARE(foregroundColor(restored),QColor(Qt::blue));
        QCOMPARE(restored.width,item.width);
        for(qsizetype i=0;i<item.sourcePoints.size();++i)QVERIFY(close(restored.sourcePoints[i],rotation.map(item.sourcePoints[i])));
        if(opaque)QCOMPARE(restored.strokes.first().color,QColor(Qt::white));
        // Native recoloring may change the foreground, but the white mask must
        // still match independently before semantic controls are exposed.
        auto recolored=before;
        for(qsizetype i=0;i<recolored.lines.size();++i)if(!isBackgroundStroke(item,i))recolored.lines[i].stroke.color=Qt::red;
        QVERIFY(reopened.observe(recolored));QVERIFY(reopened.selectedModel(ids,&restored));
        QCOMPARE(foregroundColor(restored),QColor(Qt::red));
        if(opaque){
            recolored.lines.first().stroke.color=Qt::red;
            QVERIFY(reopened.observe(recolored));QVERIFY(!reopened.selectedModel(ids,&restored));
            QVERIFY(reopened.expandSelection(ids).valid);
        }
        auto flattened=before;
        for(auto &line:flattened.lines)line.stroke.width=item.width;
        QVERIFY(reopened.observe(flattened));QVERIFY(!reopened.selectedModel(ids,&restored));
        QVERIFY(reopened.observe(before));QVERIFY(reopened.selectedModel(ids,&restored));
    }
    void malformedStencilParametersPreserveExistingSidecar() {
        QTemporaryDir directory;NativeObjectBindings bindings(directory.path());
        Item item;item.kind="symbol";item.symbolId="table";item.sourcePoints={{200,200},{440,200},{200,360}};
        QVERIFY(rebuild(item));auto snapshot=emptyPage();const auto ids=addItem(snapshot,item);
        QVERIFY(bindings.registerInserted(item,snapshot,ids));const auto path=bindings.storagePath();
        auto json=QJsonDocument::fromJson(readFile(path)).object();auto objects=json["objects"].toArray();
        auto object=objects.first().toObject();auto model=object["model"].toObject();
        model["stencilParameters"]=QJsonObject{{"rows",0}};object["model"]=model;objects[0]=object;json["objects"]=objects;
        const auto malformed=QJsonDocument(json).toJson(QJsonDocument::Compact);QVERIFY(writeFile(path,malformed));
        NativeObjectBindings reopened(directory.path());QVERIFY(!reopened.observe(snapshot));QCOMPARE(readFile(path),malformed);
    }
    void duplicateCreatesIndependentBindingsAndSurvivesUndo() {
        QTemporaryDir directory; NativeObjectBindings bindings(directory.path());
        auto before = emptyPage(); const auto item = resistor(); const auto ids = addItem(before, item);
        QVERIFY(bindings.registerInserted(item, before, ids));
        auto after = before; auto copy = item;
        for (auto &stroke : copy.strokes) for (auto &point : stroke.points) point += QPointF(24, 24);
        const auto copyIds = addItem(after, copy, 500);
        auto shuffled = copyIds; std::reverse(shuffled.begin(), shuffled.end());
        QVERIFY(bindings.registerDuplicate(before, ids, after, shuffled));
        QCOMPARE(bindings.activeObjectCount(), 2);
        const auto selectedCopy = bindings.expandSelection({copyIds.first()});
        QVERIFY(selectedCopy.valid); QCOMPARE(selectedCopy.ids.size(), ids.size());
        for (auto id : copyIds) QVERIFY(selectedCopy.ids.contains(id));
        for (auto id : ids) QVERIFY(!selectedCopy.ids.contains(id));
        QCOMPARE(bindings.snapWirePoint(item.anchors.first() + QPointF(24, 24), 5).point,
                 item.anchors.first() + QPointF(24, 24));
        QVERIFY(bindings.observe(before)); QCOMPARE(bindings.activeObjectCount(), 1);
        QVERIFY(bindings.observe(after)); QCOMPARE(bindings.activeObjectCount(), 2);
        NativeObjectBindings reopened(directory.path());
        QVERIFY(reopened.observe(after)); QCOMPARE(reopened.activeObjectCount(), 2);
        auto invalidAfter = after; invalidAfter.lines.last().stroke.points.first() += QPointF(5, 0);
        QVERIFY(!bindings.registerDuplicate(before, ids, invalidAfter, copyIds));
    }
    void connectionsAndMultiSegmentRoutesSurviveReopen() {
        QTemporaryDir directory;NativeObjectBindings bindings(directory.path());auto snapshot=emptyPage();
        const auto component=resistor();const auto componentIds=addItem(snapshot,component);
        QVERIFY(bindings.registerInserted(component,snapshot,componentIds));
        auto trunk=wire(component.anchors.first(),{700,800});
        trunk.startAttachment={component.id,component.portIds.first()};
        trunk.wireRouteMode="manual";
        trunk.wireRoute={trunk.sourcePoints[0],{140,trunk.sourcePoints[0].y()},{140,450},{620,450},{620,800},trunk.sourcePoints[1]};
        QVERIFY(rebuild(trunk));const auto trunkIds=addItem(snapshot,trunk,500);
        QVERIFY2(bindings.registerInserted(trunk,snapshot,trunkIds),qPrintable(bindings.reason()));
        auto branch=wire({380,450},{380,950});branch.startAttachment={trunk.id,"route",.5};
        const auto branchIds=addItem(snapshot,branch,1000);
        QVERIFY(bindings.registerInserted(branch,snapshot,branchIds));
        NativeObjectBindings reopened(directory.path());QVERIFY(reopened.observe(snapshot));
        Item restored;QVERIFY(reopened.selectedModel(trunkIds,&restored));
        QCOMPARE(restored.id,trunk.id);QCOMPARE(restored.startAttachment.objectId,component.id);
        QCOMPARE(restored.startAttachment.portId,component.portIds.first());
        QCOMPARE(restored.wireRoute,trunk.wireRoute);QCOMPARE(restored.wireRouteMode,QString("manual"));
        QVERIFY(reopened.selectedModel(branchIds,&restored));QCOMPARE(restored.startAttachment.objectId,trunk.id);
        QCOMPARE(restored.startAttachment.portId,QString("route"));QCOMPARE(restored.startAttachment.wirePosition,.5);
        QCOMPARE(reopened.documentModels().size(),3);QCOMPARE(reopened.idsForObject(component.id),componentIds);
        const auto snap=reopened.snapWirePoint({400,455},10);
        QVERIFY(snap.snapped);QCOMPARE(snap.objectId,trunk.id);QCOMPARE(snap.portId,QString("route"));
        const qreal total=QLineF(trunk.wireRoute[0],trunk.wireRoute[1]).length()+
            QLineF(trunk.wireRoute[1],trunk.wireRoute[2]).length()+480+350+80;
        const qreal along=QLineF(trunk.wireRoute[0],trunk.wireRoute[1]).length()+
            QLineF(trunk.wireRoute[1],trunk.wireRoute[2]).length()+260;
        QVERIFY(std::abs(snap.wirePosition-along/total)<1e-9);
    }
    void batchKeepsLogicalConnectionsAndHistoryRevisions() {
        QTemporaryDir directory;NativeObjectBindings bindings(directory.path());auto before=emptyPage();
        auto component=resistor();const auto componentIds=addItem(before,component);
        QVERIFY(bindings.registerInserted(component,before,componentIds));
        auto connector=wire(component.anchors.first(),{700,800});
        connector.startAttachment={component.id,component.portIds.first()};
        const auto wireIds=addItem(before,connector,500);QVERIFY(bindings.registerInserted(connector,before,wireIds));
        const auto unrelated=addItem(before,wire({-100,-100},{-200,-200}),1500);
        auto moved=component;QTransform translate;translate.translate(50,25);QVERIFY(transform(moved,translate));
        auto rerouted=connector;rerouted.sourcePoints[0]=moved.anchors.first();QVERIFY(rebuild(rerouted));
        auto after=before;
        after.lines.erase(std::remove_if(after.lines.begin(),after.lines.end(),[&](const auto &line){
            return componentIds.contains(line.id)||wireIds.contains(line.id);}),after.lines.end());
        const auto newComponentIds=addItem(after,moved,2000),newWireIds=addItem(after,rerouted,2500);
        QVERIFY2(bindings.registerReplacementsBatch(before,{componentIds,wireIds},after,{newComponentIds,newWireIds},{moved,rerouted}),qPrintable(bindings.reason()));
        QCOMPARE(bindings.idsForObject(component.id),newComponentIds);QCOMPARE(bindings.idsForObject(connector.id),newWireIds);
        const auto stored=QJsonDocument::fromJson(readFile(bindings.storagePath())).object()["objects"].toArray();QCOMPARE(stored.size(),4);
        QSet<QString> revisions;for(const auto &entry:stored)revisions.insert(entry.toObject()["id"].toString());QCOMPARE(revisions.size(),4);
        NativeObjectBindings reopened(directory.path());
        for(const auto &state:{before,after,before,after}) {
            QVERIFY(reopened.observe(state));QCOMPARE(reopened.activeObjectCount(),2);
            const bool isAfter=state.lines.last().id==after.lines.last().id;
            const auto ids=isAfter?newWireIds:wireIds;Item model;QVERIFY(reopened.selectedModel(ids,&model));
            QCOMPARE(model.id,connector.id);QCOMPARE(model.startAttachment.objectId,component.id);
            QCOMPARE(reopened.idsForObject(component.id),isAfter?newComponentIds:componentIds);
            QVERIFY(reopened.expandSelection(unrelated).valid);
        }
        // Geometry alone cannot choose between two active revisions of one ID.
        auto ambiguous=after;
        for(const auto &line:before.lines)if(componentIds.contains(line.id)||wireIds.contains(line.id))ambiguous.lines.append(line);
        QVERIFY(reopened.observe(ambiguous));QVERIFY(reopened.documentModels().isEmpty());
        QVERIFY(reopened.idsForObject(component.id).isEmpty());QVERIFY(!reopened.expandSelection({newWireIds.first()}).valid);
    }
    void batchFailureNeverPersistsPartialBindings() {
        QTemporaryDir directory;NativeObjectBindings bindings(directory.path());auto before=emptyPage();
        auto a=resistor(),b=wire();const auto aIds=addItem(before,a),bIds=addItem(before,b,500);
        QVERIFY(bindings.registerInserted(a,before,aIds));QVERIFY(bindings.registerInserted(b,before,bIds));
        const auto unrelated=addItem(before,wire({-100,-100},{-200,-200}),1500);
        QTransform move;move.translate(50,25);QVERIFY(transform(a,move));QVERIFY(transform(b,move));
        auto after=emptyPage();after.lines.append(before.lines.last());
        const auto newA=addItem(after,a,2000),newB=addItem(after,b,2500);
        const auto bytes=readFile(bindings.storagePath());
        auto invalid=after;invalid.lines.last().stroke.points.last()+=QPointF(5,0);
        QVERIFY(!bindings.registerReplacementsBatch(before,{aIds,bIds},invalid,{newA,newB},{a,b}));
        QCOMPARE(readFile(bindings.storagePath()),bytes);QCOMPARE(bindings.idsForObject(a.id),aIds);
        invalid=after;invalid.lines.first().version+="-external";
        QVERIFY(!bindings.registerReplacementsBatch(before,{aIds,bIds},invalid,{newA,newB},{a,b}));
        QCOMPARE(readFile(bindings.storagePath()),bytes);
        QVERIFY(!bindings.registerReplacementsBatch(before,{aIds,aIds},after,{newA,newB},{a,b}));
        QVERIFY(!bindings.registerReplacementsBatch(before,{aIds,bIds},after,{newA,newA},{a,b}));
        QCOMPARE(readFile(bindings.storagePath()),bytes);QVERIFY(!unrelated.isEmpty());
        QVERIFY(bindings.registerReplacementsBatch(before,{aIds,bIds},after,{newA,newB},{a,b}));
    }
    void duplicatedGraphRemapsOnlyInternalConnections() {
        QTemporaryDir directory;NativeObjectBindings bindings(directory.path());auto before=emptyPage();
        const auto component=resistor();const auto componentIds=addItem(before,component);
        QVERIFY(bindings.registerInserted(component,before,componentIds));
        auto connector=wire(component.anchors.first(),{700,800});
        connector.startAttachment={component.id,component.portIds.first()};connector.endAttachment={"external-component","north"};
        const auto wireIds=addItem(before,connector,500);QVERIFY(bindings.registerInserted(connector,before,wireIds));
        auto copyComponent=component,copyWire=connector;QTransform translate;translate.translate(24,24);
        QVERIFY(transform(copyComponent,translate));QVERIFY(transform(copyWire,translate));
        auto after=before;const auto newComponent=addItem(after,copyComponent,2000),newWire=addItem(after,copyWire,2500);
        QVERIFY(bindings.registerDuplicate(before,componentIds+wireIds,after,newComponent+newWire));
        Item componentModel,wireModel;QVERIFY(bindings.selectedModel(newComponent,&componentModel));QVERIFY(bindings.selectedModel(newWire,&wireModel));
        QVERIFY(componentModel.id!=component.id);QVERIFY(wireModel.id!=connector.id);
        QCOMPARE(wireModel.startAttachment.objectId,componentModel.id);QVERIFY(wireModel.endAttachment.empty());
        NativeObjectBindings reopened(directory.path());QVERIFY(reopened.observe(after));QVERIFY(reopened.selectedModel(newWire,&wireModel));
        QCOMPARE(wireModel.startAttachment.objectId,componentModel.id);
    }
    void malformedConnectionsAndOldSchemaAreBounded() {
        QTemporaryDir directory;NativeObjectBindings bindings(directory.path());auto snapshot=emptyPage();
        auto connector=wire();connector.startAttachment={"component","west"};const auto ids=addItem(snapshot,connector);
        QVERIFY(bindings.registerInserted(connector,snapshot,ids));const auto path=bindings.storagePath();
        const auto original=QJsonDocument::fromJson(readFile(path)).object();
        for(const auto &ref:{QJsonObject{{"objectId",connector.id},{"portId","end"},{"wirePosition",-1}},
                             QJsonObject{{"objectId","component"},{"portId","route"},{"wirePosition",1.1}},
                             QJsonObject{{"objectId","component"},{"portId","west"},{"wirePosition",.5}}}) {
            auto json=original;auto entries=json["objects"].toArray();auto object=entries.first().toObject();auto model=object["model"].toObject();
            model["startAttachment"]=ref;object["model"]=model;entries[0]=object;json["objects"]=entries;
            const auto malformed=QJsonDocument(json).toJson(QJsonDocument::Compact);QVERIFY(writeFile(path,malformed));
            NativeObjectBindings reopened(directory.path());QVERIFY(!reopened.observe(snapshot));QCOMPARE(readFile(path),malformed);
        }
        auto legacy=original;legacy["schemaVersion"]=3;auto entries=legacy["objects"].toArray();auto object=entries.first().toObject();
        object.remove("logicalId");auto model=object["model"].toObject();
        for(const auto *key:{"startAttachment","endAttachment","wireRoute","wireRouteMode"})model.remove(key);
        object["model"]=model;entries[0]=object;legacy["objects"]=entries;
        QVERIFY(writeFile(path,QJsonDocument(legacy).toJson(QJsonDocument::Compact)));
        NativeObjectBindings reopened(directory.path());QVERIFY(reopened.observe(snapshot));Item restored;QVERIFY(reopened.selectedModel(ids,&restored));
        QCOMPARE(restored.id,connector.id);QVERIFY(restored.startAttachment.empty());QCOMPARE(restored.wireRouteMode,QString("auto"));
    }
    void malformedCachedRouteDoesNotChangeSavedInk() {
        QTemporaryDir directory;NativeObjectBindings bindings(directory.path());auto snapshot=emptyPage();
        auto connector=wire();connector.wireRouteMode="manual";connector.wireRoute={{100,500},{350,500},{350,800},{700,800}};
        QVERIFY(rebuild(connector));const auto ids=addItem(snapshot,connector);QVERIFY(bindings.registerInserted(connector,snapshot,ids));
        const auto path=bindings.storagePath();auto json=QJsonDocument::fromJson(readFile(path)).object();
        auto entries=json["objects"].toArray();auto object=entries.first().toObject();auto model=object["model"].toObject();
        auto points=model["wireRoute"].toArray();points[0]=QJsonArray{100,520};model["wireRoute"]=points;
        object["model"]=model;entries[0]=object;json["objects"]=entries;
        const auto malformed=QJsonDocument(json).toJson(QJsonDocument::Compact);QVERIFY(writeFile(path,malformed));
        NativeObjectBindings reopened(directory.path());QVERIFY(!reopened.observe(snapshot));QCOMPARE(readFile(path),malformed);
    }
    void wireSnapCannotCreateCycleThroughBranch() {
        QTemporaryDir directory;NativeObjectBindings bindings(directory.path());auto snapshot=emptyPage();
        const auto trunk=wire({100,500},{700,500});const auto trunkIds=addItem(snapshot,trunk);
        QVERIFY(bindings.registerInserted(trunk,snapshot,trunkIds));
        auto branch=wire({400,500},{400,900});branch.startAttachment={trunk.id,"route",.5};
        const auto branchIds=addItem(snapshot,branch,500);QVERIFY(bindings.registerInserted(branch,snapshot,branchIds));
        QVERIFY(bindings.snapWirePoint({400,700},10).snapped);
        QVERIFY(!bindings.snapWirePoint({400,700},10,trunk.id).snapped);
        auto endpoint=bindings.snapWirePoint({700,502},10,branch.id);
        QVERIFY(endpoint.snapped);QCOMPARE(endpoint.objectId,trunk.id);QCOMPARE(endpoint.portId,QString("end"));QCOMPARE(endpoint.wirePosition,qreal(-1));
    }
    void damagedConnectionTargetIsDistinctFromDeletion() {
        QTemporaryDir directory;NativeObjectBindings bindings(directory.path());auto before=emptyPage();
        const auto component=resistor();const auto componentIds=addItem(before,component);
        QVERIFY(bindings.registerInserted(component,before,componentIds));
        auto connector=wire(component.anchors.first(),{700,800});connector.startAttachment={component.id,component.portIds.first()};
        const auto wireIds=addItem(before,connector,500);QVERIFY(bindings.registerInserted(connector,before,wireIds));
        QVERIFY(bindings.connectionModelsReady());
        auto damaged=before;damaged.lines.first().stroke.points.first()+=QPointF(5,0);
        QVERIFY(bindings.observe(damaged));QVERIFY(!bindings.connectionModelsReady());
        QCOMPARE(bindings.documentModels().size(),1);QVERIFY(bindings.idsForObject(component.id).isEmpty());
        auto deleted=before;deleted.lines.erase(std::remove_if(deleted.lines.begin(),deleted.lines.end(),[&](const auto &line){
            return componentIds.contains(line.id);}),deleted.lines.end());
        QVERIFY(bindings.observe(deleted));QVERIFY(bindings.connectionModelsReady());
        // Old explicit geometry keeps its snap, but cannot claim an attachment
        // until the legacy model has been exactly reconstructed.
        auto json=QJsonDocument::fromJson(readFile(bindings.storagePath())).object();auto entries=json["objects"].toArray();
        auto legacy=entries[0].toObject();legacy.remove("model");entries[0]=legacy;json["objects"]=entries;
        QVERIFY(writeFile(bindings.storagePath(),QJsonDocument(json).toJson(QJsonDocument::Compact)));
        NativeObjectBindings reopened(directory.path());QVERIFY(reopened.observe(before));QVERIFY(!reopened.connectionModelsReady());
        const auto snap=reopened.snapWirePoint(component.anchors.last(),5);QVERIFY(snap.snapped);
        QVERIFY(snap.objectId.isEmpty());QVERIFY(snap.portId.isEmpty());QCOMPARE(snap.wirePosition,qreal(-1));
    }
};
QTEST_GUILESS_MAIN(NativeObjectBindingsTest)
#include "NativeObjectBindingsTest.moc"
