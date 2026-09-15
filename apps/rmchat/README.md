# RMChat natif

**Archivé depuis le 6 septembre 2026.** RMChat est exclu de la compilation,
du lanceur et de la livraison standard. L'envoi réel reste bloqué par un
HTTP 403. Voir [le statut et la réactivation explicite](ARCHIVED.md).
Ce guide conserve le fonctionnement du prototype pour une éventuelle reprise.

RMChat est une application Qt pour retrouver les conversations du compte
ChatGPT connecté, choisir un modèle, écrire un message avec réponse progressive
et joindre des PDF. Elle utilise `rmchat-core`, lancé localement à côté de
l’interface. Aucun relais PC permanent ni clé API OpenAI n’est nécessaire.

## Lancer dans AppLoad sur ce PC

Depuis la racine du projet, dans Linux/WSL, après l'enregistrement explicite
décrit dans [le statut d'archivage](ARCHIVED.md) :

```sh
python3 tools/emulator-apps.py --state-dir /root/repaper-appload \
  launch --app rmchat --allow-archived
```

Le menu standard exclut **RMChat (ChatGPT)**. Le profil de l'émulateur
conserve sa session dans son propre coffre chiffré.

## Connecter son compte

1. Installer l’extension locale Chrome ou Edge en suivant le
   [guide de connexion](../../tools/rmchat-browser-helper/README.md).
2. Se connecter à ChatGPT dans ce navigateur puis cliquer sur **Exporter la
   session** dans l’extension.
3. Dans RMChat, choisir **Choisir le fichier de session…** puis
   `rmchat-session.json` dans Téléchargements. Une fois l’import confirmé,
   supprimer ce fichier qui contient la session en clair.

L’app échange la session avec ChatGPT et enregistre immédiatement la session
renouvelée dans le coffre, avant de charger les modèles et conversations.
Au prochain démarrage, **Utiliser la session enregistrée** reconnecte le compte.
L’ouverture de l’app seule ne fait aucune requête au compte.

L’accès dépend du service web ChatGPT : une session expirée ou un refus de
vérification côté service est affiché dans l’app. L’app ne relance pas
automatiquement une requête refusée ou un message dont l’envoi est incertain.
La connexion et la lecture d’historique ont été confirmées sur le compte réel.
L’envoi réel reste bloqué par un HTTP 403. Le diagnostic du 6 septembre 2026
a ensuite reçu une page de vérification HTTP 403 au renouvellement de la session,
avant toute lecture du catalogue et sans envoyer de message. Ce diagnostic
ne prouve pas que le refus d’envoi précédent avait la même cause.

## Utiliser l’app

- **Nouveau chat** ouvre une conversation ; la liste à gauche retrouve
  l’historique. **Charger la suite** affiche les pages suivantes.
- Choisir un modèle, saisir le message puis **Envoyer** ou `Ctrl+Entrée`.
- Le sélecteur élimine les identifiants répétés. Les modèles distincts portant
  le même nom sont différenciés ; **Actualiser** recharge le catalogue sans
  effacer le brouillon. Les refus d’accès explicites du catalogue sont filtrés.
  L’absence d’un tel refus ne garantit pas le droit ou le quota d’envoi :
  le contrôle du catalogue réel reste empêché par la vérification HTTP 403.
- Les messages affichent titres, listes, tableaux, citations, blocs de code et
  formules LaTeX en ligne ou en bloc. Le rendu est natif, sans navigateur intégré.
  **Copier** récupère le texte Markdown complet. Les formules non prises en charge
  restent lisibles en source ; les très longues réponses peuvent être abrégées
  à l’écran pour limiter la mémoire utilisée sur tablette.
- L’historique affiche la branche active de la conversation et ses messages
  destinés à l’utilisateur. Les traces de raisonnement, messages système,
  outils et messages masqués sont exclus. Les références natives deviennent
  des liens lisibles ; voir [les règles de lecture et leurs sources](../../docs/rmchat-message-format.md).
- **+ PDF** ajoute un fichier local. Les PDF sont limités à 64 Mio.
- **Interrompre** arrête la requête locale ; le service peut avoir déjà reçu
  un message. Le brouillon reste disponible si l’envoi échoue.
- Un refus d’envoi indique l’étape concernée et conserve le brouillon.
  **Ouvrir ChatGPT sur le PC** et **Copier le brouillon** permettent de poursuivre
  manuellement dans le navigateur. Ces actions ne sont jamais automatiques.
- **Compte → Déconnecter ce compte** efface la session du coffre de cette app.

## Construire pour Linux PC

La construction globale `bash tools/build-pc.sh` exclut RMChat par défaut.
Pour une construction autonome avec Qt 6.2+, OpenSSL, CMake et Go :

```sh
bash tools/build-rmchat-core.sh /tmp/rmchat-core amd64
cmake -S apps/rmchat -B /tmp/rmchat-build \
  -DBUILD_TESTING=ON -DRMCHAT_CORE_BINARY=/tmp/rmchat-core
cmake --build /tmp/rmchat-build -j4
ctest --test-dir /tmp/rmchat-build --output-on-failure
python3 tools/emulator-apps.py --state-dir /root/repaper-appload \
  register --app rmchat --allow-archived --app-build /tmp/rmchat-build
```

Fermer la fenêtre AppLoad avant l’enregistrement. Le binaire `rmchat-core`
doit être exécutable et voisin de `rmchat`. Les détails du protocole initial
restent dans [la documentation du prototype](../../docs/rmchat-phase0.md) ;
le contrat actuel de persistance est décrit dans [le moteur](core/README.md).

La connexion navigateur se fait sur PC. Pour la tablette, le fichier de session
doit être transféré localement puis importé ; cette version n’ajoute pas encore
de transfert par QR code et n’a pas été installée sur une tablette physique.
