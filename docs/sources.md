# Sources et composants tiers

Les applications du projet sont une implémentation nouvelle. Elles n’embarquent
pas le code d’Avermate. La publication des sources du 16 septembre 2026 place le
code original sous licence MIT, selon le choix de son auteur. Les composants
tiers et les portions dérivées conservent leurs licences : voir
[NOTICE.md](../NOTICE.md) pour les exceptions et la portée de cette publication.

| Référence | Snapshot utilisé | Usage / licence |
|---|---|---|
| [PDFio](https://github.com/michaelrsweet/pdfio) | v1.6.5, `5102eddff28d9b605f3e6783d50ad380f61f692e` | Sous-module `external/pdfio`, parseur PDF ; Apache-2.0, notices incluses dans le sous-module. |
| [AppLoad](https://github.com/asivery/rm-appload) | `1a708d297df4b3c108e78a02ec7e2634ff33033c` | Source privée sous `.tools/`, émulateur/shim construit dans WSL ; GPL-3.0, licence amont conservée avec les sources. |
| [AppLoad PR 59](https://github.com/asivery/rm-appload/pull/59) | `40506d47427123f07030bb2e83453a43d035b16a` | Référence du support des hooks Xochitl 3.28 ; ces hooks ne sont pas injectés par le lanceur PC. |
| [SDK officiel reMarkable](https://developer.remarkable.com/links) | 3.28.0.172, OS 5.8.203, cible ferrari AArch64 | Installateur sous les conditions du fournisseur, non committé au dépôt. |
| [rmscene](https://github.com/ricklupton/rmscene) | `d7d86ca3a8ca4965d911886a1660bc8acf654c1a` | Lecteur indépendant MIT pour vérifier le writer .rm v6 ; aucun code Python incorporé au runtime des apps. |
| [Moodle mobile launch.php](https://github.com/moodle/moodle/blob/MOODLE_405_STABLE/admin/tool/mobile/launch.php) | branche MOODLE_405_STABLE consultée | Protocole du lien mobile, passport et réponse base64 ; implémentation C++ nouvelle. |
| [CPE Lyon — Papillon PR 793](https://github.com/PapillonApp/Papillon/pull/793) | `9c6f30bc332b49a7a3df1a8e412e322298116908` | Contrat HTTP étudié ; [détails](cpe-protocol.md), provider C++ nouveau. |
| Qt, OpenSSL, Zlib, Boost | Paquets Ubuntu ou SDK ciblé | Bibliothèques système, pas copiées dans ce dépôt. Appliquer leurs obligations lors du packaging. |

SHA-256 de l’installateur SDK utilisé :
`4d37b13e673d1963c48e09f1d39182084938bb7990361a54ad73185a49726698`.

Les 20 symboles du catalogue reStencil sont créés dans ce projet. Leurs
métadonnées et les références inspectées sont détaillées dans son README et
celui de l’extension. Le monorepo Avermate original reste intact ; le worktree
`avermate/`, ignoré par Git, n’est qu’une référence de lecture.
