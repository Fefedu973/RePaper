# Connexion Moodle par QR du profil

## Verdict et état des preuves

Le parcours sans compagnon est réalisable **par partage explicite de l’image du QR Moodle**, puis échange de sa clé par reMoodle. Il ne s’agit pas d’un retour automatique du SSO vers le site public. L’utilisateur reste dans son navigateur habituel pour se connecter à Moodle ; il choisit ensuite une capture du QR dans le site reMoodle. Aucune URL ou jeton n’est à recopier et aucun script ne lit une page Moodle d’une autre origine.

Le 4 septembre 2026, la sonde publique CPE exécutée par l’agent de connexion a retourné : `wwwroot=https://e-campus.cpe.fr`, `enablewebservices=1`, `enablemobilewebservice=1`, `typeoflogin=3`, fournisseur CAS et `tool_mobile_qrcodetype=2`. Cette observation indique que le QR d’authentification est configuré. Elle ne prouve ni un abonnement Moodle, ni les permissions de ce compte, ni un échange réussi. La version exacte du serveur et la valeur privée de `qrsameipcheck` ne sont pas connues. Les sources de référence ci-dessous sont la branche officielle `MOODLE_405_STABLE`, consultée à cette date.

**Aucun QR réel du compte étudiant n’a encore été échangé dans le cadre de cette recherche.** La réussite du parcours reste à vérifier avec le compte, le navigateur et l’adresse réseau effectifs.

## Où obtenir le QR

