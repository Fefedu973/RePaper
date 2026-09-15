# Connexion reMoodle

Portail public de liaison par QR/code temporaire. Il lit **localement dans le navigateur** une capture du QR officiel de connexion Moodle ou la caméra. Aucun mot de passe, token Moodle ou fichier image n'est envoyé au relais. La tablette échange directement la clé QR à usage unique auprès de Moodle, puis conserve le token dans son coffre local.

## Parcours

1. reMoodle crée une clé RSA 2048 éphémère, demande une session et affiche son QR ainsi qu'un code à huit chiffres.
2. Le portail associe ce code à un navigateur, une seule fois.
3. L'utilisateur ouvre son profil Moodle `/user/profile.php`, s'authentifie normalement, affiche le QR de la section Application mobile, puis choisit une capture dans le portail ou utilise la caméra.
4. Le navigateur vérifie le domaine et le chemin Moodle, chiffre `{kind:"qr",userid,qrloginkey}` en RSA-OAEP SHA-256, puis transmet le ciphertext au relais. L'identifiant attendu de la session est vérifié, y compris entre onglets.
5. La tablette récupère le ciphertext avec son secret dédié. Elle déchiffre, échange directement la clé QR contre un token Moodle, puis vérifie l'identité avant de conserver la connexion.

Le QR Moodle doit autoriser une connexion (`qrcodetype=2`). CPE annonce cette capacité ; aucun compte étudiant réel n'a encore été utilisé pour valider l'échange. Moodle peut lier la clé à l'adresse IP du navigateur : garder la tablette et le navigateur ayant affiché le QR derrière la même sortie Internet, généralement le même Wi-Fi sans VPN différent. Une IP IPv6 distincte peut aussi nécessiter un ajustement réseau. Sans QR de connexion activé, ce parcours est indisponible : aucun fallback caché ne prétend extraire automatiquement le lien dans tous les navigateurs.

## Stockage et expiration

D1 conserve temporairement le domaine Moodle, la clé publique, les empreintes SHA-256 des secrets, le code et le ciphertext. Il n'a pas la clé privée. Les lectures expirent strictement après dix minutes. L'acquittement ou l'annulation tablette supprime la session ; les lignes expirées restantes sont purgées lors des créations suivantes. Le navigateur possède un cookie HttpOnly/SameSite=Lax/Secure, le secret tablette ne figure pas dans les URLs. Créations et essais de codes sont limités. Le ciphertext est récupérable jusqu'à acquittement pour tolérer une réponse réseau perdue ; cela ne rend pas réutilisable la clé QR Moodle.

Aucun protocole navigateur spécial, compagnon ou extension Moodle n'est nécessaire. Le portail ne contacte pas Moodle côté serveur. La caméra est optionnelle ; les captures PNG/JPEG/WebP sont prises en charge sans BarcodeDetector.

## Développement

Node 22.13+ et npm. `npm install`, `npm test`, `npm run typecheck`, `npm run dev`, `npm run build`. Schéma dans `db/schema.ts`, migrations Drizzle versionnées dans `drizzle/`. Le relais utilise D1 via des requêtes préparées ; pas de création de schéma au runtime. Tests avec SQLite réel et WebCrypto : expiration, concurrence de codes, secrets, limites, QR associé au bon Moodle, chiffrement et reprise de transmission.

Dans un clone public, `npx --yes npm@11.6.2 ci` réinstalle les versions verrouillées avec la version npm validée ; npm 10 peut rejeter ce verrou de dépendances optionnelles. La configuration privée `.openai/hosting.json` est optionnelle et n'est pas distribuée : sans elle, Vite utilise une base locale D1 liée à `DB` et désactive l'intégration de déploiement Sites. Appliquer les migrations avant d'utiliser le portail. Pour votre déploiement, fournir votre base D1, adapter `PUBLIC_ORIGIN` dans `lib/protocol.ts` et configurer `REPAPER_MOODLE_PORTAL_URL` dans l'application tablette. Le relais de développement historique n'est pas un service garanti par cette publication.

Les API sont définies par `lib/relay.ts`. Origine publique dans `lib/protocol.ts`. Les données de test doivent rester fictives ; ne jamais mettre de capture de QR réel, mot de passe, token ou clé privée dans les sources.

## Sources

- [Moodle QR API](https://github.com/moodle/moodle/blob/MOODLE_405_STABLE/admin/tool/mobile/classes/api.php)
- [Échange QR Moodle](https://github.com/moodle/moodle/blob/MOODLE_405_STABLE/admin/tool/mobile/classes/external.php)
- [QR dans le profil](https://github.com/moodle/moodle/blob/MOODLE_405_STABLE/admin/tool/mobile/lib.php)
- [jsQR, Apache-2.0](https://github.com/cozmo/jsQR)

Les versions React/RSC, Vinext et Vite du scaffold ont été mises à jour en réponse aux avis de sécurité détectés pendant l'implémentation. Aucune authentification réelle n'a été simulée comme réussie.
