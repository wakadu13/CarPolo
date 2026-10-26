# CarPolo 🚗💨

Deux jeux sur le même trajet (Cornillon-Confoux → Lançon → près de l'A54) :

- **CarPolo 3D** (`game3d/`, C + raylib) : la vraie C4 Cactus en 3D sur le vrai tracé, relief et bâtiments réels.
  **Jouer en ligne : https://wakadu13.github.io/CarPolo/**
  — Android : `https://github.com/wakadu13/CarPolo/releases/download/apk3d-latest/CarPolo3D.apk`
  — PC Windows / macOS / Linux : release `desktop3d-latest` — voir [game3d/README.md](game3d/README.md).
- **CarPolo** (version cartoon, `www/`, HTML5) : décrite ci-dessous, jouable sur https://wakadu13.github.io/CarPolo/cartoon/.

Jeu de course mobile Android, style cartoon. Tu pilotes une **Citroën C4 bleu turquoise** sur le vrai trajet :
**2000 chemin départemental 15 (La Pécette, Cornillon-Confoux) ➜ Les Grandes Bastides ➜ Lançon-Provence ➜ A7 ➜ arrivée près de l'A54** (12,6 km).

But : **arriver le plus vite possible sans accident**.

## Installer sur un téléphone Android

1. Sur le téléphone, ouvre la page **Releases** du dépôt (`apk-latest`) et télécharge **CarPolo.apk**.
   Lien direct : `https://github.com/wakadu13/carpolo/releases/download/apk-latest/CarPolo.apk`
2. Ouvre le fichier et autorise l'installation depuis cette source si Android le demande.
3. Lance **CarPolo** et tourne le téléphone en paysage.

Chaque push sur GitHub reconstruit l'APK automatiquement (GitHub Actions) et met à jour cette release.
Les nouvelles versions s'installent par-dessus l'ancienne, sans perdre les records.

## Comment jouer

| Commande | Tactile | Clavier |
|---|---|---|
| Tourner | ◀ ▶ (en bas à gauche), ou inclinaison du téléphone (option) | ← → |
| Accélérer / freiner | pédales **GAZ** / **FREIN** | ↑ / ↓ |
| Turbo | bouton ⚡ | Espace |
| Klaxon | bouton 📯 | K |
| Pause | bouton ⏸ ou retour Android | P / Échap |

- **Permis à points** : 12 points. Accident = −3 points, flash radar = −1 point et +3 s. À 0 : permis annulé, partie perdue !
- **Routes à double sens** : pour doubler, il faut passer sur la voie d'en face… quand personne n'arrive.
- **Turbo** : ramasse les **chevrons dorés** et **frôle** les voitures pour remplir la jauge.
- **Klaxon** : la voiture devant toi se range sur le bas-côté (« POUET ! »).
- **Départ canon** : appuie sur GAZ juste au dernier feu du compte à rebours.
- **Radars** à Cornillon-Confoux, Lançon et sur l'A7 : un panneau te prévient avant.
- **Rond-point et travaux** à Lançon : freine, et suis les plots !
- Médailles : **Or** < 2:20, **Argent** < 2:40, **Bronze** < 3:05. Temps intermédiaires comparés à ton record à chaque zone.

## Structure du projet

```
www/                  le jeu (HTML5 Canvas, aucune dépendance, aucun fichier image/son)
  index.html          écrans (titre, aide, pause, résultats)
  css/style.css
  js/util.js          outils (maths, stockage, texte cartoon)
  js/sprites.js       tous les dessins cartoon générés par code (C4, voitures, décor Provence…)
  js/audio.js         moteur, effets et musique synthétisés (WebAudio)
  js/track.js         piste construite à partir du tracé Google Maps (virages, collines, zones, radars)
  js/render.js        rendu pseudo-3D façon OutRun
  js/hud.js           chrono, permis, vitesse, turbo, mini-carte, boutons tactiles
  js/game.js          physique, trafic, collisions, règles, écrans
android/              application Android (WebView plein écran qui charge www/)
tools/build-apk-local.sh   construit l'APK sans Android SDK (outils Debian/Ubuntu)
.github/workflows/android.yml   build Gradle + publication de l'APK
```

## Développement

- **Tester dans un navigateur** : ouvre `www/index.html` (fonctionne aussi en `file://`). Clavier ou écran tactile.
- **APK via Gradle** (Android Studio ou CI) : `cd android && ./gradlew assembleRelease`
  → `android/app/build/outputs/apk/release/app-release.apk`
- **APK sans SDK** (Linux Debian/Ubuntu) : `tools/build-apk-local.sh` → `dist/CarPolo.apk`

L'APK est signé avec une clé de développement **non secrète** (`android/app/carpolo-dev.keystore`, mot de passe `carpolo`),
pour que toutes les versions s'installent les unes par-dessus les autres. Ne pas l'utiliser pour une publication sur le Play Store.
