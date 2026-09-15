#pragma once
#include <QObject>
#include <QPointer>

class InkCanvas;

// This adapter deliberately never guesses an internal Xochitl method signature.
// The only enabled target in this release is the application's in-process emulator page.
class ActivePageAdapter : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(bool emulator READ emulator CONSTANT)
    Q_PROPERTY(QString status READ status NOTIFY changed)
public:
    explicit ActivePageAdapter(QObject *parent=nullptr);
    bool available() const;
    bool emulator() const {return m_emulator;}
    QString status() const;
    Q_INVOKABLE void attachEmulatorPage(QObject *page);
    Q_INVOKABLE bool insert(const QString &symbolId);
signals:
    void changed();
private:
    bool m_emulator;
    QPointer<InkCanvas> m_page;
};
