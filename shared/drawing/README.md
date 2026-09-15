# Local native-object reconciliation

NativeObjectRegistry is preparatory local bookkeeping. It is not connected to
NativeScene, does not inspect native memory and never opens a .rm file.

The future verified adapter must supply:

- Opaque identities of the actual native items, not clone addresses or geometry hashes.
- Opaque content versions that differ after a material change and match when Undo
  restores that content. A monotonic edit counter alone cannot provide this contract.
- The complete observed native item set for the active page.
- A transaction-attributed binding for every managed semantic object after a command.

activate(documentId, pageId) creates a fresh epoch and loads the associated state
locked. observe(epoch, items) confirms an unambiguous known state. begin(proposed)
accepts the complete next semantic object set only while editable; confirm(ticket,
bindings) saves it only after the adapter has verified the native result. A canceled
confirmation or an unattributed native event requires a fresh observation. Old-page
observations and late tickets cannot change the active page.

Known observations can restore earlier semantic states after native Undo/Redo.
Unknown edits, partial deletion, overlapping identities and ambiguous histories lock
semantic editing. A native copy is not silently interpreted as a parametric copy:
its new semantic identity and native bindings must be explicitly confirmed.

Files use schema version 1 and a hash of document/page identifiers for the filename.
QSaveFile atomically replaces them with owner-only permissions. A writer lock and
comparison against the loaded file prevent stale writers from overwriting another
session. Malformed, foreign-page or unknown-version files remain untouched. Retention
is bounded to 16 states and 32 MiB; Undo outside retained evidence cannot be certified.

These sidecars do not demonstrate cloud transport or native identity stability.
The registry must remain disconnected until that contract is established on the
target firmware. The existing DrawingModel validates geometry and connections;
parameters stored here are already-validated semantic data from that model.

Standalone validation:

    cmake -S shared/drawing -B /root/repaper-native-registry-build -DBUILD_TESTING=ON
    cmake --build /root/repaper-native-registry-build --parallel 4
    ctest --test-dir /root/repaper-native-registry-build --output-on-failure
