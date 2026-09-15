#include "AgendaController.h"
#include "KeyboardController.h"
#include "ClipboardBridge.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

class SyntheticAgendaData {
    QByteArray previousDataHome=qgetenv("XDG_DATA_HOME");
    bool previousTestMode=QStandardPaths::isTestModeEnabled();
public:
    explicit SyntheticAgendaData(const QString &directory){
        // Qt test mode otherwise redirects XDG_DATA_HOME to ~/.qttest. Keep
        // both the calendar cache and its empty vault inside this test's temp dir.
        qputenv("XDG_DATA_HOME",directory.toUtf8());QStandardPaths::setTestModeEnabled(false);
    }
    ~SyntheticAgendaData(){
        if(previousDataHome.isNull())qunsetenv("XDG_DATA_HOME");else qputenv("XDG_DATA_HOME",previousDataHome);
        QStandardPaths::setTestModeEnabled(previousTestMode);
    }
};

class AgendaClipboard:public repaper::ClipboardBridge {
public:
    int calls=0;QString value;
    void request() override{++calls;emit ready(value);}
};
class AgendaUiTest:public QObject {
    Q_OBJECT
    static QQuickItem *visibleText(QQuickItem *root,const QString &text){
        if(!root->isVisible())return nullptr;
        if(root->property("text").toString().contains(text))return root;
        for(auto child:root->childItems())if(auto found=visibleText(child,text))return found;
        return nullptr;
    }
private slots:
    void initTestCase(){
        QCoreApplication::setOrganizationName("RePaperTests");QCoreApplication::setApplicationName("reagenda-ui-tests");QStandardPaths::setTestModeEnabled(true);
        QQuickStyle::setStyle("Basic");repaper::registerKeyboardTypes();
    }
    void focusedDialogFieldsRemainVisibleAndPasteWorks_data(){QTest::addColumn<QSize>("size");QTest::newRow("emulator")<<QSize(936,1248);QTest::newRow("compact")<<QSize(800,800);}
    void focusedDialogFieldsRemainVisibleAndPasteWorks(){
        QFETCH(QSize,size);AgendaController agenda;QQmlApplicationEngine engine;QSignalSpy warnings(&engine,&QQmlEngine::warnings);
        engine.rootContext()->setContextProperty("agenda",&agenda);engine.rootContext()->setContextProperty("previewPage",QString());engine.load(QUrl("qrc:/reagenda/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());auto window=qobject_cast<QQuickWindow *>(engine.rootObjects().first());QVERIFY(window);window->resize(size);QVERIFY(QTest::qWaitForWindowExposed(window));
        auto keyboard=window->findChild<repaper::KeyboardController *>("textKeyboard");QVERIFY(keyboard);AgendaClipboard clipboard;keyboard->setClipboardBridge(&clipboard);
        auto login=window->findChild<QObject *>("loginDialog");QVERIFY(login);QVERIFY(QMetaObject::invokeMethod(login,"open"));
        auto username=window->findChild<QQuickItem *>("loginUsername"),password=window->findChild<QQuickItem *>("loginPassword");QVERIFY(username);QVERIFY(password);
        QTRY_COMPARE(keyboard->target(),username);QTRY_VERIFY(keyboard->fallbackVisible());QCOMPARE(clipboard.calls,0);
        username->setProperty("text","ancien compte");QVERIFY(QMetaObject::invokeMethod(username,"selectAll"));clipboard.value=QString::fromUtf8("élève-éç");QTest::keyClick(window,Qt::Key_V,Qt::ControlModifier);QCOMPARE(username->property("text").toString(),clipboard.value);
        password->forceActiveFocus();QTRY_COMPARE(keyboard->target(),password);QTRY_VERIFY(keyboard->fallbackVisible());QTest::qWait(80);
        const auto fieldRect=password->mapRectToScene(password->boundingRect());QVERIFY(QRectF(QPointF(),size).contains(fieldRect));
        // Every visible fallback panel must sit below the focused dialog field.
        int visiblePaste=0;for(auto button:window->findChildren<QQuickItem *>("keyboardPaste"))if(button->isVisible()){++visiblePaste;QVERIFY(fieldRect.bottom()<button->mapToScene(QPointF()).y());}QCOMPARE(visiblePaste,1);
        password->setProperty("text","remplacer");QVERIFY(QMetaObject::invokeMethod(password,"selectAll"));clipboard.value=QString::fromUtf8("essai-éè-😀");QTest::keyClick(window,Qt::Key_V,Qt::ControlModifier);QCOMPARE(password->property("text").toString(),clipboard.value);
        const auto evidence=qEnvironmentVariable("PAPER_UI_EVIDENCE");if(!evidence.isEmpty()&&size.width()==936){QTest::qWait(120);QVERIFY(window->grabWindow().save(evidence));}
        QVERIFY(QMetaObject::invokeMethod(login,"close"));QTRY_VERIFY(!keyboard->fallbackVisible());QTRY_COMPARE(password->property("text").toString(),QString());
        auto ics=window->findChild<QObject *>("icsDialog");QVERIFY(ics);QVERIFY(QMetaObject::invokeMethod(ics,"open"));auto url=window->findChild<QQuickItem *>("icsUrl");QVERIFY(url);
        QTRY_COMPARE(keyboard->target(),url);QTRY_VERIFY(keyboard->fallbackVisible());QTest::qWait(80);QVERIFY(QRectF(QPointF(),size).contains(url->mapRectToScene(url->boundingRect())));
        clipboard.value="https://example.test/calendrier.ics";QTest::keyClick(window,Qt::Key_V,Qt::ControlModifier);QCOMPARE(url->property("text").toString(),clipboard.value);
        QVERIFY(QMetaObject::invokeMethod(ics,"close"));QTRY_VERIFY(!keyboard->fallbackVisible());QCOMPARE(warnings.size(),0);
    }
    void cachedCpeFavoriTitleIsReadableInCalendarAndEventDialog(){
        QTemporaryDir temporary;QVERIFY(temporary.isValid());SyntheticAgendaData isolation(temporary.path());
        const QString dataDirectory=QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        QVERIFY(QDir::cleanPath(dataDirectory).startsWith(QDir::cleanPath(temporary.path())+'/'));QVERIFY(QDir().mkpath(dataDirectory));
        const QString title="Analyse des circuits";
        const QJsonObject event{{"id","synthetic-favori"},{"date_debut","2026-09-07T09:00:00"},{"date_fin","2026-09-07T11:00:00"},
                                {"matiere",QJsonValue::Null},{"type_activite",QJsonValue::Null},
                                {"favori",QJsonObject{{"f2"," | A101"},{"f3",title},{"f5","Cours"},{"f4","Enseignant test"}}}};
        const QJsonObject source{{"id","mycpe"},{"kind","cpe"},{"label","Calendrier synthétique"},
                                 {"cachedWeeks",QJsonArray{"2026-09-07"}},{"events",QJsonArray{event}}};
        const QByteArray cache=QJsonDocument(QJsonObject{{"version",1},{"sources",QJsonArray{source}}}).toJson();
        QFile file(dataDirectory+"/agenda-cache.json");QVERIFY(file.open(QIODevice::WriteOnly));QCOMPARE(file.write(cache),qint64(cache.size()));file.close();

        AgendaController agenda;QVERIFY(!agenda.cpeConnected());agenda.selectDate("2026-09-07");agenda.setView("week");
        const auto events=agenda.days().first().toMap().value("events").toList();QCOMPARE(events.size(),1);QCOMPARE(events.first().toMap().value("title").toString(),title);
        QQmlApplicationEngine engine;
        engine.setInitialProperties({{"width",936},{"height",1248}});
        engine.rootContext()->setContextProperty("agenda",&agenda);engine.rootContext()->setContextProperty("previewPage",QString());engine.load(QUrl("qrc:/reagenda/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());auto window=qobject_cast<QQuickWindow *>(engine.rootObjects().first());QVERIFY(window);
        QCOMPARE(window->size(),QSize(936,1248));QVERIFY(QTest::qWaitForWindowExposed(window));QTest::qWait(60);
        auto calendarTitle=visibleText(window->contentItem(),title);QVERIFY(calendarTitle);
        QVERIFY(!calendarTitle->property("truncated").toBool());QVERIFY(calendarTitle->width()>0);QVERIFY(calendarTitle->height()>0);
        QVERIFY(QRectF(0,0,936,1248).contains(calendarTitle->mapRectToScene(calendarTitle->boundingRect())));
        QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,calendarTitle->mapToScene({calendarTitle->width()/2,calendarTitle->height()/2}).toPoint());
        QTRY_COMPARE(window->property("selectedEvent").toMap().value("title").toString(),title);

        QObject *dialog=nullptr;
        for(auto object:window->findChildren<QObject *>())if(object->property("title").toString()==title&&object->metaObject()->indexOfSignal("opened()")>=0){dialog=object;break;}
        QVERIFY(dialog);QTRY_VERIFY(dialog->property("visible").toBool());
        auto header=qobject_cast<QQuickItem *>(dialog->property("header").value<QObject *>());
        auto content=qobject_cast<QQuickItem *>(dialog->property("contentItem").value<QObject *>());QVERIFY(header);QVERIFY(content);
        auto detailTitle=visibleText(header,title);QVERIFY(detailTitle);QVERIFY(!detailTitle->property("truncated").toBool());
        QVERIFY(QRectF(0,0,936,1248).contains(detailTitle->mapRectToScene(detailTitle->boundingRect())));
        QVERIFY(visibleText(content,"A101"));QVERIFY(visibleText(content,"Enseignant test"));
        QStringList noteContext;
        QJsonObject structuredNote;
        agenda.setNoteHandler([&](const QString &date,const QString &eventId,const QString &noteTitle,const QJsonObject &context){noteContext={date,eventId,noteTitle};structuredNote=context;return true;});
        auto notesButton=window->findChild<QQuickItem *>("eventNotesButton");QVERIFY(notesButton);
        QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,notesButton->mapToScene({notesButton->width()/2,notesButton->height()/2}).toPoint());
        auto noteDialog=window->findChild<QObject *>("noteProgressDialog");QVERIFY(noteDialog);QTRY_VERIFY(noteDialog->property("visible").toBool());
        QCOMPARE(noteContext,QStringList({"2026-09-07",events.first().toMap().value("id").toString(),title}));
        QCOMPARE(structuredNote["event"].toObject()["subject"].toString(),title);
        QCOMPARE(structuredNote["event"].toObject()["start"].toString(),QString("2026-09-07T09:00:00+02:00"));
        QVERIFY(agenda.noteBusy());
        agenda.reportBridgeError("Ouverture native synthétique indisponible");
        auto noteMessage=window->findChild<QQuickItem *>("noteProgressMessage");QVERIFY(noteMessage);
        QTRY_COMPARE(noteMessage->property("text").toString(),QString("Ouverture native synthétique indisponible"));
        QVERIFY(!agenda.noteBusy());QVERIFY(noteMessage->isVisible());
        QVERIFY(QMetaObject::invokeMethod(noteDialog,"close"));
        // Qt 6.2 can report an existing day-card implicitHeight binding loop
        // at this width. Keep that warning visible; this regression checks the
        // actual rendered title, geometry and interaction, without changing layout.
        QVERIFY(QMetaObject::invokeMethod(dialog,"close"));QTRY_VERIFY(!dialog->property("visible").toBool());
    }
};
QTEST_MAIN(AgendaUiTest)
#include "AgendaUiTest.moc"
