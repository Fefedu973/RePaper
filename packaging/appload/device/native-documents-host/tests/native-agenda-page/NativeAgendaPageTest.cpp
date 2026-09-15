#include "NativeAgendaPage.h"
#include "NativeObjectAccess.h"
#include <QtTest>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QRawFont>
#include <QTransform>
#include <algorithm>

using RePaperNative::NativeObjectSnapshot;
namespace {
const QRectF paper(-936, 0, 1872, 1404);
QVariantMap fields(QString title = QStringLiteral("Électricité — Révision des circuits")) {
    return {{"title", title}, {"day", QStringLiteral("Lundi")}, {"date", "07 / 09 / 2026"}, {"time", "09:00 – 10:30"}};
}
QVariantMap context() { return {{"documentId", "doc-a"}, {"pageId", "page-a"}, {"layer", 0}}; }
QByteArray fontData() {
    QFile font(QString::fromLocal8Bit(qgetenv("REPAPER_AGENDA_TEST_FONT")));
    return font.open(QIODevice::ReadOnly) ? font.readAll() : QByteArray{};
}
NativeObjectSnapshot emptySnapshot() {
    NativeObjectSnapshot snapshot;
    snapshot.complete = snapshot.pendingEditIdentity = snapshot.nativeSelectionExact = true;
    snapshot.documentId = "doc-a"; snapshot.pageId = "page-a"; snapshot.layer = 0;
    snapshot.layerId = 100; snapshot.sceneIdentity = 1000; snapshot.fingerprint = "empty";
    snapshot.history.valid = snapshot.history.appendIsolated = true; snapshot.history.historyIdentity = 10000;
    return snapshot;
}
NativeObjectSnapshot inkSnapshot(const NativeAgendaLayout::Plan &plan) {
    auto snapshot = emptySnapshot();
    snapshot.fingerprint = "prefilled";
    quint64 id = 500;
    for (const auto &stroke : plan.strokes) {
        RePaperNative::NativeObjectLine line;
        line.id = ++id; line.parentId = snapshot.layerId; line.lineageId = id;
        line.tool = 19; line.stroke = stroke; line.uniformPointWidth = true;
        line.maximumPointWidth = qRound(stroke.width * 4);
        line.version = line.contentVersion = QByteArray::number(id);
        snapshot.lines.append(line);
    }
    return snapshot;
}
QObject *controller(QObject *parent) {
    auto *result = new QObject(parent);
    result->setProperty("pageId", "page-a"); result->setProperty("currentLayer", 0);
    result->setProperty("working", false); result->setProperty("rootDocumentLength", 0);
    result->setProperty("hasAnnotations", false); result->setProperty("paperNoteBounds", paper);
    result->setProperty("pendingEdit", QTransform{});
    return result;
}
class FakeAccess : public NativeObjectAccess {
public:
    using NativeObjectAccess::NativeObjectAccess;
    int inspections = 0, insertions = 0, cancels = 0;
    bool active = false, accepted = true;
    NativeObjectSnapshot snapshot = emptySnapshot(), expected;
    QVector<quint64> inserted;
    QString error;
    bool inspect(QObject *, const QVariantMap &) override { ++inspections; active = accepted; return accepted; }
    bool insertIfUnchanged(QObject *, const QVariantMap &, const NativeObjectSnapshot &baseline,
                           const QVariant &, QPointF) override {
        ++insertions; expected = baseline;
        error = RePaperNative::ObjectAccessDetail::insertionBaselineError(baseline, snapshot);
        active = accepted && error.isEmpty(); return active;
    }
    bool busy() const override { return active; }
    QString reason() const override { return error; }
    NativeObjectSnapshot result() const override { return snapshot; }
    QVector<quint64> insertedIds() const override { return inserted; }
    void cancel() override { ++cancels; active = false; }
    void finish(bool success = true) { active = false; emit finished(success); }
};
class Page : public NativeAgendaPage {
public:
    Page(FakeAccess *access, QObject *parent = nullptr) : NativeAgendaPage(access, fontData(), parent) {}
    QVariant nativeItems(const NativeAgendaLayout::Plan &) override { return 1; }
};
}

class NativeAgendaPageTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { QVERIFY2(!fontData().isEmpty(), "REPAPER_AGENDA_TEST_FONT must point to the privately cached native font"); }
    void layoutFieldsAndNativeOrigin() {
        const auto plan = NativeAgendaLayout::create(fields(), paper, fontData());
        QVERIFY2(plan.valid(), qPrintable(plan.reason));
        QVERIFY(plan.strokes.size() <= 128); QCOMPARE(plan.hash.size(), 64);
        QCOMPARE(plan.displayedTitle, fields()["title"].toString());
        QVERIFY(paper.contains(plan.inkBounds));
        const auto title = plan.fieldBounds["title"].toRectF().translated(-paper.topLeft());
        const auto time = plan.fieldBounds["time"].toRectF().translated(-paper.topLeft());
        QVERIFY(title.left() >= 200 && title.top() > 245 && title.bottom() < 379);
        QVERIFY(time.left() >= 0 && time.right() < 186 && time.top() > 245 && time.bottom() < 379);
        QVERIFY(plan.fieldBounds["dateDay"].toRectF().right() < paper.left() + paper.width() / 2 + 820);
        QVERIFY(plan.fieldBounds["dateMonth"].toRectF().right() < paper.left() + paper.width() / 2 + 865);
        QCOMPARE(plan.hash, NativeAgendaLayout::create(fields(), paper, fontData()).hash);
    }
    void longTitleEllipsizedWithinNativeBudget() {
        const QString full = QString(512, QChar('B'));
        QElapsedTimer timer; timer.start();
        const auto plan = NativeAgendaLayout::create(fields(full), paper, fontData());
        qInfo() << "longTitle512Milliseconds" << timer.elapsed();
        QVERIFY2(plan.valid(), qPrintable(plan.reason));
        QVERIFY(plan.strokes.size() <= 128); QVERIFY(plan.displayedTitle.endsWith(QChar(0x2026)));
        QVERIFY(plan.displayedTitle.size() < full.size());
        const auto changed = NativeAgendaLayout::create(fields(full.left(511) + "C"), paper, fontData());
        QVERIFY(changed.hash != plan.hash); // full title remains part of the journal's plan identity
    }
    void allDayAndCrossDayTimesFit() {
        for (const auto &time : {QStringLiteral("Toute la journée"), QStringLiteral("09:00 – 08/09 10:30"), QString{}}) {
            auto input = fields(); input["time"] = time;
            const auto plan = NativeAgendaLayout::create(input, paper, fontData());
            QVERIFY2(plan.valid(), qPrintable(plan.reason));
        }
    }
    void invalidInputsRefused() {
        QVERIFY(!NativeAgendaLayout::create(fields(), QRectF(0, 0, 1404, 1872), fontData()).valid());
        QVERIFY(!NativeAgendaLayout::create(fields(), paper, {}).valid());
        auto input = fields(); input["date"] = "invalid";
        QVERIFY(!NativeAgendaLayout::create(input, paper, fontData()).valid());
    }
    void exactInkRecoveryRejectsChanges() {
        const auto plan = NativeAgendaLayout::create(fields(), paper, fontData());
        auto snapshot = inkSnapshot(plan);
        QVERIFY(NativeAgendaLayout::matches(plan, snapshot));
        std::reverse(snapshot.lines.begin(), snapshot.lines.end());
        QVERIFY(NativeAgendaLayout::matches(plan, snapshot));
        snapshot.lines[0].stroke.points[0] += QPointF(1, 0);
        QVERIFY(!NativeAgendaLayout::matches(plan, snapshot));
        snapshot = inkSnapshot(plan); snapshot.lines[0].tool = 4;
        QVERIFY(!NativeAgendaLayout::matches(plan, snapshot));
    }
    void prepareNeedsExplicitCommit() {
        auto *access = new FakeAccess; Page page(access); auto *scene = controller(&page);
        QSignalSpy prepared(&page, &Page::prepared), done(&page, &Page::finished);
        QVERIFY(page.prepare("request", scene, context(), fields(), QSizeF(1620, 2160)));
        access->finish(); QCOMPARE(prepared.size(), 1); QVERIFY(page.busy()); QCOMPARE(access->insertions, 0);
        QCOMPARE(prepared[0][1].toMap()["state"].toString(), "empty");
        QVERIFY(page.commit("request")); QCOMPARE(access->insertions, 1);
        const auto plan = NativeAgendaLayout::create(fields(), paper, fontData());
        access->snapshot = inkSnapshot(plan);
        for (const auto &line : access->snapshot.lines) access->inserted.append(line.id);
        scene->setProperty("hasAnnotations", true); access->finish();
        QCOMPARE(done.size(), 1); QCOMPARE(done[0][1].toBool(), true); QVERIFY(!page.busy());
        QCOMPARE(done[0][2].toMap()["state"].toString(), "inserted");
    }
    void alreadyPresentRecheckedWithoutInsertion() {
        auto *access = new FakeAccess; Page page(access); auto *scene = controller(&page);
        access->snapshot = inkSnapshot(NativeAgendaLayout::create(fields(), paper, fontData()));
        scene->setProperty("hasAnnotations", true);
        QSignalSpy prepared(&page, &Page::prepared), done(&page, &Page::finished);
        QVERIFY(page.prepare("request", scene, context(), fields(), QSizeF(1620, 2160)));
        access->finish(); QCOMPARE(prepared[0][1].toMap()["state"].toString(), "already-present");
        QVERIFY(page.commit("request")); QCOMPARE(access->inspections, 2); QCOMPARE(access->insertions, 0);
        access->finish(); QCOMPARE(done.size(), 1); QVERIFY(done[0][1].toBool()); QVERIFY(!page.busy());
    }
    void foreignInkAndUnsupportedObjectsRefused() {
        for (bool unsupported : {false, true}) {
            auto *access = new FakeAccess; Page page(access); auto *scene = controller(&page);
            if (unsupported) access->snapshot.unsupportedItemCount = 1;
            else { access->snapshot = inkSnapshot(NativeAgendaLayout::create(fields(), paper, fontData())); access->snapshot.lines.removeLast(); }
            QSignalSpy done(&page, &Page::finished);
            QVERIFY(page.prepare("request", scene, context(), fields(), QSizeF(1620, 2160)));
            access->finish(); QCOMPARE(done.size(), 1); QVERIFY(!done[0][1].toBool()); QCOMPARE(access->insertions, 0);
        }
    }
    void textBeforePrepareRefused() {
        auto *access = new FakeAccess; Page page(access); auto *scene = controller(&page);
        scene->setProperty("rootDocumentLength", 5);
        QVERIFY(!page.prepare("request", scene, context(), fields(), QSizeF(1620, 2160)));
        QCOMPARE(access->inspections, 0);
    }
    void pageSwitchAndTextBeforeCommitRefused() {
        for (bool text : {false, true}) {
            auto *access = new FakeAccess; Page page(access); auto *scene = controller(&page);
            QVERIFY(page.prepare("request", scene, context(), fields(), QSizeF(1620, 2160))); access->finish();
            if (text) scene->setProperty("rootDocumentLength", 5); else scene->setProperty("pageId", "page-b");
            QVERIFY(!page.commit("request")); QCOMPARE(access->insertions, 0); QVERIFY(!page.busy());
        }
    }
    void externalHistoryChangeRefusedAtCommit() {
        auto *access = new FakeAccess; Page page(access); auto *scene = controller(&page);
        QVERIFY(page.prepare("request", scene, context(), fields(), QSizeF(1620, 2160))); access->finish();
        RePaperNative::NativeHistoryCommand edit; edit.identity = 20;
        access->snapshot.history.undo.append(edit);
        QVERIFY(!page.commit("request")); QCOMPARE(page.reason(), "insertion-history-changed");
    }
    void cancelledRequestCannotCommit() {
        auto *access = new FakeAccess; Page page(access); auto *scene = controller(&page);
        QSignalSpy prepared(&page, &Page::prepared);
        QVERIFY(page.prepare("request", scene, context(), fields(), QSizeF(1620, 2160)));
        page.cancel("other"); QVERIFY(page.busy()); page.cancel("request"); QVERIFY(!page.busy());
        access->finish(); QCOMPARE(prepared.size(), 0); QVERIFY(!page.commit("request")); QCOMPARE(access->insertions, 0);
    }
    void nativeWorkingCompletionCanUnwind() {
        auto *access = new FakeAccess; Page page(access); auto *scene = controller(&page);
        QSignalSpy prepared(&page, &Page::prepared);
        QVERIFY(page.prepare("request", scene, context(), fields(), QSizeF(1620, 2160)));
        scene->setProperty("working", true); access->finish(); QCOMPARE(prepared.size(), 0);
        scene->setProperty("working", false); QTRY_COMPARE(prepared.size(), 1);
        page.cancel("request");
    }
    void cancelledWorkingCompletionCannotConsumeNewRequest() {
        auto *access = new FakeAccess; Page page(access); auto *scene = controller(&page);
        QSignalSpy prepared(&page, &Page::prepared);
        QVERIFY(page.prepare("request", scene, context(), fields(), QSizeF(1620, 2160)));
        scene->setProperty("working", true); access->finish(); page.cancel("request");
        scene->setProperty("working", false);
        QVERIFY(page.prepare("request", scene, context(), fields(), QSizeF(1620, 2160)));
        QTest::qWait(60); QCOMPARE(prepared.size(), 0);
        access->finish(); QCOMPARE(prepared.size(), 1);
    }
    void nativeAllocatorRemainsGatedOnForeignProcess() {
        NativeAgendaPage page;
        auto *scene = controller(&page);
        // The public production path has no native font on this foreign host.
        QVERIFY(!page.prepare("request", scene, context(), fields(), QSizeF(1620, 2160)));
        QVERIFY(!page.busy());
    }
    void visualPreview() {
        const auto destination = QString::fromLocal8Bit(qgetenv("REPAPER_AGENDA_TEST_PREVIEW"));
        if (destination.isEmpty()) QSKIP("Preview path not requested");
        const auto input = fields(); const auto plan = NativeAgendaLayout::create(input, paper, fontData());
        QVERIFY(plan.valid());
        QImage image(1872, 1404, QImage::Format_RGB32); image.fill(Qt::white);
        QPainter painter(&image); painter.setRenderHint(QPainter::Antialiasing);
        const auto label = [&](const QString &text, qreal size, QPointF position) {
            QRawFont font(fontData(), size, QFont::PreferNoHinting);
            const auto glyphs = font.glyphIndexesForString(text); const auto advances = font.advancesForGlyphIndexes(glyphs);
            for (qsizetype i = 0; i < glyphs.size(); ++i) { painter.fillPath(font.pathForGlyph(glyphs[i]).translated(position), Qt::black); position += advances[i]; }
        };
        label("Day:", 72, QPointF(346,95)); label("TIME",32,QPointF(28,205));
        label("DATE",24,QPointF(1656,205)); label("/",24,QPointF(1756,205)); label("/",24,QPointF(1801,205));
        painter.setPen(QPen(Qt::black,1)); painter.drawLine(QPointF(0,145),QPointF(1872,145));
        painter.drawLine(QPointF(0,245),QPointF(1872,245));
        for (qreal y=379;y<1404;y+=116.5) painter.drawLine(QPointF(186,y),QPointF(1872,y));
        painter.translate(-paper.topLeft());
        for (const auto &stroke : plan.strokes) {
            painter.setPen(QPen(stroke.color, stroke.width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.drawPolyline(stroke.points);
        }
        painter.end(); QVERIFY(image.save(destination));
    }
};
QTEST_MAIN(NativeAgendaPageTest)
#include "NativeAgendaPageTest.moc"
