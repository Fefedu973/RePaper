#!/usr/bin/env python3
"""Run a native Qt executable in AppLoad's emulated framebuffer (Linux/PC only)."""
import argparse
import math
import os
from pathlib import Path
import signal
import subprocess
import sys

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--state", type=Path, required=True)
    parser.add_argument("binary", type=Path)
    args, extra = parser.parse_known_args()
    if not os.environ.get("QTFB_KEY", "").isdigit():
        parser.error("Launch this wrapper through the AppLoad PC emulator.")
    try:
        scale = float(os.environ.get("REPAPER_EMULATOR_UI_SCALE", "1.5"))
    except ValueError:
        parser.error("REPAPER_EMULATOR_UI_SCALE must be a number between 1 and 2.")
    if not math.isfinite(scale) or not 1 <= scale <= 2:
        parser.error("REPAPER_EMULATOR_UI_SCALE must be a number between 1 and 2.")
    state = args.state.resolve()
    devices = state / "runtime/input"
    devices.mkdir(exist_ok=True)
    for name in ("touch", "pen", "buttons", "keys", "framebuffer"):
        (devices / name).touch(exist_ok=True)
    # Qt's evdev specification parser accepts /dev/* names only. Existing /dev/fd
    # aliases let it open our private regular files without creating host devices.
    input_fds = {name: os.open(devices / name, os.O_RDWR) for name in ("touch", "pen", "keys")}
    evdev = {name: f"/dev/fd/{fd}" for name, fd in input_fds.items()}
    environment = os.environ.copy()
    environment.update({
        "REPAPER_PC_EMULATOR": "1",
        "REPAPER_PC_HANDOFF_HELPER": str(Path(__file__).resolve().parents[2] / "tools/pc-handoff.py"),
        "QT_QPA_PLATFORM": "linuxfb:fb=" + str(devices / "framebuffer"),
        "QT_QPA_FB_NO_LIBINPUT": "1",
        "QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS": evdev["touch"],
        "QT_QPA_EVDEV_KEYBOARD_PARAMETERS": evdev["keys"],
        "QT_QPA_EVDEV_TABLET_PARAMETERS": evdev["pen"],
        "QT_QPA_EGLFS_HIDECURSOR": "1",
        "QT_QPA_FB_HIDECURSOR": "1",
        "QT_QUICK_BACKEND": "software",
        # 1404×1872 pixels are shown in an 810×1080 PC window. Keep readable UI
        # sizes while Qt maps native evdev coordinates to logical coordinates once.
        "QT_SCALE_FACTOR": str(scale),
        "QT_FONT_DPI": "96",
        "QT_ENABLE_HIGHDPI_SCALING": "1",
        "QTFB_SHIM_MODEL": "RMPP",
        "QTFB_SHIM_MODE": "RM2FB",
        "QTFB_SHIM_FB_PATH": str(devices / "framebuffer"),
        "QTFB_SHIM_INPUT_MODE": "RMPP",
        "QTFB_SHIM_INPUT_PATH_TOUCHSCREEN": str(devices / "touch"),
        "QTFB_SHIM_INPUT_PATH_DIGITIZER": str(devices / "pen"),
        "QTFB_SHIM_INPUT_PATH_BUTTONS": str(devices / "buttons"),
        "QTFB_SHIM_INPUT_PATH_KEYS": str(devices / "keys"),
        "LD_PRELOAD": str(state / "shim-build/qtfb-shim.so"),
    })
    # Host-specific screen overrides must not multiply the framebuffer's explicit scale.
    environment.pop("QT_SCREEN_SCALE_FACTORS", None)
    environment.pop("QT_USE_PHYSICAL_DPI", None)
    # The pump has no preload, so it cannot accidentally intercept host I/O.
    pump_env = os.environ.copy()
    pump_env.pop("LD_PRELOAD", None)
    pump = subprocess.Popen([str(state / "qt-linuxfb-refresh")], env=pump_env)
    child = None
    def stop(_signal, _frame):
        if child and child.poll() is None: child.terminate()
        if pump.poll() is None: pump.terminate()
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    try:
        child = subprocess.Popen([str(args.binary.resolve()), *extra], env=environment, pass_fds=tuple(input_fds.values()))
        return child.wait()
    finally:
        stop(None, None)
        try: pump.wait(timeout=3)
        except subprocess.TimeoutExpired: pump.kill(); pump.wait()
        for fd in input_fds.values(): os.close(fd)

if __name__ == "__main__":
    raise SystemExit(main())
