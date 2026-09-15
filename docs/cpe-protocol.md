# My CPE Lyon : contrat observé

Le connecteur `reAgenda` est autonome. Il ne nécessite ni compte ni serveur
Avermate. Les faits ci-dessous proviennent de la [PR Papillon #793](https://github.com/PapillonApp/Papillon/pull/793),
figée au commit `9c6f30bc332b49a7a3df1a8e412e322298116908` le 4 septembre 2026.
La PR est encore ouverte ; cette API Aurion mobile n'est pas un contrat public
garanti par CPE. Notre adaptateur est une implémentation indépendante du protocole.

[MyCPE-Plus](https://github.com/Toinoux38/MyCPE-Plus/tree/c8ee7739e9da3f221906511b3098ca582067eb50)
sert également de référence, comparée le 5 septembre 2026 :
[client HTTP](https://github.com/Toinoux38/MyCPE-Plus/blob/c8ee7739e9da3f221906511b3098ca582067eb50/lib/data/services/api_client.dart),
[service planning](https://github.com/Toinoux38/MyCPE-Plus/blob/c8ee7739e9da3f221906511b3098ca582067eb50/lib/data/services/planning_service.dart)
et [modèle des séances](https://github.com/Toinoux38/MyCPE-Plus/blob/c8ee7739e9da3f221906511b3098ca582067eb50/lib/data/models/planning_event.dart).
Ces sources confirment les routes et les paramètres de date ; le modèle
planning ne couvre pas la variante `favori` observée dans notre cache réel.

## Authentification et transport

- Origine unique : `https://mycpe.cpe.fr/mobile/`.
- `POST login`, `Content-Type: application/json`, corps `{ "login": "…", "password": "…" }`.
- La réponse contient un jeton non vide dans `normal`. Le champ optionnel
  `comptage` n'est pas utilisé pour lire les données scolaires.
- Les routes suivantes utilisent `Authorization: Bearer <normal>` et
  `Accept: application/json`.
- Aucun endpoint de rafraîchissement, de révocation ou d'écriture n'est vérifié.
  Une session expirée impose une nouvelle connexion ; le mot de passe n'est
  jamais enregistré. Une déconnexion détruit le jeton local.
- HTTP 401/403 signifie identifiants refusés ou reconnexion nécessaire. Les
  erreurs publiques ne doivent pas reproduire la réponse distante, les secrets
  ou un objet contenant des données personnelles.
- Les redirections sont refusées afin de ne pas transmettre un Bearer à une
  autre origine. Les requêtes et les corps de réponse doivent être bornés.

Référence : [api.ts figé](https://github.com/PapillonApp/Papillon/blob/9c6f30bc332b49a7a3df1a8e412e322298116908/services/mycpe/api.ts).

## Capacités et identité

`GET configuration` retourne un objet :

- `individu.individu_id`, `prenom`, `nom` ;
- `individu.est_apprenant`, `est_intervenant` ;
- `visibilite.est_visible_mon_planning`, `est_visible_mes_notes`,
  `est_visible_mes_absences`.

Une capacité explicitement désactivée ne doit pas être interrogée. Les champs
absents ne prouvent pas qu'une capacité est désactivée. L'absence d'une identité
stable ne doit jamais conduire à fusionner silencieusement deux comptes.

Référence : [models.ts figé](https://github.com/PapillonApp/Papillon/blob/9c6f30bc332b49a7a3df1a8e412e322298116908/services/mycpe/models.ts).

## Agenda

`GET mon_planning?date_debut=YYYY-MM-DD&date_fin=YYYY-MM-DD` retourne une liste.
La fenêtre observée couvre du lundi au dimanche inclus. Les éléments ont les
champs optionnels suivants :

| Champ distant | Utilisation |
|---|---|
| `id` | Identité distante, chaîne ou nombre |
| `date_debut`, `date_fin` | Début et fin |
| `matiere` | Titre du cours |
| `type_activite` | CM/TD/TP/autre activité, titre de repli |
| `statut_intervention` | Statut brut, annulé/modifié/distanciel si reconnaissable |
| `intervenants` | Enseignant(s) |
| `ressource`, puis `salle` | Salle |
| `description` | Description complémentaire |
| `is_break`, `is_empty` | Placeholders à exclure si vrais |
| `duree` | Valeur distante optionnelle, ne remplace pas les dates |

Une réponse réelle consultée localement le 5 septembre 2026 présente une
variante absente des fixtures Papillon : `matiere` et `type_activite` sont
`null`, `ressource` est vide, et les libellés se trouvent dans `favori`.
`favori.f2` contient `libellé de séance | salle`, `f3` la matière, `f4`
l'enseignant et `f5` le type d'activité. Sans matière ordinaire, reAgenda prend
le libellé non vide avant le dernier `|`, puis `f3` (sauf le placeholder `_`),
puis `type_activite`, puis `f5`. La salle après le séparateur et l'enseignant
`f4` complètent uniquement les champs ordinaires manquants. `f1` n'est pas
utilisé pour déduire une identité. Cette variante est couverte par des tests
synthétiques ; aucune réponse personnelle n'est ajoutée au dépôt.

Les dates sans décalage UTC sont interprétées dans `Europe/Paris`, même si la
tablette utilise un autre fuseau. Les dates invalides ou les durées négatives
sont exclues. Les cours annulés restent visibles avec leur statut. Les IDs
distants sont conservés ; à défaut, une empreinte déterministe de l'événement
évite d'utiliser sa position variable dans la réponse. Une empreinte ne peut
pas garantir le suivi d'une séance déplacée sans ID distant.

La route ne prouve pas qu'une réponse représente toutes les vacances ou tous
les jours ouvrés. Aucun événement absent du cache ne doit être présenté comme
une suppression distante hors de la fenêtre effectivement rafraîchie.

Référence : [timetable.ts et tests figés](https://github.com/PapillonApp/Papillon/blob/9c6f30bc332b49a7a3df1a8e412e322298116908/services/mycpe/timetable.test.ts).

## Notes, validations et crédits

`GET mes_notes` retourne une liste de cours, ou HTTP 204 sans corps lorsqu'il
n'y a pas de notes. Chaque cours peut contenir :

- `id`, `cours_code`, `cours_libelle`, `intervenants` ;
- `inscription_cours.moyenne`, `est_validee`, `nombre_credits_obtenus`,
  `nombre_credits_potentiels` ;
- `epreuves[]` : `id`, `libelle`, `date_debut_evt`, `date_obtention`, `note`,
  `est_absent`, `est_non_noter`, `appreciation`, `intervenants`.

La virgule décimale doit être acceptée. Une valeur textuelle non numérique reste
une valeur textuelle ; elle ne devient pas zéro. La moyenne fournie par le
service et les crédits sont conservés tels quels. L'API observée ne fournit pas
de coefficient, de barème, de période scolaire vérifiée ni de moyenne générale.
Le connecteur ne doit donc pas inventer ces données ou présenter une moyenne
recalculée comme officielle.

## Absences

`GET mes_absences` retourne un objet dont `absences` peut être `null` (à
normaliser en liste vide). Les compteurs et durées fournis par le service sont :

- `nbr_total_absence_excuser`, `nbr_total_absence_non_excuser` ;
- `duree_totale_absence_excuser`, `duree_totale_absence_non_excuser`.

Une absence possède `id`, `duree`, `motif_absence.libelle`,
`motif_absence.est_excuser`, et `evenement` contenant `date_debut`, `date_fin`,
`intervenants`, `libelle_construit`. L'absence d'un booléen de justification
signifie « inconnu », pas « injustifié ».

## Validation réelle restant nécessaire

Les fixtures de la PR utilisent des données synthétiques. Le transport et la
normalisation peuvent être testés sans compte. Un cache de planning réel a été
observé localement le 5 septembre 2026 pour corriger les libellés ci-dessus.
L'expiration de jeton et l'ensemble des capacités du compte restent à valider
avec un compte CPE autorisé. Le mot de passe SSH de la tablette n'est pas
un identifiant CPE et ne doit jamais être essayé sur ce service.
