# Native editor provenance

Target: Paper Pro (`ferrari`), firmware `3.28.0.169`, Qt `6.10.3`, executable SHA-256
`43a9d5d0acc5b998264c16586e11b848f3b83d2d63b5fd322b09c0977d94d3d4`.
Source inspection and offline QMD validation are not a device editing attestation.

| Component | Pinned primary source | Reuse |
| --- | --- | --- |
| XOVI | https://github.com/asivery/xovi/tree/2b99649f5e4fd6288be7792a8570bd16418adb70 | Generator and generated import/resource table, LGPL-3.0 |
| Scene Assistant | https://github.com/ingatellent/xovi-scene-assistant/tree/8afbac01ca7816f9aa22a5897a3f2133b0b0d9d7 | `rm_Line` geometry and opaque `rm_SceneItem`, GPL v2; `rm_SceneLineItem` factory removed in 0.2.2; original attribution to HookedBehemoth/xovi-sudoku retained in upstream README |
| Native QMD references | https://github.com/ingatellent/xovi-qmd-extensions/tree/67025316fa58f0b9e34b33e374521d29517d7f51 | Toolbar and paste research; exact target selectors independently checked against local resources |
| Resource rebuilder | https://github.com/asivery/rm-xovi-extensions/tree/7874154dba6793cc68a15fae0fb9dd272c4ed20a | `qmldiff_add_external_diff(const char*,const char*)` exported C signature and QMD loader behaviour |

The actual checkout origin URLs are recorded by the local source bundles. No
proprietary Xochitl executable, extracted QML or device hashtable is redistributed.
NativePageHost.qml and editor.qmd contain original integration code. The exact
resource tree supplies the verified object relationships, method names and selectors.

`SceneAssistant.cpp` and its mutating `ensureVtable()` bootstrap are neither built
nor loaded. Version 0.2.0 confirmed the container, RTTI, module/page attachment and
two toolbar buttons on the exact device. Its presumed identity fields were not
probative. Version 0.2.1 no longer reads them or initializes the write factory.
It reports the compiled layout separately from established native field semantics.

Read-only ELF inspection of the pinned binary establishes a SceneLineItem object
size of 0xb0 (deleting destructor 0xed7640), its Line member at +0x48, and virtual
clone at 0xed7220 copying 8-byte words at +0x10 and +0x18. The upstream `pageIndex`
at +0x0c is padding not copied or initialized by this clone, so reading it cannot
identify a stroke. The other upstream candidate names subdivide the wrong words.

Crucially, the Qt method `cloneSelectedItems(int,double)` resolves through
SceneController static metacall 0x873480 to 0x899250. After cloning each item, it
explicitly executes `stp xzr,xzr,[x0,#0x10]` at 0x8994e8: both words are erased.
`cloneAddAndSelectItems` at 0x897190 performs the same reset at 0x89729c. These
copy/paste APIs therefore cannot expose original identities through cloned items.
The older adapter's guessed fields cannot establish live item identity. Version
0.6.0 instead observes active original entries in the verified native CRDT
lists, including their actual ID, parent and relocation origin. No alternative
pointer or geometry-based identity is substituted. Offline disassembly and the
Qt dispatch chain are retained locally in `.local/qa/native-abi-3.28/`; the binary
itself is not redistributed.

The native JSON reader independently establishes the meaning on original
SceneItems: table 0x1681e78 names `id` and `parentId`; integration at 0xf3168c
stores those values at +0x10 and +0x18 respectively. The internal CRDT format
is high-16-bit author index plus low-48-bit counter. File author indexes may
be remapped during loading, so comparisons must retain the author UUID map.
These static facts establish field meaning. Version 0.6.0 adds runtime receipts
and conservative local binding reconciliation, with physical Undo/Redo/reopen
and copy/synchronization acceptance still separate.

Unverified upstream candidate words are not used as IDs, authors or layers.
A SHA-256 of UTF-8 compact JSON `[documentId,pageId]` permits diagnostic
correlation without logging these raw IDs.
Versions 0.2.0/0.2.1 lock all writes in C++, regardless of local configuration.
Version 0.2.2 used explicit page/layer activation and clones of selected strokes.
Device feedback exposed the resulting dependence on a nonempty selection.
Version 0.3.0 removes both requirements. The exact-target native deserialization
factory at 0xe85a50 constructs tag-5 SceneLineItems with native shared ownership;
the module verifies the resulting vtable and replaces the attested Line subobject.
The initial timestamp is set to 0:1, matching the native new-stroke path. Item IDs
remain zero until assigned by insertion. See NATIVE_LINE_FACTORY.md.
Versions 0.3.0–0.5.0 submit one QList to the native paste API. Version 0.6.0
submits the verified private batch directly through one serialized native
insertion callback, confirms its single history command, and observes actual
active IDs on the retained input objects in their original order. Existing
active ink must remain unchanged. A dispatched command or settled native
selection signal is not reported as durable storage. Local birth membership is
implemented. Version 0.7.0 adds verified parametric replacement and ends creation
with an exact empty selection in the same worker callback, preventing selected
new ink from being omitted by the native base tiles after preview retirement.

