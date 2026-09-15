The native agenda helper creates the four fields of the landscape `LS Dayplanner`
template as ordinary native ink contours. It reads `paperNoteBounds` from the
active SceneController, including its nonzero origin. It loads the registered
native regular font at runtime. Neither the font nor a native template asset is
redistributed.

`prepare` observes the page and reports an immutable plan. It keeps the helper
busy until the host has durably journaled that plan and calls `commit`. The
guarded insertion rechecks the exact scene, line identities/content, selection,
unsupported objects, and native history while holding the page lock. Native
text, undo, or drawing changes invalidate the baseline. A matching existing
prefill is rechecked and acknowledged without insertion. A request cancellation
also fences delayed completion callbacks.

The day is beside the native Day label. The numeric date fits around the
template's existing slashes. The time occupies the left column and the subject
occupies the first notes row. A long subject is shortened at a grapheme boundary
to fit both width and the 128-contour native batch limit. The full title remains
in the host metadata and plan identity. The resulting lettering is outlined
ink, not a native editable text block.

Run from Linux/WSL, supplying a privately cached copy of the native resource
`:/reMarkableSans-Regular.ttf` from the reviewed firmware. The test fixture
requires it explicitly and does not obtain it over the network:

```sh
bash run-tests.sh host /root/agenda-page-host /private/reMarkableSans-Regular.ttf /root/agenda-preview-host.png
bash run-tests.sh arm /root/agenda-page-arm /private/reMarkableSans-Regular.ttf /root/agenda-preview-arm.png
python3 export-validation.py --host-build /root/agenda-page-host --arm-build /root/agenda-page-arm \
  --font /private/reMarkableSans-Regular.ttf --host-preview /root/agenda-preview-host.png \
  --arm-preview /root/agenda-preview-arm.png --output /root/agenda-page-results
```

ARM tests use Qt 6.10.3 and QEMU from the local rePaper SDK. The native allocation
and insertion code is compiled with undefined-symbol checks, but its firmware
gate rejects the foreign test executable. Stateful tests substitute the native
access object and never call fixed addresses in QEMU. They prove layout,
budgets, recovery, asynchronous lifecycle, and dispatch guards; they do not
claim physical tablet execution or a completed native filesystem save.
