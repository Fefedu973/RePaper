# Connexion Moodle par le site public

Dans reMoodle, ouvrez **Connexion**, indiquez l’adresse HTTPS de votre établissement, puis touchez **Créer un code de connexion**. Sur PC ou téléphone, ouvrez le site affiché et saisissez les huit chiffres, ou scannez le QR de reMoodle. Dans l’émulateur, **Ouvrir le site sur ce PC** ouvre directement la demande dans le navigateur Windows.

Le site vous guide vers votre profil Moodle (`/user/profile.php`). Après votre connexion habituelle à l’établissement, affichez le QR dans la section **Application mobile**, puis ajoutez sa capture au site. Vous pouvez aussi scanner ce QR avec la caméra du site si vous disposez d’un autre écran. Le QR est décodé localement : la capture n’est pas envoyée au serveur.

Le site chiffre la clé temporaire du QR pour cette demande. reMoodle la récupère automatiquement puis l’échange directement auprès de votre Moodle. La demande expire après dix minutes. Aucun lien ni jeton n’est à copier, aucun compte Avermate, compagnon ou protocole Windows n’est nécessaire.

Moodle peut imposer la même adresse IP publique pour l’affichage et l’échange du QR : utilisez le même Wi-Fi pour la tablette et l’appareil connecté au profil Moodle. Les VPN et réseaux mobiles peuvent changer cette adresse. Le QR est à usage unique ; après une erreur ou une expiration, affichez-en un nouveau. Le service mobile et la connexion QR doivent être activés par l’établissement. CPE annonce cette fonction dans sa configuration publique ; un compte étudiant réel n’a pas été testé.

Le protocole, les sources officielles et les limites sont détaillés dans [moodle-qr-login.md](moodle-qr-login.md).

## Configuration technique du PC

L’ouverture Windows est activée uniquement par `REPAPER_PC_EMULATOR=1` avec le chemin absolu `REPAPER_PC_HANDOFF_HELPER`. Le wrapper AppLoad fournit ces valeurs. Le helper `tools/pc-handoff.py` transmet une URL HTTPS au script PowerShell fixe par son entrée standard. Il n’enregistre aucune association et ne lit pas le presse-papiers. Le clavier partagé conserve son action Coller habituelle, déclenchée explicitement dans les champs éditables.

L’origine du portail est fournie par défaut dans reMoodle. `REPAPER_MOODLE_PORTAL_URL` permet de la remplacer par une autre origine HTTPS. Si le portail n’est pas disponible ou n’a pas encore été publié, reMoodle affiche une erreur ; aucun état connecté n’est simulé. La clé RSA privée, le secret de demande et la clé du QR restent en mémoire. Seul le jeton Moodle validé est conservé dans le coffre local chiffré.

Tests sans navigateur ni compte réel :

```sh
python3 -m unittest discover -s tools/tests -p test_pc_handoff.py -v
ctest --test-dir /root/repaper-build -R 'moodle-(relay|controller)-tests' --output-on-failure
```

Le helper accepte `open --validate` avec `{"url":"https://example.org/"}` sur stdin. Le script PowerShell accepte `-Action open -Validate` pour vérifier son transport sans ouvrir le navigateur.
