#include "NativeAgendaHeader.h"
#include "NativeObjectAccess.h"
#include <QtTest>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPainterPath>
#include <QRawFont>
#include <QTemporaryDir>
#include <QTransform>
#include <stdexcept>

using RePaperNative::NativeObjectSnapshot;
namespace {
const QRectF paper(-810, 0, 1620, 2160);
QVariantMap fields() {
    return {{"day", "Mercredi"}, {"date", "16 / 09 / 2026"}, {"time", "08:00 – 12:15"},
            {"title", "This remains native editable text"}};
}
QVariantMap context() { return {{"documentId", "doc-a"}, {"pageId", "page-a"}, {"layer", 0}}; }
QByteArray read(const QString &path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; }
bool write(const QString &path, const QByteArray &bytes) {
    QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray fontData() { return read(QString::fromLocal8Bit(qgetenv("REPAPER_AGENDA_TEST_FONT"))); }
// Original synthetic structure; no firmware template or font is distributed.
struct Environment {
    QTemporaryDir temp;
    QString source = temp.filePath("source.template"), output = temp.filePath("library");
    QByteArray bytes = QJsonDocument(QJsonObject{{"orientation", "portrait"}, {"items", QJsonArray{
        QJsonObject{{"children", QJsonArray{QJsonObject{{"text", "Day:"}}, QJsonObject{{"text", "TIME"}},
            QJsonObject{{"id", "group-date-item"}}, QJsonObject{{"id", "two-h-lines"}}}}}}}}).toJson();
    Environment() {
        if (!temp.isValid() || !write(source, bytes) || !QDir().mkdir(output)) throw std::runtime_error("Temporary fixture unavailable");
    }
    QByteArray hash() const { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex(); }
};
NativeObjectSnapshot emptySnapshot() {
    NativeObjectSnapshot s;
    s.complete = s.pendingEditIdentity = s.nativeSelectionExact = true;
    s.documentId = "doc-a"; s.pageId = "page-a"; s.layer = 0; s.layerId = 100; s.sceneIdentity = 1000;
    s.fingerprint = "synthetic-native-snapshot";
    s.history.valid = s.history.appendIsolated = true; s.history.historyIdentity = 10000;
    return s;
}
void appendInk(NativeObjectSnapshot &snapshot, const QVector<PaperDrawing::Stroke> &strokes) {
    quint64 id = snapshot.lines.isEmpty() ? 500 : snapshot.lines.last().id;
    for (const auto &stroke : strokes) {
        RePaperNative::NativeObjectLine line;
        line.id = ++id; line.parentId = snapshot.layerId; line.lineageId = id; line.tool = 19;
        line.stroke = stroke; line.uniformPointWidth = true; line.maximumPointWidth = qRound(stroke.width * 4);
        line.version = line.contentVersion = QByteArray::number(id); snapshot.lines.append(line);
    }
}
NativeObjectSnapshot inkSnapshot(const NativeAgendaHeader::Plan &plan) {
    auto snapshot = emptySnapshot(); appendInk(snapshot, plan.strokes); return snapshot;
}
PaperDrawing::Stroke unrelatedInk(qreal y = 450) {
    PaperDrawing::Stroke stroke; stroke.width = 2; stroke.color = Qt::black;
    stroke.points = {{-100, y}, {100, y + 10}}; return stroke;
}
QObject *controller(QObject *parent) {
    auto *result = new QObject(parent), *worker = new QObject(result);
    worker->setProperty("jobQueueSize", 0); result->setProperty("worker", QVariant::fromValue(worker));
    result->setProperty("pageId", "page-a"); result->setProperty("currentLayer", 0);
    result->setProperty("working", false); result->setProperty("rootDocumentLength", 25);
    result->setProperty("hasTextSelection", false); result->setProperty("selectionItemCount", 0);
    result->setProperty("paperPortraitBounds", paper); result->setProperty("pendingEdit", QTransform{}); return result;
}
class FakeAccess : public NativeObjectAccess {
public:
    bool active = false;
    int inspections = 0, insertions = 0, cancels = 0;
    NativeObjectSnapshot snapshot = emptySnapshot();
    QVector<quint64> inserted;
    QString error;
    bool inspect(QObject *, const QVariantMap &) override { ++inspections; active = true; return true; }
    bool insertIfUnchanged(QObject *, const QVariantMap &, const NativeObjectSnapshot &baseline,
                           const QVariant &, QPointF) override {
        error = RePaperNative::ObjectAccessDetail::insertionBaselineError(baseline, snapshot);
        if (!error.isEmpty()) return false;
        ++insertions; active = true; return true;
    }
    bool busy() const override { return active; }
    QString reason() const override { return error; }
    NativeObjectSnapshot result() const override { return snapshot; }
    QVector<quint64> insertedIds() const override { return inserted; }
    void cancel() override { ++cancels; active = false; }
    void finish(bool success = true) { active = false; emit finished(success); }
};
class Header : public NativeAgendaHeader {
public:
    Header(FakeAccess *access, const Environment &env) : NativeAgendaHeader(access, fontData(), env.source, env.hash()) {}
    QVariant nativeItems(const Plan &) override { return 1; }
};
}

class NativeAgendaHeaderTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { QVERIFY2(!fontData().isEmpty(), "REPAPER_AGENDA_TEST_FONT must point to the privately cached native font"); }
    void sourceAndLibraryAreNeverWritten() {
        Environment env; NativeAgendaHeader header(env.source, env.output, env.hash());
        QVERIFY(write(QDir(env.output).filePath("owned.metadata"), "original library entry"));
        const auto files = QDir(env.temp.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
        const auto result = header.ensure(fields()); QVERIFY(!result.contains("error"));
        QCOMPARE(result["templateName"].toString(), "P Day"); QCOMPARE(result["storage"].toString(), "native-ink");
        QVERIFY(!result.contains("templateId")); QCOMPARE(read(env.source), env.bytes);
        QCOMPARE(QDir(env.output).entryList(QDir::Files), QStringList{"owned.metadata"});
        QCOMPARE(read(QDir(env.output).filePath("owned.metadata")), "original library entry");
        QCOMPARE(QDir(env.temp.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot), files);
        NativeAgendaHeader absentOutput(env.source, env.output + "-absent", env.hash());
        QVERIFY(!absentOutput.ensure(fields()).contains("error")); QVERIFY(!QFileInfo::exists(env.output + "-absent"));
    }
    void nativeFieldsFitTheirDesignatedRegions() {
        const auto plan = NativeAgendaHeader::createPlan(fields(), paper, fontData());
        QVERIFY2(plan.valid(), qPrintable(plan.reason)); QCOMPARE(plan.hash.size(), 64); QVERIFY(plan.strokes.size() <= 128);
        for (auto it = plan.fieldBounds.begin(); it != plan.fieldBounds.end(); ++it)
            QVERIFY(plan.fieldRegions[it.key()].toRectF().contains(it.value().toRectF()));
        QVERIFY(plan.fieldBounds["day"].toRectF().bottom() < 145);
        QVERIFY(plan.fieldBounds["time"].toRectF().right() < 490);
        QVERIFY(plan.fieldBounds["dateDay"].toRectF().right() < 590);
        QVERIFY(plan.fieldBounds["dateMonth"].toRectF().right() < 635);
        QVERIFY(plan.inkBounds.bottom() < 245);
        auto normalized = fields(); normalized["day"] = " Mercredi "; normalized["date"] = "16/09/2026";
        normalized["time"] = "08:00–12:15"; normalized["title"] = "Different title";
        QCOMPARE(NativeAgendaHeader::createPlan(normalized, paper, fontData()).hash, plan.hash);
        normalized["time"] = "09:00–12:15";
        QVERIFY(NativeAgendaHeader::createPlan(normalized, paper, fontData()).hash != plan.hash);
        QVERIFY(NativeAgendaHeader::createPlan(fields(), {-702, 0, 1404, 1872}, fontData()).valid());
    }
    void invalidFieldsAndSourcesFailWithoutWriting() {
        Environment env; NativeAgendaHeader header(env.source, env.output, env.hash());
        for (const auto &pair : QList<QPair<QString, QVariant>>{{"day", QString(17, 'x')}, {"date", "30/02/2026"},
                 {"date", 123}, {"time", "24:00–25:00"}, {"time", "08:60–09:00"}}) {
            auto input = fields(); input[pair.first] = pair.second;
            QCOMPARE(header.ensure(input)["error"].toString(), "agenda-header-invalid-fields");
        }
        QVERIFY(write(env.source, env.bytes + "\n"));
        QCOMPARE(header.ensure(fields())["error"].toString(), "agenda-header-template-unsupported");
        QVERIFY(QDir(env.output).entryList(QDir::Files).isEmpty());
        QVERIFY(!NativeAgendaHeader::createPlan(fields(), {0, 0, 1620, 2160}, fontData()).valid());
        QVERIFY(!NativeAgendaHeader::createPlan(fields(), paper, {}).valid());
    }
    void titleAndUnrelatedInkArePreservedByOneCommit() {
        Environment env; auto *access = new FakeAccess; Header header(access, env); auto *scene = controller(&header);
        appendInk(access->snapshot, {unrelatedInk()}); const auto before = access->snapshot;
        QSignalSpy prepared(&header, &Header::prepared), done(&header, &Header::finished);
        QVERIFY(header.prepare("one", scene, context(), fields(), {1620, 2160})); access->finish();
        QCOMPARE(prepared.size(), 1); QCOMPARE(prepared[0][1].toMap()["state"].toString(), "empty");
        QCOMPARE(access->insertions, 0); QVERIFY(header.commit("one")); QCOMPARE(access->insertions, 1);
        const auto plan = NativeAgendaHeader::createPlan(fields(), paper, fontData()); appendInk(access->snapshot, plan.strokes);
        for (int i = before.lines.size(); i < access->snapshot.lines.size(); ++i) access->inserted.append(access->snapshot.lines[i].id);
        access->finish(); QCOMPARE(done.size(), 1); QVERIFY(done[0][1].toBool());
        QCOMPARE(scene->property("rootDocumentLength").toInt(), 25);
        QCOMPARE(access->snapshot.lines[0].version, before.lines[0].version); QVERIFY(!header.busy());
    }
    void exactHeaderRetryNeverInsertsAgain() {
        Environment env; auto *access = new FakeAccess; Header header(access, env); auto *scene = controller(&header);
        access->snapshot = inkSnapshot(NativeAgendaHeader::createPlan(fields(), paper, fontData()));
        appendInk(access->snapshot, {unrelatedInk()});
        QSignalSpy prepared(&header, &Header::prepared), done(&header, &Header::finished);
        QVERIFY(header.prepare("one", scene, context(), fields(), {1620, 2160})); access->finish();
        QCOMPARE(prepared.size(), 1);
        QCOMPARE(prepared[0][1].toMap()["state"].toString(), "already-present");
        QVERIFY(header.commit("one")); access->finish(); QCOMPARE(done.size(), 1); QVERIFY(done[0][1].toBool());
        QCOMPARE(access->insertions, 0); QCOMPARE(access->inspections, 2);
    }
    void occupiedPartialOrUnsupportedInkIsRejected_data() {
        QTest::addColumn<int>("kind"); QTest::newRow("occupied") << 0; QTest::newRow("partial") << 1;
        QTest::newRow("unknown-object") << 2; QTest::newRow("modified-contour") << 3;
    }
    void occupiedPartialOrUnsupportedInkIsRejected() {
        QFETCH(int, kind); Environment env; auto *access = new FakeAccess; Header header(access, env); auto *scene = controller(&header);
        if (kind == 0) appendInk(access->snapshot, {unrelatedInk(60)});
        else if (kind == 2) access->snapshot.unsupportedItemCount = 1;
        else {
            access->snapshot = inkSnapshot(NativeAgendaHeader::createPlan(fields(), paper, fontData()));
            if (kind == 1) access->snapshot.lines.removeLast(); else access->snapshot.lines[0].stroke.points[0] += QPointF(1, 0);
        }
        QSignalSpy done(&header, &Header::finished);
        QVERIFY(header.prepare("one", scene, context(), fields(), {1620, 2160})); access->finish();
        QCOMPARE(done.size(), 1); QVERIFY(!done[0][1].toBool()); QCOMPARE(access->insertions, 0); QVERIFY(!header.busy());
    }
    void changedPageTextAndHistoryCannotCommit_data() {
        QTest::addColumn<int>("kind"); QTest::newRow("page") << 0; QTest::newRow("text") << 1; QTest::newRow("history") << 2;
    }
    void changedPageTextAndHistoryCannotCommit() {
        QFETCH(int, kind); Environment env; auto *access = new FakeAccess; Header header(access, env); auto *scene = controller(&header);
        QVERIFY(header.prepare("one", scene, context(), fields(), {1620, 2160})); access->finish();
        if (kind == 0) scene->setProperty("pageId", "page-b");
        else if (kind == 1) scene->setProperty("rootDocumentLength", 30);
        else { RePaperNative::NativeHistoryCommand edit; edit.identity = 20; access->snapshot.history.undo.append(edit); }
        QVERIFY(!header.commit("one")); QCOMPARE(access->insertions, 0); QVERIFY(!header.busy());
    }
    void cancellationFencesLateWorkerCompletion() {
        Environment env; auto *access = new FakeAccess; Header header(access, env); auto *scene = controller(&header);
        QSignalSpy prepared(&header, &Header::prepared);
        QVERIFY(header.prepare("one", scene, context(), fields(), {1620, 2160}));
        scene->setProperty("working", true); access->finish(); header.cancel("one"); scene->setProperty("working", false);
        QVERIFY(header.prepare("two", scene, context(), fields(), {1620, 2160}));
        QTest::qWait(60); QCOMPARE(prepared.size(), 0); access->finish(); QCOMPARE(prepared.size(), 1);
        header.cancel("two"); QCOMPARE(access->insertions, 0); QVERIFY(!header.busy());
    }
    void visualPreview() {
        const auto output = QString::fromLocal8Bit(qgetenv("REPAPER_AGENDA_TEST_PREVIEW"));
        if (output.isEmpty()) QSKIP("No local preview requested");
        const auto plan = NativeAgendaHeader::createPlan(fields(), paper, fontData()); QVERIFY(plan.valid());
        QImage image(1620, 550, QImage::Format_RGB32); image.fill(Qt::white); QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing); painter.translate(810, 0);
        const auto label = [&](QString text, qreal size, QPointF position) {
            QRawFont font(fontData(), size, QFont::PreferNoHinting);
            const auto glyphs = font.glyphIndexesForString(text); const auto advances = font.advancesForGlyphIndexes(glyphs);
            for (int i = 0; i < glyphs.size(); ++i) { painter.fillPath(font.pathForGlyph(glyphs[i]).translated(position), Qt::black); position += advances[i]; }
        };
        label("Day:", 72, {-355, 95}); label("TIME", 32, {-660, 205}); label("DATE", 24, {490, 205});
        label("/", 24, {590, 205}); label("/", 24, {635, 205});
        painter.setPen(QPen(Qt::black, 1)); painter.drawLine(QPointF(-810, 145), QPointF(810, 145));
        painter.drawLine(QPointF(-810, 245), QPointF(810, 245)); painter.drawLine(QPointF(-624, 379), QPointF(810, 379));
        for (const auto &stroke : plan.strokes) {
            painter.setPen(QPen(stroke.color, stroke.width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)); painter.drawPolyline(stroke.points);
        }
        painter.end(); QVERIFY(image.save(output));
    }
};
QTEST_MAIN(NativeAgendaHeaderTest)
#include "NativeAgendaHeaderTest.moc"
