# Native parametric selection, version 0.8.0

`NativeObjectGesture` supplies endpoint controls for lines/arrows/wires, a wire
bend control, oriented box controls for rectangles/ellipses/catalogue symbols,
and free rotation. Gesture updates only change an in-memory preview. Endpoints
retain their opposite endpoint, line width, arrow head size and pattern scale.
Box dimensions are measured along the object's own orthogonal axes. Wire
routing retains its local axis under rotation and distinguishes a two-segment
corner from a three-segment route even when the bend offset crosses zero.

`NativeObjectBindings` exposes a model only for one complete explicit object
whose current members reproduce its rendering. Native tool 19, uniform point
widths, opaque color and ordered geometry are checked. The identity check still
uses the actual persisted relocation roots and all original reference samples;
path equivalence does not assign ownership. Model-to-native comparison permits
different subdivision counts along the same ordered path, with both-direction
arc-length checks within 0.2 scene units. `rebuildNativeObject` applies the same
joined solid-arrow representation during creation and later regeneration.
Float32 rounding in native coordinates can make a fitted rotated box slightly
non-orthogonal. Binding reconstruction projects that box back to perpendicular
axes only if the correction stays within the same geometry tolerance; the full
rendering comparison still runs afterward. The generic model continues to
reject shear. Small rotated rectangles, ellipses and symbols are regression tested.
Observed transforms first try a similarity map and accept it only when every
stored native reference sample matches the current geometry. This prevents
nearly collinear float samples from inventing perpendicular scale and changing
the dash pattern during rotation. Genuine nonuniform transforms still use the
fully checked affine fit.

The schema-3 sidecar stores model parameters. Existing schema-2 bindings may
recover a model from known ports and birth geometry only after full rendering
equivalence. Unbound ink is not classified or grouped. Ordinary and multiple
object selections retain the native affine path.

## Hold to preserve proportions

`NativeAspectHold` waits 1000 monotonic milliseconds after an eligible pen
press. Every raw view sample must remain within an 8-pixel radius of that
press until activation; leaving the radius cancels eligibility for the entire
contact, even if a later sample returns. Before advancing the hold timer,
`NativeScene` calls the actual QML host's synchronous input-scheduler flush.
This preserves out-and-back samples that were queued within a 16 ms batch.
Ordinary movement is still batched; the flush is needed only at activation.

Rectangle and ellipse creation lock to 1:1. Symbols lock to the existing
catalogue's nominal 120 x 80 box. Resize controls preserve the initial local
width/height, including a previously stretched or rotated object. Corner
handles use the greater proportional departure from their original size;
side handles use their active axis and keep the opposite edge midpoint fixed.
Both axes use one scale through minimum-size and coordinate-bound clamping.
Parametric objects regenerate at fixed pen width. Ordinary/multiple-object
selections retain native affine width scaling with the existing width gate.

The visible indicator appears only after activation. Release commits the
constrained geometry and resets the hold; cancel, tool/page changes, capture
loss and touch-guard cancellation discard it. Endpoint, wire-bend, move,
rotation, freehand, line/arrow/wire creation and region selection never arm it.
Tests cover all handles, rotated axes, quadrants, timer boundaries, jitter,
queued movement, real-QML feedback/flush and native creation/replacement
workflow receipts. Physical pen/display behavior remains to be checked.

## Replacement and history

The scene first reads a fresh worker snapshot and rejects a changed baseline.
It prepares new private native lines before requesting replacement.
`NativeObjectAccess::replace` then owns one retained-page `DocumentWorker`
callback and rechecks scope, exact selection, IDs and versions under that lock.
The native deletion at 0xec4180 and insertion at 0xec7130 must each add exactly
one history command. The actual new entries must belong to the retained input
objects, have fresh IDs and expected full point/style versions, and leave
every other active line unchanged. Old selected IDs must be absent. Exact
selection lists and UI records are rebuilt for the new IDs before the native
history grouping at 0xe51880 reduces those two commands to one.

The operation uses the same tested recorded-edit sequencing as recoloring.
Failed verification compensates only its known history delta; it never guesses
about unrelated commands. After confirmed compensation, selection restoration
requires the original active identities and versions. Arbitrary native
allocation exceptions or unconfirmed history changes are not a universal
atomic recovery contract. See `NATIVE_SELECTION_COLOR.md`.

Tool changes keep the operation locked through completion and exact cleanup.
Page changes cancel the obsolete receipt. A successful replacement records a
new explicit binding; the previous binding remains dormant so native Undo can
recover the prior model and Redo the new one. All retained revisions count
toward storage limits. A sidecar write failure is reported separately from the
already observed native edit and does not erase the document's strokes.

## Creation visibility and touch input

Creation differs from selection editing: after observing its new IDs, the same
worker callback rebuilds an exact empty selection. This prevents native tiles
from omitting still-selected new ink after the creation overlay retires.
`NativePageHost` handles selection notifications throughout a custom tool's
capture period, so Xochitl does not reopen or clear a competing selection UI.
The final preview still waits for tiles and a submitted Qt frame, with the
existing bounded retirement fallback. These are not e-paper display receipts.

The 0.6 touch relay, native-target alias and production registration were
removed. The pen MouseArea is visible only for proximity or an ongoing pen
contact; Qt finger-derived presses are rejected before that item can grab.
The underlying touch stream and native target remain unchanged. Proximity
exit before pen release preserves the final endpoint. The observer-only touch
guard can cancel a custom gesture but never consumes touch events. Idle native
navigation avoids redundant editor state rebuilds.

Tests cover rotated geometry, fixed thickness, endpoint offsets and snapping,
wire bend continuity, exact replacement and tool/page races, old-model
recovery, style/topology changes, Undo/Redo and reopen, finger transparency,
button taps and immediate pen use after touch. Host tests and ARM compilation
do not establish physical latency, final e-paper appearance or disk durability.
Dynamic attached-wire following and metadata transport through native page
copying/cloud synchronization remain outside this implementation.