The 0.3.0 QMD buttons are PenTool instances selected through the native
requestPenSelect protocol. They use the valid primary pen type beneath the custom
input surface. Exact extracted Toolbar.qml behavior was tested locally without
redistributing that QML. The page host follows the firmware's TextDragHotspot
MouseArea/PenInputBlocker contract, including move events independent of pressed.
Read-only ELF inspection confirms Qt press/move/release forwarding, a 20-pixel
initial move threshold, and current release coordinates. Tests use the actual
original NativePageHost.qml with doubles for proprietary services. These tests
do not replace physical stylus, rendering, persistence and Undo/Redo checks.

Version 0.4.0 adds original custom selection interaction code: tap/rectangle
selection, eight independent resize handles, a free rotation handle and a
properties inspector. Native affine operations are confirmed using a retained
Scene QObject's queued changed signal followed by bounded geometry observation.
The working property is a delayed spinner and is never treated as an affine
completion receipt. See NATIVE_SCENE_OBSERVER.md. The exact-resource patch now
also protects document history actions while a custom command is outstanding.

The Qt Quick painted preview coalesces moves at a 33 ms cadence. Version 0.5.0
adds the missing native ScreenDriver.snapMode gate, the same global gate used
by the firmware's DrawAndHoldGesture/ShapesOverlay, and retains the preview
until native tiles are ready and Qt submits a frame. See NATIVE_PREVIEW.md for
the attested native display path and the physical-latency limitation. Native Line scaling multiplies
point widths by sqrt(abs(det(transform))) and rounds to quarter units; the
preview follows that encoding and guards the uint16 limit. SceneLineItem's native
transform at 0xed7bc0 reaches Line transform 0xf79780, which adjusts maskScale and
the encoded point widths. Palette color decoding follows the documented native
codes from the pinned Scene Assistant source; ARGB code 9 carries explicit RGB.

Selection color uses genuine native Copy clones, retaining pressure/width and
geometry, followed by one serialized native worker callback. It validates the
original selection, native history preconditions, group offsets and each Line
before invoking the native deletion/insertion paths and grouping their exact
recorded history delta. The helper retains its own job ID and requires both its
callback result and a fresh expected selection. It makes no synthetic native
command, no vtable patch and no direct live-file replacement. See
NATIVE_SELECTION_COLOR.md for compensation limits and the distinction between
one recorded Undo and unverified physical Undo/Redo or disk durability.

Version 0.6.0 adds `NativeInputScheduler` at the input item's C++ event boundary.
It batches every forwarded stylus point, flushing after 16 ms or 256 points,
and preserves the exact release endpoint. Cancellation and context changes
discard pending input. Version 0.7.0 removes `NativeTouchRelay` from production,
including its QML registration and native gesture target alias. The input item
is visible only for pen proximity or an ongoing pen contact. Qt finger-derived
mouse presses are rejected at that item boundary; original touch events and
the native recognizer target are not rerouted. `NativeTouchGuard` observes
cancellation without accepting or grabbing events. Idle native navigation does
not rebuild editor state. Physical pen latency, pinch/pan, proximity and palm
behavior still require tablet acceptance.

`NativeObjectAccess` traverses actual undeleted native entries within the
current layer and its verified child groups. It omits stale selection pointers
from active membership and retains its own worker job receipt. After affine
replacement, `NativeScene` resolves the selected roots to actual descendants,
checks the expected geometry, and rebuilds both native selected lists and
native selection records before releasing the handles. Exact selection proof
gates hiding native selected ink beneath the custom overlay. These operations
do not create structural GroupItems: their native clone path still returns
null and is incompatible with generic insertion.

RM field 7 is the relocation origin at `SceneItem+0x20`, read at
`0xeda40c`–`0xeda418` and written through `0xedc6f4`/`0xedc82c`. The native
selection-record constructor restores that origin, or the source own ID when
the origin's low 48 bits are zero. The color helper now matches private native
Copy clones to exact source world-coordinate content and preserves that root.
`SceneLineItem+0xa8` is separate RM field 9 rendering data, not ownership.

