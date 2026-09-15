#!/usr/bin/env python3
"""Apply the native editor patch to a privately extracted, exact firmware tree.

This performs local validation only. It never connects to or installs on a
tablet, and does not redistribute the firmware resources supplied by the user.
Requires the upstream qmldiff CLI at commit 25681c3cc7addb93fdbb41ceac1f1bdce8b2625d.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

FIRMWARE = "3.28.0.169"
EXECUTABLE_SHA = "43a9d5d0acc5b998264c16586e11b848f3b83d2d63b5fd322b09c0977d94d3d4"
SOURCES = {
    "qt/qml/xofm/libs/toolbar/qml/Toolbar.qml": "f0f552fa18a27733b59fafddc1a1cd10e35328136eb56cbb695c8799e91483f6",
    "qml/device/view/documentview/DeviceSceneView.qml": "78df7fe68f0be6e0795cde67bdb5ce5c5f355c9100f0c14d6e8157c688271a0c",
    "qml/device/view/documentview/DocumentView.qml": "867b30a725a4ae97e3459f07a60cd72bcafb21db9a0b374f02d75c5cd8d83269",
}
MARKERS = {
    "qt/qml/xofm/libs/toolbar/qml/Toolbar.qml": ("RePaperInkTool", "RePaperStencilTool", "RePaperSelectTool"),
    "qml/device/view/documentview/DeviceSceneView.qml": ("RePaperNativePageHost",),
    "qml/device/view/documentview/DocumentView.qml": (),
}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qmldiff", type=Path, required=True)
    parser.add_argument("--qml-root", type=Path, required=True)
    parser.add_argument("--hashtab", type=Path, required=True)
    parser.add_argument("--xochitl", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="New local directory; never cleared or overwritten")
    parser.add_argument("--diff", type=Path, default=Path(__file__).resolve().parents[1] / "extensions/reink/native/editor.qmd")
    args = parser.parse_args()
    if digest(args.xochitl) != EXECUTABLE_SHA:
        parser.error("Xochitl does not match the documented firmware profile")
    for relative, expected in SOURCES.items():
        if digest(args.qml_root / relative) != expected:
            parser.error(f"Extracted resource differs from the documented profile: {relative}")
    if args.output.exists():
        parser.error("Output already exists; choose a new directory to preserve earlier evidence")
    args.output.mkdir(parents=True)
    commands = [
        [str(args.qmldiff), "check-compatibility", str(args.hashtab), str(args.diff)],
        [str(args.qmldiff), "apply-diffs", "--hashtab", str(args.hashtab), "--version", FIRMWARE,
         str(args.qml_root), str(args.output), str(args.diff)],
    ]
    for index, command in enumerate(commands):
        result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=120)
        log = result.stdout + result.stderr
        (args.output / f"qmldiff-{index+1}.log").write_text(log, encoding="utf-8")
        if result.returncode:
            raise SystemExit(f"QMLDiff failed ({result.returncode}); see {args.output / f'qmldiff-{index+1}.log'}")
    generated = {path.relative_to(args.output).as_posix() for path in args.output.rglob("*.qml")}
    if generated != set(SOURCES):
        raise SystemExit(f"Unexpected set of patched resources: {sorted(generated)}")
    for relative, markers in MARKERS.items():
        modified = (args.output / relative).read_text(encoding="utf-8")
        if "~&" in modified or "~{" in modified:
            raise SystemExit(f"Unresolved QMD token in {relative}")
        for marker in markers:
            # Markers are unique objectName strings, independent of formatting
            # chosen by QMLDiff's emitter. Applying a diff can succeed even when
            # a selector does not match; verify that the objects were inserted.
            if modified.count('"' + marker + '"') != 1:
                raise SystemExit(f"Expected one inserted object {marker} in {relative}")
        if relative.endswith(("/DeviceSceneView.qml", "/DocumentView.qml")) and len(re.findall(r"readonly\s+property\s+bool\s+repaperNativeOperationPending:", modified)) != 1:
            raise SystemExit(f"Expected one native operation guard in {relative}")
        if relative.endswith("/DeviceSceneView.qml"):
            if "repaperNativeGestures" in modified:
                raise SystemExit("The retired native touch-target alias is still present")
            host = (args.diff.parent / "NativePageHost.qml").read_text(encoding="utf-8")
            if "NativeTouchRelay" in host or "repaperNativeGestures" in host:
                raise SystemExit("The native page host must not relay or retarget finger events")
            if len(re.findall(r"readonly\s+property\s+bool\s+repaperNativePreviewActive:", modified)) != 1:
                raise SystemExit("Expected one native preview activity binding")
            if len(re.findall(r"snapMode:\s*root\.repaperNativePreviewActive\s*\|\|\s*root\.penGestureHandler\?\.gestureId\s*===\s*ScenePenInputHandler\.DrawAndHoldGesture", modified)) != 1:
                raise SystemExit("Native preview did not preserve and extend ScreenDriver's snap mode binding")
    report = {
        "firmware": FIRMWARE,
        "xochitlSha256": EXECUTABLE_SHA,
        "hashtabSha256": digest(args.hashtab),
        "qmdSha256": digest(args.diff),
        "sourceSha256": SOURCES,
        "outputSha256": {relative: digest(args.output / relative) for relative in SOURCES},
        "resourcePatchApplied": True,
        "nativeRuntimeValidated": False,
        "nativeTouchRelayRemoved": True,
        "note": "Local resource validation only; does not establish runtime type, gesture, scene ABI, Undo or persistence correctness.",
    }
    (args.output / "validation.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Validated the {len(SOURCES)} native resource patches for {FIRMWARE}; runtime validation remains separate.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
