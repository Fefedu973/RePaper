# RMChat — archivé le 6 septembre 2026

RMChat est suspendu et retiré de la livraison standard RePaper. Les cinq
applications maintenues sont reMoodle, reAgenda, reStencil, reCalc et reInk.
Les sources, tests, outils de connexion et documents RMChat restent conservés
pour une éventuelle reprise. Cette décision ne conclut pas à une impossibilité
définitive.

La connexion et la lecture de conversations ont fonctionné lors des essais
réels. **L'envoi réel n'a pas été validé : il reste bloqué par un HTTP 403.**
Le diagnostic du 6 septembre a aussi reçu une page de vérification HTTP 403
au renouvellement de session, avant toute lecture du catalogue et sans envoyer
de message. Ces observations ne prouvent pas une cause commune. Les tests
locaux du moteur et du rendu ne constituent pas une validation de l'envoi réel.

## Effet sur la livraison standard

- CMake désactive `REPAPER_BUILD_RMCHAT` et `REPAPER_BUILD_RMCHAT_PROBE` par défaut.
- `tools/build-pc.sh` force ces options à `OFF`, même avec un ancien cache,
  et ne construit plus le moteur Go. Les anciens binaires restent sur disque.
- La préparation, l'enregistrement général et le lancement standard AppLoad
  excluent RMChat, même si ses deux binaires sont encore présents.
- L'ancien manifeste RMChat est déplacé dans
  `STATE/archived-applications/rmchat/external.manifest.<date numérique>.json`.
  Les autres applications et les données du bac à sable sont conservées.
- Le lanceur Windows explique l'archivage si `-App rmchat` est demandé.

Pour retirer seulement l'entrée d'un profil existant, fermer AppLoad puis lancer
dans Linux/WSL :

```sh
python3 tools/emulator-apps.py --state-dir /root/repaper-appload archive --app rmchat
```

Cette commande conserve les binaires, les documents, le coffre et ses
identifiants. Elle ne contacte ni ChatGPT, ni un compte, ni la tablette.

## Réactivation explicite pour recherche

Les commandes suivantes réactivent un prototype archivé avec ses limites
connues. Elles ne font pas partie du parcours d'installation standard.

```sh
REPAPER_BUILD_RMCHAT=ON bash tools/build-pc.sh
python3 tools/emulator-apps.py --state-dir /root/repaper-appload \
  register --app rmchat --allow-archived --app-build /root/repaper-build
python3 tools/emulator-apps.py --state-dir /root/repaper-appload \
  launch --app rmchat --allow-archived
```

Fermer AppLoad avant chaque enregistrement ou lancement. `--allow-archived`
est requis avec `--app rmchat` ; un enregistrement général ne le réajoute
jamais. Le prochain lancement standard retire de nouveau son entrée du menu.
Une construction autonome reste décrite dans le [guide historique](README.md).
Un cache CMake ancien utilisé directement doit être reconfiguré avec
`-DREPAPER_BUILD_RMCHAT=OFF -DREPAPER_BUILD_RMCHAT_PROBE=OFF` pour revenir au
périmètre standard.