`NativeObjectBindings` records newly inserted 0.6.0 objects by those explicit
native roots and verifies one common affine transform of their original point
sequences. This enables whole-member selection and cached wire snapping to
ports, endpoints and segment interiors. Color replacement retains membership;
the extension Duplicate command verifies fresh independent native roots and
records a separate object. The bounded local sidecar uses atomic writes and
preserves corrupt or concurrently changed files. Host tests cover affine/color
lineage matching, independent duplicate membership, Undo/Redo and reopen
reconciliation. Those tests do not establish physical persistence or metadata
transport through native cloud sync and page/document copying.

Unbound 0.5.0 and older multipart drawings must be enclosed fully or recreated.
Moving a component does not yet reroute a connected wire. Version 0.7.0 adds
endpoint, wire-bend and oriented box editing for one complete revalidated
object. `NativeObjectGesture` regenerates geometry at constant pen width;
`NativeObjectAccess::replace` verifies the exact baseline, erases and inserts
inside one retained worker callback, checks other ink and the new exact IDs,
then groups the two known history commands through native 0xe51880. The old
binding remains dormant for Undo and the replacement receives fresh explicit
native roots. This is an in-memory receipt, not disk durability. See
NATIVE_PARAMETRIC_EDITING.md, NATIVE_OBJECT_BINDINGS.md and
NATIVE_AFFINE_RECEIPT.md for the precise scope.

Version 0.5.0 joins each newly created opaque solid arrow into one native Line,
retracing only existing head edges. It retains both heads under all native Line
transformations without an object registry or guessed structural group. Exact
edge coverage and bounds are tested; Qt raster coverage differs by at most one
boundary pixel in the tested diagonal case. Native brush appearance still needs
device verification. That 0.5.0 single-line helper does not itself group dashed
arrows, multipart symbols or previously created arrows. Fresh 0.6.0 objects use
the explicit binding path described above. See NATIVE_ARROW_STROKE.md.

The combined experimental module includes GPL-v2 source. It is not represented as
an MIT-only binary. XOVI glue retains LGPL-v3 provenance, and Qt remains dynamically
linked under its upstream terms. This is a local test archive with complete relevant
source and licences; no public distribution or licence-compatibility attestation is
made. Resolve the GPL-v2/LGPL-v3 combination explicitly before public distribution.

## Color/history correction in 0.7.1

The 0.7.0 feedback log contains repeated native history preflight refusals, including new insertions after editing. Static inspection of the exact firmware proves that the native macro created by grouping delegates its merge check to its last child. Requiring the top command’s merge entry to be the unconditional-false leaf therefore rejected our own grouped commands. Version 0.7.1 follows only this verified, bounded macro chain and retains the existing history-count, active-identity and rollback checks. This fixes a proven preflight defect; the log does not capture the runtime command pointer and physical color/display behavior is still pending. See `NATIVE_SELECTION_COLOR.md`.

## Aspect-ratio hold in 0.8.0

The hold recognizer and ratio geometry are original project code. A monotonic
1000 ms timer and 8-view-pixel movement radius activate only at the start of an
eligible stylus creation or resize. The native QML scheduler flushes all queued
samples before the timer decides, so an out-and-back movement cannot appear
stationary through latest-point coalescing. Creation uses square/circle or the
catalogue's existing 120 x 80 nominal box; resizing preserves the current local
dimensions and opposite anchor. No native ABI or history command was added.
The user reports 0.7.1 working; 0.8.0 physical gesture validation is pending.
See `NATIVE_PARAMETRIC_EDITING.md` for geometry and lifecycle contracts.

## Direct selection and Properties cancellation in 0.8.1

The new reSelect button uses the exact native PenTool selection protocol and
the firmware's selection_tool icon. It activates the extension's select mode
without a foldout. Original firmware resources remain private; the changed
QMD is validated against all three exact resources and tested in the private
native-toolbar harness.

The user clarified that popup Annuler means abandoning all edits since opening,
then closing. Native SceneController Undo first cancels an active selection and
can return without queuing a history command. The popup now has its own session
contract. A bounded, retained history checkpoint attributes each committed edit;
cancellation undoes only that exact suffix under the verified native Page lock,
then verifies full restored content by canonical lineage before closing.
Native Undo reissues own IDs, so those IDs cannot be a restoration receipt.
No firmware executable, proprietary QML or user ink is included in the package.
See `NATIVE_PROPERTIES_SESSION.md` for the source and ABI contracts.
