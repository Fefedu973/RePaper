`native-agenda-folder-tests` runs the production `NativeDocumentsAdapter.qml`
and `NativeDocumentHost` through temporary Unix sockets. Its in-memory native
fixtures expose separate `entry::Id` and `navigation::EntityId` gadgets and the
firmware's `moveEntries(QList<entry::Id>, entry::Id)` signature. The fixtures
never open the user's library, write native notebook files, or contact Xochitl.

The tests cover native meeting-folder reuse and recreation, new notebook parent
selection, the native `LS Dayplanner` landscape template, persistent notebook
identity across host restarts, moving a legacy note once, rejecting failed or
unconfirmed moves, missing/trashed folders, trashed notes, and ordinary notes.
Preservation checks use an opaque fixture ink value and count the native
formatting calls; they do not simulate Xochitl's storage implementation or cloud
synchronization.

Run on a Qt 6 development host:

```sh
cmake -S packaging/appload/device/native-documents-host -B /tmp/native-documents-tests -G Ninja
cmake --build /tmp/native-documents-tests --target native-agenda-folder-tests
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software /tmp/native-documents-tests/native-agenda-folder-tests
```

The `typedJavascriptArrayConvertsToNativeIdList` test also logs a comparison with
an array of UUID strings. Qt 6.2.4 converted that array, but SDK ARM Qt 6.10.3
silently produced an empty `entry::Id` for its element. Passing `[existing.id]`
preserves the actual gadget and passes on both versions. No manual list
converter is installed by the fixture.

Firmware evidence comes from the cached ferrari 3.28.0.169 `xochitl` binary
(SHA-256 `43a9d5d0acc5b998264c16586e11b848f3b83d2d63b5fd322b09c0977d94d3d4`):

- `Library` Qt metadata: file offsets `0xcd14d0` / strings `0xcd1c90`.
  `meetingNotesId()` returns `entry::Id`; `createCollection` accepts only
  `(entry::Id parentId, QString visibleName)`, without an injectable UUID.
- `LibraryController` Qt metadata: `0xe28fa0` / strings `0xe294dc`.
  `moveEntries(QList<entry::Id>, entry::Id)` returns `bool`.
- `QmlEntryWrapper` metadata: `0xdff120` / strings `0xdff610`.
  It exposes `id`, `parentId`, `isTrashed`, and a read-only
  `isExcludedFromSync`; no hidden-folder property was found.
- Native `CalendarActions.qml` calls `library.meetingNotesId()` when its
  experimental current-folder override is off.
- `Library`'s static metacall at virtual address `0x9d4f40`, method index 38,
  resolves to `0x9d5b78`. It first uses the cached collection, otherwise looks
  up the native fixed ID and creates the collection with that same ID only if
  missing. The initializer at `0x47d478` reads its UUID from file offset
  `0x1189fe0`: `94db1b5c-7599-5db2-8052-83077bc87711`.

Production code intentionally obtains the ID through the native method. The
fixture duplicates the observed value only to exercise identity reuse. This
folder is shared with the native calendar and must not be renamed or hidden by
reAgenda. Actual tablet behavior and synchronization require device validation.
