# reMoodle

**Retrouvez vos cours Moodle et importez leurs documents dans la bibliothèque reMarkable.**

reMoodle relie les ressources d'un cours à la prise de notes sur tablette : choisissez un cours, ouvrez une section, puis importez le fichier qui vous intéresse. Le document importé devient une copie indépendante, que vous pouvez annoter avec les outils habituels de reMarkable.

<img src="../design/mockups/current/remoodle.png" alt="Liste de cours fictifs dans reMoodle" width="420">

*Capture de l'application avec des cours de démonstration.*

## Ce que vous pouvez faire

- Parcourir les cours, sections, activités et fichiers ; rechercher dans la liste affichée.
- Importer des PDF et images dans la bibliothèque native, par une action explicite.
- Importer des présentations `.ppt` et `.pptx` après conversion locale en PDF, lorsque le moteur de conversion est installé.
- Retrouver les éléments déjà mis en cache. Le cache des téléchargements est séparé par compte.
- Annuler un téléchargement ou une conversion et réessayer ensuite.
- Réouvrir une ressource déjà importée sans créer systématiquement un nouveau document pour la même révision.

## Premier import

1. Ouvrez **reMoodle** dans AppLoad, saisissez l'adresse HTTPS de votre Moodle et choisissez **Se connecter**.
2. Sur un téléphone ou un ordinateur, ouvrez le lien de liaison affiché par reMoodle, ou utilisez son code temporaire.
3. Connectez-vous à Moodle dans votre navigateur habituel. Dans **Profil → Application mobile**, affichez le QR de connexion, puis choisissez sa capture dans le portail de liaison.
4. Après validation du compte, ouvrez un cours, une section et une ressource compatible. Touchez le fichier pour lancer son import.

La tablette échange elle-même la clé du QR avec Moodle. Le mot de passe Moodle reste dans le parcours de connexion de votre navigateur. Le portail décode la capture localement et transmet une demande chiffrée à la tablette.

## Prérequis et limites

**Cette première publication fournit les sources, sans paquet binaire prêt à installer.** Consultez le [guide d'installation et de compilation](../INSTALLATION.md).

Sur tablette, reMoodle utilise **AppLoad** et **Paper Bridge** pour l'import natif. La cible d'intégration documentée est la **Paper Pro « ferrari » sous OS 3.28.0.169** ; les autres modèles et versions ne sont pas attestés.

Le serveur Moodle doit autoriser les services mobiles et le QR d'authentification pour votre compte. Un QR qui ne contient que l'adresse de l'établissement ne suffit pas. Selon la configuration du serveur, le navigateur et la tablette doivent partager la même adresse de sortie Internet. Le même Wi-Fi est généralement le point de départ, mais un VPN ou IPv6 peut changer cette adresse.

Le parcours de connexion utilise un **portail de liaison HTTPS**. Son code figure dans [moodle-connect](../../moodle-connect/README.md) ; une installation autonome doit disposer d'un portail configuré. Les essais décrits dans le dépôt ne constituent pas une validation de connexion réelle pour tous les établissements ; l'échange QR d'un compte CPE réel reste indiqué comme non validé dans le guide technique.

Les imports portent sur les fichiers compatibles de l'origine Moodle autorisée. reMoodle n'est pas une application complète de remise de devoirs, de quiz ou d'édition des cours.

La conversion PowerPoint requiert le runtime LibreOffice local séparé. Les polices et certaines mises en page peuvent différer ; animations, médias actifs et macros ne figurent pas dans le PDF. Une équation Office sans aperçu compatible est refusée pour éviter un document incomplet. Les fichiers sont limités à 64 Mio.

## Données et vérifications

Les jetons sont conservés dans un coffre chiffré local. Le cache des cours et des fichiers reste local ; se déconnecter ne supprime pas les documents déjà importés dans la bibliothèque.

Les tests couvrent les protocoles Moodle, le relais de liaison, l'isolation des comptes, l'annulation et la conversion PowerPoint. Les tests réseau simulés ne valident pas le SSO d'un établissement. Le guide technique distingue aussi la conversion ARM64 exécutée sous QEMU des essais physiques sur tablette.

[Guide technique et compilation](../../apps/remoodle/README.md) · [Fonctionnement du QR Moodle](../moodle-qr-login.md) · [Retour à RePaper](../../README.md)
