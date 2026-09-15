#!/usr/bin/env python3
"""Generate the integration-only patch; never edit the upstream checkout."""
import argparse
import difflib
from pathlib import Path


def replace_once(text, before, after):
    if text.count(before) != 1:
        raise ValueError(f"Expected one matching integration anchor: {before[:90]!r}")
    return text.replace(before, after, 1)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    patches = []
    for name in ["appload.pro", "xovi/template/appload.pro", "src/main.cpp",
                 "xovi/template/src/main.cpp", "resources/qml/window.qml", "resources/qml/appload.qml"]:
        original = (args.source / name).read_text(encoding="utf-8")
        updated = original
        if name.endswith(".pro"):
            updated = replace_once(updated, "QT     += core gui qml quickcontrols2",
                                    "QT     += core gui qml quickcontrols2 network")
            updated += ("\nINCLUDEPATH += src\n"
                        "SOURCES += src/native-keyboard-host/NativeKeyboardHost.cpp\n"
                        "HEADERS += src/native-keyboard-host/NativeKeyboardHost.h\n")
        elif name.endswith("main.cpp"):
            updated = replace_once(updated, '#include "qtfb/FBController.h"',
                '#include "qtfb/FBController.h"\n#include "native-keyboard-host/NativeKeyboardHost.h"')
            anchor = 'qmlRegisterType<FBController>("net.asivery.Framebuffer", 1, 0, "FBController");'
            indent = "        " if name.startswith("xovi/") else "    "
            updated = replace_once(updated, anchor, anchor + '\n' + indent
                + 'qmlRegisterType<NativeKeyboardHost>("net.asivery.Framebuffer", 1, 0, "NativeKeyboardHost");')
        elif name.endswith("window.qml"):
            updated = replace_once(updated, "    property var qtfbKey: -1",
                "    property var qtfbKey: -1\n"
                "    readonly property string nativeKeyboardSocket: nativeKeyboardHost.socketPath")
            updated = replace_once(updated, "            id: windowCanvas\n",
                "            id: windowCanvas\n\n"
                "            NativeKeyboardHost {\n"
                "                id: nativeKeyboardHost\n"
                "                anchors.fill: parent\n"
                "                framebufferID: root.qtfbKey\n"
                "                framebufferItem: windowCanvas\n"
                "            }\n")
        else:
            updated = replace_once(updated,
                "            win.appPid = library.launchExternal(modelData.id, qtfbKey, extraArgs || [], extraEnv || {});",
                "            const environment = Object.assign({}, extraEnv || {});\n"
                "            if (qtfbKey !== -1 && win.nativeKeyboardSocket.length > 0)\n"
                "                environment.REPAPER_NATIVE_KEYBOARD_SOCKET = win.nativeKeyboardSocket;\n"
                "            win.appPid = library.launchExternal(modelData.id, qtfbKey, extraArgs || [], environment);")
        patches.extend(difflib.unified_diff(original.splitlines(keepends=True), updated.splitlines(keepends=True),
                                          fromfile="a/" + name, tofile="b/" + name))
    args.output.write_text("".join(patches), encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
