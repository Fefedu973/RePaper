# Third-party components in the native RMChat renderer

## MicroTeX

- Project: https://github.com/NanoMichael/MicroTeX
- Exact upstream commit: `0e3707f6dafebb121d98b53c64364d16fefe481d`
- Upstream source license: MIT, copyright 2020 Nano Michael; retained in `vendor/microtex/LICENSE`.
- Included: core C++ library, built-in font/symbol definitions, Qt painter backend, and the `res/fonts` base font collection.
- Excluded: demos, applications, alternate graphics backends, optional Greek/Cyrillic alphabet packages and their separate GPL font resources.

The font files are unmodified upstream binaries and are **not** covered by the source-code MIT license. The upstream font notices are retained in `vendor/microtex/res/fonts/licences/` and embedded alongside the fonts:

- `OFL.txt`: SIL Open Font License 1.1 for the AMS fonts `eufb10`, `eufm10`, `msam10`, `msbm10`, including their reserved font names.
- `Knuth_License.txt`: upstream notice governing its Computer Modern font collection.
- `License_for_dsrom.txt`: upstream distribution/modification permission for `dsrom10`.

This is an altered, application-specific subset of MicroTeX. Local source changes are confined to:

| Upstream source | Local change |
| --- | --- |
| `src/latex.cpp` | Explicit embedded resource root; environment and filesystem discovery disabled. |
| `src/core/formula.cpp` | Optional Greek/Cyrillic alphabet registration disabled; built-in mathematical Greek symbols remain available. |
| `src/platform/qt/graphic_qt.cpp` | Static `QFontDatabase` API for Qt 6.2 and Qt 6.10 compatibility. |
| `src/core/parser.cpp` | Cooperative operation/deadline/depth checks through the application `MathBudget.h`. |
| `src/core/macro_impl.h` | Shared ownership of temporary array formulas so parse/budget exceptions release allocations. |
| `src/atom/atom_matrix.cpp` | Bounded array layout/repetition; cooperative checks; RAII ownership of temporary layout arrays and boxes. |
| `src/atom/atom_space.cpp` | Finite, bounded dimensions from TeX length arguments. |
| `src/box/box_factory.cpp` | Bounded delimiter/arrow construction, cooperative checks, and RAII temporary ownership. |
| `src/render.cpp` | RAII environment ownership across layout exceptions. |

`VENDOR_SHA256SUMS.txt` records hashes of the vendored subset, including these local changes. Upstream sources remain identified by the exact commit above.

## TinyXML-2

- Project: https://github.com/leethomason/tinyxml2
- Version: `11.0.0`, exact upstream commit `9148bdf719e997d1f474be6bcc7943881046dba1`.
- Included, unmodified: `tinyxml2.cpp`, `tinyxml2.h`, `LICENSE.txt` in `vendor/tinyxml2/`.
- License: zlib; original source copyright and license notices retained.
- Used by compiled MicroTeX definitions/parsers; RMChat exposes no XML file-loading API to message text.

The project's existing Qt distribution and license obligations continue to apply to Qt Core, Gui, Qml, Quick and the Markdown implementation shipped with Qt.
