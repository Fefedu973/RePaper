# Boutons Calculatrice et PDF de la barre native

Un appui ouvre reCalc en fenêtre au-dessus du carnet, directement depuis la barre d’outils développée. Un nouvel appui retrouve la calculatrice déjà ouverte et la réaffiche en fenêtre si elle était réduite ou en plein écran. Le stylo ou l’outil de dessin sélectionné reste le même.

Le bouton appartient à AppLoad et utilise son API `AppLoadLauncher.launchApplication("external::recalc", [], {}, true)`. Il fonctionne également sans reInk. Si reInk exécute une commande native, le bouton attend sa fin avant de redevenir disponible.

Pour assembler le module, appliquer `calculator-toolbar.patch` et `calculator-window.patch` après les six correctifs de `packaging/appload/device` utilisés par AppLoad 3.28.4. Copier `calculator.svg` vers `resources/icons/calculator.svg`. Ajouter `calculator-toolbar.qmd` après les QMD AppLoad et `native-documents.qmd`, puis reconstruire le RCC et `appload.so` avec le SDK du projet. Le binaire reCalc et le module reInk restent compatibles avec cette modification.

Le bouton **PDF en fenêtre** ouvre de la même manière `external::repdf`, avec un sélecteur de PDF, navigation dans les pages, zoom et rotation. Voir [rePDF](../../../../apps/repdf/README.md). Appliquer aussi `pdf-toolbar.patch` après `calculator-toolbar.patch`, copier `pdf-reader.svg` dans `resources/icons`, puis ajouter `pdf-toolbar.qmd` à la suite des autres QMD. `tools/build-appload-module.sh` réalise cet assemblage complet.

Les deux applications conservent une seule fenêtre par bouton. Un nouvel appui réaffiche la fenêtre concernée au premier plan, y compris si elle était réduite ou maximisée.

rePDF autorise une largeur et une hauteur indépendantes. `pdf-responsive-window.patch` lui transmet la géométrie par `NativeViewportHost` et active l’étirement du framebuffer uniquement quand son client est connecté. rePDF compense cet étirement et la rotation pour remplir la fenêtre sans déformation. Sa poignée calcule la hauteur du contenu sans recompter la barre de titre. reCalc conserve le redimensionnement proportionnel AppLoad.

Les boutons et rePDF sont installés sur la Paper Pro cible à la demande de l’utilisateur. Le code de préparation et les tests locaux ne se connectent pas à la tablette.
