# reInk + reStencil — outils natifs 0.8.2

La refonte de fluidité du 9 septembre 2026 sépare l'aperçu des mises à jour des panneaux, allège le zoom natif et virtualise le catalogue de stencils. L'architecture, les mesures comparatives et leurs limites sont décrites dans [NATIVE_RESPONSIVENESS.md](NATIVE_RESPONSIVENESS.md).

Ajouts : ampoule, tableaux configurables, axes, quadrillage de Bode et réponses exponentielle/du second ordre, avec fond blanc optionnel. Voir [les réglages des pochoirs scientifiques](../../docs/stencils-scientifiques.md). Ce module de développement, distinct du paquet 0.8.2 distribué, a été installé sur demande explicite le 6 septembre 2026 ; son chargement a été vérifié.

Module pour **Paper Pro ferrari, Xochitl 3.28.0.169, Qt 6.10.3**, avec le binaire exact indiqué dans `compatibility.json`.

La version 0.8.2 attend que la page et ses coordonnées soient disponibles avant d’attacher les outils natifs. Elle réessaie lorsque la page devient prête. L’utilisateur a confirmé que **Carnet 2** s’ouvre après ce correctif.

Les fonctions de 0.8.1 sont conservées : **reSelect** active directement la sélection dans la barre latérale, et **Annuler** dans le popup **Propriétés** abandonne les modifications suivies depuis son ouverture. Le verrouillage des proportions par appui immobile d’une seconde reste disponible. Ces parcours d’édition n’ont pas été revérifiés dans leur ensemble sur la tablette avec 0.8.2.

## Mise à jour manuelle avec reManager

Dépendances : **XOVI** et **Qt Resource Rebuilder**, déjà présents sur la tablette de test.

1. Fermer le carnet, puis choisir **Stop UI** dans reManager.
2. Garder l’ancien module sur le PC, puis copier le **`reink-editor.so` de 0.8.2** vers `/home/root/xovi/extensions.d/reink-editor.so`. Conserver un seul exemplaire actif dans `extensions.d`.
3. Choisir **Start UI with Mods**. Le QMD et les palettes sont intégrés au `.so` ; aucun autre fichier de configuration n’est nécessaire.
4. Ouvrir un carnet d’essai : reInk, reSelect et reStencil sont dans la barre native.

La commande de préparation du paquet ne transfère rien sur la tablette et ne redémarre pas son interface.

## Sélection et popup Propriétés

**reSelect** active directement l’outil de sélection de l’extension. Touchez un trait ou encadrez les éléments à modifier. Les poignées et le bouton Propriétés restent ceux de l’extension.

Les modifications de Propriétés sont visibles au fur et à mesure. **Annuler** revient sur toutes celles qui ont été confirmées depuis cette ouverture, puis ferme le popup. Le texte encore saisi dans un champ est abandonné. **Valider** et **Fermer** terminent la saisie en cours, conservent les changements et ferment le popup. Une nouvelle ouverture établit un nouvel état de départ. Après Supprimer, le popup reste accessible pour permettre Annuler.

Les boutons d’historique des palettes de dessin restent Annuler/Rétablir pour la dernière action du document. L’annulation du popup utilise l’historique natif : ses changements annulés restent rétablissables. Comme après toute édition native, une branche Rétablir antérieure remplacée par une nouvelle modification n’est pas recréée.

L’annulation ne traverse que les commandes attribuées à cette fenêtre. Une modification extérieure ou un changement de page/couche invalide cet état de départ ; les changements déjà appliqués restent dans le document. Une fenêtre est limitée à 128 modifications confirmées : annuler ou valider permet ensuite de continuer.

## Verrouiller les proportions

1. Poser le stylet au point de départ d’un rectangle, d’une ellipse ou d’un symbole, ou sur une poignée de redimensionnement.
2. Le maintenir immobile environ **1 seconde**, jusqu’à l’indicateur **Proportions verrouillées**.
3. Glisser sans lever le stylet, puis relâcher pour terminer.

