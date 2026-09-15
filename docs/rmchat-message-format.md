# Lecture des conversations ChatGPT dans RMChat

Le contenu renvoyé par le service Web contient un arbre de messages et des
métadonnées de présentation. Tous les nœuds ne sont pas des messages à afficher.
Cette adaptation concerne le service Web ChatGPT ; elle ne constitue pas une
spécification officielle de son protocole.

## Messages et branches

RMChat suit les parents depuis `current_node`, conserve cette branche dans
l’ordre chronologique, puis applique la pagination aux messages visibles.
Le parent de continuation reste celui de la branche distante, même lorsque
le dernier nœud est masqué dans l’interface.

Le texte destiné à l’utilisateur provient des rôles `user` et `assistant`,
du canal `final` ou du canal vide des anciens messages, et d’un destinataire
public. Les messages système, outils, raisonnement, contexte éditable, préambules
de réflexion et ceux portant `is_visually_hidden_from_conversation` sont exclus.
Le contenu textuel des parties multimodales reste lisible ; le JSON des objets
multimédias ne devient pas du texte de conversation.

## Flux progressif

Le parseur RMChat conserve l’état JSON d’un message et applique les snapshots,
les mises à jour de champs et les lots de modifications avant de décider quoi
afficher. Il tient compte des métadonnées de visibilité et change d’état lorsqu’un
nouvel identifiant de message arrive. Les messages anciens sans canal explicite
attendent leur terminaison avant d’être publiés, pour ne pas exposer un fragment
avant l’arrivée de sa classification.

## Citations et présentation

Les objets `content_references` associent un marqueur du texte à un titre et une
URL, directement ou dans `items`, `sources` ou `alt`. RMChat transforme ces
références en liens Markdown HTTPS. Il reconnaît aussi les citations anciennes
avec offsets, en validant que la plage désigne bien un marqueur complet. Une
référence sans URL disponible reste indiquée par `[source]`.

Les délimiteurs privés de présentation ne sont pas affichés. Une citation
incomplète pendant le flux attend le fragment suivant. Le texte situé dans les
blocs de code ou les segments entre accents graves est conservé. Les objets
interactifs non pris en charge reçoivent un libellé simple, sans exposer leur
charge JSON. Aucun média ni aucune URL ne sont chargés par le moteur de rendu.

Qt compose ensuite le Markdown et MicroTeX les formules LaTeX. Les polices sont
initialisées avant le premier document pour éviter qu’un premier chargement
épuise le temps réservé au traitement des formules. Les limites de temps et de
mémoire restent appliquées au contenu distant.

## Sources consultées le 6 septembre 2026

- [Schéma observé des exports ChatGPT, dmarx](https://gist.github.com/dmarx/08afeb669cdc2f974d6aca61dcce360d) : champs du message, visibilité et références.
- [Types des conversations, sanand0/openai-conversations](https://github.com/sanand0/openai-conversations/blob/main/conversation.ts) : variantes de contenus et structures de citations.
- [Références et marqueurs privés, pionxzh/chatgpt-exporter](https://github.com/pionxzh/chatgpt-exporter/issues/259) : exemples de liens associés au texte et délimiteurs de présentation.
- [Format des événements du service Web, lowkruc/chatgpt2api](https://github.com/lowkruc/chatgpt2api/blob/master/docs/upstream-sse-conversation.md) : snapshots et mises à jour progressives.

Ces sources sont des observations et implémentations publiques, pas un contrat
OpenAI garantissant la stabilité du service Web. Les fixtures locales vérifient
ces cas de lecture ; elles ne prouvent pas qu’un compte réel peut envoyer un
message. Le HTTP 403 observé sur le compte reste un problème distinct.
