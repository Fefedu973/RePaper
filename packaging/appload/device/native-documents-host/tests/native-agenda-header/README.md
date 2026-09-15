# NativeAgendaHeader tests

This suite builds the real `NativeAgendaHeader` with an original synthetic
JSON fixture stored in `QTemporaryDir`. It includes no firmware template,
native font, account data or notebook files. The injected constructor keeps
all source and output operations inside that temporary directory.

Coverage includes generated slot positions, preservation of unrelated items
and writing lines, absence of a static title, deterministic identifiers,
reuse without rewriting existing files, normalized input, date/time limits,
source digest mismatch, unsupported source shape, unavailable paths, and
refusal to reuse conflicting template/content/metadata files. Existing notebook,
moved-template and deleted-template entries are rejected before creating a
template; an unrelated synthetic body file remains byte-identical.

From Linux/WSL with Qt 6.2+ Core/Test and CMake 3.21+:

```sh
cmake -S packaging/appload/device/native-documents-host/tests/native-agenda-header \
  -B /tmp/native-agenda-header-tests -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/native-agenda-header-tests --parallel 2
ctest --test-dir /tmp/native-agenda-header-tests --output-on-failure
```

These tests exercise local file generation. They do not establish that
Xochitl loads the generated template, and they do not exercise its native
title API or a physical tablet.
