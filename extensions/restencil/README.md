# reStencil active-page adapter

The user's corrections in specification §§1.3 and 10.2 require insertion in the active document from its editing sidebar. This remains a required device feature. A library-only workflow is not considered its completion.

## Implemented PC path

Run `restencil --emulator`. Select a symbol, then **Insérer dans la page d’essai**. The same window contains an editable page and the catalogue sidebar. `ActivePageAdapter` calls the real `InkCanvas::insertSymbol()` method in the same process. Symbols are arrays of vector strokes, not screenshots; selection, drag, scaling, rotation, duplication and undo mutate those strokes. The page can be exported through the same native scene contract as reInk.

This is an application-level emulator of the page editing contract. It does not emulate Xochitl, its private ABI or its framebuffer/input pipeline. The UI explicitly distinguishes this mode from validated tablet support. Without `--emulator`, the adapter refuses insertion even if a page object is attached.

## Exact device target and evidence

`compatibility.json` pins Paper Pro `ferrari`, firmware `3.28.0.169`, Qt `6.10.3` and the SHA-256 of the locally inspected Xochitl executable. No native extension is enabled or installed by this directory.

Read-only inspection of that executable identified Qt metadata strings for `SceneController`, `addDrawingLine`, its `Line` argument, `applyPendingEdit`, `cloneAddAndSelectItems`, and clipboard text/image methods. A method name alone does **not** establish the construction, ownership or ABI of `Line`, nor an undo transaction. No guessed call is shipped.

Primary sources inspected on 2026-09-04:

- [AppLoad PR #59](https://github.com/asivery/rm-appload/pull/59), head `40506d47427123f07030bb2e83453a43d035b16a`: updates the AppLoad QMD hooks for 3.28, breaking compatibility with <=3.27. Author reports testing on Paper Pro Move 3.28.0.157. This establishes a launcher compatibility path, not a native stroke insertion API for Paper Pro 3.28.0.169.
- [smart_remarkable](https://github.com/yangg1224/smart_remarkable/tree/cb787065281b7211b012bd5e5d9be751fe5adaef): MIT project with a live Qt selection-menu button and synthetic evdev/uinput pen strokes. Its reference button uses GUI-thread traversal and `TapHandler`; it is not an insertion API and its uinput kernel modules are not validated for this target.
- [inkling xovi-ext](https://github.com/nathanmarlor/inkling/tree/089efb2c9f24ce64127c6fc4d7fd30f93ae5ad94/xovi-ext): documents Qt scene traversal, native selection deletion, tool switching, GUI-thread constraints, and the danger of parenting into native selection containers. No arbitrary new-stroke constructor is provided.
- [rm-librarian](https://github.com/rmitchellscott/rm-librarian): GPL-3.0 native library integration. Library import is different from insertion in an active page.
- [XOVI](https://github.com/asivery/xovi): extension framework, not a stable document editing contract.

No GPL source was copied into the applications or this adapter. Symbol geometry and adapter code are original implementations. Any future reuse of GPL extension code must retain its licence and notices in the corresponding separate package.

## Before enabling the tablet adapter

1. Read the live Qt metadata for the exact executable on the GUI thread, without reading document content.
2. Establish a real, typed new-stroke path and its edit/undo ownership; alternatively validate native pen-event input with bounds and cancellation.
3. Establish the actual editing-sidebar attachment point and its lifecycle on this firmware.
4. Validate insertion, lasso, copy, resize, undo, reopen and export on a disposable document.
5. Record evidence and enable only the exact tested firmware/executable combination. Unknown firmware stays disabled.

The adapter must never write files belonging to an open document from outside Xochitl.

## Common reInk / reStencil sidebar package

The reusable panel and shared typed adapter now live in [`../reink`](../reink/README.md) and `../editor-common`. The panel contains both reInk tools and the reStencil catalogue. Its PC build uses the same original geometry and canvas as this application. Its native build contains neither the PC canvas nor any document mutation implementation: the exact-target XOVI module only registers the QML type/resource, with native editing unavailable. No unverified 3.28 QMD attachment is shipped. See that package's compatibility manifest and tests for the precise supported scope.
