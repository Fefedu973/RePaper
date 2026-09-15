#include "KeyboardController.h"
#include "../ui/PaperFonts.h"
#include "ClipboardBridge.h"
#include "NativeKeyboardClient.h"
#include <QGuiApplication>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMetaProperty>
#include <QQmlEngine>
#include <QTextBoundaryFinder>
#include <algorithm>

static void initializeKeyboardResources(){Q_INIT_RESOURCE(repaper_keyboard_qml);}

namespace repaper {
KeyboardController::KeyboardController(QObject *parent):QObject(parent) {
    qGuiApp->installEventFilter(this);setClipboardBridge(new ClipboardBridge(this));
    m_native = new NativeKeyboardClient(this);
    connect(m_native, &NativeKeyboardClient::changed, this, [this] { emit changed(); });
    m_fallbackTimer.setSingleShot(true);m_fallbackTimer.setInterval(250);
    connect(&m_fallbackTimer,&QTimer::timeout,this,[this]{m_waiting=false;emit changed();});
    auto input=QGuiApplication::inputMethod();
    connect(input,&QInputMethod::visibleChanged,this,[this]{emit changed();});
    connect(input,&QInputMethod::keyboardRectangleChanged,this,[this]{emit changed();});
}
bool KeyboardController::editable(QQuickItem *item) const {
    return item&&item->isVisible()&&item->isEnabled()&&item->hasActiveFocus()
        &&item->property("readOnly").isValid()&&!item->property("readOnly").toBool()
        &&item->property("cursorPosition").isValid()&&item->property("text").isValid();
}
void KeyboardController::setWindow(QQuickWindow *window) {
    if(m_window==window)return;
    if(m_window)disconnect(m_window,nullptr,this,nullptr);
    m_window=window;
    if(window){
        connect(window,&QQuickWindow::activeFocusItemChanged,this,&KeyboardController::updateFocus);
        connect(window,&QWindow::visibleChanged,this,&KeyboardController::updateFocus);
        connect(window,&QWindow::heightChanged,this,[this]{emit changed();});
    }
    updateFocus();emit windowChanged();
}
void KeyboardController::updateFocus() {
    auto item=m_window&&m_window->isVisible()?m_window->activeFocusItem():nullptr;
    if(!editable(item))item=nullptr;
    if(item==m_target){emit changed();return;}
    const bool keepFallback=m_target&&!m_waiting&&!QGuiApplication::inputMethod()->isVisible();
    for(auto connection:m_targetConnections)disconnect(connection);m_targetConnections.clear();
    m_target=item;++m_focusEpoch;m_fallbackTimer.stop();m_waiting=false;
    if(item) {
        m_targetConnections.append(connect(item,&QQuickItem::visibleChanged,this,&KeyboardController::updateFocus));
        m_targetConnections.append(connect(item,&QQuickItem::enabledChanged,this,&KeyboardController::updateFocus));
        const auto readOnly=item->metaObject()->property(item->metaObject()->indexOfProperty("readOnly"));
        if(readOnly.hasNotifySignal())m_targetConnections.append(connect(item,readOnly.notifySignal(),this,metaObject()->method(metaObject()->indexOfSlot("updateFocus()"))));
        m_targetConnections.append(connect(item,&QObject::destroyed,this,[this]{m_target=nullptr;m_fallbackTimer.stop();m_waiting=false;emit changed();}));
        // The AppLoad host uses Xochitl's real input method when its socket is available.
        m_native->focus(m_window, item);
        m_waiting=!keepFallback;QGuiApplication::inputMethod()->show();if(m_waiting)m_fallbackTimer.start();
    } else { m_native->blur(); QGuiApplication::inputMethod()->hide(); }
    emit changed();
}
bool KeyboardController::platformVisible() const {return editable(m_target)&&(m_native->visible()||QGuiApplication::inputMethod()->isVisible());}
bool KeyboardController::fallbackVisible() const {return editable(m_target)&&!m_waiting&&!m_native->waiting()&&!m_native->available()&&!platformVisible();}
QRectF KeyboardController::platformRectangle() const {
    if(!platformVisible()||!m_window)return {};
    return m_native->visible()?m_native->keyboardRectangle():QGuiApplication::inputMethod()->keyboardRectangle();
}
void KeyboardController::refreshGeometry() {
    if(m_window && editable(m_target))m_native->focus(m_window,m_target);
}
qreal KeyboardController::platformHeight() const {
    if(!platformVisible()||!m_window)return 0;
    const auto rect=platformRectangle();
    if(rect.isEmpty())return 0; // Floating keyboards may not report an occlusion.
    return std::clamp(m_window->height()-rect.top(),qreal(0),qreal(m_window->height()));
}
void KeyboardController::insertText(const QString &text) {
    if(!editable(m_target)||text.isEmpty())return;
    QInputMethodEvent event;event.setCommitString(text);QCoreApplication::sendEvent(m_target,&event);
}
void KeyboardController::sendKey(int key) {
    if(!editable(m_target))return;
    QPointer<QQuickItem> item=m_target;
    QKeyEvent press(QEvent::KeyPress,key,Qt::NoModifier);QCoreApplication::sendEvent(item,&press);
    if(item){QKeyEvent release(QEvent::KeyRelease,key,Qt::NoModifier);QCoreApplication::sendEvent(item,&release);}
}
void KeyboardController::backspace() {
    if(!editable(m_target))return;
    if(m_target->property("selectionStart")!=m_target->property("selectionEnd")){sendKey(Qt::Key_Backspace);return;}
    const auto text=m_target->property("text").toString();
    const int cursor=std::clamp(m_target->property("cursorPosition").toInt(),0,int(text.size()));
    if(cursor==0)return;
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme,text);finder.setPosition(cursor);
    const bool boundary=finder.isAtBoundary();const int start=finder.toPreviousBoundary();
    if(start<0)return;
    int end=cursor;
    if(!boundary){finder.setPosition(cursor);end=finder.toNextBoundary();if(end<0)end=text.size();}
    QInputMethodEvent event;event.setCommitString(QString(),start-cursor,end-start);QCoreApplication::sendEvent(m_target,&event);
}
void KeyboardController::enter(){sendKey(Qt::Key_Return);}
void KeyboardController::moveCursor(int direction){if(direction)sendKey(direction<0?Qt::Key_Left:Qt::Key_Right);}
void KeyboardController::dismiss() {
    m_native->blur();
    QGuiApplication::inputMethod()->hide();
    if(m_target)m_target->setFocus(false);
    updateFocus();
}
void KeyboardController::setClipboardBridge(ClipboardBridge *clipboard) {
    if(m_clipboard==clipboard)return;
    if(m_clipboard){disconnect(m_clipboard,nullptr,this,nullptr);if(m_clipboard->parent()==this)m_clipboard->deleteLater();}
    m_clipboard=clipboard;m_pastePending=false;m_pasteTarget=nullptr;m_pasteText.clear();if(!clipboard)return;
    connect(clipboard,&ClipboardBridge::ready,this,[this](const QString &text){
        const bool current=m_pasteTarget&&m_pasteTarget==m_target&&m_pasteEpoch==m_focusEpoch
            &&m_target->property("cursorPosition").toInt()==m_pasteCursor
            &&m_target->property("selectionStart").toInt()==m_pasteAnchor
            &&m_target->property("selectionEnd").toInt()==m_pasteEnd
            &&m_target->property("text").toString()==m_pasteText;
        m_pasteTarget=nullptr;m_pasteText.clear();m_pastePending=false;if(current)insertText(text);
    });
    connect(clipboard,&ClipboardBridge::failed,this,[this]{m_pasteTarget=nullptr;m_pasteText.clear();m_pastePending=false;emit pasteFailed();});
}
void KeyboardController::paste() {
    if(!editable(m_target)||!m_clipboard||m_pastePending)return;
    m_pastePending=true;m_pasteTarget=m_target;m_pasteEpoch=m_focusEpoch;m_pasteCursor=m_target->property("cursorPosition").toInt();
    m_pasteAnchor=m_target->property("selectionStart").toInt();m_pasteEnd=m_target->property("selectionEnd").toInt();m_pasteText=m_target->property("text").toString();m_clipboard->request();
}
bool KeyboardController::eventFilter(QObject *object,QEvent *event) {
    if(!editable(m_target)||!m_window)return QObject::eventFilter(object,event);
    const auto item=qobject_cast<QQuickItem *>(object);
    if(object!=m_window&&object!=m_target&&(!item||item->window()!=m_window))return false;
    if(event->type()==QEvent::ShortcutOverride||event->type()==QEvent::KeyPress) {
        const auto key=static_cast<QKeyEvent *>(event);
        if(key->matches(QKeySequence::Paste)) {
            event->accept();if(event->type()==QEvent::KeyPress&&!key->isAutoRepeat())paste();return true;
        }
    }
    return QObject::eventFilter(object,event);
}
void registerKeyboardTypes() {
    repaper::registerPaperFonts();
    static bool registered=false;if(registered)return;registered=true;initializeKeyboardResources();
    qmlRegisterType<KeyboardController>("RePaper.Keyboard",1,0,"KeyboardController");
    qmlRegisterType(QUrl("qrc:/repaper/keyboard/TouchKeyboard.qml"),"RePaper.Keyboard",1,0,"TouchKeyboard");
}
}
