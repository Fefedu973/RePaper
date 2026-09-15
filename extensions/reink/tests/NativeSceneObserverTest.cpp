#include "NativeSceneObserver.h"
#include <QtTest>
#include <QThread>

class SceneController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString pageId MEMBER pageId)
public:
    QString pageId = QStringLiteral("fixture-page");
};
class ObserverFixtureScene : public QObject {
    Q_OBJECT
public:
    void mutate() { emit changed(QRectF(1,2,30,40)); }
signals:
    void changed(QRectF dirtyRect);
};

class NativeSceneObserverTest : public QObject {
    Q_OBJECT
private slots:
    void samePageWithRetainedOldSceneNeedsIdentityRevalidation() {
        SceneController controller;
        ObserverFixtureScene retainedOldScene,newScene;
        NativeSceneObserver observer(&controller);
        QVERIFY(observer.connectScene(&retainedOldScene));
        const auto oldIdentity=reinterpret_cast<quintptr>(&retainedOldScene);
        const auto newIdentity=reinterpret_cast<quintptr>(&newScene);
        QVERIFY(observer.ready());
        QVERIFY(observer.observesIdentity(oldIdentity));
        QVERIFY(!observer.observesIdentity(newIdentity));
        QVERIFY(!observer.observesIdentity(0));
        QVERIFY(observer.connectScene(&newScene));
        QVERIFY(observer.observesIdentity(newIdentity));
        QVERIFY(!observer.observesIdentity(oldIdentity));
        controller.pageId=QStringLiteral("other-page");
        QVERIFY(!observer.observesIdentity(newIdentity));
    }
    void desktopGateCannotAttachPrivateScene() {
        SceneController controller;
        NativeSceneObserver observer(&controller);
        QVERIFY(!observer.tryAttach());
        QCOMPARE(observer.reason(), QStringLiteral("unsupported-build"));
        QVERIFY(!observer.ready());
    }
    void queuedChangeIsNotSynchronousReceipt() {
        SceneController controller;
        ObserverFixtureScene scene;
        NativeSceneObserver observer(&controller);
        QVERIFY(observer.connectScene(&scene));
        QSignalSpy changed(&observer,&NativeSceneObserver::contentChanged);
        scene.mutate();
        QCOMPARE(changed.count(),0);
        QTRY_COMPARE(changed.count(),1);
        QVERIFY(observer.ready());
    }
    void changedPageRejectsAlreadyQueuedEvent() {
        SceneController controller;
        ObserverFixtureScene scene;
        NativeSceneObserver observer(&controller);
        QVERIFY(observer.connectScene(&scene));
        QSignalSpy changed(&observer,&NativeSceneObserver::contentChanged);
        scene.mutate();
        controller.pageId = QStringLiteral("other-page");
        QCoreApplication::sendPostedEvents(nullptr,QEvent::MetaCall);
        QCOMPARE(changed.count(),0);
        QVERIFY(!observer.ready());
        QVERIFY(!observer.tryAttach());
        QCOMPARE(observer.reason(),QStringLiteral("page-or-controller-changed"));
    }
    void detachDiscardsQueuedEventsAndCannotRearm() {
        SceneController controller;
        ObserverFixtureScene scene;
        NativeSceneObserver observer(&controller);
        QVERIFY(observer.connectScene(&scene));
        QSignalSpy changed(&observer,&NativeSceneObserver::contentChanged);
        scene.mutate();
        observer.detach();
        QCoreApplication::sendPostedEvents(nullptr,QEvent::MetaCall);
        QCOMPARE(changed.count(),0);
        QVERIFY(!observer.ready());
        QVERIFY(!observer.tryAttach());
    }
    void destroyedSceneInvalidatesPendingChange() {
        SceneController controller;
        auto *scene = new ObserverFixtureScene;
        NativeSceneObserver observer(&controller);
        QVERIFY(observer.connectScene(scene));
        QSignalSpy changed(&observer,&NativeSceneObserver::contentChanged);
        scene->mutate();
        delete scene;
        QVERIFY(!observer.ready());
        QCoreApplication::sendPostedEvents(nullptr,QEvent::MetaCall);
        QCOMPARE(changed.count(),0);
        QCOMPARE(observer.reason(),QStringLiteral("detached"));
    }
    void destroyedProxyRemovesOldPageQueuedDelivery() {
        SceneController controller;
        ObserverFixtureScene scene;
        int oldDeliveries=0;
        auto *old = new NativeSceneObserver(&controller);
        QVERIFY(old->connectScene(&scene));
        connect(old,&NativeSceneObserver::contentChanged,this,[&]{++oldDeliveries;});
        scene.mutate();
        delete old;
        NativeSceneObserver next(&controller);
        QVERIFY(next.connectScene(&scene));
        QSignalSpy changed(&next,&NativeSceneObserver::contentChanged);
        QCoreApplication::sendPostedEvents(nullptr,QEvent::MetaCall);
        QCOMPARE(oldDeliveries,0);
        QCOMPARE(changed.count(),0);
        scene.mutate();
        QTRY_COMPARE(changed.count(),1);
    }
    void workerEmissionRunsReceiverOnGuiThread() {
        SceneController controller;
        QThread worker;
        auto *scene = new ObserverFixtureScene;
        scene->moveToThread(&worker);
        connect(&worker,&QThread::finished,scene,&QObject::deleteLater);
        NativeSceneObserver observer(&controller);
        QVERIFY(observer.connectScene(scene));
        bool gui=false;
        connect(&observer,&NativeSceneObserver::contentChanged,this,[&]{gui=QThread::currentThread()==thread();});
        worker.start();
        QVERIFY(QMetaObject::invokeMethod(scene,[scene]{scene->mutate();},Qt::QueuedConnection));
        QTRY_VERIFY(gui);
        worker.quit();
        QVERIFY(worker.wait());
    }
    void staleSenderCannotNotifyOrDetachReplacement() {
        SceneController controller;
        auto *oldScene = new ObserverFixtureScene;
        ObserverFixtureScene newScene;
        NativeSceneObserver observer(&controller);
        QVERIFY(observer.connectScene(oldScene));
        QSignalSpy changed(&observer,&NativeSceneObserver::contentChanged);
        oldScene->mutate();
        delete oldScene; // both content and destruction callbacks are queued
        observer.disconnectScene();
        QVERIFY(observer.connectScene(&newScene));
        QCoreApplication::sendPostedEvents(nullptr,QEvent::MetaCall);
        QCOMPARE(changed.count(),0);
        QVERIFY(observer.ready());
        newScene.mutate();
        QTRY_COMPARE(changed.count(),1);
    }
    void queuedLiveOldSenderIsIgnoredAfterRebind() {
        SceneController controller;
        ObserverFixtureScene oldScene, newScene;
        NativeSceneObserver observer(&controller);
        QVERIFY(observer.connectScene(&oldScene));
        QSignalSpy changed(&observer,&NativeSceneObserver::contentChanged);
        oldScene.mutate();
        QVERIFY(observer.connectScene(&newScene));
        QCoreApplication::sendPostedEvents(nullptr,QEvent::MetaCall);
        QCOMPARE(changed.count(),0);
        QVERIFY(observer.connectScene(&newScene)); // does not duplicate binding
        newScene.mutate();
        QTRY_COMPARE(changed.count(),1);
    }
    void wrongControllerAndMissingSignalFailClosed() {
        QObject wrongController;
        NativeSceneObserver missingPage(&wrongController);
        QVERIFY(!missingPage.tryAttach());
        SceneController controller;
        NativeSceneObserver observer(&controller);
        QObject noSignal;
        QVERIFY(!observer.connectScene(&noSignal));
        QVERIFY(!observer.ready());
    }
};
QTEST_GUILESS_MAIN(NativeSceneObserverTest)
#include "NativeSceneObserverTest.moc"
