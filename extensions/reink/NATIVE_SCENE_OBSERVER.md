# Native scene change observer

`NativeSceneObserver` observes the existing native Scene on the exact supported
Paper Pro / Xochitl 3.28.0.169 executable. It does not create a Scene, edit a page,
call a private worker callback, or access document files.

Create one instance for each controller/page epoch. `tryAttach()` acquires the
already loaded Page through the same bounded, exact-profile reader as
`NativeSelectionProbe`: retain its existing shared owner under the worker mutex,
release that mutex, then acquire the Page mutex. The Scene's verified QObject
base, vtable and `changed(QRectF)` signal are checked before connecting. Its
`connectNotify` and `disconnectNotify` implementations are inherited from
QObject. No creating Page accessor or Scene method is invoked.

`tryAttach()` revalidates the actual Scene even if the page identifier is unchanged.
A temporary busy result can be retried. `ready()` describes the current binding;
it does not reacquire a Page. Call `tryAttach()` before dispatching an operation
which needs this observer. Destroy the observer on a page/controller change, or
call `detach()` to invalidate it permanently. No document identifier is logged.

The change and destruction connections use queued delivery to the observer's
thread. A QPointer, sender check and current-page check reject stale delivery.
The observer never dereferences a raw Scene after releasing the native lock.
Destroying an old proxy removes its pending calls; destruction of the native
sender invalidates readiness. A queued destruction from an old sender cannot
invalidate a new binding.

The observed native `Scene.changed` is emitted after the in-memory mutation.
It is a wake-up to inspect the result, not proof that our command caused it,
not a disk-flush notification, and not a durable semantic identity. A pending
affine operation must additionally match its expected native stroke geometry.
An unchanged transform should be skipped locally.

Do not substitute `SceneController.documentContentChanged`: that signal belongs
to the SceneTextItem/SceneParticipant text-editing route. Similarly, the native
`areaSelected` emitted by move/scale/rotate is an **affine preview** notification
which can precede `applyPendingEdit`'s later mutation. `working` and queue size
are UI activity indicators and cannot certify completion.

Local verification: twelve QtTest cases cover unsupported desktop gating,
queued delivery, worker-to-GUI delivery, stale pages, detach, sender/proxy
destruction, replacement senders and duplicate-binding prevention. The helper
also compiles through the ARM SDK with its real ABI branch enabled. These checks
do not replace physical-device tests of free scaling, movement and Undo/Redo.
