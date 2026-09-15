# AppLoad sur PC

Ce harnais compile l’émulateur PC d’[AppLoad](https://github.com/asivery/rm-appload/tree/1a708d297df4b3c108e78a02ec7e2634ff33033c), puis charge les vrais exécutables Qt via ses manifestes externes et son framebuffer QTFB. Il fonctionne dans Linux/WSL avec un affichage X11 (WSLg sur Windows). Aucun SSH ni changement sur la tablette n’est effectué.

```sh
python3 tools/emulator-apps.py --state-dir /root/repaper-appload --sandbox-dir /root/repaper-emulator setup --app-build /root/repaper-build
python3 tools/emulator-apps.py --state-dir /root/repaper-appload launch
python3 tools/emulator-apps.py --state-dir /root/repaper-appload launch --app reagenda
```

Le menu permet d’ouvrir reMoodle, reAgenda, reStencil, reCalc et reInk. `--app` accepte leur nom en minuscules. Les applications doivent être compilées auparavant, par exemple avec `tools/build-pc.sh`. Les dépendances incluent Qt 6 Core/Gui/Quick/QML/QuickControls2/SVG, les outils qmake6/CMake, g++, make et Python 3. Le lanceur Windows à la racine prend en charge la préparation.

La référence upstream est clonée dans `.tools/rm-appload` si elle manque, puis son commit exact est contrôlé. Un checkout divergent ou modifié est refusé. Les adaptations ne touchent qu’une copie générée dans `STATE/source` : compatibilité Qt 6.2, fichiers de périphériques privés, souris traduite en toucher et taille de la fenêtre. Les sources externes et leur licence GPL-3.0 sont conservées avec le build. La [PR 59](https://github.com/asivery/rm-appload/pull/59), commit `40506d47427123f07030bb2e83453a43d035b16a`, modifie les hooks Xochitl 3.28 ; ces hooks ne sont pas utilisés sur PC.

`run-native.py` charge le shim QTFB fourni par AppLoad. Le backend Qt linuxfb écrit le framebuffer partagé ; `qt-linuxfb-refresh.cpp` transmet une mise à jour quand son contenu change. Les entrées evdev sont des fichiers privés, accessibles par les alias existants `/dev/fd`; aucun périphérique hôte n’est créé. La souris agit comme un doigt. La pression physique du stylet, le rafraîchissement e-ink et l’intégration réelle Xochitl restent à vérifier sur appareil.

Le journal est `STATE/emulator.log`, les manifestes sont dans `STATE/runtime/applications_root`, et les détails du build dans `STATE/build-info.json`. Paper Bridge est lancé en sandbox s’il n’est pas déjà disponible ; les apps reçoivent `XDG_DATA_HOME=SANDBOX/state/inputs` et `PAPER_BRIDGE_SOCKET=SANDBOX/core.sock`. Un bridge déjà actif est réutilisé. Fermer le lanceur arrête seulement ses propres processus. Un verrou empêche deux lanceurs d’utiliser le même profil. Chaque profil possède son socket QTFB dans `STATE/runtime/qtfb.sock` et son espace de mémoire partagée.

L’échelle native vaut 1,5 par défaut ; `launch --ui-scale 1.75` l’agrandit. Elle est limitée à l’intervalle 1–2 et ne change pas la taille de la fenêtre AppLoad. Un ancien profil doit être reconstruit avec `setup` pour obtenir les sockets isolés et cette mise à l’échelle.

Vérification effectuée : affichage de reAgenda dans le framebuffer AppLoad et clic traversant fenêtre hôte → QTFB → evdev → dialogue « Sources » de reAgenda. Les options `launch --app reagenda --screenshot chemin.png --click X,Y` permettent de contrôler une cible aux coordonnées de la fenêtre hôte et ferment cette instance après capture. Les coordonnées dépendent de l’échelle choisie. Fermez votre instance interactive avant ce test. Cette validation PC ne prouve pas la compatibilité des hooks tablette 3.28.
