# RMChat — prototype PC, phase 0

Cette page conserve la documentation du prototype initial. L’interface native,
l’intégration AppLoad et le parcours de connexion actuel sont décrits dans
[le guide RMChat](../apps/rmchat/README.md).

La phase 0 vérifie le protocole sur PC avant de construire une interface
reMarkable. Le cœur Go dérivé des composants utiles d’Aurora communique avec
un lanceur C++ par socket Unix. Aucun écran QML ChatGPT, portail QR, déploiement
cloud ou paquet tablette RMChat n’est livré par cette phase. La distribution
rePaper 3.28.3 reste indépendante.

L’objectif est de consulter et poursuivre les conversations du compte
**ChatGPT Web**. L’historique et l’authentification de l’API OpenAI constituent
un autre produit : une clé API ne remplace pas une session Web dans ce contrat.
La présence d’une méthode dans le protocole ne prouve pas qu’elle fonctionne
avec un compte réel ; les capacités exposées et les validations distinguent
les opérations disponibles de celles encore bloquées.

## Processus et coffre

```text
stdin → rmchat-probe (C++ / SecretStore) → socket Unix → rmchat-core (Go) → HTTPS
```

Le C++ est l’unique propriétaire du coffre `repaper::SecretStore` : il chiffre,
lit et supprime les credentials. Le core Go les conserve uniquement en mémoire.
Il ne reçoit aucun secret dans ses arguments, son environnement ou un fichier
de configuration. Les réponses IPC, les journaux et les erreurs ne doivent pas
reproduire de credential, cookie ou réponse d’authentification brute.

Chaque commande, sauf `logout`, démarre un nouveau core avec `--socket CHEMIN_ABSOLU` et, pour
les fichiers, zéro ou plusieurs `--upload-root DOSSIER_ABSOLU`. Le lanceur crée
un répertoire de socket privé, retire au processus Go les variables de preload
et QTFB, puis ferme et attend son enfant à la fin. Aucun daemon n’est nécessaire.
Pour une commande réseau utilisant un compte enregistré, le lanceur lit le
coffre et appelle de nouveau `auth.import`, qui effectue une validation réseau. Il n’existe ni
`auth.restore`, ni `auth.commit`, ni validation fictive d’un credential chargé.

Lors d’un nouvel import, le lanceur ne remplace l’ancien coffre qu’après une
validation réseau réussie du candidat. Il ne déclare le compte prêt qu’après
le succès de `SecretStore.put`. Une erreur de sauvegarde ferme le core
provisoire et conserve le coffre précédent. `logout` retire le credential
local et arrête le core ; cette action ne prétend pas révoquer les autres
sessions du compte chez le fournisseur.

Le coffre utilise AES-256-GCM et des permissions privées. Sa clé reste sur le
même appareil : il ne protège pas contre un compte système compromis disposant
des droits de lecture correspondants.

## Commandes du prototype

```text
rmchat-probe [--core PATH] [--vault-dir PATH] import
rmchat-probe [--core PATH] [--vault-dir PATH] status
rmchat-probe [--core PATH] [--vault-dir PATH] models
rmchat-probe [--core PATH] [--vault-dir PATH] list [--cursor CURSOR] [--limit 1..50]
rmchat-probe [--core PATH] [--vault-dir PATH] conversation ID [--cursor CURSOR] [--limit 1..50]
rmchat-probe [--core PATH] [--vault-dir PATH] send --model ID [--conversation ID --parent ID] [--pdf PATH]
rmchat-probe [--core PATH] [--vault-dir PATH] upload PATH
rmchat-probe [--vault-dir PATH] logout
```

Sans `--core`, le lanceur cherche `rmchat-core` à côté de son propre exécutable.
Le core doit être fourni par chemin absolu si cette option est utilisée. Le
coffre par défaut est le sous-dossier `vault` du profil local `RePaper/rmchat`.
La limite de liste vaut 20 par défaut. Une seule commande peut utiliser le
coffre à la fois ; une seconde reçoit `BUSY`.

