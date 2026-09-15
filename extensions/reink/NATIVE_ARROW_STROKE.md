# Complete solid-arrow selection

The shared drawing model emits an arrow as a shaft plus one or two separate
three-point head strokes. The native factory turns each stroke into a distinct
`SceneLineItem`. Native selection hits and transforms individual item IDs, so
selecting the shaft alone left the head behind.

`wholeNativeArrowStrokes` now prepares newly drawn opaque solid arrows as one
native stroke. `NativeGesture::rebuild` applies the helper after assigning color,
so the preview, insertion observation, and committed geometry use the same path.
All three directions (end, start, both) retain every original edge. The path
retraces an existing head edge to reach the remaining branch; it introduces no
connector segment, and preserves stroke width and color. Existing native point
sampling then makes the shaft and every head branch hittable as that one item.

The helper requires the exact generated topology, shared appearance, and opaque
color. Dashed/dotted arrows, unrelated object kinds, translucent strokes, and
malformed or mixed-appearance input pass through unchanged. It does not infer
ownership from proximity or bounding rectangles. Previously saved unbound
multipart arrows are not migrated. Version 0.6.0 additionally records explicit
birth bindings for new disconnected symbols and dash fragments; extension
selection can expand a child hit to their verified active native members.
See `NATIVE_OBJECT_BINDINGS.md`. Version 0.7.0 uses `rebuildNativeObject` for
semantic endpoint/style edits too, retaining this same single-line topology
for opaque solid arrows after editing and model revalidation.

`editor-native-arrow-stroke-tests` checks every direction in four orientations
and two colors, exact bidirectional edge coverage, bounds, sampled vertices,
rotation/nonuniform-scale coverage, pass-through cases, and preview/commit
integration. Qt raster comparisons allow one boundary pixel: a 180-degree
retrace join can round differently from a separate cap (the diagonal fixture
differs by one pixel at a retraced tip). Geometry equality is exact. The tablet's
native solid brush and physical selection/Undo/Redo still require device checks;
these host tests do not certify its raster or display timing.
