#!/usr/bin/env python3
"""Stage the ARM applications and their Qt runtime; never access a tablet."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import zlib

APPS = {"remoodle": "reMoodle", "reagenda": "reAgenda", "recalc": "reCalc", "repdf": "rePDF"}
OS_LIBS = {"libc.so.6", "libm.so.6", "libdl.so.2", "libpthread.so.0", "librt.so.1",
           "libresolv.so.2", "libutil.so.1", "ld-linux-aarch64.so.1"}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def elf(path):
    data = path.read_bytes()[:20]
    return data[:6] == b"\x7fELF\x02\x01" and int.from_bytes(data[18:20], "little") == 183


def needed(path):
    result = subprocess.run(["readelf", "-d", str(path)], text=True, capture_output=True, check=True)
    return re.findall(r"\(NEEDED\).*?\[(.*?)\]", result.stdout)


def target_file(path, sysroot):
    """Resolve SDK symlinks inside the target sysroot, never into host /usr/lib."""
    for _ in range(32):
        path = Path(os.path.normpath(path))
        path.relative_to(sysroot)
        if not path.is_symlink():
            if not path.is_file():
                raise FileNotFoundError(path)
            return path
        link = Path(os.readlink(path))
        path = sysroot / link.relative_to("/") if link.is_absolute() else path.parent / link
    raise ValueError("SDK symlink cycle: " + str(path))


def icon(path, kind):
    # Original monochrome geometric icons; PNG encoding uses only stdlib.
    size = 128
    pixels = bytearray([255] * (size * size))
    def box(x0, y0, x1, y1, fill=False):
        for y in range(y0, y1):
            for x in range(x0, x1):
                if fill or min(x - x0, x1 - 1 - x, y - y0, y1 - 1 - y) < 5:
                    pixels[y * size + x] = 0
    if kind == "recalc":
        box(27, 12, 101, 116)
        box(37, 24, 91, 48)
        for row in range(3):
            for column in range(3):
                box(38 + column * 18, 59 + row * 16, 49 + column * 18, 69 + row * 16, True)
    elif kind == "repdf":
        box(26, 10, 102, 118)
        box(77, 10, 102, 35)
        for y in (48, 67, 86):
            box(39, y, 88 if y < 86 else 74, y + 6, True)
    elif kind == "reagenda":
        box(16, 25, 112, 112)
        box(16, 45, 112, 50, True)
        box(35, 12, 42, 38, True)
        box(86, 12, 93, 38, True)
        for row in range(2):
            for column in range(3):
                box(32 + column * 24, 64 + row * 24, 44 + column * 24, 76 + row * 24, True)
    else:
        box(13, 23, 66, 108)
        box(62, 23, 115, 108)
        for y in (43, 61, 79):
            box(24, y, 54, y + 5, True)
            box(76, y, 104, y + 5, True)
    def chunk(name, data):
        return struct.pack(">I", len(data)) + name + data + struct.pack(">I", zlib.crc32(name + data))
    rows = b"".join(b"\0" + pixels[y * size:(y + 1) * size] for y in range(size))
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 0, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sysroot", type=Path, required=True)
    parser.add_argument("--app-build", type=Path, required=True)
    parser.add_argument("--runtime-build", type=Path, required=True)
    parser.add_argument("--qt-plugins", type=Path, required=True)
    parser.add_argument("--shim", type=Path, required=True)
    parser.add_argument("--appload-source", type=Path, help="Reviewed AppLoad source (for its license)")
    parser.add_argument("--office-runtime", type=Path, help="Validated self-contained AArch64 office converter directory")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    sysroot = args.sysroot.resolve()
    output = args.output.absolute()
    if output.exists():
        parser.error("Use a new empty output path; staging never overwrites an earlier package")
    inputs = [args.app_build / "apps" / name / name for name in APPS]
    inputs += [args.app_build / "bridge/paper-bridge"]
    inputs += [args.runtime_build / "repaper-appload-launch", args.runtime_build / "qt-linuxfb-refresh", args.shim,
               args.qt_plugins / "platforms/libqlinuxfb.so", args.qt_plugins / "sqldrivers/libqsqlite.so",
               args.qt_plugins / "generic/libqevdevtablet.so"]
    for path in inputs:
        if not path.is_file() or not elf(path):
            parser.error("Missing AArch64 input: " + str(path))
    output.mkdir(parents=True)
    runtime = output / "runtime"
    if args.office_runtime:
        if not (args.office_runtime / "convert").is_file():
            parser.error("Office runtime must contain its convert launcher")
        shutil.copytree(args.office_runtime, runtime / "office", symlinks=True)
    origins = {}
    def copy(source, destination, executable=False):
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)
        destination.chmod(0o755 if executable else 0o644)
        origins[destination.relative_to(output).as_posix()] = str(source)
    for name, title in APPS.items():
        folder = output / name
        copy(args.app_build / "apps" / name / name, folder / name, True)
        manifest = {"name": title, "application": "../runtime/repaper-appload-launch", "args": ["./" + name],
                    "qtfb": True, "disablesWindowedMode": name not in ("recalc", "repdf"), "supportsVirtualKeyboard": False,
                    "aspectRatio": "original"}
        (folder / "external.manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
        icon(folder / "icon.png", name)
    for name in ("repaper-appload-launch", "qt-linuxfb-refresh"):
        copy(args.runtime_build / name, runtime / name, True)
    copy(args.shim, runtime / "qtfb-shim.so")
    copy(args.app_build / "bridge/paper-bridge", runtime / "paper-bridge", True)
    for name in ("bridge-start", "bridge-run"):
        copy(root / "packaging/appload/device" / name, runtime / name, True)
        script = runtime / name
        script.write_bytes(script.read_bytes().replace(b'\r\n', b'\n'))
    for relative in ("platforms/libqlinuxfb.so", "sqldrivers/libqsqlite.so"):
        copy(args.qt_plugins / relative, runtime / "plugins" / relative)
    copy(args.qt_plugins / "generic/libqevdevtablet.so", runtime / "plugins/generic/libqevdevtabletplugin.so")
    for category, names in {"generic": ["libqevdevkeyboardplugin.so", "libqevdevtouchplugin.so"],
                            "imageformats": ["libqjpeg.so", "libqsvg.so", "libqgif.so"],
                            "tls": ["libqopensslbackend.so"]}.items():
        for name in names:
            copy(sysroot / "usr/lib/plugins" / category / name, runtime / "plugins" / category / name)
    # Only Qt's open-source modules. No xofm, ark, Xochitl or other device QML.
    qml = sysroot / "usr/lib/qml"
    for module in ("QtQml", "QtQuick", "QtCore", "QML"):
        for source in sorted((qml / module).rglob("*")):
            if source.is_file() and (source.name == "qmldir" or source.suffix in (".qml", ".js", ".qmltypes", ".so")):
                copy(source, runtime / "qml" / source.relative_to(qml))
    fonts = Path("/usr/share/fonts/truetype/dejavu")
    for name in ("DejaVuSans.ttf", "DejaVuSans-Bold.ttf", "DejaVuSansMono.ttf"):
        copy(fonts / name, runtime / "fonts" / name)
    (runtime / "fonts/fonts.conf").write_text(
        '<?xml version="1.0"?><!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">\n'
        '<fontconfig><dir prefix="relative">.</dir><cachedir prefix="xdg">fontconfig</cachedir></fontconfig>\n')
    copy(sysroot / "etc/ssl/certs/ca-certificates.crt", runtime / "certs/ca-certificates.crt")
    appload_source = args.appload_source or root / ".tools/rm-appload-328-candidate"
    copy(appload_source / "LICENSE", runtime / "licenses/AppLoad-GPL-3.0.txt")
    copy(Path("/usr/share/doc/fonts-dejavu-core/copyright"), runtime / "licenses/DejaVu-copyright.txt")
    for source in (root / "packaging/appload/device/qt-linuxfb/LICENSES").iterdir():
        if source.is_file():
            copy(source, runtime / "licenses" / source.name)
    # Close all dynamically linked dependencies over the target SDK, keeping
    # glibc and its loader supplied by the matching tablet OS.
    queue = [path for path in output.rglob("*") if path.is_file() and elf(path) and "office" not in path.relative_to(output).parts]
    libraries = {}
    os_needed = set()
    edges = {}
    for binary in queue:
        edges[binary.relative_to(output).as_posix()] = needed(binary)
        for soname in edges[binary.relative_to(output).as_posix()]:
            if soname in OS_LIBS:
                os_needed.add(soname)
                continue
            if soname in libraries:
                continue
            source = None
            for directory in (sysroot / "usr/lib", sysroot / "lib"):
                candidate = directory / soname
                if candidate.exists() or candidate.is_symlink():
                    source = target_file(candidate, sysroot)
                    break
            if source is None or not elf(source):
                raise ValueError("Unresolved target dependency: " + soname + " needed by " + str(binary))
            destination = runtime / "lib" / soname
            copy(source, destination)
            libraries[soname] = digest(destination)
            queue.append(destination)
    report = {"schemaVersion": 1, "applications": list(APPS), "architecture": "aarch64", "qtVersion": "6.10.3",
              "target": "Paper Pro ferrari 3.28.0.169", "deviceInstallationPerformed": False,
              "runtimeValidatedOnDevice": False, "bridgeIncluded": True, "dependencies": edges,
              "osSuppliedLibraries": sorted(os_needed),
              "runtimeLoadedOsLibraries": {"repdf": ["libpdfium.so"]},
              "libraries": libraries, "origins": origins,
              "files": {path.relative_to(output).as_posix(): digest(path)
                        for path in sorted(output.rglob("*")) if path.is_file()}}
    (output / "package-manifest.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps({"output": str(output), "apps": list(APPS), "elfFiles": len(queue),
                      "libraries": len(libraries), "files": len(report["files"]),
                      "bytes": sum(path.stat().st_size for path in output.rglob("*") if path.is_file())}, indent=2))


if __name__ == "__main__":
    main()