`status` interroge seulement l’état local du nouveau core, sans lui transmettre
le credential et sans vérifier le réseau. Il ajoute `credentialStored` et
`sessionVerifiedThisRun:false` : un coffre rempli n’est pas une preuve de
session valide. `logout` supprime la session locale sans lancer le core.

`send` lit le texte UTF-8 sur stdin, jusqu’à **128 Kio**. `--model` doit reprendre
un identifiant fourni par `models`. Pour poursuivre une conversation, fournir
ensemble `--conversation` et `--parent`, ce dernier venant de
`continuationParentId`. `--pdf` est répétable jusqu’à quatre fois : le lanceur
transfère les PDF puis envoie le message dans le **même processus core**.
La commande `upload` seule sert au diagnostic ; sa référence ne peut pas être
réutilisée dans une commande ultérieure, qui démarre un autre core.

La sortie standard contient du NDJSON : progression éventuelle puis résultat
final ou erreur. L’existence de `send` et de ces options ne prouve pas qu’un
envoi réel réussit face aux protections du service Web.

`import` lit uniquement **stdin**, jusqu’à **32 Kio**, sous forme de JSON :

```json
{"version":1,"provider":"chatgpt-web","kind":"access_token","value":"<secret>"}
```

Ce format est strict : ces quatre champs seulement, version et fournisseur
exacts, type reconnu et valeur non vide. Le C++ peut aussi convertir un objet
JSON de session `auth.session` contenant `accessToken` en ce format ; les autres
données de cet objet ne sont ni conservées ni transmises. Sur Unix, `import`
refuse un stdin connecté directement au terminal : il attend un flux redirigé
pour éviter d’y afficher le secret. Aucun token brut dans les arguments n’est
accepté. `session_token` est l’autre type prévu ; le core
n’accepte que les types annoncés dans `credentialKinds`. `refresh_token` et le
renouvellement persistant automatique ne font pas partie de la phase 0.
Si le parcours `session_token` détecte une rotation du cookie de session, il
refuse la connexion avec `SESSION_REIMPORT_REQUIRED` au lieu de sauvegarder
un ancien cookie. Il faut alors importer un JSON de session contenant
`accessToken` ; le core ne renvoie pas le nouveau cookie au lanceur.

