# Compiler RePaper

La publication du **16 septembre 2026 distribue les sources**. Les commandes ci-dessous construisent et testent le projet localement ; elles ne se connectent pas à une tablette et n’y installent rien. Les contraintes de licences des composants assemblés sont décrites dans [NOTICE](../NOTICE.md).

## Récupérer le dépôt

Dans un terminal Linux ou WSL :

```sh
git clone --recurse-submodules https://github.com/Fefedu973/RePaper.git
cd RePaper
```

Pour un clone existant :

```sh
git submodule update --init --recursive
```

**PDFio** est le sous-module `external/pdfio`. Le commit enregistré par le dépôt fait partie des sources de la compilation. Une archive GitHub sans ses sous-modules ne suffit donc pas à reproduire ce clone.

## Applications et tests sur PC

Environnement documenté : Ubuntu ou WSL Ubuntu, compilateur C/C++, CMake, Git, Python 3, Qt **6.2 minimum**, OpenSSL, zlib et Boost. Les modules Qt requis comprennent Core, Gui, Network, Sql, Qml, Quick, QuickControls2 et Concurrent ; les tests utilisent QtTest. Le script indique la liste de paquets Ubuntu et possède l’option explicite `--install-deps`, qui utilise `apt-get` et nécessite les droits adaptés.

La commande habituelle, après installation des dépendances, est :

```sh
bash tools/build-pc.sh
```

Elle initialise PDFio, configure une compilation Debug, construit les applications et exécute CTest avec un rendu logiciel hors écran. La sortie se trouve par défaut dans `~/repaper-build`. Pour choisir un dossier et le parallélisme :

```sh
REPAPER_BUILD_DIR="$PWD/build/pc" REPAPER_JOBS=4 bash tools/build-pc.sh
```

Les exécutables sont dans `apps/<nom>/<nom>` sous le dossier de build. Pour une interface visible sous Linux/WSLg, utilisez par exemple :

```sh
QT_QPA_PLATFORM=xcb QT_QUICK_BACKEND=software \
  "$HOME/repaper-build/apps/recalc/recalc"
```

Les programmes autonomes `apps/reink` et `apps/restencil` restent des bancs PC ; ils n’installent pas les outils dans Xochitl. RMChat est archivé et désactivé par défaut.

**rePDF est optionnel** : CMake l’active automatiquement lorsqu’il trouve l’en-tête public `fpdfview.h`. Pour l’exiger, configurez `REPAPER_BUILD_REPDF=ON` et `PDFIUM_INCLUDE_DIR`. La bibliothèque PDFium doit aussi être disponible à l’exécution ; voir [le guide rePDF](../apps/repdf/README.md). N’ajoutez pas tout le répertoire d’en-têtes d’un SDK ARM à une compilation PC.

## Tests de l’éditeur natif sur l’hôte

La suite du module natif est une compilation séparée :

```sh
cmake -S extensions/reink -B build/editor-host \
  -DCMAKE_BUILD_TYPE=Release \
  -DREPAPER_BUILD_XOVI_PACKAGE=OFF \
  -DREPAPER_BUILD_EDITOR_PC=ON
cmake --build build/editor-host --parallel 4
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
  ctest --test-dir build/editor-host --output-on-failure
```

Cette configuration utilise les adaptateurs de test et ne charge pas Xochitl. La campagne figée du **9 septembre** a passé les **32 suites CTest** de l’hôte. Des cas de mesures comparatives et de captures sont optionnels et ne sont pas tous exécutés par la commande ordinaire. Voir [le compte rendu de fluidité](../extensions/reink/NATIVE_RESPONSIVENESS.md) pour la portée exacte des résultats.

## Applications ARM

La compilation croisée documentée utilise le SDK reMarkable **5.8.203**, avec GCC 13.4.0 et Qt 6.10.3. Installez votre SDK localement ; il n’est pas distribué ici. Son emplacement par défaut est `/opt/repaper-sdk/5.8.203`.

```sh
bash tools/build-arm.sh "$PWD/build/arm-apps"
```

`REPAPER_SDK_ROOT`, `REPAPER_ARM_BUILD_DIR` et `REPAPER_BUILD_JOBS` permettent d’adapter les chemins et le parallélisme. Le script compile en Release, sans exécuter les tests ARM, puis vérifie l’architecture des applications principales et du bridge et produit leurs empreintes. Les sorties PC et ARM doivent être dans des dossiers différents. Le [rapport ARM](arm-build.md) décrit le SDK et les vérifications ; son inventaire historique précède l’ajout optionnel de rePDF.

