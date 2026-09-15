# reAgenda

**Votre emploi du temps et les notes qui vont avec, sur la tablette.**

reAgenda affiche les calendriers CPE Lyon et les fichiers ou abonnements ICS. Depuis une journée ou un événement, ouvrez un carnet natif pour prendre vos notes au stylet, puis retrouvez ce même carnet lors d'une prochaine consultation.

<img src="../design/mockups/current/reagenda-week.png" alt="Vue hebdomadaire de reAgenda avec des événements fictifs" width="420">

*Capture de l'application avec un calendrier de démonstration.*

## Ce que vous pouvez faire

- Lire le calendrier en vues **Jour**, **Semaine** et **Mois**, revenir à aujourd'hui ou changer de période.
- Ajouter un compte CPE Lyon, un abonnement ICS HTTPS ou un fichier `.ics` local.
- Consulter le titre, les horaires, la salle et les détails disponibles d'un événement.
- Actualiser la période affichée automatiquement à la navigation, ou demander une actualisation manuelle.
- Créer et retrouver une **Note du jour** ou les **notes d'un événement** dans la bibliothèque native.
- Consulter les notes, moyennes, crédits, validations et absences que CPE rend disponibles pour votre compte.

Les moyennes scolaires affichées viennent du serveur : reAgenda n'invente ni coefficients ni barèmes. Aucun serveur Avermate n'est nécessaire.

## Commencer

1. Ouvrez **reAgenda**, puis **Mes calendriers**, avec l'icône de réglages.
2. Choisissez **Connecter CPE** et saisissez vos identifiants, ou **Ajouter un ICS** et indiquez un lien HTTPS ou un fichier local.
3. Sélectionnez **Jour**, **Semaine** ou **Mois**. Touchez une journée du mois pour l'ouvrir ; touchez un événement pour consulter ses détails.
4. Utilisez l'icône de note d'une journée ou **Ouvrir les notes** sur un événement. Le dialogue suit la création et l'ouverture du carnet.

Avec l'intégration native, les carnets d'agenda rejoignent le dossier **Notes de réunion**. Les nouveaux carnets utilisent le modèle **P Day**, avec un en-tête lié à la date et à l'événement, et un titre en texte natif modifiable. Réouvrir une note retrouve son contenu ; les anciens carnets ne sont pas remis en forme automatiquement.

## Prérequis et limites

**Cette première publication fournit les sources, sans paquet binaire prêt à installer.** Consultez le [guide d'installation et de compilation](../INSTALLATION.md).

La lecture d'un calendrier fonctionne dans l'application Qt sur PC. La création et l'ouverture des notes sur tablette nécessitent **Paper Bridge** et l'hôte natif **AppLoad**. La cible d'intégration documentée est la **Paper Pro « ferrari » sous OS 3.28.0.169**.

Le connecteur CPE dépend d'une API mobile observée, sans garantie de stabilité de l'établissement. Une session expirée demande une reconnexion. Les fonctions disponibles dépendent aussi des droits du compte.

Les abonnements ICS sont en **lecture seule**. Les récurrences courantes et leurs exceptions sont prises en charge ; les règles non reconnues produisent un avertissement. CalDAV et la connexion directe par Google OAuth ne sont pas implémentés. Utilisez un lien ICS final : les redirections HTTPS sont refusées.

L'affichage utilise le fuseau **Europe/Paris**. L'application accepte jusqu'à 12 sources et borne la taille des téléchargements, du cache et des récurrences. Le détail des limites figure dans le guide technique.

## Données et vérifications

Le mot de passe CPE n'est pas enregistré. Le jeton de session et les liens ICS privés sont protégés par le coffre local ; les événements et données scolaires restent dans un cache local. **Déconnecter** conserve ce cache. **Retirer le calendrier** enlève les données de cette source sans supprimer les carnets déjà créés.

Les tests couvrent les dates, changements d'heure, récurrences, requêtes concurrentes et demandes de notes. Le rapport de livraison documente la création et la réouverture de cinq notes sur la tablette. Le [contrat CPE](../cpe-protocol.md) documente également une variante de réponse réelle ; cela ne vaut pas validation de tous les comptes et de tous les services CPE.

[Guide technique et compilation](../../apps/reagenda/README.md) · [Contrat du connecteur CPE](../cpe-protocol.md) · [Retour à RePaper](../../README.md)
