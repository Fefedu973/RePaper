# reCalc

**Une calculatrice avec des fractions, puissances et racines que vous éditez directement.**

reCalc affiche la formule en deux dimensions pendant la saisie. Touchez un numérateur, un exposant ou un chiffre pour le corriger, calculez, puis retrouvez vos expressions dans l'historique. Sur tablette, sa fenêtre peut accompagner le carnet actif.

## Ce que vous pouvez faire

- Saisir et modifier des fractions, puissances, racines et parenthèses avec un curseur dans la formule.
- Calculer exactement les rationnels : `0,1 + 0,2` donne `3/10`.
- Utiliser trigonométrie et fonctions inverses, logarithmes, valeur absolue, racine cubique, `π` et `e`.
- Choisir **DEG** ou **RAD** et afficher les approximations avec 6, 12 ou 15 chiffres significatifs.
- Réutiliser **Ans**, mémoriser un résultat avec **MS**, le rappeler avec **MR** et vider la mémoire avec **MC**.
- Annuler ou rétablir une édition ; restaurer une expression et son mode d'angle depuis les 200 derniers calculs.
- Copier une expression en texte ou en LaTeX, et exporter un document `.tex`.

Les calculs s'exécutent en tâche de fond. Un résultat approché porte le signe `≈`.

## Essayer une formule

1. Ouvrez **reCalc** depuis AppLoad, ou touchez **Calculatrice** dans la barre native du carnet.
2. Saisissez `1`, puis **a/b**, puis `2` : le premier nombre devient le numérateur et le second le dénominateur.
3. Choisissez **Sortir ↗**, puis `+`, puis `√`, puis `2` et `=`. Le résultat de `1/2 + √2` est affiché sous la formule.
4. Touchez le numérateur pour modifier la fraction. Utilisez les flèches pour naviguer entre les cases et dans les nombres.

La poignée AppLoad permet de redimensionner la fenêtre tout en gardant ses proportions. Le déplacement et les commandes de fenêtre appartiennent à AppLoad. Sur PC, le clavier physique permet aussi la saisie, les flèches, Entrée et les raccourcis d'annulation.

## Prérequis et limites

**Cette première publication fournit les sources, sans paquet binaire prêt à installer.** Consultez le [guide d'installation et de compilation](../INSTALLATION.md).

Le calcul fonctionne **localement, sans compte et sans réseau**. L'application peut être compilée pour PC avec Qt 6.2+, C++17 et Boost. Le mode fenêtre et le bouton de barre native demandent l'intégration AppLoad de la **Paper Pro « ferrari » sous OS 3.28.0.169**.

reCalc couvre le calcul numérique réel. Il ne comprend pas de calcul formel symbolique, de grapheur ou de nombres complexes. Les approximations utilisent la précision numérique native, pas un moteur décimal à précision arbitraire.

Les expressions et les entiers exacts sont bornés pour garder le calcul maîtrisé. Une division par zéro, un domaine invalide ou une limite atteinte produit un message en conservant la formule. L'export direct d'un calcul dans une page native n'est pas présenté comme une fonction disponible.

## Données et vérifications

Le brouillon, l'historique SQLite, les réglages et la mémoire sont stockés localement. Aucun historique n'est envoyé sur le réseau. reCalc ne modifie pas directement les documents reMarkable.

Les tests couvrent le calcul exact, la priorité des opérations, l'édition structurée, les erreurs de domaine, la persistance et 2 500 commandes aléatoires contrôlant l'intégrité de la formule. Des tests d'interface vérifient les zones tactiles, le calcul asynchrone, l'historique et la surface native redimensionnable. Ils ne constituent pas une mesure de latence physique de l'écran e-paper.

[Guide technique et compilation](../../apps/recalc/README.md) · [Retour à RePaper](../../README.md)
