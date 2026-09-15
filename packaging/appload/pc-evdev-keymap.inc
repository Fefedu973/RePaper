// Stock Qt evdev consumes Linux key codes, not upstream's X11 keysyms.
// This conversion is used only under the explicit PC emulator environment.
static int mapAsciiToLinuxKey(int value) {
    static const int letters[]={30,48,46,32,18,33,34,35,23,36,37,38,50,49,24,25,16,19,31,20,22,47,17,45,21,44};
    if(value>='A'&&value<='Z')value+='a'-'A';
    if(value>='a'&&value<='z')return letters[value-'a'];
    if(value>='1'&&value<='9')return 2+value-'1';
    switch(value) {
        case '0':return 11;case INPUT_VKB_CTRLMOD:return 29;case INPUT_VKB_SHIFTMOD:return 42;case INPUT_VKB_ALTMOD:return 56;
        case 8:return 14;case 9:return 15;case 13:return 28;case 27:return 1;case 127:return 111;
        case INPUT_VKB_LEFT:return 105;case INPUT_VKB_RIGHT:return 106;case INPUT_VKB_UP:return 103;case INPUT_VKB_DOWN:return 108;
        case INPUT_VKB_HOME:return 102;case INPUT_VKB_END:return 107;case INPUT_VKB_PGUP:return 104;case INPUT_VKB_PGDOWN:return 109;
        case ' ':return 57;case '.':return 52;case ',':return 51;case '/':return 53;case '-':return 12;case '=':return 13;
        case ';':return 39;case '\'':return 40;case '[':return 26;case ']':return 27;case '\\':return 43;
        default:return 0;
    }
}
