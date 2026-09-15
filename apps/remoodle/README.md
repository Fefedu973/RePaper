# reMoodle

Client Moodle autonome Qt Quick : authentification mobile dans le navigateur habituel, navigation cours → sections → activités → fichiers, cache local par compte et import explicite par Paper Bridge. L’émulateur PC et `--demo` fonctionnent sans accès à une tablette.

## Connexion et profils

Le champ d’adresse de connexion représente une **adresse en préparation**, distincte de celle du compte actif. Préparer un lien ou modifier ce champ ne change jamais la destination des requêtes du compte déjà connecté. Modifier l’adresse invalide le lien de connexion précédent. Une erreur ou une annulation pendant la vérification d’un nouveau compte restaure le compte précédent et conserve son coffre chiffré.

Une connexion réelle réussie depuis la démonstration quitte le mode démo ; la navigation et l’actualisation interrogent alors le Moodle choisi. Déconnexion et connexion mettent à jour la propriété QML de simulation.

Les PDF/images téléchargés sont nommés `<empreinte-compte>-<révision>.download`. L’empreinte dépend de l’adresse du site et de l’identifiant utilisateur Moodle. Un fichier d’un autre compte n’est jamais considéré comme un cache valide. Les fichiers de l’ancien format non partitionné sont ignorés, sans migration implicite qui leur attribuerait arbitrairement un propriétaire. La limite locale de cache reste globale (256 Mio) ; les documents déjà importés sont des copies indépendantes.

## Décision : identité indépendante de l’ordre

L’exemple d’identité qui utilisait l’index d’un fichier dans la liste de contenu n’est pas retenu. Cet index change lorsque Moodle insère ou réordonne les ressources d’un dossier et entraînait une nouvelle clé d’import pour un document inchangé.

L’identité utilisée est maintenant le SHA-256 de `(compte, cours, module, URL publique canonique)`. Les paramètres d’authentification sont retirés de l’URL. L’ancien paramètre `index` reste accepté par la fonction C++ pour compatibilité des appels mais n’entre pas dans le hash. Deux références au même fichier dans un même module ont donc la même identité, indépendamment de leur position. La révision de contenu reste séparée de cette identité.

Les clés calculées par la version de développement précédente ne sont pas identiques. Cette correction précède la première distribution ; elle ne tente aucune réécriture de documents natifs existants.

## Annulation et transport

Les callbacks de téléchargement vérifient leur génération d’opération avant de modifier l’état. Une progression ou une fin de réponse annulée ne peut pas remplacer la progression ni perdre le handle d’un téléchargement ultérieur. Les fichiers temporaires annulés ne sont pas validés.

La production utilise `QNetworkAccessManager` avec la vérification TLS normale de Qt, des URLs HTTPS, des redirections manuelles, des limites de volume et des délais réseau. Le constructeur peut recevoir un gestionnaire réseau C++ externe pour les tests ; aucun paramètre de ligne de commande, variable d’environnement ou API QML n’active un contournement TLS.

## Import PowerPoint local

Les fichiers `.ppt` et `.pptx` de la même origine Moodle sont téléchargés puis convertis en PDF par LibreOffice Impress sans interface, avant l’import Paper Bridge. Le cache garde son nom `<empreinte-compte>-<révision>.download` et la clé d’idempotence du document d’origine. Seul un PDF terminé remplace atomiquement le téléchargement ; réimporter la même ressource ne relance ni le téléchargement ni la conversion. Le titre importé porte l’extension `.pdf`.

La conversion utilise un processus séparé, un profil temporaire avec macros désactivées et une limite de 180 secondes. Annuler tue ce processus et conserve l’original en cache pour réessayer. Un arrêt, un contenu invalide ou un manque de place n’enregistre aucun PDF partiel. Les entrées et sorties restent limitées à 64 Mio. La lecture préalable des ZIP PPTX est bornée à 8 Mio par partie XML et 256 Mio décompressés au total.

Le moteur ARM64 est un paquet LibreOffice 25.2.3.2 `nogui` issu des dépôts Debian signés. Le script `tools/build-powerpoint-runtime.py` télécharge et extrait ses dépendances dans un répertoire privé ; il ne les installe pas sur le PC et ne contacte aucune tablette. La variante validée occupe environ 342 Mio, ou 129 Mio compressés. Le chargeur et la glibc Debian sont exclus : le test ARM utilise le chargeur/glibc 2.39 du SDK reMarkable 5.8. Les bibliothèques privées et les polices ne modifient pas celles d’AppLoad ou de Qt.

```sh
python3 tools/build-powerpoint-runtime.py \
  --work /tmp/repaper-office-build \
  --output /tmp/repaper-office-arm64
# Reproduire le paquet sans accès réseau à partir du verrou et des .deb déjà présents :
python3 tools/build-powerpoint-runtime.py \
  --work /tmp/repaper-office-build \
  --output /tmp/repaper-office-arm64-copy --reuse-downloads
```

