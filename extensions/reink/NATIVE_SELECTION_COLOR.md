# Native selected-stroke color

Static verification on 2026-09-05 covers Paper Pro (`ferrari`), Xochitl
`3.28.0.169`, Qt `6.10.3`, executable SHA256
`43a9d5d0acc5b998264c16586e11b848f3b83d2d63b5fd322b09c0977d94d3d4`.
Addresses below are ELF virtual addresses in that exact executable.

`NativeSelectionColor` implements selected-stroke recoloring through one native
worker callback. It performs native deletion and insertion, checks the exact
history-size changes, and groups just those two commands into one native macro.
It preserves the insertion's native selection. The PC sequencing tests and ARM
build are offline checks; no tablet content was changed during this work.

## Adapter contract

`dispatch(controller, layer, QColor)` first validates the exact executable,
native metatypes, controller thread and page, opaque color and idle pending
transform. It takes a bounded original-selection snapshot under the existing
worker and Page locks. The snapshot includes native IDs, item and node
addresses, line geometry and style fingerprints, selected node membership, and
the group translation fields consumed by native Copy. It also captures the
persisted relocation origin at `SceneItem+0x20`. These are local command
preconditions; the object-binding store consumes the origin independently.

Native `cloneSelectedItems(layer, 1.0)` creates genuine private clones and
applies the native group translation. A second original snapshot must still
match. The adapter validates at most 128 lines and 200,000 total points, rejects
invalid geometry and style, and rejects tools 8–11 and 22, which native append
silently refuses. Exact world-coordinate line fingerprints match each clone
back to one observed source. The adapter restores that source's relocation
lineage on its private clone, then changes `Line.color = 9` and the ARGB value.
Already matching colors require no edit. The native bounds getter supplies the
same rectangle insertion will use for its placement center. Mutable bounds
cache bytes are excluded from content fingerprints: native zero-offset
translation invalidates this cache, and recomputation need not preserve it.

Version 0.6.0 preserves the relocation origin, RM field 7: the reader stores it at `SceneItem+0x20`
(`0xeda40c`–`0xeda418`), and the writer loads it at `0xedc6f4` and emits tag
`0x7f` at `0xedc82c`. Line clone clears this field at `0xed7290`; native
selection-record construction restores the source origin when its low 48 bits
are nonzero, otherwise the source's own ID. Recoloring follows that rule so
multipart objects retain ownership across replacement and Undo/Redo. Ordinary
Copy retains its native zero origin and creates independent duplicate roots.
`SceneLineItem+0xa8` is a separate RM field 9 rendering value and is untouched.

The original's expected world fingerprint applies a group offset only when
the native group marker has nonzero low 48 bits. It reproduces native
translation's float offsets and float point additions (`0xed7190 → 0xf79640`).
An exact multiset match includes full point pressure/width bytes and style,
handles reordered clones, and refuses missing, changed, or additional ink.
The worker verifies each inserted clone still has its expected relocation
origin as well as its immutable expected line content.

The worker callback rechecks the Scene epoch and complete original snapshot
before mutation, and preflights normal native history refusal gates:

- `history+0x10` owns the exact callback Scene.
- The 16-byte author ID at `scene+0x26a` is nonzero. Helper `0x10587e0` tests
  the all-zero case; native rejection reports an invalid CRDT author ID.
- Global byte `0x1a676b0` has bit zero clear, the native read-only check.
- The selected layer's native node exists and still resolves to the same ID.
- Empty history is accepted; otherwise the previous command must resolve to
  known-false merge function `0x496520`. A native `SceneMacroAction` (vtable
  `0x1680820`, merge slot `+0x28` = `0xe4efc0`) returns false when empty, or
  delegates to its last child's merge function. The shared `NativeHistoryGuard`
  follows only that exact macro's final child, with mapped-memory validation,
  cycle detection, at most 64 macro levels and 4,096 cumulative child entries.
  Unknown merge implementations and malformed lists are refused. This accepts
  a prior grouped recolor/transform whose final command cannot merge, while
  preserving the separate-history-entry requirement. The guard never calls a
  merge function to probe it: such a call could change history.

