The stylus checks exercise both sides of the AppLoad input transport using the matching ARM Qt 6.10.3 runtime. No tablet or live network is used.

`run-validation.py` copies the reviewed AppLoad checkout into a fresh directory, normalizes the private source copies, applies the maintained patches, and compiles the actual `FBController` and `StylusInputHandler.qml`. It injects both Qt tablet events and Xochitl's native mouse events with a Stylus/Pen device through `QWindowSystemInterface`. The suite captures the resulting QTFB packets and verifies scaling, rotations, pressure, cancellation, overlapping windows and preserved mouse/touch behavior. The original implementation is compiled separately and must fail the native-tablet test with zero forwarded packets. Add `--native-mouse-fix` to test the complete correction; without it, the six Xochitl mouse/stylus regressions deliberately expose the previous release's failure.

The packet capture is then replayed through the actual ARM shim and corrected evdevtablet plugin. `LinuxFbStylusClient.cpp` uses an ordinary Qt Quick Button, Flickable and the shared app input adapter. The test requires one click after the pen release and two clicks after the subsequent finger tap. The native scroll capture also requires the Flickable's `contentY` to increase by at least 100. Coordinates and normalized pressure are checked before the app adapter handles the tablet events.

`audit-xochitl-mouse-contract.py` checks imports and AArch64 instruction bytes in the supplied, SHA-pinned Xochitl 3.28 executable. That firmware calls `handleMouseEvent` with a Stylus/Pen device whose only capability is Position; it does not import `handleTabletEvent`. Its native route exposes binary contact, not measured pressure. The fixture reproduces that device, a null target window and `MouseEventNotSynthesized`. Separate Qt tablet cases still validate measured pressure when that information exists.

Example commands inside WSL, with paths adjusted to the current build:

```sh
python3 packaging/appload/device/stylus-input-tests/run-validation.py \
  --appload-source .tools/rm-appload-328-candidate \
  --stage /root/repaper-appload-3282-stage-v1 \
  --native-mouse-fix \
  --output /root/repaper-stylus-validation

unshare -m -n --propagation private \
  python3 packaging/appload/device/stylus-input-tests/run-linuxfb-validation.py \
  --client /root/repaper-stylus-validation/build-after/linuxfb-stylus-client \
  --shim /root/repaper-stylus-validation/build-shim/qtfb-shim.so \
  --stage /root/repaper-appload-3282-stage-v1 \
  --packets /root/repaper-stylus-validation/native-mouse-scroll-packets.json \
  --output /root/repaper-stylus-linuxfb-validation
```

The LinuxFB runner requires private mount and network namespaces before mounting private `/tmp` and `/dev/shm`. Its input paths are private inherited file descriptors; the real device files and existing QTFB socket are never opened. All child settings and cache paths are isolated. Omitting `--packets` uses a small hand-authored protocol fixture for diagnosis; use the captured packets for the final integration check.

Four fixes are required by the observed failures:

- `stylus-input.patch` connects native tablet handlers to AppLoad's pen packets and reports the matching pressure range from the virtual digitizer.
- `native-stylus-mouse.patch` disables the tablet handler for Xochitl's native mouse/stylus path and forwards the current event coordinates through AppLoad's pen state. The old handler used a stale position on this path, causing an initial press at `(0, 0)` and lost drag updates. This route supplies binary contact pressure because the firmware mouse event has no measured pressure capability.
- The rebuilt evdevtablet plugin maps against native screen pixels so Qt's scale factor is applied once.
- `shared/input/TabletMouseAdapter.h`, enabled by `REPAPER_APPLOAD_TABLET_INPUT=1`, delivers the app's tablet gestures to controls and canvases that use mouse handlers.

Qt's [tablet event dispatch](https://github.com/qt/qtdeclarative/blob/v6.10.3/src/quick/util/qquickdeliveryagent.cpp) routes native tablet events through pointer handlers. The upstream [evdev tablet implementation](https://github.com/qt/qtbase/blob/v6.10.3/src/platformsupport/input/evdevtablet/qevdevtablethandler.cpp) supplies the pressure normalization and release behavior exercised by these checks.

The tests establish behavior in ARM Qt/QML and the shipped transport components. They do not execute proprietary Xochitl, test physical pen hardware, or create native notebooks. The host packet collector and LinuxFB receiver run as separate processes; the integration runner replays the captured packets without altering their contents.
