#pragma once
#include <QObject>
#include <QPointer>
#include <QRectF>
#include <QString>

class NativeSceneObserver;
namespace RePaperNative {
bool attachExistingSceneObserver(QObject *controller, const QString &pageId,
                                 NativeSceneObserver *observer, QString *reason);
}

// One observer per controller/page epoch. Native Scene::changed is a wake-up
// after in-memory mutation, not a command receipt or a disk-persistence signal.
class NativeSceneObserver : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool ready READ ready NOTIFY readinessChanged)
    Q_PROPERTY(QString reason READ reason NOTIFY readinessChanged)
public:
    explicit NativeSceneObserver(QObject *controller, QObject *parent = nullptr);
    ~NativeSceneObserver() override;
    bool tryAttach();
    bool ready() const;
    // Compare against the Scene identity of a snapshot copied under the native
    // Page lock. An unchanged pageId alone does not identify a reloaded Scene.
    bool observesIdentity(quintptr sceneIdentity) const;
    QString reason() const { return m_reason; }
    void detach();
signals:
    void contentChanged();
    void readinessChanged();
private slots:
    void sceneChanged(const QRectF &dirtyRect);
    void sceneDestroyed();
private:
    friend class NativeSceneObserverTest;
    friend bool RePaperNative::attachExistingSceneObserver(QObject *, const QString &,
                                                            NativeSceneObserver *, QString *);
    bool connectScene(QObject *scene);
    bool samePage() const;
    void disconnectScene();
    QPointer<QObject> m_controller;
    QPointer<QObject> m_scene;
    QString m_pageId;
    QString m_reason;
    QMetaObject::Connection m_changeConnection;
    QMetaObject::Connection m_destroyConnection;
    bool m_ready = false;
    bool m_detached = false;
};
