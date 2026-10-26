# CarPolo 3D 🚙 — la C4 Cactus sur le vrai trajet

Jeu de conduite en 3D écrit en **C avec raylib 5.5**, pour Android (APK), PC (Windows, macOS, Linux) et
**navigateur web** (le même moteur compilé en WebAssembly).
Tu pilotes une **Citroën C4 Cactus phase 2 « Bleu Émeraude »** (modèle 3D fait sur mesure d'après les photos)
sur le **vrai trajet** : 2000 chemin départemental 15 (La Pécette, Cornillon-Confoux) → Les Grandes Bastides →
Lançon-Provence → route de Pélissanne → arrivée près de l'A54 (12,5 km).

But : arriver **le plus vite possible sans accident**.

## Jouer dans le navigateur

**https://wakadu13.github.io/CarPolo/** — rien à installer (Chrome, Firefox, Edge, Safari ; sur téléphone,
tourne l'écran en paysage, les commandes tactiles s'affichent). Publié automatiquement par GitHub Actions
(`.github/workflows/web3d.yml`), qui dépose aussi le site complet dans la release `web3d-latest`
(`CarPolo3D-web.zip`, à servir avec un serveur web local, par exemple `python3 -m http.server`).
La première version du jeu (cartoon) reste jouable sur https://wakadu13.github.io/CarPolo/cartoon/.

## Télécharger pour PC (Windows, macOS, Linux)

Release `desktop3d-latest`, reconstruite automatiquement par GitHub Actions :

