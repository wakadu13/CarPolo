/* CarPolo — logique du jeu : physique, trafic, collisions, radars, turbo, écrans */
(function () {
  'use strict';

  const SEG = Track.SEG;
  const MAX = SEG * 60;                  // vitesse max normale (unités/s) = 200 km/h
  const KMH = 200 / MAX;
  const ACCEL = MAX / 4.2;
  const BRAKE = -MAX * 1.15;
  const DECEL = -MAX / 6;
  const OFFROAD_DECEL = -MAX / 1.5;
  const OFFROAD_LIMIT = MAX / 3.2;
  const CENTRIFUGAL = 0.2;
  const BOOST_MULT = 1.32;
  const BOOST_TIME = 3.2;                // secondes pour vider une jauge pleine
  const PLAYER_W = 1100;
  const PLAYER_HIT_W = PLAYER_W * 0.8;
  const CRASH_POINTS = 3;
  const RADAR_PENALTY = 3000;
  const CONE_PENALTY = 1000;
  const STEP = 1 / 60;

  const MEDALS = [
    { id: 'or', name: 'OR', time: 140000, color: '#ffd23a' },
    { id: 'argent', name: 'ARGENT', time: 160000, color: '#d9dde4' },
    { id: 'bronze', name: 'BRONZE', time: 185000, color: '#e09a5a' }
  ];

  const CAR_INFO = {
    car_red: { len: 460, speed: [0.42, 0.56] }, car_white: { len: 460, speed: [0.4, 0.55] },
    car_yellow: { len: 460, speed: [0.38, 0.52] }, car_green: { len: 460, speed: [0.4, 0.55] },
    car_purple: { len: 460, speed: [0.42, 0.56] }, car_orange: { len: 460, speed: [0.4, 0.54] },
    cv2: { len: 420, speed: [0.3, 0.38], bob: 18 }, van: { len: 560, speed: [0.36, 0.48] },
    truck: { len: 1000, speed: [0.33, 0.42] }, tractor: { len: 520, speed: [0.15, 0.2], bob: 25 },
    police: { len: 460, speed: [0.5, 0.6] }
  };

  let canvas, ctx, track, segs;
  const g = {
    state: 'boot', time: 0, input: {}, cars: [], particles: [], popups: [],
    bgOffset: [0, 0, 0, 0], cloudDrift: 0, fovBoost: 0,
    showTouch: !!(window.matchMedia && matchMedia('(pointer: coarse)').matches) || ('ontouchstart' in window)
  };
  window.CP = g; // exposé pour les hooks Android et le débogage

  const settings = Object.assign({ tilt: false, quality: 'auto' }, U.store.get('carpolo.settings', {}));
  function saveSettings() { U.store.set('carpolo.settings', settings); }

  // ================================================================ helpers
  function segAt(z) { return segs[U.clamp(Math.floor(z / SEG), 0, segs.length - 1)]; }
  function playerZ() { return g.position + Render.PLAYER_Z; }
  function laneXs(seg) {
    if (seg.lanes === 3) return [-2000, 0, 2000];
    return [-1000, 1000];
  }
  function popup(text, color, size, y, x) {
    y = y || 0.4;
    // évite que deux textes se superposent
    for (const p of g.popups) if (p.max - p.t < 0.5 && Math.abs(p.y - y) < 0.06) y = p.y + 0.075;
    g.popups.push({ text: text, color: color || '#ffffff', size: size || 22, x: x != null ? x : 0.5, y: y, t: 1.4, max: 1.4 });
    if (g.popups.length > 5) g.popups.shift();
  }
  function vibrate(ms) { try { if (navigator.vibrate) navigator.vibrate(ms); } catch (e) { /* rien */ } }
  function sfx(name, opts) { if (window.AudioFX) AudioFX.play(name, opts); }

  // ================================================================ partie
  function resetRace() {
    track = g.track = Track.build();
    segs = track.segments;
    g.position = 0;
    g.speed = 0;
    g.playerX = 1000;
    g.steer = 0;
    g.raceTime = 0;
    g.penalty = 0;
    g.points = 12;
    g.gauge = 0.25;
    g.boosting = false;
    g.crash = null;
    g.invuln = 0;
    g.crashes = 0;
    g.nearMisses = 0;
    g.combo = 0; g.comboT = 0;
    g.tokens = 0;
    g.flashes = 0;
    g.cones = 0;
    g.splits = [];
    g.zone = 0;
    g.cars = [];
    g.particles = [];
    g.popups = [];
    g.banner = null; g.pendingBanner = null;
    g.flash = 0; g.shake = 0; g.pointsFlash = 0;
    g.hornT = 0; g.hornCd = 0;
    g.goT = 0;
    g.perfectWindow = false;
    g.bounce = 0; g.squash = 0;
    g.finalTime = 0;
    g.resultsShown = false;
    g.spawnSame = 90; g.spawnOnc = 110;
    g.progress = 0;
    g.kmh = 0;
    g.offroad = false;
    g.bumpT = 0;
    updateCamera();
  }

  function startCountdown() {
    resetRace();
    g.state = 'countdown';
    g.countdown = 3;
    g.lastBeep = 4;
    g.gasDuringCountdownEarly = false;
    spawnTraffic(true);
    if (window.AudioFX) AudioFX.music('race');
    showScreen(null);
  }

  // ================================================================ trafic
  function spawnTraffic() {
    const pSeg = Math.floor(playerZ() / SEG);
    const horizon = Math.min(track.finishSeg - 40, pSeg + Render.DRAW + 10);
    while (g.spawnSame < horizon) {
      const s = g.spawnSame;
      const seg = segs[s];
      const zone = track.zones[seg.zone];
      if (s > 80 && seg.lanes > 0 && !seg.roundabout && !seg.works) {
        const model = U.pick(zone.traffic.models);
        let x;
        if (seg.lanes === 3) {
          const lanes = model === 'truck' ? [2000, 2000, 0] : [-2000, 0, 2000, 0, 2000];
          x = U.pick(lanes);
        } else x = 1000;
        addCar(model, s * SEG, x, 1);
      }
      g.spawnSame += U.randInt(zone.traffic.same[0], zone.traffic.same[1]);
    }
    while (g.spawnOnc < horizon) {
      const s = g.spawnOnc;
      const seg = segs[s];
      const zone = track.zones[seg.zone];
      if (zone.traffic.onc && seg.oncoming && track.lastNoOnc[s] < pSeg - 2 && s > 120) {
        const model = U.pick(zone.traffic.models.filter(m => m !== 'truck' || Math.random() < 0.5));
        addCar(model, s * SEG, -1000, -1);
      }
      const r = zone.traffic.onc || [60, 120];
      g.spawnOnc += U.randInt(r[0], r[1]);
    }
  }

  function addCar(model, z, x, dir) {
    const info = CAR_INFO[model] || CAR_INFO.car_red;
    // pas de voiture collée à une autre dans la même voie
    for (const c of g.cars) {
      if (c.dir === dir && Math.abs(c.x - x) < 900 && Math.abs(c.z - z) < 1800) return;
    }
    const zone = track.zones[segAt(z).zone];
    let sp = U.rand(info.speed[0], info.speed[1]);
    if (zone.id === 'a7' && model !== 'truck') sp = U.rand(0.5, 0.68);
    const spr = window.Sprites && Sprites.get(model + (dir > 0 ? '_rear' : '_front'));
    g.cars.push({
      model: model, sprite: model + (dir > 0 ? '_rear' : '_front'),
      z: z, x: x, targetX: x, dir: dir,
      base: sp * MAX, speed: sp * MAX * dir,
      len: info.len, w: (spr && spr.worldW) || 1050,
      bobA: info.bob || 0, bob: 0, phase: Math.random() * 10,
      prevDz: null, near: false, honked: false, honkT: 0, pulled: 0, lane: null, laneCd: U.rand(2, 6)
    });
  }

  function updateCars(dt) {
    const pz = playerZ();
    const pSeg = Math.floor(pz / SEG);
    for (let i = g.cars.length - 1; i >= 0; i--) {
      const c = g.cars[i];
      const seg = segAt(c.z);
      // retrait des voitures hors de vue
      if (c.z < g.position - 3 * SEG || c.z > g.position + (Render.DRAW + 80) * SEG || c.z >= (segs.length - 5) * SEG) {
        g.cars.splice(i, 1); continue;
      }
      if (c.dir < 0 && !seg.oncoming) {
        if (Math.abs(seg.index - pSeg) > 50 || seg.index < pSeg) { g.cars.splice(i, 1); continue; }
      }

      // voie visée
      if (c.dir > 0) {
        if (seg.lanes === 3) {
          if (c.lane == null) {
            const xs = [-2000, 0, 2000];
            let best = 0;
            xs.forEach((v, k) => { if (Math.abs(v - c.x) < Math.abs(xs[best] - c.x)) best = k; });
            c.lane = best;
          }
          c.targetX = [-2000, 0, 2000][c.lane];
        } else if (seg.lanes === 2) {
          c.lane = null;
          c.targetX = seg.works ? -1000 : (c.pulled > 0 ? 2150 : 1000);
        }
      } else {
        c.targetX = -1000;
      }

      // suivre la voiture de devant
      let want = c.base * (c.pulled > 0 ? 0.6 : 1);
      if (seg.roundabout) want = Math.min(want, MAX * 0.28);
      for (const o of g.cars) {
        if (o === c || o.dir !== c.dir) continue;
        const ahead = (o.z - c.z) * c.dir;
        if (ahead > 0 && ahead < 2200 && Math.abs(o.x - c.x) < 900) {
          want = Math.min(want, Math.abs(o.speed) * (ahead < 1000 ? 0.85 : 1));
          // sur l'autoroute : on double si la voie est libre
          if (seg.lanes === 3 && c.dir > 0 && c.laneCd <= 0) tryLaneChange(c);
        }
      }
      // le joueur est un obstacle aussi : pas de choc par l'arrière, et ceux d'en face freinent
      const dzp = pz - c.z;
      if (Math.abs(g.playerX - c.x) < 1000 && !g.crash) {
        if (c.dir > 0 && dzp > 0 && dzp < 2600) want = Math.min(want, g.speed * (dzp < 1200 ? 0.8 : 0.95));
        if (c.dir < 0 && dzp < 0 && dzp > -26 * SEG) want = Math.min(want, MAX * 0.12);
      }
      c.laneCd -= dt;
      const sp = Math.abs(c.speed);
      c.speed = c.dir * U.approach(sp, want, (want < sp ? MAX * 1.1 : MAX * 0.5) * dt);
      c.z += c.speed * dt;
      c.x = U.approach(c.x, c.targetX, 1500 * dt);
      if (c.pulled > 0) c.pulled -= dt;
      if (c.honkT > 0) c.honkT -= dt;
      c.phase += dt * (8 + Math.abs(c.speed) / MAX * 10);
      c.bob = c.bobA ? -Math.abs(Math.sin(c.phase)) * c.bobA : 0;
    }
  }

  function laneFree(lx, z, self) {
    for (const o of g.cars) {
      if (o === self || o.dir < 0) continue;
      if (Math.abs(o.x - lx) < 1000 && Math.abs(o.z - z) < 2600) return false;
    }
    // ne pas couper la route du joueur
    if (Math.abs(g.playerX - lx) < 1100 && Math.abs(playerZ() - z) < 2400) return false;
    return true;
  }
  function tryLaneChange(c) {
    const opts = [c.lane - 1, c.lane + 1].filter(l => l >= 0 && l <= 2);
    if (c.model === 'truck') opts.sort((a, b) => b - a);
    for (const l of opts) {
      if (laneFree([-2000, 0, 2000][l], c.z, c)) { c.lane = l; c.laneCd = U.rand(3, 6); return true; }
    }
    c.laneCd = 1;
    return false;
  }

  function assignCarsToSegments() {
    const base = Math.floor(g.position / SEG);
    const last = Math.min(segs.length - 1, base + Render.DRAW + 2);
    for (let i = Math.max(0, base - 4); i <= last; i++) segs[i].cars.length = 0;
    for (const c of g.cars) {
      const s = Math.floor(c.z / SEG);
      if (s >= base && s <= last) segs[s].cars.push(c);
    }
  }

  // ================================================================ joueur
  function updateCamera() {
    const pz = playerZ();
    const ps = segAt(pz);
    const pct = (pz % SEG) / SEG;
    g.playerY = U.lerp(ps.p1.world.y, ps.p2.world.y, pct);
    g.camY = g.playerY + Render.CAMERA_HEIGHT;
    g.progress = U.clamp(pz / track.finishZ, 0, 1);
  }

  function readSteerInput() {
    const inp = g.input;
    if (settings.tilt && g.tiltOK) return U.clamp(g.tiltSteer, -1, 1);
    return (inp.right ? 1 : 0) - (inp.left ? 1 : 0);
  }

  function updatePlayer(dt) {
    const inp = g.input;
    const pz = playerZ();
    const seg = segAt(pz);
    const width = seg.p1.world.w;
    const speedPct = g.speed / MAX;

    // ---- accident en cours
    if (g.crash) {
      g.crash.t += dt;
      g.speed = U.approach(g.speed, 0, MAX * 1.2 * dt);
      g.position += g.speed * dt;
      g.steer = 0;
      g.braking = false;
      if (g.crash.t > 1.5) endCrash();
      return;
    }

    // ---- direction
    const target = readSteerInput();
    g.steer = settings.tilt && g.tiltOK ? target : U.approach(g.steer, target, 7 * dt);
    const dxw = dt * 2 * U.clamp(speedPct, 0, 1) * 2000;
    const moving = g.speed > MAX * 0.01;
    if (moving) g.playerX += g.steer * Math.max(dxw, dt * 2 * 0.2 * 2000);
    g.playerX -= dxw * speedPct * seg.curve * CENTRIFUGAL;

    // ---- gaz / frein / turbo
    const gas = inp.gas && g.state === 'race';
    const brake = inp.brake && g.state === 'race';
    if (inp.boostPressed && !g.boosting && g.gauge >= 0.2 && g.state === 'race') {
      g.boosting = true;
      sfx('boost');
      popup('TURBO !', '#fff36b', 30, 0.46);
    }
    inp.boostPressed = false;
    if (g.boosting) {
      g.gauge -= dt / BOOST_TIME;
      if (g.gauge <= 0) { g.gauge = 0; g.boosting = false; }
    }
    const maxS = MAX * (g.boosting ? BOOST_MULT : 1);
    let acc;
    if (brake) acc = BRAKE;
    else if (g.boosting) acc = g.speed < maxS ? ACCEL * 2.4 : 0;
    else if (gas) acc = g.speed < maxS ? ACCEL * (1 - 0.5 * U.clamp(g.speed / MAX, 0, 1)) : 0;
    else acc = DECEL;
    g.speed += acc * dt;
    if (g.speed > maxS) g.speed = Math.max(maxS, g.speed - MAX * 0.5 * dt);
    g.braking = brake && g.speed > 0;

    // ---- hors piste
    g.offroad = Math.abs(g.playerX) > width + 80;
    if (g.offroad && g.speed > OFFROAD_LIMIT) g.speed += OFFROAD_DECEL * dt;
    g.speed = Math.max(0, g.speed);
    g.playerX = U.clamp(g.playerX, -width - 3600, width + 3600);

    g.position += g.speed * dt;

    // ---- sensations
    const sp = g.speed / MAX;
    const u = Render.H / 360;
    g.bounce = (Math.sin(g.time * 34) * 0.5 * sp + (g.offroad ? (Math.random() - 0.5) * 6 * sp : 0)) * u;
    const targetSquash = brake ? -1 : (gas && sp < 0.6 ? 1 : 0);
    g.squash = U.approach(g.squash, targetSquash * Math.min(1, sp * 3), 4 * dt);
    g.skid = U.clamp(Math.abs(seg.curve) * sp / 5.5 + (brake && sp > 0.5 ? 0.45 : 0) + Math.abs(g.steer) * sp * 0.25, 0, 1);
    if (g.offroad && sp > 0.1) {
      g.bumpT -= dt;
      if (g.bumpT <= 0) { sfx('bump'); g.bumpT = U.rand(0.18, 0.4); }
      if (Math.random() < 0.5) dust('#c8a86a');
    }
    if (g.skid > 0.65 && Math.random() < 0.4) dust('#ffffff');
    if (g.boosting && Math.random() < 0.3) dust('#ffcf70');
  }

  function dust(color) {
    const ps = Render.playerScreen;
    if (!ps) return;
    const side = Math.random() < 0.5 ? -1 : 1;
    g.particles.push({
      kind: 'smoke', x: ps.x + side * ps.w * U.rand(0.25, 0.42), y: ps.y - ps.h * 0.05,
      vx: side * U.rand(20, 60), vy: U.rand(-40, -10), size: U.rand(14, 28), life: 0.6, max: 0.6, alpha: 0.7, grow: 40
    });
    if (g.particles.length > 80) g.particles.shift();
  }

  // ================================================================ collisions
  function overlap(x1, w1, x2, w2) {
    return Math.abs(x1 - x2) < (w1 + w2) / 2;
  }

  function checkCollisions() {
    if (g.crash || g.state !== 'race') return;
    const pz = playerZ();
    const pSeg = Math.floor(pz / SEG);
    const seg = segs[pSeg];
    const width = seg.p1.world.w;
    const offroad = Math.abs(g.playerX) > width * 0.98;

    // décor
    for (let k = pSeg; k <= pSeg + 1; k++) {
      const s = segs[k];
      if (!s) continue;
      for (const sp of s.sprites) {
        if (!sp.hit || sp.arch || sp.knocked) continue;
        if (!offroad && !sp.onRoad) continue;
        const spr = Render.spriteFor(sp);
        const w = (sp.worldW || spr.worldW) * sp.hit;
        if (overlap(g.playerX, PLAYER_HIT_W, sp.offset, w)) {
          if (sp.name === 'cone') { hitCone(sp); continue; }
          if (g.invuln > 0) continue;
          crash(sp.offset > g.playerX ? -1 : 1, 'decor', sp.name + '@' + Math.round(sp.offset));
          return;
        }
      }
      // jetons
      for (const t of s.tokens) {
        if (t.taken) continue;
        if (Math.abs(k * SEG - pz) < 420 && overlap(g.playerX, PLAYER_W, t.x, 700)) {
          t.taken = true;
          g.tokens++;
          g.gauge = Math.min(1, g.gauge + 0.2);
          sfx('pickup');
          popup('+ TURBO', '#ffd23a', 18, 0.55, 0.5 + (Math.random() - 0.5) * 0.1);
        }
      }
    }

    // voitures
    const sp = g.speed / MAX;
    for (const c of g.cars) {
      const dz = c.z - pz;
      const hitLen = (c.len + 460) / 2;
      const crossed = c.prevDz != null && c.prevDz > 0 && dz <= 0;
      const xOverlap = overlap(g.playerX, PLAYER_HIT_W, c.x, c.w * 0.86);
      if ((Math.abs(dz) < hitLen || crossed) && xOverlap) {
        if (g.invuln > 0) { c.prevDz = dz; continue; }
        c.speed *= 0.4;
        c.pulled = 3;
        crash(c.x > g.playerX ? -1 : 1, 'car', c.model + (c.dir > 0 ? '>' : '<') + '@' + Math.round(c.x) + ' dz' + Math.round(dz));
        return;
      }
      // dépassement : frôlé ?
      if (c.prevDz != null && c.prevDz > 0 && dz <= 0 && !c.near) {
        const gap = Math.abs(c.x - g.playerX) - (PLAYER_W + c.w) / 2;
        if (gap < 1500) sfx('whoosh', { pan: U.clamp((c.x - g.playerX) / 2500, -0.8, 0.8), volume: 0.7 });
        if (gap < 520 && sp > 0.45) {
          c.near = true;
          g.nearMisses++;
          g.combo = g.comboT > 0 ? g.combo + 1 : 1;
          g.comboT = 3;
          g.gauge = Math.min(1, g.gauge + 0.12);
          sfx('nearmiss');
          popup(g.combo > 1 ? 'FRÔLÉ ! x' + g.combo : 'FRÔLÉ !', '#7ff0e8', 26, 0.42, 0.5 + (c.x > g.playerX ? 0.15 : -0.15));
        }
      }
      // les voitures d'en face klaxonnent si on est dans leur voie
      if (c.dir < 0 && !c.honked && dz > 0 && dz < 45 * SEG && overlap(g.playerX, PLAYER_W, c.x, c.w)) {
        c.honked = true;
        c.honkT = 1.2;
        sfx('horn', { volume: 0.55, pan: -0.3 });
      }
      c.prevDz = dz;
    }
  }

  function hitCone(sp) {
    sp.knocked = true;
    sp.flyT = 0;
    g.cones++;
    g.penalty += CONE_PENALTY;
    g.speed *= 0.82;
    g.shake = Math.max(g.shake, 0.35);
    sfx('bump');
    popup('PLOT ! +1 s', '#ff9f1c', 18, 0.5, 0.5 + (sp.offset > g.playerX ? 0.12 : -0.12));
    // le plot s'envole
    sp.vx = (sp.offset > g.playerX ? 1 : -1) * U.rand(3000, 5000);
  }

  function crash(dir, cause, what) {
    g.crash = { t: 0, dir: dir, cause: cause };
    (g.crashLog = g.crashLog || []).push({ cause: cause, what: what, seg: Math.floor(playerZ() / SEG), x: Math.round(g.playerX), kmh: Math.round(g.kmh), steer: +g.steer.toFixed(2), curve: +segAt(playerZ()).curve.toFixed(2) });
    g.crashes++;
    g.points = Math.max(0, g.points - CRASH_POINTS);
    g.pointsFlash = 1.5;
    g.boosting = false;
    g.combo = 0;
    g.speed *= 0.45;
    g.shake = 1;
    g.flash = 0.5; g.flashColor = '#ff5a3c';
    sfx('crash');
    vibrate([120, 60, 200]);
    popup('ACCIDENT ! -' + CRASH_POINTS + ' pts', '#ff5a3c', 24, 0.6);
    // étoiles
    const ps = Render.playerScreen;
    if (ps) {
      for (let i = 0; i < 10; i++) {
        g.particles.push({ kind: 'star', x: ps.x, y: ps.y - ps.h * 0.5, vx: U.rand(-300, 300), vy: U.rand(-420, -120), gy: 700, size: U.rand(12, 22), rot: 0, vr: U.rand(-8, 8), life: 1, max: 1 });
      }
    }
  }

  function endCrash() {
    g.crash = null;
    if (g.points <= 0) { gameOver(); return; }
    const pz = playerZ();
    const seg = segAt(pz);
    // replacement sur une voie sûre (sans plot ni barrière juste devant)
    const pSeg = Math.floor(pz / SEG);
    const blocked = (lx) => {
      for (let k = pSeg - 2; k < pSeg + 25 && k < segs.length; k++) {
        if (k < 0) continue;
        for (const sp of segs[k].sprites) if (sp.onRoad && !sp.knocked && Math.abs(sp.offset - lx) < 1100) return true;
      }
      return false;
    };
    const cands = laneXs(seg).slice().sort((a, b) => Math.abs(a - (seg.lanes === 3 ? g.playerX : 1000)) - Math.abs(b - (seg.lanes === 3 ? g.playerX : 1000)));
    g.playerX = cands.find((lx) => !blocked(lx));
    if (g.playerX == null) g.playerX = -1000;
    g.speed = 0;
    g.invuln = 2.2;
    g.steer = 0;
    // on dégage les voitures trop proches
    g.cars = g.cars.filter(c => {
      const dz = c.z - pz;
      if (c.dir > 0) return dz < -400 || dz > 14 * SEG;
      return dz < -400 || dz > 70 * SEG;
    });
    popup('C’EST REPARTI !', '#ffffff', 20, 0.45);
  }

  // ================================================================ radars, zones, arrivée
  function checkRadars() {
    const pz = playerZ();
    for (const r of track.radars) {
      if (r.done || pz < r.seg * SEG) continue;
      r.done = true;
      const kmh = g.kmh;
      if (kmh > r.limit + r.tol && !g.crash) {
        g.flashes++;
        g.points = Math.max(0, g.points - 1);
        g.pointsFlash = 1.2;
        g.penalty += RADAR_PENALTY;
        g.flash = 0.9; g.flashColor = '#ffffff';
        sfx('flash');
        popup('FLASHÉ ! ' + Math.round(kmh) + ' km/h', '#ffffff', 26, 0.38);
        popup('-1 pt · +3 s', '#ff8a80', 16, 0.48);
        for (const sp of segs[r.seg].sprites) if (sp.radar) sp.flashT = 0.7;
        if (g.points <= 0) gameOver();
      } else if (!g.crash) {
        popup('RADAR : PAS VU, PAS PRIS !', '#3fd38a', 16, 0.4);
      }
    }
  }

  function checkZone() {
    const pz = playerZ();
    const z = segAt(pz).zone;
    if (z !== g.zone && pz < track.finishZ) {
      g.zone = z;
      const zone = track.zones[z];
      const split = g.raceTime + g.penalty;
      g.splits[z] = split;
      let splitInfo = null;
      if (g.bestSplits && g.bestSplits[z]) {
        const d = split - g.bestSplits[z];
        splitInfo = { text: (d <= 0 ? '−' : '+') + (Math.abs(d) / 1000).toFixed(2) + ' s', color: d <= 0 ? '#3fd38a' : '#ff6b5e' };
      }
      g.banner = { title: zone.name, sub: zone.sub, t: 3, max: 3, town: zone.id === 'cornillon' || zone.id === 'lancon', split: splitInfo };
      sfx('checkpoint');
    }
  }

  function finish() {
    g.state = 'finished';
    g.finalTime = g.raceTime + g.penalty;
    g.boosting = false;
    g.finishT = 0;
    sfx('finish');
    vibrate(80);
    if (window.AudioFX) AudioFX.music(null);
    // confettis
    for (let i = 0; i < 140; i++) {
      g.particles.push({
        kind: 'confetti', x: U.rand(0, Render.W), y: U.rand(-Render.H * 0.6, 0), vx: U.rand(-40, 40), vy: U.rand(80, 220),
        size: U.rand(6, 12), rot: U.rand(0, 6), vr: U.rand(-6, 6), life: 4, max: 4,
        color: U.pick(['#ff3b30', '#ffd23a', '#3fd38a', '#20c9c0', '#5b3df5', '#ff7ac6'])
      });
    }
    // record ?
    const best = U.store.get('carpolo.best', null);
    g.newRecord = best == null || g.finalTime < best;
    if (g.newRecord) {
      U.store.set('carpolo.best', g.finalTime);
      U.store.set('carpolo.bestSplits', g.splits);
      g.best = g.finalTime;
    }
    const stats = U.store.get('carpolo.stats', { runs: 0, finished: 0, clean: 0 });
    stats.runs++; stats.finished++;
    if (g.crashes === 0) stats.clean++;
    U.store.set('carpolo.stats', stats);
    setTimeout(() => { if (g.state === 'finished') showResults(); }, 2300);
  }

  function gameOver() {
    if (g.state === 'gameover') return;
    g.state = 'gameover';
    g.finalTime = g.raceTime + g.penalty;
    g.boosting = false;
    sfx('gameover');
    if (window.AudioFX) AudioFX.music(null);
    const stats = U.store.get('carpolo.stats', { runs: 0, finished: 0, clean: 0 });
    stats.runs++;
    U.store.set('carpolo.stats', stats);
    setTimeout(() => { if (g.state === 'gameover') showResults(); }, 1600);
  }

  // ================================================================ boucle
  function update(dt) {
    g.time += dt;
    g.cloudDrift += dt * 0.004;

    // effets
    g.flash = Math.max(0, g.flash - dt * 2.2);
    g.shake = Math.max(0, g.shake - dt * 2.5);
    g.pointsFlash = Math.max(0, g.pointsFlash - dt);
    g.hornT = Math.max(0, g.hornT - dt);
    g.hornCd = Math.max(0, g.hornCd - dt);
    g.goT = Math.max(0, g.goT - dt);
    if (g.invuln > 0 && !g.crash) g.invuln -= dt;
    if (g.comboT > 0) g.comboT -= dt;
    if (g.banner) g.banner.t -= dt;
    if (g.pendingBanner && (g.pendingBanner.delay -= dt) <= 0) { g.banner = g.pendingBanner; g.pendingBanner = null; }
    for (let i = g.popups.length - 1; i >= 0; i--) { g.popups[i].t -= dt; if (g.popups[i].t <= 0) g.popups.splice(i, 1); }
    for (let i = g.particles.length - 1; i >= 0; i--) {
      const p = g.particles[i];
      p.life -= dt;
      if (p.life <= 0) { g.particles.splice(i, 1); continue; }
      p.x += (p.vx || 0) * dt; p.y += (p.vy || 0) * dt;
      if (p.gy) p.vy += p.gy * dt;
      if (p.vr) p.rot += p.vr * dt;
      if (p.grow) p.size += p.grow * dt;
      if (p.kind === 'confetti') p.x += Math.sin(g.time * 3 + p.rot) * 30 * dt;
    }

    if (g.state === 'title' || g.state === 'boot') {
      g.speed = 0;
      g.bounce = Math.sin(g.time * 30) * 0.4;
      updateCamera();
      engineAudio(dt, false);
      return;
    }
    if (g.state === 'paused') { engineAudio(dt, false); return; }

    if (g.state === 'countdown') {
      const prev = g.countdown;
      g.countdown -= dt;
      const n = Math.ceil(g.countdown);
      if (n !== Math.ceil(prev) && n > 0) sfx('beep');
      if (prev > 3 && n === 3) sfx('beep');
      // départ canon : gaz enfoncé pendant le dernier "1" (pas avant)
      if (g.input.gas && g.countdown > 0.55) g.gasEarly = true;
      if (!g.input.gas) g.gasEarly = false;
      if (g.countdown <= 0) {
        g.state = 'race';
        g.goT = 1.2;
        sfx('go');
        if (g.input.gas && !g.gasEarly) {
          g.speed = MAX * 0.45;
          g.gauge = Math.min(1, g.gauge + 0.35);
          popup('DÉPART CANON !', '#fff36b', 26, 0.45);
          sfx('nearmiss');
        }
        g.pendingBanner = { delay: 1.1, title: track.zones[0].name, sub: track.zones[0].sub, t: 3, max: 3, town: false };
      }
      updateCars(dt);
      assignCarsToSegments();
      updateCamera();
      engineAudio(dt, true);
      return;
    }

    if (g.state === 'race') {
      g.raceTime += dt * 1000;
      if (g.input.hornPressed) { horn(); }
      g.input.hornPressed = false;
      updatePlayer(dt);
      spawnTraffic();
      updateCars(dt);
      checkCollisions();
      checkRadars();
      checkZone();
      if (playerZ() >= track.finishZ && g.state === 'race') finish();
    } else if (g.state === 'finished' || g.state === 'gameover') {
      // pilote automatique : on ralentit doucement
      g.input.boostPressed = false;
      if (g.crash) { g.crash.t += dt; if (g.crash.t > 1.5) g.crash = null; }
      g.speed = U.approach(g.speed, g.state === 'finished' ? MAX * 0.25 : 0, MAX * 0.5 * dt);
      const seg = segAt(playerZ());
      g.playerX = U.approach(g.playerX, seg.lanes === 3 ? 0 : 1000, 1500 * dt);
      g.playerX -= dt * 2 * (g.speed / MAX) * 2000 * (g.speed / MAX) * seg.curve * CENTRIFUGAL * 0.6;
      g.position = Math.min(g.position + g.speed * dt, (segs.length - Render.DRAW - 5) * SEG);
      g.steer = U.approach(g.steer, 0, 3 * dt);
      g.braking = g.state === 'gameover';
      g.boosting = false;
      updateCars(dt);
    }

    // décor animé : plots renversés, flash radar
    const base = Math.floor(g.position / SEG);
    for (let i = base; i < Math.min(segs.length, base + 40); i++) {
      for (const sp of segs[i].sprites) {
        if (sp.flashT > 0) sp.flashT -= dt;
        if (sp.knocked && sp.vx) { sp.offset += sp.vx * dt; sp.vx *= 0.96; }
      }
    }

    g.kmh = g.speed * KMH;
    g.speedPct = g.speed / MAX;
    g.fovBoost = U.approach(g.fovBoost, g.boosting ? 16 : (g.speedPct > 0.9 ? 4 : 0), 30 * dt);

    // parallaxe du décor lointain
    const seg = segAt(playerZ());
    const travel = g.speed * dt / SEG;
    const rates = [0.0006, 0.001, 0.0018, 0.003];
    for (let i = 0; i < 4; i++) g.bgOffset[i] += rates[i] * seg.curve * travel;

    assignCarsToSegments();
    updateCamera();
    engineAudio(dt, g.state === 'race' || (g.state === 'finished' && !g.resultsShown));
  }

  function engineAudio(dt, running) {
    if (!window.AudioFX) return;
    AudioFX.update(dt, {
      running: running && g.state !== 'boot',
      speed: (g.speed || 0) / MAX,
      throttle: g.state === 'race' && g.input.gas ? 1 : (g.boosting ? 1 : 0),
      offroad: !!g.offroad && g.state === 'race',
      skid: g.state === 'race' ? (g.skid || 0) : 0,
      boost: !!g.boosting
    });
  }

  function horn() {
    if (g.hornCd > 0 || g.crash) return;
    g.hornCd = 1.6;
    g.hornT = 0.9;
    sfx('horn');
    // la voiture devant nous se pousse
    const pz = playerZ();
    let best = null;
    for (const c of g.cars) {
      if (c.dir < 0) continue;
      const dz = c.z - pz;
      if (dz > 0 && dz < 40 * SEG && Math.abs(c.x - g.playerX) < 1300 && (!best || dz < best.z - pz)) best = c;
    }
    if (best) {
      const seg = segAt(best.z);
      if (seg.lanes === 3) {
        if (best.lane == null) best.lane = 1;
        const order = best.lane === 2 ? [1] : [best.lane + 1, best.lane - 1];
        for (const l of order) {
          if (l >= 0 && l <= 2 && laneFree([-2000, 0, 2000][l], best.z, best)) { best.lane = l; best.laneCd = 4; break; }
        }
      } else {
        best.pulled = 4;
      }
      best.honkT = 0.01;
    }
  }

  // ================================================================ rendu
  function render() {
    Render.frame(g);
    g.layout = HUD.layout(Render.W, Render.H, settings.tilt && g.tiltOK);
    HUD.draw(ctx, g, Render.W, Render.H);
  }

  // ================================================================ entrées
  const keys = {};
  const pointers = new Map();

  function computeInput() {
    const inp = g.input;
    const prevBoost = inp.boost, prevHorn = inp.horn, prevPause = inp.pause;
    const t = { left: false, right: false, gas: false, brake: false, boost: false, horn: false, pause: false };
    const L = g.layout || HUD.layout(Render.W, Render.H, settings.tilt && g.tiltOK);
    pointers.forEach((p) => {
      const id = HUD.hit(L, Render.W, Render.H, p.x, p.y);
      if (id) t[id] = true;
    });
    inp.left = t.left || keys.ArrowLeft || keys.KeyA || keys.KeyQ;
    inp.right = t.right || keys.ArrowRight || keys.KeyD;
    inp.gas = t.gas || keys.ArrowUp || keys.KeyW || keys.KeyZ;
    inp.brake = t.brake || keys.ArrowDown || keys.KeyS;
    inp.boost = t.boost || keys.Space || keys.ShiftLeft;
    inp.horn = t.horn || keys.KeyK || keys.KeyH;
    inp.pause = t.pause;
    if (g.bot) g.bot(g, inp);
    if (inp.boost && !prevBoost) inp.boostPressed = true;
    if (inp.horn && !prevHorn) inp.hornPressed = true;
    if (inp.pause && !prevPause && (g.state === 'race' || g.state === 'countdown')) pause();
  }

  function toCanvas(e) {
    const r = canvas.getBoundingClientRect();
    return { x: (e.clientX - r.left) * (Render.W / r.width), y: (e.clientY - r.top) * (Render.H / r.height) };
  }

  function setupInput() {
    canvas.addEventListener('pointerdown', (e) => {
      if (e.pointerType === 'touch' || e.pointerType === 'pen') g.showTouch = true;
      if (window.AudioFX) AudioFX.unlock();
      try { canvas.setPointerCapture(e.pointerId); } catch (err) { /* rien */ }
      pointers.set(e.pointerId, toCanvas(e));
      computeInput();
      e.preventDefault();
    }, { passive: false });
    canvas.addEventListener('pointermove', (e) => {
      if (!pointers.has(e.pointerId)) return;
      pointers.set(e.pointerId, toCanvas(e));
      e.preventDefault();
    }, { passive: false });
    const up = (e) => { pointers.delete(e.pointerId); computeInput(); };
    canvas.addEventListener('pointerup', up);
    canvas.addEventListener('pointercancel', up);
    canvas.addEventListener('lostpointercapture', up);
    canvas.addEventListener('contextmenu', (e) => e.preventDefault());
    document.addEventListener('touchmove', (e) => { if (e.target === canvas) e.preventDefault(); }, { passive: false });

    // un doigt sur l'écran (même sur un menu) -> on affiche les commandes tactiles
    window.addEventListener('pointerdown', (e) => { if (e.pointerType === 'touch' || e.pointerType === 'pen') g.showTouch = true; }, true);
    window.addEventListener('touchstart', () => { g.showTouch = true; }, { capture: true, passive: true });
    window.addEventListener('keydown', (e) => {
      keys[e.code] = true;
      if (window.AudioFX) AudioFX.unlock();
      if (e.code === 'KeyP' || e.code === 'Escape') {
        if (g.state === 'race' || g.state === 'countdown') pause();
        else if (g.state === 'paused') resume();
      }
      if (e.code === 'Enter' && g.state === 'title') startCountdown();
      if (['ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight', 'Space'].includes(e.code)) e.preventDefault();
    });
    window.addEventListener('keyup', (e) => { keys[e.code] = false; });
    window.addEventListener('blur', () => { for (const k in keys) keys[k] = false; pointers.clear(); });

    // inclinaison du téléphone
    window.addEventListener('deviceorientation', (e) => {
      if (e.beta == null) return;
      g.tiltOK = true;
      const angle = (screen.orientation && screen.orientation.angle != null) ? screen.orientation.angle : (window.orientation || 0);
      let v;
      if (angle === 90) v = e.beta;
      else if (angle === -90 || angle === 270) v = -e.beta;
      else v = e.gamma;
      g.tiltSteer = U.clamp((v || 0) / 20, -1, 1);
    });

    document.addEventListener('visibilitychange', () => {
      if (document.hidden && (g.state === 'race' || g.state === 'countdown')) pause();
    });
  }

  // ================================================================ écrans (DOM)
  const $ = (id) => document.getElementById(id);
  function showScreen(id) {
    document.querySelectorAll('.screen').forEach((s) => s.classList.toggle('hidden', s.id !== id));
  }

  function pause() {
    if (g.state !== 'race' && g.state !== 'countdown') return;
    g.prevState = g.state;
    g.state = 'paused';
    pointers.clear();
    if (window.AudioFX) AudioFX.pauseAll();
    showScreen('scr-pause');
  }
  function resume() {
    if (g.state !== 'paused') return;
    g.state = g.prevState || 'race';
    for (const k in g.input) if (typeof g.input[k] === 'boolean') g.input[k] = false;
    if (window.AudioFX) AudioFX.resumeAll();
    showScreen(null);
  }
  function toTitle() {
    resetRace();
    g.state = 'title';
    pointers.clear();
    if (window.AudioFX) { AudioFX.resumeAll(); AudioFX.music('menu'); }
    refreshTitle();
    showScreen('scr-title');
  }

  function medalFor(t) {
    for (const m of MEDALS) if (t <= m.time) return m;
    return null;
  }

  function showResults() {
    g.resultsShown = true;
    const over = g.state === 'gameover';
    $('res-title').textContent = over ? 'PERMIS ANNULÉ !' : 'ARRIVÉE !';
    $('res-title').className = over ? 'big-title red' : 'big-title';
    $('res-sub').textContent = over
      ? 'Trop d’accidents… La C4 part à la fourrière 🚨'
      : (g.crashes === 0 ? 'Conduite sans accident, bravo pilote !' : 'Arrivé·e… avec quelques bosses !');
    $('res-time').textContent = over ? '—' : U.fmtTime(g.finalTime);
    $('res-pen').textContent = g.penalty > 0 ? 'dont ' + (g.penalty / 1000).toFixed(0) + ' s de pénalités' : 'aucune pénalité';
    const m = over ? null : medalFor(g.finalTime);
    const medal = $('res-medal');
    medal.className = 'medal ' + (m ? m.id : 'none');
    medal.textContent = m ? m.name : (over ? '0 pt' : '—');
    $('res-record').classList.toggle('hidden', !(g.newRecord && !over));
    $('st-crash').textContent = g.crashes;
    $('st-near').textContent = g.nearMisses;
    $('st-tok').textContent = g.tokens;
    $('st-flash').textContent = g.flashes;
    $('st-pts').textContent = g.points + ' / 12';
    const clean = !over && g.crashes === 0;
    $('res-clean').classList.toggle('hidden', !clean);
    const next = m && m.id !== 'or' ? MEDALS[MEDALS.indexOf(m) - 1] : (!m && !over ? MEDALS[2] : null);
    $('res-next').textContent = next ? 'Prochaine médaille (' + next.name + ') : sous ' + U.fmtTime(next.time) : (m && m.id === 'or' ? 'Médaille d’or : la Provence est à toi !' : '');
    showScreen('scr-results');
    if (window.AudioFX) AudioFX.music('menu');
  }

  function refreshTitle() {
    g.best = U.store.get('carpolo.best', null);
    g.bestSplits = U.store.get('carpolo.bestSplits', null);
    $('t-best').textContent = g.best ? 'Record : ' + U.fmtTime(g.best) + (medalFor(g.best) ? ' · ' + medalFor(g.best).name : '') : 'Pas encore de record';
    $('opt-music').checked = window.AudioFX ? AudioFX.isMusicEnabled() : false;
    $('opt-sfx').checked = window.AudioFX ? AudioFX.isSfxEnabled() : false;
    $('opt-tilt').checked = !!settings.tilt;
    drawTitleMap();
  }

  function drawTitleMap() {
    const c = $('t-map');
    if (!c) return;
    const dpr = Math.min(2, window.devicePixelRatio || 1);
    const size = c.clientWidth || 160;
    c.width = size * dpr; c.height = size * dpr;
    const x = c.getContext('2d');
    HUD.drawMinimap(x, track, 0, 0, 0, size * dpr, dpr * 1.1, { places: true, noPanel: true });
  }

  function setupUI() {
    const click = (id, fn) => $(id).addEventListener('click', (e) => {
      if (window.AudioFX) { AudioFX.unlock(); AudioFX.play('click'); }
      fn(e);
    });
    click('btn-play', () => { tryFullscreen(); startCountdown(); });
    click('btn-help', () => showScreen('scr-help'));
    click('btn-help-back', () => showScreen('scr-title'));
    click('btn-resume', resume);
    click('btn-restart', () => startCountdown());
    click('btn-quit', toTitle);
    click('btn-again', () => startCountdown());
    click('btn-menu', toTitle);
    $('opt-music').addEventListener('change', (e) => { if (window.AudioFX) { AudioFX.unlock(); AudioFX.setMusicEnabled(e.target.checked); } });
    $('opt-sfx').addEventListener('change', (e) => { if (window.AudioFX) { AudioFX.unlock(); AudioFX.setSfxEnabled(e.target.checked); } });
    $('opt-tilt').addEventListener('change', (e) => {
      settings.tilt = e.target.checked; saveSettings();
      // iOS : autorisation nécessaire
      if (settings.tilt && window.DeviceOrientationEvent && typeof DeviceOrientationEvent.requestPermission === 'function') {
        DeviceOrientationEvent.requestPermission().catch(() => {});
      }
    });
  }

  function tryFullscreen() {
    // seulement dans un navigateur mobile (l'appli Android est déjà en plein écran)
    if (!g.showTouch || /CarPoloApp/.test(navigator.userAgent)) return;
    try {
      const el = document.documentElement;
      if (!document.fullscreenElement && el.requestFullscreen) {
        el.requestFullscreen({ navigationUI: 'hide' }).then(() => {
          if (screen.orientation && screen.orientation.lock) screen.orientation.lock('landscape').catch(() => {});
        }).catch(() => {});
      }
    } catch (e) { /* rien */ }
  }

  // hooks Android (bouton retour, mise en arrière-plan)
  g.onBack = function () {
    if (g.state === 'race' || g.state === 'countdown') { pause(); return 'handled'; }
    if (g.state === 'paused') { resume(); return 'handled'; }
    if (!$('scr-help').classList.contains('hidden')) { showScreen('scr-title'); return 'handled'; }
    if (g.state === 'finished' || g.state === 'gameover') { toTitle(); return 'handled'; }
    return 'exit';
  };
  g.onAppPause = function () {
    if (g.state === 'race' || g.state === 'countdown') pause();
    if (window.AudioFX) AudioFX.pauseAll();
  };
  g.onAppResume = function () {
    if (window.AudioFX && g.state !== 'paused') AudioFX.resumeAll();
  };

  // ================================================================ taille / qualité
  let targetH = 0;
  function resize() {
    const cssW = window.innerWidth, cssH = window.innerHeight;
    const dpr = window.devicePixelRatio || 1;
    const cap = settings.quality === 'low' ? 360 : 640;
    if (!targetH) targetH = cap;
    const s = Math.min(dpr, targetH / Math.min(cssW, cssH));
    Render.resize(cssW * s, cssH * s);
    canvas.style.width = cssW + 'px';
    canvas.style.height = cssH + 'px';
    g.layout = HUD.layout(Render.W, Render.H, settings.tilt && g.tiltOK);
    if (g.state === 'title') drawTitleMap();
  }

  // ================================================================ démarrage
  let last = 0, acc = 0, frames = 0, slowTime = 0, perfT = 0;
  function loop(now) {
    requestAnimationFrame(loop);
    if (!last) last = now;
    let dt = (now - last) / 1000;
    last = now;
    if (dt > 0.25) dt = 0.25;
    dt *= g.timeScale || 1;
    // qualité adaptative : si le téléphone peine, on baisse la résolution
    perfT += dt; frames++;
    if (dt > 0.024) slowTime += dt;
    if (perfT > 2) {
      if (!g.timeScale && slowTime / perfT > 0.5 && targetH > 300 && settings.quality === 'auto' && (g.state === 'race' || g.state === 'title')) {
        targetH = Math.round(Math.max(300, Math.min(targetH, Render.H) * 0.85));
        resize();
      }
      perfT = 0; slowTime = 0; frames = 0;
    }
    acc += dt;
    let steps = 0;
    const maxSteps = 6 * (g.timeScale || 1);
    while (acc >= STEP && steps < maxSteps) {
      computeInput();
      update(STEP);
      acc -= STEP;
      steps++;
    }
    if (steps >= maxSteps) acc = 0;
    render();
  }

  function boot() {
    canvas = document.getElementById('c');
    if (window.Sprites) Sprites.init();
    Render.init(canvas);
    ctx = Render.ctx;
    setupInput();
    setupUI();
    resetRace();
    resize();
    window.addEventListener('resize', resize);
    window.addEventListener('orientationchange', () => setTimeout(resize, 200));
    g.state = 'title';
    refreshTitle();
    showScreen('scr-title');
    if (window.AudioFX) AudioFX.music('menu');
    document.body.classList.add('ready');
    requestAnimationFrame(loop);
  }

  g.debug = { MAX: MAX, startCountdown: startCountdown, pause: pause, resume: resume, toTitle: toTitle, finish: () => finish(), setPos: (f) => { g.position = f * track.finishZ; } };
  g.pause = pause; g.resume = resume;

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', boot);
  else boot();
})();
