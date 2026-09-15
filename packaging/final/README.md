# RePaper — correctifs du 6 septembre 2026

Pour **reMarkable Paper Pro ferrari, OS 3.28.0.169, Qt 6.10.3**.
Les correctifs portent les versions **AppLoad rePaper 3.28.4** et
**reInk / reSelect / reStencil 0.8.2**. Ils ne changent pas la version
du système reMarkable. XOVI et Qt Resource Rebuilder doivent déjà être installés.

**RMChat est mis en pause et exclu du paquet.** Son code est conservé dans le
projet ; aucun compte, session ChatGPT ou profil personnel du PC n’est fourni.

## Installation avec reManager

Utiliser les fichiers des paquets `appload-repaper-3.28.4` et `reink-native-0.8.2`
sur le PC. Remplacer les deux modules. L’archive des applications reste celle de
3.28.3 ; son extraction n’est nécessaire que si cette version n’est pas installée.
L’ancien ZIP `repaper-final-2026-09-06.zip` contient les versions 3.28.3 et 0.8.1.

1. Fermer les applications et le carnet, puis choisir **Stop UI** dans reManager.
2. Sauvegarder sur le PC les anciens `appload.so` et `reink-editor.so` présents
   dans `/home/root/xovi/extensions.d/`.
3. Copier les nouveaux **`appload.so`** et **`reink-editor.so`** dans
   `/home/root/xovi/extensions.d/`, en remplaçant les anciens. Garder un seul
   exemplaire actif de chaque module dans ce dossier.
4. Si nécessaire, avec **File Browser**, envoyer **`repaper-apps-aarch64.tar.gz`**
   dans `/home/root/`. Dans le **Terminal** de reManager, exécuter :

   ```sh
   systemctl stop repaper-paper-bridge.service 2>/dev/null || true
   mkdir -p /home/root/xovi/exthome/appload
   tar -xzf /home/root/repaper-apps-aarch64.tar.gz -C /home/root/xovi/exthome/appload
   ```

5. Choisir **Start UI with Mods**. AppLoad ouvre **reMoodle, reAgenda et reCalc**.
   Dans un carnet, la barre native donne accès à **reInk, reSelect et reStencil**.

L’extraction conserve les comptes et données dans `~/.local/share/RePaper/`.
Les sources et les rapports de validation sont destinés au PC et ne sont pas à
copier sur la tablette.

## Correctifs inclus

- **reAgenda** : chargement au changement de période ; notes dans le dossier
  natif **Notes de réunion** ; nouveaux carnets avec une copie du modèle **P Day en portrait**,
  jour, horaire et date remplis dans les cases du fond, titre en texte natif modifiable. Une réouverture retrouve
  le même carnet sans rajouter le texte. Les anciennes notes sont déplacées dans
  le dossier à leur réouverture en conservant leurs pages et annotations.
- **reMoodle** : import natif des PDF et images, confirmation dans la bibliothèque
  et reprise de la même demande sans nouvel import systématique.
- **AppLoad** : clavier natif reMarkable, clics et glissements au stylet ;
  **reCalc en fenêtre** par appui long, avec déplacement et redimensionnement corrigés.
- **reInk / reStencil** : conservation des formes dans le document, édition des
  extrémités et des propriétés, couleurs et historique ; proportions verrouillées
  par un appui immobile d’une seconde. **reSelect** active directement la sélection.
  Dans **Propriétés**, **Annuler** revient sur les modifications de cette ouverture ;
  **Valider** et **Fermer** les conservent. La version 0.8.2 attend que la page et
  ses coordonnées soient disponibles avant d’attacher les outils.

Le dossier Notes de réunion reste visible et suit la synchronisation native de
la tablette. Le titre utilise le texte natif modifiable de reMarkable ;
le jour, l’horaire et la date appartiennent au modèle de page. Les anciennes pages
et les notes déjà initialisées ne sont pas remises en forme automatiquement.

## Vérification après installation

1. Dans reAgenda, ouvrir les notes d’un nouvel événement, vérifier le dossier,
   le modèle et les quatre champs. Annoter, puis rouvrir : même carnet, sans doublon.
2. Dans reMoodle, essayer le clavier, puis l’import d’un PDF et d’une image.
3. Ouvrir reCalc par appui long, déplacer et redimensionner la fenêtre, puis
   toucher les touches au stylet et au doigt.
4. Dans un carnet d’essai, créer une forme reStencil, la sélectionner avec reSelect,
   modifier plusieurs propriétés et choisir Annuler. Essayer aussi le maintien
   des proportions, Annuler/Rétablir et la réouverture du carnet.

L’utilisateur a confirmé l’ouverture d’un carnet après le correctif reInk.
Cinq notes d’agenda ont reçu leur en-tête et leur titre natif sur la tablette,
avec confirmation de fin par l’API et contrôle des fichiers enregistrés et des
cinq rendus. La réouverture ne rajoute pas le titre. Ces observations ne valent
pas validation de tous les parcours ; les guides et rapports précisent leur portée.

## Contenu et retour à la version précédente

- `appload.so` : AppLoad rePaper 3.28.4 ; `repaper-apps-aarch64.tar.gz` : applications inchangées de 3.28.3.
- `reink-editor.so` : outils natifs 0.8.2, incluant reInk, reSelect et reStencil.
- `README.md` de chaque composant : guide détaillé.
- Archives de sources : sources correspondant aux binaires,
  dépendances publiques, licences et instructions de reconstruction.
- `validation/`, `manifest.json`, `SHA256SUMS` : preuves et empreintes du paquet.

Pour revenir à la livraison précédente : choisir Stop UI, arrêter le service
Bridge avec la commande ci-dessus, restaurer les anciens modules et réextraire
l’ancienne archive des applications au même emplacement, puis choisir Start UI
with Mods. Les livraisons précédentes sont conservées sur le PC.
