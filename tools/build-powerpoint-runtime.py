#!/usr/bin/env python3
"""Prepare a relocatable, headless LibreOffice runtime without installing packages.

Run on Linux with apt-get, dpkg-deb, gpg and Python 3. The ARM package uses the
tablet's glibc (the 5.8 SDK has glibc 2.39); Debian's loader/libc are never shipped.
All APT state, downloads and extraction stay below --work. No device is contacted.
"""
from pathlib import Path
import argparse
import hashlib
import gzip
import json
import os
import shutil
import subprocess
import tarfile
import urllib.request

KEYS = [
    ("archive-key-12.asc", "B8B80B5B623EAB6AD8775C45B7C5D7D6350947F8"),
    ("archive-key-12-security.asc", "05AB90340C0C5E797F44A8C8254CF3B5AEC0A8F0"),
    ("archive-key-13.asc", "04B54C3CDCA79751B16BC6B5225629DF75B188BD"),
]
SYSTEM_LIBRARIES = {
    "libc.so.6", "libm.so.6", "libdl.so.2", "libpthread.so.0", "librt.so.1",
    "libresolv.so.2", "libutil.so.1", "ld-linux-aarch64.so.1", "ld-linux-x86-64.so.2",
    "libanl.so.1", "libBrokenLocale.so.1", "libnss_files.so.2", "libnss_dns.so.2",
    "libnss_compat.so.2", "libnsl.so.1", "libmvec.so.1",
}


