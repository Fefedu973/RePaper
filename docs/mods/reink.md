# reInk

**Des outils de dessin précis directement dans votre carnet reMarkable.**

reInk ajoute lignes, flèches, formes et fils orthogonaux à la barre de l’éditeur natif. Le dessin devient de l’encre vectorielle dans la page active : vous pouvez continuer à écrire autour avec les outils habituels, puis reprendre les formes avec [reSelect](reselect.md).

## Ce que vous pouvez faire

| Outil | Usage |
| --- | --- |
| Trait libre | Dessiner au stylet avec la couleur et l’épaisseur choisies. |
| Ligne et flèche | Tracer un segment ; reprendre ses extrémités et choisir le sens des pointes. |
| Rectangle et ellipse | Créer une forme, la tourner et modifier ses dimensions. |
| Fil orthogonal | Relier des éléments avec des angles droits et une accroche aux bornes et aux fils reconnus. |
| Style | Choisir un trait continu, tireté ou pointillé et une couleur opaque. |
| Historique | Annuler et rétablir les modifications par l’historique du document. |

Les évolutions de septembre ajoutent le suivi des fils attachés pendant les transformations des composants reconnus, ainsi que des repères d’alignement temporaires. Les détails sont dans le [guide des gestes](../design/README.md). Ces évolutions appartiennent au code courant ; leur présence dans un binaire doit être indiquée par la livraison correspondante.

## Premier dessin

1. Ouvrez un carnet et touchez **reInk** dans la barre native.
2. Choisissez une ligne, une flèche ou une forme, puis sa couleur et son épaisseur.
3. Posez le stylet, tracez et relevez-le pour terminer.
4. Touchez **reSelect** pour reprendre une extrémité, déplacer la forme ou ouvrir **Propriétés**.

Pour créer un carré ou un cercle, gardez le stylet immobile environ une seconde au début du geste. Attendez **Proportions verrouillées**, puis tracez sans lever le stylet. Ce verrouillage dure le temps de ce geste.

Le stylet dessine ; les doigts commandent les boutons et la navigation native, notamment le zoom et le défilement. Le code courant conserve aussi l’effacement natif avec la gomme physique du stylet.

## Disponibilité et compatibilité

**Cette publication fournit les sources, sans module `.so` ni paquet binaire prêt à installer.** Les conditions de reconstruction et d’utilisation expérimentale sont expliquées dans le [guide d’installation](../INSTALLATION.md).

Dans l’intégration tablette développée, reInk, reSelect et reStencil sont trois entrées d’un même module : **`reink-editor.so`**. Il n’est pas nécessaire d’installer trois extensions.

| Élément | Cible documentée |
| --- | --- |
| Tablette | reMarkable Paper Pro, modèle `ferrari` |
| Système | `3.28.0.169` |
| Qt | `6.10.3` |
| Dépendances | XOVI et Qt Resource Rebuilder |
| Dernier numéro de paquet local documenté | `0.8.2` — repère historique, binaire non joint |

Le module vérifie également l’empreinte exacte de Xochitl dans le [profil de compatibilité](../../extensions/reink/compatibility.json). Aucun support d’un autre modèle ou firmware n’est établi.

Le numéro `0.8.2` désigne aussi le socle des évolutions de septembre. L’archive locale initiale 0.8.2 et la reconstruction de fluidité du 9 septembre sont des builds différents ; le numéro seul ne permet pas d’identifier leur contenu. Aucun de ces binaires n’est distribué dans cette publication.

## État vérifié et limites

La reconstruction de fluidité du 9 septembre a passé les **32 suites CTest sur l’hôte**, une compilation ARM propre et le contrôle du démarrage sur la tablette cible. L’essai utilisateur a confirmé une amélioration nette du pincement pour zoomer et de l’ouverture de Stencil. Cette observation n’est pas une mesure de FPS ou de latence et ne valide pas chaque geste d’édition sur tablette.

Les objets paramétriques conservent leur épaisseur lors d’un redimensionnement. Une sélection d’encre ordinaire garde le comportement d’échelle natif. Les anciennes formes sans association reconnue restent de l’encre : le module ne leur invente pas une structure. Les associations sont locales ; leur transport par copie de page ou synchronisation n’est pas garanti. Une opération est bornée à 128 traits et 200 000 points.

Le programme autonome dans `apps/reink` est un **banc PC** avec sa propre page de test et ses exports. Il ne correspond pas au module intégré à Xochitl et ses captures ne mesurent pas l’affichage e-paper.

## Documentation technique

- [Module natif : utilisation et reconstruction](../../extensions/reink/README.md)
- [Refonte de fluidité et portée des mesures](../../extensions/reink/NATIVE_RESPONSIVENESS.md)
- [Aperçu et transfert au rendu natif](../../extensions/reink/NATIVE_PREVIEW.md)
- [Édition paramétrique](../../extensions/reink/NATIVE_PARAMETRIC_EDITING.md) — contrat du socle 0.8.0 ; les connexions ont évolué depuis.
- [Origine du code et dépendances](../../extensions/reink/PROVENANCE.md)

[Retour à RePaper](../../README.md) · [reSelect](reselect.md) · [reStencil](restencil.md)