| Système | Fichier |
|---|---|
| Windows 10/11 64 bits | [CarPolo3D-windows-x64.zip](https://github.com/wakadu13/CarPolo/releases/download/desktop3d-latest/CarPolo3D-windows-x64.zip) — décompresser, lancer `CarPolo3D.exe` (SmartScreen : « Informations complémentaires » → « Exécuter quand même ») |
| macOS 11+ (Apple Silicon et Intel) | [CarPolo3D-macos.zip](https://github.com/wakadu13/CarPolo/releases/download/desktop3d-latest/CarPolo3D-macos.zip) — glisser « CarPolo 3D.app » dans Applications, puis clic droit → Ouvrir (app non notarisée) |
| Linux 64 bits | [CarPolo3D-linux-x64.tar.gz](https://github.com/wakadu13/CarPolo/releases/download/desktop3d-latest/CarPolo3D-linux-x64.tar.gz) — `tar xzf …` puis `./CarPolo3D/carpolo3d` |

Sur PC : clavier (flèches/ZQSD/WASD, H, C, P, **F11 plein écran**) ou manette ; graphismes « élevé » par défaut.
Le record est enregistré dans le dossier utilisateur (`%APPDATA%\CarPolo3D`, `~/Library/Application Support/CarPolo3D`, `~/.local/share/carpolo3d`).

## Installer sur Android

Télécharge **CarPolo3D.apk** sur le téléphone :
`https://github.com/wakadu13/CarPolo/releases/download/apk3d-latest/CarPolo3D.apk`
puis ouvre-le (autorise l'installation depuis cette source si Android le demande).
L'APK est reconstruit automatiquement par GitHub Actions à chaque modification de `game3d/`.

## Commandes

| | Tactile | Clavier |
|---|---|---|
| Tourner | ◀ ▶ en bas à gauche | ← → |
| Gaz / frein (et marche arrière) | pédales GAZ / FREIN | ↑ / ↓ |
| Klaxon | bouton violet | H |
| Changer de vue (poursuite, éloignée, capot) | bouton caméra | C |
| Pause | bouton ⏸ ou retour Android | P / Échap |

- **Aide à la direction** (activée par défaut, désactivable dans le menu) : la voiture suit les virages, tu gères
  la vitesse, les dépassements et la trajectoire. Si tu arrives trop vite dans un virage ou un giratoire,
  les pneus crissent et la Cactus part tout droit.
- **Permis à 12 points** : chaque accident (voiture, arbre, mur, poteau, glissière) coûte 3 points. À 0 : permis annulé.
- **Circulation réelle** : les autres voitures respectent les limitations du tracé (50 en agglomération, 30 dans
  les giratoires, 80/90 sur route) ; celles d'en face freinent et klaxonnent si tu es dans leur voie.
- **Graphismes léger/normal/élevé** : réglage dans le menu (léger pour les téléphones modestes).
- **Rendu** : textures procédurales (herbe, garrigue, calcaire, bitume, gravier, enduits, tuiles canal,
  écorce) générées au lancement, arbres faits d'un tronc texturé et de « cartes » de feuillage (pins
  d'Alep, chênes, oliviers, cyprès, platanes), pack de modèles low-poly (`assets/trees.bin` + `trees.png`,
  construit par `tools/treepack.py` à partir du pack d'arbres fourni) pour les arbres isolés près de la
  route, les arbres des villages, les rochers et les arbres morts de la garrigue ; ombres portées du
  soleil (qualité « élevé »), brume atmosphérique jusqu'à l'horizon (30 km).

## D'où viennent les données

Google Maps et OpenTopoMap n'étaient pas accessibles depuis l'environnement de développement ; les mêmes données
de base ont été prises à la source (`tools/world/`, voir son README) :

- **Tracé et routes** : OpenStreetMap via Overture Maps — itinéraire recalculé sur le graphe routier réel et calé
  sur la capture Google (écart moyen ≈ 11 m, 12,5 km contre 12,6 km annoncés). Le trajet longe l'A7 sans l'emprunter.
- **Relief** : EU-DEM (tuiles Terrarium), grille de 10 m ; horizon sur 40 × 40 km (étang de Berre, Durance…).
- **Occupation du sol** : ESA WorldCover 10 m + OSM (vignes, oliveraies, forêts de pins, garrigue, canal EDF…).
- **Bâtiments** : 5 387 empreintes réelles (OSM/Overture), murs ocre, tuiles, volets.
- **Arbres cartographiés** : platanes, pins, oliviers ; limitations de vitesse et noms de rues d'OSM.

## Compiler

PC (Linux, macOS, Windows avec MinGW ou MSVC) :
```sh
cmake -S game3d -B game3d/build -DCMAKE_BUILD_TYPE=Release   # récupère raylib 5.5 automatiquement
cmake --build game3d/build -j && ./game3d/build-bin/carpolo3d
```
Windows depuis Linux : `-DCMAKE_TOOLCHAIN_FILE=game3d/desktop/mingw-w64-x86_64.cmake` (paquet `mingw-w64`).
Web (Emscripten ≥ 3.1.25 recommandé) : `emcmake cmake -S game3d -B build-web -DCMAKE_BUILD_TYPE=Release && cmake --build build-web`
→ `game3d/build-web-bin/index.{html,js,wasm,data}` ; test automatique : `node game3d/web/smoke.js game3d/build-web-bin`.
Archives joueurs : `game3d/desktop/package.sh windows|linux|macos <exécutable> <dossier>` (voir `.github/workflows/desktop3d.yml`).
Android : voir [`android/README.md`](android/README.md) (CI GitHub, ou `android/build-apk.sh` avec un NDK).

## Organisation du code (`src/`)

| fichier | rôle |
|---|---|
| `world.c` | lecture de `assets/world.bin` (tracé, relief, sol, bâtiments, routes, lieux) |
| `scene.c` | monde en tronçons de 100 m générés en arrière-plan : terrain adapté à la route, marquage, balises J6, lampadaires, glissières, ponts, végétation, bâtiments, routes voisines, horizon |
| `render.c` | shaders d'éclairage (4 textures mélangées par sommet, variante feuillage à test alpha, soleil d'après-midi, brume), ciel, constructeurs de maillages |
| `textures.c` | textures procédurales répétables + atlas de feuillage (mipmaps à couverture constante) |
| `tools/treepack.py` | convertit un pack d'arbres OBJ + PNG en `assets/trees.bin` (sommets 16 bits) + atlas `trees.png` ; `tools/treeview.c` les affiche |
| `car_model.c` | modèles 3D procéduraux : C4 Cactus + 5 véhicules du trafic |
| `car.c` | physique de la voiture (adhérence, sous-virage, aide à la direction, suspension) |
| `traffic.c` | trafic (limitations réelles, distances de sécurité, voies, circulation en face) |
| `signs.c` | panneaux d'agglomération avec les vrais noms, départ, épingle d'arrivée |
| `audio.c` | moteur 4 cylindres, pneus, vent et effets synthétisés en temps réel |
| `game.c` | états du jeu, caméra, commandes tactiles, HUD, menus |

Mode test automatique (PC) : `CARPOLO_TEST=1 ./build-bin/carpolo3d` (pilote automatique + captures d'écran) ;
sur le web, ajouter `?test` à l'adresse. `tools/texdump.c` écrit les textures générées en PNG.
