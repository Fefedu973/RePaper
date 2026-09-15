# Paper Bridge

Service C++/Qt local de création de documents. Les apps utilisent `repaper::BridgeClient`
et un socket Unix ; le service n’expose ni SSH, ni commande shell arbitraire,
ni écoute TCP. Les contrats et le cycle de vie sont testés localement ; la
validation du paquet sur tablette est une étape distincte.

```sh
mkdir -p "$HOME/repaper-emulator/state/inputs"
"$HOME/repaper-build/bridge/paper-bridge" \
  --sandbox "$HOME/repaper-emulator" \
  --socket "$HOME/repaper-emulator/core.sock"
```

Les apps doivent recevoir `PAPER_BRIDGE_SOCKET` avec ce chemin et placer leurs
exports/cache sous `state/inputs`. Le lanceur AppLoad le configure automatiquement.

## Contrat actuel

| Route | Fonction |
|---|---|
| `GET /v1/health` | Santé et version du protocole. |
| `GET /v1/capabilities` | Capacités disponibles ou explicitement indisponibles. |
| `GET /v1/documents` | Liste bornée des métadonnées documentaires. |
| `POST /v1/imports` | Import natif d’un PDF ou d’une image sur appareil ; scènes en sandbox. |
| `POST /v1/notebooks` | Création idempotente d’un notebook vierge. |
| `POST /v1/documents/<id>/open` | Ouverture par l’hôte natif AppLoad sur appareil. |
| `GET /v1/operations/<id>` | Lecture d’une opération du writer de test journalisée par Bridge. |

Les requêtes utilisent HTTP/1.1, `Content-Length` et un objet JSON (y compris
`{}` pour GET). Pas de chunked, pipelining ou upload HTTP de fichier. Les
requêtes restent sous 64 Kio ; le contenu est lu depuis un chemin canonique
autorisé, puis vérifié par SHA-256, avec une limite de 64 Mio.

Corps d’import : `path`, `displayName`, `sha256`, `idempotencyKey`. Le fichier
doit être un export/cache propre à l’app. Les liens symboliques sont refusés.
Pour un notebook : `displayName`, `idempotencyKey`. Le client ajoute aussi
`callerQtfbKey` quand il tourne dans AppLoad, pour la création et l’ouverture.
Une même clé restitue le
résultat précédent ; une révision différente sur la même clé d’import donne
`IDEMPOTENCY_CONFLICT`. Les erreurs sont des objets `error` avec `code`,
`message` et `retryable` ; l’actuel transport retourne HTTP 400 pour ces erreurs.

Une demande de carnet peut ajouter un objet `agenda` sans modifier la clé de
création. Un ancien client sans cet objet garde le contrat précédent. Le
contexte d’un événement a cette forme :

```json
{
  "schemaVersion": 1,
  "kind": "event",
  "date": "2026-09-07",
  "timeZone": "Europe/Paris",
  "event": {
    "id": "mycpe:course-17",
    "title": "Travaux dirigés",
    "subject": "Électronique",
    "start": "2026-09-07T09:00:00+02:00",
    "end": "2026-09-07T10:30:00+02:00",
    "timeZone": "Europe/Paris",
    "allDay": false
  }
}
```

Pour `kind: "day"`, `event` est absent : aucune heure n’est fabriquée. Bridge
borne le contexte à 8 Kio, vérifie les types, les dates, les fuseaux et la
cohérence de leurs décalages UTC, puis transmet l’objet à l’hôte natif, qui le
revalide et choisit le dossier et la page. L’identifiant, le titre et la matière
sont limités à 512 caractères chacun, le fuseau à 128 et les horodatages à 40.
Les mêmes clés retrouvent les carnets existants ; ce contexte ne demande pas
de réinitialiser une note déjà créée.

## Préparation des imports natifs

