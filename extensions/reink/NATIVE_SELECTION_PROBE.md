# Original-selection probe

`RePaperNative::observeOriginalSelection(QObject *controller, int layer)` is a
read-only utility in `../editor-common/NativeSelectionProbe.{h,cpp}`. Since 0.3.0,
NativeScene uses it while a custom tool or an insertion is active. No user
activation is required. It is diagnostic, not a prerequisite to further creation.
The utility itself does not authorize or perform native edits.

Call it synchronously on the controller's owning thread, within the current page
context. Obtain `layer` from the current controller property. Do not call
it while holding a worker/Page mutex or from a native edit callback. The utility
uses try-locks and returns a busy status immediately instead of waiting.

It requires the exact ferrari/3.28.0.169/Qt6.10.3 executable profile and the known
24-byte item-list metatype. Host builds refuse before reading native getters.
The worker and page getters were independently traced through Qt metacall. All
subsequent private structures are read locally using checked mapped ranges and
fixed budgets, not through private function calls. The only native lifetime
operation is retaining an already existing `std::shared_ptr<Page>` while the
worker mutex is held. No page is created, no trait is cloned, no geometry is
written, and the Page owner-thread marker is not changed.

On success the returned QVariantMap has:

- `status: "observed"`, `complete: true`, `count`, `lineCount`, `layer`;
- `pageIdSha256`: SHA-256 of the UTF-8 page ID captured for this observation;
- `items`: `{idHex, parentIdHex, type: "SceneLineItem"}` records. Hex strings
  are exactly 16 characters and preserve the full 64-bit native values;
- `nativeIdentityValidated: false`, `mutationsEnabled: false`.

Every refusal has `complete: false`, a diagnostic status and no item list. The
caller must combine page scope with its document/page fingerprint and discard
old observations after page changes. The utility rechecks the page, worker and
selection count after unlocking. It logs and saves nothing itself.

Limits: 128 selected items, 4096 tree nodes (including root), 32768 buckets per
hash table, 65536 pages. Mixed selections containing unsupported native item
types are rejected as a whole. Duplicate/zero identities are rejected. This is
an observation of the source lists used by Copy; canonical identity across
partial selection, Undo/Redo, reopen and duplication still needs device tests.
The file author index can be remapped at load time, so correlate native IDs with
the .rm author UUID table instead of comparing counters alone.

Static ELF evidence is described in `PROVENANCE.md`; the local disassembly is in
`.local/qa/native-abi-3.28/`. Exact PageData inheritance/vtable and Page ownership
were checked independently. No proprietary binary is distributed.

The extension CMake now includes `NativeSelectionProbe.cpp` in its common sources;
`REPAPER_WITH_NATIVE_ABI` selects the AArch64 implementation. Its separate
QtCore/QtTest target links `TargetProfile.cpp`. The test intentionally includes
the implementation to exercise otherwise private bounded readers on fictitious
memory; it never runs the native path.

Validation completed locally: host Qt6.2 build and 8 QtTest cases including setup
and cleanup; AArch64 SDK build with the native ABI branch enabled. The standalone
validation project lives in `.local/qa/native-abi-3.28/probe-src`, builds in
`/root/repaper-original-selection-probe` and
`/root/repaper-original-selection-probe-arm`. No tablet installation was made.

## Missing contract before persistent replacement

This observation contains identity and parent identity only. It does not provide
a content revision. In particular, it cannot detect an in-place geometry or
style change that preserves the same selected-item IDs, so it is insufficient
to authorize a parametric replacement through NativeObjectRegistry.

A future content revision must hash canonical, bounded values: Line tool/color,
RGBA, mask scale, thickness, the point count and every point's coordinates,
speed, width, direction and pressure. It must never hash C++ padding, pointers,
QList allocation headers or an entire raw object. Floating-point representation
and invalid-value rejection need a documented deterministic rule. The Line
bounds cache is lazily recalculated and invalidated by native translation; a
cache-only change must not masquerade as a geometry revision.

Ancestor group transforms, pending selection transforms, and any native state
outside Line that changes the rendered result still require characterization.
The observation must be made under the same bounded page lock and paired with
its document/page/epoch; then deletion, partial selection, Undo/Redo, copy/paste
and reopen must be compared on the device. Until these contracts are attested,
the persistent registry remains disconnected. No pointer or geometry hash is
substituted for the native identity.