Pour le premier essai, ouvrir [la session ChatGPT](https://chatgpt.com/api/auth/session)
dans le navigateur déjà connecté au compte voulu et enregistrer le JSON dans
un fichier privé **hors du dépôt**. Fournir seulement son chemin pour un essai
assisté ; le contenu ne doit pas être collé dans le chat. Depuis Linux/WSL :

```sh
chmod 600 /chemin/prive/session.json
/chemin/du/prototype/rmchat-probe import < /chemin/prive/session.json
/chemin/du/prototype/rmchat-probe models
/chemin/du/prototype/rmchat-probe list
```

Pour vérifier le parcours de lecture dans **un seul processus et une seule
session HTTP**, utiliser désormais :

```sh
/chemin/du/prototype/rmchat-probe check --import-stdin < /chemin/prive/session.json
```

Cette commande conserve les cookies reçus entre les requêtes. Elle valide la
session une fois, charge les modèles, liste au plus cinq conversations et lit
au plus cinq messages de la première conversation disponible. Elle s'arrête
au premier échec, sans nouvelle tentative. Sa sortie contient uniquement les
étapes, les comptages et les diagnostics HTTP filtrés, jamais les titres,
identifiants, textes ou accès. Avec `--import-stdin`, elle ne lit ni n'écrit le
coffre ; `check` sans cette option lit le coffre existant sans le modifier.

Ces deux dernières commandes lisent les métadonnées du compte. Tester ensuite
une conversation avec son identifiant renvoyé par `list`. Les commandes `send`
et `upload` modifient le compte distant et constituent des essais distincts.
Le fichier d’import peut être supprimé après l’import réussi ; le coffre est
alors utilisé par les commandes suivantes.

## IPC version 1

Transport : JSON-RPC 2.0 en UTF-8, **un objet JSON par ligne** (NDJSON), sans
batch, sur une socket Unix accessible uniquement à son propriétaire dans un
répertoire `0700`. Une seule connexion cliente ; chaque ligne est limitée à
**1 Mio**. Les lectures sont asynchrones et une requête d’annulation reste
recevable pendant une opération réseau.

Les identifiants de requêtes sont des chaînes `ui:<entier>` uniques pendant la
connexion. Chaque requête reçoit une réponse finale, avec `result` ou `error`.
Les événements intermédiaires sont des notifications sans `id`. Les réponses
et événements d’une connexion précédente ne sont pas réutilisables.

```json
{"jsonrpc":"2.0","id":"ui:1","method":"auth.status","params":{}}
{"jsonrpc":"2.0","id":"ui:1","result":{"authenticated":false,"account":null,"capabilities":{"protocolVersion":1,"provider":"chatgpt-web","credentialKinds":[],"methods":["auth.status","auth.import","auth.logout","request.cancel"]}}}
```

`capabilities` expose `protocolVersion:1`, `provider:"chatgpt-web"`, la liste
`credentialKinds` et la liste `methods`. Ces deux listes dépendent de ce que
l’implémentation sait réellement exécuter ; elles ne sont pas une promesse de
disponibilité réseau permanente. `auth.status` est accessible sans connexion.

| Méthode | Paramètres | Résultat |
|---|---|---|
| `auth.import` | `{credential:Credential}` | `{authenticated:true,account:Account\|null,capabilities:Capabilities}` |
| `auth.status` | `{}` | `{authenticated:bool,account:Account\|null,capabilities:Capabilities}` |
| `models.list` | `{}` | `{items:[{id,name}],defaultModelId:string\|null}` |
| `conversations.list` | `{cursor:string\|null,limit:1..50}` | `{items:[{id,title,updatedAt:string\|null}],nextCursor:string\|null}` |
| `conversations.get` | `{conversationId,cursor:string\|null,limit:1..50}` | `{id,title,messages:[Message],nextCursor:string\|null,continuationParentId:string\|null}` |
| `chat.send` | `{conversationId:string\|null,parentMessageId:string\|null,clientMessageId:UUID,modelId,text,attachments:[attachmentRef]}` | `{conversationId,message:Message,continuationParentId}` |
| `files.upload` | `{path:absolute,filename,mimeType,sha256}` | `{attachmentRef,filename,mimeType,size}` |
| `request.cancel` | `{requestId}` | `{requestId,accepted:bool}` |
| `auth.logout` | `{}` | `{authenticated:false}` |

```text
Credential = {version:1, provider:"chatgpt-web", kind:"access_token"|"session_token", value:string}
Account = {id:string, displayName:string|null}
Message = {
  id:string, parentId:string|null, role:"system"|"user"|"assistant"|"tool",
  text:string,
  attachments:[{id:string,filename:string|null,mimeType:string|null}]
}
```

`auth.import` valide le candidat en réseau avant de remplacer le compte en
mémoire et ne renvoie jamais le credential. Un échec conserve le compte
précédent. Les informations de compte et de modèle doivent venir du service,
sans nom d’abonnement, modèle ou identité inventé.
Une validation d’`access_token` par les modèles peut réussir sans renseigner
l’identité : `account` vaut alors `null`. Le parcours `session_token` peut
fournir l’identité renvoyée par le service, si elle est présente.

Les curseurs sont opaques. `conversations.get` présente la branche active
normalisée ; `continuationParentId` vient du fournisseur. Une limite de taille
produit une erreur explicite plutôt qu’un message silencieusement tronqué.

`files.upload` accepte en phase 0 un PDF régulier de **64 Mio maximum**, situé
dans une racine autorisée par `--upload-root`. Le core vérifie le chemin, le
type et l’empreinte SHA-256 avant envoi. Le contenu du fichier ne passe pas dans
une ligne JSON. L’`attachmentRef` retournée appartient au compte et au core
courants ; une référence inconnue est refusée. L’export de carnets reMarkable
n’est pas fourni par cette opération.

## Progression, annulation et erreurs

Pendant `chat.send`, le core émet au plus environ un instantané du texte
accumulé toutes les 300 ms. La réponse finale contient le message complet.

```json
{"jsonrpc":"2.0","method":"chat.progress","params":{"requestId":"ui:8","sequence":1,"conversationId":null,"messageId":null,"text":"Pour commencer…"}}
```

`sequence` croît pour cette requête. Les identifiants restent `null` jusqu’à
confirmation par le fournisseur. Après annulation, terminaison ou fermeture
de la connexion, le lanceur ignore les notifications tardives.

`request.cancel` acquitte la demande d’annulation. La requête ciblée reçoit
encore sa propre réponse finale, généralement `CANCELLED`. Une seule opération
de génération est active à la fois ; les demandes incompatibles reçoivent
`BUSY`. Une annulation ou une perte réseau ne prouve pas que le fournisseur a
supprimé le message : **aucun envoi interrompu n’est réémis automatiquement**.
`clientMessageId` permet la corrélation, sans garantir l’idempotence distante.

```json
{"jsonrpc":"2.0","id":"ui:8","error":{"code":-32000,"message":"Reconnectez ce compte dans le navigateur.","data":{"kind":"WEB_AUTH_REQUIRED","retryable":false}}}
```

Les erreurs applicatives emploient `code:-32000` et
`data:{kind:string,retryable:bool}`. Types communs : `AUTH_REQUIRED`,
`AUTH_INVALID`, `UNSUPPORTED_CREDENTIAL_KIND`, `WEB_AUTH_REQUIRED`,
`RATE_LIMITED`, `NETWORK_ERROR`, `UPSTREAM_ERROR`, `RESPONSE_TOO_LARGE`,
`CANCELLED`, `BUSY`, `OUTCOME_UNKNOWN`, `SESSION_REIMPORT_REQUIRED`,
`UPSTREAM_FORBIDDEN`, `UNEXPECTED_HTML`. Les erreurs JSON-RPC standard restent
réservées à la syntaxe, aux méthodes inconnues et aux paramètres invalides.

Un challenge explicitement annoncé produit `WEB_AUTH_REQUIRED`. Un HTTP 403
sans signal explicite produit `UPSTREAM_FORBIDDEN` et ne permet pas d’affirmer
qu’un CAPTCHA ou une reconnexion résoudra le problème. Une réponse HTML 2xx
inattendue produit `UNEXPECTED_HTML` ; HTTP 401 et 429 restent respectivement
`AUTH_INVALID` et `RATE_LIMITED`, même si leur corps est HTML.
Les erreurs HTTP comportent seulement `httpStatus`, `contentType` normalisé
et `challenge` ; aucun corps, cookie, URL ou en-tête brut n’est transmis.
`challenge:false` signifie seulement qu’aucun signal explicite n’a été observé.
Les marqueurs JSON `challenge:false` ou `required:false` ne bloquent plus un
résultat valide. Aucun solveur de protection n’est intégré.
Un accès aux modèles ou à l’historique ne démontre
pas que `send` est disponible. Les tests locaux ne constituent pas une preuve
de création ou de reprise effective d’une conversation dans ChatGPT Web.

## Validation et suites ultérieures

État après correction du diagnostic, le 6 septembre 2026 : **47 cas C++**,
**102 cas Go Linux x86_64** et **102 cas Go ARM64 sous QEMU** passent.
Les contrôles IPC entre les vrais
exécutables passent également, sans compte.

Le premier essai réel a reçu `WEB_AUTH_REQUIRED` dans la version 0.1.0. Ce
classement était trop large : cette réponse seule ne démontrait pas un challenge.
Un diagnostic ultérieur de la même requête a reçu HTTP 200 JSON, sans signal
de challenge. Après correction, **l’import réel a réussi** et le credential a
été enregistré dans le coffre. La validation de session de la commande suivante
a reçu **HTTP 403 HTML avec un en-tête de challenge explicite**.

L’acceptation du client est donc intermittente dans ces essais. La cause exacte
de cette alternance n’est pas établie. La liste de conversations n’a pas été
chargée ; aucun message ni PDF n’a été envoyé. Le JSON de session et les données
du compte ne figurent pas dans les sources ou les paquets de test.
L'accès durable reste **bloqué**, malgré les tests locaux réussis : l'interface
tablette ne doit pas être annoncée comme fonctionnelle sur cette base.

Nouvelle vérification du 6 septembre 2026 à 11:51 UTC, après correction de la
continuité HTTP : **68 cas C++**, **111 cas Go Linux x86_64** et **111 cas Go
ARM64 sous QEMU** passent. Les cookies sont désormais conservés en mémoire
dans une session isolée ; un import refusé préserve la session précédente,
la déconnexion purge les cookies et le stockage PDF utilise un client sans
cookies ni bearer. Une relecture indépendante a validé ces changements.

L'essai réel unique avec `check --import-stdin` a cependant reçu **HTTP 403,
HTML, challenge explicite**, dès `auth.import`. Aucune conversation n'a été
chargée et aucun message ou PDF n'a été envoyé. Le test ne modifie pas le
coffre existant. Aucun essai réel ARM ni déploiement sur tablette n'a suivi ce
refus. Le transfert PC vers tablette et l'utilisation autonome ne sont donc
**pas validés** ; les corrections ne sont pas publiées comme application
fonctionnelle. Les archives 0.1.0 et 0.1.1 restent inchangées.

Précision après lecture du code de la [PR Aurora #278](https://github.com/aurora-develop/aurora/pull/278) :
le helper nommé `NewStdClient()` emploie lui-même `tls-client` avec un profil
de navigateur au commit fusionné ; il n'est pas équivalent à notre transport
Go `net/http`. La PR conserve également le cookie renouvelé, alors que notre
protocole refuse cette rotation. Sa présence dans le snapshot de référence
ne signifie donc pas que notre extraction l'a reprise entièrement. Les essais
ci-dessus évaluent RMChat, pas l'authentification complète d'Aurora. Ils
n'établissent pas que cette dernière échoue dans les mêmes conditions.

La compilation native Linux demande Go 1.25+, CMake 3.21+, un compilateur C++17,
Qt 6 Core/Network/Test et OpenSSL. Depuis la racine du dépôt :

```sh
bash tools/build-rmchat-probe.sh /chemin/absolu/build
```

Le script exécute les tests Go, compile `rmchat-core` et `rmchat-probe`, lance
les tests C++, puis vérifie `status` avec un coffre de test. `GO_BIN` et
`CMAKE_BIN` permettent de sélectionner les outils installés hors du `PATH`.
Le contrôle `status` ne contacte pas ChatGPT et ne valide aucun compte.

La validation locale doit couvrir les limites stdin/IPC, le JSON strict, la
conversion de session, l’absence de secrets dans les sorties, la conservation
de l’ancien coffre après un candidat refusé, l’échec de sauvegarde, la
fermeture du core, les réponses tardives, les uploads bornés et les erreurs de
challenge. Les tests utilisent des secrets et transports fictifs. La
validation avec un compte réel doit être rapportée séparément, opération par
opération.

La suite C++ `rmchat_probe_tests` utilise son propre exécutable comme faux core
dans un processus enfant, ainsi que le véritable CLI. Elle vérifie notamment
les trames fragmentées ou invalides, l’arrêt sur EOF/délai, la conservation du
coffre après rejet, l’annulation après échec de sauvegarde, le verrou du coffre,
la distinction entre `status` et validation réseau, et le PDF envoyé dans le
même core que le message. Les données, réponses et credentials sont fictifs.

Les prochaines phases restent indépendantes : interface QML/AppLoad ; pairing
QR hybride avec endpoints et schéma propres à ChatGPT ; pièces jointes natives
et export de pages. Le relais Moodle actuel, ses données et son protocole ne
sont pas modifiés par la phase 0. Aucune réorganisation des cours, dossiers,
carnets ou fichiers de l’utilisateur n’est comprise dans ce prototype.