PDFio lit le véritable arbre de pages PDF. Les images sont contrôlées avant
décodage (dimensions, pixels et allocation), puis centrées dans un PDF. Le
fichier source doit se trouver sous `/home/root/.local/share/RePaper/`, sans
lien symbolique ni chemin traversant un lien. Le PDF contrôlé, son empreinte et
le reçu de préparation sont écrits durablement sous le dossier privé
`/home/root/.local/share/paper-bridge/native-imports/<empreinte-clé>/`.
Les dossiers ont le mode 0700 et les fichiers 0600. Une nouvelle tentative
avec la même clé et le même contenu réutilise exactement le PDF préparé ; un
changement de contenu ou de titre avec cette clé est refusé. Le fichier source
doit encore être disponible pour vérifier la nouvelle tentative.

Bridge transmet le chemin de ce PDF, son SHA-256, le titre et la clé à
`POST /v1/imports` de l’hôte natif AppLoad. L’hôte est responsable de l’import,
de l’idempotence et de la confirmation dans la bibliothèque Xochitl. Bridge
attend au plus 100 secondes sa réponse. L’import ne demande pas d’ouverture
automatique du document.

## Writer de test et transactions existantes

Une scène `.paper-scene.json` contient `schemaVersion: 1`, une page 1404×1872
et des traits noirs composés de `width` et `points: [[x,y], ...]`.

Le writer produit des blocs .rm v6 avec un calque et des traits natifs. Son
périmètre est volontairement étroit : pas de texte riche, couleurs, assets
embarqués ou conversion générique d’un fichier RMDOC. Les symboles et dessins
restent vectoriels ; le parser indépendant `rmscene` vérifie leur lecture.

Une transaction crée un nouvel UUID. Elle prépare tous ses fichiers sur le
même filesystem, écrit un manifeste durable, journalise la phase, arrête les
services nécessaires hors sandbox, installe les fichiers et ne restaure que
les services précédemment actifs. Un rollback ne vise que les entrées de son
nouvel UUID. La phase committée `verifying` est conservée en récupération, afin
de ne jamais supprimer d’éventuelles modifications ultérieures de l’utilisateur.

Les essais `bridge/tests/integration.py` couvrent la panne après le premier
fichier installé, l’idempotence, les entrées refusées, le transport réel et
la suspension/reprise après un changement de validation. L’option de test
`--sandbox-unvalidated` ne fonctionne qu’avec `--sandbox` et retire des capacités.

## Frontière appareil

Sans `--sandbox`, les nouveaux imports PDF/images, la création et l’ouverture
des carnets passent exclusivement par l’hôte natif AppLoad. Les capacités
reflètent sa disponibilité. Ce parcours n’utilise ni l’attestation du writer,
ni ses écritures directes dans les fichiers documentaires. Aucune attestation
n’est fournie avec le projet. La récupération d’anciennes transactions du
writer conserve son verrou de validation modèle/firmware/aller-retour/rollback.

L’hôte écoute sur `/run/repaper-appload/documents.sock`. Une création ou un
import n’est accepté qu’avec un UUID valide, `status: succeeded` et
`nativeIndexVerified: true` ; l’ouverture doit confirmer `status: opened`.
Une réponse incertaine ne déclenche aucun repli vers le writer. L’insertion dans
la page active et l’import de scènes sur appareil restent indisponibles. En
sandbox, imports et carnets utilisent le writer de test et l’ouverture native
est indisponible ; leurs résultats portent `nativeIndexVerified: false`.

`--api` permet un processus proxy distinct vers le socket du worker privilégié.
Le paquet appareil utilise `--device-service` et le socket
`/run/paper-bridge/core.sock`. Son helper `bridge-start` crée à la demande une
unité systemd transitoire indépendante de Xochitl, vérifie sa santé et réutilise
le worker actif. Le service s’arrête après quinze minutes d’inactivité ; le
client le relance au besoin. Aucun fichier unité ni démarrage permanent n’est
installé. Voir le [contrat du runtime AppLoad](../packaging/appload/device/bridge-RUNTIME.md).

`bridge/tests/runtime_integration.py` vérifie ce cycle de vie avec de vrais
processus Bridge et un faux gestionnaire de services, ainsi que le contrat d’un
faux hôte natif. Il couvre aussi PDF/image, les fichiers et réponses refusés,
l’idempotence, la reprise après délai dépassé et l’émission unique du résultat
au client. Ces tests ne contactent pas la tablette.
