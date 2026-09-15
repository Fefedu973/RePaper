# reCalc

Calculatrice autonome C++17 / Qt Quick pour PC et reMarkable : la formule est un arbre éditable, rendu en deux dimensions pendant la saisie. Aucune expression n’est exécutée comme du JavaScript ou du code.

## Utilisation

- `a/b` transforme le nombre ou le bloc précédent en numérateur ; le curseur passe au dénominateur. `xⁿ` agit de la même façon pour la base et l’exposant.
- Touchez un chiffre ou une case, ou utilisez les flèches. Haut/bas relie les deux cases d’une fraction ou d’une puissance. `Sortir ↗` et `)` quittent la structure courante.
- `⌫` efface un chiffre. Dans une case vide, une seconde suppression retire le conteneur et conserve son autre contenu. `↶` / `↷` annulent et rétablissent les éditions.
- `=` calcule en tâche de fond. Les fractions rationnelles restent exactes ; les transcendantes portent toujours `≈`. Les décimaux entrés sont des rationnels exacts : `0,1 + 0,2 = 3/10`.
- `DEG` / `RAD` règle la trigonométrie. Le menu choisit 6, 12 ou 15 chiffres significatifs pour les approximations. `Fonctions 1/2` donne accès aux fonctions inverses, à la valeur absolue et à la racine cubique.
- `MS` mémorise le résultat affiché, `MR` insère la référence `M`, `MC` vide la mémoire. `Ans` désigne le dernier calcul réussi.
- Le menu copie texte/LaTeX et exporte un document `.tex`. L’historique conserve les 200 calculs les plus récents et restaure l’expression ainsi que son mode d’angle.
- Clavier PC : chiffres, virgule/point, `+ - * /`, `^`, parenthèses, flèches, Retour, Suppr, Entrée, Ctrl+Z, Ctrl+Maj+Z. Échap sort de la structure courante.

## Construction et émulateur PC

Dépendances : CMake 3.16+, compilateur C++17, Qt 6.2+ Core/Gui/Qml/Quick/QuickControls2/Concurrent/Sql et Boost headers (`multiprecision::cpp_int`). Le plugin Qt SQLite et les modules QML Quick/Controls/Layout doivent être installés à l’exécution. Qt Test est requis quand `BUILD_TESTING=ON`.

```sh
cmake -S apps/recalc -B build/recalc -DCMAKE_BUILD_TYPE=Release
cmake --build build/recalc --parallel
ctest --test-dir build/recalc --output-on-failure
QT_QPA_PLATFORM=xcb QT_QUICK_BACKEND=software build/recalc/recalc
```

Sous WSLg, `xcb` utilise le serveur d’affichage du PC ; aucune connexion à la tablette n’est nécessaire. Pour une capture reproductible dans un profil isolé :

```sh
XDG_DATA_HOME=/tmp/recalc-demo-data XDG_CONFIG_HOME=/tmp/recalc-demo-config \
QT_QPA_PLATFORM=xcb QT_QUICK_BACKEND=software \
build/recalc/recalc --demo --screenshot recalc.png
```

`--demo` remplace le brouillon du profil courant par `1/2 + √2`. Les paramètres de profil isolé ci-dessus évitent de modifier vos calculs. `--screenshot` ferme l’application après capture. L’application peut démarrer sur la plateforme Qt `offscreen`, mais la capture `grabWindow()` nécessite un écran WSLg/xcb sur Qt 6.2.

La compilation croisée utilise le SDK reMarkable :

```sh
. /opt/repaper-sdk/5.8.203/environment-setup-cortexa53-crypto-remarkable-linux
cmake -S apps/recalc -B build/recalc-arm -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build/recalc-arm --parallel
```

## Données et limites explicites

Les données résident sous `QStandardPaths::AppDataLocation`, organisation `RePaper`, application `recalc` : typiquement `~/.local/share/RePaper/recalc/`. L’historique SQLite possède un schéma versionné, et les arbres JSON portent `version: 1`. Un historique futur inconnu est refusé. `draft.json` utilise une écriture atomique. Les exports restent sous `exports/`. Réglages, Ans et mémoire utilisent QSettings. Aucun historique n’est envoyé sur le réseau.

Le moteur est volontairement borné : 512 nœuds, 24 niveaux, 128 caractères par nombre, entiers exacts de 4096 bits, exposants entiers de valeur absolue au plus 10 000. Une limite atteinte donne un message et préserve la formule. Division par zéro, racines négatives non réelles, logarithmes invalides, tangentes singulières et débordements numériques sont détectés. Les opérations rationnelles annulent les facteurs avant multiplication. Le calcul numérique utilise le type natif `long double`, avec affichage de 6 à 15 chiffres ; il ne s’agit pas d’un moteur décimal arbitraire.

Le rendu ne crée une texture que pour la zone visible, même lorsque la formule défile. Le curseur ne clignote pas. Les mouvements de curseur invalident les deux régions concernées ; le rafraîchissement physique e-ink dépend du backend Qt de l’appareil.

Export vers une page Paper Bridge, latence stylet physique et rafraîchissement e-ink restent à valider lors de l’intégration appareil. Cette application n’écrit jamais dans le store Xochitl. Le MVP ne comprend ni CAS, ni grapheur, ni nombres complexes.

## Vérification

`recalc_semantics` vérifie les rationnels, la précédence, l’édition structurée et ses inverses, les curseurs internes aux nombres, puissances/racines, modes d’angle, domaines, budgets, JSON invalide, Ans/mémoire, la persistance SQLite et 2500 commandes aléatoires avec invariant d’arbre/curseur.

`recalc_ui_semantics` envoie des événements de clic aux zones de fraction et aux nombres défilés, vérifie le curseur et le résultat édité, puis le calcul asynchrone, la mémoire et la reprise de l’historique. Le test ciblé `helpDialogHasNoBindingLoop` ouvre également l’aide à trois tailles d’écran et vérifie l’absence de boucle de dimensions QML.
