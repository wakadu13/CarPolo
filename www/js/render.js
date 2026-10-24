/* CarPolo — rendu pseudo-3D (route à segments façon OutRun, style cartoon) */
(function () {
  'use strict';

  const DRAW = 300;               // segments dessinés
  const CAMERA_HEIGHT = 1000;
  const PLAYER_Z = 1600;          // distance caméra -> voiture
  const BASE_FOV = 100;

  const R = {
    DRAW: DRAW, CAMERA_HEIGHT: CAMERA_HEIGHT, PLAYER_Z: PLAYER_Z,
    canvas: null, ctx: null, W: 0, H: 0, K: 0, horizon: 0, depth: 1
  };

  let ctx, W, H, K, horizon, depth;
  const own = {};                 // sprites dessinés ici (épingle, point de départ)
  let skyCache = { key: '', grad: null };

  // ---------------------------------------------------------------- init
  R.init = function (canvas) {
    R.canvas = canvas;
    R.ctx = ctx = canvas.getContext('2d', { alpha: false });
    buildOwnSprites();
  };

  R.resize = function (w, h) {
    R.canvas.width = W = R.W = Math.max(2, Math.round(w));
    R.canvas.height = H = R.H = Math.max(2, Math.round(h));
    // échelle uniforme : en paysage on garde ~ la même taille de voiture quel que soit l'écran
    R.K = K = Math.min(W / 2, H * 0.82);
    R.horizon = horizon = Math.round(H * 0.47);
    skyCache.key = '';
  };

  function buildOwnSprites() {
    // Épingle rouge façon carte (arrivée)
    let c = document.createElement('canvas'); c.width = 220; c.height = 340;
    let x = c.getContext('2d');
    x.lineJoin = 'round';
    x.fillStyle = 'rgba(0,0,0,0.25)';
    x.beginPath(); x.ellipse(110, 326, 46, 11, 0, 0, Math.PI * 2); x.fill();
    x.beginPath();
    x.moveTo(110, 322);
    x.bezierCurveTo(70, 250, 18, 190, 18, 112);
    x.arc(110, 112, 92, Math.PI, 0);
    x.bezierCurveTo(202, 190, 150, 250, 110, 322);
    x.closePath();
    x.fillStyle = '#ea4335'; x.fill();
    x.lineWidth = 9; x.strokeStyle = '#1b1b2f'; x.stroke();
    x.fillStyle = '#ff7b6e';
    x.beginPath(); x.ellipse(70, 80, 18, 30, -0.5, 0, Math.PI * 2); x.fill();
    x.beginPath(); x.arc(110, 112, 34, 0, Math.PI * 2); x.fillStyle = '#a3261c'; x.fill();
    x.lineWidth = 6; x.stroke();
    own.pin = { img: c, worldW: 1300 };

    // Point de départ (cercle blanc de la carte) sur un poteau
    c = document.createElement('canvas'); c.width = 200; c.height = 300;
    x = c.getContext('2d');
    x.fillStyle = 'rgba(0,0,0,0.25)';
    x.beginPath(); x.ellipse(100, 290, 40, 9, 0, 0, Math.PI * 2); x.fill();
    x.fillStyle = '#9aa0ad'; x.strokeStyle = '#1b1b2f'; x.lineWidth = 7;
    x.beginPath(); x.rect(90, 150, 20, 140); x.fill(); x.stroke();
    x.beginPath(); x.arc(100, 95, 80, 0, Math.PI * 2); x.fillStyle = '#ffffff'; x.fill();
    x.lineWidth = 10; x.stroke();
    x.beginPath(); x.arc(100, 95, 46, 0, Math.PI * 2); x.lineWidth = 16; x.strokeStyle = '#3c4043'; x.stroke();
    own.startdot = { img: c, worldW: 900 };

    // Panneau de remplacement (si un sprite manque)
    c = document.createElement('canvas'); c.width = 64; c.height = 64;
    x = c.getContext('2d'); x.fillStyle = '#ff00ff'; x.fillRect(0, 0, 64, 64);
    own.missing = { img: c, worldW: 600 };
  }

  function spriteFor(sp) {
    if (sp.town) {
      const s = window.Sprites && Sprites.townSign ? Sprites.townSign(sp.town, !!sp.ended) : null;
      return s ? { img: s.img, worldW: sp.worldW || s.worldW } : own.missing;
    }
    if (own[sp.name]) return own[sp.name];
    const s = window.Sprites && Sprites.get(sp.name);
    return (s && s.img) ? s : own.missing;
  }
  R.spriteFor = spriteFor;
  R.ownSprite = (n) => own[n];

  // ---------------------------------------------------------------- projection
  function project(p, camX, camY, camZ) {
    const cz = p.world.z - camZ;
    p.camera.z = cz;
    const s = depth / cz;
    p.screen.scale = s;
    p.screen.x = Math.round(W / 2 - s * camX * K);
    p.screen.y = Math.round(horizon - s * (p.world.y - camY) * K);
    p.screen.w = Math.round(s * p.world.w * K);
  }

  function poly(x1, y1, x2, y2, x3, y3, x4, y4, color) {
    ctx.fillStyle = color;
    ctx.beginPath();
    ctx.moveTo(x1, y1); ctx.lineTo(x2, y2); ctx.lineTo(x3, y3); ctx.lineTo(x4, y4);
    ctx.closePath();
    ctx.fill();
  }

  // ---------------------------------------------------------------- ciel & décor lointain
  function skyColors(progress) {
    // après-midi ensoleillé -> lumière dorée vers l'arrivée
    const t = U.smooth((progress - 0.55) / 0.45) * 0.6;
    return {
      top: U.mixHex('#2f9bf0', '#5a8fe0', t),
      mid: U.mixHex('#6cc6fb', '#f2b07a', t),
      bottom: U.mixHex('#c8f1ff', '#ffe2a8', t),
      fog: U.mixHex('#d6f3ff', '#ffe9c2', t)
    };
  }
  R.skyColors = skyColors;

  function drawBackground(g, sky) {
    const key = sky.top + sky.bottom + H;
    if (skyCache.key !== key) {
      const gr = ctx.createLinearGradient(0, 0, 0, horizon + H * 0.05);
      gr.addColorStop(0, sky.top);
      gr.addColorStop(0.6, sky.mid);
      gr.addColorStop(1, sky.bottom);
      skyCache = { key: key, grad: gr };
    }
    ctx.fillStyle = skyCache.grad;
    ctx.fillRect(0, 0, W, horizon + 2);

    const S = window.Sprites && Sprites.bg;
    const vshift = U.clamp((g.camY - CAMERA_HEIGHT) * 0.000012 * H, -H * 0.06, H * 0.06);

    // soleil souriant
    if (S && S.sun) {
      const sz = H * 0.24;
      const sx = W * 0.78 - (g.bgOffset[0] % 1) * W * 0.25;
      ctx.drawImage(S.sun, ((sx % (W * 1.3)) + W * 1.3) % (W * 1.3) - sz * 0.3, H * 0.03, sz, sz);
    }
    // nuages
    if (S && S.clouds && S.clouds.length) {
      for (let i = 0; i < 5; i++) {
        const img = S.clouds[i % S.clouds.length];
        const cw = H * (0.34 + (i % 3) * 0.08), ch = cw * img.height / img.width;
        const span = W + cw * 2;
        let x = ((i * 0.37 + g.bgOffset[0] * 0.6 + g.cloudDrift * (0.6 + i * 0.1)) % 1) * span;
        x = ((x % span) + span) % span - cw;
        ctx.drawImage(img, x, H * (0.05 + (i * 0.071) % 0.2), cw, ch);
      }
    }
    // couches de collines (parallaxe)
    if (S) {
      const layers = [[S.far, 0.30, 1], [S.mid, 0.2, 2], [S.near, 0.13, 3]];
      for (const [img, hf, idx] of layers) {
        if (!img) continue;
        const lh = H * hf, lw = lh * img.width / img.height;
        const y = horizon - lh + H * 0.012 + vshift * (idx / 3);
        let off = (g.bgOffset[idx] % 1) * lw;
        if (off < 0) off += lw;
        for (let x = -off; x < W; x += lw) ctx.drawImage(img, Math.floor(x), y, Math.ceil(lw) + 1, lh);
      }
    }
  }

  // ---------------------------------------------------------------- segments
  function drawSegment(seg, sky) {
    const a = seg.p1.screen, b = seg.p2.screen;
    const y1 = a.y, y2 = b.y;
    // herbe
    ctx.fillStyle = seg.grass;
    ctx.fillRect(0, y2, W, y1 - y2 + 1);
    // bas-côtés (bandes rouges et blanches)
    const r1 = a.w * 0.085, r2 = b.w * 0.085;
    poly(a.x - a.w - r1, y1 + 1, a.x - a.w, y1 + 1, b.x - b.w, y2, b.x - b.w - r2, y2, seg.rumble);
    poly(a.x + a.w + r1, y1 + 1, a.x + a.w, y1 + 1, b.x + b.w, y2, b.x + b.w + r2, y2, seg.rumble);
    // route
    poly(a.x - a.w, y1 + 1, a.x + a.w, y1 + 1, b.x + b.w, y2, b.x - b.w, y2, seg.road);

    // lignes de rive continues
    const e1 = a.w * 0.03, e2 = b.w * 0.03;
    const ein1 = a.w * 0.93, ein2 = b.w * 0.93;
    poly(a.x - ein1 - e1, y1 + 1, a.x - ein1, y1 + 1, b.x - ein2, y2, b.x - ein2 - e2, y2, '#f4f4f4');
    poly(a.x + ein1 + e1, y1 + 1, a.x + ein1, y1 + 1, b.x + ein2, y2, b.x + ein2 + e2, y2, '#f4f4f4');

    // damier départ / arrivée
    if (seg.start || seg.finishLine) {
      const n = 10;
      for (let k = 0; k < n; k++) {
        if ((k + seg.index) % 2) continue;
        const f1 = -1 + 2 * k / n, f2 = -1 + 2 * (k + 1) / n;
        poly(a.x + a.w * f1, y1 + 1, a.x + a.w * f2, y1 + 1, b.x + b.w * f2, y2, b.x + b.w * f1, y2, '#ffffff');
      }
      for (let k = 0; k < n; k++) {
        if (!((k + seg.index) % 2)) continue;
        const f1 = -1 + 2 * k / n, f2 = -1 + 2 * (k + 1) / n;
        poly(a.x + a.w * f1, y1 + 1, a.x + a.w * f2, y1 + 1, b.x + b.w * f2, y2, b.x + b.w * f1, y2, '#1b1b2f');
      }
    } else if (seg.lane) {
      // marquage central / voies (tirets)
      const l1 = a.w * 0.022, l2 = b.w * 0.022;
      const color = seg.works ? '#ffd23a' : seg.lane;
      if (seg.lanes === 3 || (seg.lanes === 0 && seg.p1.world.w > 2600)) {
        for (const f of [-1 / 3, 1 / 3]) {
          const ax = a.x + a.w * f, bx = b.x + b.w * f;
          poly(ax - l1, y1 + 1, ax + l1, y1 + 1, bx + l2, y2, bx - l2, y2, color);
        }
      } else if (!seg.roundabout) {
        poly(a.x - l1, y1 + 1, a.x + l1, y1 + 1, b.x + l2, y2, b.x - l2, y2, color);
      }
    }
    // brume au loin
    if (seg.fog > 0.01) {
      ctx.globalAlpha = seg.fog;
      ctx.fillStyle = sky.fog;
      ctx.fillRect(0, y2, W, y1 - y2 + 1);
      ctx.globalAlpha = 1;
    }
  }

  // ---------------------------------------------------------------- sprites
  function drawSprite(sp, scale, x, y, clipY, alpha, lift, squash) {
    const img = sp.img;
    let destW = sp.worldW * scale * K;
    if (destW < 0.6 || destW > W * 5) return;
    const destH = destW * img.height / img.width;
    const sw = squash != null ? destW * squash : destW;
    const dx = x - sw / 2;
    const dy = y - destH - (lift || 0) * scale * K;
    if (dx > W || dx + sw < 0 || dy > H) return;
    let clipH = 0;
    if (clipY != null) clipH = Math.max(0, dy + destH - clipY);
    if (clipH >= destH) return;
    const srcH = img.height * (destH - clipH) / destH;
    if (srcH < 0.5) return;
    if (alpha < 1) ctx.globalAlpha = alpha;
    ctx.drawImage(img, 0, 0, img.width, srcH, dx, dy, sw, destH - clipH);
    if (alpha < 1) ctx.globalAlpha = 1;
  }
  R.drawSprite = drawSprite;

  // ---------------------------------------------------------------- joueur
  function drawPlayer(g) {
    const S = window.Sprites;
    const dir = g.steer < -0.3 ? 'l' : g.steer > 0.3 ? 'r' : 'c';
    const name = 'c4_' + dir + (g.braking ? '_b' : '');
    const sp = (S && S.get(name)) || own.missing;
    if (!sp || !sp.img) return;
    const scale = depth / PLAYER_Z;
    const destW = sp.worldW * scale * K;
    const destH = destW * sp.img.height / sp.img.width;
    let cx = W / 2;
    let by = horizon + scale * (g.camY - g.playerY) * K;   // sol sous la voiture
    // rebond sur la route
    by += g.bounce;
    let rot = g.steer * 0.035;
    let sx = 1, sy = 1;
    if (g.crash) {
      const t = g.crash.t;
      const k = U.easeOut(Math.min(1, t / 1.1));
      rot = g.crash.dir * k * Math.PI * 4;
      by -= Math.sin(Math.min(1, t / 0.9) * Math.PI) * destH * 0.6;
      cx += g.crash.dir * k * destW * 0.15;
    } else {
      // squash & stretch cartoon
      sx = 1 + g.squash * 0.05; sy = 1 - g.squash * 0.05;
    }
    if (g.invuln > 0 && !g.crash && Math.floor(g.invuln * 10) % 2 === 0) ctx.globalAlpha = 0.45;

    ctx.save();
    ctx.translate(cx, by - destH * 0.45);
    ctx.rotate(rot);
    ctx.scale(sx, sy);
    const x0 = -destW / 2, y0 = -destH * 0.55;
    ctx.drawImage(sp.img, x0, y0, destW, destH);

    // flammes du turbo
    if (g.boosting && !g.crash && sp.exhaust) {
      for (const e of sp.exhaust) {
        drawFlame(x0 + e[0] * destW, y0 + e[1] * destH, destW * 0.11, g.time);
      }
    }
    ctx.restore();
    ctx.globalAlpha = 1;
    R.playerScreen = { x: cx, y: by, w: destW, h: destH };
  }

  function drawFlame(x, y, size, t) {
    const f = 0.75 + 0.35 * Math.sin(t * 60 + x);
    const len = size * (1.4 + 0.6 * Math.random()) * f;
    ctx.save();
    ctx.translate(x, y);
    ctx.lineJoin = 'round';
    const layers = [['#ff5a1f', 1], ['#ffb21f', 0.68], ['#fff6a8', 0.38]];
    for (const [col, k] of layers) {
      ctx.beginPath();
      const w = size * 0.55 * k, l = len * k;
      ctx.moveTo(-w, 0);
      ctx.quadraticCurveTo(-w * 1.3, l * 0.5, (Math.random() - 0.5) * w * 0.6, l);
      ctx.quadraticCurveTo(w * 1.3, l * 0.5, w, 0);
      ctx.closePath();
      ctx.fillStyle = col;
      ctx.fill();
      if (k === 1) { ctx.lineWidth = Math.max(1.5, size * 0.08); ctx.strokeStyle = '#1b1b2f'; ctx.stroke(); }
    }
    ctx.restore();
  }

  // ---------------------------------------------------------------- image
  R.frame = function (g) {
    const T = g.track, segs = T.segments, SEG = T.SEG;
    depth = R.depth = 1 / Math.tan((BASE_FOV + g.fovBoost) / 2 * Math.PI / 180);
    const sky = skyColors(g.progress);
    R.sky = sky;

    ctx.save();
    if (g.shake > 0) ctx.translate((Math.random() - 0.5) * g.shake * H * 0.03, (Math.random() - 0.5) * g.shake * H * 0.03);

    // sol de secours sous l'horizon
    const baseSeg = segs[Math.min(segs.length - 1, Math.floor(g.position / SEG))];
    ctx.fillStyle = baseSeg.grass;
    ctx.fillRect(-20, horizon - 2, W + 40, H - horizon + 22);
    drawBackground(g, sky);

    const basePercent = (g.position % SEG) / SEG;
    const baseIndex = baseSeg.index;
    let maxy = H;
    let x = 0, dx = -(baseSeg.curve * basePercent);
    const visible = [];
    const last = Math.min(segs.length - 1, baseIndex + DRAW);
    const fogStart = DRAW * 0.5;

    for (let i = baseIndex; i <= last; i++) {
      const seg = segs[i];
      const n = i - baseIndex;
      seg.clip = maxy;
      seg.fog = n > fogStart ? Math.pow((n - fogStart) / (DRAW - fogStart), 1.4) * 0.9 : 0;
      project(seg.p1, g.playerX - x, g.camY, g.position);
      project(seg.p2, g.playerX - x - dx, g.camY, g.position);
      x += dx;
      dx += seg.curve;
      seg.visible = false;
      if (seg.p1.camera.z <= depth || seg.p2.screen.y >= seg.p1.screen.y || seg.p2.screen.y >= maxy) continue;
      seg.visible = true;
      visible.push(seg);
      maxy = seg.p2.screen.y;
    }
    R.visibleTop = maxy;

    // route de l'arrière vers l'avant
    for (let k = visible.length - 1; k >= 0; k--) drawSegment(visible[k], sky);

    // sprites, voitures, jetons — de l'arrière vers l'avant
    const playerSegIndex = Math.floor((g.position + PLAYER_Z) / SEG);
    for (let i = last; i >= baseIndex; i--) {
      const seg = segs[i];
      const n = i - baseIndex;
      if (seg.p1.camera.z > depth) {
        const alpha = n > DRAW * 0.82 ? U.clamp(1 - (n - DRAW * 0.82) / (DRAW * 0.18), 0, 1) : 1;
        const clip = seg.clip;
        // décor
        for (let k = 0; k < seg.sprites.length; k++) {
          const sp = seg.sprites[k];
          const spr = spriteFor(sp);
          const s = seg.p1.screen.scale;
          const sx = seg.p1.screen.x + s * sp.offset * K;
          drawSprite(spr, s, sx, seg.p1.screen.y, clip, alpha, 0);
          if (sp.radar && sp.flashT > 0) {
            ctx.globalAlpha = Math.min(1, sp.flashT * 3);
            const fx = window.Sprites && Sprites.fx && Sprites.fx.flash;
            const r = Math.max(10, spr.worldW * s * K * 1.6);
            if (fx) ctx.drawImage(fx, sx - r / 2, seg.p1.screen.y - r * 1.2, r, r);
            ctx.globalAlpha = 1;
          }
        }
        // jetons
        if (seg.tokens.length) {
          const tok = window.Sprites && Sprites.get('token');
          if (tok && tok.img) {
            for (const t of seg.tokens) {
              if (t.taken) continue;
              const s = seg.p1.screen.scale;
              const sx = seg.p1.screen.x + s * t.x * K;
              drawSprite(tok, s, sx, seg.p1.screen.y, clip, alpha, 260 + Math.sin(g.time * 4 + i) * 90, Math.abs(Math.cos(g.time * 3 + i * 0.3)) * 0.85 + 0.15);
            }
          }
        }
        // voitures
        for (let k = 0; k < seg.cars.length; k++) {
          const car = seg.cars[k];
          const spr = window.Sprites ? Sprites.get(car.sprite) : own.missing;
          const p = (car.z % SEG) / SEG;
          const s = U.lerp(seg.p1.screen.scale, seg.p2.screen.scale, p);
          const cx = U.lerp(seg.p1.screen.x, seg.p2.screen.x, p) + s * car.x * K;
          const cy = U.lerp(seg.p1.screen.y, seg.p2.screen.y, p);
          drawSprite(spr, s, cx, cy + car.bob * s * K, clip, alpha, 0);
          if (car.honkT > 0) {
            const b = window.Sprites && Sprites.fx && Sprites.fx.pouet;
            if (b) {
              const bw = Math.max(30, spr.worldW * s * K * 0.9);
              ctx.globalAlpha = Math.min(1, car.honkT * 2);
              ctx.drawImage(b, cx - bw * 0.1, cy - bw * 1.45, bw, bw * b.height / b.width);
              ctx.globalAlpha = 1;
            }
          }
        }
      }
      if (i === playerSegIndex) drawPlayer(g);
    }
    if (playerSegIndex < baseIndex || playerSegIndex > last) drawPlayer(g);

    ctx.restore();
  };

  window.Render = R;
})();
