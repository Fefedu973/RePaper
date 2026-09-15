# Third-party provenance

- XOVI / `util/xovigen.py`: author asivery and contributors, commit `2b99649f5e4fd6288be7792a8570bd16418adb70`, <https://github.com/asivery/xovi>. Upstream repository licence: LGPL version 3. The package retains the unchanged generator, generated glue and licence texts. XOVI itself is not bundled or installed.
- Qt is dynamically linked. This package does not bundle Qt libraries; its upstream copyright and licence terms apply separately.
- Scene Assistant layouts/factories: ingatellent and contributors, with original HookedBehemoth/xovi-sudoku work, commit `8afbac01ca7816f9aa22a5897a3f2133b0b0d9d7`, GPL v2. The local archive includes the unchanged files and upstream licence in `third-party/scene-assistant`. Its mutating helper is not built. The combined binary must not be described as MIT-only; see `../PROVENANCE.md`.
- Original code under `extensions/editor-common` and `extensions/reink` is covered by `../LICENSE` (MIT), excluding this third-party material.

No source from GPL-licensed reTaskable or AppLoad is included in this package. Their documented hooks were used as research evidence only.
