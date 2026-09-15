# reInk

Experimental native Qt 6 / C++17 drawing canvas, usable offline and without Avermate.

Implemented tools: freehand, straight line, orthogonal L wire (both corner orientations), arrow, and 20 electronics symbols shared with reStencil. Every tool produces vector strokes. Dashed and dotted patterns follow arc length, so spacing does not depend on pointer sampling speed. Anchor snapping takes precedence over the optional grid. Selection supports move, resize, 90-degree rotation, duplicate and deletion; document edits support undo/redo. The drawing is saved atomically after each completed edit, and unreadable existing data is preserved rather than overwritten.

In **Sélection**, lines, arrows and L wires show two round endpoint handles (48 logical pixel hit targets). Drag a handle to move only that end, or drag the body to move the whole object. The arrowhead and orthogonal corner are rebuilt; snapping uses other objects' anchors, then the enabled grid. Edits that would put an arrowhead outside the page are refused. **Propriétés** changes the selected object's width, continuous/dashed/dotted stroke, and arrow direction (end, start, both, none). The L button changes the selected wire's corner orientation. Controls that do not apply to a selected symbol are disabled; symbol width and whole-object transforms remain available.

Drawing schema 2 stores source geometry alongside the vector strokes, preserving endpoint/style edits across restart and undo/redo. Schema 1 drawings are accepted: an old arrow, line or L wire gains editable endpoints only when its saved strokes can be reproduced exactly. Other old groups remain movable/scalable rather than being guessed into a different shape. Arbitrary curve control-point editing is outside this feature.

SVG/PDF exports and the native `.paper-scene.json` contract are available. **Bibliothèque** asks Paper Bridge to create a native notebook, using a content hash for idempotency. PDF exports contain the drawn strokes as page content; native scene import is the path for individually manipulable handwriting. App code never modifies an active Xochitl document.

## Run on PC

```sh
cmake -S apps/reink -B build/reink -G Ninja -DBUILD_TESTING=ON
cmake --build build/reink
ctest --test-dir build/reink --output-on-failure
QT_QPA_PLATFORM=xcb QT_QUICK_BACKEND=software build/reink/reink
```

Qt >=6.2, OpenSSL, and the workspace `shared` library are required. Use xcb under WSLg; the host's offscreen platform runs tests but does not support `grabWindow()` screenshots. `reink --screenshot /absolute/path.png` captures the UI and exits.

App data uses organisation `RePaper`, application `reink`, via `QStandardPaths`. `drawing.json` stores editable groups/anchors and `exports/` contains generated files. Maximum counts bound hostile or excessively large input, including dash expansion before allocation. Each saved group is restored as an independently selectable object. The geometry tests in reStencil verify the shared native writer contract; canvas tests here cover real QML mouse endpoint dragging and keyboard property changes, 936×1248 layout, arrowhead boundaries, L routing/snapping, cancellation, undo/redo, restart, legacy migration, symbol insertion and protection of malformed saved files.

Not yet implemented: Xochitl drawing-toolbar integration, obstacle-aware routing, pressure-sensitive width, SVG import, full multi-page document management, and document-to-stencil pack authoring. The current PC implementation is not device latency or e-ink refresh evidence.
