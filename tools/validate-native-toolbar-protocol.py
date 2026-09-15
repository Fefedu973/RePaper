#!/usr/bin/env python3
"""Prepare a private Qt harness from locally validated native toolbar resources.

The extracted firmware implementation stays in --output, which must be inside
the ignored .local directory. Only the display shell and native dependencies
are stubbed; selection functions and injected buttons come from QMLDiff output.
This never connects to a device or copies firmware sources into the repository.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re


def braced(source, start):
    """Return a complete QML/JS block, ignoring braces in comments and strings."""
    opening = source.index("{", start)
    depth, quote, comment, escape = 0, None, None, False
    i = opening
    while i < len(source):
        ch, pair = source[i], source[i:i + 2]
        if comment == "line":
            if ch == "\n":
                comment = None
        elif comment == "block":
            if pair == "*/":
                comment = None
                i += 1
        elif quote:
            if escape:
                escape = False
            elif ch == "\\":
                escape = True
            elif ch == quote:
                quote = None
        elif pair in ("//", "/*"):
            comment = "line" if pair == "//" else "block"
            i += 1
        elif ch in ("'", '"', "`"):
            quote = ch
        elif ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return source[start:i + 1]
        i += 1
    raise ValueError("Unclosed QML block")


def extract(source, marker):
    if source.count(marker) != 1:
        raise ValueError(f"Expected exactly one {marker}")
    return braced(source, source.index(marker))


def assignment(source, name):
    matches = re.findall(r"^\s*" + re.escape(name) + r":\s*([^\n]+)", source, re.MULTILINE)
    if len(matches) != 1:
        raise ValueError(f"Expected one single-line assignment {name}")
    return matches[0].strip()


def bool_property(source, name):
    matches = re.findall(r"readonly\s+property\s+bool\s+" + re.escape(name) + r":[^\n]+", source)
    if len(matches) != 1:
        raise ValueError(f"Expected one readonly bool property {name}")
    return matches[0]


def button(source, name):
    marker = f'objectName: "{name}"'
    if source.count(marker) != 1:
        raise ValueError(f"Expected exactly one {name}")
    index = source.index(marker)
    candidates = list(re.finditer(r"\b(?:PenTool|ToolbarTool)\s*\{", source[:index]))
    result = braced(source, candidates[-1].start())
    if marker not in result:
        raise ValueError("Button extraction missed its marker")
    return result


SHELL = '''import QtQuick
Item {
    id: root
    width: 700; height: 200
    property string log: "private-toolbar-protocol"
    property bool expanded: true
    property Item selectedTool: null
    property Item selectedPen: null
    property QtObject primaryPen: QtObject {}
    property QtObject selectionPen: QtObject {}
    readonly property string nativeActiveTool: selectedPen ? selectedPen.penToolType : "primary"
    property var toolbarConfiguration: ({hideTooltips: true})
    property var repaperEditorHost: host
    signal requestPenSelect(Item penTool, int mode)
    signal selectSelection()
    enum SelectMode { DontActivate, Activate }
    QtObject {
        id: host
        objectName: "editorHostFixture"
        property int selectionActivations: 0
        property bool inspectorOpen: false
        function activateCustomSelection() {
            selectionActivations += 1
            editor.chooseTool("select")
            inspectorOpen = false
        }
        property QtObject editor: QtObject {
            property bool available: true
            property string tool: "line"
            property int refreshCount: 0
            function refreshNativeState() { refreshCount += 1 }
            function chooseTool(value) { tool = value }
        }
    }
    // The following blocks are extracted unchanged into this ignored harness.
    @PROTOCOL@
    @ACTIVE@
    Row {
        PenTool {
            id: brush
            objectName: "brush"
            toolbar: root; pen: root.primaryPen; penToolType: "primary"
            type: ToolbarTool.Type.ToolbarButton
            implicitlySelected: root.selectedPen === brush
            onPressed: root.requestPenSelect(brush, Toolbar.SelectMode.Activate)
        }
        @INK@
        @SELECT@
        @STENCIL@
        NativeSelectionButton {
            id: lasso
            objectName: "lasso"
            toolbar: root; pen: root.selectionPen; penToolType: "selection"
            type: ToolbarTool.Type.ToolbarButton
            implicitlySelected: root.selectedPen === lasso
            onPressed: root.requestPenSelect(lasso, Toolbar.SelectMode.Activate)
        }
        ToolbarTool {
            id: layers
            objectName: "layers"
            readonly property bool sticky: false
            toolbar: root; type: ToolbarTool.Type.ToolbarButton
            foldoutContent: Item {}
            onPressed: root._select(layers)
        }
    }
}
'''

TOOL_STUB = '''import QtQuick
Item {
    id: tool
    width: 96; height: 72
    enum Type { ToolbarButton, FoldoutButton }
    required property Item toolbar
    required property int type
    property string iconSource
    property string label
    property bool selected: false
    property bool implicitlySelected: false
    property var toolTip: null
    property Item foldoutContent: null
    property Item foldout: Item { visible: false }
    readonly property bool hasFoldout: foldoutContent !== null
    readonly property bool foldoutVisible: hasFoldout && foldout.visible
    readonly property bool highlighted: selected || implicitlySelected
    signal pressed()
    MouseArea { anchors.fill: parent; onPressed: tool.pressed() }
}
'''

SELECTION_GUARD = '''import QtQuick
Item {
    id: root
    width: 200; height: 200
    property bool customHandlesSelection: false
    property bool customHostAvailable: true
    property int cleanupCount: 0
    property int repaintCount: 0
    property int clearCount: 0
    property int endCount: 0
    property int hostCallCount: 0
    property int receivedLayer: -1
    property rect receivedRect
    property string events: ""
    property var glyphSelection: null
    property var repaperNativePageHost: ({item: customHostAvailable ? customHost : null})
    function endItemSelection() { endCount += 1; events += "end;" }
    QtObject {
        id: customHost
        function handleNativeSelection(layer, rect) {
            root.hostCallCount += 1
            root.receivedLayer = layer; root.receivedRect = rect
            root.events += "host;"
            return root.customHandlesSelection
        }
    }
    QtObject { id: inputSurface; function clearFramebuffer() { root.cleanupCount += 1; root.events += "clean;" } }
    QtObject { id: viewport; function requestRepaintDirty() { root.repaintCount += 1; root.events += "paint;" } }
    QtObject { id: controller; function clearSelectedItems() { root.clearCount += 1; root.events += "clear;" } }
    QtObject { id: strokeHandler; property int lineTool: 0 }
    @HANDLER@
}
'''

HISTORY_GUARD = '''import QtQuick
Item {
    id: root
    width: 200; height: 200
    property bool customHostAvailable: true
    property bool customPending: false
    property bool nativeUndoAvailable: true
    property bool nativeRedoAvailable: true
    property bool newPageVisible: true
    property int undoCalls: 0
    property int redoCalls: 0
    property int exitTextCalls: 0
    property int closeCalls: 0
    property var sceneController: controller
    property var repaperNativePageHost: ({item: customHostAvailable ? customHost : null})
    QtObject { id: customHost; property bool nativeOperationPending: root.customPending }
    QtObject { id: toolbar; property var repaperEditorHost: root.customHostAvailable ? customHost : null }
    QtObject {
        id: controller
        property bool undoAvailable: root.nativeUndoAvailable
        property bool redoAvailable: root.nativeRedoAvailable
        function undo() { root.undoCalls += 1 }
        function redo() { root.redoCalls += 1 }
    }
    QtObject { id: hwcSelectionTool; function close() { root.closeCalls += 1 } }
    QtObject { id: undoRedoPenMonitor; property bool penWasClose: false }
    QtObject { id: touchArea; property bool panAndZoomOnly: false }
    property bool enableUndoRedoGestures: true
    function exitTextMode() { exitTextCalls += 1 }
    // Actual patched expressions and handlers; only service dependencies above
    // are replaced. Availability is evaluated even when custom tools are off.
    @PENDING@
    @SCENE_PENDING@
    @UNDO_AVAILABLE@
    @REDO_AVAILABLE@
    property bool toolbarUndoEnabled: @TOOLBAR_UNDO_ENABLED@
    property bool toolbarRedoEnabled: @TOOLBAR_REDO_ENABLED@
    property bool keyboardUndoEnabled: @KEYBOARD_UNDO_ENABLED@
    property bool keyboardRedoEnabled: @KEYBOARD_REDO_ENABLED@
    property bool gestureUndoEnabled: @GESTURE_UNDO_ENABLED@
    property bool gestureRedoEnabled: @GESTURE_REDO_ENABLED@
    @TOOLBAR_UNDO@
    @TOOLBAR_REDO@
    function invokeToolbarUndo() { toolbarUndo() }
    function invokeToolbarRedo() { toolbarRedo() }
    function shortcutUndo() { @SHORTCUT_UNDO@ }
    function shortcutRedo() { @SHORTCUT_REDO@ }
    @HWC_FAILED@
    @HWC_CLOSE@
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--patched", type=Path, required=True)
    parser.add_argument("--legacy-patched", type=Path, required=True)
    parser.add_argument("--qml-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    out = args.output.resolve()
    if not out.is_relative_to(repo / ".local") or out.exists():
        parser.error("Choose a new output directory inside the ignored .local tree")
    relative = Path("qt/qml/xofm/libs/toolbar/qml/Toolbar.qml")
    pen = args.qml_root / relative.parent / "PenTool.qml"
    selection = args.qml_root / relative.parent / "SelectionButton.qml"
    select_handler = extract(selection.read_text(encoding="utf-8"), "function onSelectSelection()")
    out.mkdir(parents=True)
    manifest = {"nativeRuntimeValidated": False, "sources": {}, "stubbed": [
        "ToolbarTool visual shell, tooltip absent, pointer surface is MouseArea",
        "Ordinary brush/lasso/Layers native selection signal contracts",
        "EditorAdapter refresh and palette toolChosen signal; no drawing",
    ]}
    for version, patched in (("current", args.patched), ("legacy", args.legacy_patched)):
        path = patched / relative
        validation = json.loads((patched / "validation.json").read_text())
        if version == "current" and validation["qmdSha256"] != hashlib.sha256(
                (repo / "extensions/reink/native/editor.qmd").read_bytes()).hexdigest():
            raise ValueError("Current QMD changed; validate its application again first")
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        if digest != validation["outputSha256"][relative.as_posix()]:
            raise ValueError(f"Patched source changed since validation: {path}")
        source = path.read_text(encoding="utf-8")
        protocol = [extract(source, "onRequestPenSelect:")]
        protocol += [extract(source, f"function {name}(") for name in
                     ("closeFoldout", "selectLastTool", "_select", "maybeShowToolTip")]
        active = re.search(r"readonly property\s+bool repaperToolActive:[^\n]+", source)
        if not active and version == "current":
            raise ValueError("Missing injected repaperToolActive property")
        generated = (SHELL.replace("@PROTOCOL@", "\n".join(protocol))
                     .replace("@ACTIVE@", active.group() if active else
                              "readonly property bool repaperToolActive: false")
                     .replace("@INK@", button(source, "RePaperInkTool"))
                     .replace("@SELECT@", button(source, "RePaperSelectTool") if version == "current" else "")
                     .replace("@STENCIL@", button(source, "RePaperStencilTool")))
        folder = out / version
        folder.mkdir()
        (folder / "Toolbar.qml").write_text(generated, encoding="utf-8")
        (folder / "ToolbarTool.qml").write_text(TOOL_STUB, encoding="utf-8")
        (folder / "PenTool.qml").write_bytes(pen.read_bytes())
        (folder / "NativeSelectionButton.qml").write_text(
            'import QtQuick\nPenTool { id: root\nConnections { target: root.toolbar\n'
            + select_handler + '\n}\n}\n', encoding="utf-8")
        manifest["sources"][version] = {"toolbarSha256": digest,
            "qmdSha256": validation["qmdSha256"],
            "penToolSha256": hashlib.sha256(pen.read_bytes()).hexdigest(),
            "selectionButtonSha256": hashlib.sha256(selection.read_bytes()).hexdigest()}
    (out / "ReInkSidebar.qml").write_text(
        'import QtQuick\nItem { property var editor; property string mode; signal toolChosen() }\n',
        encoding="utf-8")
    scene_path = args.patched / "qml/device/view/documentview/DeviceSceneView.qml"
    current_validation = json.loads((args.patched / "validation.json").read_text())
    for relative, expected in current_validation["outputSha256"].items():
        if hashlib.sha256((args.patched / relative).read_bytes()).hexdigest() != expected:
            raise ValueError(f"Patched source changed since validation: {relative}")
    scene_source = scene_path.read_text(encoding="utf-8")
    handler = extract(scene_source, "function onAreaSelected(")
    if handler.count("repaperNativePageHost.item.handleNativeSelection(") != 1:
        raise ValueError("Expected the custom selection guard exactly once")
    # Require the cleanup, repaint and guard to remain ordered ahead of native
    # selection validation, which otherwise clears custom primary-pen selection.
    positions = [handler.index(part) for part in ("inputSurface.clearFramebuffer()",
                 "viewport.requestRepaintDirty()", "repaperNativePageHost.item.handleNativeSelection(",
                 "const validSelection")]
    if positions != sorted(positions):
        raise ValueError("Native selection cleanup/guard order changed")
    (out / "SelectionGuard.qml").write_text(SELECTION_GUARD.replace("@HANDLER@", handler), encoding="utf-8")
    document_source = (args.patched / "qml/device/view/documentview/DocumentView.qml").read_text(encoding="utf-8")
    gestures_path = args.qml_root / "qml/device/view/documentview/SceneViewGestures.qml"
    gestures_source = gestures_path.read_text(encoding="utf-8")
    history = HISTORY_GUARD.replace("@PENDING@", bool_property(document_source, "repaperNativeOperationPending"))
    history = history.replace("@SCENE_PENDING@", bool_property(scene_source, "repaperNativeOperationPending")
                              .replace("bool repaperNativeOperationPending:", "bool sceneOperationPending:"))
    for action in ("Undo", "Redo"):
        upper = action.upper()
        available = action.lower() + "Available"
        history = history.replace(f"@{upper}_AVAILABLE@", bool_property(scene_source, available))
        history = history.replace(f"@TOOLBAR_{upper}_ENABLED@", assignment(document_source, action.lower() + "Enabled"))
        history = history.replace(f"@KEYBOARD_{upper}_ENABLED@", assignment(document_source, "vkbMenu.can" + action))
        history = history.replace(f"@SHORTCUT_{upper}@", assignment(document_source, "on" + action + "Requested"))
        history = history.replace(f"@TOOLBAR_{upper}@", extract(document_source, "on" + action + "Selected:")
                                  .replace("on" + action + "Selected:", "property var toolbar" + action + ":", 1))
        filters = re.findall(r"enabled:\s*([^\n]*\b" + available + r"\b[^\n]*)", gestures_source)
        if len(filters) != 1:
            raise ValueError(f"Expected one native gesture availability expression for {action}")
        history = history.replace(f"@GESTURE_{upper}_ENABLED@", filters[0])
    for marker, name, token in (("onConversionFailed:", "conversionFailed", "HWC_FAILED"),
                                ("onCloseAndUndo:", "closeAndUndo", "HWC_CLOSE")):
        history = history.replace("@" + token + "@", extract(scene_source, marker).replace(marker, "function " + name + "()", 1))
    if "@" in history:
        raise ValueError("Unresolved history guard harness token")
    # Each native action must check the operation lock before its original
    # mutation. This includes direct toolbar signals and shortcut dispatch.
    for marker in ("onUndoSelected:", "onRedoSelected:"):
        body = extract(document_source, marker)
        if body.index("repaperNativeOperationPending") > body.index("sceneController."):
            raise ValueError("Native toolbar action is not guarded before dispatch")
    (out / "HistoryGuard.qml").write_text(history, encoding="utf-8")
    manifest["historyGuard"] = {
        "patchedSources": {relative: digest for relative, digest in current_validation["outputSha256"].items()
                           if relative.endswith(("/DocumentView.qml", "/DeviceSceneView.qml"))},
        "gestureSourceSha256": hashlib.sha256(gestures_path.read_bytes()).hexdigest(),
        "covered": ["toolbar buttons and direct signals", "document keyboard shortcuts", "two/three-finger gesture availability",
                    "virtual keyboard history availability", "selection conversion Undo callbacks"],
        "nativeRuntimeValidated": False,
    }
    (out / "Line.qml").write_text("import QtQuick\nQtObject { enum Tools { SelectionTool = 11 } }\n", encoding="utf-8")
    (out / "palettes.qrc").write_text(
        '<RCC><qresource prefix="/repaper/editor"><file>ReInkSidebar.qml</file></qresource></RCC>\n',
        encoding="utf-8")
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    # Configure via -DREPAPER_REPO and -DHARNESS_DIR for either host environment.
    (out / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.16)
project(NativeToolbarProtocol LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick Test)
add_executable(native-toolbar-protocol-tests
    ${REPAPER_REPO}/extensions/reink/tests/NativeToolbarProtocolTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/palettes.qrc)
target_compile_definitions(native-toolbar-protocol-tests PRIVATE HARNESS_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(native-toolbar-protocol-tests PRIVATE Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Test)
''', encoding="utf-8")
    print(f"Prepared private protocol harness at {out}")


if __name__ == "__main__":
    main()
