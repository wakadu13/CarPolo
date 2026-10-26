# CarPolo 3D – version web

Ce dossier est le site publié sur GitHub Pages par `.github/workflows/web3d.yml` :

- `index.html`, `index.js`, `index.wasm`, `index.data` : CarPolo 3D (C + raylib compilé en WebAssembly avec Emscripten).
- `cartoon/` : la première version du jeu (HTML5 Canvas, style cartoon).

Il faut un serveur web (http://), les navigateurs refusent WebAssembly et les fichiers `.data` en `file://`.
