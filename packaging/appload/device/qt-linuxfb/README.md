# Qt 6.10.3 plugins for the AppLoad application bundle

This directory rebuilds the LinuxFB platform plugin and SQLite SQL driver that
are absent from the reMarkable 5.8.203 ARM64 SDK's plugin directories, and an
evdev tablet input plugin with a coordinate correction for scaled applications.
LinuxFB and SQLite use unaltered Qt 6.10.3 implementation sources. The tablet
plugin uses the same upstream version with the small patch described below.
All three link matching SDK libraries. The standalone CMake project replaces
Qt's internal monorepo build entry points.

## Source and license

- Upstream: <https://github.com/qt/qtbase>
- Tag: `v6.10.3`
- Commit: `7ddbc87d8e14ce51d2957ea72d0a6077593d5ff4`
- `upstream/`: `src/plugins/platforms/linuxfb/` from that commit. DRM sources are
  omitted because the SDK disables KMS.
- `upstream-sqlite/`: `src/plugins/sqldrivers/sqlite/` from that commit.
- `upstream-evdevtablet/plugin/`: `src/plugins/generic/evdevtablet/` from that
  commit; `upstream-evdevtablet/handler/`: the four implementation/header files
  from `src/platformsupport/input/evdevtablet/`.
- `upstream.sha256` records SHA-256 checksums for all vendored upstream files
  and license texts. Builds verify these before compiling and need no network.
- Upstream C++ sources retain their copyright and SPDX notices:
  `LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only`.
  The upstream build files use `BSD-3-Clause`. Applicable open-source license
  texts are included in `LICENSES/`.

The LinuxFB plugin links the SDK's `libQt6FbSupport.a`,
`libQt6InputSupport.a`, and `libQt6DeviceDiscoverySupport.a`. These matching
private support archives and private headers are supplied by SDK 5.8.203; they
are prerequisites rather than copies in this source directory. SQLite uses
the SDK's shared system SQLite 3.45.3 library. Rebuilding against another Qt
private ABI is unsupported. Preserve these sources, notices, and build
instructions alongside the plugins in source distributions.

## Offline build and validation

From WSL Ubuntu, with SDK 5.8.203 installed:

```sh
bash packaging/appload/device/qt-linuxfb/build-arm.sh /root/repaper-appload-qtplugins-arm-v2
```

Set `REPAPER_SDK_DIR` if the same SDK is installed elsewhere. The script sources
its cross compiler environment and selects its Qt toolchain. It produces:

```text
/root/repaper-appload-qtplugins-arm-v2/plugins/platforms/libqlinuxfb.so
/root/repaper-appload-qtplugins-arm-v2/plugins/sqldrivers/libqsqlite.so
/root/repaper-appload-qtplugins-arm-v2/plugins/generic/libqevdevtablet.so
```

All three plugins use `-Wl,--no-undefined` and a relative runtime search path
of `$ORIGIN/../../lib`. The QSQLITE target defines `QT_USE_QSTRINGBUILDER`, as
Qt's own `cmake/QtInternalTargets.cmake` does for internal plugin targets. No
SDK feature-header changes are required. The previous LinuxFB/SQLite outputs
in `/root/repaper-appload-linuxfb-arm` are left unchanged; these two plugins
retain identical SHA-256 checksums in the new build directory.

The script reports ELF headers/dependencies and runs `validate-plugins`
under the SDK's AArch64 QEMU user emulator. Validation checks the LinuxFB
and evdev tablet plugin metadata and object loading, then opens a `QSQLITE` memory database,
commits a transaction, and checks a prepared Unicode insert/select. It never
creates a platform integration or accesses a tablet/device. The ELF checks
and smoke validation passed with SDK 5.8.203 / Qt 6.10.3. Their reports are
`elf-report.txt`, `plugins.sha256`, and `smoke.log` in the selected build
directory.

Direct dynamic dependencies:

| Plugin | Shared libraries |
| --- | --- |
| LinuxFB | `libQt6Gui.so.6`, `libQt6Core.so.6`, `libudev.so.1`, `libstdc++.so.6`, `libm.so.6`, `libgcc_s.so.1`, `libc.so.6`, `ld-linux-aarch64.so.1` |
| SQLite | `libsqlite3.so.0`, `libQt6Sql.so.6`, `libQt6Core.so.6`, `libstdc++.so.6`, `libgcc_s.so.1`, `libc.so.6` |
| EvdevTablet | `libQt6Gui.so.6`, `libQt6Core.so.6`, `libudev.so.1`, `libstdc++.so.6`, `libgcc_s.so.1`, `libc.so.6`, `ld-linux-aarch64.so.1` |

## Tablet coordinate correction

Qt 6.10.3's `QEvdevTabletData::report()` multiplies normalized pen coordinates
by `QScreen::geometry()`, which uses logical pixels. It passes the result to
`QWindowSystemInterface::handleTabletEvent()`, which expects native pixels and
converts them to logical pixels. With `QT_SCALE_FACTOR=2`, that combination
divides the position twice: framebuffer position `(400,300)` arrives near
logical `(100,75)` instead of `(200,150)`.

`evdevtablet-native-coordinates.patch` adds the platform-screen header and uses
`QPlatformScreen::geometry()` for the native pixel dimensions. It does not
change pressure, button state, device discovery, or input-device access. The
source files in `upstream-evdevtablet/` remain byte-identical to the pinned Qt
commit. CMake copies them into `evdevtablet-patched/` inside the selected build
directory and applies the patch there with zero fuzz. The target compiles its
own tablet manager and handler; other support code comes from the SDK. The
SDK's installed plugin and support archives are not modified.

Package this rebuilt `generic/libqevdevtablet.so` in place of the SDK's
`generic/libqevdevtabletplugin.so`, renaming the rebuilt file to the SDK name
or excluding the original. Do not ship both in one plugin directory: they
advertise the same `EvdevTablet` key. The existing Qt copyright/SPDX notices and LGPL/GPL
license texts apply to the patched implementation too. Keep the original
sources, the patch, checksums, and build instructions with source distributions.

## Runtime integration

The parent application bundle supplies the private framebuffer/input bridge,
matching Qt libraries, generic evdev plugins, and display refreshes. For the
unmodified LinuxFB plugin to use only those private paths, its launcher uses:

```text
QT_QPA_PLATFORM=linuxfb:fb=<private-framebuffer-file>:tty=<private-null-file>:nographicsmodeswitch
QT_QPA_FB_DISABLE_INPUT=1
QT_QPA_PRESERVE_CONSOLE_STATE=1
QT_QPA_NO_SIGNAL_HANDLER=1
QT_QPA_GENERIC_PLUGINS=<explicit permitted evdev plugins with /dev/fd/N paths>
```

The launcher also attaches a non-terminal private file to stdin. The explicit
`tty=` platform parameter is necessary: the LinuxFB screen separately probes
terminal devices when it is omitted, even if console preservation is enabled.
Together, these settings prevent Qt's screen and virtual-terminal helper from
opening terminal devices or changing stdin's keyboard mode. Harmless tty
ioctls on the private regular file return `ENOTTY`. Qt's evdev specification
parser accepts `/dev/fd/N` as an
explicit device path. The platform's default input handlers must remain
disabled to avoid automatic device discovery. Loading an evdev generic plugin
without an explicit private input path would enable discovery again.

These plugin build checks do not validate display rendering or input behavior
on the tablet. Those belong to the parent AppLoad bundle's integration tests
and the eventual manual tablet test.
