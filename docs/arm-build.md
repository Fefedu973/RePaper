# Compilation ARM locale

La compilation croisée du projet complet a été vérifiée le 4 septembre 2026 sous WSL Ubuntu avec le SDK reMarkable **5.8.203**, métadonnées `a53cae3de9f45417b97efc4c46c42e4c8ebdb939` (snapshot SDK du 27 août 2026). Le SDK fournit GCC 13.4.0, Qt 6.10.3, Boost 1.84, OpenSSL 3.5.6 et zlib 1.3.1. PDFio est compilé depuis la copie versionnée dans `external/pdfio`.

Depuis la racine du projet dans WSL/Linux :

```sh
bash tools/build-arm.sh /root/repaper-arm
```

Le script utilise par défaut `/opt/repaper-sdk/5.8.203`, configure `Release` et `BUILD_TESTING=OFF`, puis construit les cinq applications et le bridge. Il ne lance aucun exécutable ARM et ne réalise ni SSH, ni installation, ni opération sur une tablette. Aucun changement CMake n’a été nécessaire pour ce SDK.

Le répertoire doit être distinct d’une compilation PC. Le SDK doit déjà être installé. Les variables `REPAPER_SDK_ROOT`, `REPAPER_ARM_BUILD_DIR` et `REPAPER_BUILD_JOBS` permettent des choix explicites ; utiliser un autre SDK demande une nouvelle validation de compatibilité. Sans argument, le répertoire est `~/repaper-arm`.

| Composant | Binaire produit sous `/root/repaper-arm` |
|---|---|
| Paper Bridge | `bridge/paper-bridge` |
| reMoodle | `apps/remoodle/remoodle` |
| reAgenda | `apps/reagenda/reagenda` |
| reStencil | `apps/restencil/restencil` |
| reCalc | `apps/recalc/recalc` |
| reInk | `apps/reink/reink` |

Les six sorties sont des exécutables **ELF 64 bits little-endian AArch64**, liés dynamiquement, avec interpréteur `/lib/ld-linux-aarch64.so.1`. Le script vérifie leur architecture avec `readelf`, écrit les formats dans `arm-artifacts.txt`, leurs empreintes dans `SHA256SUMS` et les informations SDK dans `arm-build-info.txt`.

La compilation réussie vérifie les en-têtes, les ressources Qt et l’édition de liens pour cette cible. Elle ne prouve pas le lancement avec l’OS 3.28 de l’appareil ni la compatibilité AppLoad/Xochitl. Les tests métier et d’interface restent exécutés dans la compilation PC ; les tests de latence et de rafraîchissement e-ink nécessitent une validation matérielle séparée.

Le build conserve les avertissements du compilateur. GCC 13 émet notamment `-Wstringop-overflow` dans le code AES de PDFio ; Qt 6.10 signale l’ancien `QDateTime::setTimeSpec` et des résultats de `QFile::open` ignorés. Ces avertissements n’empêchent pas la compilation et ne sont pas masqués par des options CMake.
