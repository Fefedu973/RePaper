# Native multipart objects and wire attachment

Version 0.6.0 introduces `NativeObjectBindings`, associating newly generated editor objects with their
observed native relocation lineages. A generated resistor may contain several
native lines; touching any registered member expands the extension selection
to every verified current member. Wires snap to verified stencil ports, wire
endpoints, and the interior of existing wire segments.

The association is recorded only after `NativeObjectAccess::insert` returns
the actual active IDs in input order from the insertion's exact worker
callback. Registration requires fresh distinct native roots and the expected
ordered sampled geometry. It never assigns ownership to nearby or matching
pre-existing ink. Unbound multipart objects created in 0.5.0 or earlier remain
ordinary native ink; enclose every part or insert new objects in 0.6.0 to obtain
the multipart behavior.

## Persisted identity

On the exact Paper Pro / Xochitl 3.28.0.169 profile, RM field 7 is the
relocation origin at `SceneItem+0x20`. A line's root is that field when its low
48 bits are nonzero, otherwise its current own ID at `+0x10`. The native
selection-record construction preserves this root through affine replacement.
`NativeSelectionColor` follows that verified rule for recoloring. Field 9 at
`SceneLineItem+0xa8` is a separate rendering value and is not object ownership.

Each binding contains the explicit ordered root IDs, the observed native
reference points, semantic port IDs and locations, and a wire's continuous
logical path. The filename is a SHA-256 of document ID, page ID, and native
layer ID. JSON schema version 3 stores IDs as 16-digit hexadecimal strings and
reference XY points as raw little-endian float32 bytes in base64. The decoded
size is checked before point parsing; loading does not decompress untrusted
data. Writes use `QLockFile` and `QSaveFile`, require
the previously observed file digest, and disable direct-write fallback. A
corrupt or concurrently modified existing file is preserved.

The store is bounded to 32 MiB, 3,000 objects, 128 lines and 200,000 reference
points per object, 2,000,000 total reference points, and 128 ports per object.
These are local editor associations, not native scene groups. They do not
modify the notebook's RM files directly or synchronize through the native
notebook service.

Version 0.7 stores source geometry, width/style, arrow direction and head size,
box parameters, symbol ID, and the wire routing axis/bend. Only a complete
revalidated object receives semantic handles. Uniform native tool-19 widths
and ordered path equivalence are checked before exposing those controls.
Native resampling differences are compared by ordered arc-length geometry;
the explicit root membership is still established independently.

The schema-2 reader remains supported. For an explicitly associated 0.6
object that is selected, a model may be recovered only if its full rendering
matches all of those existing members. Endpoint/box port labels and original
reference geometry constrain the candidates. This never discovers ownership
for unbound ink. A new parametric replacement uses fresh roots and a new
binding record; its predecessor stays dormant for native Undo/Redo. The
3,000-record and point/file budgets also cover retained edit revisions.

Endpoint snapping controls creation and endpoint-edit coordinates. Moving a
component does not reroute an already drawn wire. Persistent connection
relationships and coordinated component-plus-wire updates remain unfinished.
Older 0.6 modules do not read schema 3; their refusal preserves that sidecar.
The notebook's native strokes remain independent of the sidecar reader.

## Reconciliation and interaction

Each fresh actual native snapshot resolves every recorded root to exactly one
active descendant. The descendants must have the original point count/order
and fit one common affine transform within 0.2 scene units. Ports and wire
paths receive that same transform. A missing part, duplicate root, or
independently changed member disables that object's expansion and snapping.
Unrelated objects remain usable. Entirely absent objects stay dormant so
native Undo can restore their original association.

The extension's Duplicate command records new independent bindings only after
checking that the originals remain in place, the native selected copies have
fresh roots, and the complete source selection matches the copies at the
requested 24-unit offset. Native Undo/Redo of that duplicate preserves the
independence of the original and copied objects.

Snapping reads the last verified in-memory snapshot. It performs no native
memory traversal or file access per pointer move. The caller converts a
14-pixel radius into scene units. A nearby terminal has priority over segment
projection so a nearly finished wire cannot leave a small gap beside an
endpoint. Dashed wires retain continuous logical paths for attachment even
between rendered dashes.

## Verification

The `editor-native-object-bindings` QtTest target covers exact resistor
membership, unrelated nearby ink, affine replacement and reopen, Undo/Redo,
partial erasure, ambiguous lineage, independent-part changes, independent
native copies, stencil ports, endpoints and segment snapping, dashed logical
paths, page/layer scope, insertion attribution, corrupted/concurrent storage,
unmanaged lineage resolution, and independent extension duplicates.

`editor-native-selection-color` covers exact clone-to-source lineage matching,
including reordered clones, coincident ink, invalid roots, and changed source
bytes, together with the existing native edit/history receipt tests. Host
tests and ARM SDK compilation are offline verification; physical tablet
acceptance remains necessary for the final interaction and Undo/Redo behavior.
