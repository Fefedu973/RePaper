#!/usr/bin/env python3
"""Exercise AppLoad -> QTFB -> evdev -> actual CPE dialog using a fake clipboard.

Use a dedicated QA profile. No real clipboard read, account login or tablet I/O.
The QA manifest is restored on exit; result JSON contains counters only.
"""
import argparse
import json
import os
import signal
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--state", type=Path, required=True)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--screenshot", type=Path, required=True)
    args = parser.parse_args()
    state = args.state.resolve()
    if "qa" not in state.name.lower():
        parser.error("Choose a dedicated profile whose directory name contains qa.")
    manifest = state / "runtime/applications_root/reagenda/external.manifest.json"
    original = manifest.read_bytes()
    data = json.loads(original)
    result = state / "keyboard-qa-result.json"
    if result.exists():
        result.unlink()
    data["args"] = data["args"][:3] + [str(args.probe.resolve()), str(result)]
    root = Path(__file__).resolve().parents[1]
    environment = dict(os.environ, REPAPER_EMULATOR_KEYBOARD_QA="1")
    environment.pop("REPAPER_EMULATOR_CLICK", None)
    try:
        manifest.write_text(json.dumps(data, indent=2) + "\n")
        process = subprocess.Popen([sys.executable, str(root / "tools/emulator-apps.py"), "--state-dir", str(state),
                                    "launch", "--app", "reagenda", "--screenshot", str(args.screenshot.resolve())], env=environment)
        try:
            code = process.wait(timeout=20)
        except subprocess.TimeoutExpired:
            # SIGINT lets the launcher's finally block stop only its own AppLoad
            # process group and bridge before the QA manifest is restored.
            process.send_signal(signal.SIGINT)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            raise
        if code:
            raise RuntimeError("AppLoad QA exited with code " + str(code))
        evidence = json.loads(result.read_text())
        if not (evidence["plainPaste"] and evidence["passwordPaste"] and evidence["clipboardRequests"] == 2
                and evidence["controlPresses"] >= 2 and evidence["ctrlVPresses"] >= 2):
            raise RuntimeError("QTFB keyboard route failed: " + json.dumps(evidence))
        print(json.dumps(evidence, sort_keys=True))
    finally:
        manifest.write_bytes(original)


if __name__ == "__main__":
    main()