def sha256(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest() if hasattr(hashlib, "file_digest") else hashlib.sha256(source.read()).hexdigest()


def download(work, arch):
    for name in ("lists/partial", "archives/partial", "gnupg", "rootfs"):
        (work / name).mkdir(parents=True, exist_ok=True)
    (work / "gnupg").chmod(0o700)
    (work / "status").touch()
    armored = b""
    for name, fingerprint in KEYS:
        path = work / name
        if not path.exists():
            with urllib.request.urlopen("https://ftp-master.debian.org/keys/" + name, timeout=30) as response:
                path.write_bytes(response.read())
        check = subprocess.check_output(["gpg", "--homedir", str(work / "gnupg"), "--batch",
            "--show-keys", "--with-colons", str(path)], stderr=subprocess.DEVNULL, text=True)
        primary = [line for line in check.splitlines() if line.startswith("pub:")]
        fingerprints = [line.split(':')[9] for line in check.splitlines() if line.startswith("fpr:")]
        if len(primary) != 1 or not fingerprints or fingerprints[0] != fingerprint:
            raise RuntimeError("Unexpected Debian archive signing key")
        armored += path.read_bytes() + b"\n"
    keyring = work / "debian-archive.gpg"
    keyring.write_bytes(subprocess.check_output(["gpg", "--homedir", str(work / "gnupg"),
        "--batch", "--dearmor"], input=armored))
    sources = "\n".join("deb [arch=" + arch + " signed-by=" + str(keyring) + "] " + site + " " + suite + " main"
        for site, suite in [("https://deb.debian.org/debian", "bookworm"),
            ("https://deb.debian.org/debian", "bookworm-backports"),
            ("https://security.debian.org/debian-security", "bookworm-security")]) + "\n"
    (work / "sources.list").write_text(sources)
    options = ["-o", "Dir::State::lists=" + str(work / "lists"),
        "-o", "Dir::State::status=" + str(work / "status"),
        "-o", "Dir::Cache::archives=" + str(work / "archives"),
        "-o", "Dir::Etc::sourcelist=" + str(work / "sources.list"),
        "-o", "Dir::Etc::sourceparts=-", "-o", "Dir::Etc::main=-", "-o", "Dir::Etc::parts=-",
        "-o", "APT::Architecture=" + arch,
        "-o", "APT::Architectures::=" + arch, "-o", "Debug::NoLocking=true",
        "-o", "APT::Sandbox::User=root", "-o", "Acquire::Languages=none"]
    with (work / "apt-update.log").open("w") as log:
        subprocess.run(["apt-get", *options, "update"], stdout=log, stderr=subprocess.STDOUT, check=True)
    with (work / "apt-download.log").open("w") as log:
        subprocess.run(["apt-get", *options, "-t", "bookworm-backports", "--download-only",
            "--no-install-recommends", "--yes", "install", "libreoffice-impress-nogui",
            "libreoffice-math-nogui", "fonts-liberation2", "fonts-texgyre-math"],
            stdout=log, stderr=subprocess.STDOUT, check=True)


def build(work, output, arch, reuse):
    work.mkdir(parents=True, exist_ok=True)
    archive_path = Path(str(output) + ".tar.gz")
    if output.exists() or archive_path.exists():
        raise RuntimeError("Output already exists; choose a new output directory")
    if work == Path("/") or output == work or (work / "rootfs") in output.parents:
        raise RuntimeError("Choose separate private work and output directories")
    if not reuse:
        download(work, arch)
    tree = work / "rootfs"
    tree.mkdir(exist_ok=True)
    archives = sorted((work / "archives").glob("*.deb"))
    if not archives:
        raise RuntimeError("No downloaded Debian archives")
    previous = work / "packages.json"
    expected = {entry["file"]: entry["sha256"] for entry in json.loads(previous.read_text())} if reuse and previous.exists() else {}
    manifest = []
    for archive in archives:
        digest = sha256(archive)
        if reuse and expected and expected.get(archive.name) != digest:
            raise RuntimeError("Archive differs from the recorded package lock: " + archive.name)
        info = subprocess.check_output(["dpkg-deb", "--show", "--showformat=${Package}\t${Version}\t${Architecture}\t${Installed-Size}\t${Source}", str(archive)], text=True).split("\t")
        manifest.append(dict(file=archive.name, package=info[0], version=info[1], architecture=info[2],
            installedKiB=int(info[3]), source=info[4] or info[0], bytes=archive.stat().st_size, sha256=digest))
        subprocess.run(["dpkg-deb", "--extract", str(archive), str(tree)], check=True)
    (work / "packages.json").write_text(json.dumps(manifest, indent=2) + "\n")
    output.mkdir(parents=True)
    # Retain resources and registry topology; Debian's maintainers normally create
    # main.xcd during installation, which isolated extraction deliberately skips.
    for relative in ("usr/lib/libreoffice", "usr/share/libreoffice", "usr/share/fonts", "usr/share/texmf/fonts", "etc/libreoffice", "usr/share/doc/libreoffice-common/examples"):
        source = tree / relative
        if source.exists():
            shutil.copytree(source, output / "rootfs" / relative, symlinks=True)
    registry = output / "rootfs/etc/libreoffice/registry"
    for source in (output / "rootfs/usr/lib/libreoffice/share/.registry").glob("*.xcd"):
        shutil.copy2(source, registry / source.name)
    for relative in ("var/lib/libreoffice/share/prereg/bundled", "var/spool/libreoffice/uno_packages/cache"):
        (output / "rootfs" / relative).mkdir(parents=True, exist_ok=True)
    libraries = output / "libraries"
    libraries.mkdir()
    triplet = "aarch64-linux-gnu" if arch == "arm64" else "x86_64-linux-gnu"
    for directory in (tree / "usr/lib" / triplet, tree / "lib" / triplet):
        for source in directory.glob("*.so*"):
            if source.name in SYSTEM_LIBRARIES or not source.is_file():
                continue
            target = source.resolve()
            try:
                target.relative_to(tree.resolve())
            except ValueError:
                raise RuntimeError("A dependency symlink escapes the extracted package root: " + str(source))
            destination = libraries / target.name
            if destination.exists():
                if sha256(destination) != sha256(target):
                    raise RuntimeError("Conflicting runtime library: " + target.name)
            else:
                shutil.copy2(target, destination)
            alias = libraries / source.name
            if source.name != target.name and not alias.exists():
                alias.symlink_to(target.name)
    # Rebase absolute links inside the copied Debian tree. Nothing can point back
    # to the build host's /usr, /etc or private temporary directories.
    for path in (output / "rootfs").rglob("*"):
        if path.is_symlink() and os.readlink(path).startswith("/"):
            target = output / "rootfs" / os.readlink(path).lstrip("/")
            path.unlink()
            path.symlink_to(os.path.relpath(target, path.parent))
    # Copyright/license notices remain with every downloaded dependency. Keep the
    # package lock so redistribution can also recover exact Debian source versions.
    notices = output / "licenses"
    notices.mkdir()
    for entry in manifest:
        source = tree / "usr/share/doc" / entry["package"]
        if source.exists():
            for name in ("copyright", "changelog.Debian.gz"):
                path = source / name
                if path.is_file():
                    shutil.copy2(path, notices / (entry["package"] + "-" + name))
    common = tree / "usr/share/common-licenses"
    if common.exists():
        shutil.copytree(common, notices / "common-licenses", symlinks=False)
    (output / "packages.json").write_text(json.dumps(manifest, indent=2) + "\n")
    (output / "fonts.conf").write_text('''<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">
<fontconfig>
  <dir prefix="relative">rootfs/usr/share/fonts</dir>
  <dir prefix="relative">rootfs/usr/share/texmf/fonts</dir>
  <cachedir prefix="xdg">fontconfig</cachedir>
  <alias><family>Arial</family><prefer><family>Liberation Sans</family></prefer></alias>
  <alias><family>Calibri</family><prefer><family>Liberation Sans</family></prefer></alias>
  <alias><family>Times New Roman</family><prefer><family>Liberation Serif</family></prefer></alias>
  <alias><family>Cambria Math</family><prefer><family>TeX Gyre Termes Math</family></prefer></alias>
</fontconfig>
''')
    (output / "convert").write_text('''#!/bin/sh
set -eu
runtime_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
office_root="$runtime_root/rootfs/usr/lib/libreoffice"
# This environment belongs only to this exec'd converter, never to AppLoad/Qt.
unset LD_PRELOAD QTFB_KEY
export LD_LIBRARY_PATH="$office_root/program:$runtime_root/libraries"
export SAL_USE_VCLPLUGIN=svp SAL_DISABLE_OPENCL=1 SAL_DISABLE_JAVA=1
export FONTCONFIG_FILE="$runtime_root/fonts.conf"
export XDG_CACHE_HOME="$PWD/cache"
export TMPDIR="$PWD/tmp"
mkdir -p -- "$XDG_CACHE_HOME" "$TMPDIR"
office_uri=$(printf '%s' "$office_root" | sed -e 's/%/%25/g' -e 's/ /%20/g' -e 's/#/%23/g' -e 's/?/%3F/g')
exec "$office_root/program/soffice.bin" "-env:BRAND_BASE_DIR=file://$office_uri" "$@"
''')
    (output / "convert").chmod(0o755)
    files = [path for path in output.rglob("*") if path.is_file() and not path.is_symlink()]
    (output / "SHA256SUMS").write_text("".join(sha256(path) + "  " + str(path.relative_to(output)) + "\n" for path in sorted(files)))
    def normalize(info):
        info.uid = info.gid = 0
        info.uname = info.gname = ""
        info.mtime = 0
        return info
    with archive_path.open("wb") as raw, gzip.GzipFile(filename="", fileobj=raw, mode="wb", compresslevel=6, mtime=0) as compressed:
        with tarfile.open(fileobj=compressed, mode="w") as tar:
            tar.add(output, arcname="repaper-office", recursive=True, filter=normalize)
    print(json.dumps(dict(runtime=str(output), archive=str(archive_path), architecture=arch,
        runtimeBytes=sum(path.stat().st_size for path in files), archiveBytes=archive_path.stat().st_size,
        archiveSha256=sha256(archive_path), packages=len(manifest)), indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "amd64"), default="arm64")
    parser.add_argument("--reuse-downloads", action="store_true", help="Verify and reuse packages.json + cached archives without network")
    arguments = parser.parse_args()
    build(arguments.work.resolve(), arguments.output.resolve(), arguments.arch, arguments.reuse_downloads)
