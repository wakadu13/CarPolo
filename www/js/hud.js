/* CarPolo — HUD : chrono, permis à points, vitesse, turbo, mini-carte, boutons tactiles, effets écran */
(function () {
  'use strict';

  const HUD = {};
  const OUT = '#1b1b2f';
  const FX = () => (window.Sprites && Sprites.fx) || {};

  function unit(W, H) { return Math.max(0.6, Math.min(H / 360, W / 560)); }
  HUD.unit = unit;

  // ---------------------------------------------------------------- boutons tactiles
  HUD.layout = function (W, H, tilt) {
    const u = unit(W, H);
    const L = {
      u: u,
      left: { cx: 64 * u, cy: H - 64 * u, r: 42 * u },
      right: { cx: 164 * u, cy: H - 64 * u, r: 42 * u },
      brake: { x: W - 186 * u, y: H - 104 * u, w: 66 * u, h: 86 * u },
      gas: { x: W - 108 * u, y: H - 132 * u, w: 84 * u, h: 114 * u },
      boost: { cx: W - 66 * u, cy: H - 186 * u, r: 32 * u },
      horn: { cx: W - 153 * u, cy: H - 160 * u, r: 24 * u },
      pause: { cx: 24 * u, cy: 24 * u, r: 17 * u },
      tilt: tilt
    };
    return L;
  };

  // zones de toucher généreuses -> liste des commandes actives
  HUD.hit = function (L, W, H, x, y) {
    const u = L.u;
    const d2 = (a, b, c) => (x - a) * (x - a) + (y - b) * (y - b) <= c * c;
    if (d2(L.pause.cx, L.pause.cy, L.pause.r * 1.6)) return 'pause';
    if (d2(L.boost.cx, L.boost.cy, L.boost.r * 1.35)) return 'boost';
    if (d2(L.horn.cx, L.horn.cy, L.horn.r * 1.45)) return 'horn';
    if (x >= L.gas.x - 6 * u && y >= L.gas.y - 10 * u) return 'gas';
    if (x >= L.brake.x - 10 * u && x < L.gas.x - 6 * u && y >= L.brake.y - 14 * u) return 'brake';
    if (!L.tilt && x < 250 * u && y > H * 0.36) {
      return x < (L.left.cx + L.right.cx) / 2 ? 'left' : 'right';
    }
    return null;
  };

  function circleBtn(ctx, b, pressed, fill, icon, u) {
    ctx.save();
    ctx.globalAlpha = pressed ? 0.95 : 0.62;
    ctx.beginPath(); ctx.arc(b.cx, b.cy + (pressed ? 2 * u : 0), b.r, 0, Math.PI * 2);
    ctx.fillStyle = 'rgba(0,0,0,0.25)';
    ctx.fill();
    ctx.beginPath(); ctx.arc(b.cx, b.cy - (pressed ? 0 : 3 * u), b.r, 0, Math.PI * 2);
    ctx.fillStyle = pressed ? U.mixHex(fill, '#ffffff', 0.25) : fill;
    ctx.fill();
    ctx.lineWidth = 3.5 * u; ctx.strokeStyle = OUT; ctx.stroke();
    ctx.globalAlpha = pressed ? 1 : 0.85;
    ctx.translate(b.cx, b.cy - (pressed ? 0 : 3 * u));
    icon(ctx, b.r);
    ctx.restore();
  }

  function arrowIcon(dir) {
    return function (ctx, r) {
      ctx.beginPath();
      ctx.moveTo(dir * r * 0.5, 0);
      ctx.lineTo(-dir * r * 0.3, -r * 0.5);
      ctx.lineTo(-dir * r * 0.3, r * 0.5);
      ctx.closePath();
      ctx.fillStyle = '#ffffff'; ctx.fill();
      ctx.lineWidth = r * 0.09; ctx.strokeStyle = OUT; ctx.lineJoin = 'round'; ctx.stroke();
    };
  }
  function boltIcon(ctx, r) {
    ctx.beginPath();
    ctx.moveTo(r * 0.12, -r * 0.62); ctx.lineTo(-r * 0.36, r * 0.08); ctx.lineTo(-r * 0.02, r * 0.08);
    ctx.lineTo(-r * 0.14, r * 0.62); ctx.lineTo(r * 0.38, -r * 0.1); ctx.lineTo(r * 0.04, -r * 0.1);
    ctx.closePath();
    ctx.fillStyle = '#fff36b'; ctx.fill();
    ctx.lineWidth = r * 0.09; ctx.strokeStyle = OUT; ctx.lineJoin = 'round'; ctx.stroke();
  }
  function hornIcon(ctx, r) {
    ctx.lineJoin = 'round';
    ctx.beginPath();
    ctx.moveTo(-r * 0.5, -r * 0.18); ctx.lineTo(-r * 0.15, -r * 0.18); ctx.lineTo(r * 0.38, -r * 0.48);
    ctx.lineTo(r * 0.38, r * 0.48); ctx.lineTo(-r * 0.15, r * 0.18); ctx.lineTo(-r * 0.5, r * 0.18);
    ctx.closePath();
    ctx.fillStyle = '#ffffff'; ctx.fill();
    ctx.lineWidth = r * 0.11; ctx.strokeStyle = OUT; ctx.stroke();
  }
  function pauseIcon(ctx, r) {
    ctx.fillStyle = '#fff';
    ctx.strokeStyle = OUT; ctx.lineWidth = r * 0.1;
    for (const s of [-1, 1]) {
      ctx.beginPath(); ctx.rect(s * r * 0.22 - r * 0.12, -r * 0.42, r * 0.24, r * 0.84); ctx.fill(); ctx.stroke();
    }
  }

  function pedal(ctx, b, pressed, color, label, u) {
    ctx.save();
    ctx.globalAlpha = pressed ? 0.95 : 0.62;
    const off = pressed ? 3 * u : 0;
    U.roundRect(ctx, b.x, b.y + 5 * u, b.w, b.h, 12 * u);
    ctx.fillStyle = 'rgba(0,0,0,0.3)'; ctx.fill();
    U.roundRect(ctx, b.x, b.y + off, b.w, b.h, 12 * u);
    ctx.fillStyle = pressed ? U.mixHex(color, '#ffffff', 0.25) : color; ctx.fill();
    ctx.lineWidth = 3.5 * u; ctx.strokeStyle = OUT; ctx.stroke();
    // stries
    ctx.globalAlpha *= 0.5;
    ctx.strokeStyle = OUT; ctx.lineWidth = 3 * u;
    for (let i = 1; i <= 3; i++) {
      const yy = b.y + off + b.h * (0.18 + i * 0.13);
      ctx.beginPath(); ctx.moveTo(b.x + b.w * 0.22, yy); ctx.lineTo(b.x + b.w * 0.78, yy); ctx.stroke();
    }
    ctx.restore();
    U.text(ctx, label, b.x + b.w / 2, b.y + off + b.h * 0.86, 13 * u, '#ffffff', { alpha: pressed ? 1 : 0.85, shadow: false });
  }

  HUD.drawControls = function (ctx, g, L) {
    const u = L.u, inp = g.input;
    if (!L.tilt) {
      circleBtn(ctx, L.left, inp.left, '#4aa8ff', arrowIcon(-1), u);
      circleBtn(ctx, L.right, inp.right, '#4aa8ff', arrowIcon(1), u);
    }
    pedal(ctx, L.brake, inp.brake, '#ff6b5e', 'FREIN', u);
    pedal(ctx, L.gas, inp.gas, '#3fd38a', 'GAZ', u);
    // turbo avec jauge circulaire
    const ready = g.gauge >= 0.2 || g.boosting;
    circleBtn(ctx, L.boost, inp.boost || g.boosting, ready ? '#ff9f1c' : '#9a8f86', boltIcon, u);
    ctx.save();
    ctx.lineWidth = 5 * u; ctx.lineCap = 'round';
    ctx.strokeStyle = g.boosting ? '#fff36b' : '#ffd23a';
    ctx.beginPath();
    ctx.arc(L.boost.cx, L.boost.cy - 3 * u, L.boost.r + 6 * u, -Math.PI / 2, -Math.PI / 2 + Math.PI * 2 * U.clamp(g.gauge, 0, 1));
    ctx.stroke();
    ctx.restore();
    circleBtn(ctx, L.horn, inp.horn || g.hornT > 0.6, '#b07cff', hornIcon, u);
  };

  HUD.drawPauseBtn = function (ctx, L) {
    circleBtn(ctx, L.pause, false, '#5a6275', pauseIcon, L.u);
  };

  // ---------------------------------------------------------------- mini-carte
  HUD.drawMinimap = function (ctx, T, progress, x, y, size, u, opts) {
    opts = opts || {};
    const box = T.routeBox;
    const bw = box.maxX - box.minX, bh = box.maxY - box.minY;
    const pad = size * 0.12;
    const padR = opts.places ? size * 0.36 : pad, padL = opts.places ? size * 0.2 : pad;
    const sc = Math.min((size - padL - padR) / bw, (size - pad * 2) / bh);
    const ox = x + padL + ((size - padL - padR) - bw * sc) / 2, oy = y + (size - bh * sc) / 2;
    const P = (p) => [ox + (p[0] - box.minX) * sc, oy + (p[1] - box.minY) * sc];

    ctx.save();
    if (!opts.noPanel) {
      U.roundRect(ctx, x, y, size, size, 12 * u);
      ctx.fillStyle = 'rgba(232, 245, 226, 0.88)'; ctx.fill();
      ctx.lineWidth = 3 * u; ctx.strokeStyle = OUT; ctx.stroke();
    }
    ctx.lineJoin = 'round'; ctx.lineCap = 'round';
    const path = () => {
      ctx.beginPath();
      T.route.forEach((p, i) => { const q = P(p); if (i) ctx.lineTo(q[0], q[1]); else ctx.moveTo(q[0], q[1]); });
    };
    path(); ctx.lineWidth = 7 * u; ctx.strokeStyle = '#ffffff'; ctx.stroke();
    path(); ctx.lineWidth = 4 * u; ctx.strokeStyle = '#5b3df5'; ctx.stroke();
    // trajet déjà parcouru
    const cur = T.routePoint(progress);
    ctx.beginPath();
    const N = 60;
    for (let i = 0; i <= N; i++) {
      const q = P(T.routePoint(progress * i / N));
      if (i) ctx.lineTo(q[0], q[1]); else ctx.moveTo(q[0], q[1]);
    }
    ctx.lineWidth = 4 * u; ctx.strokeStyle = '#20c9c0'; ctx.stroke();

    if (opts.places) {
      for (const pl of T.places) {
        const q = P([pl.x, pl.y]);
        ctx.beginPath(); ctx.arc(q[0], q[1], 2.2 * u, 0, Math.PI * 2); ctx.fillStyle = '#3c4043'; ctx.fill();
        U.text(ctx, pl.name, q[0] + pl.dx * 7 * u, q[1] + pl.dy * 7 * u, 8 * u, '#3c4043',
          { stroke: '#ffffff', strokeW: 0.35, shadow: false, align: pl.dx >= 0 ? 'left' : 'right' });
      }
    }
    // départ
    const s0 = P(T.route[0]);
    ctx.beginPath(); ctx.arc(s0[0], s0[1], 4.5 * u, 0, Math.PI * 2);
    ctx.fillStyle = '#fff'; ctx.fill(); ctx.lineWidth = 2.5 * u; ctx.strokeStyle = '#3c4043'; ctx.stroke();
    // arrivée : épingle
    const e = P(T.route[T.route.length - 1]);
    ctx.beginPath();
    ctx.moveTo(e[0], e[1]);
    ctx.bezierCurveTo(e[0] - 4 * u, e[1] - 6 * u, e[0] - 7 * u, e[1] - 9 * u, e[0] - 7 * u, e[1] - 13 * u);
    ctx.arc(e[0], e[1] - 13 * u, 7 * u, Math.PI, 0);
    ctx.bezierCurveTo(e[0] + 7 * u, e[1] - 9 * u, e[0] + 4 * u, e[1] - 6 * u, e[0], e[1]);
    ctx.fillStyle = '#ea4335'; ctx.fill(); ctx.lineWidth = 2 * u; ctx.strokeStyle = OUT; ctx.stroke();
    ctx.beginPath(); ctx.arc(e[0], e[1] - 13 * u, 2.6 * u, 0, Math.PI * 2); ctx.fillStyle = '#7d1d15'; ctx.fill();
    // la C4
    const c = P(cur);
    const pulse = 1 + 0.18 * Math.sin(performance.now() / 160);
    ctx.beginPath(); ctx.arc(c[0], c[1], 6 * u * pulse, 0, Math.PI * 2);
    ctx.fillStyle = '#20c9c0'; ctx.fill(); ctx.lineWidth = 2.5 * u; ctx.strokeStyle = OUT; ctx.stroke();
    ctx.restore();
  };

  // ---------------------------------------------------------------- HUD principal
  HUD.draw = function (ctx, g, W, H) {
    const u = unit(W, H);
    const L = g.layout;
    const T = g.track;

    // effets plein écran : lignes de vitesse en turbo
    if (g.boosting || g.speedPct > 0.97) {
      const n = g.boosting ? 26 : 10;
      ctx.save();
      ctx.strokeStyle = 'rgba(255,255,255,' + (g.boosting ? 0.55 : 0.25) + ')';
      ctx.lineCap = 'round';
      const cx = W / 2, cy = H * 0.45;
      for (let i = 0; i < n; i++) {
        const a = Math.random() * Math.PI * 2;
        const r0 = Math.max(W, H) * (0.42 + Math.random() * 0.2);
        const r1 = r0 + Math.max(W, H) * (0.1 + Math.random() * 0.2);
        ctx.lineWidth = (1 + Math.random() * 3) * u;
        ctx.beginPath();
        ctx.moveTo(cx + Math.cos(a) * r0, cy + Math.sin(a) * r0 * 0.7);
        ctx.lineTo(cx + Math.cos(a) * r1, cy + Math.sin(a) * r1 * 0.7);
        ctx.stroke();
      }
      ctx.restore();
    }

    // particules (fumée, poussière, confettis, étoiles)
    for (const p of g.particles) {
      ctx.save();
      ctx.globalAlpha = U.clamp(p.life / p.max, 0, 1) * (p.alpha || 1);
      if (p.kind === 'smoke' && FX().smoke) {
        const s = p.size * u;
        ctx.drawImage(FX().smoke, p.x - s / 2, p.y - s / 2, s, s);
      } else if (p.kind === 'confetti') {
        ctx.translate(p.x, p.y); ctx.rotate(p.rot);
        ctx.fillStyle = p.color;
        ctx.fillRect(-p.size * u / 2, -p.size * u / 4, p.size * u, p.size * u / 2);
      } else if (p.kind === 'star' && FX().star) {
        const s = p.size * u;
        ctx.translate(p.x, p.y); ctx.rotate(p.rot);
        ctx.drawImage(FX().star, -s / 2, -s / 2, s, s);
      }
      ctx.restore();
    }

    // étoiles qui tournent autour de la voiture après un accident + BOUM
    if (g.crash && Render.playerScreen) {
      const ps = Render.playerScreen;
      const fx = FX();
      if (fx.boom && g.crash.t < 0.9) {
        const k = U.easeOutBack(Math.min(1, g.crash.t / 0.25));
        const bw = ps.w * 1.25 * k, bh = bw * fx.boom.height / fx.boom.width;
        ctx.save();
        ctx.globalAlpha = g.crash.t > 0.7 ? (0.9 - g.crash.t) / 0.2 : 1;
        ctx.drawImage(fx.boom, ps.x - bw / 2, ps.y - ps.h * 1.25 - bh / 2, bw, bh);
        ctx.restore();
      }
      if (fx.star) {
        for (let i = 0; i < 5; i++) {
          const a = g.time * 5 + i * Math.PI * 2 / 5;
          const s = 18 * u;
          ctx.drawImage(fx.star, ps.x + Math.cos(a) * ps.w * 0.45 - s / 2, ps.y - ps.h * 1.0 + Math.sin(a) * ps.h * 0.15 - s / 2, s, s);
        }
      }
    }
    // POUET au-dessus de la C4
    if (g.hornT > 0 && Render.playerScreen && FX().pouet) {
      const ps = Render.playerScreen, b = FX().pouet;
      const k = U.easeOutBack(Math.min(1, (0.9 - g.hornT) / 0.15 + 0.01));
      const bw = ps.w * 0.6 * k;
      ctx.save(); ctx.globalAlpha = Math.min(1, g.hornT * 3);
      ctx.drawImage(b, ps.x + ps.w * 0.12, ps.y - ps.h * 1.55, bw, bw * b.height / b.width);
      ctx.restore();
    }

    // flash radar / accident
    if (g.flash > 0) {
      ctx.save();
      ctx.globalAlpha = Math.min(1, g.flash);
      ctx.fillStyle = g.flashColor || '#ffffff';
      ctx.fillRect(0, 0, W, H);
      ctx.restore();
    }
    // vignette turbo
    if (g.boosting) {
      ctx.save();
      const gr = ctx.createRadialGradient(W / 2, H / 2, H * 0.35, W / 2, H / 2, Math.max(W, H) * 0.75);
      gr.addColorStop(0, 'rgba(255,170,40,0)');
      gr.addColorStop(1, 'rgba(255,120,20,0.35)');
      ctx.fillStyle = gr; ctx.fillRect(0, 0, W, H);
      ctx.restore();
    }

    if (g.state === 'title') return;

    // ---- en haut à gauche : pause + chrono + permis
    HUD.drawPauseBtn(ctx, L);
    const tx = 48 * u;
    const shown = g.state === 'finished' || g.state === 'gameover' ? g.finalTime : g.raceTime + g.penalty;
    U.text(ctx, U.fmtTime(shown), tx, 22 * u, 22 * u, '#ffffff', { align: 'left' });
    if (g.best) U.text(ctx, 'RECORD ' + U.fmtTime(g.best), tx, 42 * u, 10 * u, '#fff36b', { align: 'left', strokeW: 0.3 });
    if (g.penalty > 0) U.text(ctx, '+' + Math.round(g.penalty / 1000) + ' s pénalité', tx, (g.best ? 56 : 42) * u, 9 * u, '#ff8a80', { align: 'left', strokeW: 0.3 });

    // permis à points (carte rose)
    const px = 10 * u, py = (g.best || g.penalty ? 66 : 50) * u + (g.best && g.penalty ? 10 * u : 0);
    const shake = g.pointsFlash > 0 ? Math.sin(g.time * 60) * 3 * u * g.pointsFlash : 0;
    ctx.save();
    ctx.translate(px + shake, py);
    U.roundRect(ctx, 0, 0, 92 * u, 34 * u, 6 * u);
    ctx.fillStyle = g.pointsFlash > 0 ? '#ff8a80' : '#ffc4d6'; ctx.fill();
    ctx.lineWidth = 2.5 * u; ctx.strokeStyle = OUT; ctx.stroke();
    ctx.fillStyle = '#2b5fd9'; ctx.fillRect(4 * u, 4 * u, 8 * u, 26 * u);
    ctx.fillStyle = '#ffd23a';
    ctx.beginPath(); ctx.arc(8 * u, 11 * u, 2 * u, 0, Math.PI * 2); ctx.fill();
    ctx.restore();
    U.text(ctx, 'PERMIS', px + shake + 52 * u, py + 10 * u, 8 * u, '#b0306a', { stroke: false, shadow: false });
    U.text(ctx, g.points + ' / 12 pts', px + shake + 52 * u, py + 23 * u, 12 * u, g.points <= 4 ? '#ff3b30' : '#ffffff', { strokeW: 0.28 });

    // ---- en haut au centre : vitesse + turbo
    const kmh = Math.round(g.kmh);
    U.text(ctx, String(kmh), W / 2 - 6 * u, 24 * u, 30 * u, g.boosting ? '#fff36b' : '#ffffff', { align: 'right' });
    U.text(ctx, 'km/h', W / 2, 29 * u, 11 * u, '#ffffff', { align: 'left', strokeW: 0.3 });
    // jauge turbo
    const gw = 120 * u, gh = 10 * u, gx = W / 2 - gw / 2, gy = 45 * u;
    U.roundRect(ctx, gx, gy, gw, gh, gh / 2);
    ctx.fillStyle = 'rgba(0,0,0,0.35)'; ctx.fill();
    if (g.gauge > 0.005) {
      U.roundRect(ctx, gx, gy, Math.max(gh, gw * U.clamp(g.gauge, 0, 1)), gh, gh / 2);
      ctx.fillStyle = g.boosting ? '#fff36b' : (g.gauge >= 0.2 ? '#ff9f1c' : '#c9a27a'); ctx.fill();
    }
    U.roundRect(ctx, gx, gy, gw, gh, gh / 2);
    ctx.lineWidth = 2.5 * u; ctx.strokeStyle = OUT; ctx.stroke();
    U.text(ctx, 'TURBO', W / 2, gy + gh / 2 + 0.5 * u, 7.5 * u, '#ffffff', { strokeW: 0.35, shadow: false });

    // ---- en haut à droite : mini-carte
    const ms = 112 * u;
    HUD.drawMinimap(ctx, T, g.progress, W - ms - 8 * u, 8 * u, ms, u);
    const km = Math.max(0, (1 - g.progress) * T.realKm);
    U.text(ctx, km.toFixed(1).replace('.', ',') + ' km', W - ms / 2 - 8 * u, ms + 18 * u, 11 * u, '#ffffff', { strokeW: 0.3 });

    // ---- bannière de zone
    if (g.banner && g.banner.t > 0) {
      const b = g.banner, life = b.max - b.t;
      const k = life < 0.35 ? U.easeOutBack(life / 0.35) : (b.t < 0.4 ? b.t / 0.4 : 1);
      const by = Math.max(H * 0.215, 78 * u);
      ctx.save();
      ctx.globalAlpha = U.clamp(k, 0, 1);
      ctx.translate(W / 2, by);
      ctx.scale(0.6 + 0.4 * U.clamp(k, 0, 1.2), 0.6 + 0.4 * U.clamp(k, 0, 1.2));
      ctx.font = '900 ' + Math.round(19 * u) + 'px ' + U.FONT;
      const tw = Math.max(ctx.measureText(b.title).width + 34 * u, 170 * u);
      U.roundRect(ctx, -tw / 2, -18 * u, tw, 48 * u, 11 * u);
      ctx.fillStyle = b.town ? '#ffffff' : '#2b5fd9'; ctx.fill();
      ctx.lineWidth = 4 * u; ctx.strokeStyle = b.town ? '#e8443a' : '#ffffff'; ctx.stroke();
      ctx.lineWidth = 2.5 * u; ctx.strokeStyle = OUT;
      U.roundRect(ctx, -tw / 2 - 2.5 * u, -20.5 * u, tw + 5 * u, 53 * u, 13 * u); ctx.stroke();
      ctx.restore();
      ctx.save(); ctx.globalAlpha = U.clamp(k, 0, 1);
      U.text(ctx, b.title, W / 2, by + 1 * u, 18 * u * (0.6 + 0.4 * U.clamp(k, 0, 1)), b.town ? '#1b1b2f' : '#ffffff', { stroke: b.town ? false : OUT, shadow: false });
      if (b.sub) U.text(ctx, b.sub, W / 2, by + 19 * u, 8.5 * u, b.town ? '#e8443a' : '#fff36b', { stroke: false, shadow: false });
      ctx.restore();
      if (b.split) U.text(ctx, b.split.text, W / 2, by + 44 * u, 13 * u, b.split.color, { alpha: U.clamp(k, 0, 1) });
    }

    // ---- textes flottants (Frôlé !, Flashé !, ...)
    for (const p of g.popups) {
      const life = p.max - p.t;
      const k = life < 0.2 ? U.easeOutBack(life / 0.2) : 1;
      const a = p.t < 0.3 ? p.t / 0.3 : 1;
      U.text(ctx, p.text, p.x * W, p.y * H - life * 22 * u, p.size * u * k, p.color, { alpha: a });
    }

    // ---- compte à rebours (feu tricolore)
    if (g.state === 'countdown') {
      const n = Math.ceil(g.countdown);
      const fw = 64 * u, fh = 150 * u, fx = W / 2 - fw / 2, fy = H * 0.12;
      ctx.save();
      U.roundRect(ctx, fx, fy, fw, fh, 14 * u);
      ctx.fillStyle = '#2d3142'; ctx.fill();
      ctx.lineWidth = 4 * u; ctx.strokeStyle = OUT; ctx.stroke();
      const cols = [['#ff3b30', n >= 3], ['#ff9f1c', n === 2], ['#ffd23a', n === 1]];
      cols.forEach(([c, on], i) => {
        ctx.beginPath(); ctx.arc(W / 2, fy + 28 * u + i * 47 * u, 18 * u, 0, Math.PI * 2);
        ctx.fillStyle = on ? c : '#4a4e5e'; ctx.fill();
        ctx.lineWidth = 3 * u; ctx.stroke();
        if (on) { ctx.beginPath(); ctx.arc(W / 2 - 6 * u, fy + 22 * u + i * 47 * u, 5 * u, 0, Math.PI * 2); ctx.fillStyle = 'rgba(255,255,255,0.6)'; ctx.fill(); }
      });
      ctx.restore();
      const frac = g.countdown - Math.floor(g.countdown);
      U.text(ctx, String(n), W / 2 + 90 * u, H * 0.3, (48 + frac * 30) * u, '#ffffff');
      U.text(ctx, 'Garde le doigt sur GAZ au dernier moment pour un départ canon !', W / 2, H * 0.62, 11 * u, '#ffffff', { strokeW: 0.3 });
    }
    if (g.goT > 0) {
      const k = U.easeOutBack(Math.min(1, (1.2 - g.goT) / 0.25));
      U.text(ctx, 'PARTEZ !', W / 2, H * 0.32, 56 * u * k, '#3fd38a', { alpha: Math.min(1, g.goT * 2) });
    }

    // ---- commandes tactiles
    if (g.showTouch && (g.state === 'race' || g.state === 'countdown')) HUD.drawControls(ctx, g, L);
    if (!g.showTouch && g.state === 'race' && g.raceTime < 9000) {
      U.text(ctx, '← → diriger   ↑ gaz   ↓ frein   ESPACE turbo   K klaxon   P pause', W / 2, H - 16 * u, 10 * u, '#ffffff', { strokeW: 0.3, alpha: 0.9 });
    }
  };

  window.HUD = HUD;
})();
