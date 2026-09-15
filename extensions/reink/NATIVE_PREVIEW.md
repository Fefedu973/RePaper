# Native preview presentation

Target: Paper Pro (`ferrari`), Xochitl `3.28.0.169`, Qt `6.10.3`, executable
SHA-256 `43a9d5d0acc5b998264c16586e11b848f3b83d2d63b5fd322b09c0977d94d3d4`.
The following contracts were checked against the user's local executable and
resource tree. Those proprietary inputs and disassembly remain private.

## Native display mode

The native `SnapHandler` uses a `ShapesOverlay`, whose image update reaches
`QQuickPaintedItem::update` at `0x8be178`. Its draw-and-hold gesture also sets
`ScreenDriver.snapMode` in `DeviceSceneView.qml`. These are two distinct parts
of the native preview path.

The exact `ScreenDriver` Qt metadata identifies `snapMode` as its eighth
property, stored at object offset `0x2f`. Its native update function reads that
field at `0x561468` and selects global mode `2` at `0x561400`. The exact
`EPScreenModeItem` enum metadata identifies mode `2` as `Animation`.
`DeviceSceneView.globalScreenMode` feeds the document's full-screen
`Epaper.ScreenModeItem`; the ordinary native Content mode is visible only while
there is no global mode. The native driver retains its own pen proximity,
pending-tile, timer and ghost-removal transitions.

The QMD patch extends the existing native `ScreenDriver.snapMode` binding with
the host's `nativePreviewActive` flag. It preserves the native draw-and-hold
condition. The flag spans active gestures, pending selection, and creation
through preview handover. It becomes false on cancellation or loss of the
custom input surface. A local `ScreenModeItem` alone does not exercise this
native global driver transition.

`NativePreviewItem` still paints temporary view-space geometry through Qt Quick.
It submits the first frame immediately after idle, then coalesces later samples
with a 16 ms cooldown started after parsing. Only the latest pending geometry is
parsed, and idle timer expiry does not repaint. Clearing the preview cancels any
pending frame immediately. This scheduling reduces the Qt-side preview wait;
the existing native Snap display-mode coordination remains in place. The preview
does not insert a custom raw framebuffer writer or claim the native freehand
pen's direct ink renderer. Static inspection and desktop tests cannot establish
the physical display latency.

## Forwarded pen input and native touch arbitration

The exact digitizer fallback sends each blocked pen sample through Qt's mouse
event path (`0xa4b78c` for moves). The 0.5.0 user log recorded 815 moves for one
gesture. Throttling only the painted preview still let each of those moves
cross into QML and rebuild editor geometry and state.

`NativeInputScheduler` now filters moves at the input item's C++ event boundary.
It keeps every sample in bounded batches of at most 256 points, flushes on a
16 ms timer or when that limit is reached, and rebuilds once per batch. Release
flushes the remaining points before the exact endpoint reaches `pointerEnd`.
Cancellation, target/window loss and tool changes discard pending work. This
reduces per-sample work before rendering; device validation must still establish
whether it resolves the stylus-specific presentation delay.

The host enables the custom MouseArea while the stylus is nearby or its press
is still active. `NativeInputScheduler` rejects Qt/system finger-to-mouse
synthesis before that surface can grab it. Original touch events continue to
the existing native `SceneViewGestures` recognizer; the current host preserves
its `TouchArea.target` throughout tool changes and uses no relay or proxy window.
The native recognizer retains its own gesture thresholds, pen-proximity and
palm policies. `NativeTouchGuard` cancels custom ink on multi-touch or window
loss while leaving those events available to the native handlers.

For verified custom selection ink overlays, the host temporarily binds
`SceneTileManager.selectedIncluded` to false. Exact metadata identifies property
9 at object offset `0xb1`; its setter at `0x879670` emits the native notification,
resets tiles and schedules a reload. Render-job assembly copies that flag into
its render context at `0x8a8b94`. `Binding.RestoreBindingOrValue` restores the
prior native value or binding on deselection, hide or native tool activation;
the implementation assumes no constructor default.

## Pen-up handover

The preview's stroke list is retained before the insertion command is dispatched.
After the insertion has been observed, `presentCommittedPreview(token)` waits
for the native tile manager to stop reporting pending tiles and for the viewport
to stop blocking updates. It then requests a native dirty repaint. The next
window `frameSwapped` callback queues an acknowledgement back to `NativeScene`.
This marks a submitted Qt frame, not a physical e-paper refresh receipt.

The exact `SceneTileManager` metadata at `0x1208210` exposes `pendingTiles`
and `pendingTilesChanged`. The exact `DeviceSceneViewport` metadata at
`0x11edde0` exposes `blockingUpdates`, `blockingUpdatesChanged`, and
`requestRepaintDirty()`. It exposes no frame completion signal. Native
`DocumentView.qml` also uses the window's `frameSwapped` signal when deferring
screen-share and rotation state until after a frame.

Host generation checks reject stale queued callbacks. A renewed tile or
viewport block invalidates the outstanding frame request. A page attachment
drops the previous request; visibility, service or window loss releases its
current token. NativeScene separately validates its current token and page and
caps this post-observation wait at 1500 ms so the input lock cannot remain held
indefinitely. The timeout is a fallback limit, not a normal presentation delay.

## Validation

The exact resource patch was applied offline and its generated Snap binding
checked for a single preserved native condition and a single custom extension.
The real shipped page host runs in the desktop test with service doubles that
exercise pending tiles, blocked updates, submitted frame acknowledgement,
stale-token cancellation, page changes, hide, and missing or replaced services.
These checks establish the state transitions. Physical stylus latency, e-paper
refresh quality and native tile appearance still require the device trial.
