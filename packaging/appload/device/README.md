# AppLoad rePaper 3.28.4

Paquet local pour **Paper Pro ferrari, OS 3.28.0.169, Qt 6.10.3**.
Il fournit AppLoad corrigé et **reMoodle, reAgenda et reCalc**.
Le numéro 3.28.4 désigne cette livraison, pas une mise à jour du système reMarkable.

## Corrections

- **reCalc en fenêtre** : un appui long sur son icône ouvre une fenêtre déplaçable
  et redimensionnable. Un appui simple ouvre le plein écran. Les coordonnées des
  entrées tiennent compte de la taille, des marges et de la rotation de la fenêtre.
  Le rendu conserve désormais la transformation du peintre e-paper natif : le
  contenu suit la fenêtre lorsqu’elle est déplacée ou redimensionnée.
- **Stylet** : transmission des appuis, déplacements, relâchements et de la pression
  par AppLoad ; correction de l’échelle dans le pilote tablette Qt et activation
  des boutons et champs des trois applications avec le stylet.
  Cette livraison traite aussi les événements souris marqués « stylet » que
  Xochitl utilise réellement, pour les clics et les glissements.
  Cette voie native indique le contact ; elle ne fournit pas une pression mesurée.
- **Clavier reMarkable** : les champs de reMoodle et reAgenda communiquent avec le
  véritable clavier de Xochitl, y compris composition, sélection, effacement et
  validation. Le clavier de secours reste disponible si cette liaison échoue.
- **Paper Bridge** : inclus et démarré automatiquement pour reMoodle et reAgenda.
  Il s’arrête après quinze minutes d’inactivité et redémarre à la demande.
- **Notes de reAgenda** : le bouton demande à reMarkable de créer puis ouvrir un
  carnet. Les clics suivants retrouvent le même carnet pour cet événement.
  Les carnets sont rangés dans le dossier natif **Notes de réunion** (« Meeting
  Notes » selon la langue du système), visible dans la bibliothèque et géré comme
  les autres dossiers reMarkable. Les anciens carnets y sont déplacés à leur
  prochaine ouverture, sans changer leur nom, leurs pages ni leurs annotations.
  Les nouveaux carnets utilisent une copie du modèle journalier **`P Day` en portrait**,
  avec le jour, l’horaire et la date inscrits dans ses cases. Ces trois valeurs
  font partie du fond de page. Une note de journée conserve un horaire vide.
  Le titre de l’événement est prioritaire ; la matière sert de repli si le titre
  manque. Ce titre utilise le texte natif reMarkable, modifiable avec ses outils.
  Les réouvertures ne rajoutent pas le titre ; les changements ultérieurs du calendrier
  ne réécrivent pas le contenu du carnet. Les anciens carnets ne sont pas remis en
  forme automatiquement. L’application se ferme après confirmation de l’ouverture
  et du préremplissage ; une erreur laisse la même note disponible pour réessayer.
- **Import reMoodle** : les PDF et images passent par l’importeur natif reMarkable.
  Les images sont préparées en PDF. Le succès exige la présence du document dans
  la bibliothèque ; une reprise de la même demande évite un second import.
- **Calendrier** : chargement automatique au changement de semaine, de jour ou de
  mois. Les résultats des anciennes périodes sont ignorés. Les périodes chargées
  depuis moins de trente secondes sont réutilisées ; Actualiser force une requête.

## Mise à jour manuelle avec reManager

Depuis **3.28.3**, remplacer `appload.so` suffit : l’archive des applications est
inchangée. Depuis une version plus ancienne, remplacer également l’archive des
applications. XOVI et Qt Resource Rebuilder doivent être présents, comme pour reInk.

1. Fermer les applications et le carnet, puis choisir **Stop UI** dans reManager.
2. Conserver l’ancien `appload.so` sur le PC. Copier le nouveau **appload.so** dans
   `/home/root/xovi/extensions.d/`, en remplaçant celui qui s’y trouve. Garder un
   seul module AppLoad actif dans ce dossier.
3. Si les applications 3.28.3 ne sont pas déjà installées, avec **File Browser**,
   envoyer **repaper-apps-aarch64.tar.gz** dans `/home/root/`.
