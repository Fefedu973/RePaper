# Contribuer à RePaper

Les signalements précis, corrections, tests et améliorations de documentation sont les bienvenus. Commencez par la [présentation des mods](docs/mods/README.md) et le [guide de compilation](docs/BUILDING.md).

## Signaler un problème

Ouvrez une issue en indiquant :

- le mod concerné et le commit testé (`git rev-parse HEAD`) ;
- l'environnement : PC/émulateur ou tablette ; pour une tablette, le modèle et la version exacte du système ;
- les étapes minimales pour reproduire, le résultat attendu et le résultat observé ;
- les journaux utiles ou une capture avec des données fictives, si cela aide à comprendre.

Retirez des pièces jointes les identifiants, jetons, QR de connexion, liens ICS privés et données personnelles. Un problème observé sur PC et un problème observé dans Xochitl doivent être décrits avec leur environnement respectif.

## Proposer une modification

Créez une branche, gardez la modification centrée sur un problème et décrivez son effet dans la pull request. Suivez le style des fichiers concernés et mettez à jour la documentation lorsque l'utilisation ou les limites changent.

Pour un changement de comportement, ajoutez ou adaptez un test qui reproduit le cas utile. Dans la pull request, indiquez les commandes exécutées et leur résultat ; précisez les vérifications que vous n'avez pas pu effectuer. Une simple correction de documentation n'exige pas de nouveaux tests de code.

## Vérifier sur PC

Avec les dépendances Linux ou WSL installées, depuis la racine du dépôt :

```sh
bash tools/build-pc.sh
```

Le script initialise PDFio, compile les applications actives et exécute leurs tests en rendu logiciel. Il ne déploie rien sur une tablette. rePDF exige aussi les dépendances PDFium décrites dans [son guide](apps/repdf/README.md).

Pour relancer uniquement les tests d'une compilation existante :

```sh
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
  ctest --test-dir "$HOME/repaper-build" --output-on-failure
```

Si vous utilisez un autre dossier de compilation, remplacez ce chemin. Pour les modules natifs ou le portail Moodle, suivez les commandes supplémentaires du README du composant concerné. Les tests PC ne prouvent pas à eux seuls la compatibilité d'un firmware ou la latence de l'écran de la tablette.

## Licences et contenus tiers

Le code original RePaper est proposé sous [licence MIT](LICENSE). Les contributions originales sont destinées à être distribuées dans ce même cadre. Les bibliothèques, composants adaptés et autres contenus tiers conservent leurs propres licences et notices : la licence MIT de RePaper ne les remplace pas.

Préservez ces notices et indiquez la provenance et la licence de toute nouvelle dépendance ou ressource. N'ajoutez pas de firmware, polices propriétaires ou données de compte au dépôt. Les références existantes sont réunies dans [les sources du projet](docs/sources.md).
