/* CarPolo — construction de la piste à partir du trajet Google Maps
 * Cornillon-Confoux (2000 chemin départemental 15) -> Lançon -> A7 -> arrivée près de l'A54
 */
(function () {
  'use strict';

  const SEG = 200;                 // longueur d'un segment (unités monde)
  const TRACK_SEGS = 6000;         // segments jusqu'à l'arrivée
  const PAD_SEGS = 420;            // route après l'arrivée (pour la caméra)
  const RUMBLE = 3;                // segments par bande de couleur
  const ROAD_W = 2000;             // demi-largeur route départementale (2 voies)
  const HWY_W = 3000;              // demi-largeur autoroute (3 voies)
  const REAL_KM = 12.6;

  // Tracé lu sur la capture (pixels), du départ au point d'arrivée
  const ROUTE = [[305,607],[318,596],[333,583],[336,566],[340,548],[358,532],[375,515],[388,495],[402,474],[415,455],[428,432],[440,410],[452,390],[468,368],[495,354],[520,338],[545,318],[562,300],[575,288],[592,288],[605,282],[622,274],[635,268],[642,250],[650,225],[660,205],[672,185],[680,160],[683,130],[668,118],[650,110],[630,100],[618,88],[608,72],[596,55],[575,46],[555,40],[540,37]];

  // Lieux affichés sur la mini-carte
  const PLACES = [
    { name: 'La Pécette', x: 340, y: 548, dx: 1, dy: 0.3 },
    { name: 'Cornillon-Confoux', x: 388, y: 495, dx: 1, dy: 0.3 },
    { name: 'Les Grandes Bastides', x: 440, y: 410, dx: 1, dy: 0.4 },
    { name: 'Lançon', x: 592, y: 288, dx: 0.6, dy: 1.6 },
    { name: 'A7', x: 672, y: 185, dx: 1, dy: 0 },
    { name: 'Arrivée', x: 540, y: 37, dx: -1.2, dy: 0.2 }
  ];

  // Zones (début en fraction du parcours)
  const ZONES = [
    {
      id: 'pecette', from: 0.0, name: 'D15 · LA PÉCETTE', sub: '2000 chemin départemental 15',
      grass: ['#8cd14b', '#7fc443'], rumble: ['#e8443a', '#fbfbfb'], road: ['#6c7180', '#676b7a'], lane: '#ffffff',
      hill: 0.55, wide: false,
      traffic: { same: [85, 160], onc: [130, 260], models: ['car_red', 'car_white', 'car_yellow', 'cv2', 'van', 'car_green'] },
      scenery: { density: 0.34, spread: 3200, items: [
        { n: 'tree_olive', w: 4 }, { n: 'tree_cypress', w: 3 }, { n: 'bush', w: 3 }, { n: 'rock', w: 2 },
        { n: 'vine', w: 2 }, { n: 'house_mas', w: 0.6, far: true }, { n: 'tree_pine', w: 1.5 } ] },
      rows: []
    },
    {
      id: 'cornillon', from: 0.089, name: 'CORNILLON-CONFOUX', sub: 'Village perché · radar à 50 !',
      grass: ['#a7cf57', '#9ac34d'], rumble: ['#e8443a', '#fbfbfb'], road: ['#6c7180', '#676b7a'], lane: '#ffffff',
      hill: 0.9, wide: false,
      traffic: { same: [80, 150], onc: [120, 240], models: ['car_red', 'car_white', 'cv2', 'car_purple', 'van', 'police', 'car_orange'] },
      scenery: { density: 0.3, spread: 2600, items: [
        { n: 'house_village', w: 4, far: true }, { n: 'bush_flower', w: 3 }, { n: 'tree_cypress', w: 2 },
        { n: 'house_mas', w: 1.5, far: true }, { n: 'bush', w: 2 } ] },
      rows: [{ n: 'tree_plane', every: 11, off: 700 }]
    },
    {
      id: 'bastides', from: 0.236, name: 'LES GRANDES BASTIDES', sub: 'Lavande, vignes… et tracteurs !',
      grass: ['#9b7ad8', '#86c64a'], rumble: ['#e8443a', '#fbfbfb'], road: ['#6c7180', '#676b7a'], lane: '#ffffff',
      hill: 1.0, wide: false,
      traffic: { same: [75, 140], onc: [110, 230], models: ['tractor', 'car_yellow', 'car_green', 'tractor', 'van', 'cv2', 'car_red'] },
      scenery: { density: 0.42, spread: 3600, items: [
        { n: 'lavender', w: 5 }, { n: 'vine', w: 3 }, { n: 'sunflower', w: 3 }, { n: 'tree_cypress', w: 2 },
        { n: 'tree_olive', w: 2 }, { n: 'house_mas', w: 0.8, far: true } ] },
      rows: []
    },
    {
      id: 'lancon', from: 0.473, name: 'LANÇON-PROVENCE', sub: 'Rond-point et travaux : prudence !',
      grass: ['#b5cf5c', '#a7c252'], rumble: ['#e8443a', '#fbfbfb'], road: ['#6c7180', '#676b7a'], lane: '#ffffff',
      hill: 1.1, wide: false,
      traffic: { same: [80, 150], onc: [120, 240], models: ['car_white', 'car_red', 'car_purple', 'cv2', 'van', 'truck', 'car_orange'] },
      scenery: { density: 0.32, spread: 2600, items: [
        { n: 'house_village', w: 4, far: true }, { n: 'tree_plane', w: 2 }, { n: 'bush_flower', w: 2 },
        { n: 'tree_cypress', w: 2 }, { n: 'church', w: 0.3, far: true } ] },
      rows: []
    },
    {
      id: 'a7', from: 0.627, name: 'AUTOROUTE A7', sub: 'Direction Salon-de-Provence · 3 voies',
      grass: ['#c9c86a', '#bcbb5f'], rumble: ['#d9dde4', '#f4f4f4'], road: ['#5f6472', '#5b606e'], lane: '#ffffff',
      hill: 0.45, wide: true,
      traffic: { same: [26, 55], onc: null, models: ['truck', 'car_red', 'car_white', 'car_yellow', 'truck', 'van', 'car_green', 'car_purple', 'police'] },
      scenery: { density: 0.3, spread: 3600, items: [
        { n: 'tree_pine', w: 4 }, { n: 'bush', w: 3 }, { n: 'rock', w: 2 }, { n: 'tree_cypress', w: 1 } ] },
      rows: []
    },
    {
      id: 'final', from: 0.831, name: 'VERS L’ARRIVÉE', sub: 'Dernière ligne droite près de l’A54',
      grass: ['#7cc94a', '#71bc42'], rumble: ['#e8443a', '#fbfbfb'], road: ['#6c7180', '#676b7a'], lane: '#ffffff',
      hill: 0.8, wide: false,
      traffic: { same: [85, 160], onc: [120, 240], models: ['car_red', 'car_white', 'car_yellow', 'cv2', 'van', 'car_green', 'car_orange'] },
      scenery: { density: 0.4, spread: 3000, items: [
        { n: 'tree_pine', w: 5 }, { n: 'tree_cypress', w: 2 }, { n: 'rock', w: 2 }, { n: 'bush', w: 2 }, { n: 'tree_olive', w: 1 } ] },
      rows: []
    }
  ];

  // Collision : fraction de la largeur du sprite qui est "dure"
  const HIT = {
    tree_cypress: 0.55, tree_olive: 0.3, tree_pine: 0.18, tree_plane: 0.22, bush: 0.6, bush_flower: 0.6,
    lavender: 0, vine: 0, sunflower: 0, rock: 0.8, house_mas: 0.95, house_village: 0.95, church: 0.95,
    sign_speed_50: 0.2, sign_speed_70: 0.2, sign_speed_90: 0.2, sign_speed_110: 0.2, sign_speed_130: 0.2,
    sign_radar: 0.2, radar_box: 0.35, sign_chevron_l: 0.6, sign_chevron_r: 0.6, sign_roundabout: 0.2,
    sign_works: 0.2, cone: 0.7, barrier_works: 0.95, sign_a7: 0.9, sign_dir: 0.9, sign_autoroute: 0.2,
    billboard_calisson: 0.9, billboard_savon: 0.9, billboard_melon: 0.9, billboard_olive: 0.9,
    town: 0.9, pin: 0.4, startdot: 0
  };

  function build() {
    const rng = U.seeded(13013);
    const N = TRACK_SEGS + PAD_SEGS;

    // --- longueur d'arc du tracé
    const L = [0];
    for (let i = 1; i < ROUTE.length; i++) {
      L.push(L[i - 1] + Math.hypot(ROUTE[i][0] - ROUTE[i - 1][0], ROUTE[i][1] - ROUTE[i - 1][1]));
    }
    const LT = L[L.length - 1];
    function pointAt(f) {
      const d = U.clamp(f, 0, 1) * LT;
      let i = 1;
      while (i < L.length - 1 && L[i] < d) i++;
      const t = (d - L[i - 1]) / ((L[i] - L[i - 1]) || 1);
      return [U.lerp(ROUTE[i - 1][0], ROUTE[i][0], t), U.lerp(ROUTE[i - 1][1], ROUTE[i][1], t)];
    }

    // --- cap lissé le long du parcours -> courbure de base
    const heading = new Float64Array(TRACK_SEGS + 1);
    const delta = 0.018;
    let prev = null;
    for (let i = 0; i <= TRACK_SEGS; i++) {
      const f = i / TRACK_SEGS;
      const a = pointAt(f - delta), b = pointAt(f + delta);
      let h = Math.atan2(b[1] - a[1], b[0] - a[0]);
      if (prev !== null) {
        while (h - prev > Math.PI) h -= 2 * Math.PI;
        while (h - prev < -Math.PI) h += 2 * Math.PI;
      }
      heading[i] = h;
      prev = h;
    }
    const GAIN = 330;
    const curve = new Float64Array(N);
    for (let i = 0; i < TRACK_SEGS; i++) curve[i] = (heading[i + 1] - heading[i]) * GAIN;

    // --- zones en segments
    const zoneStart = ZONES.map(z => Math.round(z.from * TRACK_SEGS));
    function zoneIndexAt(i) {
      let z = 0;
      for (let k = 0; k < ZONES.length; k++) if (i >= zoneStart[k]) z = k;
      return z;
    }
    const zEnd = (k) => (k + 1 < ZONES.length ? zoneStart[k + 1] : TRACK_SEGS);

    // --- virages "fun" ajoutés (somme nulle pour respecter le cap réel)
    function addS(start, len, amp) {      // S : droite puis gauche (ou l'inverse si amp < 0)
      for (let k = 0; k < len; k++) {
        const t = k / len;
        const v = Math.sin(t * Math.PI * 2) * amp;
        if (start + k < TRACK_SEGS) curve[start + k] += v;
      }
    }
    function addRoundabout(start) {       // entrée à droite, tour à gauche, sortie à droite
      const prof = [[18, 5.5], [40, -5.2], [18, 5.5]];
      let s = start;
      // compense pour que la somme soit nulle : 18*5.5*2 = 198 ; 40*5.2 = 208 -> on ajuste la fin
      for (const [len, c] of prof) {
        for (let k = 0; k < len; k++) {
          const t = k / len, env = Math.sin(t * Math.PI);
          curve[s + k] += c * (0.55 + 0.45 * env) * 1.0;
        }
        s += len;
      }
      return s;
    }

    const zPec = zoneStart[0], zCor = zoneStart[1], zBas = zoneStart[2], zLan = zoneStart[3], zA7 = zoneStart[4], zFin = zoneStart[5];

    // La Pécette : petit S après le départ
    addS(zPec + 150, 160, 2.2);
    addS(zPec + 360, 120, -2.6);
    // Cornillon : enchaînement dans le village
    addS(zCor + 120, 140, -3.2);
    addS(zCor + 330, 120, 3.4);
    addS(zCor + 560, 160, -2.6);
    // Grandes Bastides : grandes ondulations
    for (let k = 0; k < 4; k++) addS(zBas + 120 + k * 330, 300, (k % 2 ? 2.4 : -2.8));
    // Lançon : rond-point au carrefour + chicane
    const roundaboutStart = Math.round(0.538 * TRACK_SEGS);
    const roundaboutEnd = addRoundabout(roundaboutStart);
    addS(zLan + 70, 110, 3.0);
    // A7 : grandes courbes rapides
    addS(zA7 + 200, 420, 1.8);
    addS(zA7 + 700, 380, -1.6);
    // Final : sinueux dans les pins
    addS(zFin + 80, 130, 3.6);
    addS(zFin + 260, 130, -3.8);
    addS(zFin + 460, 160, 3.0);

    // départ et arrivée bien droits
    for (let i = 0; i < 90; i++) curve[i] *= i / 90;
    for (let i = TRACK_SEGS - 160; i < N; i++) curve[i] = i < TRACK_SEGS ? curve[i] * Math.max(0, (TRACK_SEGS - 40 - i) / 120) : 0;
    for (let i = 0; i < N; i++) curve[i] = U.clamp(curve[i], -6.5, 6.5);

    // --- relief (collines)
    function hillAmp(i) {
      if (i >= TRACK_SEGS) return 0;
      const z = zoneIndexAt(i);
      let a = ZONES[z].hill;
      // fondu entre zones
      const into = i - zoneStart[z];
      if (z > 0 && into < 120) a = U.lerp(ZONES[z - 1].hill, a, into / 120);
      if (i < 140) a *= i / 140;
      if (i > TRACK_SEGS - 200) a *= Math.max(0, (TRACK_SEGS - i) / 200);
      if (i >= roundaboutStart - 40 && i < roundaboutEnd + 40) a *= 0.15;
      return a;
    }
    // amplitude lissée (moyenne glissante) pour éviter toute marche dans le relief
    const ampRaw = new Float64Array(N + 1);
    for (let i = 0; i <= N; i++) ampRaw[i] = hillAmp(Math.min(i, N - 1));
    const amp = new Float64Array(N + 1);
    const HW = 60;
    for (let i = 0; i <= N; i++) {
      let acc = 0;
      for (let k = -HW; k <= HW; k++) acc += ampRaw[U.clamp(i + k, 0, N)];
      amp[i] = acc / (2 * HW + 1);
    }
    const ys = new Float64Array(N + 1);
    for (let i = 0; i <= N; i++) {
      const a = amp[i];
      const raw = 2600 * (0.62 * Math.sin(i / 290 * Math.PI * 2 + 0.8) + 0.38 * Math.sin(i / 131 * Math.PI * 2 + 2.1));
      ys[i] = raw * a;
    }
    // base de hauteur relative pour que y(0) = 0
    const y0 = ys[0];
    for (let i = 0; i <= N; i++) ys[i] -= y0;

    // --- largeur de route (autoroute plus large)
    const TRANS = 70;
    const widthAt = new Float64Array(N + 1);
    const a7s = zA7, a7e = zFin;
    for (let i = 0; i <= N; i++) {
      let w = ROAD_W;
      if (i >= a7s - TRANS && i < a7s) w = U.lerp(ROAD_W, HWY_W, U.smooth((i - (a7s - TRANS)) / TRANS));
      else if (i >= a7s && i < a7e) w = HWY_W;
      else if (i >= a7e && i < a7e + TRANS) w = U.lerp(HWY_W, ROAD_W, U.smooth((i - a7e) / TRANS));
      widthAt[i] = w;
    }

    // --- segments
    const segments = new Array(N);
    for (let i = 0; i < N; i++) {
      const z = zoneIndexAt(Math.min(i, TRACK_SEGS - 1));
      const zone = ZONES[z];
      const w1 = widthAt[i], w2 = widthAt[i + 1];
      let lanes = 2;
      if (w1 >= HWY_W - 1 && w2 >= HWY_W - 1) lanes = 3;
      else if (w1 > ROAD_W + 1 || w2 > ROAD_W + 1) lanes = 0; // transition
      const alt = Math.floor(i / RUMBLE) % 2;
      segments[i] = {
        index: i,
        p1: { world: { y: ys[i], z: i * SEG, w: w1 }, camera: {}, screen: {} },
        p2: { world: { y: ys[i + 1], z: (i + 1) * SEG, w: w2 }, camera: {}, screen: {} },
        curve: curve[i],
        zone: z,
        lanes: lanes,
        oncoming: lanes === 2,
        roundabout: false,
        works: false,
        alt: alt,
        grass: zone.grass[alt],
        rumble: zone.rumble[alt],
        road: zone.road[alt],
        lane: alt ? zone.lane : null,
        sprites: [],
        cars: [],
        tokens: [],
        clip: 0
      };
    }
    // lavande en bandes seulement dans une partie des Grandes Bastides
    for (let i = zBas; i < zLan; i++) {
      const s = segments[i];
      const f = (i - zBas) / (zLan - zBas);
      if (f > 0.55) s.grass = s.alt ? '#86c64a' : '#79b943';
    }
    // la route continue après l'arrivée
    for (let i = TRACK_SEGS; i < N; i++) {
      segments[i].oncoming = false;
    }
    // départ : damier sur la ligne
    for (let i = 10; i < 13; i++) segments[i].start = true;
    for (let i = TRACK_SEGS; i < TRACK_SEGS + 3; i++) segments[i].finishLine = true;

    // rond-point : pas de circulation en face, îlot central fleuri à gauche
    for (let i = roundaboutStart - 6; i < roundaboutEnd + 6; i++) {
      segments[i].roundabout = true;
      segments[i].oncoming = false;
    }

    // --- outils de placement
    function put(i, name, offset, extra) {
      if (i < 0 || i >= N) return;
      const sp = { name: name, offset: offset, hit: HIT[name] != null ? HIT[name] : 0.5 };
      if (extra) Object.assign(sp, extra);
      segments[i].sprites.push(sp);
    }
    function worldW(name) {
      const s = window.Sprites && Sprites.get(name);
      return (s && s.worldW) || 1000;
    }
    function edge(i) { return segments[i].p1.world.w; }
    function sideOffset(i, name, side, extra, ww) {
      return side * (edge(i) * 1.12 + 350 + (ww || worldW(name)) / 2 + (extra || 0));
    }
    // éviter que deux décors se chevauchent sur le même côté
    const occupied = new Map();
    function free(i, side, span) {
      for (let k = i - span; k <= i + span; k++) if (occupied.get(k * 2 + (side > 0 ? 1 : 0))) return false;
      return true;
    }
    function mark(i, side) { occupied.set(i * 2 + (side > 0 ? 1 : 0), true); }

    // --- panneaux & éléments scénarisés
    const radars = [];
    function addRadar(i, limit, tol) {
      put(i - 95, 'sign_radar', sideOffset(i - 95, 'sign_radar', 1)); mark(i - 95, 1);
      put(i - 70, 'sign_speed_' + limit, sideOffset(i - 70, 'sign_speed_' + limit, 1)); mark(i - 70, 1);
      put(i, 'radar_box', sideOffset(i, 'radar_box', 1, -150), { radar: true }); mark(i, 1);
      radars.push({ seg: i, limit: limit, tol: tol, done: false });
    }

    // départ
    put(14, 'arch_start', 0, { arch: true, hit: 0 });
    put(4, 'startdot', -edge(4) * 1.12 - 900, { hit: 0 });
    // panneaux de ville
    put(zCor + 20, 'town', sideOffset(zCor + 20, 'town', 1, 0, 1700), { town: 'CORNILLON-CONFOUX', worldW: 1700 }); mark(zCor + 20, 1);
    put(zBas - 20, 'town', sideOffset(zBas - 20, 'town', 1, 0, 1700), { town: 'CORNILLON-CONFOUX', ended: true, worldW: 1700 }); mark(zBas - 20, 1);
    put(zLan + 20, 'town', sideOffset(zLan + 20, 'town', 1, 0, 1700), { town: 'LANÇON-PROVENCE', worldW: 1700 }); mark(zLan + 20, 1);
    put(zA7 - 120, 'town', sideOffset(zA7 - 120, 'town', 1, 0, 1700), { town: 'LANÇON-PROVENCE', ended: true, worldW: 1700 }); mark(zA7 - 120, 1);
    // église de Cornillon (village perché)
    put(zCor + 260, 'church', -(edge(zCor + 260) * 1.12 + 350 + worldW('church') / 2 + 600)); mark(zCor + 260, -1);
    put(zLan + 180, 'church', (edge(zLan + 180) * 1.12 + 350 + worldW('church') / 2 + 900)); mark(zLan + 180, 1);

    // radars
    addRadar(zCor + 420, 50, 30);
    addRadar(zLan + 130, 50, 30);
    addRadar(zA7 + 900, 130, 20);

    // rond-point
    put(roundaboutStart - 80, 'sign_roundabout', sideOffset(roundaboutStart - 80, 'sign_roundabout', 1)); mark(roundaboutStart - 80, 1);
    put(roundaboutStart - 55, 'sign_speed_50', sideOffset(roundaboutStart - 55, 'sign_speed_50', 1)); mark(roundaboutStart - 55, 1);
    for (let i = roundaboutStart + 4; i < roundaboutEnd - 4; i += 6) {
      const inside = -1;
      put(i, (i / 6) % 3 < 1 ? 'tree_olive' : 'bush_flower', inside * (edge(i) * 1.12 + 250 + worldW('bush_flower') / 2));
      mark(i, inside);
    }

    // travaux après le rond-point : la voie de droite est fermée
    const worksStart = roundaboutEnd + 70, worksEnd = worksStart + 90;
    put(worksStart - 150, 'sign_works', sideOffset(worksStart - 150, 'sign_works', 1)); mark(worksStart - 150, 1);
    put(worksStart - 75, 'sign_works', sideOffset(worksStart - 75, 'sign_works', 1)); mark(worksStart - 75, 1);
    // biseau de plots qui rabat vers la voie de gauche
    for (let k = 0; k < 8; k++) {
      const i = worksStart - 40 + k * 5;
      put(i, 'cone', U.lerp(1850, 260, k / 8), { onRoad: true, hit: 0.7 });
    }
    put(worksStart, 'barrier_works', 1000, { hit: 0.95, onRoad: true });
    for (let i = worksStart - 60; i < worksEnd + 30; i++) segments[i].oncoming = false;
    for (let i = worksStart; i < worksEnd; i++) {
      segments[i].works = true;
      if ((i - worksStart) % 5 === 0) put(i, 'cone', 220, { onRoad: true, hit: 0.7 });
    }
    put(worksEnd + 1, 'barrier_works', 1000, { hit: 0.95, onRoad: true });

    // autoroute
    put(zA7 - 40, 'sign_autoroute', sideOffset(zA7 - 40, 'sign_autoroute', 1)); mark(zA7 - 40, 1);
    put(zA7 + 30, 'sign_a7', sideOffset(zA7 + 30, 'sign_a7', 1, -200)); mark(zA7 + 30, 1);
    put(zA7 + 500, 'sign_speed_130', sideOffset(zA7 + 500, 'sign_speed_130', 1)); mark(zA7 + 500, 1);
    put(zA7 + 1000, 'sign_a7', sideOffset(zA7 + 1000, 'sign_a7', 1, -200)); mark(zA7 + 1000, 1);
    put(zFin - 60, 'sign_dir', sideOffset(zFin - 60, 'sign_dir', 1, -200)); mark(zFin - 60, 1);
    put(zFin + 40, 'sign_speed_90', sideOffset(zFin + 40, 'sign_speed_90', 1)); mark(zFin + 40, 1);

    // panneaux publicitaires
    [[zPec + 250, 'billboard_calisson', -1], [zBas + 200, 'billboard_melon', 1], [zBas + 760, 'billboard_savon', -1],
     [zA7 + 300, 'billboard_olive', -1], [zA7 + 1150, 'billboard_calisson', 1], [zFin + 400, 'billboard_savon', 1],
     [zLan + 60, 'billboard_olive', -1]].forEach(([i, n, side]) => {
      put(i, n, sideOffset(i, n, side, 300)); mark(i, side);
      mark(i + 1, side); mark(i - 1, side);
    });

    // arrivée
    put(TRACK_SEGS + 1, 'arch_finish', 0, { arch: true, hit: 0 });
    put(TRACK_SEGS + 4, 'pin', sideOffset(TRACK_SEGS + 4, 'tree_olive', 1, -400), { hit: 0 });

    // chevrons dans les virages serrés
    for (let i = 60; i < TRACK_SEGS; i++) {
      const c = curve[i];
      if (Math.abs(c) >= 3.6 && i % 9 === 0 && !segments[i].roundabout) {
        const outer = c > 0 ? -1 : 1;
        const name = c > 0 ? 'sign_chevron_r' : 'sign_chevron_l';
        if (free(i, outer, 2)) { put(i, name, sideOffset(i, name, outer, -250)); mark(i, outer); }
      }
    }

    // rangées d'arbres (platanes) et décor aléatoire
    for (let i = 20; i < N - 2; i++) {
      const zone = ZONES[segments[i].zone];
      if (i >= TRACK_SEGS) {
        if (i % 6 === 0) {
          const side = i % 12 === 0 ? -1 : 1;
          put(i, 'tree_pine', sideOffset(i, 'tree_pine', side, rng.range(0, 1500)));
        }
        continue;
      }
      for (const row of zone.rows) {
        if (i % row.every === 0) {
          for (const side of [-1, 1]) {
            if (free(i, side, 2)) { put(i, row.n, side * (edge(i) * 1.12 + row.off + worldW(row.n) * 0.1)); mark(i, side); }
          }
        }
      }
      if (segments[i].roundabout) continue;
      for (const side of [-1, 1]) {
        if (rng() < zone.scenery.density * 0.5) {
          const it = rng.weighted(zone.scenery.items.map(x => ({ w: x.w, it: x }))).it;
          const extra = (it.far ? 1200 : 0) + rng.range(0, zone.scenery.spread);
          const span = worldW(it.n) > 3000 ? 6 : 1;
          if (free(i, side, span)) {
            put(i, it.n, sideOffset(i, it.n, side, extra));
            mark(i, side);
          }
        }
      }
      // lavande en rangs serrés
      if (zone.id === 'bastides' && i % 4 === 0 && (i - zoneStart[2]) / (zoneStart[3] - zoneStart[2]) < 0.55) {
        for (const side of [-1, 1]) {
          put(i, 'lavender', sideOffset(i, 'lavender', side, 2600 + rng.range(0, 2400)), { hit: 0 });
        }
      }
    }

    // --- jetons (double chevron doré) en lignes sur les voies
    let i = 130;
    while (i < TRACK_SEGS - 100) {
      const s = segments[i];
      if (s.lanes > 0 && !s.roundabout && !s.works && !segments[i + 40].works) {
        const lanesX = s.lanes === 3 ? [-2000, 0, 2000] : [-1000, 1000];
        const x = rng.pick(lanesX);
        const count = 3 + Math.floor(rng() * 3);
        for (let k = 0; k < count; k++) {
          const sg = segments[i + k * 7];
          if (sg && sg.lanes === s.lanes) sg.tokens.push({ x: x, taken: false });
        }
      }
      i += Math.floor(rng.range(140, 260));
    }

    // --- pour chaque segment : dernier segment (<= i) interdit à la circulation en face
    const lastNoOnc = new Int32Array(N);
    let lb = -1;
    for (let k = 0; k < N; k++) { if (!segments[k].oncoming) lb = k; lastNoOnc[k] = lb; }

    // --- mini-carte : boîte englobante
    let minX = 1e9, minY = 1e9, maxX = -1e9, maxY = -1e9;
    for (const p of ROUTE) { minX = Math.min(minX, p[0]); minY = Math.min(minY, p[1]); maxX = Math.max(maxX, p[0]); maxY = Math.max(maxY, p[1]); }

    return {
      SEG: SEG,
      RUMBLE: RUMBLE,
      segments: segments,
      length: N * SEG,
      finishSeg: TRACK_SEGS,
      finishZ: TRACK_SEGS * SEG,
      zones: ZONES,
      zoneStart: zoneStart,
      radars: radars,
      lastNoOnc: lastNoOnc,
      route: ROUTE,
      places: PLACES,
      routeBox: { minX: minX, minY: minY, maxX: maxX, maxY: maxY },
      routePoint: pointAt,
      realKm: REAL_KM,
      roundabout: [roundaboutStart, roundaboutEnd],
      works: [worksStart, worksEnd],
      laneXs(seg) {
        if (seg.lanes === 3) return [-2000, 0, 2000];
        return [-1000, 1000];
      }
    };
  }

  window.Track = { build: build, SEG: SEG, ROAD_W: ROAD_W, HWY_W: HWY_W, TRACK_SEGS: TRACK_SEGS };
})();