4. Si l’archive doit être mise à jour, dans le **Terminal** de reManager, exécuter :

   ```sh
   systemctl stop repaper-paper-bridge.service 2>/dev/null || true
   mkdir -p /home/root/xovi/exthome/appload
   tar -xzf /home/root/repaper-apps-aarch64.tar.gz -C /home/root/xovi/exthome/appload
   ```

   L’extraction met à jour `remoodle`, `reagenda`, `recalc` et `runtime`, avec leurs
   permissions exécutables. Les comptes et données dans `~/.local/share/RePaper/`
   sont conservés. Aucun profil personnel du PC n’est inclus.
5. Choisir **Start UI with Mods**, puis ouvrir **AppLoad** depuis la barre latérale.

Pour revenir au paquet précédent, arrêter l’UI et le service Bridge, restaurer
l’ancien module et réextraire l’ancienne archive, puis redémarrer l’UI avec les mods.

## Essai après installation

1. Ouvrir reCalc par appui long, réduire sa fenêtre et activer plusieurs touches
   au stylet et au doigt. La barre de titre permet de déplacer, réduire et fermer
   la fenêtre ; son coin inférieur droit permet de la redimensionner.
2. Ouvrir un champ dans reMoodle ou reAgenda : vérifier le clavier reMarkable,
   quelques accents, l’effacement et la validation.
3. Dans reAgenda avec une source configurée, changer de semaine : les événements
   se chargent automatiquement. Le chargement dépend de la connexion et de la source.
4. Ouvrir les notes d’un nouvel événement : vérifier le dossier Notes de réunion,
   le modèle P Day en portrait, la matière, le jour, la date et l’horaire.
   Ajouter une annotation, revenir dans reAgenda et répéter : le même carnet doit
   être retrouvé sans doublon de texte, y compris après l’avoir renommé.
   Une ancienne note doit être rangée dans ce dossier en conservant ses pages.
5. Dans reMoodle, importer un PDF puis une image. Vérifier leur présence dans la
   bibliothèque reMarkable. Répéter l’import du même fichier pour vérifier que
   le document déjà créé est retrouvé.

Le dossier reste visible ; aucun mécanisme de masquage n’est activé. La synchronisation
reste celle de la bibliothèque reMarkable et de la configuration de l’appareil.
Le module natif reInk 0.8.2 reste installé séparément pour ses outils d’édition ;
le préremplissage des notes ne nécessite pas ce module.

## Portée et validation

Les campagnes précédentes ont couvert les builds ARM, le clavier, les entrées
stylet sous LinuxFB/QEMU, le rendu e-paper isolé du SDK et les interfaces de carnets
et d’import. Ces validations ne constituent pas une nouvelle campagne complète
pour 3.28.4.

Sur la tablette Paper Pro 3.28.0.169, cinq carnets existants ont reçu leur en-tête
et leur titre natif, avec confirmation de fin par l’API, relecture des fichiers
enregistrés et contrôle visuel des cinq rendus. Les titres accentués, un titre
long et la réouverture sans doublon ont été vérifiés. Les notes supprimées n’ont
pas été recréées. L’utilisateur a également confirmé l’ouverture d’un carnet
après la correction de disponibilité de page de reInk 0.8.2.

Les huit suites de l’hôte natif, les 19 contrôles du texte et les 33 contrôles
du modèle passent. Le QMD reInk reste inchangé ; les deux ordres de chargement
avec AppLoad et les ressources compilées des modules ont été vérifiés hors ligne.
Les imports et les autres applications ne constituent pas une nouvelle campagne
complète sur tablette pour cette livraison.

Paper Bridge permet ici la création et l’ouverture des notes et l’import des PDF
et images via les interfaces natives. Les autres formats restent indisponibles.
Si un import a été lancé sans confirmation, une nouvelle tentative vérifie son
état sans le relancer à l’aveugle. Les téléchargements locaux restent disponibles.

AppLoad repose sur `502d03f4a0f4ea39810af1827366cac46bdc045f`, avec les hooks de
la PR 59 (`40506d47427123f07030bb2e83453a43d035b16a`) et les adaptations de cette
livraison. Les composants ARM utilisent le SDK reMarkable 5.8.203. Le runtime inclut
Qt 6.10.3, LinuxFB, SQLite, les pilotes d’entrée, les polices et les certificats.
La libc et son chargeur restent fournis par le système de la tablette.

`sources.tar.gz` contient les sources publiques, adaptations, licences et instructions
de reconstruction. Aucun Xochitl, QML propriétaire, document ni identifiant personnel
n’est redistribué.
