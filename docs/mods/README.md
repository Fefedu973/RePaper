# Les mods RePaper

Des outils pour dessiner, calculer, consulter vos cours et préparer vos notes sur reMarkable.

**Cette première publication contient les sources, sans modules ni applications binaires prêts à installer.** Le [guide d'installation et de compilation](../INSTALLATION.md) précise comment explorer le projet et ses conditions de compatibilité. La cible native documentée est la **Paper Pro « ferrari », OS 3.28.0.169** ; aucun support d'un autre modèle ou firmware n'est établi.

| Mod | À quoi il sert | Où il s'utilise |
| --- | --- | --- |
| [**reInk**](reink.md) | Dessiner lignes, flèches, formes et fils orthogonaux ; choisir couleurs et styles. | Dans la page du carnet natif. |
| [**reSelect**](reselect.md) | Sélectionner, déplacer, redimensionner et modifier les propriétés des objets reconnus. | Dans la page du carnet natif. |
| [**reStencil**](restencil.md) | Poser des symboles électroniques, tableaux et graphiques configurables. | Dans la page du carnet natif. |
| [**reCalc**](recalc.md) | Éditer des formules en deux dimensions, calculer et retrouver l'historique. | Application Qt ; fenêtre AppLoad sur tablette. |
| [**rePDF**](repdf.md) | Garder une consigne PDF visible à côté de ses notes. | Fenêtre AppLoad, lecture seule. |
| [**reAgenda**](reagenda.md) | Consulter un calendrier CPE ou ICS et ouvrir les notes d'une journée ou d'un événement. | Application Qt ; carnets natifs via Paper Bridge. |
| [**reMoodle**](remoodle.md) | Parcourir les cours Moodle et importer PDF, images ou présentations converties. | Application Qt ; bibliothèque native via Paper Bridge. |
| [**AppLoad et Paper Bridge**](appload.md) | Lancer les applications, gérer leurs fenêtres et relier imports et carnets à la bibliothèque. | Infrastructure locale de la tablette et banc PC. |

## Intégration native et essais sur PC

**reInk, reSelect et reStencil** sont trois entrées d'un même module natif, `reink-editor.so`. Elles travaillent dans l'éditeur reMarkable Xochitl. AppLoad et les applications constituent l'autre partie de la suite ; Paper Bridge prend en charge leurs demandes de documents.

Le dépôt contient aussi des **bancs PC** : applications Qt, éditeurs de test et émulateur AppLoad. Ils permettent de développer et de vérifier l'interface sans compte ni tablette pour les fonctions locales. Ils n'exécutent pas le firmware reMarkable et ne mesurent pas le rafraîchissement physique de l'écran e-paper. Les anciens programmes `apps/reink` et `apps/restencil` servent à ces essais ; ils ne remplacent pas le module natif actuel.

Chaque fiche distingue les fonctions, prérequis, limites et vérifications connues. Les anciens numéros de paquets cités dans les guides techniques sont des repères de développement local ; ils ne désignent pas des téléchargements joints à cette publication.

**RMChat est archivé** et exclu de la suite active. Son [état d'archive](../../apps/rmchat/ARCHIVED.md) reste disponible pour comprendre le code conservé.

[Accueil RePaper](../../README.md) · [Installation et compilation](../INSTALLATION.md) · [Contribuer](../../CONTRIBUTING.md)