À la création, Rectangle donne un carré, Ellipse un cercle et les symboles gardent leurs proportions du catalogue. Au redimensionnement, le geste conserve les proportions actuelles de l’objet, y compris après rotation ou étirement préalable. Une poignée latérale agrandit l’autre dimension symétriquement autour du milieu du bord opposé. Les objets paramétriques gardent leur épaisseur de trait ; une sélection d’encre ordinaire conserve le comportement d’échelle natif.

Les petits tremblements sont tolérés dans un rayon de 8 pixels de vue. Si le stylet sort de ce rayon avant l’activation, le geste reste libre, même en revenant au point de départ. Le verrouillage vaut pour un seul geste ; relâcher ou changer d’outil/de page l’annule. Les extrémités, coudes, déplacements, rotations et traits libres ne déclenchent pas ce verrouillage.

## Dessin et sélection

- **Stylet** : dessiner, encadrer un objet avec Sélection, déplacer les poignées. Le doigt ne dessine plus, même lorsque le stylet survole l’écran.
- **Doigts** : boutons et navigation native, dont zoom, défilement et changement de page. Le relais tactile de 0.6.0 a été retiré ; la cible du gestionnaire natif reste intacte.
- **Fin de création** : le travail natif confirme les nouveaux traits, puis vide leur sélection avant de céder l’affichage aux tuiles. L’aperçu attend les tuiles et une image Qt ; le succès de l’insertion n’est pas déduit de la disparition de l’aperçu.
- **Lignes et flèches** : déplacer les ronds des deux extrémités. L’autre extrémité, l’épaisseur et la taille des pointes restent fixes. La direction de la flèche, la couleur et le style sont disponibles dans Propriétés.
- **Fils orthogonaux** : déplacer les extrémités ou le losange du coude. Les angles droits suivent l’orientation du fil. L’aimantation fonctionne à la création et pendant le déplacement d’une extrémité.
- **Rectangles, ellipses et symboles** : le cadre et ses poignées conservent les axes de la forme après rotation. Les dimensions se modifient dans ces axes sans épaissir les traits. Le rond au-dessus permet une rotation libre.

Les modifications paramétriques sont regroupées en une commande native Annuler. Les paramètres sont associés aux traits réellement observés ; les versions précédentes restent disponibles pour Annuler/Rétablir et la réouverture. Dupliquer crée une association indépendante.

Les objets associés par 0.6.0 peuvent retrouver ces poignées si leur rendu complet correspond exactement à un modèle connu. Les objets plus anciens sans association restent de l’encre native : encadrer tous leurs traits ou les recréer. Une sélection de plusieurs objets conserve les transformations natives communes. Les contrôles paramétriques concernent un seul objet reconnu.

L’association reste locale à la tablette. Les fichiers de paramètres passent au schéma 3 ; 0.7 lit également le schéma 2 de 0.6. Un retour à 0.6 ne lira pas les nouveaux fichiers de paramètres, mais les traits restent dans le document natif. La copie/synchronisation de pages et le suivi automatique des fils lors du déplacement d’un composant restent à implémenter ou à valider séparément.

La création propose des épaisseurs jusqu’à 4 ; les propriétés des objets permettent aussi de modifier une épaisseur existante jusqu’à 100. Chaque dessin ou sélection est limité à 128 traits et 200 000 points. Les couleurs sont opaques.

## Vérification sur tablette

Essayer d’abord **reSelect**, puis ouvrir Propriétés, modifier successivement largeur, couleur et rotation, et choisir **Annuler** : l’état initial doit revenir et le popup se fermer. Répéter avec une saisie non validée au clavier, avec Supprimer, puis avec Valider/Fermer et une nouvelle ouverture. Les modifications d’une ouverture validée ne doivent pas être annulées par la suivante.

Essayer également l’appui prolongé : créer un carré, un cercle et une résistance avec puis sans attente ; redimensionner une forme tournée depuis un coin puis une poignée latérale. Vérifier le maintien des proportions au relâchement, Annuler/Rétablir, réouverture et retour au geste libre après une levée du stylet.