Deletion must add exactly one history entry; insertion must add exactly one
more. After verifying the inserted IDs, geometry and colors against the private
clones and immutable pre-insertion fingerprints, `0xe51880(history, 2)` groups
exactly that pair. The history count must then be the original count plus one.
Mappings are refreshed before each post-mutation history or selection read,
because native allocations can extend the heap or add mappings.

An ordinary insertion refusal compensates the already recorded deletion with
native Undo. A post-insertion geometry mismatch compensates both recorded
commands. Unknown history deltas never trigger guesses that could Undo an
unrelated edit. Native allocation exceptions after partial mutation do not
provide a verified atomic rollback contract; the implementation does not claim
one. This is the native edit/history reliability model, with explicit ordinary
refusal checks and one macro on success. `atomicUndoValidated` remains false
until physical acceptance checks establish the actual device behavior.

## Native command and selection evidence

`SceneDeleteAppendItemsCrdt` can record deletion and insertion in one native
history entry. A scan of all executable `.text` references to its vtable
`0x1680e98` found three constructions:

| Construction | Referencing instruction | Enclosing operation |
| --- | --- | --- |
| Delete | `0xec440c` | Scene delete, entry `0xec4180` |
| Transform | `0xec61c8` | Scene transform, entry `0xec60f0` |
| Insert | `0xec732c` | Scene insertion, entry `0xec7130` |

Each construction inlines native allocation and initialization. No callable
constructor or builder accepting arbitrary originals and replacement clones
has been verified. The transform entry has the effective ABI
`bool(Scene*, int layer, const QTransform*)`; it consumes native selection state
and does not accept recolored clones.

More significantly, this command records an edit that its caller has already
applied. The transform path performs these calls before history submission:

| Call site | Destination | Observed operation |
| --- | --- | --- |
| `0xec6588` | `0xf3b040` | Remove selected originals from the live group |
| `0xec6594` | `0xf3df20` | Install the clone list |
| `0xec65a0` | `0xf3de20` | Apply the transform |
| `0xec6bb8` | `0xe50f60` | Submit the resulting history command |
| `0xec6c34` | `0xe51880` | Group the resulting history-size delta |

The command's first redo, `0xe564c0`, checks its flag at `+0x30`. When the flag
is set, it calls virtual slot `+0x30`, resolving to `0xe53680`, to recalculate
selection bounds, then clears the flag. It does not perform the replacement.
Undo and subsequent redo use `0xe55f40`: remove the record's second list,
append its first list, and swap the two lists. Applicability at `0xe53f30`
checks that a referenced group exists; it does not validate a proposed complete
replacement or establish rollback.

The native allocation is `0x48` bytes, with the command at allocation `+0x10`,
control-block vtable `0x1682598`, and a list of `0x58`-byte records. These facts
do not establish a safe command-construction API. Copying this layout and
installing a vtable would neither perform a new replacement nor prove the
ownership, failure, and selection-restoration contracts needed here.

The implemented route calls native delete `0xec4180(Scene*, int layer)` and
native insert `0xec7130(Scene*, int layer, Items*, QPointF center)` directly
inside the same serialized callback. Both functions' ordinary false returns
are invalid-layer refusals before mutation; delete can also return true for an
empty selection without recording anything, which is why both snapshot and
history-delta checks are required. Their separate commands do not merge, so
the callback explicitly groups them before returning. It never queues two
independent GUI mutation requests and never changes the live originals' color.

Native Copy reads `node+0x138` at `0x8994ec`, tests its low 48 bits, and when
nonzero translates the clone by the QPointF at `node+0x1b8/+0x1c0`
(`0x899500`–`0x899510`). Those exact fields and selected membership are included
in the original snapshot; a group-only movement must invalidate it.

