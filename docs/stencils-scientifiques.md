# Pochoirs scientifiques reStencil

Les ajouts de septembre comprennent désormais **Racine carrée** et **Générateur de tension simple** (cercle traversé par un trait, sans polarité). La racine étend sa barre sans élargir son crochet. Les dipôles proposent une pose horizontale ou verticale et une flèche de tension facultative, solidaire du composant. Les derniers symboles et les commandes de pose restent sur la page ; [gestes et captures](design/README.md).

Ajouts préparés et vérifiés localement le 6 septembre 2026, puis installés le même jour sur demande explicite. Le chargement du module dans Xochitl a été vérifié. Le rendu des nouveaux pochoirs sur l’écran de la tablette reste à essayer.

Dans **reStencil**, rechercher **Ampoule**, **Tableau**, **Graphe à deux axes** ou **Diagramme de Bode**. Les trois derniers ouvrent leurs réglages avec aperçu. **Placer** prépare l’objet ; toucher la page ou tracer sa taille pour l’insérer. Sélectionner ensuite l’objet, ouvrir **Propriétés**, puis **Configurer le tableau ou le graphique** pour changer ses paramètres. **Appliquer** conserve le réglage ; **Annuler** dans le formulaire abandonne son brouillon. Dans la fenêtre native Propriétés, le bouton Annuler du bas conserve son rôle d’annulation de la session entière.

| Pochoir | Réglages |
| --- | --- |
| Ampoule | Icône vectorielle, couleur et taille habituelles. |
| Tableau | 1 à 30 lignes et 1 à 30 colonnes, indépendamment. |
| Graphe à deux axes | Quadrillage optionnel, durée, bornes verticales, sans courbe ou avec exponentielle, réponse du second ordre ou sinusoïde. Les axes restent à annoter à la main. |
| Diagramme de Bode | Fréquences minimale/maximale en Hz, quadrillage logarithmique, gain en dB ou phase en degrés, bornes verticales. Courbe du second ordre optionnelle. |

Le **fond blanc**, activé par défaut pour tableaux et graphes, masque la grille et les traits situés en dessous. Il reste blanc lorsque la couleur du dessin change. Le désactiver rend le pochoir transparent. La couleur et l’épaisseur des traits restent réglables ; les motifs tiretés ne s’appliquent pas à ces pochoirs.

Pour la réponse exponentielle : `y(t) = y0 + (K − y0) × (1 − exp(−t/τ))`. Régler la valeur initiale `y0`, la valeur finale `K` et la constante de temps `τ` en secondes.

Pour le sinus, choisir **Graphe → Courbe → Sinusoïde**. La forme est `y(t) = décalage + A × sin(2π f t + φ × π/180)`, avec amplitude `A`, fréquence `f` en hertz et phase `φ` saisie en degrés. La sélection initiale ajuste les bornes verticales à l’amplitude ; elles restent modifiables. La durée et la fréquence sont limitées ensemble à 32 périodes.

Le second ordre représente la réponse à un échelon unitaire du système `H(s) = K ω0² / (s² + 2 ζ ω0 s + ω0²)`. Régler le gain `K`, la pulsation propre `ω0` en rad/s et l’amortissement `ζ`. Les régimes sous-amorti, critique et suramorti sont pris en charge. Le même système sert à la courbe de Bode, avec conversion `ω = 2πf`.

Les nombres acceptent le point ou la virgule décimale et la notation exponentielle. Une valeur invalide empêche l’application. Le Bode est limité à huit décades et le graphe temporel à 32 oscillations pour conserver un tracé lisible. Les bornes choisies découpent les courbes ; agrandir l’intervalle vertical pour afficher les dépassements.

Les paramètres sont conservés à la réouverture, au redimensionnement, à la duplication et dans l’historique. Les contrôles natifs sont proposés seulement lorsque les traits correspondent encore à l’objet paramétrique reconnu.

Les tests locaux couvrent les formules, les bornes, le masque blanc, la persistance, les gestes, l’adaptateur PC et les actions de la palette. La planche `.artifacts/stencil-science/stencil-science-preview.png` utilise directement le moteur du dépôt. Le banc PC accepte `--stencil table`, `--stencil graph` ou `--stencil bode`, avec `--screenshot chemin.png` pour une capture locale.