Placer le dossier produit sous `lib/repaper-office` dans une installation contenant `bin/remoodle`, ou définir `REPAPER_OFFICE_CONVERTER` avec le chemin absolu du fichier `convert`. Le lanceur AppLoad peut ainsi utiliser son propre emplacement de runtime. Le lanceur Office fonctionne depuis le répertoire temporaire préparé par le backend ; son `exec` conserve la possibilité d’annuler le processus. `packages.json`, `SHA256SUMS` et `licenses/` accompagnent le moteur. Ils enregistrent les versions, sources Debian et licences des dépendances ; toute redistribution doit aussi respecter les obligations de fourniture des sources de ces licences.

La fidélité dépend des fonctions prises en charge par LibreOffice et des polices disponibles. Arial/Times sont remplacées par Liberation, Calibri par Liberation Sans ; une identité pixel à pixel avec Microsoft Office n’est donc pas promise. Les animations, médias actifs et macros ne sont pas conservés dans un PDF statique.

Un cas précis a été reproduit avec une formule OMML affichée correctement et sérialisée par Microsoft PowerPoint : lorsqu’une branche `mc:Choice` Office 2010 contient cette formule et que son `mc:Fallback` ne contient aucun aperçu image embarqué, le moteur choisit le repli et perd la formule. Le backend refuse ce cas avec « Cette présentation contient une équation Office sans aperçu compatible. Exportez-la en PDF depuis PowerPoint pour conserver la formule. ». Il accepte la même équation munie de son aperçu image ; cet aperçu reste une image dans le PDF. Ce contrôle ciblé ne constitue pas un audit exhaustif de toutes les fonctions PowerPoint.

Les tests du processus utilisent un petit exécutable de simulation pour les arrêts, délais, redémarrages et écritures atomiques. La vraie conversion est un test distinct, activé explicitement :

```sh
QT_QPA_PLATFORM=offscreen \
REPAPER_OFFICE_REAL_CONVERTER=/chemin/repaper-office/convert \
REPAPER_OFFICE_TEST_ARTIFACTS=/tmp/ppt-pdf-verification \
build/remoodle/powerpoint-converter-tests
```

Les trois fichiers de `tests/fixtures` couvrent PPTX texte et image, ancien PPT binaire, et équation native avec aperçu. Une quatrième fixture vérifie le refus exact sans aperçu. Le moteur ARM64 a été exécuté localement sous QEMU avec le SDK reMarkable, y compris après déplacement du paquet dans un chemin contenant des espaces. Les six pages PDF ont été rendues et vérifiées visuellement. Cela ne remplace pas une mesure de performances sur tablette ; aucun essai tablette n’est effectué par ces tests.

Références du moteur et du format : [paramètres LibreOffice](https://help.libreoffice.org/latest/en-US/text/shared/guide/start_parameters.html), [paquet Impress sans interface](https://packages.debian.org/bookworm-backports/libreoffice-impress-nogui), [extensions mathématiques DrawingML de Microsoft](https://learn.microsoft.com/en-us/openspecs/office_standards/ms-odrawxml/853b19c7-68a9-4f9a-a2ae-5e6cb0d02e62).

## Vérification locale

```sh
cmake -S apps/remoodle -B build/remoodle -DCMAKE_BUILD_TYPE=Debug
cmake --build build/remoodle --parallel
ctest --test-dir build/remoodle --output-on-failure
```

`moodle-tests` couvre les liens de connexion, la validation HTTPS, la suppression des secrets dans les identités, les révisions, les origines et la stabilité après réorganisation des fichiers.

`moodle-controller-tests` utilise un transport en mémoire et des profils temporaires. Il couvre :

- connexion réelle après une démo, navigation et actualisation réseau ;
- saisie par événement Qt dans le véritable champ QML, préservation de l’URL en préparation et isolation du compte actif ;
- même ressource pour deux comptes, absence de réutilisation du cache, conservation du fichier du premier compte et redaction des snapshots SQLite ;
- progression et fin tardives d’une réponse annulée pendant qu’un autre téléchargement est actif, puis annulation du bon téléchargement ;
- connexion annulée ou refusée, réponse tardive ignorée, conservation du compte et du token précédent dans le coffre ;
- refus serveur après chargement : les sections en cache restent disponibles.

Ce transport vérifie également que les requêtes produites conservent HTTPS, les redirections manuelles, un délai et une politique de vérification TLS active. Il ne teste pas une vraie négociation TLS ni le SSO de CPE ; ces essais restent distincts. Aucune connexion à la tablette ni écriture Xochitl n’est effectuée par ces tests.
