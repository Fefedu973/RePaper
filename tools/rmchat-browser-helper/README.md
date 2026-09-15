# Connexion RMChat — helper Chrome et Edge

Cette extension locale prépare `rmchat-session.json` à partir de votre session
ChatGPT, uniquement lorsque vous cliquez sur **Exporter la session**. Importez
ensuite ce fichier dans RMChat. Le fichier contient votre session en clair :
importez-le dans le coffre chiffré RMChat, puis supprimez-le.

## Installation manuelle

Le dossier à charger est **`tools/rmchat-browser-helper`**, depuis la racine
de votre copie du projet.
Aucun téléchargement de dépendances ni compilation n’est nécessaire.

Dans Chrome, ouvrir `chrome://extensions`, activer **Mode développeur**, puis
choisir **Charger l’extension non empaquetée / Load unpacked** et sélectionner
ce dossier. Épingler **RMChat — connexion locale** dans le menu des extensions.
[Procédure officielle Chrome](https://developer.chrome.com/docs/extensions/get-started/tutorial/hello-world#load_an_unpacked_extension).

Dans Edge, ouvrir `edge://extensions`, activer **Mode développeur**, puis
choisir **Charger l’extension décompressée / Load unpacked** et sélectionner
le même dossier.
[Procédure officielle Edge](https://learn.microsoft.com/en-us/microsoft-edge/extensions/getting-started/extension-sideloading).

## Utilisation

1. Cliquer sur l’extension, puis **Ouvrir ChatGPT**. Se connecter normalement au
   compte souhaité dans ce profil du navigateur.
2. Ouvrir de nouveau l’extension et cliquer sur **Exporter la session**.
3. Récupérer `rmchat-session.json` dans les téléchargements, ou à l’emplacement
   choisi si le navigateur demande où enregistrer le fichier.
4. Dans RMChat, choisir l’import d’une session et sélectionner ce fichier.
   Une fois l’import confirmé dans le coffre, supprimer le fichier exporté.

Un message de session absente, modifiée ou ambiguë empêche tout export. Se
reconnecter dans ChatGPT puis recommencer. L’extension n’échange pas elle-même
la session avec un serveur et ne prouve pas l’accès aux modèles ou conversations.
Pour retirer le helper, utiliser **Supprimer** dans la page des extensions.

## Accès et format

La seule permission est `cookies`, limitée par `host_permissions` à
`https://chatgpt.com/*`. L’API officielle permet de filtrer les lectures par nom,
URL, chemin et propriété Secure ; sans `storeId` elle utilise le magasin du
contexte courant, et ses lectures sont non partitionnées par défaut.
[Référence officielle chrome.cookies](https://developer.chrome.com/docs/extensions/reference/api/cookies).

Sur clic, le code interroge exclusivement le nom
`__Secure-next-auth.session-token` et les noms numérotés `.0` à `.16` ; `.16`
sert uniquement à refuser un dépassement. L’export accepte une base unique ou
au plus 16 chunks contigus `.0` à `.15`, du même domaine racine et magasin, au
chemin `/`, Secure et HttpOnly. Il refuse les doublons, mélanges, trous,
partitions, valeurs expirées et changements entre deux lectures. Il n’énumère
pas les autres noms de cookies.

Le fichier ne contient que `{version: 1, provider: "chatgpt-web", kind:
"session_token", value: "…"}`, avec une limite de 32 Kio pour le JSON complet.
L’export passe par un Blob et le téléchargement HTML local ; aucune permission
de gestion des téléchargements n’est demandée. Il n’y a ni serveur tiers,
analytics, accès presse-papiers, script injecté, lecture en arrière-plan, ni
enregistrement des cookies dans le stockage de l’extension.

## Vérification sans compte

Avec Node.js 20 ou ultérieur, depuis la racine du projet :

```sh
node --test tools/rmchat-browser-helper/session.test.mjs
```

Les tests utilisent uniquement des cookies fictifs et des API simulées. Ils
couvrent le format, les limites, l’ambiguïté, les permissions, la stabilité et
l’absence de lecture à l’ouverture du popup. L’extension n’a pas été installée
automatiquement et aucun cookie réel n’a servi aux tests.
