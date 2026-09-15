# Native Properties sessions, version 0.8.1

Opening the native Properties inspector captures the stable selected page and
its history checkpoint. Edits are applied through the existing native workers
and remain visible immediately. Accept/close discards the checkpoint and keeps
the edits. Cancel restores all attributed changes from this opening and closes
only after its worker result is confirmed. The inspector remains available
after deleting its selection. A later opening captures a fresh baseline.

`NativeScene` brackets parametric replacement, color, affine transformations,
duplication and deletion with exact before/after snapshots. Each confirmed
operation must append exactly one isolated command, preserve the preexisting
history prefix and satisfy the existing content/selection receipt. Replacement
and color checkpoints are taken after native history grouping. An unrelated
history/content change invalidates the session; it is never absorbed as an
owned edit. At most 128 commands are attributed to one session.

`NativeHistorySnapshot` retains the native shared commands and their control
blocks. A bounded fingerprint records complete macro child structure as well
as root identities, preventing pointer reuse or regrouping from masquerading
as an unchanged command. The private reader checks owner, list layout, executing
flag, control blocks, cycles/depth and aggregate traversal bounds. These pointers
are ephemeral process state, never saved in object metadata or exposed to QML.

Cancellation uses the existing verified `requestThreadSceneCallback` Page-lock
path. It checks exact current ink and the owned history suffix before mutation,
clears native selection, then invokes `historyUndo` at `0xe50b20` only on those
commands. Every step checks the expected history tail/prefix and the final
result must have empty selection and restored canonical native content.
This function returns void; calling it is not a success receipt.

Native Undo can reinsert a retained Line under a fresh CRDT own ID and set its
origin to its prior ID. The restoration comparator therefore uses parent,
effective lineage and full canonical Line/style/pressure/timestamp/render-group
content. Strict own IDs and raw versions remain mandatory before editing and
before starting cancellation. Bindings continue to resolve revisions through
the observed native lineage.

Normal Undo behavior is retained: undone session commands move to Redo. A Redo
branch discarded by the first applied edit is not recreated by Cancel. This is
restoration of the page's ink, not exact restoration of prior history storage
or proof that the tablet has saved to disk.

The popup footer uses session Annuler/Valider. Drawing palettes retain ordinary
document Undo/Redo. Session cancellation discards unsubmitted field text;
validation commits that text once before closing. Color text submission is
tracked by actual user edits so Enter followed by focus loss cannot enqueue
the same color again while waiting for the native receipt.

Host tests cover real QML button/input routing and asynchronous closure. Worker
tests cover multiple owned edits, ordinary and parametric ink, fresh Undo IDs,
prefix/suffix ownership, rejected external changes, retained macro structure,
bounded traversal, page cancellation and failed receipts. Exact ARM compilation
and offline QMD tests supplement these tests; physical UI/Undo testing remains
separate.
