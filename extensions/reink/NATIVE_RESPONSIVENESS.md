# Native responsiveness refactor

This change reduces work performed by reInk in Xochitl's GUI thread, especially during native navigation, opening the stencil palette, and drawing a long stroke. The native compatibility gate, native history and native display acknowledgement remain in place.

## Update paths

`NativeScene::changed` publishes tool, selection, operation and page changes. `previewChanged` publishes live geometry and small overlay state. `EditorAdapter` caches each state snapshot until its revision changes. A pen move therefore updates the preview without reevaluating every toolbar, catalogue and properties binding. Direct compatibility reads still see the current state.

Clean refreshes compare the native page context and reuse the object snapshot. Native contents changes, explicit object invalidation and page/layer changes request fresh observation. When reInk is inactive, ordinary native navigation does not publish artificial scene changes. The QML host listens to view transforms only while it has custom geometry to cancel or reproject; it coalesces projection updates without requesting a new object scan.

Native object inspection obtains the retained page and its scene inside the native worker callback, where Xochitl already holds the page lock. GUI context resolution does not traverse `PageData` or take its lock. Scope and scene identity checks continue to reject stale mutation requests.

Binding-file I/O, JSON parsing, model validation and connection indexing run on a serial background thread over copied geometry. Native history references stay on the GUI thread. Publication checks the request generation, page/layer, content revision and pending edit. A native change during the calculation triggers another guarded read; an already registered insertion is preserved without registering twice. Input remains gated until the operation settles.

## Palette lifecycle

The immutable stencil catalogue is built once and shared by adapters. The hidden palette reads neither the catalogue nor page state. Its list creates only delegates needed by the viewport and reuses them when scrolling. Reopening retains the loaded palette and catalogue. Hidden thumbnail canvases do not repaint. Properties-session changes are deferred out of reactive binding evaluation, preserving Accept and Cancel behavior.

## Live preview and pen routing

`PreviewFrame` transports immutable typed geometry through QML. Points, bounds and colors are validated once per published frame; painting no longer unpacks a map for every point. `NativePreviewItem` sizes its backing image to visible ink bounds instead of the entire page. An empty preview has zero dimensions and no `ItemHasContents`, so it contributes no idle painted surface. Existing antialias margins, caps, joins and native handover rules are retained.

Solid freehand drawing reuses its validated source polyline. A read of the last point no longer detaches the entire vector on every move. Copy-on-write preserves externally retained snapshots. Native selection sampling also shares paths that already meet the spacing requirement, while preserving validation and point budgets.

The input scheduler publishes native input regions synchronously when reInk changes pen ownership or restores proximity capture. Consecutive contacts with unchanged routing no longer trigger an extra walk of Xochitl's item tree. Xochitl retains its own `beforeRendering` region update. The physical eraser is still classified before native first-point routing.

## Measurements

These are reproducible Release measurements on the host with Qt 6.2.4, using a frozen copy of the previous source. They demonstrate eliminated CPU work and raster allocation; they are not end-to-end tablet latency measurements.

| Workload | Before | After |
| --- | ---: | ---: |
| 100 clean custom refreshes, 4,096 lines / 524,288 points | 100 object inspections, 200 broad updates | 0 inspections, 0 broad updates |
| 12,000 solid freehand moves | 518.27 ms | 1.68 ms |
| Resulting freehand vertices | 12,001 | 12,001 |
| Real Qt backing image for a 160-pixel line | 1404 × 1872 | 168 × 8 |
| Painted surface while preview is empty | Full viewport | None |
| Hidden catalogue delegates | 29 | 0 |
| Visible catalogue delegates in the test viewport | 29 | 5 |
| 400-symbol stress catalogue delegates | 400 | 5 |

The backing image still grows when visible ink covers a large area. Changed preview geometry still needs projection and painting. Native pinch responsiveness and e-ink perception must also be checked with real gestures on the tablet.

## Evidence and regression coverage

The local evidence is in `.local/qa/native-performance-refactor/`: baseline source hashes, adapter and scene benchmarks, actual Qt backing-image tests, palette construction counts, input-routing review, complete host test output, ARM build and device installation records.

Coverage includes preview colors and clipping, long-stroke geometry equivalence, frame acknowledgement, selection transformations, native history, first-point pen/eraser routing, finger gestures, object workflows, and synchronous/asynchronous properties Accept/Cancel. Performance regressions use deterministic counts and shared-buffer identity rather than fragile wall-clock assertions.

The final host build, made in an empty build directory after source freeze, passes all 32 CTest suites. The popup reopen workflow also passes five consecutive runs. Six optional baseline/screenshot cases are skipped by the ordinary CTest command; those separate measurements and visual captures are recorded in the UI evidence directory. The final module also builds from an empty directory for AArch64 with the pinned device SDK.

## Installation correction

The first incremental ARM artifact caused a startup loop and was rolled back to the preceding module. Compilation had overlapped a header change: `NativeSceneObjects.cpp` retained the old `NativeScene` layout while other translation units used the new layout. Compiled field accesses differed by 240 bytes. The defective artifact is retained as rejected evidence and must not be installed.

The replacement was built with all sources frozen in a new, empty ARM build directory. A binary check compares complete debug layout information with representative compiled field accesses in every production translation unit using `NativeScene.h`. The replacement passes all 15 checks; the rejected artifact fails the seven checks affected by the stale object file. This establishes binary coherence; tablet startup is verified separately. The installation guard now requires 75 consecutive seconds with the same active Xochitl process and restart counter before reporting success.

Detailed evidence is in `device-install/ARM_LAYOUT_CRASH_ANALYSIS.md`, `device-install/clean-layout-check/`, the `host-clean-*` logs, and `device-install-attempt2/`. Future release builds must freeze source edits before compiling and use an empty build directory after any overlapping edit/build session.

The replacement module (`66e3ac19988660eb707e9b38d4937a3b7df6e14cf2d03577727f5081c7a06a07`) is installed on the target tablet. The startup guard passed. A subsequent read-only check confirmed the same Xochitl process, zero restarts, both modules mapped, the exact target binary hash, and no fatal, reInk QML or native-object errors in the startup journal. A temporary Wi-Fi interruption was resolved by reading the existing transaction; installation was not repeated. Both fresh backups were verified on the PC, covering 376 user-data files, including 317 document files.

On 9 September 2026, the user tested pinch zoom inside a document and opening the Stencil panel on the tablet and reported a clear improvement. This is physical user feedback, not an instrumented latency or frame-rate measurement.
