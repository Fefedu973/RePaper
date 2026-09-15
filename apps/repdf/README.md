# rePDF

Petite visionneuse PDF AppLoad, destinée à garder la consigne visible pendant l’écriture dans le carnet actif.

Le bouton **PDF en fenêtre** apparaît dans la barre d’outils native développée. Il ouvre rePDF en fenêtre ; un nouvel appui retrouve cette fenêtre. Les commandes AppLoad permettent de la déplacer, de la redimensionner librement en largeur et en hauteur, de la réduire et de la fermer. Le bouton **Calculatrice** utilise le même fonctionnement pour reCalc, qui garde ses proportions de fenêtre.

**Ouvrir** affiche le sélecteur :

- **Bibliothèque** : titres et dossiers de la bibliothèque reMarkable, avec recherche dans les titres. Les documents supprimés et les carnets ne sont pas proposés. Un PDF absent du stockage local est signalé ; il faut d’abord le télécharger depuis la bibliothèque native.
- **Fichiers** : navigation dans les dossiers locaux, pour ouvrir un PDF qui n’appartient pas à la bibliothèque.

La lecture propose page précédente/suivante, numéro de page, zoom, ajustement à la largeur et rotation. On peut faire défiler la page au doigt ou au stylet. Les PDF sont ouverts en lecture seule. rePDF affiche le PDF source, y compris ses annotations PDF statiques ; les traits manuscrits reMarkable stockés séparément dans les fichiers `.rm` ne sont pas superposés. Les PDF protégés par mot de passe ne sont pas pris en charge.

Le PDF s’ajuste à toute la largeur disponible et suit le redimensionnement de la fenêtre. **Plein écran** masque les barres de rePDF pour consacrer tout l’intérieur de la fenêtre au document, en gardant le carnet visible à côté. Un appui sur le PDF affiche les commandes de zoom, de pages et de sortie ; elles se masquent après quatre secondes sans action. Faire défiler le PDF ne déclenche pas ces commandes.

## Compilation

Qt 6.2 ou plus récent : Core, Gui, Qml, Quick, QuickControls2, Concurrent et Network. Le seul en-tête PDFium nécessaire est `fpdfview.h`. Il est fourni par le SDK de la tablette. Ne pas ajouter tout le répertoire `usr/include` du SDK ARM au chemin d’inclusion d’une compilation PC ; utiliser un répertoire contenant seulement cet en-tête.

```sh
cmake -S apps/repdf -B /tmp/repdf-build -G Ninja \
  -DPDFIUM_INCLUDE_DIR=/path/to/pdfium/public
cmake --build /tmp/repdf-build
REPAPER_PDFIUM_LIBRARY=/path/to/libpdfium.so ctest --test-dir /tmp/repdf-build --output-on-failure
```

La compilation globale active rePDF si l’en-tête est trouvé. `-DREPAPER_BUILD_REPDF=ON` permet de l’exiger explicitement. La bibliothèque PDFium est chargée à l’exécution : `libpdfium.so` sur la tablette, ou le chemin défini par `REPAPER_PDFIUM_LIBRARY` sur PC. Le paquet cible Paper Pro / ferrari 3.28.0.169 et utilise le moteur PDF déjà fourni par ce système.

Dans AppLoad, le framebuffer reste à 1620 × 2160 avec une échelle Qt de 3. Le module transmet à rePDF les dimensions réelles et la rotation de la fenêtre par un socket local privé. La surface de l’interface suit ces dimensions et compense la transformation du framebuffer : le texte garde ses proportions, les commandes leur taille, et le PDF utilise toute la largeur. La poignée en bas à droite permet de modifier les deux dimensions indépendamment, dans les limites minimales AppLoad. En l’absence de cette connexion, l’affichage AppLoad habituel reste disponible.

```sh
REPAPER_PDFIUM_LIBRARY=/path/to/libpdfium.so /tmp/repdf-build/repdf \
  --library-root /path/to/test-library --files-root /path/to/documents
```

`--open /path/to/exercice.pdf` ouvre directement un document ; `--reading-mode` active le mode lecture après son chargement. `--size 540x770 --screenshot /tmp/repdf.png` capture l’interface réelle puis quitte ; sur un PC sans écran, exécuter sous Xvfb avec `QT_QPA_PLATFORM=xcb` et `QT_QUICK_BACKEND=software`.

Le rendu s’effectue dans un worker, avec accès PDFium sérialisé et rejet des résultats devenus obsolètes. Une image est limitée à 4096 pixels par dimension et 8 millions de pixels. Le sélecteur ne modifie ni les PDF ni les métadonnées de la bibliothèque.

## Intégration AppLoad

`tools/build-appload-module.sh` assemble les correctifs et les deux boutons. `tools/package-appload-device.py` inclut rePDF, son icône et son manifeste AppLoad en mode fenêtré. Le module reInk fonctionne avec les deux boutons mais n’est pas requis pour les afficher.

Cette extension est installée sur la Paper Pro cible. Le démarrage de rePDF a été corrigé pour Qt embarqué sans accessibilité et son interface a été rendue avec le binaire installé. Les outils de compilation et de validation locale ne se connectent pas à la tablette.