Commencer par cette séquence : créer un trait noir, le sélectionner, choisir Rouge puis Bleu, modifier deux fois une extrémité, changer d’outil, puis créer un nouveau trait coloré. Refaire cette séquence avec les extrémités et le coude d’un fil orthogonal. Vérifier que chaque modification reste visible après désélection et réouverture. Tester plusieurs Annuler/Rétablir et recommencer un dessin ensuite. Les traits dont l’insertion avait été refusée en 0.7.0 ne peuvent pas être récupérés par cette mise à jour.

1. Au stylet, tracer plusieurs lignes, flèches et résistances sur une page blanche. Vérifier qu’elles restent visibles au relâchement, au changement d’outil et après réouverture.
2. Avec Sélection, déplacer l’extrémité d’une flèche diagonale ; vérifier la pointe, l’autre extrémité et l’épaisseur.
3. Tourner un rectangle et une résistance d’un angle libre, puis modifier leurs dimensions avec les poignées et les champs Largeur/Hauteur.
4. Déplacer les extrémités et le coude d’un fil ; tester l’aimantation aux ports, aux bouts et aux segments d’autres fils, y compris en tirets.
5. Vérifier Annuler/Rétablir, couleur, style, Dupliquer, suppression, réouverture et changement d’outil pendant une modification.
6. Avec un outil reInk/reStencil actif, utiliser les doigts pour naviguer et cliquer sur les boutons, puis dessiner immédiatement au stylet. Aucun geste des doigts ne doit ajouter de trait.

Les campagnes PC précédentes couvrent la géométrie, l’hôte QML avec des services natifs simulés, les reçus de travaux, les changements de contexte, la sélection, les paramètres, l’aimantation et l’historique. Pour ce correctif, la confirmation sur tablette porte sur l’ouverture de Carnet 2 ; elle ne valide pas tous les parcours d’édition, leur latence ou leur sauvegarde durable.

## Reconstruction

PC, Qt 6.2 minimum :

```sh
cmake -S extensions/reink -B build-editor -DREPAPER_BUILD_XOVI_PACKAGE=OFF
cmake --build build-editor --parallel 4
ctest --test-dir build-editor --output-on-failure
```

ARM64, après activation du SDK reMarkable 5.8.203 :

```sh
cmake -S extensions/reink -B build-arm \
  -DREPAPER_BUILD_XOVI_PACKAGE=ON -DREPAPER_BUILD_EDITOR_PC=OFF \
  -DXOVI_SOURCE_DIR="$PWD/.tools/xovi-native-reference" \
  -DSCENE_ASSISTANT_SOURCE_DIR="$PWD/.tools/xovi-scene-assistant" \
  -DPython3_EXECUTABLE=/usr/bin/python3
cmake --build build-arm --parallel 4
python3 extensions/reink/package.py --build-dir build-arm \
  --xovi-source .tools/xovi-native-reference \
  --qmd-validation .local/qa/native-qmd-v081/validation.json \
  --output reink-native-0.8.2.tar.gz
```

L’archive fournit les sources dans `src/` et les bundles Git publics dans `third-party/`. Pour reconstruire hors ligne, cloner ces bundles dans `src/.tools/xovi-native-reference` et `src/.tools/xovi-scene-assistant`, puis exécuter les commandes ARM depuis `src/`. L’attestation QMD fournie est dans `validation/qmd-offline.json`. Elle reste celle de 0.8.1 : le QMD est inchangé. Le module contient les ressources recompilées, dont le nouvel hôte de page.

Aucun document vivant n’est remplacé directement, et aucun QML propriétaire, hashtable ou binaire Xochitl n’est redistribué. Voir `NATIVE_PROPERTIES_SESSION.md`, `NATIVE_PARAMETRIC_EDITING.md`, `NATIVE_OBJECT_BINDINGS.md`, `NATIVE_PREVIEW.md`, `NATIVE_AFFINE_RECEIPT.md`, `NATIVE_SELECTION_COLOR.md` et `PROVENANCE.md` pour les contrats et limites.
