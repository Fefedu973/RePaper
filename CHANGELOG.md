# Historique

Les entrées distinguent la publication du dépôt des builds locaux essayés sur tablette. Les étiquettes **reInk 0.8.2** et **AppLoad rePaper 3.28.4** ont servi de socles à plusieurs évolutions ; elles ne sont pas des identifiants uniques de leurs binaires. Le commit source et l’empreinte de chaque artefact restent nécessaires pour identifier un build.

## 16 septembre 2026 — publication initiale des sources

- Publication de RePaper sur GitHub, avec présentation de reMoodle, reAgenda, reCalc, rePDF, reInk, reSelect, reStencil, AppLoad et Paper Bridge.
- Guides publics d’utilisation, de compilation, de compatibilité et d’installation expérimentale.
- Code original sans licence préexistante placé sous MIT ; licences et attributions des composants tiers conservées séparément.
- Publication de sources uniquement : aucun module `.so`, runtime ARM, paquet d’applications, SDK, ressource propriétaire ou donnée personnelle joint.
- RMChat reste archivé et exclu des builds et paquets habituels.

Cette publication ne constitue pas une nouvelle campagne de validation sur tablette et ne transforme pas les anciens numéros de paquets locaux en nouveaux numéros de versions binaires.

## 9 septembre 2026 — fluidité des outils natifs

- Séparation des mises à jour d’aperçu et des panneaux pour réduire le travail dans le thread d’interface.
- Réutilisation des observations natives inchangées ; préparation des associations d’objets en arrière-plan.
- Catalogue reStencil construit une seule fois, liste virtualisée et absence de repeints des miniatures masquées.
- Aperçu limité aux limites de l’encre visible, sans surface peinte au repos ; suppression de copies inutiles pendant un long trait libre.
- Routage du stylet allégé en conservant la classification de la gomme et les gestes tactiles natifs.
- Validation sur l’hôte : 32 suites CTest réussies sur une reconstruction propre. Les mesures CPU et d’allocation restent des mesures PC, sans conversion en promesses de latence ou de FPS sur tablette.

Le premier artefact ARM incrémental a provoqué une boucle au démarrage : des fichiers objets avaient été compilés avec deux dispositions différentes de `NativeScene`. Il a été rejeté et le code précédent rétabli. Le remplacement, compilé depuis des sources figées dans un dossier neuf, a passé les 15 contrôles de cohérence binaire et le contrôle de démarrage stable pendant 75 secondes. Le build corrigé installé est identifié dans le [compte rendu technique](extensions/reink/NATIVE_RESPONSIVENESS.md).

L’essai utilisateur sur tablette a confirmé une amélioration nette du pincement pour zoomer dans un document et de l’ouverture du panneau Stencil. Ce retour physique ne vaut pas mesure instrumentée ni validation complète de tous les parcours d’édition.

## Septembre 2026 — évolutions fonctionnelles locales

- Outils natifs : poignées de formes et d’extrémités, propriétés, couleurs, verrouillage des proportions par appui immobile, accès direct reSelect et annulation des changements suivis depuis l’ouverture de Propriétés.
- reStencil : pochoirs scientifiques configurables, tableaux, graphes temporels et de Bode, sinusoïde, racine étirable, options de pose et flèches de tension ; suivi des fils attachés pour les objets reconnus.
- AppLoad : entrées stylet et clavier natif, fenêtres corrigées, boutons Calculatrice et PDF dans la barre native.
- rePDF : lecture de PDF en fenêtre, sélecteur local et bibliothèque, largeur et hauteur indépendantes, zoom, rotation et mode lecture.
- Paper Bridge : création et ouverture natives de notes reAgenda, import natif de PDF et d’images pour reMoodle, reprise des demandes et limitation des doublons.

Le socle local **AppLoad 3.28.4** utilise l’archive d’applications **3.28.3** dans son paquet historique. Des adaptations ultérieures ont ajouté des fonctions au-delà de cette archive. Le socle **reInk 0.8.2** corrige l’attente de disponibilité de page avant l’attachement des outils. La portée de chaque validation est précisée dans les [guides des mods](docs/mods/appload.md), les [documents de conception](docs/design/README.md) et les rapports techniques associés.
