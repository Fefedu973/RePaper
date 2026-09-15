# reAgenda

Application Qt Quick autonome pour calendrier CPE Lyon et abonnements/fichiers
ICS. Aucun serveur Avermate n'est requis.

## Utilisation

1. Ouvrir **Sources**, puis **Connecter CPE** ou **Ajouter un ICS**.
2. Saisir les identifiants CPE avec le clavier tactile. Le mot de passe ne sort
   que vers `https://mycpe.cpe.fr/mobile/login` et n'est pas enregistré.
   Le jeton de session est conservé dans le coffre chiffré local `repaper`.
3. Pour ICS, fournir une URL HTTPS d'abonnement ou un chemin local `.ics`.
   L'adresse distante, qui peut contenir un jeton privé, est également protégée
   par le coffre ; le calendrier synchronisé est conservé en cache local.
4. Choisir Jour, Semaine ou Mois. Le démarrage et la navigation téléchargent
   automatiquement la fenêtre affichée après une courte temporisation. Les
   anciennes requêtes CPE sont annulées quand les semaines demandées changent ;
   **Actualiser** force une nouvelle récupération même si les données sont récentes.
5. **Note du jour** ou la fiche d'un événement demande un notebook natif à
   Paper Bridge, avec une clé idempotente. Un dialogue affiche la progression,
   puis l’éventuelle erreur. Sur appareil, Bridge crée et ouvre la note via
   l’hôte natif AppLoad ; l’identité de l’événement et la date choisie restent
   attachées à la demande même si la vue change.

La demande de note transmet aussi un contexte d’agenda versionné à l’hôte natif :
date choisie, titre, matière, début/fin avec leurs décalages UTC, fuseau nommé et
indicateur « toute la journée ». La fiche conserve les détails qui étaient
affichés à son ouverture, même si une actualisation modifie ensuite le cours.
Pour CPE, la matière vient de `matiere`, puis de `favori.f3` si ce champ est
renseigné ; le titre de l’événement sert de repli, notamment pour ICS. Une note
du jour transmet sa date sans événement ni heure inventée. L’hôte natif choisit
le dossier et la page d’agenda à partir de ce contexte. Les clés existantes
`reagenda:event:<id>` et `reagenda:day:<date>` restent identiques, afin de
retrouver les carnets déjà créés sans réinitialiser leur contenu.

Les données scolaires CPE disponibles (notes, moyenne par cours, crédits,
validation et absences) sont consultables depuis **Sources**. Les moyennes ne
sont jamais recalculées ou présentées avec un barème inventé.

Une déconnexion supprime le jeton et conserve le cache. **Retirer le calendrier**
retire ses données locales et, pour CPE, ses notes et absences. Les fichiers
créés dans Xochitl ne sont pas supprimés par cette action.

## Compilation et validation sur PC

Qt 6.2+, OpenSSL, CMake 3.21+ et un compilateur C++17 sont nécessaires.

```sh
cmake -S apps/reagenda -B build/reagenda -DCMAKE_BUILD_TYPE=Debug
cmake --build build/reagenda -j4
ctest --test-dir build/reagenda --output-on-failure
build/reagenda/reagenda
```

Le programme peut également être compilé depuis le CMake racine. En mode
desktop, `--view day|week|month` et `--date YYYY-MM-DD` choisissent la première
vue. Pour la validation visuelle, `--screenshot image.png` enregistre la fenêtre
et quitte ; `--screenshot-page login|ics|sources` ouvre le dialogue voulu.
Sous WSL, le rendu validé utilise `QT_QPA_PLATFORM=xcb QT_QUICK_BACKEND=software`.

Les tests vérifient les contrats CPE, les heures françaises hiver/été, les
all-day à fin exclusive, les récurrences et leurs exceptions, les IDs stables,
le rejet des dates invalides et les limites d'expansion. Les tests du contrôleur
vérifient aussi la temporisation, les requêtes obsolètes, la déduplication par
semaine, l’actualisation forcée et la déconnexion pendant une requête. Les tests
QML cliquent le bouton Notes et vérifient le dialogue de progression et d’erreur.
Le contexte des notes est aussi testé après une actualisation concurrente,
pendant un changement d’heure et pour les événements à la journée.

## Limites explicites

- CPE utilise l'API observée dans la [PR Papillon #793](https://github.com/PapillonApp/Papillon/pull/793),
  commit `9c6f30bc332b49a7a3df1a8e412e322298116908` ; voir
  [le contrat détaillé](../../docs/cpe-protocol.md). Aucun test avec un compte
  CPE réel n'a été exécuté pendant l'implémentation.
- Les requêtes CPE sont hebdomadaires. Le renouvellement automatique du jeton
  n'est pas documenté : HTTP 401/403 demande une reconnexion.
- Les flux ICS prennent en charge `DAILY`, `WEEKLY`, `MONTHLY`, `YEARLY`,
  `INTERVAL`, `COUNT`, `UNTIL`, `BYDAY`, `BYMONTHDAY`, `BYMONTH`, `EXDATE`,
  `RDATE` et les remplacements/annulations `RECURRENCE-ID` ordinaires. Les
  clauses non prises en charge, `RANGE`, les fuseaux non IANA et les règles
  trop complexes produisent un avertissement visible. Ce n'est pas une
  implémentation intégrale de RFC 5545.
- Les événements flottants et CPE utilisent `Europe/Paris`. Les instants
  explicitement décalés sont convertis dans ce fuseau pour l'affichage.
- CalDAV et Google OAuth directs ne sont pas encore implémentés. Les liens
  HTTPS ICS de Google ou d'autres services fonctionnent en lecture seule.
- Requêtes bornées à 30 secondes et 8 Mio, 12 sources, cache total 24 Mio,
  affichage 20 000 événements et expansion bornée. Les redirections HTTPS sont
  refusées ; il faut fournir l'adresse finale du flux.
- Le coffre protège les secrets au repos ; il ne protège pas contre un compte
  root compromis. Les données du calendrier, notes et absences restent dans un
  cache local privé au système de fichiers.
