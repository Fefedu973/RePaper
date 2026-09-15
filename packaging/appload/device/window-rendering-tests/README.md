# Window composition regression

`window-rendering.patch` fixes AppLoad framebuffer content staying at a fixed
screen position while its window moves. Apply it after `windowed-input.patch`.
It only changes `FBController::paint`: save the incoming painter state, compose
the framebuffer rotation with the existing scene transform, then restore state.

The e-paper renderer differs from Qt's stock software texture renderer. The
SDK's actual `libqsgepaper.so` painter node calls the item's `paint()` directly
with the scene painter. AppLoad's old `resetTransform()` therefore discards the
window position, parent rotation and scale, while the window clip still moves.

Run the offline AArch64 validation from WSL, with an existing packaged runtime:

```sh
python3 packaging/appload/device/window-rendering-tests/run-validation.py \
  --appload-source .tools/rm-appload-328-candidate \
  --stage /root/repaper-appload-3281-stage-v1 \
  --output /root/repaper-window-rendering-qa-new
```

The output directory must be new. The driver builds private source copies with
the ARM SDK and runs them under QEMU in a network namespace with fresh profiles.
It does not connect to the tablet, start Xochitl, initialize DRM, or modify the
source checkout or packaged stage.

The suite first proves that the SDK's real e-paper node passes its incoming
transformation unchanged into a spy item's paint callback. It then checks real
FBController drawing through both direct calls and that native node across four
framebuffer rotations, four scene rotations and 1x/2x scale. It also checks three
window positions, clipping a damaged region, and restoration of painter state.
Each image comparison samples over 20,000 flat scene pixels, excluding two
pixels around color edges where direct and intermediate image resampling can
differ. Existing input mapping and identity-transform rendering checks also run.

The native-node diagnostic resolves exported factories and the verified draw
vtable slot of SDK `libqsgepaper.so`, pinned to SHA256
`3f76b7db328f7e16cde2d360ebe4a1db043a113cd2386d4c995cc4d25a0eeaec`.
The test refuses another library hash until that diagnostic ABI is reviewed.
This is a test adapter only; the production patch has no private Qt ABI calls.

The report requires the old implementation to fail all 64 scene compositions,
window movement and state restoration, and the patched implementation to pass.
The 85 existing input/rendering checks must pass with an unchanged hash of the
64 original identity-transform framebuffer renders. This does not exercise
the full native renderer, physical e-paper updates or a live Xochitl process.
