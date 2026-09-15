# Device runtime contract

This launcher runs one standalone Qt application inside AppLoad. For reMoodle and reAgenda it starts the bundled Paper Bridge through a bounded helper before opening the app. Builds and tests make no tablet connection. See `bridge-RUNTIME.md` for the service protocol and lifetime.

The installed shared directory is:

```text
runtime/
  repaper-appload-launch
  qt-linuxfb-refresh
  qtfb-shim.so
  paper-bridge
  bridge-start
  bridge-run
  lib/
  plugins/platforms/libqlinuxfb.so
  plugins/generic/                  # evdev touch, keyboard and tablet plugins
  plugins/                         # other required Qt plugins, including SQL/TLS
  qml/
  fonts/fonts.conf
  fonts/                           # bundled font files
  certs/ca-certificates.crt
```

An app manifest can use `application: "../runtime/repaper-appload-launch"`, `args: ["./remoodle"]`, and `qtfb: true`, with the app directory as its working directory. Set `disablesWindowedMode: false` for reCalc and `true` for reMoodle/reAgenda. Keep `supportsVirtualKeyboard: false` to disable AppLoad's community keyboard. The native keyboard host in Xochitl instead passes `REPAPER_NATIVE_KEYBOARD_SOCKET` to the application; this environment variable is preserved. reAgenda and reCalc use their corresponding binary names. The supervisor resolves the shared directory from `/proc/self/exe`.

In AppLoad, a long press on reCalc opens a movable, resizable window; a tap opens fullscreen. The title bar toggles fullscreen and minimization. AppLoad scales the fixed framebuffer with its aspect ratio preserved and maps input coordinates back to it, so window resizing does not require a different LinuxFB resolution.

The shim and refresh helper both use **RMPP RGB565, 1620 × 2160, 2 bytes per pixel**. `QT_SCALE_FACTOR=2` gives an 810 × 1080 logical surface. AppLoad's device socket and shared-memory names remain `/tmp/qtfb.sock` and `/qtfb_%d`. The helper consumes the upstream protocol definitions from the reviewed source and sends changed row bounds at up to ten updates per second. It verifies the shared-memory size and fails on a disconnected AppLoad server. It does not log input payloads.

Each launch creates a mode-0700 temporary directory with mode-0600 regular input files. Qt receives inherited `/dev/fd/N` aliases; the shim matches their private file identities. Qt's automatic input discovery is disabled and only explicit generic evdev handlers are loaded. Both Qt console paths are isolated: the platform receives a private `tty=` file and `nographicsmodeswitch`, and the VT handler receives preserved console state, no signal hooks, and a non-TTY stdin. No physical framebuffer or input device is selected by the launcher.

The supervisor owns separate process groups for its app and helper. It waits for the helper's successful framebuffer handshake before launching the app. App completion, helper failure, SIGINT, SIGTERM or SIGHUP closes both groups, waits up to two seconds for graceful shutdown, then kills remaining members and reaps the direct children. Linux parent-death signals stop the two direct children if the supervisor is killed abruptly. A SIGKILL of the supervisor can leave its temporary files; regular shutdown removes only the six files it created and their directory.

The supervisor explicitly configures the bundled Qt library, plugin, QML, fontconfig, font and CA paths. Both native executables contain an `$ORIGIN/lib` runtime search path, including when copied straight from their build directory. `REPAPER_PC_*`, PC scale configuration, inherited bridge-socket overrides and framebuffer DRM selection are removed for the app. AppLoad's inherited `LD_PRELOAD` is replaced with this runtime's shim; the refresh helper receives no preload.

Apply the shim hunk of `stylus-input.patch`, then `shim-private-input.patch`, **only to a shipping copy** of the reviewed AppLoad source. They provide pressure metadata, enable `QTFB_SHIM_FB_PATH` for the private framebuffer file and remove input-payload logging. The module checkout and PC emulator sources remain separate. CRLF checkouts require `git apply --ignore-space-change`.

The runtime bundles the separately patched evdevtablet plugin under `plugins/generic/libqevdevtabletplugin.so`. It converts native screen coordinates to logical coordinates once. `REPAPER_APPLOAD_TABLET_INPUT=1` enables the three applications' `TabletMouseAdapter`, which delivers tablet gestures to their QtQuick controls; it is not installed into the native Xochitl process.

Build locally with the installed ARM SDK:

```sh
bash packaging/appload/device/build-runtime.sh /path/to/reviewed/rm-appload /root/repaper-appload-device-runtime/release
```

The script exports the source commit to a new directory, applies the two shim patches, and builds the two native helpers and ARM64 shim. It refuses to reuse an existing shipping-source directory. A candidate's uncommitted QML hook changes are irrelevant to this shim export; the separately built AppLoad module supplies those hooks.

Host integration validation, with no live AppLoad or physical device:

```sh
cmake -S packaging/appload/device -B /root/repaper-appload-device-runtime/host \
  -DAPPLOAD_SOURCE:PATH=/path/to/reviewed/rm-appload -DCMAKE_BUILD_TYPE=Release
cmake --build /root/repaper-appload-device-runtime/host --parallel 2
python3 packaging/appload/device/test_runtime.py \
  --build /root/repaper-appload-device-runtime/host --appload /path/to/reviewed/rm-appload -v
```

The thirteen host checks cover input isolation and environment, rejected launch keys, refresh startup and runtime failures, bounded termination, independent concurrent launches, changed-row refresh bounds, incompatible framebuffer rejection, and Bridge startup/failure/timeout/service independence. Supervisor tests use explicit fake child programs and a no-op preload; refresh tests compile the real helper against a private host socket and use exclusively created test shared memory. They do not establish tablet visual or pen/touch behavior.
