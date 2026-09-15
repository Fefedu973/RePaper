#!/usr/bin/env python3
"""Create a source-inclusive local native editor test package; never installs it."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile

PIN = "2b99649f5e4fd6288be7792a8570bd16418adb70"
SCENE_PIN = "8afbac01ca7816f9aa22a5897a3f2133b0b0d9d7"
SCENE_FILES = ("rm_Line.cpp", "rm_Line.hpp", "rm_SceneItem.hpp", "LICENSE.txt", "README.MD")


def pinned_files(checkout, pin, names):
    revision = subprocess.check_output(["git", "-C", str(checkout), "rev-parse", "HEAD"], text=True).strip()
    if revision != pin:
        raise ValueError("Upstream checkout is not at the documented commit")
    for name in names:
        original = subprocess.check_output(["git", "-C", str(checkout), "show", pin + ":" + name])
        if (checkout / name).read_bytes().replace(b"\r\n", b"\n") != original.replace(b"\r\n", b"\n"):
            raise ValueError("Pinned upstream file differs: " + name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--xovi-source", type=Path, required=True)
    parser.add_argument("--scene-source", type=Path)
    parser.add_argument("--qmd-validation", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    own = Path(__file__).resolve().parent
    repo = own.parents[1]
    scene = args.scene_source or repo / ".tools/xovi-scene-assistant"
    validation = args.qmd_validation or repo / ".local/qa/native-qmd-v081/validation.json"
    binary = args.build_dir / "reink-editor.so"
    data = binary.read_bytes()
    if data[:6] != b"\x7fELF\x02\x01" or int.from_bytes(data[18:20], "little") != 183:
        parser.error("Only the little-endian ELF64 AArch64 module may be packaged")
    qmd = (own / "native/editor.qmd").read_bytes()
    evidence = json.loads(validation.read_text(encoding="utf-8"))
    if not evidence.get("resourcePatchApplied") or evidence.get("qmdSha256") != hashlib.sha256(qmd).hexdigest():
        parser.error("Offline exact-resource validation is missing or does not match the current QMD")
    if qmd not in data:
        parser.error("Module does not embed the current validated QMD; rebuild it")
    for resource in ("editor_panels.rcc", "native_resources.rcc"):
        if (args.build_dir / resource).read_bytes() not in data:
            parser.error("Module does not embed its current RCC resource; rebuild it")
    try:
        pinned_files(args.xovi_source, PIN, ("util/xovigen.py", "LICENSE"))
        pinned_files(scene, SCENE_PIN, SCENE_FILES)
    except ValueError as error:
        parser.error(str(error))
    generator = args.xovi_source / "util/xovigen.py"
    files = {"native/reink-editor.so": binary,
             "README.md": own / "README.md",
             "PROVENANCE.md": own / "PROVENANCE.md",
             "validation/qmd-offline.json": validation,
             "third-party/xovi/xovigen.py": generator,
             "third-party/xovi/LICENSE": args.xovi_source / "LICENSE",
             "third-party/xovi/generated/xovi.c": args.build_dir / "xovi.c",
             "third-party/xovi/generated/xovi.h": args.build_dir / "xovi.h"}
    for resource in ("editor_panels.rcc", "native_resources.rcc"):
        files["third-party/xovi/generated/" + resource] = args.build_dir / resource
    for directory in (own, own.parent / "editor-common"):
        for source in sorted(directory.rglob("*")):
            if source.is_file() and "__pycache__" not in source.parts:
                files["src/extensions/" + directory.name + "/" + source.relative_to(directory).as_posix()] = source
    for path in ("apps/restencil/src/Geometry.cpp", "apps/restencil/src/Geometry.h", "shared/drawing/DrawingModel.h"):
        files["src/" + path] = repo / path
    for source in sorted((repo / "shared/drawing").rglob("*")):
        if source.is_file() and source.suffix in (".h", ".cpp", ".md", ".txt"):
            files["src/" + source.relative_to(repo).as_posix()] = source
    for name in SCENE_FILES:
        files["third-party/scene-assistant/" + name] = scene / name
    # Bundles make the exact git-verified source checkouts reproducible offline.
    # They contain upstream public history only, never local user/device files.
    with tempfile.TemporaryDirectory(prefix="repaper-native-package-") as temp:
        for label, checkout, pin in (("xovi", args.xovi_source, PIN), ("scene-assistant", scene, SCENE_PIN)):
            bundle = Path(temp) / (label + ".bundle")
            subprocess.run(["git", "-C", str(checkout), "bundle", "create", str(bundle), "HEAD"], check=True,
                           stdout=subprocess.DEVNULL)
            files["third-party/" + label + ".bundle"] = bundle
        manifest = {"schemaVersion": 2, "packageKind": "native-editor-selection-test", "packageVersion": "0.8.2", "architecture": "aarch64",
                    "nativeEditingEnabledByDefault": True, "nativeEditingEnabledByConfiguration": False,
                    "nativeCreationEnabledByDefaultOnExactTarget": True,
                    "nativeCreationEnabledByExplicitSessionAction": False,
                    "nativePersistentEditingEnabled": False,
                    "nativeLocalObjectMembershipPersistenceImplemented": True,
                    "nativeLocalObjectMembershipScope": "local-explicit-new-object-membership;native-relocation-lineages;physical-qa-pending",
                    "nativeSemanticEndpointEditingImplemented": True,
                    "nativeOrientedObjectHandlesImplemented": True,
                    "nativeParametricStrokeWidthPreserved": True,
                    "nativeAspectRatioHoldImplemented": True,
                    "nativeAspectRatioHoldMs": 1000,
                    "nativeAspectRatioHoldViewRadius": 8,
                    "nativeDirectSelectionButtonImplemented": True,
                    "nativePropertiesSessionCancelImplemented": True,
                    "nativePropertiesSessionMaximumEdits": 128,
                    "nativeCreationFinishesDeselected": True,
                    "nativeDynamicAttachedConnectorsImplemented": False,
                    "nativeAffineSelectionEditingImplemented": True,
                    "nativeSelectionStrokeColorImplemented": True,
                    "nativePreviewDriverGateImplemented": True,
                    "nativePreviewFirstFrameImmediate": True,
                    "nativePreviewCooldownMs": 16,
                    "nativeSolidArrowSingleStrokeImplemented": True,
                    "nativePenBatchInputImplemented": True,
                    "nativeTouchRelayImplemented": False,
                    "nativeFingerDrawingEnabled": False,
                    "nativeFingerNavigationMode": "original-native-target;no-relay",
                    "nativeActiveSelectionRepairImplemented": True,
                    "nativeWholeObjectBindingsImplemented": True,
                    "nativePortAndWireSegmentSnapImplemented": True,
                    "nativeColorAndDuplicateBindingPersistenceImplemented": True,
                    "nativeHistoryMacroTailValidated": True,
                    "nativeDeviceValidation": "0.8.2-page-readiness-fix-Carnet-2-open-user-confirmed;other-editing-workflows-not-revalidated",
                    "installerIncluded": False, "installsAnythingWhenBuilt": False,
                    "xoviCommit": PIN, "sceneAssistantCommit": SCENE_PIN,
                    "compatibility": json.loads((own / "compatibility.json").read_text(encoding="utf-8")),
                    "sha256": {name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in files.items()}}
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with tarfile.open(args.output, "w:gz") as package:
            for name, source in files.items():
                package.add(source, arcname=name)
            content = (json.dumps(manifest, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
            info = tarfile.TarInfo("manifest.json")
            info.size = len(content)
            info.mode = 0o644
            package.addfile(info, io.BytesIO(content))
    print(f"Created {args.output} (native creation and custom selection test; manual installation only)")


if __name__ == "__main__":
    main()
