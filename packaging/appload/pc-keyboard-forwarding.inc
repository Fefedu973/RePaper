// PC harness: preserve modifier press/release through the QTFB virtual keys.
static int pcKeyboardKey(int key) {
    if(key>=Qt::Key_A&&key<=Qt::Key_Z)return 'a'+key-Qt::Key_A;
    if(key>=Qt::Key_0&&key<=Qt::Key_9)return '0'+key-Qt::Key_0;
    switch(key) {
        case Qt::Key_Control:return INPUT_VKB_CTRLMOD;
        case Qt::Key_Shift:return INPUT_VKB_SHIFTMOD;
        case Qt::Key_Alt:return INPUT_VKB_ALTMOD;
        case Qt::Key_Return:case Qt::Key_Enter:return 13;
        case Qt::Key_Backspace:return 8;case Qt::Key_Tab:return 9;case Qt::Key_Escape:return 27;
        case Qt::Key_Delete:return INPUT_VKB_DEL;
        case Qt::Key_Left:return INPUT_VKB_LEFT;case Qt::Key_Right:return INPUT_VKB_RIGHT;
        case Qt::Key_Up:return INPUT_VKB_UP;case Qt::Key_Down:return INPUT_VKB_DOWN;
        case Qt::Key_Home:return INPUT_VKB_HOME;case Qt::Key_End:return INPUT_VKB_END;
        case Qt::Key_Space:case Qt::Key_Period:case Qt::Key_Comma:
        case Qt::Key_Slash:case Qt::Key_Minus:case Qt::Key_Equal:
        case Qt::Key_Semicolon:case Qt::Key_Apostrophe:
        case Qt::Key_BracketLeft:case Qt::Key_BracketRight:case Qt::Key_Backslash:return key;
        default:return -1;
    }
}
void FBController::keyPressEvent(QKeyEvent *event) {
    const int key=pcKeyboardKey(event->key());
    if(key<0){event->ignore();return;}
    virtualKeyboardKeyDown(key);event->accept();
}
void FBController::keyReleaseEvent(QKeyEvent *event) {
    const int key=pcKeyboardKey(event->key());
    if(key<0){event->ignore();return;}
    virtualKeyboardKeyUp(key);event->accept();
}
