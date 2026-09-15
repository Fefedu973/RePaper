# Disponibilité et installation

La publication initiale du **16 septembre 2026 contient les sources uniquement**. Aucun `appload.so`, `reink-editor.so`, runtime ARM ou paquet d’applications prêt à installer n’est joint. Les anciennes instructions de paquet conservées dans le dépôt décrivent des livraisons locales de développement ; elles ne désignent pas des fichiers téléchargeables dans cette publication.

Pour essayer le projet sur PC, commencez par le [guide de compilation](BUILDING.md). Les interfaces et les tests peuvent fonctionner sans tablette. Les applications connectées demandent ensuite leur propre configuration ; aucun compte ni profil personnel n’est fourni.

## Cible des intégrations natives

| Vérification | Valeur documentée |
| --- | --- |
| Modèle | reMarkable Paper Pro, `ferrari` |
| OS | `3.28.0.169` |
| Qt dans Xochitl | `6.10.3` |
| SHA-256 de `/usr/bin/xochitl` | `43a9d5d0acc5b998264c16586e11b848f3b83d2d63b5fd322b09c0977d94d3d4` |
| Prérequis | XOVI et Qt Resource Rebuilder |

Ces intégrations utilisent des interfaces privées de Xochitl. Un modèle voisin, un numéro de firmware proche ou une compilation ARM réussie ne suffisent pas à établir leur compatibilité. Le [profil natif](../extensions/reink/compatibility.json) précise les contrôles réalisés. Le SDK de compilation documenté est **5.8.203** ; son numéro ne remplace pas celui de l’OS de la tablette.

## Installation expérimentale d’un build local

Ce parcours s’adresse à une personne ayant déjà reconstruit et validé ses propres artefacts pour la cible exacte. Il ne télécharge aucun binaire. Consultez également [NOTICE](../NOTICE.md) pour les licences des composants et les limites actuelles de redistribution des modules assemblés.

1. **Identifier les artefacts.** Conservez le commit source, les versions des dépendances, les résultats des tests et les empreintes SHA-256. Après une modification d’en-tête ou une compilation ayant chevauché des modifications de source, reconstruisez dans un dossier vide avec les sources figées. Un simple succès de compilation incrémentale ne suffit pas.
2. **Préparer le retour arrière.** Fermez applications et carnets. Sauvegardez les anciens modules et, si elle change, l’archive précédente des applications **sur le PC ou dans un dossier distinct, hors de `extensions.d`**. Conservez également une sauvegarde vérifiée des données de la tablette avant les essais.
3. **Arrêter l’interface.** Dans reManager, choisissez **Stop UI**. Si le runtime ou les applications changent, arrêtez aussi `repaper-paper-bridge.service` avant leur remplacement.
4. **Remplacer seulement les composants préparés.** Placez votre `reink-editor.so` et/ou votre `appload.so` validé dans `/home/root/xovi/extensions.d/`. Gardez un seul exemplaire actif de chaque module. reInk, reSelect et reStencil sont inclus dans le même `reink-editor.so`. Les applications AppLoad et leur runtime doivent correspondre au module utilisé ; les guides de paquet historiques détaillent leur structure.
5. **Démarrer avec les mods.** Choisissez **Start UI with Mods** et contrôlez le démarrage pendant au moins **75 secondes consécutives** : Xochitl doit rester actif avec le même PID et sans augmentation de son compteur de redémarrages. Vérifiez aussi l’empreinte des fichiers copiés, le chargement des modules attendus et l’absence d’erreur fatale ou d’erreur QML propre à RePaper dans le journal.
6. **Essayer un carnet de test.** Vérifiez l’ouverture, le dessin, la sélection, l’annulation, la réouverture, la gomme et la navigation au doigt. Pour AppLoad, essayez séparément le clavier, les fenêtres et les fonctions documentaires dont vous avez besoin.

Ces commandes de lecture, dans le terminal de la tablette, aident au contrôle du démarrage ; une seule exécution ne remplace pas l’observation sur 75 secondes :

```sh
systemctl show xochitl.service -p ActiveState -p SubState -p MainPID -p NRestarts
sha256sum /usr/bin/xochitl
journalctl --no-pager -u xochitl.service --since '2 minutes ago'
```

L’affichage d’une fenêtre et le chargement d’un module ne prouvent pas à eux seuls la sauvegarde durable de toutes les modifications. La [campagne de fluidité](../extensions/reink/NATIVE_RESPONSIVENESS.md) distingue tests de l’hôte, contrôle du démarrage et retour d’usage physique.

## Revenir au code précédent

Si l’interface redémarre en boucle ou si le build échoue au contrôle, arrêtez les mods avec reManager. **Restaurez les anciens modules depuis leur sauvegarde externe à `extensions.d`** ; restaurez aussi l’ancien runtime et les anciens exécutables si vous les avez changés, après avoir arrêté Paper Bridge. Relancez ensuite l’interface et recommencez le contrôle de stabilité.

Le retour arrière porte sur le code et le runtime. **Ne restaurez pas automatiquement les documents depuis la sauvegarde** : cela pourrait écraser les annotations créées depuis. Les données d’applications et les documents vivants ne sont pas des fichiers d’installation à remplacer.

## Guides associés

- [Compiler et exécuter localement](BUILDING.md)
- [reInk](mods/reink.md), [reSelect](mods/reselect.md) et [reStencil](mods/restencil.md)
- [AppLoad et Paper Bridge](mods/appload.md)
- [Structure historique des paquets AppLoad](../packaging/appload/device/README.md)
- [Instructions historiques du module natif](../extensions/reink/README.md)

[Retour à RePaper](../README.md)
