# Native Markdown and mathematics for RMChat

`RichMessage` is a Qt Quick painted item. Markdown is parsed by Qt's native `QTextDocument` GitHub Markdown implementation. LaTeX is parsed and laid out by the vendored MicroTeX C++ engine and painted through its Qt backend. Fractions, radicals, indices, Greek symbols, integrals, sums, delimiters, matrices and aligned equations are typeset as mathematics.

The implementation uses neither a WebView nor WebEngine, TeX executables, shell commands, online renderers, network downloads or external runtime font directories. Its 30 unmodified font binaries and their notices are embedded with Qt resources. Exact versions, license notices and local changes are recorded in [THIRD_PARTY.md](THIRD_PARTY.md).

## Integration

Add this directory with CMake, link `rmchat_rich`, include `RichMessage.h`, and call `rmchat::registerRichTypes()` before loading QML:

```qml
import RePaper.Rich 1.0

RichMessage {
    width: parent.width
    height: contentHeight
    text: model.messageText
    fontPixelSize: 21
    color: "black"
    onLinkActivated: function(url) { localFiles.openExternalLink(url) }
}
```

Properties include `text`, `fontPixelSize`, `color`, `contentHeight`, `hasMath`, `mathErrorCount` and `truncated`. `implicitHeight` follows the measured document height. A single 40 ms timer coalesces streamed updates; changing width recalculates wrapping. A parent Flickable should use the resulting measured height.

Only deliberate press/release on the same HTTPS anchor emits `linkActivated`. The item never opens URLs itself. The application must apply its normal external-link policy to that signal.

## Supported input and fallback

- Native Markdown: headings, emphasis, strikeout, ordered/unordered/nested lists, task lists, quotations, inline/fenced code, tables and links.
- Inline mathematics: `$...$` and `\(...\)`.
- Display mathematics: `$$...$$` and `\[...\]`.
- Escaped delimiters, fenced code, inline code and indented code remain literal. The scanner only finds boundaries; MicroTeX performs mathematical parsing and layout.
- Incomplete formulas remain source text during streaming. Invalid, unsupported or over-budget formulas remain source text, with an error count for the UI. They are never silently deleted.
- Raw HTML remains literal. Every document resource request is rejected without delegating to Qt's file loader. Images become their alt text and are not loaded, including local, remote and data URLs.
- TeX macro definitions, file/package access, global renderer mutations and several unbounded extension commands are rejected. This is a mathematical expression renderer, not a full TeX document interpreter.

## Bounds

The message input is capped at 128 KiB UTF-8, with an explicit visible notice when truncated. At most 128 mathematical spans are considered. Each expression is capped at 8 KiB, 32 nested braces, 1,024 commands and bounded matrix rows/columns. The embedded parser has 64-frame depth and operation/deadline checks; a formula receives at most 75 ms and the document receives a shared 350 ms mathematical processing budget. These are cooperative checks, not an OS-enforced execution deadline.

Rasterized formula dimensions and pixels are bounded. The global formula cache and each document's retained formula images are independently capped at 8 MiB. The document paint surface is capped at 4 million physical pixels, accounting for device pixel ratio, and 6,000 logical pixels in height. Very long messages retain an explicit truncation notice; callers can offer the original text separately. Long tables are remeasured after truncation, with a bounded plain-text excerpt if their layout still exceeds the surface limit.

All font registration, TeX parsing and document mutation happen on the GUI thread. Paint only draws an already-built document and existing images. The built-in renderer does not log message content or parser exception strings.

## Build and verification

Requires C++17 and Qt 6.2+ Core, Gui, Qml and Quick. Tests also require Qt Test. No dependency is downloaded during configuration or build.

```sh
cmake -S apps/rmchat/rich -B build/rmchat-rich -DCMAKE_BUILD_TYPE=Release
cmake --build build/rmchat-rich -j4
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software ctest --test-dir build/rmchat-rich --output-on-failure
```

Set `RMCHAT_RICH_TEST_OUTPUT` when running `rmchat-rich-test` to save the actual native QML render and 640/1080 px Markdown/math documents. The tests verify Markdown structure, distinct math objects, nonempty mathematical raster output, invalid/incomplete fallback, code/escape boundaries, external-resource blocking, safe links, streaming coalescing, size/depth/memory bounds and 100 invalid formulas. `tests/Preview.qml` exercises the registered QML item directly.

Primary implementation references: [Qt Markdown parsing](https://doc.qt.io/qt-6/qtextdocument.html#setMarkdown), [Qt inline object interface](https://doc.qt.io/qt-6/qtextobjectinterface.html), [Qt Quick painted item](https://doc.qt.io/qt-6/qquickpainteditem.html), [MicroTeX project](https://github.com/NanoMichael/MicroTeX).
