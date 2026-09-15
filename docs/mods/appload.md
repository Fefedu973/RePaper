# AppLoad et Paper Bridge

**Ouvrir les applications RePaper et retrouver leurs documents dans la bibliothèque native.**

AppLoad lance les applications sur la tablette. Paper Bridge assure la liaison locale entre ces applications et les fonctions natives de création, d’import et d’ouverture des documents. Les outils [reInk](reink.md), [reSelect](reselect.md) et [reStencil](restencil.md) s’installent dans un module séparé.

## Le rôle de chacun

| Composant | Fonction |
| --- | --- |
| AppLoad | Ouvre reMoodle, reAgenda, reCalc et, dans les évolutions récentes, rePDF. Gère les fenêtres et relaie les entrées. |
| Clavier natif | Permet la saisie dans les champs reMoodle/reAgenda avec le clavier reMarkable ; un clavier de secours existe. |
| Paper Bridge | Transmet aux interfaces natives les demandes de carnets et d’import PDF/images. |
| Notes reAgenda | Création et ouverture d’une note liée à un événement ou à une journée ; une nouvelle demande retrouve le même carnet. |
| Imports reMoodle | Envoie un PDF ou une image préparée en PDF dans la bibliothèque et vérifie le résultat. |

Paper Bridge communique par des sockets Unix locaux. Il démarre à la demande, réutilise son processus existant et s’arrête après quinze minutes d’inactivité. L’utilisateur n’a pas de serveur distant à configurer pour ce service ; les sources en ligne de reMoodle et reAgenda conservent leurs propres besoins de connexion.

## Utilisation

1. Ouvrez **AppLoad** depuis la barre latérale et choisissez une application.
2. Un appui simple sur reCalc ouvre le plein écran ; un appui long ouvre sa fenêtre déplaçable et redimensionnable.
3. Dans les versions comportant les boutons natifs, **Calculatrice** et **PDF en fenêtre** ouvrent directement ces outils au-dessus du carnet. Un nouvel appui retrouve la fenêtre existante.
4. Dans reAgenda, ouvrez les notes d’un événement ; dans reMoodle, demandez explicitement l’import d’un fichier. Paper Bridge intervient automatiquement.

reCalc conserve les proportions de sa fenêtre. rePDF permet de régler largeur et hauteur séparément ; son mode lecture maximise la place du document dans cette fenêtre. Ces commandes sont détaillées dans le [guide des boutons natifs](../../packaging/appload/device/calculator-toolbar/README.md).

## Disponibilité et versions

**Cette publication fournit les sources. Aucun `appload.so`, `reink-editor.so` ou paquet d’applications binaires n’est joint.** Le [guide d’installation](../INSTALLATION.md) précise les conditions de reconstruction et d’utilisation expérimentale.

Les versions ci-dessous identifient les paquets locaux documentés pendant le développement :

| Élément | Livraison documentée |
| --- | --- |
| Tablette et système | Paper Pro `ferrari`, OS `3.28.0.169`, Qt `6.10.3` |
| Prérequis | XOVI et Qt Resource Rebuilder |
| Module de base local | AppLoad rePaper **3.28.4**, fichier `appload.so` |
| Archive du paquet local de base | Applications **3.28.3**, `repaper-apps-aarch64.tar.gz` |
| Outils de page indépendants | `reink-editor.so`, socle **0.8.2** |

Ces numéros désignent les paquets RePaper, pas une mise à jour du système reMarkable. Les adaptations des boutons natifs, de rePDF et de ses fenêtres ont été ajoutées après le paquet AppLoad 3.28.4 initial. Le numéro de base ne suffit donc pas à identifier tous les ajouts. Aucun support d’autres tablettes ou versions du système n’est établi.

Le [guide AppLoad historique](../../packaging/appload/device/README.md) décrit la structure et la mise à jour de ces paquets locaux. Il ne constitue pas une offre de téléchargement binaire pour cette publication. Les données des applications sont séparées de l’archive des exécutables.

## État vérifié et limites

Les validations documentées couvrent les compilations ARM, les coordonnées du stylet, le clavier et le rendu des fenêtres sur les bancs de test. La création d’en-têtes et de titres de notes d’agenda ainsi que leur réouverture sans doublon ont aussi été contrôlées sur la tablette cible. Les boutons natifs et rePDF ont été installés ; le rendu de l’interface de rePDF a été vérifié avec son binaire installé. Cela ne constitue pas une campagne matérielle complète pour toutes les applications et chaque évolution.

Sur appareil, Paper Bridge passe par les interfaces natives AppLoad/Xochitl. Il ne remplace pas directement les fichiers du document actif. Un import est confirmé par son résultat dans l’index natif ; en cas de réponse incertaine, une reprise vérifie l’état de la même demande. Les imports PDF et images sont pris en charge ; l’import générique EPUB/RMDOC et l’insertion de scènes dans la page active ne le sont pas. L’insertion des outils reInk/reStencil emprunte leur propre module.

Le **writer de scènes et de carnets du banc PC** est un autre chemin de test. L’émulateur AppLoad n’exécute pas le firmware reMarkable et ne reproduit pas les délais ni le rafraîchissement physique de son écran. Les corrections de coordonnées ou les captures PC ne prouvent pas une latence sur tablette.

## Documentation technique et origine

Cette adaptation s’appuie sur le projet AppLoad, avec les références et licences conservées dans le dépôt. Les interfaces de RePaper, le bridge et les adaptations sont documentés séparément.

- [Paquet AppLoad et portée de sa validation](../../packaging/appload/device/README.md)
- [Cycle de vie de Paper Bridge dans AppLoad](../../packaging/appload/device/bridge-RUNTIME.md)
- [API Paper Bridge et frontière avec le banc PC](../../bridge/README.md)
- [Clavier partagé](../../shared/keyboard/README.md)
- [Visionneuse rePDF](../../apps/repdf/README.md)
- [Compilation ARM](../arm-build.md)

[Retour à RePaper](../../README.md) · [reInk](reink.md) · [reSelect](reselect.md) · [reStencil](restencil.md)
