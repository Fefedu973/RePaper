# Interfaces RePaper — septembre 2026

Les applications reprennent les mesures relevées sur l’interface de la Paper Pro sous 3.28 : fond blanc, texte noir, titres en reMarkable Serif Small, texte et commandes en reMarkable Sans, lignes de séparation fines. Les composants du dépôt sont une implémentation originale ; les polices propriétaires sont chargées depuis la tablette de leur propriétaire et ne sont pas distribuées dans les paquets.

| Élément | Pixels de l’écran Paper Pro | Valeur QML avec DPR 2 |
| --- | ---: | ---: |
| Texte courant | 28 | 14 |
| Légende | 22 | 11 |
| Titre | 48 | 24 |
| Zone de commande | 96 | 48 |
| Icône | 48 | 24 |
| Marge principale | 80 | 40 |

`shared/ui/qml/Theme.qml` centralise ces mesures. Les panneaux natifs et les surfaces redimensionnables utilisent les pixels de leur fenêtre hôte ; ils adaptent l’espace disponible en conservant des commandes accessibles. Les formulaires et menus suivent les mêmes composants. Les messages d’état ordinaires et les indications de cache ont disparu des écrans principaux ; les erreurs restent visibles.

La [planche de recherche](mockups/suite-native-concept-v1.png) a été générée avec ImageGen, en mode intégré, à partir du [prompt conservé](mockups/prompt-v1.txt). Les captures dans `mockups/current/` proviennent ensuite du véritable QML et du moteur de dessin. Les cours et calendriers présentés sont fictifs.

## Prise de notes

La palette de la page donne directement accès au noir, au rouge et au vert du stylo natif. Elle suit le dernier stylo d’écriture utilisé. La palette complète reste accessible dans les contrôles habituels.

La gomme physique du stylet conserve l’effacement natif lorsque les outils RePaper sont actifs. Retourner le stylet annule l’aperçu en cours ; reprendre la pointe retrouve l’outil choisi. Si le type de contact ne peut pas être déterminé, le contact reste confié au système natif.

Après avoir choisi un stencil, une barre sur la page propose les quatre derniers symboles, l’orientation et, pour les dipôles, la flèche de tension. Un toucher place le symbole ; un tracé définit sa taille. L’orientation et les options de tension restent sélectionnées pour les poses suivantes et sont conservées sur la tablette. Le bouton Stylo revient au dernier stylo d’écriture.

La pose au toucher utilise une ancre adaptée : centre des dipôles, sortie de l’amplificateur, base ou grille du transistor, borne de la masse et du connecteur, début du crochet de la racine. Les tableaux et graphiques partent de leur coin supérieur gauche. À moins de huit pixels d’une cible compatible, une borne prend priorité sur cette ancre ; le repère d’accroche apparaît dès le premier appui. La borne choisie reste fixe pendant un tracé de dimensionnement. Les flèches de tension ne décalent pas l’ancre. La [planche de placement](mockups/current/pose-stencils.png) illustre ces règles.

Une pose sur l’extrémité libre d’un fil crée aussi sa connexion au composant : le fil suit les déplacements du composant. Sur le milieu d’un fil ou sur la borne d’un autre composant, l’accroche sert au placement géométrique ; elle ne crée pas automatiquement de fil supplémentaire.

La flèche et son composant forment un seul objet. Le sens et le côté de la flèche se changent dans la barre ou dans les propriétés du composant sélectionné. La résistance, le condensateur et les sources conservent leurs bornes ; les fils attachés suivent leurs déplacements, rotations et redimensionnements.

Des repères pointillés apparaissent pendant le placement ou le déplacement près d’un autre objet. Ils servent à aligner les centres, les bords et les bornes avec une tolérance de huit pixels à l’écran. Ils ne sont jamais enregistrés dans le carnet ni exportés.

La racine carrée étirable garde la forme de son crochet lorsque sa barre s’allonge. Le générateur de tension simple est un cercle traversé par un trait continu, sans annotation de polarité ; la source avec « + » et « − » reste disponible.

Dans **Graphe → Courbe → Sinusoïde**, l’amplitude, le décalage vertical, la fréquence en hertz et la phase en degrés règlent l’onde. Les bornes verticales s’adaptent lors de la sélection initiale et restent modifiables. La durée et la fréquence sont limitées ensemble à 32 périodes. Les courbes exponentielles et du second ordre restent disponibles.

## Applications

- reMoodle présente les cours et documents dans des listes simples. PPT et PPTX sont convertis localement en PDF avant leur import. Les détails et limites de fidélité sont décrits dans [son guide](../../apps/remoodle/README.md).
- reAgenda propose les vues jour, semaine et mois et les notes liées aux événements. Les nouveaux en-têtes utilisent le modèle P Day existant et des traits natifs positionnés, sans créer de modèle personnalisé par événement. Les anciens modèles déjà utilisés ne sont pas supprimés.
- reCalc conserve les commandes dans la fenêtre disponible. La poignée de redimensionnement d’AppLoad se trouve hors de la zone des touches, notamment de « = ». Les options, l’historique et l’aide utilisent la même surface redimensionnable.
- rePDF adapte les commandes et le choix des fichiers à la taille disponible. Le mode lecture privilégie le document et fait apparaître ses commandes au toucher.

Le rafraîchissement des applications distingue les changements rapides pendant le déplacement et la restitution nette à la fin. Les vérifications de rendu logiciel et de coordonnées ne constituent pas une mesure optique du panneau e-paper.
