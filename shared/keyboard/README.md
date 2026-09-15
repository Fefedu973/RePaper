# Shared form keyboard

`repaper_keyboard` serves reAgenda, reMoodle and reStencil. It is separate from the math input in reCalc.

Each text focus first requests the installed Qt platform input method with `QInputMethod::show()`. If Qt reports a visible panel, the fallback stays hidden. Otherwise, after 250 ms, the app displays **Clavier de secours · AZERTY**. Switching between editable fields keeps that fallback open. Read-only, hidden or disabled fields are excluded; leaving text focus or pressing **Masquer** hides it. Platform-reported keyboard occlusion is exposed as `platformHeight` so the app can reserve space. The CPE and ICS dialogs use scrollable forms above the fallback and reveal the focused field after resizing.

This does **not** establish access to the proprietary Xochitl keyboard. The inspected 5.8.203 SDK has no separately usable native keyboard plugin or Qt Virtual Keyboard package. Xochitl 3.28 contains internal keyboard/input-context symbols, but no external integration ABI was verified. AppLoad's optional community keyboard is also distinct from the Xochitl keyboard. The component makes no claim of native availability beyond Qt actually reporting a panel as visible.

The implementation uses public Qt APIs: [QInputMethod](https://doc.qt.io/qt-6/qinputmethod.html), [QInputMethodEvent](https://doc.qt.io/qt-6/qinputmethodevent.html) for selection-aware insertion, and [QTextBoundaryFinder](https://doc.qt.io/qt-6/qtextboundaryfinder.html) for grapheme deletion. Accents, surrogate pairs and composed emoji are not split by backspace. Enter and cursor arrows use the text control's normal key handling.

## Integration

Link `repaper_keyboard`, include `KeyboardController.h`, and call `repaper::registerKeyboardTypes()` before loading QML (also in tests that load an app's QML). Do not retain a local `TouchKeyboard.qml` that would shadow the shared type.

```qml
import RePaper.Keyboard 1.0

KeyboardController { id: keyboard; window: window }
// Put this at the bottom of the relevant ColumnLayout, or inside the dialog.
TouchKeyboard { controller: keyboard; Layout.fillWidth: true }
```

`KeyboardController` follows the window's active text item automatically. It exposes `target`, `fallbackVisible`, `platformVisible`, `platformHeight`, and invokables `insertText`, `backspace`, `enter`, `moveCursor`, `paste`, `dismiss`. Reserve `platformHeight` at the bottom of the app layout when using a platform input panel. The shared fallback sizes and hides itself. No integration is added to reCalc.

## Windows clipboard in the PC emulator

Ctrl+V (including normal Qt paste shortcuts) and the **Coller** button are explicit read requests. In WSL with `REPAPER_PC_EMULATOR=1`, the clipboard bridge launches a fixed, hidden `powershell.exe` command, reads `Get-Clipboard -Raw` through a UTF-8 stdout pipe, and captures/discards stderr. Clipboard contents never enter command arguments, logs or the clipboard of another system. The bridge never polls or writes either clipboard. Reads are bounded to 262,144 UTF-16 code units, 1 MiB of output and five seconds. Outside PC emulation it reads the normal Qt clipboard.

A pending paste is applied only if the originating field, focus epoch, text, cursor and selection still match. Changing fields or editing while a Windows read is pending discards the result. Password fields support the same explicit selection-aware paste.

The PC AppLoad generator patches physical keyboard forwarding and the shim's key mapping: Qt Control/V -> QTFB virtual-key packets -> Linux evdev Control/V -> the focused text field. Upstream otherwise only forwards a few navigation keys, and its virtual-key path emits X11 keysyms rather than the Linux key codes expected by stock Qt evdev. Input values are not logged. Rebuild an existing AppLoad PC profile with `tools/emulator-apps.py setup` to obtain this change. Tablet code and deployment are untouched.

## Validation

`repaper_keyboard_tests` covers focus, read-only fields, selection replacement, Unicode backspace, enter/arrows, explicit paste into passwords, delayed-paste fencing and the UTF-8 process adapter using a fake executable. `reagenda_ui_tests` loads the actual CPE/ICS dialogs at 936×1248 and 800×800. `restencil_ui_tests` covers search focus and preserves its existing canvas checks. All clipboard tests use fake data; they never read the user's clipboard.

The additional `reagenda_qtfb_probe` target is a test-only executable using the actual reAgenda QML. Run the following against a separately built QA AppLoad profile:

```sh
python3 tools/test-keyboard-qtfb.py \
  --state /root/repaper-appload-qa \
  --probe /root/reagenda-keyboard-build/reagenda_qtfb_probe \
  --screenshot /absolute/path/qtfb-keyboard.png
```

The helper temporarily swaps the QA calendar manifest, injects Control+V into AppLoad twice, and restores the manifest. The fake clipboard must replace selections in the actual username and password fields through QTFB/evdev. The recorded evidence contains success flags and event counts only. Verified result: two explicit clipboard requests, two Ctrl+V presses, both field replacements successful. It does not authenticate an account or connect to a tablet.
