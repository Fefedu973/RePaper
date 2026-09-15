#!/usr/bin/env python3
"""Compile public-Qt host tests against a private copy of AppLoad's real item."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess


def run(command, log, env=None):
    with log.open("w", encoding="utf-8") as output:
        subprocess.run(command, stdout=output, stderr=subprocess.STDOUT, env=env, check=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--appload", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=True)
    source = root / "appload"
    shutil.copytree(args.appload / "src", source / "src", dirs_exist_ok=True)
    # The workstation's Qt predates this unrelated upstream focus-policy
    # setter. The ARM shipping build uses Qt 6.10.3 without this adaptation.
    header = source / "src/qtfb/FBController.h"
    text = header.read_text(encoding="utf-8")
    text = text.replace("setFocusPolicy(Qt::StrongFocus);", "")
    header.write_text(text, encoding="utf-8")
    build = root / "build"
    here = Path(__file__).resolve().parent
    run(["cmake", "-S", str(here), "-B", str(build), "-G", "Ninja",
         f"-DAPPLOAD_SOURCE={source}", "-DCMAKE_BUILD_TYPE=Debug"], root / "configure.log")
    run(["cmake", "--build", str(build), "--parallel", "4"], root / "build.log")
    environment = dict(os.environ, QT_QPA_PLATFORM="offscreen", QT_QUICK_BACKEND="software")
    environment.pop("REPAPER_NATIVE_KEYBOARD_SOCKET", None)
    run([str(build / "native-keyboard-host-tests"), "-o", str(root / "test-results.txt") + ",txt"],
        root / "test-console.log", environment)
    (root / "validation.json").write_text(json.dumps({"passed": True,
        "scope": "Real host QQuickItem, public input-method queries/events, local socket/session isolation",
        "tablet_tested": False}, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
