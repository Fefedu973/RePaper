# AppLoad tablet input for Qt Quick controls

`TabletMouseAdapter.h` provides the application-side pointer adapter used by
reMoodle, reAgenda, and reCalc when launched through AppLoad. Call
`repaper::installTabletMouseAdapter()` after constructing `QGuiApplication`.
It installs one adapter only when `REPAPER_APPLOAD_TABLET_INPUT=1` and parents
it to the application. The header uses public Qt Gui and Qt Quick APIs and
does not require a MOC compilation step.

Pen and eraser contact events become left mouse gestures delivered to the
receiving `QWindow`, including its Qt Quick controls. The adapter preserves
logical coordinates and keyboard modifiers. It tracks the window and pen
that started a gesture, releases outside the window, and cancels grabs when
the window hides, closes, deactivates, or is destroyed. A window hidden from
its own pressed handler is cancelled after that handler finishes.

Tablet events consumed by the adapter cannot trigger Qt's second mouse
synthesis path. Physical mouse and touch events retain their existing delivery
paths. The controls receive mouse events; pressure is tested separately in the
AppLoad tablet transport and corrected evdev tablet plugin.

## Tests

The standalone suite creates real Qt Quick `Button`, `MouseArea`, `Flickable`,
and `Popup` controls. It exercises pen and eraser input, fractional logical
coordinates, modifiers, dragging, scrolling, multiple windows, cancellation,
release without a preceding outside move, and physical mouse/touch input.

Host Qt build:

```sh
cmake -S shared/input/tests -B /root/repaper-tablet-mouse-tests -G Ninja
cmake --build /root/repaper-tablet-mouse-tests --parallel 4
ctest --test-dir /root/repaper-tablet-mouse-tests --output-on-failure
```

Matching SDK ARM64 build and QEMU test with Qt 6.10.3:

```sh
bash shared/input/tests/run-arm-tests.sh \
  /root/repaper-tablet-mouse-tests-arm /root/repaper-appload-328-stage-v1
```

The ARM test uses the SDK's offscreen platform and the staged public Qt QML
modules. It produces text and JUnit reports plus the adapter source checksum
in the selected build directory. Both host Qt 6.2.4 and ARM Qt 6.10.3 passed
20 QtTest cases. The separate `packaging/appload/device/stylus-input-tests/`
suite also verifies real FBController packets through the ARM shim, LinuxFB,
evdev tablet plugin, and adapter: one pen click followed by one touch click.