Après connexion CAS, ouvrir [son profil CPE](https://e-campus.cpe.fr/user/profile.php), puis la section **Application mobile** et le bouton d’affichage du QR. `user/profile.php` utilise l’utilisateur courant lorsque `id` est absent ; ce lien doit donc être ouvert après authentification. Le QR vient du profil, pas de la gestion des jetons dans les préférences. Le thème CPE peut présenter les libellés différemment. [Source du profil, lignes 38–43 et 218–220](https://github.com/moodle/moodle/blob/MOODLE_405_STABLE/user/profile.php#L38).

Le callback `tool_mobile_myprofile_navigation()` ajoute le QR uniquement au propriétaire connecté de ce profil lorsque les services mobiles et l’option correspondante sont actifs. Les administrateurs et les sessions d’usurpation sont exclus. Un bouton déplie l’image PNG intégrée à la page. [Source du callback, lignes 100–134](https://github.com/moodle/moodle/blob/MOODLE_405_STABLE/admin/tool/mobile/lib.php#L100).

Le site public peut proposer **Choisir une capture du QR** avec un `input type=file` et un décodage local JavaScript. La caméra est une option supplémentaire ; elle n’est pas nécessaire sur un téléphone qui sélectionne sa propre capture. Le fichier sélectionné reste dans le navigateur et ne doit pas être envoyé au serveur. Cette utilisation du sélecteur de fichiers est prise en charge largement ; la matrice réelle Chrome/Edge/Firefox/Safari reste à tester. [Documentation du sélecteur de fichiers](https://developer.mozilla.org/en-US/docs/Web/HTML/Reference/Elements/input/file).

## Contenu du QR et durée de vie

Moodle construit la donnée encodée ainsi :

```text
<forcedurlscheme ou moodlemobile>://<wwwroot>?qrlogin=<clé>&userid=<identifiant>
```

Par exemple, la partie `https://e-campus.cpe.fr` suit le préfixe `moodlemobile://` ; ce n’est pas le jeton REST final. Avec `qrcodetype=1`, le QR ne contient que l’adresse du site et ne permet pas l’authentification. Avec la valeur `2`, il contient la clé et l’utilisateur. Chaque génération supprime la clé QR précédente de ce compte. La durée par défaut est de 600 secondes, configurable par l’administrateur. Le temps commence à la génération du QR dans Moodle, indépendamment du délai du relais public. [Construction et clé, `api.php` lignes 422–430 et 704–718](https://github.com/moodle/moodle/blob/MOODLE_405_STABLE/admin/tool/mobile/classes/api.php#L422).

**La contrainte réseau est matérielle.** `qrsameipcheck` vaut `1` par défaut : la clé peut être restreinte à l’adresse de sortie du navigateur qui a généré le QR. Téléphone/PC et tablette doivent alors utiliser la même sortie IP ; le même Wi-Fi est une consigne pratique, sans garantie si IPv6, VPN, proxy ou routage donnent des adresses différentes. Ne pas faire l’échange depuis le serveur public. La valeur CPE n’est pas exposée par la sonde publique. [Réglages, lignes 118–125](https://github.com/moodle/moodle/blob/MOODLE_405_STABLE/admin/tool/mobile/settings.php#L118), [description Moodle du contrôle réseau](https://github.com/moodle/moodle/blob/MOODLE_405_STABLE/admin/tool/mobile/lang/en/tool_mobile.php#L128).

## Échange HTTP exact par l’application

Envoyer depuis reMoodle une requête HTTPS directe vers le Moodle de la demande, avec vérification TLS et sans suivre une redirection vers une autre origine :

```http
POST /lib/ajax/service-nologin.php
Content-Type: application/json

[{"index":0,"methodname":"tool_mobile_get_tokens_for_qr_login","args":{"qrloginkey":"<clé décodée>","userid":12345}}]
```

`service-nologin.php` interdit les cookies Moodle et réutilise le répartiteur AJAX ; celui-ci accepte le tableau JSON brut du corps. Ne pas mettre la clé dans l’URL, ni utiliser `cachekey`. L’enregistrement de la fonction porte `ajax=true` et `loginrequired=false` : aucun jeton REST préalable n’est nécessaire. [Point d’entrée sans session](https://github.com/moodle/moodle/blob/MOODLE_405_STABLE/lib/ajax/service-nologin.php#L27), [répartiteur, lignes 41–78](https://github.com/moodle/moodle/blob/MOODLE_405_STABLE/lib/ajax/service.php#L41), [déclaration de service, lignes 82–90](https://github.com/moodle/moodle/blob/MOODLE_405_STABLE/admin/tool/mobile/db/services.php#L82).

La réponse de succès est un tableau contenant `error:false` et `data:{token,privatetoken,warnings}`. `token` est le véritable accès aux services mobiles. reMoodle n’a pas besoin de conserver le `privatetoken` pour lire cours, calendrier et fichiers. Une erreur Moodle peut être contenue dans un HTTP 200 sous `error:true` et `exception` : le statut HTTP seul ne suffit pas.

La fonction exige le QR d’authentification activé, un client reconnu comme application Moodle, HTTPS, un utilisateur actif non administrateur et une clé valide. Elle consomme la clé, vérifie son propriétaire puis appelle la génération standard du jeton de service mobile. Une erreur survenant après la consommation peut nécessiter un nouveau QR ; ne pas réessayer aveuglément une requête dont la réponse réseau a été perdue. [Implémentation, paramètres et retour, lignes 570–650](https://github.com/moodle/moodle/blob/MOODLE_405_STABLE/admin/tool/mobile/classes/external.php#L570).

Le validateur distingue `invalidkey` (inconnue ou déjà utilisée), `expiredkey` (délai dépassé) et `ipmismatch` (adresse réseau refusée). La clé générée dans cette branche est une chaîne hexadécimale de 32 caractères. [Validation et génération dans `moodlelib.php`, lignes 2634–2647 et 2703–2719](https://github.com/moodle/moodle/blob/MOODLE_405_STABLE/lib/moodlelib.php#L2634).

Dans cette version, la reconnaissance de l’application repose sur la présence de `MoodleMobile` dans le User-Agent, sans distinction de casse. Le client natif doit satisfaire le contrat de l’API mobile et être testé ; un `fetch` de navigateur ordinaire n’est pas un substitut pris en charge. [Détection exacte, lignes 1004–1016](https://github.com/moodle/moodle/blob/MOODLE_405_STABLE/lib/classes/useragent.php#L1004).

La génération conserve les contrôles du service : compte actif, capacité requise, utilisateurs autorisés et restrictions éventuelles. Pour créer un nouveau jeton mobile, elle vérifie notamment `moodle/webservice:createmobiletoken` ou la capacité générale admissible. Un QR configuré ne contourne pas ces permissions. [Génération de jeton, lignes 313–415](https://github.com/moodle/moodle/blob/MOODLE_405_STABLE/lib/external/classes/util.php#L313).

## Contrat proposé entre le site et la tablette

Ce paragraphe décrit notre conception, pas une fonction native de Moodle.

1. La tablette prépare une session de relais courte, sa clé publique éphémère et le Moodle attendu. L’utilisateur relie le navigateur à cette session avec le code ou le QR affiché par la tablette.
2. Le navigateur décode uniquement l’image choisie. Il valide la structure complète, un identifiant entier positif sûr, la clé bornée, l’absence de paramètres ambigus et l’origine Moodle identique à celle de la session. Il refuse un QR simple d’adresse ou celui d’un autre établissement.
3. Il chiffre avec la clé publique de cette session le petit objet `{kind:'qr',userid:number,qrloginkey:string}`. Le relais ne reçoit ni l’image ni ces champs en clair ; il transporte le ciphertext lié à la session. La clé privée reste sur la tablette. La taille doit rester compatible avec RSA-OAEP SHA-256 et la clé 2048 bits du protocole existant.
4. La tablette déchiffre et valide strictement l’objet puis appelle l’API Moodle directement. La base Moodle provient de sa demande initiale, pas du retour déchiffré. Elle confirme ensuite l’identité retournée par Moodle avant de remplacer un compte existant.
5. Le client efface le secret QR de son état temporaire et termine le relais. Une demande annulée ou expirée n’est pas réactivée par une réponse tardive.

L’interface doit annoncer honnêtement l’action de choisir une capture et la possible contrainte de réseau. Une image partagée exprès est un transfert de clé d’authentification ; elle ne doit pas être mise dans les journaux, les erreurs, les URL du portail ou un service d’analyse.

## Abonnement et configuration

Le [guide officiel des administrateurs](https://docs.moodle.org/405/en/Moodle_app_guide_for_admins#QR_Login) présente QR Login comme disponible à partir de Moodle 3.9 avec un abonnement Pro/Premium et un réglage administrateur. Le drapeau CPE `qrcodetype=2` ne prouve pas l’abonnement. Les fonctions serveur étudiées ci-dessus contrôlent la configuration, l’utilisateur et la clé ; elles ne constituent pas une attestation de l’offre commerciale du site. Si CPE n’affiche pas le QR ou refuse l’échange, traiter la fonctionnalité comme indisponible et conserver l’erreur utile ; ne pas promettre une activation par l’étudiant.

## Ce que les autres mécanismes permettent

`launch.php` accepte un nom de schéma, puis construit un retour `scheme://token=...`. Il ne fournit pas de paramètre de callback HTTPS vers un site public. L’option OAuth SSO choisit un fournisseur de connexion ; elle ne remplace pas ce retour. [Source du lancement mobile](https://github.com/moodle/moodle/blob/MOODLE_405_STABLE/admin/tool/mobile/launch.php#L27).

L’OAuth2 intégré à Moodle fait de Moodle le client d’un fournisseur d’identité. Un jeton CAS/OIDC obtenu ailleurs n’est donc pas automatiquement un jeton REST Moodle. [Documentation OAuth2](https://docs.moodle.org/500/en/OAuth2_Services#Issuer_Configuration). Le plugin serveur tiers [local_oauth2](https://github.com/enovation/moodle-local_oauth2) fournit un Authorization Code avec retour HTTPS, mais documente explicitement que ses tokens ne sont pas acceptés directement par les services Moodle ; il faut un pont vers `external_tokens`.

Pour un retour SSO entièrement automatique et indépendant des restrictions QR, un plugin Moodle `local_repaper` pourrait fournir une page `require_login()` avec consentement, une autorisation courte liée à la demande tablette et un échange serveur typé vers un jeton mobile autorisé. Cela exige l’installation/configuration par l’administrateur ou l’hébergeur, hors des droits d’un étudiant. Un flux [RFC 8628](https://www.rfc-editor.org/rfc/rfc8628) décrit bien l’expérience code/QR et interrogation par l’appareil, mais il faut un serveur d’autorisation qui l’implémente ; l’existence de la norme n’ajoute pas ce serveur à CPE. [Plugins locaux Moodle](https://moodledev.io/docs/4.5/apis/plugintypes/local), [droits et installation](https://docs.moodle.org/405/en/Installing_plugins).

## Vérifications avant d’annoncer une connexion réussie

- Tests de parseur : QR Moodle correct, QR simple URL, autre origine, paramètres répétés, mauvais utilisateur, clé trop longue et image illisible.
- Tests de transport : réponse AJAX d’erreur en HTTP 200, réponse malformée/trop grande, redirection, erreur TLS, annulation et réponse tardive.
- Tests de cycle : clé expirée ou déjà utilisée, nouveau QR invalidant l’ancien, perte de réponse après échange, refus réseau et permissions.
- Test réel CPE avec un QR du compte étudiant : sélection locale de l’image, échange direct sur le réseau prévu, contrôle de l’identité, navigation dans les cours et expiration du relais. Répéter la sélection d’image sur les navigateurs visés avant de revendiquer leur compatibilité.

Ces essais réels ne sont pas remplacés par la sonde de configuration publique ni par les tests avec réponses simulées.
