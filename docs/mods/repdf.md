# rePDF

**Gardez une consigne PDF visible pendant que vous écrivez dans votre carnet.**

rePDF ouvre un document dans une fenêtre AppLoad déplaçable et redimensionnable. Placez la consigne à côté de vos notes, changez de page ou zoomez sans quitter le carnet actif.

<img src="../design/mockups/current/repdf-fenetre.png" alt="rePDF affichant un exercice fictif sur un circuit RC" width="420">

*Capture de l'application avec un document de démonstration.*

## Ce que vous pouvez faire

- Ouvrir un PDF de la bibliothèque reMarkable en parcourant ses dossiers et ses titres.
- Rechercher un titre dans la bibliothèque ou choisir un PDF dans les fichiers locaux.
- Passer d'une page à l'autre ou saisir un numéro de page.
- Zoomer, ajuster la page à la largeur disponible et tourner l'affichage.
- Faire défiler la page au doigt ou au stylet.
- Masquer les barres avec **Plein écran**, puis toucher le PDF pour faire apparaître les commandes temporaires.

La fenêtre se redimensionne librement en largeur et en hauteur. Le PDF suit l'espace disponible ; le mode Plein écran utilise l'intérieur de cette fenêtre et permet de garder le carnet visible à côté.

## Lire à côté de vos notes

1. Dans un carnet, développez la barre d'outils native et touchez **PDF en fenêtre**.
2. Choisissez **Ouvrir**, puis **Bibliothèque** ou **Fichiers**, et sélectionnez un PDF.
3. Déplacez et redimensionnez la fenêtre avec les commandes AppLoad. Un nouvel appui sur **PDF en fenêtre** retrouve la fenêtre déjà ouverte.
4. Ajustez la largeur, changez de page ou activez **Plein écran** pour privilégier le document. Dans ce mode, les commandes se masquent après quatre secondes sans action.

## Prérequis et limites

**Cette première publication fournit les sources, sans paquet binaire prêt à installer.** Consultez le [guide d'installation et de compilation](../INSTALLATION.md).

rePDF utilise **AppLoad** et le moteur **PDFium** présent sur la tablette. La cible documentée est la **Paper Pro « ferrari » sous OS 3.28.0.169**. reInk n'est pas nécessaire pour afficher le bouton PDF.

Le PDF doit être présent localement. Si la bibliothèque indique qu'il est absent, téléchargez-le d'abord dans l'interface native reMarkable. Les carnets et les documents supprimés ne sont pas proposés par le sélecteur.

La lecture est **en lecture seule**. Les annotations intégrées au PDF source sont visibles ; les traits manuscrits reMarkable conservés séparément dans les fichiers `.rm` ne sont pas superposés. Les PDF protégés par mot de passe ne sont pas pris en charge.

Le rendu est borné à 4 096 pixels par dimension et 8 millions de pixels par image. Une compilation PC nécessite Qt et une bibliothèque PDFium compatible ; le guide technique détaille leur configuration.

## Données et vérifications

Le sélecteur ne modifie ni les PDF ni les métadonnées de la bibliothèque. Le rendu s'exécute en arrière-plan et ignore les résultats devenus obsolètes après une autre navigation.

Les suites `repdf_rendering`, `repdf_file_selector` et `repdf_reader_ui` couvrent le rendu, le choix de fichier et les interactions de lecture. Le guide technique documente aussi le démarrage et le rendu de l'interface avec le binaire installé sur la tablette ; cela ne constitue pas une validation de tous les PDF possibles.

[Guide technique et compilation](../../apps/repdf/README.md) · [Retour à RePaper](../../README.md)
