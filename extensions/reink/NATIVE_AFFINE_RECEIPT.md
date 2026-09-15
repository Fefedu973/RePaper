# Geometric confirmation of a native selection

Version 0.7.0 routes one recognized object’s size and endpoint edits through
constant-width parametric regeneration (see `NATIVE_PARAMETRIC_EDITING.md`).
The affine path below remains in use for native move/rotate, duplication,
removal, and ordinary or multiple-object native selections.

`NativeAffineReceipt` prepares expected sampled positions before one native
move, independent X/Y scaling, 90-degree rotation, duplicate (+24,+24), or
removal. Version 0.6.0 compares this expectation with actual active lines from
a serialized `NativeObjectAccess` callback on the same native worker, while
`pendingEdit` is identity and the same page/layer remains active. Native
selection copies can contain stale IDs or a pending display transform and are
insufficient evidence. A dispatch return, `working == false`, or an empty job
queue is not completion evidence. A mismatch must keep the operation pending.

The check requires the actual selection count as well as all selected stroke
paths. It is independent of path order, preserves duplicate-path multiplicity,
and retains point order within each path. Its absolute tolerance is 0.125 scene
pixels per coordinate, with finite coordinates within +/-1000000 and budgets of
128 paths/200000 points. Bounds reject impossible pairs before point comparisons;
a bounded bipartite matching prevents greedy ambiguity errors.

Callers must skip dispatch when `isNoop()` is true: an operation whose expected
geometry cannot be distinguished from the baseline within the tolerance cannot
be confirmed by this geometry check. Reversed samples, changed sample counts,
or a native resampling operation will be rejected rather than guessed equivalent.

Brush widths, color, object IDs, and durable attribution are deliberately not
compared. `expectedBounds()` bounds sampled positions only; it is not a brush
extent or hit-test bound. `NativeScene` separately captures the original actual
native relocation lineages and resolves their active descendants after a
transform. Removal requires those lineages to be absent from the active scene;
an empty selected list alone does not prove deletion. Duplicate additionally
requires fresh selected IDs, retained original ink, and the expected offset.

After geometry verification, the exact-ID selection operation rebuilds both
the native node selected lists and the layer's selection records. Native affine
replacement otherwise leaves stale IDs in those records. Handles and the
verified selection overlay use the repaired result. The geometry helper itself
does not write metadata or establish parametric persistence; the local explicit
object binding contract is documented in `NATIVE_OBJECT_BINDINGS.md`.
