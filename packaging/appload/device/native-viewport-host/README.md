# Géométrie de la fenêtre rePDF

`NativeViewportHost` transmet largeur, hauteur et rotation du canvas AppLoad au client `AppLoadViewport` de rePDF. Il ouvre un socket Unix privé uniquement pour `external::repdf`, sans prendre le focus ni gérer les événements tactiles. `tools/build-appload-module.sh` copie les sources et applique `pdf-responsive-window.patch` après les neuf correctifs précédents.

Le client s’identifie avec la clé QTFB. Après le message `hello` validé, le serveur envoie des messages JSON `geometry` version 1, numérotés, limités à 8 Kio, au plus toutes les 20 ms. Les dimensions et rotations sont bornées. Le client retrouve sa connexion après une interruption, avec un délai plafonné à trois secondes. Aucun contenu de document ne circule sur ce socket.

Le framebuffer partagé et le runtime LinuxFB conservent leur taille. AppLoad utilise `Stretch` pour rePDF connecté ; la matrice inverse appliquée à sa surface QML annule étirement et rotation. Cela permet un véritable changement de disposition sans réallouer le framebuffer ni relancer l’application. Le sélecteur reste enfant de la surface transformée ; les rectangles du clavier et du curseur utilisent les mêmes transformations. Les autres applications conservent leur affichage habituel.

Les tests dans `tests` relient le vrai serveur, le vrai client et le `FBController` compilé à partir du module. Ils vérifient les pixels peints, les coordonnées d’entrée et l’occlusion clavier pour quatre tailles et quatre rotations, puis les redimensionnements, reconnexions et messages invalides. Un passage visuel distinct lance le vrai rePDF/PDFium, capture son framebuffer et le peint avec `FBController`.

```sh
cmake -S packaging/appload/device/native-viewport-host/tests -B /tmp/viewport-tests \
  -DAPPLOAD_SOURCE=/path/to/patched/module/source
cmake --build /tmp/viewport-tests
ctest --test-dir /tmp/viewport-tests --output-on-failure
```

Sous Qt PC antérieur à 6.7, seule l’instruction de constructeur `setFocusPolicy` est retirée d’une copie privée de l’en-tête AppLoad. Le code de rendu et d’entrée testé reste identique. Le SDK ARM utilise l’en-tête complet.
