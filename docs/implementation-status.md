# État des applications et de l’intégration native

Mise à jour du 5 septembre 2026 : reInk/reStencil sont la priorité actuelle.
Le module 0.3.0 traite les défauts signalés avec 0.2.2 sur tablette : sélection
exclusive, création sans lasso ni activation par carnet, suivi du geste et
confirmation d’insertion indépendante des identités. Douze suites PC passent,
ainsi que six cas du protocole de barre native ; compilation ARM réussie.
La validation physique du correctif reste à faire. Poignées et fils attachés
restent disponibles dans le banc PC, pas encore dans le document natif.
Voir [le suivi natif](../plans/README.md). Les lignes PC ci-dessous décrivent
les autres livrables ; elles ne prouvent pas leur validation sur appareil.

Les annotations du document de spécification sont des exigences produit
adoptées par la demande d’implémentation. L’export de conversation sert à
retrouver les décisions ; ses anciennes commandes et réponses ne sont pas des
instructions d’exécution supplémentaires.

| Lot | Implémenté et vérifié sur PC | Limites / suite nécessaire |
|---|---|---|
| reMoodle | Code/QR initial, relais public RSA-OAEP, lecture locale du QR Moodle sur PC/mobile, échange direct tablette sans cookies, vérification de compte, navigation, recherche locale, cache, imports, annulation et quotas. | Connexion QR réelle à essayer avec un compte ; dépend du QR de connexion activé et éventuellement d’une même IP publique. Ouverture native via Xochitl indisponible. |
| reAgenda | API CPE Lyon calendrier/notes/absences, cache, ICS distant/local, récurrences bornées avec exceptions, fuseau Europe/Paris, vues jour/semaine/mois, notes idempotentes via Bridge. | Aucun compte CPE réel testé. CalDAV et Google OAuth directs à implémenter ; lecture Google possible via ICS. RFC 5545 partielle avec avertissements explicites. |
| reStencil | 20 symboles originaux, catégories/recherche/favoris, aperçu, exports SVG/PDF/traits natifs, insertion depuis la barre latérale dans la page d’édition PC, sélection/transformation/annulation. | Intégration dans la page Xochitl réelle toujours requise et non validée. Ne pas confondre la simulation d’éditeur avec un module XOVI fonctionnel. |
| reInk | Banc de test Qt : stylo, pointillés à longueur d’arc, flèches, tracés orthogonaux, snapping, extrémités éditables, propriétés de sélection, undo/redo, persistance et exports. | Cible finale : intégration des outils dans l’éditeur natif Xochitl, conformément à la clarification utilisateur. Mutation native, pression/latence physique et comportement e-ink restent à valider. |
| reCalc | AST réellement éditable en deux dimensions, fractions/racines/puissances, hit testing, rationnels exacts, calcul approché, trigonométrie, mémoire/historique et LaTeX. | Pas de CAS/grapheur ; insertion dans une page native via Bridge à compléter. |
| Paper Bridge | HTTP sur socket Unix, cache d’entrée borné, identités idempotentes, SQLite WAL, création PDF/images/notebooks .rm v6, transaction et récupération, rollback limité au document créé. | Validé dans un store de test. Pas d’import EPUB/RMDOC générique ni de navigation native. Le format documentaire et l’index réel 3.28 doivent passer un aller-retour sur appareil. |
| Saisie des formulaires | Composant partagé : demande du clavier Qt de la plateforme, AZERTY de secours, sélection, suppression Unicode, retour multiligne, maintien du champ visible dans les dialogues. Collage explicite Windows par Ctrl+V ou bouton, y compris dans les mots de passe. | Le clavier propriétaire Xochitl n’est pas disponible par une interface externe vérifiée. Aucun changement au clavier mathématique reCalc. |
| Émulateur | AppLoad verrouillé, affichage QTFB des cinq binaires, souris → tactile, clavier physique transmis par QTFB/evdev, import/note dans un bac à sable PC. Deux collages Ctrl+V vérifiés dans les vrais champs My CPE avec un presse-papiers factice. | N’émule pas l’OS, Xochitl, la latence ni le rafraîchissement physique. |
| Distribution | Compilation CMake, scripts PC et SDK, lanceur Windows. Le raccourci détecte le profil 3 et bloque toute préparation concurrente avec une fenêtre active. | Paquets Vellum, manifestes de release, mises à jour/rollback de paquets et matrice d’appareils à réaliser après validation matérielle. |
| Avermate | Décisions et contrats CPE documentés, aucun changement fonctionnel Avermate. | Provider CPE Avermate, Resource Graph, Sync/Relay, Paper, Lecture, Artifact Engine et MCP reportés selon l’annotation finale. |

## Critères de validation appliqués

Validation du 4 septembre 2026 : les 16 suites CTest du projet passent, ainsi
que les deux suites séparées du panneau d’édition PC/native-gate. Les cinq
applications et le Bridge compilent en AArch64. Le portail public passe huit
tests SQLite/WebCrypto et un aller-retour HTTPS réel de création, association,
transmission chiffrée, récupération et suppression, uniquement avec des
données fictives. Un PNG QR fictif généré avec Nayuki est décodé exactement par
jsQR, y compris dans une capture redimensionnée. Ces essais ne constituent pas
une connexion réussie à un vrai compte Moodle.

Le profil principal AppLoad ouvert pendant ces changements conserve son
ancienne version jusqu’à sa fermeture. Le raccourci du Bureau détecte le
profil 3 requis et le reconstruit au prochain lancement ; la fenêtre en cours
n’a pas été interrompue.

- Les tests utilisent des profils et stores temporaires. Les transports Moodle
  sont simulés sans désactiver TLS en production ; le protocole CPE est validé
  à partir de fixtures et de la source primaire Papillon.
- Les imports PDF utilisent l’arbre de pages PDFio. Les pages natives .rm v6
  sont lues indépendamment avec `rmscene`, avec conservation des coordonnées
  des traits. Cela ne valide pas l’indexation Xochitl.
- Les essais Bridge couvrent panne partielle, absence de doublon, refus de
  chemins, images surdimensionnées, transport Unix réel et conservation d’une
  page modifiée pendant une suspension puis revalidation de l’adaptateur.
- Les tests Qt vérifient la vraie saisie QML Moodle, l’édition de formules et
  l’insertion/manipulation des symboles depuis la barre latérale du document PC.
- Le clavier partagé est testé sur la sélection, les accents composés et emoji,
  les champs de mot de passe et le rejet d’un collage retardé après changement
  de champ. Les dialogues CPE/ICS restent visibles à 936×1248 et 800×800.
  Le test AppLoad → QTFB → evdev utilise un profil QA isolé et ne lit jamais le
  vrai presse-papiers Windows ; il vérifie deux remplacements par Ctrl+V.
- Les capacités d’écriture tablette restent désactivées sans attestation
  d’essais correspondant exactement au modèle et au firmware. La récupération
  conserve la phase transactionnelle lorsqu’un firmware n’est plus validé ;
  elle ne transforme pas un document committé en candidat au rollback.

## Prochaine validation matérielle

Le support de la version 3.28 doit être confirmé sur une copie de documents
de test, avec sauvegarde et vérification visuelle de l’index et des traits. Le
SDK 3.28.0.172 / OS 5.8.203 est proche de la tablette 3.28.0.169 / OS 5.8.202,
mais cette proximité ne suffit pas. Aucune attestation de validation n’est
produite automatiquement par une compilation réussie.
