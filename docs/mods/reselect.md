# reSelect

**Reprendre un schéma sans tout redessiner.**

reSelect ouvre directement la sélection de l’extension depuis la barre native. Touchez un trait ou encadrez des éléments, puis utilisez les poignées et **Propriétés** pour les modifier dans le carnet.

## Des commandes adaptées à l’objet

| Sélection | Commandes disponibles |
| --- | --- |
| Ligne ou flèche reconnue | Extrémités indépendantes ; épaisseur, couleur, style et sens des pointes. |
| Fil orthogonal reconnu | Extrémités, coude et paramètres du tracé. |
| Rectangle, ellipse ou symbole reconnu | Déplacement, dimensions dans les axes de la forme et rotation libre. |
| Objet configurable reStencil | Réouverture des paramètres du tableau ou du graphique. |
| Encre ordinaire ou sélection multiple | Transformations natives communes, duplication et suppression selon la sélection. |

Les poignées paramétriques apparaissent seulement si l’objet complet est reconnu. Un trait voisin ne devient pas automatiquement membre d’une forme.

## Modifier puis choisir ce que l’on garde

1. Touchez **reSelect**, puis un objet ou tracez un cadre autour de ses traits.
2. Déplacez une poignée, ou ouvrez **Propriétés** pour saisir une valeur.
3. Les modifications confirmées s’affichent dans la page au fur et à mesure.
4. Dans le popup, choisissez **Valider** ou **Fermer** pour conserver les changements ; **Annuler** revient sur les changements suivis depuis cette ouverture.

**Annuler dans Propriétés** et **Annuler dans la palette** ont des rôles différents : le premier annule la session de propriétés ; le second revient sur la dernière action du document. Une nouvelle ouverture de Propriétés établit un nouveau point de départ. Le texte saisi mais non confirmé est abandonné lors d’une annulation.

Pour garder les proportions au redimensionnement, maintenez le stylet immobile environ une seconde sur une poignée, puis glissez après l’indicateur de verrouillage. Une forme déjà tournée conserve ses propres axes. Le trait d’un objet paramétrique garde son épaisseur.

## Disponibilité et compatibilité

**Cette publication fournit les sources, sans `.so` prêt à installer.** Dans l’intégration tablette développée, reSelect est inclus avec [reInk](reink.md) et [reStencil](restencil.md) dans **`reink-editor.so`**, socle local **0.8.2**. Il n’existe pas de paquet reSelect séparé.

La cible documentée est **Paper Pro `ferrari`, OS 3.28.0.169, Qt 6.10.3**, avec XOVI et Qt Resource Rebuilder. Le [profil exact de Xochitl](../../extensions/reink/compatibility.json) est vérifié avant l’activation. Aucun autre modèle ou firmware n’est validé. Voir les conditions de reconstruction et d’utilisation expérimentale dans le [guide d’installation](../INSTALLATION.md).

## Ce qui a été vérifié

Les tests de l’hôte couvrent les poignées, la sélection, les transformations, la reconnaissance d’objets, les historiques simulés et les actions réelles du popup QML, y compris les réponses asynchrones. Le build de fluidité du 9 septembre passe les 32 suites de l’hôte et démarre sur la tablette cible. Ces éléments ne remplacent pas une campagne physique complète de sélection et d’annulation pour chaque reconstruction.

Une modification extérieure, un changement de page ou de couche invalide le point de départ de Propriétés : les changements déjà appliqués restent dans le document. Une session suit au plus **128 modifications confirmées** ; validez ou annulez pour repartir. Les commandes annulées restent rétablissables par l’historique natif, mais une branche Rétablir déjà remplacée par une nouvelle édition n’est pas recréée.

La conservation des modèles après réouverture est couverte par les tests de l’hôte. Leurs associations restent locales à la tablette ; leur transport par synchronisation ou copie de page n’est pas établi. Les commandes des anciens éditeurs autonomes PC servent au développement et ne constituent pas une autre version installable de reSelect.

## Documentation technique

- [Contrat de la session Propriétés](../../extensions/reink/NATIVE_PROPERTIES_SESSION.md)
- [Poignées paramétriques et proportions](../../extensions/reink/NATIVE_PARAMETRIC_EDITING.md)
- [Associations entre modèles et encre native](../../extensions/reink/NATIVE_OBJECT_BINDINGS.md)
- [Vérification des couleurs et de l’historique](../../extensions/reink/NATIVE_SELECTION_COLOR.md)
- [Refonte de fluidité : résultats et limites](../../extensions/reink/NATIVE_RESPONSIVENESS.md)

[Retour à RePaper](../../README.md) · [reInk](reink.md) · [reStencil](restencil.md)