The native insertion preprocessor `0xec82b0` forwards non-image entries as the
same shared pointers in the original order. Its only special branch is type
7, an image. The all-line adapter therefore uses an equivalent QList copy.
The line bounds getter is virtual slot `+0x20`, `0x6111e0 → 0xf78c70` on the
Line at item `+0x48`. It returns a positive cached rectangle, or recomputes and
caches one using point bounds and native tool/scale padding. Using this getter
on private clones before taking the union preserves insertion position even
when the input cache was empty.

Insertion installs selected items at `0xec77c4 → 0xedf930 → 0xf695f0`, then
emits selection rectangle and presence notifications at `0xec77d8` and
`0xec7824`. Grouping `0xe51880` only reorganizes retained history commands;
it does not alter that selection. Native Undo is `0xe50b20(history)`, traced
from the real Undo callback at `0x896680`. Undo does not separately restore
the original selected QList, so a compensated failure must refresh the UI's
current selection instead of assuming the original handles remain active.

## Worker completion route that is verified

`requestThreadSceneCallback` at `0xad71d0` has the effective ABI:

```cpp
uint64_t request(void *worker, const QString *pageId,
                 const std::function<bool(Scene*)> *callback);
```

The exact metatype name at `0x1549470` is
`std::function<bool(Scene*)>`. The request copies the callback into a QVariant,
queues request type `27`, and returns its own enqueue job ID. Worker dispatch
invokes the copied callback at `0xae3fb8` while holding the native page context.
The worker emits `jobCompleted(qulonglong)` after dispatch returns, including
refused requests and callbacks returning false. Thus this signal alone is not
a successful edit receipt or evidence of disk durability.

The adapter connects before enqueue and retains the returned exact job ID.
Its callback captures only a shared result, retained private clones and an
atomic cancellation flag; it never captures a controller or adapter QObject.
The GUI owns guarded QObject references and rejects obsolete worker, page and
Scene epochs. Cancellation before the edit phase refuses the request. Once
deletion begins, the callback finishes its pair; page changes can discard its
obsolete GUI result but must not stop halfway through the native edit.

Successful confirmation requires both the exact job's adapter-owned committed
result and a fresh original-selection snapshot matching the expected inserted
IDs, node state, geometry and color. A 30-second timer only reports continued
waiting and keeps `busy` set; it cannot release the edit lock while the native
job may still be running. After the exact job has finished, bounded retries can
report an unconfirmed current selection without claiming nothing changed.
The callback's return bool publishes dirty scene regions after an edit or
compensating Undo independently of the adapter-owned success result.

## Validation and remaining device checks

`editor-native-history-guard-tests` exercises the same mapped-memory traversal
used by both selected-stroke color and native object insertion/replacement.
It covers consecutive grouped edits, nested and empty macros, unknown final
children, cycles, missing mappings, invalid list counts and bounded traversal.
The exact macro delegation evidence is retained privately in
`.local/qa/native-history-071/`.

`editor-native-selection-color-tests` checks successful one-step Undo/Redo in a
deterministic history model, cancellation before and during the pair, ordinary
refusal compensation, geometry mismatch, unknown history deltas, partial native
exceptions, the unsupported-host gate, exact job matching, timeout lock
retention, page changes and controller destruction. The actual AArch64 path is
compiled with the pinned SDK. These checks do not replace physical recolor,
one-step Undo/Redo, page reopen and durability acceptance checks.

Investigation evidence is in the private, unshipped
`.local/qa/native-transactions/` directory: `scene-commands.txt`,
`command-vtable.txt`, `history-submit.txt`, `worker-callback-case.txt`,
`worker-loop.txt`, and `findings.md`. The existing `xrefs.py` and
`native-abi-3.28/abi-inspect.py` scripts reproduce the static address checks.