## Dépendances des modules XOVI

La cible native est **Paper Pro `ferrari`, OS 3.28.0.169, Qt 6.10.3**, avec l’empreinte Xochitl fixée par le [profil de compatibilité](../extensions/reink/compatibility.json). Les références utilisées sont :

| Composant | Référence exacte |
| --- | --- |
| XOVI | `2b99649f5e4fd6288be7792a8570bd16418adb70` |
| Scene Assistant | `8afbac01ca7816f9aa22a5897a3f2133b0b0d9d7` |
| Références QMD natives | `67025316fa58f0b9e34b33e374521d29517d7f51` |
| Qt Resource Rebuilder | `7874154dba6793cc68a15fae0fb9dd272c4ed20a` |
| Base AppLoad | `502d03f4a0f4ea39810af1827366cac46bdc045f` |
| Hooks AppLoad pour 3.28, PR 59 | `40506d47427123f07030bb2e83453a43d035b16a` |

Les origines et le rôle de chaque source sont dans [PROVENANCE](../extensions/reink/PROVENANCE.md) et le [guide AppLoad](../packaging/appload/device/README.md). XOVI et Scene Assistant sont des dépendances externes à préparer aux commits exacts :

```sh
mkdir -p .tools
git clone https://github.com/asivery/xovi.git .tools/xovi-native-reference
git -C .tools/xovi-native-reference checkout 2b99649f5e4fd6288be7792a8570bd16418adb70
git clone https://github.com/ingatellent/xovi-scene-assistant.git .tools/xovi-scene-assistant
git -C .tools/xovi-scene-assistant checkout 8afbac01ca7816f9aa22a5897a3f2133b0b0d9d7
```

Après activation du SDK, un build natif local se prépare ainsi :

```sh
source /opt/repaper-sdk/5.8.203/environment-setup-cortexa53-crypto-remarkable-linux
export LOCPATH="$OECORE_NATIVE_SYSROOT/usr/lib/locale"
export LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8
cmake -S extensions/reink -B build/editor-arm-clean \
  -DCMAKE_BUILD_TYPE=Release \
  -DREPAPER_BUILD_XOVI_PACKAGE=ON \
  -DREPAPER_BUILD_EDITOR_PC=OFF \
  -DXOVI_SOURCE_DIR="$PWD/.tools/xovi-native-reference" \
  -DSCENE_ASSISTANT_SOURCE_DIR="$PWD/.tools/xovi-scene-assistant" \
  -DPython3_EXECUTABLE=/usr/bin/python3
cmake --build build/editor-arm-clean --parallel 4
```

Utilisez un **nouveau dossier vide** et figez les sources pendant toute la compilation. Une précédente compilation incrémentale mêlant deux dispositions de classe a produit un module rejeté ; la reconstruction propre et les contrôles de cohérence binaire ont corrigé cet incident. Un build compilé doit encore passer la validation QMD du firmware exact, les contrôles natifs et les essais sur appareil avant d’être considéré comme installable.

Pour AppLoad, `tools/build-appload-module.sh` attend trois arguments : un checkout AppLoad préparé avec les hooks de la PR 59, un **nouveau dossier de build absolu**, et le checkout XOVI. Il vérifie l’empreinte du QMD et applique les adaptations du dépôt avant compilation. Il attend aussi Scene Assistant dans `.tools/xovi-scene-assistant`. La préparation du runtime et des applications est distincte ; voir [les instructions du paquet](../packaging/appload/device/README.md).

Ni les exécutables propriétaires, ni le QML extrait de Xochitl, ni les polices de la tablette ne sont fournis. Les outils historiques de packaging peuvent dépendre d’attestations locales absentes du dépôt public ; cette publication n’annonce pas un paquet binaire reproductible et validé prêt à télécharger.

## Portail de connexion Moodle

Le portail est un composant séparé dans [`moodle-connect/`](../moodle-connect/README.md), avec Node 22.13+ et son verrou de dépendances npm :

```sh
cd moodle-connect
npm ci
npm test
npm run typecheck
```

Le code conserve l’origine du relais de développement historique. Pour votre propre installation, configurez l’origine HTTPS dans `moodle-connect/lib/protocol.ts`, votre hébergement et sa base D1, puis `REPAPER_MOODLE_PORTAL_URL` côté application. La publication du dépôt ne fournit pas un service de relais garanti. Les métadonnées du déploiement personnel ne sont pas distribuées.

[Installation expérimentale et contrôle du démarrage](INSTALLATION.md) · [Retour à RePaper](../README.md)
