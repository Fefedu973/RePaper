# Licences et composants tiers

La publication du 16 septembre 2026 met à disposition les **sources de RePaper**.
Le code original de RePaper est sous [licence MIT](LICENSE), selon le choix de son
auteur. Les composants tiers, les portions reprises ou adaptées de ces composants
et leurs notices conservent les licences indiquées ci-dessous. La licence MIT à
la racine ne remplace aucune de ces licences.

## Code original

La licence MIT couvre le code original des applications, du Paper Bridge, des
outils de développement et des composants partagés, sous réserve des exceptions
ci-dessous. La licence MIT déjà présente dans
[`extensions/reink/LICENSE`](extensions/reink/LICENSE) est conservée ; elle couvre
le code original de `extensions/reink` et `extensions/editor-common` selon leur
[notice de provenance](extensions/reink/licenses/NOTICE.md).

Les symboles et graphismes de démonstration créés pour RePaper sont des contenus
du projet. Les noms des produits et projets cités restent ceux de leurs auteurs.
RePaper n'est pas un produit officiel reMarkable.

## Sources tierces présentes dans le dépôt

| Chemins ou composants | Auteur et source | Licence et notices conservées |
| --- | --- | --- |
| `apps/remoodle/third_party/qrcodegen.cpp` et `qrcodegen.hpp` | Nayuki, QR Code generator 1.8.0, commit `720f62bddb7226106071d4728c292cb1df519ceb` | MIT ; copyright et licence dans chaque fichier. [Provenance](apps/remoodle/third_party/README.md). |
| Sous-module `external/pdfio` | Michael R Sweet, PDFio 1.6.5, commit `5102eddff28d9b605f3e6783d50ad380f61f692e` | Apache-2.0 et exceptions de son `NOTICE`. Conserver également les notices propres aux ressources de test et polices du sous-module. [Source](https://github.com/michaelrsweet/pdfio/tree/5102eddff28d9b605f3e6783d50ad380f61f692e). |
| `packaging/appload/device/qt-linuxfb/upstream`, `upstream-sqlite`, `upstream-evdevtablet` et adaptations de ces sources | The Qt Company et contributeurs, Qt 6.10.3, commit `7ddbc87d8e14ce51d2957ea72d0a6077593d5ff4` | Les sources C++ conservent leur expression SPDX `LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only`. Les fichiers de construction amont sont sous BSD-3-Clause. Les textes libres sont dans [`LICENSES`](packaging/appload/device/qt-linuxfb/LICENSES). Le patch evdev reste soumis aux licences de Qt. [Détails et modifications](packaging/appload/device/qt-linuxfb/README.md). |
| Portions amont copiées ou adaptées dans `apps/rmchat/core` | aurora-develop, Aurora, commit `15983ced4d3a7f9faa3b5871086ebbefa8f8881b` | MIT, copyright amont conservé dans [`LICENSE`](apps/rmchat/core/LICENSE). [`UPSTREAM.md`](apps/rmchat/core/UPSTREAM.md) identifie précisément les fichiers et adaptations. |
| Dépendances du cœur Go de RMChat | Projet Go et google/uuid 1.6.0 | Notices BSD conservées dans [`licenses/go.txt`](apps/rmchat/core/licenses/go.txt) et [`licenses/google-uuid.txt`](apps/rmchat/core/licenses/google-uuid.txt). Les dépendances sont fixées dans `go.mod` et `go.sum`. |
| `apps/rmchat/rich/vendor/microtex`, hors polices | Nano Michael et contributeurs, MicroTeX, commit `0e3707f6dafebb121d98b53c64364d16fefe481d` | MIT ; [`LICENSE`](apps/rmchat/rich/vendor/microtex/LICENSE), inventaire des modifications et empreintes conservés. |
| `apps/rmchat/rich/vendor/microtex/res/fonts` | Auteurs des collections de polices fournies avec MicroTeX | Licences distinctes du code : SIL OFL 1.1 pour les polices AMS concernées, notice Knuth pour Computer Modern, permission propre à `dsrom10`. Textes dans [`res/fonts/licences`](apps/rmchat/rich/vendor/microtex/res/fonts/licences). Ces polices ne sont pas placées sous la licence MIT de RePaper. |
| `apps/rmchat/rich/vendor/tinyxml2` | Lee Thomason et contributeurs, TinyXML-2 11.0.0, commit `9148bdf719e997d1f474be6bcc7943881046dba1` | zlib ; [`LICENSE.txt`](apps/rmchat/rich/vendor/tinyxml2/LICENSE.txt) et notices dans les sources conservées. |

Les adaptations de MicroTeX et la sélection de ses ressources sont décrites dans
[`apps/rmchat/rich/THIRD_PARTY.md`](apps/rmchat/rich/THIRD_PARTY.md). RMChat est
[archivé](apps/rmchat/ARCHIVED.md) et désactivé dans les constructions habituelles ;
ce statut ne change pas les licences de ses sources.

## AppLoad et patches d'intégration

AppLoad est un projet d'[asivery et de ses contributeurs](https://github.com/asivery/rm-appload),
sous GPL-3.0. Le harnais PC se base sur le commit
`1a708d297df4b3c108e78a02ec7e2634ff33033c`. Le support des hooks Xochitl 3.28 se
réfère à la [PR 59](https://github.com/asivery/rm-appload/pull/59), commit
`40506d47427123f07030bb2e83453a43d035b16a`.

Les patches AppLoad de `packaging/appload/device/*.patch`, les portions amont
reprises ou adaptées dans les outils de préparation et les sources AppLoad
modifiées lors de la construction conservent leur provenance et leur licence
GPL-3.0. Cela concerne notamment les fragments de modification AppLoad de
`tools/emulator-apps.py`. Leur présence à côté de scripts originaux de RePaper ne
les place pas sous MIT. Le texte GPL-3.0 est conservé dans
[`extensions/reink/licenses/GPL-3.0.txt`](extensions/reink/licenses/GPL-3.0.txt).
Le checkout AppLoad est acquis séparément ; il n'est pas inclus dans cette
publication. Voir le [harnais PC](packaging/appload/README.md) et les
[sources de l'intégration tablette](packaging/appload/device/README.md).

## Dépendances des modules natifs et portée de la publication

La construction locale des modules natifs emploie deux autres projets :

- **XOVI**, asivery et contributeurs, LGPL-3.0, commit
  [`2b99649f5e4fd6288be7792a8570bd16418adb70`](https://github.com/asivery/xovi/tree/2b99649f5e4fd6288be7792a8570bd16418adb70).
  Le générateur et le code de liaison généré conservent cette provenance ; le
  [texte LGPL-3.0](extensions/reink/licenses/XOVI-LGPL-3.0.txt) est conservé.
- **Scene Assistant**, ingatellent et contributeurs, à partir du travail initial
  de HookedBehemoth/xovi-sudoku, commit
  [`8afbac01ca7816f9aa22a5897a3f2133b0b0d9d7`](https://github.com/ingatellent/xovi-scene-assistant/tree/8afbac01ca7816f9aa22a5897a3f2133b0b0d9d7).
  Son README annonce GPL v2. Les fichiers `rm_Line.cpp`, `rm_Line.hpp` et
  `rm_SceneItem.hpp` sont acquis séparément pour la construction ; le bootstrap
  mutateur `SceneAssistant.cpp` n'est pas utilisé.

Les modules `reink-editor.so` et `appload.so` construits localement combinent
actuellement les portions Scene Assistant GPL v2 et la liaison XOVI LGPL v3.
La compatibilité de cette combinaison pour leur redistribution n'a pas été
résolue dans le projet. Le README amont de Scene Assistant n'accorde pas
explicitement une option « ou version ultérieure » identifiée pour ces fichiers.
La publication initiale contient donc **les sources, sans ces binaires ni les
anciens paquets tablette**. Elle ne présente aucun module combiné comme MIT.
Les instructions de reconstruction locale ne constituent pas une déclaration
de redistribution autorisée de leur résultat. Les faits et le périmètre précis
sont documentés dans [`extensions/reink/PROVENANCE.md`](extensions/reink/PROVENANCE.md).

## Dépendances acquises séparément

Qt, OpenSSL, zlib, Boost, PDFium, le runtime LibreOffice optionnel et les paquets
Node du portail conservent leurs licences propres. Les manifestes de dépendances
et leurs versions servent à reconstruire le projet ; ils ne remplacent pas les
notices des distributions correspondantes. Cette publication n'inclut ni
`node_modules`, ni le SDK reMarkable, ni les bibliothèques ou runtimes des anciens
paquets locaux.

Le firmware Xochitl, ses ressources QML extraites, les modèles de page et les
polices propriétaires reMarkable ne sont pas distribués. Les intégrations
utilisent les ressources présentes sur la tablette de son propriétaire. Les
données de comptes, documents personnels, sauvegardes d'appareil et journaux
locaux sont également exclus.

Les autres références étudiées, sans incorporation de leur code dans les
applications originales, restent recensées dans [`docs/sources.md`](docs/sources.md).
