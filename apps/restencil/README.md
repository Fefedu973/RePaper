# reStencil

Native Qt 6 / C++17 electronics symbol catalogue. Works without Avermate, an account or a network connection.

The catalogue contains 20 original vector symbols (IEC/ANSI resistors, capacitors, coil, diode/LED/Zener, NPN/PNP/MOSFET, op-amp, voltage/current sources, battery, grounds, switch, connection node, connector and arrow). Search, categories, favourites and anchor preview operate locally. SVG, PDF and `.paper-scene.json` exports use the same vector geometry. **Importer les traits en bibliothèque** delegates a native notebook import to Paper Bridge; it never writes Xochitl storage itself.

## PC emulator

```sh
QT_QPA_PLATFORM=xcb QT_QUICK_BACKEND=software restencil --emulator
```

Select a symbol, then **Insérer dans la page d’essai**. This opens the editable test page beside the catalogue sidebar. Symbols can be selected, dragged, resized, rotated, duplicated and undone. The page saves atomically between runs. This in-process adapter is implemented in `../../extensions/restencil` and is explicitly an emulator target, not a claim that the corresponding Xochitl ABI has been validated.

Without `--emulator`, insertion in an active Xochitl document is disabled with a reason. The exact target and remaining validation are documented in `../../extensions/restencil/README.md`. This device requirement from the user's annotations remains outstanding.

## Build and verification

From this workspace:

```sh
cmake -S apps/restencil -B build/restencil -G Ninja -DBUILD_TESTING=ON
cmake --build build/restencil
ctest --test-dir build/restencil --output-on-failure
QT_QPA_PLATFORM=xcb QT_QUICK_BACKEND=software build/restencil/restencil --emulator
```

Requires Qt >=6.2 Core, Gui, Qml, Quick, QuickControls2, Network, Concurrent, OpenSSL and QtTest for tests. The workspace's `shared` library provides BridgeClient. The reusable canvas currently lives in `apps/reink/src`; its geometry primitives live here under `src/Geometry.*`.

Tests cover symbol bounds, distance-based dash sampling, phase/corner handling, snapping, export encoding, every symbol and a pen tap through the actual Bridge native scene writer, and actual QML sidebar clicks followed by move/scale/copy/undo. An xcb/WSLg visual test can write a screenshot using `PAPER_UI_EVIDENCE=/absolute/path.png build/restencil/restencil_ui_tests`. `restencil --screenshot /absolute/path.png` captures the initial UI and exits.

App data uses `QStandardPaths::AppDataLocation`, organisation `RePaper`, application `restencil`. Exports go in its `exports` directory. Favourites use QSettings. No credentials are stored by this application.

The packs are original geometry, not copies of GPL stencil assets. They follow the workspace licence; relevant external references and the native integration boundary are recorded in the extension documentation.
