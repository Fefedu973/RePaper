#!/usr/bin/env python3
"""Verify a local native-editor archive without extracting or installing it."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import tarfile


def digest(data):
    return hashlib.sha256(data).hexdigest()


def verify(archive, loose_module, source_root=None):
    with tarfile.open(archive, "r:gz") as package:
        members = package.getmembers()
        names = [member.name for member in members]
        if len(set(names)) != len(names):
            raise ValueError("Duplicate archive paths")
        for member in members:
            path = PurePosixPath(member.name)
            if not member.isfile() or path.is_absolute() or ".." in path.parts or "\\" in member.name:
                raise ValueError("Unexpected archive entry: " + member.name)
            if member.size > 100 * 1024 * 1024:
                raise ValueError("Oversize archive entry: " + member.name)
        if sum(member.size for member in members) > 250 * 1024 * 1024:
            raise ValueError("Oversize package")
        manifest = json.load(package.extractfile("manifest.json"))
        hashes = manifest["sha256"]
        if set(hashes) != set(names) - {"manifest.json"}:
            raise ValueError("Manifest and archive file lists differ")
        checked_sources = 0
        for name, expected in hashes.items():
            content = package.extractfile(name).read()
            if digest(content) != expected:
                raise ValueError("Hash mismatch: " + name)
            if source_root and name.startswith("src/"):
                local = source_root.joinpath(*PurePosixPath(name).parts[1:])
                if not local.is_file() or local.read_bytes() != content:
                    raise ValueError("Packaged source differs from workspace: " + name)
                checked_sources += 1
        module = package.extractfile("native/reink-editor.so").read()
        if module[:6] != b"\x7fELF\x02\x01" or int.from_bytes(module[18:20], "little") != 183:
            raise ValueError("Module is not little-endian ELF64 AArch64")
        if loose_module.read_bytes() != module:
            raise ValueError("Loose module differs from archive")
        for name in ("editor_panels.rcc", "native_resources.rcc"):
            resource = package.extractfile("third-party/xovi/generated/" + name).read()
            if not resource or resource not in module:
                raise ValueError("Module does not contain packaged resource: " + name)
        qmd = package.extractfile("src/extensions/reink/native/editor.qmd").read()
        qmd_evidence = json.load(package.extractfile("validation/qmd-offline.json"))
        if qmd not in module or qmd_evidence["qmdSha256"] != digest(qmd) or not qmd_evidence["resourcePatchApplied"]:
            raise ValueError("Embedded QMD and offline validation differ")
        return {
            "version": manifest.get("packageVersion", manifest["compatibility"]["version"]),
            "moduleSha256": digest(module),
            "archiveSha256": digest(archive.read_bytes()),
            "archiveFileHashesVerified": len(hashes),
            "workspaceSourcesVerified": checked_sources,
            "architecture": "aarch64",
            "nativeEditingEnabledByDefault": manifest["nativeEditingEnabledByDefault"],
            "nativeCreationEnabledByDefaultOnExactTarget": manifest.get("nativeCreationEnabledByDefaultOnExactTarget", False),
            "nativeCreationEnabledByExplicitSessionAction": manifest.get("nativeCreationEnabledByExplicitSessionAction", False),
            "nativePersistentEditingEnabled": manifest.get("nativePersistentEditingEnabled", False),
            "deviceValidation": manifest["nativeDeviceValidation"],
            "installsAnything": False,
        }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("--module", required=True, type=Path)
    parser.add_argument("--source-root", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        report = verify(args.archive, args.module, args.source_root)
    except (ValueError, KeyError, OSError, tarfile.TarError) as error:
        parser.error(str(error))
    text = json.dumps(report, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text, encoding="utf-8")
    print(text, end="")


if __name__ == "__main__":
    main()
