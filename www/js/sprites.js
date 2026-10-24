/* CarPolo — procedural cartoon sprites (Canvas 2D, no external images).
 * Every world sprite is anchored bottom-center: the bottom row of the canvas is the ground.
 * worldW = width of the sprite in world units (road half-width = 2000 on D-roads, 3000 on the A7).
 */
window.Sprites = (function () {
  'use strict';

  var OL = '#1b1b2f';
  var FONT_FAMILY = '"Arial Black", "Roboto", system-ui, sans-serif';
  var K_LINE = 22; // outline thickness in world units → same on-screen thickness for every sprite

  var store = {};
  var names = [];
  var townCache = {};
  var placeholder = null;
  var ready = false;
  var bg = { far: null, mid: null, near: null, sun: null, clouds: [] };
  var fx = { boom: null, star: null, smoke: null, pouet: null, flash: null };
  var TAU = Math.PI * 2;

  /* ------------------------------------------------------------------ helpers */

  function font(px) { return '900 ' + Math.max(1, Math.round(px)) + 'px ' + FONT_FAMILY; }

  function mk(w, h) {
    var c = document.createElement('canvas');
    c.width = Math.max(1, Math.round(w));
    c.height = Math.max(1, Math.round(h));
    return c;
  }

  function lwOf(cw, worldW) { return Math.max(3, Math.min(cw * 0.04, cw / worldW * K_LINE)); }

  function hexRgb(h) {
    h = h.replace('#', '');
    if (h.length === 3) h = h[0] + h[0] + h[1] + h[1] + h[2] + h[2];
    var n = parseInt(h, 16);
    return [(n >> 16) & 255, (n >> 8) & 255, n & 255];
  }
  function mix(a, b, t) {
    var A = hexRgb(a), B = hexRgb(b);
    var r = [0, 1, 2].map(function (i) { return Math.round(A[i] + (B[i] - A[i]) * t); });
    return '#' + r.map(function (v) { return (v < 16 ? '0' : '') + v.toString(16); }).join('');
  }
  function light(c, t) { return mix(c, '#ffffff', t); }
  function dark(c, t) { return mix(c, '#000000', t); }

  function rng(seed) { // mulberry32
    var s = seed >>> 0;
    return function () {
      s = (s + 0x6D2B79F5) >>> 0;
      var t = s;
      t = Math.imul(t ^ (t >>> 15), t | 1);
      t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
      return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
  }

  function rr(c, x, y, w, h, r) {
    r = Math.max(0, Math.min(r, w / 2, h / 2));
    c.beginPath();
    c.moveTo(x + r, y);
    c.arcTo(x + w, y, x + w, y + h, r);
    c.arcTo(x + w, y + h, x, y + h, r);
    c.arcTo(x, y + h, x, y, r);
    c.arcTo(x, y, x + w, y, r);
    c.closePath();
  }
  function poly(c, pts) {
    c.beginPath();
    c.moveTo(pts[0][0], pts[0][1]);
    for (var i = 1; i < pts.length; i++) c.lineTo(pts[i][0], pts[i][1]);
    c.closePath();
  }
  function circle(c, x, y, r) { c.beginPath(); c.arc(x, y, Math.max(0.1, r), 0, TAU); }
  function ellipse(c, x, y, rx, ry, rot) {
    c.beginPath();
    c.ellipse(x, y, Math.max(0.1, Math.abs(rx)), Math.max(0.1, Math.abs(ry)), rot || 0, 0, TAU);
  }
  // trapezoid with rounded top corners
  function trap(c, bl, br, by, tl, tr, ty, r) {
    c.beginPath();
    c.moveTo(bl, by);
    c.arcTo(tl, ty, tr, ty, r);
    c.arcTo(tr, ty, br, by, r);
    c.lineTo(br, by);
    c.closePath();
  }
  function paint(c, fill, lw, stroke) {
    if (fill) { c.fillStyle = fill; c.fill(); }
    if (lw) {
      c.lineJoin = 'round'; c.lineCap = 'round';
      c.lineWidth = lw; c.strokeStyle = stroke || OL; c.stroke();
    }
  }
  function line(c, pts, lw, color, cap) {
    c.beginPath();
    c.moveTo(pts[0][0], pts[0][1]);
    for (var i = 1; i < pts.length; i++) c.lineTo(pts[i][0], pts[i][1]);
    c.lineWidth = lw; c.strokeStyle = color || OL; c.lineCap = cap || 'round'; c.lineJoin = 'round';
    c.stroke();
  }
  function shadowEll(c, x, y, rx, ry, a) {
    c.save(); c.globalAlpha = a == null ? 0.25 : a; c.fillStyle = '#000';
    ellipse(c, x, y, rx, ry); c.fill(); c.restore();
  }
  // run draw() clipped to the path built by pathFn()
  function clipped(c, pathFn, draw) { c.save(); pathFn(); c.clip(); draw(); c.restore(); }
  // flat cartoon shading: base fill, highlight band and shadow band clipped to the shape, then outline
  function shaded(c, pathFn, base, hi, sh, hiY0, hiY1, shY0, shY1, lw) {
    pathFn(); c.fillStyle = base; c.fill();
    clipped(c, pathFn, function () {
      if (hi) { c.fillStyle = hi; c.fillRect(-10, hiY0, c.canvas.width + 20, hiY1 - hiY0); }
      if (sh) { c.fillStyle = sh; c.fillRect(-10, shY0, c.canvas.width + 20, shY1 - shY0); }
    });
    if (lw) { pathFn(); paint(c, null, lw); }
  }
  function label(c, s, x, y, px, fill, lw, stroke, maxW, align) {
    c.save();
    c.font = font(px);
    if (maxW) {
      var m = c.measureText(s).width;
      if (m > maxW) { px = px * maxW / m; c.font = font(px); }
    }
    c.textAlign = align || 'center';
    c.textBaseline = 'middle';
    if (lw) {
      c.lineJoin = 'round'; c.miterLimit = 2;
      c.lineWidth = lw; c.strokeStyle = stroke || OL; c.strokeText(s, x, y);
    }
    c.fillStyle = fill; c.fillText(s, x, y);
    c.restore();
    return px;
  }
  // union of circles with a single outline + cartoon volume
  function blob(c, circles, base, hi, sh, lw, outline) {
    var i, k;
    c.save();
    if (lw) {
      c.strokeStyle = outline || OL; c.lineWidth = lw * 2;
      for (i = 0; i < circles.length; i++) { k = circles[i]; circle(c, k[0], k[1], k[2]); c.stroke(); }
    }
    c.fillStyle = sh || base;
    for (i = 0; i < circles.length; i++) { k = circles[i]; circle(c, k[0], k[1], k[2]); c.fill(); }
    if (sh) {
      c.fillStyle = base;
      for (i = 0; i < circles.length; i++) { k = circles[i]; circle(c, k[0] - k[2] * 0.1, k[1] - k[2] * 0.14, k[2] * 0.8); c.fill(); }
    }
    if (hi) {
      c.fillStyle = hi;
      for (i = 0; i < circles.length; i++) {
        k = circles[i];
        if (k[3] === 0) continue;
        circle(c, k[0] - k[2] * 0.3, k[1] - k[2] * 0.36, k[2] * 0.36); c.fill();
      }
    }
    c.restore();
  }
  function star(c, cx, cy, n, rOut, rIn, rot) {
    c.beginPath();
    for (var i = 0; i < n * 2; i++) {
      var r = i % 2 ? rIn : rOut, a = (rot || -Math.PI / 2) + i * Math.PI / n;
      if (i === 0) c.moveTo(cx + Math.cos(a) * r, cy + Math.sin(a) * r);
      else c.lineTo(cx + Math.cos(a) * r, cy + Math.sin(a) * r);
    }
    c.closePath();
  }

  // crop fully transparent rows at the top (keeps bottom-center anchoring and width)
  function trimTop(c) {
    var x = c.getContext('2d'), d = x.getImageData(0, 0, c.width, c.height).data, top = 0;
    outer: for (var y = 0; y < c.height; y++) {
      for (var i = y * c.width * 4 + 3, e = i + c.width * 4; i < e; i += 4) if (d[i] > 8) break outer;
      top = y + 1;
    }
    top = Math.max(0, top - 2);
    if (top < 4) return c;
    var n = mk(c.width, c.height - top);
    n.getContext('2d').drawImage(c, 0, -top);
    return n;
  }

  function add(name, canvas, worldW, meta) {
    var rec = { img: canvas, worldW: worldW, name: name };
    if (meta) for (var k in meta) if (Object.prototype.hasOwnProperty.call(meta, k)) rec[k] = meta[k];
    if (!store[name]) names.push(name);
    store[name] = rec;
    return rec;
  }

  /* ------------------------------------------------------------ car parts */

  function tyre(c, x, y, w, h, lw) {
    var r = Math.min(w, h) * 0.3;
    rr(c, x, y, w, h, r); paint(c, '#2b2b38', lw);
    clipped(c, function () { rr(c, x, y, w, h, r); }, function () {
      c.strokeStyle = '#4b4b60'; c.lineWidth = Math.max(1.5, lw * 0.45); c.lineCap = 'round';
      for (var i = 1; i < 5; i++) {
        var yy = y + h * i / 5;
        c.beginPath(); c.moveTo(x + w * 0.2, yy); c.lineTo(x + w * 0.8, yy); c.stroke();
      }
    });
  }

  function plate(c, x, y, w, h, txt, lw, dept) {
    var r = h * 0.18;
    rr(c, x, y, w, h, r); paint(c, '#ffffff');
    clipped(c, function () { rr(c, x, y, w, h, r); }, function () {
      c.fillStyle = '#1f4fc4';
      c.fillRect(x, y, w * 0.13, h);
      if (dept) c.fillRect(x + w * 0.87, y, w * 0.13, h);
    });
    // EU stars (tiny ring)
    c.fillStyle = '#ffd400';
    for (var i = 0; i < 8; i++) {
      var a = i / 8 * TAU;
      c.fillRect(x + w * 0.065 + Math.cos(a) * h * 0.14 - 0.6, y + h * 0.3 + Math.sin(a) * h * 0.14 - 0.6, 1.2, 1.2);
    }
    label(c, 'F', x + w * 0.065, y + h * 0.7, h * 0.42, '#fff');
    if (dept) label(c, dept, x + w * 0.935, y + h * 0.7, h * 0.36, '#fff', 0, null, w * 0.11);
    label(c, txt, x + w * 0.5, y + h * 0.54, h * 0.62, '#15151f', 0, null, w * 0.7);
    rr(c, x, y, w, h, r); paint(c, null, lw);
  }

  // Citroën double chevron (two stacked "^")
  function chevrons(c, cx, cy, w, h, color, lw) {
    var t = h * 0.26, ys = [cy - h * 0.2, cy + h * 0.2];
    c.save();
    c.lineJoin = 'miter'; c.miterLimit = 10; c.lineCap = 'butt';
    function shape(yy) {
      c.beginPath();
      c.moveTo(cx - w / 2, yy + h * 0.17);
      c.lineTo(cx, yy - h * 0.17);
      c.lineTo(cx + w / 2, yy + h * 0.17);
    }
    if (lw) { c.strokeStyle = OL; c.lineWidth = t + lw * 2; ys.forEach(function (yy) { shape(yy); c.stroke(); }); }
    c.strokeStyle = color; c.lineWidth = t; ys.forEach(function (yy) { shape(yy); c.stroke(); });
    c.restore();
  }

  function glass(c, pathFn, x0, y0, x1, y1, lw, inside) {
    pathFn();
    var g = c.createLinearGradient(0, y0, 0, y1);
    g.addColorStop(0, '#26395a'); g.addColorStop(1, '#4f7099');
    c.fillStyle = g; c.fill();
    clipped(c, pathFn, function () {
      if (inside) inside();
      var w = x1 - x0;
      c.globalAlpha = 0.32; c.fillStyle = '#ffffff';
      poly(c, [[x0 + w * 0.12, y1], [x0 + w * 0.42, y0], [x0 + w * 0.56, y0], [x0 + w * 0.26, y1]]); c.fill();
      poly(c, [[x0 + w * 0.34, y1], [x0 + w * 0.64, y0], [x0 + w * 0.69, y0], [x0 + w * 0.39, y1]]); c.fill();
      c.globalAlpha = 1;
    });
    if (lw) { pathFn(); paint(c, null, lw); }
  }

  var SKINS = ['#f2c29b', '#d9a066', '#8d5a3b', '#f7d3b5', '#b87a4b'];
  var HAIRS = ['#3b2a20', '#e2b33c', '#1d1d1d', '#8a3b1d', '#9a9a9a'];

  function driverFront(c, cx, cy, s, idx, hat) {
    var skin = SKINS[idx % SKINS.length], hair = HAIRS[(idx * 3 + 1) % HAIRS.length];
    c.fillStyle = idx % 2 ? '#c7413a' : '#2f3d6e';
    ellipse(c, cx, cy + s * 2.15, s * 1.9, s * 1.25); c.fill();
    circle(c, cx, cy, s); paint(c, skin, Math.max(1.5, s * 0.16));
    if (hat) { // flat cap
      c.beginPath(); c.arc(cx, cy - s * 0.15, s * 1.02, Math.PI, TAU); c.closePath(); paint(c, '#6b5a3e', Math.max(1.5, s * 0.16));
      ellipse(c, cx, cy - s * 0.15, s * 1.25, s * 0.22); paint(c, '#5a4a32', Math.max(1.2, s * 0.12));
    } else {
      c.beginPath(); c.arc(cx, cy, s, Math.PI * 1.08, Math.PI * 1.92); c.closePath(); c.fillStyle = hair; c.fill();
    }
    // big cartoon eyes
    var ex = s * 0.36, ey = cy + s * 0.05;
    [-1, 1].forEach(function (k) {
      ellipse(c, cx + k * ex, ey, s * 0.26, s * 0.32); paint(c, '#ffffff', Math.max(1, s * 0.09));
      circle(c, cx + k * ex + s * 0.05, ey + s * 0.06, s * 0.13); c.fillStyle = '#111'; c.fill();
    });
    c.beginPath(); c.arc(cx, cy + s * 0.42, s * 0.3, 0.15 * Math.PI, 0.85 * Math.PI);
    c.lineWidth = Math.max(1, s * 0.12); c.strokeStyle = '#7a2a1a'; c.lineCap = 'round'; c.stroke();
  }

  function headBack(c, cx, cy, s, idx) {
    var hair = HAIRS[(idx * 3 + 1) % HAIRS.length];
    circle(c, cx, cy, s); c.fillStyle = hair; c.fill();
    rr(c, cx - s * 0.9, cy + s * 0.25, s * 1.8, s * 1.2, s * 0.4); c.fillStyle = '#3a3a4a'; c.fill();
  }

  function mirror(c, x0, y0, w, h, color, lw) { rr(c, x0, y0, w, h, h * 0.45); paint(c, color, lw); }

  function bodyPath(c, X, Y, top, bot, l, r, rad) {
    rad = rad || 0.1;
    c.beginPath();
    c.moveTo(X(l), Y(bot - 0.03));
    c.lineTo(X(l), Y(top + rad));
    c.quadraticCurveTo(X(l), Y(top), X(l + rad), Y(top));
    c.lineTo(X(r - rad), Y(top));
    c.quadraticCurveTo(X(r), Y(top), X(r), Y(top + rad));
    c.lineTo(X(r), Y(bot - 0.03));
    c.quadraticCurveTo(X(r), Y(bot), X(r - 0.05), Y(bot));
    c.lineTo(X(l + 0.05), Y(bot));
    c.quadraticCurveTo(X(l), Y(bot), X(l), Y(bot - 0.03));
    c.closePath();
  }

  /* ------------------------------------------------------- player: Citroën C4 */

  function drawC4(steer, brake) {
    var W = 360, H = 264, c = mk(W, H), x = c.getContext('2d');
    var L = lwOf(W, 1100);
    var X = function (f) { return f * W; }, Y = function (f) { return f * H; };
    var base = '#20c9c0', hi = '#8af3ea', sh = '#11928b';
    var d = steer * 0.045 * W;

    shadowEll(x, W / 2, H - 10, W * 0.48, 12, 0.3);

    // flank seen when the car yaws
    if (steer !== 0) {
      var right = steer < 0;
      var ex = right ? 0.95 : 0.05, ox = right ? 0.995 : 0.005;
      poly(x, [[X(ex), Y(0.46)], [X(ox), Y(0.53)], [X(ox), Y(0.83)], [X(ex), Y(0.87)]]);
      paint(x, sh, L);
    }

    tyre(x, X(0.085), Y(0.75), X(0.165), Y(0.235), L);
    tyre(x, X(0.75), Y(0.75), X(0.165), Y(0.235), L);
    if (steer !== 0) { // a bit of rim on the flank side
      var rx = steer < 0 ? X(0.915) : X(0.085);
      ellipse(x, rx, Y(0.865), X(0.022), Y(0.085)); paint(x, '#aeb7c2', L * 0.6);
    }

    // mirrors
    mirror(x, Math.max(L / 2, X(0.0) + d * 0.5), Y(0.375), X(0.11), Y(0.07), base, L);
    mirror(x, Math.min(W - L / 2 - X(0.11), X(0.89) + d * 0.5), Y(0.375), X(0.11), Y(0.07), base, L);

    // roof surface (seen from slightly above)
    poly(x, [[X(0.27) + d, Y(0.09)], [X(0.315) + d * 1.25, Y(0.03)], [X(0.685) + d * 1.25, Y(0.03)], [X(0.73) + d, Y(0.09)]]);
    paint(x, hi, L);
    // shark fin
    x.beginPath();
    x.moveTo(X(0.46) + d * 1.2, Y(0.06));
    x.quadraticCurveTo(X(0.5) + d * 1.2, Y(0.0) + 3, X(0.54) + d * 1.2, Y(0.012));
    x.lineTo(X(0.55) + d * 1.2, Y(0.06)); x.closePath();
    paint(x, '#1d1d29', L * 0.7);

    // greenhouse
    var gh = function () { trap(x, X(0.12) + d * 0.35, X(0.88) + d * 0.35, Y(0.48), X(0.24) + d, X(0.76) + d, Y(0.085), Y(0.06)); };
    gh(); paint(x, base, L);
    clipped(x, gh, function () { x.fillStyle = hi; x.fillRect(0, Y(0.085), W, Y(0.03)); });
    gh(); paint(x, null, L);

    var win = function () { trap(x, X(0.185) + d * 0.45, X(0.815) + d * 0.45, Y(0.455), X(0.29) + d * 0.9, X(0.71) + d * 0.9, Y(0.14), Y(0.04)); };
    glass(x, win, X(0.185), Y(0.14), X(0.815), Y(0.455), L * 0.7, function () {
      // driver's head behind the headrest (driver sits on the left) + passenger headrest
      circle(x, X(0.35) + d * 0.6, Y(0.33), X(0.058)); x.fillStyle = '#4a3020'; x.fill();
      rr(x, X(0.29) + d * 0.6, Y(0.375), X(0.12), Y(0.09), X(0.03)); x.fillStyle = '#34344a'; x.fill();
      rr(x, X(0.59) + d * 0.6, Y(0.375), X(0.12), Y(0.09), X(0.03)); x.fill();
    });
    // rear wiper parked on the right
    line(x, [[X(0.52) + d * 0.5, Y(0.445)], [X(0.7) + d * 0.6, Y(0.405)]], L * 0.55, '#15151f');

    // roof spoiler
    rr(x, X(0.225) + d, Y(0.065), X(0.55), Y(0.07), Y(0.035)); paint(x, base, L * 0.9);
    rr(x, X(0.245) + d, Y(0.072), X(0.51), Y(0.02), Y(0.01)); x.fillStyle = hi; x.fill();
    // third brake light
    rr(x, X(0.42) + d, Y(0.11), X(0.16), Y(0.02), 2);
    if (brake) { x.save(); x.shadowColor = '#ff2a2a'; x.shadowBlur = 12; x.fillStyle = '#ff4a4a'; x.fill(); x.restore(); }
    else { x.fillStyle = '#7a1020'; x.fill(); }

    // body (hatch + wide rear hips)
    var body = function () {
      x.beginPath();
      x.moveTo(X(0.035), Y(0.86)); x.lineTo(X(0.035), Y(0.58));
      x.quadraticCurveTo(X(0.04), Y(0.445), X(0.16), Y(0.43));
      x.lineTo(X(0.84), Y(0.43));
      x.quadraticCurveTo(X(0.96), Y(0.445), X(0.965), Y(0.58));
      x.lineTo(X(0.965), Y(0.86)); x.closePath();
    };
    shaded(x, body, base, hi, sh, Y(0.43), Y(0.465), Y(0.7), Y(0.9), L);
    // hatch shut lines
    line(x, [[X(0.2), Y(0.6)], [X(0.215), Y(0.72)]], L * 0.35, sh);
    line(x, [[X(0.8), Y(0.6)], [X(0.785), Y(0.72)]], L * 0.35, sh);

    // glossy black light band across the hatch
    rr(x, X(0.07), Y(0.47), X(0.86), Y(0.115), Y(0.05)); paint(x, '#16161f', L * 0.8);
    rr(x, X(0.12), Y(0.478), X(0.76), Y(0.012), 2); x.fillStyle = 'rgba(255,255,255,0.22)'; x.fill();

    // V-shaped Citroën tail lights
    var lampCol = brake ? '#ff3030' : '#e0283a';
    [-1, 1].forEach(function (side) {
      var o = side < 0 ? 0.06 : 0.94, tip = side < 0 ? 0.34 : 0.66, k = side < 0 ? 1 : -1;
      var shape = function () {
        poly(x, [[X(o), Y(0.468)], [X(tip), Y(0.528)], [X(o), Y(0.588)], [X(o), Y(0.548)],
          [X(tip - k * 0.1), Y(0.528)], [X(o), Y(0.508)]]);
      };
      if (brake) {
        x.save();
        var gx = X((o + tip) / 2), g = x.createRadialGradient(gx, Y(0.528), 2, gx, Y(0.528), X(0.2));
        g.addColorStop(0, 'rgba(255,40,30,0.55)'); g.addColorStop(1, 'rgba(255,40,30,0)');
        x.fillStyle = g; ellipse(x, gx, Y(0.528), X(0.2), Y(0.13)); x.fill();
        x.restore();
      }
      x.save();
      if (brake) { x.shadowColor = '#ff2020'; x.shadowBlur = 14; }
      shape(); x.fillStyle = lampCol; x.fill();
      x.restore();
      shape(); paint(x, null, L * 0.55);
      var core = brake ? '#fff4d6' : '#ff8a94';
      line(x, [[X(o + k * 0.025), Y(0.488)], [X(tip - k * 0.05), Y(0.524)]], L * (brake ? 0.55 : 0.35), core);
      line(x, [[X(o + k * 0.025), Y(0.568)], [X(tip - k * 0.05), Y(0.532)]], L * (brake ? 0.55 : 0.35), core);
    });
    chevrons(x, X(0.5), Y(0.528), X(0.085), Y(0.075), '#e3e8ee', L * 0.35);

    // badge + plate
    label(x, 'C4', X(0.8), Y(0.645), Y(0.05), '#e3e8ee', L * 0.4);
    plate(x, X(0.365), Y(0.615), X(0.27), Y(0.09), 'C4-013-PR', L * 0.6, '13');

    // lower bumper (black crossover cladding)
    rr(x, X(0.04), Y(0.745), X(0.92), Y(0.14), Y(0.05)); paint(x, '#22222c', L);
    rr(x, X(0.08), Y(0.757), X(0.84), Y(0.01), 2); x.fillStyle = '#4a4a5c'; x.fill();
    rr(x, X(0.065), Y(0.775), X(0.08), Y(0.022), 3); paint(x, '#d0202a', L * 0.4);
    rr(x, X(0.855), Y(0.775), X(0.08), Y(0.022), 3); paint(x, '#d0202a', L * 0.4);
    rr(x, X(0.34), Y(0.815), X(0.32), Y(0.04), 5); paint(x, '#c9d0d8', L * 0.6);
    [0.255, 0.745].forEach(function (ex) {
      ellipse(x, X(ex), Y(0.84), X(0.04), Y(0.024)); paint(x, '#c9d0d8', L * 0.6);
      ellipse(x, X(ex), Y(0.842), X(0.025), Y(0.012)); x.fillStyle = '#15151f'; x.fill();
    });
    return c;
  }

  /* --------------------------------------------------- generic traffic hatchback */

  var PLATES = ['AB-123-CD', 'GT-404-ZZ', 'PS-013-AA', 'LO-777-L', 'MA-136-RS', 'BZ-842-OK'];

  function hatch(color, view, o) {
    o = o || {};
    var W = 256, H = 200, c = mk(W, H), x = c.getContext('2d');
    var L = lwOf(W, o.worldW || 1050);
    var X = function (f) { return f * W; }, Y = function (f) { return f * H; };
    var hi = o.hi || light(color, 0.45), sh = o.sh || dark(color, 0.22);
    var ry = o.roofY != null ? o.roofY : 0.12;
    var idx = o.idx || 0;

    shadowEll(x, W / 2, H - 8, W * 0.47, 9, 0.28);
    tyre(x, X(0.09), Y(0.74), X(0.16), Y(0.245), L);
    tyre(x, X(0.75), Y(0.74), X(0.16), Y(0.245), L);
    mirror(x, X(0.005), Y(0.395), X(0.11), Y(0.07), color, L);
    mirror(x, X(0.885), Y(0.395), X(0.11), Y(0.07), color, L);

    poly(x, [[X(0.27), Y(ry + 0.005)], [X(0.31), Y(ry - 0.05)], [X(0.69), Y(ry - 0.05)], [X(0.73), Y(ry + 0.005)]]);
    paint(x, hi, L);

    if (o.police) {
      rr(x, X(0.3), Y(ry - 0.1), X(0.4), Y(0.06), Y(0.02)); paint(x, '#2a2a38', L * 0.8);
      x.save(); x.shadowColor = '#4d8dff'; x.shadowBlur = 8;
      rr(x, X(0.32), Y(ry - 0.09), X(0.14), Y(0.04), 3); x.fillStyle = '#3d7bff'; x.fill();
      rr(x, X(0.54), Y(ry - 0.09), X(0.14), Y(0.04), 3); x.fill();
      x.restore();
      rr(x, X(0.47), Y(ry - 0.09), X(0.06), Y(0.04), 2); x.fillStyle = '#f2f2f2'; x.fill();
      rr(x, X(0.34), Y(ry - 0.085), X(0.05), Y(0.012), 2); x.fillStyle = '#d4e3ff'; x.fill();
      rr(x, X(0.56), Y(ry - 0.085), X(0.05), Y(0.012), 2); x.fill();
    }

    var gh = function () { trap(x, X(0.13), X(0.87), Y(0.48), X(0.24), X(0.76), Y(ry), Y(0.05)); };
    gh(); paint(x, color, L);

    if (view === 'rear') {
      var win = function () { trap(x, X(0.19), X(0.81), Y(0.455), X(0.285), X(0.715), Y(ry + 0.045), Y(0.035)); };
      glass(x, win, X(0.19), Y(ry + 0.045), X(0.81), Y(0.455), L * 0.7, function () {
        headBack(x, X(0.36), Y(0.36), X(0.05), idx);
        if (idx % 2) headBack(x, X(0.64), Y(0.37), X(0.048), idx + 2);
        else { rr(x, X(0.57), Y(0.375), X(0.13), Y(0.085), X(0.03)); x.fillStyle = '#3a3a4a'; x.fill(); }
      });
    } else {
      var ws = function () { trap(x, X(0.17), X(0.83), Y(0.465), X(0.28), X(0.72), Y(ry + 0.04), Y(0.035)); };
      glass(x, ws, X(0.17), Y(ry + 0.04), X(0.83), Y(0.465), L * 0.7, function () {
        driverFront(x, X(0.635), Y(0.3), X(0.055), idx, false);
        ellipse(x, X(0.635), Y(0.455), X(0.075), Y(0.03)); paint(x, null, L * 0.6, '#15151f');
      });
    }

    var body = function () { bodyPath(x, X, Y, 0.44, 0.885, 0.04, 0.96); };
    shaded(x, body, color, hi, sh, Y(0.44), Y(0.49), Y(0.71), Y(0.9), L);

    if (view === 'rear') {
      if (idx % 2 === 0) {
        [0.055, 0.805].forEach(function (lx) {
          rr(x, X(lx), Y(0.5), X(0.14), Y(0.135), X(0.03)); paint(x, '#d4202f', L * 0.7);
          rr(x, X(lx + 0.02), Y(0.585), X(0.1), Y(0.03), 2); x.fillStyle = '#ffb347'; x.fill();
          rr(x, X(lx + 0.02), Y(0.515), X(0.1), Y(0.018), 2); x.fillStyle = '#ff7b86'; x.fill();
        });
      } else {
        [0.055, 0.705].forEach(function (lx) {
          rr(x, X(lx), Y(0.515), X(0.24), Y(0.075), X(0.025)); paint(x, '#d4202f', L * 0.7);
          rr(x, X(lx + 0.03), Y(0.528), X(0.18), Y(0.018), 2); x.fillStyle = '#ff7b86'; x.fill();
        });
      }
      if (o.police) {
        label(x, 'GENDARMERIE', X(0.5), Y(0.545), Y(0.055), '#ffffff', 0, null, X(0.5));
      } else {
        circle(x, X(0.5), Y(0.55), X(0.025)); paint(x, '#d8dde3', L * 0.5);
      }
      plate(x, X(0.385), Y(0.6), X(0.23), Y(0.09), PLATES[idx % PLATES.length], L * 0.55, String(13 + idx * 7));
      rr(x, X(0.07), Y(0.79), X(0.86), Y(0.075), Y(0.03)); paint(x, '#2a2a36', L * 0.7);
      ellipse(x, X(0.28), Y(0.872), X(0.03), Y(0.018)); paint(x, '#c9d0d8', L * 0.5);
    } else {
      // bonnet sheen
      [-1, 1].forEach(function (k) {
        var p = k < 0
          ? [[0.06, 0.52], [0.27, 0.545], [0.255, 0.615], [0.075, 0.6]]
          : [[0.94, 0.52], [0.73, 0.545], [0.745, 0.615], [0.925, 0.6]];
        poly(x, p.map(function (q) { return [X(q[0]), Y(q[1])]; }));
        paint(x, '#fff6c8', L * 0.7);
        circle(x, X(k < 0 ? 0.15 : 0.85), Y(0.57), X(0.025)); x.fillStyle = '#ffffff'; x.fill();
      });
      rr(x, X(0.32), Y(0.56), X(0.36), Y(0.1), Y(0.035)); paint(x, '#262633', L * 0.7);
      line(x, [[X(0.35), Y(0.595)], [X(0.65), Y(0.595)]], L * 0.35, '#5a5a70');
      line(x, [[X(0.35), Y(0.628)], [X(0.65), Y(0.628)]], L * 0.35, '#5a5a70');
      if (o.police) {
        label(x, 'GENDARMERIE', X(0.5), Y(0.505), Y(0.045), '#ffffff', 0, null, X(0.42));
      }
      circle(x, X(0.5), Y(0.61), X(0.022)); paint(x, '#d8dde3', L * 0.4);
      plate(x, X(0.39), Y(0.69), X(0.22), Y(0.075), PLATES[(idx + 3) % PLATES.length], L * 0.5, null);
      rr(x, X(0.25), Y(0.79), X(0.5), Y(0.06), Y(0.025)); paint(x, '#262633', L * 0.6);
      circle(x, X(0.15), Y(0.8), X(0.025)); paint(x, '#fff3b0', L * 0.5);
      circle(x, X(0.85), Y(0.8), X(0.025)); paint(x, '#fff3b0', L * 0.5);
    }
    return c;
  }

  /* ---------------------------------------------------------------- 2CV */

  function deuche(view) {
    var W = 256, H = 220, c = mk(W, H), x = c.getContext('2d');
    var L = lwOf(W, 950);
    var X = function (f) { return f * W; }, Y = function (f) { return f * H; };
    var cream = '#f4e6c4', creamHi = '#fff7e2', creamSh = '#d9c79f';
    var red = '#8e1c36', redHi = '#c2405a', redSh = '#651126';

    shadowEll(x, W / 2, H - 8, W * 0.44, 9, 0.28);
    tyre(x, X(0.15), Y(0.74), X(0.12), Y(0.245), L);
    tyre(x, X(0.73), Y(0.74), X(0.12), Y(0.245), L);

    var body = function () {
      x.beginPath();
      x.moveTo(X(0.13), Y(0.86));
      x.lineTo(X(0.15), Y(0.46));
      x.bezierCurveTo(X(0.15), Y(-0.07), X(0.85), Y(-0.07), X(0.85), Y(0.46));
      x.lineTo(X(0.87), Y(0.86));
      x.closePath();
    };
    body(); x.fillStyle = red; x.fill();
    clipped(x, body, function () {
      x.fillStyle = cream; x.fillRect(0, 0, W, Y(0.47));
      x.fillStyle = creamHi; x.fillRect(0, 0, W, Y(0.14));
      x.fillStyle = redSh; x.fillRect(0, Y(0.74), W, Y(0.2));
      x.fillStyle = redHi; x.fillRect(0, Y(0.47), W, Y(0.03));
    });
    body(); paint(x, null, L);
    line(x, [[X(0.15), Y(0.47)], [X(0.85), Y(0.47)]], L * 0.6);

    if (view === 'rear') {
      // canvas roof seam
      line(x, [[X(0.32), Y(0.135)], [X(0.68), Y(0.135)]], L * 0.4, creamSh);
      var win = function () { rr(x, X(0.33), Y(0.17), X(0.34), Y(0.17), X(0.04)); };
      glass(x, win, X(0.33), Y(0.17), X(0.67), Y(0.34), L * 0.7, function () {
        headBack(x, X(0.42), Y(0.32), X(0.045), 1);
      });
      rr(x, X(0.3), Y(0.5), X(0.4), Y(0.29), X(0.05)); paint(x, null, L * 0.45, redSh);
      rr(x, X(0.46), Y(0.54), X(0.08), Y(0.025), 3); paint(x, '#d8dde3', L * 0.4);
      plate(x, X(0.38), Y(0.65), X(0.24), Y(0.085), 'DC-002-CV', L * 0.5, '84');
    } else {
      var ws = function () { rr(x, X(0.24), Y(0.15), X(0.52), Y(0.26), X(0.04)); };
      glass(x, ws, X(0.24), Y(0.15), X(0.76), Y(0.41), L * 0.7, function () {
        driverFront(x, X(0.63), Y(0.27), X(0.055), 3, true);
      });
      // ribbed bonnet
      var bon = function () { poly(x, [[X(0.29), Y(0.47)], [X(0.71), Y(0.47)], [X(0.75), Y(0.8)], [X(0.25), Y(0.8)]]); };
      bon(); paint(x, red, L * 0.8);
      for (var i = 0; i < 5; i++) {
        var fx0 = 0.37 + i * 0.065;
        line(x, [[X(fx0), Y(0.5)], [X(fx0 + (fx0 - 0.5) * 0.12), Y(0.64)]], L * 0.45, redSh);
      }
      rr(x, X(0.33), Y(0.67), X(0.34), Y(0.12), X(0.03)); paint(x, '#2a2a36', L * 0.6);
      for (var j = 0; j < 3; j++) line(x, [[X(0.36), Y(0.7 + j * 0.03)], [X(0.64), Y(0.7 + j * 0.03)]], L * 0.3, '#8b8ea0');
      chevrons(x, X(0.5), Y(0.6), X(0.09), Y(0.06), '#e3e8ee', L * 0.3);
    }

    // bulbous wings
    [0.17, 0.83].forEach(function (fx0) {
      x.beginPath();
      x.ellipse(X(fx0), Y(0.8), X(0.15), Y(0.2), 0, Math.PI, TAU);
      x.closePath();
      paint(x, red, L);
      clipped(x, function () { x.beginPath(); x.ellipse(X(fx0), Y(0.8), X(0.15), Y(0.2), 0, Math.PI, TAU); x.closePath(); }, function () {
        x.fillStyle = redHi; x.fillRect(X(fx0 - 0.15), Y(0.6), X(0.3), Y(0.04));
      });
      x.beginPath(); x.ellipse(X(fx0), Y(0.8), X(0.15), Y(0.2), 0, Math.PI, TAU); x.closePath(); paint(x, null, L);
      if (view === 'rear') {
        circle(x, X(fx0 + (fx0 < 0.5 ? -0.03 : 0.03)), Y(0.665), X(0.032)); paint(x, '#e0283a', L * 0.6);
      }
    });
    if (view !== 'rear') {
      [0.17, 0.83].forEach(function (fx0) {
        line(x, [[X(fx0), Y(0.6)], [X(fx0 + (fx0 < 0.5 ? 0.06 : -0.06)), Y(0.53)]], L * 0.7, '#a8afb8');
        circle(x, X(fx0), Y(0.52), X(0.075)); paint(x, '#d8dde3', L);
        circle(x, X(fx0), Y(0.52), X(0.05)); paint(x, '#fff6c8');
        circle(x, X(fx0 - 0.018), Y(0.505), X(0.018)); x.fillStyle = '#fff'; x.fill();
      });
    }
    // thin chrome bumper with overriders
    rr(x, X(0.08), Y(0.82), X(0.84), Y(0.045), 4); paint(x, '#d8dde3', L * 0.7);
    rr(x, X(0.3), Y(0.78), X(0.035), Y(0.1), 3); paint(x, '#d8dde3', L * 0.5);
    rr(x, X(0.665), Y(0.78), X(0.035), Y(0.1), 3); paint(x, '#d8dde3', L * 0.5);
    return c;
  }

  /* ---------------------------------------------------------------- van */

  function van(view) {
    var W = 256, H = 280, c = mk(W, H), x = c.getContext('2d');
    var L = lwOf(W, 1150);
    var X = function (f) { return f * W; }, Y = function (f) { return f * H; };
    var base = '#f4f4f0', hi = '#ffffff', sh = '#cfd4dc';

    shadowEll(x, W / 2, H - 8, W * 0.47, 9, 0.28);
    tyre(x, X(0.085), Y(0.8), X(0.17), Y(0.185), L);
    tyre(x, X(0.745), Y(0.8), X(0.17), Y(0.185), L);
    rr(x, X(0.0), Y(0.27), X(0.08), Y(0.13), 4); paint(x, '#2a2a36', L);
    rr(x, X(0.92), Y(0.27), X(0.08), Y(0.13), 4); paint(x, '#2a2a36', L);

    var body = function () { bodyPath(x, X, Y, 0.05, 0.86, 0.05, 0.95, 0.07); };
    shaded(x, body, base, hi, sh, Y(0.05), Y(0.09), Y(0.68), Y(0.9), L);

    if (view === 'rear') {
      line(x, [[X(0.5), Y(0.08)], [X(0.5), Y(0.76)]], L * 0.5);
      [0.12, 0.555].forEach(function (wx) {
        var w = function () { rr(x, X(wx), Y(0.11), X(0.325), Y(0.18), X(0.03)); };
        glass(x, w, X(wx), Y(0.11), X(wx + 0.325), Y(0.29), L * 0.6);
      });
      label(x, 'BOULANGERIE', X(0.5), Y(0.37), Y(0.06), '#b5512a', L * 0.35, '#ffffff', X(0.8));
      // baguette + croissant
      x.save(); x.translate(X(0.42), Y(0.5)); x.rotate(-0.18);
      rr(x, -X(0.24), -Y(0.03), X(0.48), Y(0.06), Y(0.03)); paint(x, '#e5a95a', L * 0.6);
      for (var i = -2; i <= 2; i++) line(x, [[X(i * 0.08 - 0.02), Y(0.012)], [X(i * 0.08 + 0.02), -Y(0.012)]], L * 0.4, '#b9773a');
      x.restore();
      x.beginPath(); x.arc(X(0.75), Y(0.53), X(0.07), Math.PI * 1.05, Math.PI * 1.95); x.arc(X(0.75), Y(0.53), X(0.035), Math.PI * 1.95, Math.PI * 1.05, true); x.closePath();
      paint(x, '#e9a548', L * 0.6);
      [0.055, 0.89].forEach(function (lx) {
        rr(x, X(lx), Y(0.48), X(0.055), Y(0.22), 4); paint(x, '#d4202f', L * 0.6);
        rr(x, X(lx), Y(0.62), X(0.055), Y(0.04), 2); x.fillStyle = '#ffb347'; x.fill();
      });
      rr(x, X(0.46), Y(0.56), X(0.08), Y(0.03), 3); paint(x, '#2a2a36', L * 0.4);
      rr(x, X(0.04), Y(0.77), X(0.92), Y(0.1), Y(0.03)); paint(x, '#2a2a36', L);
      plate(x, X(0.39), Y(0.79), X(0.22), Y(0.065), 'PA-1N-AA', L * 0.5, '13');
    } else {
      var ws = function () { trap(x, X(0.1), X(0.9), Y(0.5), X(0.16), X(0.84), Y(0.14), Y(0.03)); };
      glass(x, ws, X(0.1), Y(0.14), X(0.9), Y(0.5), L * 0.7, function () {
        driverFront(x, X(0.66), Y(0.3), X(0.06), 2, false);
        ellipse(x, X(0.66), Y(0.48), X(0.08), Y(0.025)); paint(x, null, L * 0.6, '#15151f');
      });
      label(x, 'PAIN CHAUD', X(0.5), Y(0.095), Y(0.045), '#b5512a', 0, null, X(0.6));
      [-1, 1].forEach(function (k) {
        var lx = k < 0 ? 0.07 : 0.73;
        rr(x, X(lx), Y(0.57), X(0.2), Y(0.07), X(0.03)); paint(x, '#fff6c8', L * 0.7);
      });
      rr(x, X(0.31), Y(0.57), X(0.38), Y(0.1), X(0.03)); paint(x, '#262633', L * 0.7);
      line(x, [[X(0.34), Y(0.605)], [X(0.66), Y(0.605)]], L * 0.35, '#5a5a70');
      line(x, [[X(0.34), Y(0.635)], [X(0.66), Y(0.635)]], L * 0.35, '#5a5a70');
      rr(x, X(0.04), Y(0.72), X(0.92), Y(0.14), Y(0.04)); paint(x, '#2a2a36', L);
      plate(x, X(0.39), Y(0.75), X(0.22), Y(0.065), 'PA-1N-AA', L * 0.5, null);
    }
    return c;
  }

  /* ---------------------------------------------------------------- truck */

  function truck(view) {
    var W = 256, H = 380, c = mk(W, H), x = c.getContext('2d');
    var L = lwOf(W, 1350);
    var X = function (f) { return f * W; }, Y = function (f) { return f * H; };

    shadowEll(x, W / 2, H - 8, W * 0.48, 10, 0.3);
    if (view === 'rear') {
      [0.05, 0.18, 0.7, 0.83].forEach(function (tx) { tyre(x, X(tx), Y(0.85), X(0.12), Y(0.135), L); });
      var box = function () { rr(x, X(0.03), Y(0.015), X(0.94), Y(0.81), X(0.03)); };
      shaded(x, box, '#f6f6f2', '#ffffff', '#d9dde3', Y(0.015), Y(0.04), Y(0.7), Y(0.83), L);
      clipped(x, box, function () {
        x.strokeStyle = '#e3e6ea'; x.lineWidth = 2;
        for (var i = 1; i < 12; i++) { x.beginPath(); x.moveTo(X(i / 12), Y(0.03)); x.lineTo(X(i / 12), Y(0.8)); x.stroke(); }
      });
      line(x, [[X(0.5), Y(0.03)], [X(0.5), Y(0.8)]], L * 0.5);
      [0.3, 0.42, 0.58, 0.7].forEach(function (bx) { line(x, [[X(bx), Y(0.05)], [X(bx), Y(0.78)]], L * 0.55, '#9aa1ab'); });
      // lavender logo
      label(x, 'LAVANDE', X(0.5), Y(0.2), Y(0.07), '#8b5fd1', L * 0.45, OL, X(0.86));
      label(x, 'EXPRESS', X(0.5), Y(0.29), Y(0.055), '#ffd23f', L * 0.45, OL, X(0.7));
      for (var s = 0; s < 5; s++) {
        var sx = X(0.32 + s * 0.09);
        line(x, [[sx, Y(0.48)], [sx + (s - 2) * 3, Y(0.38)]], L * 0.4, '#5a8a50');
        ellipse(x, sx + (s - 2) * 3, Y(0.37), X(0.018), Y(0.035)); paint(x, '#9a6be0', L * 0.35);
      }
      // reflective band
      clipped(x, function () { x.beginPath(); x.rect(X(0.04), Y(0.775), X(0.92), Y(0.035)); }, function () {
        x.fillStyle = '#ffffff'; x.fillRect(0, Y(0.775), W, Y(0.035));
        x.fillStyle = '#e0283a';
        for (var k = -2; k < 20; k++) {
          poly(x, [[X(k * 0.08), Y(0.81)], [X(k * 0.08 + 0.04), Y(0.81)], [X(k * 0.08 + 0.075), Y(0.775)], [X(k * 0.08 + 0.035), Y(0.775)]]); x.fill();
        }
      });
      x.beginPath(); x.rect(X(0.04), Y(0.775), X(0.92), Y(0.035)); paint(x, null, L * 0.5);
      rr(x, X(0.1), Y(0.855), X(0.8), Y(0.035), 3); paint(x, '#3a3a48', L * 0.7);
      [0.03, 0.83].forEach(function (lx) {
        rr(x, X(lx), Y(0.835), X(0.14), Y(0.045), 4); paint(x, '#d4202f', L * 0.6);
        rr(x, X(lx + 0.09), Y(0.84), X(0.04), Y(0.035), 2); x.fillStyle = '#ffb347'; x.fill();
      });
      plate(x, X(0.4), Y(0.86), X(0.2), Y(0.042), 'TR-813-UK', L * 0.4, '04');
    } else {
      tyre(x, X(0.07), Y(0.84), X(0.15), Y(0.145), L);
      tyre(x, X(0.78), Y(0.84), X(0.15), Y(0.145), L);
      // roof deflector
      poly(x, [[X(0.08), Y(0.15)], [X(0.15), Y(0.03)], [X(0.85), Y(0.03)], [X(0.92), Y(0.15)]]);
      paint(x, '#4d8ae6', L);
      [0.35, 0.45, 0.55, 0.65].forEach(function (lx) { circle(x, X(lx), Y(0.065), X(0.018)); paint(x, '#ffb020', L * 0.4); });
      // mirrors on arms
      line(x, [[X(0.06), Y(0.3)], [X(0.02), Y(0.3)]], L * 0.6, '#2a2a36');
      line(x, [[X(0.94), Y(0.3)], [X(0.98), Y(0.3)]], L * 0.6, '#2a2a36');
      rr(x, X(0.0), Y(0.2), X(0.05), Y(0.17), 4); paint(x, '#2a2a36', L * 0.8);
      rr(x, X(0.95), Y(0.2), X(0.05), Y(0.17), 4); paint(x, '#2a2a36', L * 0.8);
      var cab = function () { rr(x, X(0.06), Y(0.13), X(0.88), Y(0.72), X(0.05)); };
      shaded(x, cab, '#2d6fd6', '#6aa0f0', '#1f4fa0', Y(0.13), Y(0.16), Y(0.74), Y(0.9), L);
      rr(x, X(0.1), Y(0.17), X(0.8), Y(0.04), 4); paint(x, '#1a2a50', L * 0.6);
      var ws = function () { rr(x, X(0.11), Y(0.21), X(0.78), Y(0.24), X(0.03)); };
      glass(x, ws, X(0.11), Y(0.21), X(0.89), Y(0.45), L * 0.7, function () {
        driverFront(x, X(0.7), Y(0.32), X(0.06), 4, false);
        ellipse(x, X(0.7), Y(0.44), X(0.09), Y(0.02)); paint(x, null, L * 0.6, '#15151f');
      });
      line(x, [[X(0.5), Y(0.21)], [X(0.5), Y(0.45)]], L * 0.6);
      rr(x, X(0.22), Y(0.5), X(0.56), Y(0.24), X(0.03)); paint(x, '#d8dde3', L);
      for (var g = 0; g < 6; g++) line(x, [[X(0.26), Y(0.535 + g * 0.033)], [X(0.74), Y(0.535 + g * 0.033)]], L * 0.55, '#3a3a48');
      label(x, 'PROVENCE', X(0.5), Y(0.475), Y(0.03), '#ffffff', 0, null, X(0.5));
      [0.08, 0.76].forEach(function (lx) {
        rr(x, X(lx), Y(0.64), X(0.16), Y(0.07), X(0.02)); paint(x, '#fff6c8', L * 0.7);
      });
      rr(x, X(0.04), Y(0.75), X(0.92), Y(0.09), X(0.03)); paint(x, '#9aa1ab', L);
      plate(x, X(0.4), Y(0.775), X(0.2), Y(0.04), 'TR-813-UK', L * 0.4, null);
      rr(x, X(0.1), Y(0.85), X(0.1), Y(0.025), 2); paint(x, '#2a2a36', L * 0.5);
      rr(x, X(0.8), Y(0.85), X(0.1), Y(0.025), 2); paint(x, '#2a2a36', L * 0.5);
    }
    return c;
  }

  /* ---------------------------------------------------------------- tractor */

  function bigWheel(x, X, Y, x0, L) {
    var y0 = Y(0.38), w = X(0.27), h = Y(0.605);
    rr(x, x0, y0, w, h, w * 0.4); paint(x, '#2b2b38', L);
    clipped(x, function () { rr(x, x0, y0, w, h, w * 0.4); }, function () {
      x.fillStyle = '#4b4b60';
      for (var i = 0; i < 9; i++) {
        var yy = y0 + h * (i + 0.3) / 9;
        poly(x, [[x0 + w * 0.12, yy], [x0 + w * 0.5, yy + h * 0.045], [x0 + w * 0.88, yy], [x0 + w * 0.88, yy + h * 0.035], [x0 + w * 0.5, yy + h * 0.08], [x0 + w * 0.12, yy + h * 0.035]]);
        x.fill();
      }
    });
  }

  function tractor(view) {
    var W = 256, H = 290, c = mk(W, H), x = c.getContext('2d');
    var L = lwOf(W, 1100);
    var X = function (f) { return f * W; }, Y = function (f) { return f * H; };
    var green = '#3fae4a', greenHi = '#7bd67f', greenSh = '#2a7f33';

    shadowEll(x, W / 2, H - 8, W * 0.49, 10, 0.3);
    bigWheel(x, X, Y, X(0.005), L);
    bigWheel(x, X, Y, X(0.725), L);

    // cab
    rr(x, X(0.19), Y(0.06), X(0.62), Y(0.06), Y(0.02)); paint(x, '#f4f4f0', L);
    // beacon
    x.save(); x.shadowColor = '#ffae00'; x.shadowBlur = 10;
    x.beginPath(); x.arc(X(0.7), Y(0.06), X(0.035), Math.PI, TAU); x.closePath(); x.fillStyle = '#ff9a1a'; x.fill();
    x.restore();
    x.beginPath(); x.arc(X(0.7), Y(0.06), X(0.035), Math.PI, TAU); x.closePath(); paint(x, null, L * 0.7);
    [[-0.07, -0.01], [0.07, -0.01], [0, -0.05]].forEach(function (r) {
      line(x, [[X(0.7 + r[0] * 0.6), Y(0.045 + r[1] * 0.5)], [X(0.7 + r[0]), Y(0.045 + r[1])]], L * 0.4, '#ff9a1a');
    });
    var cab = function () { rr(x, X(0.23), Y(0.11), X(0.54), Y(0.5), X(0.03)); };
    cab(); paint(x, '#2a2a36', L);
    var win = function () { rr(x, X(0.27), Y(0.135), X(0.46), Y(0.43), X(0.02)); };
    glass(x, win, X(0.27), Y(0.135), X(0.73), Y(0.565), L * 0.6, function () {
      if (view === 'rear') {
        headBack(x, X(0.5), Y(0.36), X(0.07), 4);
      } else {
        driverFront(x, X(0.5), Y(0.33), X(0.07), 0, true);
        ellipse(x, X(0.5), Y(0.53), X(0.1), Y(0.025)); paint(x, null, L * 0.6, '#15151f');
      }
    });
    line(x, [[X(0.5), Y(0.135)], [X(0.5), Y(0.2)]], L * 0.3, '#2a2a36');

    // fenders
    [0.0, 0.715].forEach(function (fx0) {
      rr(x, X(fx0), Y(0.33), X(0.285), Y(0.07), Y(0.03)); paint(x, green, L);
      rr(x, X(fx0 + 0.02), Y(0.338), X(0.245), Y(0.015), 2); x.fillStyle = greenHi; x.fill();
    });

    if (view === 'rear') {
      var chassis = function () { rr(x, X(0.28), Y(0.6), X(0.44), Y(0.18), X(0.03)); };
      shaded(x, chassis, green, greenHi, greenSh, Y(0.6), Y(0.625), Y(0.72), Y(0.8), L);
      // slow-vehicle triangle
      poly(x, [[X(0.5), Y(0.625)], [X(0.565), Y(0.745)], [X(0.435), Y(0.745)]]); paint(x, '#ff7a1a', L * 0.6);
      poly(x, [[X(0.5), Y(0.66)], [X(0.535), Y(0.725)], [X(0.465), Y(0.725)]]); paint(x, '#ffcf8a');
      // three-point hitch
      line(x, [[X(0.33), Y(0.78)], [X(0.38), Y(0.9)]], L * 1.1, '#6b6f7a');
      line(x, [[X(0.67), Y(0.78)], [X(0.62), Y(0.9)]], L * 1.1, '#6b6f7a');
      line(x, [[X(0.5), Y(0.78)], [X(0.5), Y(0.86)]], L * 0.9, '#6b6f7a');
      [0.27, 0.73].forEach(function (lx) { circle(x, X(lx), Y(0.36), X(0.02)); paint(x, '#e0283a', L * 0.4); });
    } else {
      tyre(x, X(0.29), Y(0.8), X(0.13), Y(0.185), L);
      tyre(x, X(0.58), Y(0.8), X(0.13), Y(0.185), L);
      var hood = function () { poly(x, [[X(0.33), Y(0.55)], [X(0.67), Y(0.55)], [X(0.7), Y(0.86)], [X(0.3), Y(0.86)]]); };
      shaded(x, hood, green, greenHi, greenSh, Y(0.55), Y(0.58), Y(0.8), Y(0.9), L);
      rr(x, X(0.37), Y(0.64), X(0.26), Y(0.19), X(0.02)); paint(x, '#2a2a36', L * 0.6);
      for (var i = 0; i < 5; i++) line(x, [[X(0.4), Y(0.665 + i * 0.035)], [X(0.6), Y(0.665 + i * 0.035)]], L * 0.35, '#7a7f8c');
      circle(x, X(0.38), Y(0.6), X(0.03)); paint(x, '#fff6c8', L * 0.5);
      circle(x, X(0.62), Y(0.6), X(0.03)); paint(x, '#fff6c8', L * 0.5);
      // yellow hubs of the front wheels
      circle(x, X(0.355), Y(0.9), X(0.025)); paint(x, '#ffd23f', L * 0.4);
      circle(x, X(0.645), Y(0.9), X(0.025)); paint(x, '#ffd23f', L * 0.4);
    }
    return c;
  }

  /* --------------------------------------------------------------- scenery */

  function cypress() {
    var W = 150, H = 500, c = mk(W, H), x = c.getContext('2d');
    var L = lwOf(W, 700);
    shadowEll(x, W / 2, H - 8, W * 0.36, 7, 0.25);
    rr(x, W / 2 - 9, H * 0.86, 18, H * 0.12, 4); paint(x, '#7a5636', L);
    var top = H * 0.015 + L, bot = H * 0.92, maxW = W * 0.44;
    var pts = [];
    for (var i = 0; i <= 60; i++) {
      var s = i / 60, y = top + (bot - top) * s;
      var hw = maxW * Math.pow(s, 0.72) * Math.sqrt(Math.max(0, 1 - Math.pow(s, 7)));
      hw *= 1 + 0.07 * Math.sin(s * 46);
      pts.push([y, hw]);
    }
    var shape = function () {
      x.beginPath();
      x.moveTo(W / 2, top);
      pts.forEach(function (p) { x.lineTo(W / 2 - p[1], p[0]); });
      for (var j = pts.length - 1; j >= 0; j--) x.lineTo(W / 2 + pts[j][1], pts[j][0]);
      x.closePath();
    };
    shape(); x.fillStyle = '#2f6e3b'; x.fill();
    clipped(x, shape, function () {
      x.fillStyle = '#1f4d2a'; x.fillRect(W / 2 + 4, 0, W, H);
      x.strokeStyle = '#4f9a52'; x.lineWidth = L * 0.9; x.lineCap = 'round';
      for (var k = 0; k < 9; k++) {
        var yy = H * (0.12 + k * 0.085);
        x.beginPath(); x.moveTo(W * 0.36, yy + 14); x.quadraticCurveTo(W * 0.42, yy, W * 0.5, yy - 8); x.stroke();
      }
      x.strokeStyle = '#163d21';
      for (var m = 0; m < 7; m++) {
        var y2 = H * (0.18 + m * 0.1);
        x.beginPath(); x.moveTo(W * 0.58, y2 - 8); x.quadraticCurveTo(W * 0.66, y2, W * 0.7, y2 + 14); x.stroke();
      }
    });
    shape(); paint(x, null, L);
    return c;
  }

  function olive() {
    var W = 400, H = 340, c = mk(W, H), x = c.getContext('2d');
    var L = lwOf(W, 1800);
    shadowEll(x, W / 2, H - 9, W * 0.36, 10, 0.25);
    // twisted trunk
    var trunk = function () {
      x.beginPath();
      x.moveTo(W * 0.4, H - 4);
      x.bezierCurveTo(W * 0.44, H * 0.85, W * 0.36, H * 0.75, W * 0.32, H * 0.5);
      x.lineTo(W * 0.4, H * 0.5);
      x.bezierCurveTo(W * 0.44, H * 0.65, W * 0.5, H * 0.7, W * 0.52, H * 0.62);
      x.bezierCurveTo(W * 0.56, H * 0.56, W * 0.6, H * 0.52, W * 0.64, H * 0.48);
      x.lineTo(W * 0.7, H * 0.52);
      x.bezierCurveTo(W * 0.6, H * 0.62, W * 0.58, H * 0.8, W * 0.62, H - 4);
      x.closePath();
    };
    shaded(x, trunk, '#857360', '#a8957e', '#5f5042', 0, 0, H * 0.82, H, L);
    line(x, [[W * 0.45, H * 0.92], [W * 0.5, H * 0.75], [W * 0.47, H * 0.66]], L * 0.6, '#5f5042');
    line(x, [[W * 0.56, H * 0.9], [W * 0.55, H * 0.78]], L * 0.6, '#a8957e');
    var cs = [
      [W * 0.22, H * 0.42, W * 0.13], [W * 0.36, H * 0.3, W * 0.15], [W * 0.52, H * 0.22, W * 0.16],
      [W * 0.68, H * 0.3, W * 0.15], [W * 0.8, H * 0.42, W * 0.12], [W * 0.5, H * 0.42, W * 0.15],
      [W * 0.33, H * 0.48, W * 0.11, 0], [W * 0.66, H * 0.5, W * 0.11, 0], [W * 0.12, H * 0.5, W * 0.09, 0], [W * 0.9, H * 0.52, W * 0.085, 0]
    ];
    blob(x, cs, '#9db27c', '#cad9a8', '#6f8a5a', L);
    // leaf strokes + olives
    var r = rng(7);
    x.lineCap = 'round';
    for (var i = 0; i < 40; i++) {
      var k = cs[Math.floor(r() * cs.length)];
      var a = r() * TAU, d = r() * k[2] * 0.75;
      var px = k[0] + Math.cos(a) * d, py = k[1] + Math.sin(a) * d;
      x.strokeStyle = r() < 0.5 ? '#e0ebc8' : '#5e7a4a'; x.lineWidth = 2.5;
      x.beginPath(); x.moveTo(px, py); x.lineTo(px + 7, py - 3); x.stroke();
    }
    for (var j = 0; j < 9; j++) {
      var kk = cs[Math.floor(r() * 6)];
      circle(x, kk[0] + (r() - 0.5) * kk[2], kk[1] + (r() - 0.2) * kk[2] * 0.6, 4.5);
      paint(x, '#3b2a4a', 1.5);
    }
    return c;
  }

  function pine() {
    var W = 512, H = 430, c = mk(W, H), x = c.getContext('2d');
    var L = lwOf(W, 2600);
    shadowEll(x, W / 2, H - 9, W * 0.3, 10, 0.25);
    var trunk = function () {
      x.beginPath();
      x.moveTo(W * 0.46, H - 4);
      x.bezierCurveTo(W * 0.49, H * 0.7, W * 0.44, H * 0.5, W * 0.5, H * 0.3);
      x.lineTo(W * 0.54, H * 0.3);
      x.bezierCurveTo(W * 0.5, H * 0.5, W * 0.56, H * 0.72, W * 0.54, H - 4);
      x.closePath();
    };
    shaded(x, trunk, '#9a5e3c', '#c27e57', '#6e3f27', 0, 0, H * 0.85, H, L);
    // branches
    line(x, [[W * 0.49, H * 0.4], [W * 0.33, H * 0.27]], L * 1.6);
    line(x, [[W * 0.49, H * 0.4], [W * 0.33, H * 0.27]], L * 0.9, '#9a5e3c');
    line(x, [[W * 0.51, H * 0.36], [W * 0.7, H * 0.25]], L * 1.6);
    line(x, [[W * 0.51, H * 0.36], [W * 0.7, H * 0.25]], L * 0.9, '#9a5e3c');
    var cs = [];
    for (var i = 0; i < 9; i++) {
      var f = i / 8;
      cs.push([W * (0.1 + f * 0.8), H * (0.2 - Math.sin(f * Math.PI) * 0.08), W * (0.075 + Math.sin(f * Math.PI) * 0.035)]);
    }
    cs.push([W * 0.3, H * 0.26, W * 0.08, 0], [W * 0.5, H * 0.27, W * 0.09, 0], [W * 0.7, H * 0.26, W * 0.08, 0]);
    blob(x, cs, '#3f7f3c', '#6db05a', '#2a5a2b', L);
    return c;
  }

  function plane() {
    var W = 400, H = 512, c = mk(W, H), x = c.getContext('2d');
    var L = lwOf(W, 2200);
    shadowEll(x, W / 2, H - 9, W * 0.3, 10, 0.25);
    var trunk = function () {
      x.beginPath();
      x.moveTo(W * 0.42, H - 4);
      x.bezierCurveTo(W * 0.45, H * 0.8, W * 0.44, H * 0.6, W * 0.4, H * 0.42);
      x.lineTo(W * 0.6, H * 0.42);
      x.bezierCurveTo(W * 0.56, H * 0.6, W * 0.55, H * 0.8, W * 0.58, H - 4);
      x.closePath();
    };
    trunk(); x.fillStyle = '#d9d0a8'; x.fill();
    clipped(x, trunk, function () {
      var r = rng(11);
      for (var i = 0; i < 16; i++) {
        ellipse(x, W * (0.4 + r() * 0.2), H * (0.45 + r() * 0.52), 6 + r() * 9, 8 + r() * 12, r());
        x.fillStyle = ['#a9a985', '#eee8c9', '#8f9a76'][i % 3]; x.fill();
      }
      x.fillStyle = 'rgba(0,0,0,0.16)'; x.fillRect(W * 0.53, 0, W, H);
    });
    trunk(); paint(x, null, L);
    var cs = [
      [W * 0.5, H * 0.17, W * 0.2], [W * 0.3, H * 0.27, W * 0.17], [W * 0.7, H * 0.27, W * 0.17],
      [W * 0.18, H * 0.42, W * 0.14], [W * 0.82, H * 0.42, W * 0.14], [W * 0.5, H * 0.36, W * 0.2],
      [W * 0.35, H * 0.47, W * 0.14, 0], [W * 0.65, H * 0.47, W * 0.14, 0]
    ];
    blob(x, cs, '#5aae47', '#94d96c', '#3b7f30', L);
    return c;
  }

  function bush(flower) {
    var W = 256, H = 180, c = mk(W, H), x = c.getContext('2d');
    var L = lwOf(W, flower ? 1000 : 900);
    shadowEll(x, W / 2, H - 7, W * 0.44, 8, 0.25);
    var cs = [
      [W * 0.25, H * 0.62, W * 0.2], [W * 0.5, H * 0.45, W * 0.25], [W * 0.75, H * 0.6, W * 0.2],
      [W * 0.38, H * 0.72, W * 0.17, 0], [W * 0.62, H * 0.72, W * 0.17, 0]
    ];
    blob(x, cs, flower ? '#4f9a46' : '#5bb04a', flower ? '#7cc466' : '#95d96d', flower ? '#356d30' : '#3b7f30', L);
    if (flower) {
      var r = rng(5);
      for (var i = 0; i < 16; i++) {
        var k = cs[Math.floor(r() * cs.length)];
        var a = r() * TAU, d = r() * k[2] * 0.7;
        var px = k[0] + Math.cos(a) * d, py = k[1] + Math.sin(a) * d - 4;
        for (var p = 0; p < 5; p++) {
          var pa = p / 5 * TAU;
          circle(x, px + Math.cos(pa) * 5, py + Math.sin(pa) * 5, 5); paint(x, '#ff7fb3', 1.5);
        }
        circle(x, px, py, 3.5); x.fillStyle = '#ffe14d'; x.fill();
      }
    }
    // keep base on the ground
    return c;
  }

  function lavender() {
    var W = 400, H = 180, c = mk(W, H), x = c.getContext('2d');
    var L = lwOf(W, 1600);
    shadowEll(x, W / 2, H - 7, W * 0.46, 8, 0.25);
    blob(x, [[W * 0.24, H * 0.92, W * 0.075], [W * 0.4, H * 0.9, W * 0.09], [W * 0.6, H * 0.9, W * 0.09], [W * 0.76, H * 0.92, W * 0.075]],
      '#7d9a72', '#a6bf9b', '#5f7a56', L);
    var r = rng(3);
    var spikes = [];
    for (var i = 0; i < 48; i++) {
      var f = i / 47;
      var bx = W * (0.18 + f * 0.64) + (r() - 0.5) * 8;
      var by = H * 0.86;
      var ang = (f - 0.5) * 1.1 + (r() - 0.5) * 0.16;
      var len = H * (0.36 + Math.sin(f * Math.PI) * 0.26 + r() * 0.05);
      spikes.push([bx, by, ang, len, i % 3 === 0]);
    }
    spikes.sort(function (a, b) { return (a[4] ? 0 : 1) - (b[4] ? 0 : 1); });
    spikes.forEach(function (s) {
      var tx = s[0] + Math.sin(s[2]) * s[3], ty = s[1] - Math.cos(s[2]) * s[3];
      line(x, [[s[0], s[1]], [tx, ty]], 3.5, '#5f7a58');
      var hx = s[0] + Math.sin(s[2]) * s[3] * 0.82, hy = s[1] - Math.cos(s[2]) * s[3] * 0.82;
      ellipse(x, hx, hy, 7, s[3] * 0.21, s[2]);
      paint(x, s[4] ? '#6a3fb0' : '#8f5fd8', 3);
      ellipse(x, hx - 2, hy - 3, 3, s[3] * 0.13, s[2]); x.fillStyle = s[4] ? '#946dd6' : '#c3a2f2'; x.fill();
    });
    return c;
  }

  function vine() {
    var W = 400, H = 220, c = mk(W, H), x = c.getContext('2d');
    var L = lwOf(W, 1400);
    shadowEll(x, W / 2, H - 7, W * 0.48, 8, 0.25);
    [0.02, 0.95].forEach(function (px) { rr(x, W * px, H * 0.2, W * 0.03, H * 0.78, 3); paint(x, '#9b7653', L); });
    line(x, [[W * 0.035, H * 0.3], [W * 0.965, H * 0.3]], 2, '#6a6a6a');
    [0.2, 0.42, 0.62, 0.82].forEach(function (px) {
      line(x, [[W * px, H * 0.98], [W * px + 4, H * 0.75], [W * px - 2, H * 0.6]], L * 1.6);
      line(x, [[W * px, H * 0.98], [W * px + 4, H * 0.75], [W * px - 2, H * 0.6]], L * 0.8, '#7a5636');
    });
    var cs = [];
    for (var i = 0; i < 9; i++) cs.push([W * (0.1 + i * 0.1), H * (0.45 + (i % 2) * 0.05), W * 0.075]);
    for (var j = 0; j < 6; j++) cs.push([W * (0.16 + j * 0.135), H * 0.58, W * 0.06, 0]);
    blob(x, cs, '#67b04a', '#a0d86f', '#4a8a35', L);
    [0.18, 0.37, 0.55, 0.74, 0.88].forEach(function (gx, n) {
      var gy = H * (0.66 + (n % 2) * 0.03);
      [[0, 0], [-6, -7], [6, -7], [-3, -14], [3, -14], [-9, -14], [9, -14], [0, 7]].forEach(function (o) {
        circle(x, W * gx + o[0], gy - o[1] * -1 + 0, 5); paint(x, '#6a2d82', 1.5);
      });
      circle(x, W * gx - 2, gy - 9, 2); x.fillStyle = '#b07ad0'; x.fill();
    });
    return c;
  }

  function sunflowers() {
    var W = 300, H = 320, c = mk(W, H), x = c.getContext('2d');
    var L = lwOf(W, 1200);
    shadowEll(x, W / 2, H - 7, W * 0.4, 8, 0.25);
    var flowers = [[0.3, 0.3, 0.13], [0.7, 0.22, 0.14], [0.5, 0.5, 0.12]];
    flowers.forEach(function (f, i) {
      var cx = W * f[0], cy = H * f[1], r = W * f[2];
      line(x, [[cx, cy], [cx + (i - 1) * 8, H - 4]], L * 2.2);
      line(x, [[cx, cy], [cx + (i - 1) * 8, H - 4]], L * 1.1, '#4c9a3a');
      [[-1, 0.62], [1, 0.75]].forEach(function (lf) {
        var ly = cy + (H - cy) * lf[1] * 0.7, lx = cx + (i - 1) * 8 * lf[1];
        ellipse(x, lx + lf[0] * r * 0.55, ly, r * 0.55, r * 0.22, lf[0] * -0.5);
        paint(x, '#5cb04a', L * 0.7);
      });
    });
    flowers.forEach(function (f, i) {
      var cx = W * f[0], cy = H * f[1], r = W * f[2];
      for (var p = 0; p < 12; p++) {
        var a = p / 12 * TAU + i;
        ellipse(x, cx + Math.cos(a) * r * 0.95, cy + Math.sin(a) * r * 0.95, r * 0.5, r * 0.22, a);
        paint(x, p % 2 ? '#ffd21f' : '#ffc000', L * 0.6);
      }
      circle(x, cx, cy, r * 0.62); paint(x, '#7a4a22', L * 0.8);
      circle(x, cx - r * 0.12, cy - r * 0.14, r * 0.38); x.fillStyle = '#93602f'; x.fill();
      // cute face
      circle(x, cx - r * 0.22, cy - r * 0.08, r * 0.09); x.fillStyle = '#1b1b2f'; x.fill();
      circle(x, cx + r * 0.22, cy - r * 0.08, r * 0.09); x.fill();
      x.beginPath(); x.arc(cx, cy + r * 0.08, r * 0.24, 0.15 * Math.PI, 0.85 * Math.PI);
      x.lineWidth = r * 0.08; x.strokeStyle = '#1b1b2f'; x.lineCap = 'round'; x.stroke();
      ellipse(x, cx - r * 0.38, cy + r * 0.12, r * 0.1, r * 0.06); x.fillStyle = '#ff8a6a'; x.fill();
      ellipse(x, cx + r * 0.38, cy + r * 0.12, r * 0.1, r * 0.06); x.fill();
    });
    return c;
  }

  function rock() {
    var W = 280, H = 180, c = mk(W, H), x = c.getContext('2d');
    var L = lwOf(W, 1100);
    shadowEll(x, W / 2, H - 6, W * 0.46, 7, 0.25);
    var pts = [[0.05, 0.97], [0.08, 0.62], [0.22, 0.36], [0.45, 0.16], [0.68, 0.24], [0.86, 0.44], [0.97, 0.76], [0.95, 0.97]]
      .map(function (p) { return [p[0] * W, p[1] * H]; });
    var shape = function () { poly(x, pts); };
    shape(); x.fillStyle = '#e9e3d1'; x.fill();
    clipped(x, shape, function () {
      poly(x, [[W * 0.45, H * 0.16], [W * 0.55, H * 0.55], [W * 0.95, H * 0.7], [W, H], [W, 0]]); x.fillStyle = '#c4bba3'; x.fill();
      poly(x, [[W * 0.1, H * 0.6], [W * 0.24, H * 0.38], [W * 0.42, H * 0.22], [W * 0.36, H * 0.48]]); x.fillStyle = '#fffaf0'; x.fill();
    });
    shape(); paint(x, null, L);
    line(x, [[W * 0.45, H * 0.16], [W * 0.55, H * 0.55], [W * 0.5, H * 0.95]], L * 0.6);
    line(x, [[W * 0.55, H * 0.55], [W * 0.82, H * 0.62]], L * 0.5);
    line(x, [[W * 0.2, H * 0.8], [W * 0.3, H * 0.7]], L * 0.4, '#a69c84');
    for (var i = 0; i < 6; i++) {
      var gx = W * (0.08 + i * 0.17);
      line(x, [[gx, H - 3], [gx - 5, H - 18]], 3, '#4c9a3a');
      line(x, [[gx + 3, H - 3], [gx + 6, H - 20]], 3, '#5cb04a');
    }
    return c;
  }

  function window_(x, wx, wy, ww, wh, shutter, L, arched) {
    // open shutters
    rr(x, wx - ww * 0.55, wy, ww * 0.5, wh, 2); paint(x, shutter, L * 0.7);
    rr(x, wx + ww * 1.05, wy, ww * 0.5, wh, 2); paint(x, shutter, L * 0.7);
    x.strokeStyle = dark(shutter, 0.3); x.lineWidth = Math.max(1, L * 0.35);
    for (var i = 1; i < 5; i++) {
      var yy = wy + wh * i / 5;
      x.beginPath(); x.moveTo(wx - ww * 0.5, yy); x.lineTo(wx - ww * 0.1, yy); x.stroke();
      x.beginPath(); x.moveTo(wx + ww * 1.1, yy); x.lineTo(wx + ww * 1.5, yy); x.stroke();
    }
    var pathFn = function () {
      if (arched) {
        x.beginPath(); x.moveTo(wx, wy + wh); x.lineTo(wx, wy + ww * 0.5);
        x.arc(wx + ww / 2, wy + ww * 0.5, ww / 2, Math.PI, TAU); x.lineTo(wx + ww, wy + wh); x.closePath();
      } else rr(x, wx, wy, ww, wh, 2);
    };
    pathFn(); paint(x, '#ffffff', L * 0.7);
    var inner = function () { rr(x, wx + ww * 0.12, wy + wh * 0.08, ww * 0.76, wh * 0.84, 2); };
    inner(); x.fillStyle = '#34507a'; x.fill();
    line(x, [[wx + ww / 2, wy + wh * 0.08], [wx + ww / 2, wy + wh * 0.92]], Math.max(1.5, L * 0.5), '#ffffff');
    line(x, [[wx + ww * 0.12, wy + wh * 0.45], [wx + ww * 0.88, wy + wh * 0.45]], Math.max(1.5, L * 0.5), '#ffffff');
    x.globalAlpha = 0.35; poly(x, [[wx + ww * 0.15, wy + wh * 0.4], [wx + ww * 0.4, wy + wh * 0.1], [wx + ww * 0.48, wy + wh * 0.1], [wx + ww * 0.15, wy + wh * 0.5]]);
    x.fillStyle = '#fff'; x.fill(); x.globalAlpha = 1;
  }

  function genoise(x, x0, x1, y, h, L) {
    for (var row = 0; row < 2; row++) {
      var n = Math.floor((x1 - x0) / (h * 1.1));
      for (var i = 0; i < n; i++) {
        var cx = x0 + (i + 0.5 + row * 0.5) * (x1 - x0) / n;
        if (cx > x1) continue;
        x.beginPath(); x.arc(cx, y + row * h * 0.9, h * 0.55, 0, Math.PI); x.closePath();
        paint(x, row ? '#e9b08a' : '#d2663b', Math.max(1, L * 0.45));
      }
    }
  }

  function tiles(x, pathFn, x0, x1, y0, y1, L) {
    clipped(x, pathFn, function () {
      x.strokeStyle = '#a84a28'; x.lineWidth = Math.max(1.5, L * 0.6); x.lineCap = 'round';
      for (var px = x0; px < x1; px += 12) {
        x.beginPath(); x.moveTo(px, y1);
        for (var py = y1; py > y0; py -= 8) x.lineTo(px + ((py / 8) % 2 ? 2 : -2), py);
        x.stroke();
      }
      x.fillStyle = 'rgba(255,255,255,0.18)'; x.fillRect(0, y0, x.canvas.width, (y1 - y0) * 0.25);
    });
  }

  function mas() {
    var W = 512, H = 330, c = mk(W, H), x = c.getContext('2d');
    var L = lwOf(W, 5000);
    shadowEll(x, W / 2, H - 5, W * 0.5, 7, 0.2);
    // annex
    var annex = function () { x.beginPath(); x.rect(W * 0.02, H * 0.5, W * 0.34, H * 0.48); };
    shaded(x, annex, '#e3a95c', '#efc27a', '#c98f48', H * 0.5, H * 0.53, H * 0.93, H, L);
    var aroof = function () { poly(x, [[W * 0.0, H * 0.53], [W * 0.05, H * 0.38], [W * 0.36, H * 0.38], [W * 0.37, H * 0.53]]); };
    aroof(); paint(x, '#d2663b');
    tiles(x, aroof, 0, W * 0.38, H * 0.38, H * 0.53, L);
    aroof(); paint(x, null, L);
    x.beginPath(); x.moveTo(W * 0.07, H * 0.98); x.lineTo(W * 0.07, H * 0.72); x.arc(W * 0.16, H * 0.72, W * 0.09, Math.PI, TAU); x.lineTo(W * 0.25, H * 0.98); x.closePath();
    paint(x, '#8a5a3a', L);
    line(x, [[W * 0.16, H * 0.64], [W * 0.16, H * 0.98]], L * 0.6);
    // main block
    var walls = function () { x.beginPath(); x.rect(W * 0.3, H * 0.3, W * 0.66, H * 0.68); };
    shaded(x, walls, '#f0c27c', '#f8d9a0', '#d9a660', H * 0.3, H * 0.34, H * 0.94, H, L);
    // quoins
    for (var q = 0; q < 6; q++) {
      rr(x, W * 0.3, H * (0.34 + q * 0.1), W * (q % 2 ? 0.035 : 0.05), H * 0.05, 1); paint(x, '#fbe3b8', L * 0.5);
      rr(x, W * (q % 2 ? 0.925 : 0.91), H * (0.34 + q * 0.1), W * (q % 2 ? 0.035 : 0.05), H * 0.05, 1); paint(x, '#fbe3b8', L * 0.5);
    }
    // chimney
    rr(x, W * 0.8, H * 0.06, W * 0.05, H * 0.16, 2); paint(x, '#f0c27c', L);
    rr(x, W * 0.79, H * 0.05, W * 0.07, H * 0.03, 2); paint(x, '#d2663b', L * 0.8);
    var roof = function () { poly(x, [[W * 0.27, H * 0.32], [W * 0.33, H * 0.13], [W * 0.93, H * 0.13], [W * 0.99, H * 0.32]]); };
    roof(); paint(x, '#d8693d');
    tiles(x, roof, W * 0.27, W, H * 0.13, H * 0.32, L);
    roof(); paint(x, null, L);
    line(x, [[W * 0.33, H * 0.13], [W * 0.93, H * 0.13]], L * 1.6, '#b9542e');
    genoise(x, W * 0.3, W * 0.96, H * 0.33, H * 0.035, L);
    var sh = '#5d92cf';
    [0.38, 0.6, 0.82].forEach(function (wx) { window_(x, W * wx, H * 0.43, W * 0.065, H * 0.15, sh, L); });
    [0.38, 0.82].forEach(function (wx) { window_(x, W * wx, H * 0.68, W * 0.065, H * 0.17, sh, L); });
    x.beginPath(); x.moveTo(W * 0.58, H * 0.98); x.lineTo(W * 0.58, H * 0.74); x.arc(W * 0.635, H * 0.74, W * 0.055, Math.PI, TAU); x.lineTo(W * 0.69, H * 0.98); x.closePath();
    paint(x, '#4f84c4', L);
    line(x, [[W * 0.635, H * 0.7], [W * 0.635, H * 0.98]], L * 0.6);
    circle(x, W * 0.655, H * 0.86, 2.5); x.fillStyle = '#ffd23f'; x.fill();
    // wisteria + pots
    blob(x, [[W * 0.56, H * 0.62, 12], [W * 0.6, H * 0.6, 13], [W * 0.66, H * 0.6, 13], [W * 0.71, H * 0.63, 11]], '#a77bdc', '#cfb0f5', '#7f55b8', L * 0.8);
    [0.5, 0.74].forEach(function (px) {
      blob(x, [[W * px, H * 0.9, 10], [W * px + 10, H * 0.89, 9], [W * px - 9, H * 0.9, 8]], '#4fa24a', null, null, L * 0.7);
      for (var f = 0; f < 4; f++) { circle(x, W * px - 8 + f * 6, H * 0.87 + (f % 2) * 5, 3.5); paint(x, '#e8413c', 1.2); }
      poly(x, [[W * px - 12, H * 0.92], [W * px + 12, H * 0.92], [W * px + 9, H * 0.985], [W * px - 9, H * 0.985]]); paint(x, '#c8643a', L * 0.7);
    });
    return c;
  }

  function villageHouse(wall, shutter) {
    var W = 320, H = 460, c = mk(W, H), x = c.getContext('2d');
    var L = lwOf(W, 3600);
    shadowEll(x, W / 2, H - 5, W * 0.48, 6, 0.2);
    rr(x, W * 0.7, H * 0.03, W * 0.07, H * 0.12, 2); paint(x, wall, L);
    var walls = function () { x.beginPath(); x.rect(W * 0.06, H * 0.15, W * 0.88, H * 0.835); };
    shaded(x, walls, wall, light(wall, 0.35), dark(wall, 0.12), H * 0.15, H * 0.18, H * 0.93, H, L);
    var roof = function () { poly(x, [[W * 0.01, H * 0.17], [W * 0.07, H * 0.06], [W * 0.93, H * 0.06], [W * 0.99, H * 0.17]]); };
    roof(); paint(x, '#d8693d');
    tiles(x, roof, 0, W, H * 0.06, H * 0.17, L);
    roof(); paint(x, null, L);
    genoise(x, W * 0.06, W * 0.94, H * 0.175, H * 0.022, L);
    [0.22, 0.66].forEach(function (wx) {
      window_(x, W * wx, H * 0.23, W * 0.13, H * 0.12, shutter, L);
      window_(x, W * wx, H * 0.45, W * 0.13, H * 0.14, shutter, L);
    });
    // wrought-iron balcony
    rr(x, W * 0.12, H * 0.585, W * 0.76, H * 0.02, 2); paint(x, '#efe6d2', L * 0.6);
    x.strokeStyle = OL; x.lineWidth = Math.max(1.5, L * 0.6);
    x.beginPath(); x.moveTo(W * 0.14, H * 0.535); x.lineTo(W * 0.86, H * 0.535); x.stroke();
    for (var b = 0; b <= 18; b++) { var bx = W * (0.14 + b * 0.04); x.beginPath(); x.moveTo(bx, H * 0.535); x.lineTo(bx, H * 0.585); x.stroke(); }
    [0.2, 0.5, 0.8].forEach(function (px) {
      blob(x, [[W * px - 8, H * 0.52, 8], [W * px + 8, H * 0.52, 8], [W * px, H * 0.51, 9]], '#4fa24a', null, null, L * 0.6);
      for (var f = 0; f < 3; f++) { circle(x, W * px - 7 + f * 7, H * 0.505 + (f % 2) * 6, 3.5); paint(x, '#e8413c', 1.2); }
    });
    // ground floor: arched door + shop window
    x.beginPath(); x.moveTo(W * 0.56, H * 0.985); x.lineTo(W * 0.56, H * 0.77); x.arc(W * 0.68, H * 0.77, W * 0.12, Math.PI, TAU); x.lineTo(W * 0.8, H * 0.985); x.closePath();
    paint(x, shutter, L);
    line(x, [[W * 0.68, H * 0.69], [W * 0.68, H * 0.985]], L * 0.6);
    window_(x, W * 0.2, H * 0.73, W * 0.14, H * 0.15, shutter, L, true);
    rr(x, W * 0.06, H * 0.95, W * 0.88, H * 0.035, 1); paint(x, '#cfc4ad', L * 0.6);
    return c;
  }

  function church() {
    var W = 360, H = 560, c = mk(W, H), x = c.getContext('2d');
    var L = lwOf(W, 3600);
    var stone = '#ecdcb6', stoneHi = '#f8edd2', stoneSh = '#d2bf93';
    shadowEll(x, W / 2, H - 5, W * 0.48, 6, 0.2);
    // nave facade
    var nave = function () { poly(x, [[W * 0.04, H * 0.985], [W * 0.04, H * 0.45], [W * 0.33, H * 0.3], [W * 0.62, H * 0.45], [W * 0.62, H * 0.985]]); };
    shaded(x, nave, stone, stoneHi, stoneSh, H * 0.3, H * 0.34, H * 0.95, H, L);
    line(x, [[W * 0.02, H * 0.46], [W * 0.33, H * 0.29], [W * 0.64, H * 0.46]], L * 1.8, '#d8693d');
    line(x, [[W * 0.02, H * 0.46], [W * 0.33, H * 0.29], [W * 0.64, H * 0.46]], L * 0.7, '#f08a5a');
    circle(x, W * 0.33, H * 0.53, W * 0.075); paint(x, '#34507a', L);
    for (var s = 0; s < 8; s++) {
      var a = s / 8 * TAU;
      line(x, [[W * 0.33, H * 0.53], [W * 0.33 + Math.cos(a) * W * 0.07, H * 0.53 + Math.sin(a) * W * 0.07]], Math.max(1.5, L * 0.5), '#f8edd2');
    }
    circle(x, W * 0.33, H * 0.53, W * 0.025); paint(x, '#ff9a3a', L * 0.6);
    x.beginPath(); x.moveTo(W * 0.24, H * 0.985); x.lineTo(W * 0.24, H * 0.78); x.arc(W * 0.33, H * 0.78, W * 0.09, Math.PI, TAU); x.lineTo(W * 0.42, H * 0.985); x.closePath();
    paint(x, '#8a5a3a', L);
    line(x, [[W * 0.33, H * 0.69], [W * 0.33, H * 0.985]], L * 0.6);
    x.beginPath(); x.arc(W * 0.33, H * 0.78, W * 0.11, Math.PI, TAU); paint(x, null, L * 0.8, '#c8b083');
    // tower
    var tower = function () { x.beginPath(); x.rect(W * 0.6, H * 0.17, W * 0.32, H * 0.815); };
    shaded(x, tower, '#e3d0a5', '#f3e6c6', '#c9b386', H * 0.17, H * 0.19, H * 0.95, H, L);
    clipped(x, tower, function () { x.fillStyle = 'rgba(0,0,0,0.08)'; x.fillRect(W * 0.84, 0, W, H); });
    rr(x, W * 0.58, H * 0.165, W * 0.36, H * 0.02, 2); paint(x, stoneHi, L * 0.8);
    rr(x, W * 0.58, H * 0.35, W * 0.36, H * 0.015, 2); paint(x, stoneHi, L * 0.7);
    // belfry openings
    x.beginPath(); x.moveTo(W * 0.66, H * 0.33); x.lineTo(W * 0.66, H * 0.24); x.arc(W * 0.76, H * 0.24, W * 0.1, Math.PI, TAU); x.lineTo(W * 0.86, H * 0.33); x.closePath();
    paint(x, '#2a2a3a', L);
    x.beginPath(); x.moveTo(W * 0.715, H * 0.25); x.quadraticCurveTo(W * 0.76, H * 0.17, W * 0.805, H * 0.25); x.lineTo(W * 0.82, H * 0.29); x.lineTo(W * 0.7, H * 0.29); x.closePath();
    paint(x, '#f2b632', L * 0.7);
    // clock
    circle(x, W * 0.76, H * 0.45, W * 0.07); paint(x, '#ffffff', L);
    line(x, [[W * 0.76, H * 0.45], [W * 0.76, H * 0.415]], L * 0.7);
    line(x, [[W * 0.76, H * 0.45], [W * 0.79, H * 0.46]], L * 0.7);
    rr(x, W * 0.72, H * 0.62, W * 0.08, H * 0.1, W * 0.04); paint(x, '#34507a', L * 0.8);
    // wrought-iron campanile
    var cx = W * 0.76;
    x.beginPath(); x.moveTo(cx - W * 0.05, H * 0.15); x.quadraticCurveTo(cx, H * 0.06, cx + W * 0.05, H * 0.15); x.lineTo(cx + W * 0.06, H * 0.155); x.lineTo(cx - W * 0.06, H * 0.155); x.closePath();
    paint(x, '#f2b632', L * 0.7);
    x.strokeStyle = OL; x.lineWidth = Math.max(2, L * 0.8); x.lineCap = 'round';
    [-1, -0.5, 0, 0.5, 1].forEach(function (k) {
      x.beginPath(); x.moveTo(cx + k * W * 0.12, H * 0.165);
      x.bezierCurveTo(cx + k * W * 0.14, H * 0.09, cx + k * W * 0.04, H * 0.06, cx, H * 0.035); x.stroke();
    });
    x.beginPath(); x.ellipse(cx, H * 0.165, W * 0.12, H * 0.008, 0, 0, TAU); x.stroke();
    line(x, [[cx, H * 0.035], [cx, H * 0.0 + 4]], L * 0.8);
    line(x, [[cx - 7, H * 0.016], [cx + 7, H * 0.016]], L * 0.8);
    return c;
  }

  /* ----------------------------------------------------------- road furniture */

  function pole(x, cx, y0, y1, w, L) { rr(x, cx - w / 2, y0, w, y1 - y0, w * 0.3); paint(x, '#9aa1ab', L); line(x, [[cx - w * 0.15, y0 + 4], [cx - w * 0.15, y1 - 4]], Math.max(1, w * 0.2), '#c9d0d8'); }

  function speedSign(n) {
    var W = 128, H = 300, c = mk(W, H), x = c.getContext('2d');
    var L = lwOf(W, 600);
    shadowEll(x, W / 2, H - 4, 14, 3.5, 0.25);
    pole(x, W / 2, H * 0.3, H - 2, 11, L);
    var r = W / 2 - L;
    circle(x, W / 2, r + L, r); paint(x, '#e0262b', L);
    circle(x, W / 2, r + L, r * 0.7); paint(x, '#ffffff', L * 0.6);
    label(x, String(n), W / 2, r + L + 2, r * (n >= 100 ? 0.62 : 0.8), '#15151f', 0, null, r * 1.2);
    return c;
  }

  function radarSign() {
    var W = 160, H = 330, c = mk(W, H), x = c.getContext('2d');
    var L = lwOf(W, 800);
    shadowEll(x, W / 2, H - 4, 16, 4, 0.25);
    pole(x, W / 2, H * 0.4, H - 2, 12, L);
    rr(x, L, L, W - 2 * L, W - 2 * L, 10); paint(x, '#2457c5', L);
    rr(x, W * 0.14, W * 0.14, W * 0.72, W * 0.6, 6); paint(x, '#ffffff', L * 0.6);
    // camera pictogram
    rr(x, W * 0.24, W * 0.3, W * 0.42, W * 0.27, 6); paint(x, '#15151f');
    rr(x, W * 0.33, W * 0.24, W * 0.14, W * 0.07, 3); paint(x, '#15151f');
    circle(x, W * 0.45, W * 0.435, W * 0.085); paint(x, '#ffffff');
    circle(x, W * 0.45, W * 0.435, W * 0.05); paint(x, '#15151f');
    [[0.72, 0.3, 0.8, 0.24], [0.72, 0.435, 0.82, 0.435], [0.72, 0.57, 0.8, 0.63]].forEach(function (l) {
      line(x, [[W * l[0], W * l[1]], [W * l[2], W * l[3]]], L * 0.8, '#ffb000');
    });
    label(x, 'RADAR', W / 2, W * 0.83, W * 0.13, '#ffffff', 0, null, W * 0.7);
    return c;
  }

  function radarBox() {
    var W = 130, H = 330, c = mk(W, H), x = c.getContext('2d');
    var L = lwOf(W, 650);
    shadowEll(x, W / 2, H - 4, 18, 4, 0.25);
    pole(x, W / 2, H * 0.4, H - 2, 18, L);
    rr(x, W * 0.36, H * 0.95, W * 0.28, H * 0.04, 3); paint(x, '#7a808a', L * 0.6);
    var box = function () { rr(x, L, H * 0.04, W - 2 * L, H * 0.38, 10); };
    shaded(x, box, '#a7aeb8', '#cfd5dc', '#858c96', H * 0.04, H * 0.07, H * 0.36, H * 0.43, L);
    rr(x, W * 0.08, H * 0.02, W * 0.84, H * 0.04, 6); paint(x, '#7a808a', L * 0.8);
    circle(x, W * 0.5, H * 0.18, W * 0.22); paint(x, '#2a2a36', L);
    circle(x, W * 0.5, H * 0.18, W * 0.14); paint(x, '#3b4f80');
    circle(x, W * 0.44, H * 0.165, W * 0.05); x.fillStyle = '#9ec0ff'; x.fill();
    rr(x, W * 0.25, H * 0.29, W * 0.5, H * 0.06, 4); paint(x, '#fff3a0', L * 0.7);
    circle(x, W * 0.8, H * 0.08, W * 0.04); x.fillStyle = '#ff3030'; x.fill();
    return c;
  }

  function chevronSign(dir) {
    var W = 180, H = 230, c = mk(W, H), x = c.getContext('2d');
    var L = lwOf(W, 700);
    shadowEll(x, W / 2, H - 4, W * 0.4, 4, 0.25);
    pole(x, W * 0.25, H * 0.5, H - 2, 10, L);
    pole(x, W * 0.75, H * 0.5, H - 2, 10, L);
    rr(x, L, L, W - 2 * L, H * 0.52, 8); paint(x, '#e0262b', L);
    for (var i = 0; i < 3; i++) {
      var cx = W * (0.25 + i * 0.25), cy = L + H * 0.26, s = W * 0.09;
      var pts = dir < 0
        ? [[cx + s * 0.5, cy - s * 1.4], [cx - s * 0.9, cy], [cx + s * 0.5, cy + s * 1.4], [cx + s * 1.1, cy + s * 1.4], [cx - s * 0.3, cy], [cx + s * 1.1, cy - s * 1.4]]
        : [[cx - s * 0.5, cy - s * 1.4], [cx + s * 0.9, cy], [cx - s * 0.5, cy + s * 1.4], [cx - s * 1.1, cy + s * 1.4], [cx + s * 0.3, cy], [cx - s * 1.1, cy - s * 1.4]];
      poly(x, pts); paint(x, '#ffffff');
    }
    return c;
  }

  function triangleSign(kind) {
    var W = 170, H = 330, c = mk(W, H), x = c.getContext('2d');
    var L = lwOf(W, 800);
    shadowEll(x, W / 2, H - 4, 16, 4, 0.25);
    pole(x, W / 2, H * 0.4, H - 2, 12, L);
    var top = L + 2, bot = W * 0.88, l = L, r = W - L;
    var tri = function (inset) {
      x.beginPath();
      x.moveTo(W / 2, top + inset * 1.8);
      x.lineTo(r - inset * 1.6, bot - inset);
      x.lineTo(l + inset * 1.6, bot - inset);
      x.closePath();
    };
    tri(0); paint(x, '#e0262b', L);
    tri(W * 0.1); paint(x, kind === 'works' ? '#ffd23f' : '#ffffff', L * 0.5);
    var cx = W / 2, cy = W * 0.6;
    if (kind === 'roundabout') {
      for (var i = 0; i < 3; i++) {
        var a0 = i * TAU / 3 - Math.PI / 2;
        x.beginPath(); x.arc(cx, cy, W * 0.12, a0 + 0.35, a0 + TAU / 3 - 0.15);
        x.lineWidth = W * 0.045; x.strokeStyle = '#15151f'; x.lineCap = 'butt'; x.stroke();
        var a1 = a0 + TAU / 3 - 0.15, ax = cx + Math.cos(a1) * W * 0.12, ay = cy + Math.sin(a1) * W * 0.12;
        var tx = -Math.sin(a1), ty = Math.cos(a1); // tangent (clockwise)
        var nx = Math.cos(a1), ny = Math.sin(a1);
        poly(x, [[ax + tx * W * 0.06, ay + ty * W * 0.06], [ax + nx * W * 0.05, ay + ny * W * 0.05], [ax - nx * W * 0.05, ay - ny * W * 0.05]]);
        x.fillStyle = '#15151f'; x.fill();
      }
    } else {
      // roadworker shoveling
      var k = '#15151f';
      circle(x, cx - W * 0.02, cy - W * 0.13, W * 0.04); x.fillStyle = k; x.fill();
      line(x, [[cx - W * 0.03, cy - W * 0.08], [cx - W * 0.07, cy + W * 0.04]], W * 0.05, k);
      line(x, [[cx - W * 0.07, cy + W * 0.04], [cx - W * 0.12, cy + W * 0.13]], W * 0.035, k);
      line(x, [[cx - W * 0.07, cy + W * 0.04], [cx - W * 0.01, cy + W * 0.13]], W * 0.035, k);
      line(x, [[cx - W * 0.04, cy - W * 0.05], [cx + W * 0.06, cy - W * 0.01]], W * 0.03, k);
      line(x, [[cx - W * 0.06, cy - W * 0.1], [cx + W * 0.12, cy + W * 0.08]], W * 0.02, k);
      poly(x, [[cx + W * 0.1, cy + W * 0.06], [cx + W * 0.16, cy + W * 0.1], [cx + W * 0.12, cy + W * 0.13]]); x.fillStyle = k; x.fill();
      x.beginPath(); x.moveTo(cx + W * 0.02, cy + W * 0.14); x.quadraticCurveTo(cx + W * 0.13, cy + W * 0.03, cx + W * 0.22, cy + W * 0.14); x.closePath(); x.fill();
    }
    return c;
  }

  function cone() {
    var W = 100, H = 150, c = mk(W, H), x = c.getContext('2d');
    var L = lwOf(W, 400);
    shadowEll(x, W / 2, H - 5, W * 0.46, 5, 0.25);
    rr(x, W * 0.08, H * 0.86, W * 0.84, H * 0.11, 4); paint(x, '#d95a10', L);
    var body = function () { poly(x, [[W * 0.42, H * 0.04], [W * 0.58, H * 0.04], [W * 0.8, H * 0.88], [W * 0.2, H * 0.88]]); };
    body(); x.fillStyle = '#ff7a1a'; x.fill();
    clipped(x, body, function () {
      x.fillStyle = '#ffffff'; x.fillRect(0, H * 0.28, W, H * 0.12); x.fillRect(0, H * 0.56, W, H * 0.12);
      x.fillStyle = 'rgba(255,255,255,0.35)'; x.fillRect(W * 0.38, 0, W * 0.07, H);
      x.fillStyle = 'rgba(0,0,0,0.12)'; x.fillRect(W * 0.6, 0, W, H);
    });
    body(); paint(x, null, L);
    return c;
  }

  function barrier() {
    var W = 300, H = 170, c = mk(W, H), x = c.getContext('2d');
    var L = lwOf(W, 1500);
    shadowEll(x, W / 2, H - 4, W * 0.46, 5, 0.25);
    [[0.12, 0.05], [0.12, 0.2], [0.88, 0.8], [0.88, 0.95]].forEach(function (p) {
      line(x, [[W * p[0], H * 0.4], [W * p[1], H - 4]], L * 1.6);
      line(x, [[W * p[0], H * 0.4], [W * p[1], H - 4]], L * 0.8, '#c9d0d8');
    });
    var board = function () { rr(x, W * 0.03, H * 0.28, W * 0.94, H * 0.26, 6); };
    board(); x.fillStyle = '#ffffff'; x.fill();
    clipped(x, board, function () {
      x.fillStyle = '#e0262b';
      for (var i = -2; i < 12; i++) poly(x, [[W * (i * 0.1), H * 0.54], [W * (i * 0.1 + 0.05), H * 0.54], [W * (i * 0.1 + 0.11), H * 0.28], [W * (i * 0.1 + 0.06), H * 0.28]]), x.fill();
    });
    board(); paint(x, null, L);
    // flashing lamp
    rr(x, W * 0.13, H * 0.2, W * 0.03, H * 0.1, 2); paint(x, '#2a2a36', L * 0.6);
    x.save(); x.shadowColor = '#ffae00'; x.shadowBlur = 12;
    circle(x, W * 0.145, H * 0.14, W * 0.045); x.fillStyle = '#ffb020'; x.fill(); x.restore();
    circle(x, W * 0.145, H * 0.14, W * 0.045); paint(x, null, L * 0.7);
    circle(x, W * 0.135, H * 0.125, W * 0.015); x.fillStyle = '#fff3c0'; x.fill();
    return c;
  }

  function arrow(x, x0, y0, len, h, color, dir) {
    dir = dir || 1;
    var t = h * 0.36;
    poly(x, [
      [x0, y0 - t / 2], [x0 + dir * (len - h * 0.6), y0 - t / 2], [x0 + dir * (len - h * 0.6), y0 - h / 2],
      [x0 + dir * len, y0], [x0 + dir * (len - h * 0.6), y0 + h / 2], [x0 + dir * (len - h * 0.6), y0 + t / 2], [x0, y0 + t / 2]
    ]);
    x.fillStyle = color; x.fill();
  }

  function signA7() {
    var W = 512, H = 440, c = mk(W, H), x = c.getContext('2d');
    var L = lwOf(W, 2800);
    shadowEll(x, W / 2, H - 4, W * 0.42, 5, 0.25);
    pole(x, W * 0.17, H * 0.5, H - 2, 18, L);
    pole(x, W * 0.83, H * 0.5, H - 2, 18, L);
    rr(x, L, L, W - 2 * L, H * 0.55, 14); paint(x, '#1f4fbf', L);
    rr(x, W * 0.03, H * 0.035, W * 0.94, H * 0.5, 10); paint(x, null, Math.max(2, L * 0.7), '#ffffff');
    rr(x, W * 0.07, H * 0.08, W * 0.2, H * 0.14, 8); paint(x, '#e0262b', L * 0.7, '#ffffff');
    label(x, 'A7', W * 0.17, H * 0.155, H * 0.1, '#ffffff', 0, null, W * 0.16);
    x.save(); x.font = font(H * 0.085); x.textAlign = 'left'; x.textBaseline = 'middle'; x.fillStyle = '#ffffff';
    x.font = '700 ' + Math.round(H * 0.085) + 'px "Arial", "Roboto", system-ui, sans-serif';
    x.fillText('Salon-de-Pce', W * 0.31, H * 0.155, W * 0.62);
    x.fillText('Lyon', W * 0.31, H * 0.29, W * 0.62);
    x.restore();
    arrow(x, W * 0.62, H * 0.42, W * 0.3, H * 0.1, '#ffffff', 1);
    x.save(); x.font = '700 ' + Math.round(H * 0.07) + 'px "Arial", "Roboto", system-ui, sans-serif'; x.fillStyle = '#ffffff'; x.textBaseline = 'middle';
    x.fillText('Aix · Marseille', W * 0.07, H * 0.42, W * 0.52);
    x.restore();
    return c;
  }

  function signDir() {
    var W = 420, H = 290, c = mk(W, H), x = c.getContext('2d');
    var L = lwOf(W, 2200);
    shadowEll(x, W / 2, H - 4, W * 0.4, 5, 0.25);
    pole(x, W * 0.2, H * 0.5, H - 2, 14, L);
    pole(x, W * 0.8, H * 0.5, H - 2, 14, L);
    rr(x, L, L, W - 2 * L, H * 0.56, 12); paint(x, '#1e8a4a', L);
    rr(x, W * 0.03, H * 0.045, W * 0.94, H * 0.5, 9); paint(x, null, Math.max(2, L * 0.7), '#ffffff');
    x.save(); x.font = '700 ' + Math.round(H * 0.12) + 'px "Arial", "Roboto", system-ui, sans-serif'; x.fillStyle = '#ffffff'; x.textBaseline = 'middle';
    x.fillText('Lançon-Pce', W * 0.07, H * 0.17, W * 0.6);
    x.fillText('Salon-de-Pce', W * 0.07, H * 0.4, W * 0.6);
    x.restore();
    arrow(x, W * 0.7, H * 0.17, W * 0.24, H * 0.13, '#ffffff', 1);
    // up arrow for Salon
    poly(x, [[W * 0.82, H * 0.32], [W * 0.88, H * 0.4], [W * 0.84, H * 0.4], [W * 0.84, H * 0.48], [W * 0.8, H * 0.48], [W * 0.8, H * 0.4], [W * 0.76, H * 0.4]]);
    x.fillStyle = '#fff'; x.fill();
    return c;
  }

  function signAutoroute() {
    var W = 170, H = 330, c = mk(W, H), x = c.getContext('2d');
    var L = lwOf(W, 900);
    shadowEll(x, W / 2, H - 4, 16, 4, 0.25);
    pole(x, W / 2, H * 0.4, H - 2, 12, L);
    rr(x, L, L, W - 2 * L, W - 2 * L, 12); paint(x, '#1f4fbf', L);
    rr(x, W * 0.1, W * 0.1, W * 0.8, W * 0.8, 8); paint(x, null, Math.max(2, L * 0.5), '#ffffff');
    // bridge + road pictogram
    x.fillStyle = '#ffffff';
    x.beginPath(); x.rect(W * 0.18, W * 0.3, W * 0.64, W * 0.07); x.fill();
    x.beginPath(); x.moveTo(W * 0.22, W * 0.37); x.lineTo(W * 0.3, W * 0.37); x.lineTo(W * 0.3, W * 0.42); x.lineTo(W * 0.22, W * 0.42); x.fill();
    x.beginPath(); x.moveTo(W * 0.7, W * 0.37); x.lineTo(W * 0.78, W * 0.37); x.lineTo(W * 0.78, W * 0.42); x.lineTo(W * 0.7, W * 0.42); x.fill();
    poly(x, [[W * 0.44, W * 0.42], [W * 0.56, W * 0.42], [W * 0.78, W * 0.82], [W * 0.22, W * 0.82]]); x.fill();
    x.fillStyle = '#1f4fbf';
    poly(x, [[W * 0.49, W * 0.44], [W * 0.51, W * 0.44], [W * 0.52, W * 0.8], [W * 0.48, W * 0.8]]); x.fill();
    return c;
  }

  function billboard(kind) {
    var W = 512, H = 340, c = mk(W, H), x = c.getContext('2d');
    var L = lwOf(W, 3200);
    shadowEll(x, W / 2, H - 4, W * 0.4, 5, 0.22);
    [0.2, 0.8].forEach(function (px) {
      rr(x, W * px - 9, H * 0.6, 18, H * 0.39, 4); paint(x, '#8a6a4a', L);
    });
    line(x, [[W * 0.2, H * 0.72], [W * 0.8, H * 0.92]], L * 0.9, '#6e5236');
    line(x, [[W * 0.2, H * 0.92], [W * 0.8, H * 0.72]], L * 0.9, '#6e5236');
    var bgc = { calisson: '#fff1b8', savon: '#bfe6ff', melon: '#d9f5c4', olive: '#f6eccb' }[kind];
    var board = function () { rr(x, L, L, W - 2 * L, H * 0.64, 14); };
    board(); paint(x, '#ffffff', L);
    rr(x, W * 0.025, H * 0.035, W * 0.95, H * 0.585, 10); paint(x, bgc, L * 0.6);
    var t1, t2, col;
    var ix = W * 0.2, iy = H * 0.33;
    if (kind === 'calisson') {
      t1 = 'CALISSONS'; t2 = "D'AIX"; col = '#e0563a';
      [[-28, 30, -0.25], [30, 28, 0.2], [2, -10, 0.05]].forEach(function (p) {
        x.save(); x.translate(ix + 6 + p[0], iy + p[1]); x.rotate(p[2]);
        var lens = function (dy) { x.beginPath(); x.moveTo(-50, dy); x.quadraticCurveTo(0, dy - 30, 50, dy); x.quadraticCurveTo(0, dy + 30, -50, dy); x.closePath(); };
        lens(10); paint(x, '#e2b46a', L * 0.8);
        x.beginPath(); x.rect(-50, 0, 100, 10); x.fillStyle = '#e2b46a'; x.fill();
        line(x, [[-50, 0], [-50, 10]], L * 0.8); line(x, [[50, 0], [50, 10]], L * 0.8);
        lens(0); paint(x, '#fffdf6', L * 0.8);
        ellipse(x, -12, -6, 14, 4, -0.2); x.fillStyle = '#ffffff'; x.fill();
        x.restore();
      });
      for (var i = 0; i < 3; i++) { ellipse(x, ix - 70 + i * 14, iy - 70 + (i % 2) * 6, 5, 12, -0.3); paint(x, '#9a6be0', L * 0.4); }
    } else if (kind === 'savon') {
      t1 = 'SAVON DE'; t2 = 'MARSEILLE'; col = '#2a7fd0';
      // cube of soap
      poly(x, [[ix - 60, iy - 20], [ix, iy - 50], [ix + 60, iy - 20], [ix, iy + 10]]); paint(x, '#a9df8e', L * 0.8);
      poly(x, [[ix - 60, iy - 20], [ix, iy + 10], [ix, iy + 72], [ix - 60, iy + 42]]); paint(x, '#7cc36b', L * 0.8);
      poly(x, [[ix + 60, iy - 20], [ix, iy + 10], [ix, iy + 72], [ix + 60, iy + 42]]); paint(x, '#5fa850', L * 0.8);
      x.save(); x.translate(ix - 30, iy + 28); x.transform(1, 0.5, 0, 1, 0, 0);
      label(x, 'MARSEILLE', 0, -10, 11, '#3d7a32', 0, null, 52); x.restore();
      [[-80, -60, 14], [-60, -88, 9], [70, -70, 12], [85, -40, 7], [-90, 10, 8]].forEach(function (b) {
        circle(x, ix + b[0], iy + b[1], b[2]); paint(x, 'rgba(255,255,255,0.7)', L * 0.5, '#5a8ac0');
        circle(x, ix + b[0] - b[2] * 0.35, iy + b[1] - b[2] * 0.35, b[2] * 0.25); x.fillStyle = '#fff'; x.fill();
      });
    } else if (kind === 'melon') {
      t1 = 'MELONS DE'; t2 = 'CAVAILLON'; col = '#ff7a1a';
      x.beginPath(); x.arc(ix, iy - 10, 80, 0, Math.PI); x.closePath(); paint(x, '#4f9a46', L);
      x.beginPath(); x.arc(ix, iy - 10, 68, 0, Math.PI); x.closePath(); paint(x, '#e8f5c0');
      x.beginPath(); x.arc(ix, iy - 10, 62, 0, Math.PI); x.closePath(); paint(x, '#ff9a3c');
      x.beginPath(); x.arc(ix, iy - 10, 28, 0, Math.PI); x.closePath(); paint(x, '#ffbf6a');
      for (var s = 0; s < 7; s++) { var a = 0.3 + s * 0.42; ellipse(x, ix + Math.cos(a) * 22, iy - 10 + Math.sin(a) * 22, 3, 5.5, a); x.fillStyle = '#7a4a22'; x.fill(); }
      circle(x, ix - 26, iy + 20, 5); x.fillStyle = OL; x.fill();
      circle(x, ix + 26, iy + 20, 5); x.fill();
      x.beginPath(); x.arc(ix, iy + 26, 16, 0.15 * Math.PI, 0.85 * Math.PI); x.lineWidth = 4; x.strokeStyle = OL; x.stroke();
      line(x, [[ix - 80, iy - 10], [ix + 80, iy - 10]], L);
    } else {
      t1 = 'HUILE'; t2 = "D'OLIVE"; col = '#6f8f2a';
      rr(x, ix - 30, iy - 40, 60, 110, 16); paint(x, '#c6d84a', L);
      rr(x, ix - 12, iy - 76, 24, 40, 5); paint(x, '#c6d84a', L);
      rr(x, ix - 14, iy - 86, 28, 14, 4); paint(x, '#2a2a36', L * 0.8);
      rr(x, ix - 24, iy - 5, 48, 44, 6); paint(x, '#fff8e0', L * 0.6);
      ellipse(x, ix, iy + 17, 12, 8); paint(x, '#4f7a2a', L * 0.4);
      x.globalAlpha = 0.4; rr(x, ix - 22, iy - 34, 10, 26, 5); x.fillStyle = '#fff'; x.fill(); x.globalAlpha = 1;
      line(x, [[ix + 40, iy - 60], [ix + 90, iy + 30]], L * 0.9, '#6b5a3e');
      [[52, -40], [64, -14], [76, 10]].forEach(function (p, i2) {
        ellipse(x, ix + p[0] + 14, iy + p[1] - 6, 14, 6, -0.6); paint(x, '#8aa860', L * 0.5);
        ellipse(x, ix + p[0] - 6, iy + p[1] + 8, 8, 10); paint(x, i2 % 2 ? '#3b2a4a' : '#6a8a2a', L * 0.5);
      });
    }
    label(x, t1, W * 0.66, H * 0.22, H * 0.13, col, L * 1.2, OL, W * 0.56);
    label(x, t2, W * 0.66, H * 0.43, H * 0.15, '#ffffff', L * 1.3, OL, W * 0.56);
    return c;
  }

  function archBase(kind) {
    var W = 1024, H = 560, c = mk(W, H), x = c.getContext('2d');
    var L = lwOf(W, 4800);
    var tw = W * 0.072;
    shadowEll(x, tw * 0.55, H - 7, tw * 0.7, 8, 0.25);
    shadowEll(x, W - tw * 0.55, H - 7, tw * 0.7, 8, 0.25);
    var parts = [
      function () { rr(x, L, H * 0.08, tw, H * 0.9, tw * 0.5); },
      function () { rr(x, W - L - tw, H * 0.08, tw, H * 0.9, tw * 0.5); },
      function () { rr(x, L, H * 0.06, W - 2 * L, H * 0.17, H * 0.085); }
    ];
    // outline pass then fill pass so the tubes merge
    parts.forEach(function (p) { p(); paint(x, null, L * 2); });
    var fill = kind === 'start' ? '#e8382f' : '#ffffff';
    parts.forEach(function (p) { p(); x.fillStyle = fill; x.fill(); });
    var clipAll = function (draw) {
      parts.forEach(function (p) { clipped(x, p, draw); });
    };
    if (kind === 'start') {
      clipAll(function () {
        x.fillStyle = '#ffffff';
        for (var i = -20; i < 40; i++) poly(x, [[i * 60, H], [i * 60 + 26, H], [i * 60 + 26 + H, 0], [i * 60 + H, 0]]), x.fill();
      });
    } else {
      clipAll(function () {
        var s = 26;
        x.fillStyle = '#15151f';
        for (var gy = 0; gy < H / s; gy++) for (var gx = 0; gx < W / s; gx++) if ((gx + gy) % 2) x.fillRect(gx * s, gy * s, s, s);
      });
    }
    // glossy highlights
    x.globalAlpha = 0.45;
    line(x, [[L + tw * 0.3, H * 0.3], [L + tw * 0.3, H * 0.9]], tw * 0.16, '#ffffff');
    line(x, [[W - L - tw * 0.7, H * 0.3], [W - L - tw * 0.7, H * 0.9]], tw * 0.16, '#ffffff');
    line(x, [[W * 0.1, H * 0.1], [W * 0.9, H * 0.1]], H * 0.025, '#ffffff');
    x.globalAlpha = 1;
    // sand bags
    [L - 4, W - L - tw - 4].forEach(function (bx) { rr(x, bx, H * 0.92, tw + 8, H * 0.07, 8); paint(x, '#5a5a6a', L); });
    // bunting
    var cols = ['#e8382f', '#ffffff', '#2457c5', '#ffd23f'];
    x.beginPath(); x.moveTo(L + tw, H * 0.24);
    x.quadraticCurveTo(W / 2, H * 0.42, W - L - tw, H * 0.24);
    x.lineWidth = 3; x.strokeStyle = OL; x.stroke();
    for (var b = 0; b < 16; b++) {
      var t = (b + 0.5) / 16, bx2 = (L + tw) + (W - 2 * L - 2 * tw) * t;
      var by = H * 0.24 + 4 * t * (1 - t) * H * 0.09;
      poly(x, [[bx2 - 14, by], [bx2 + 14, by], [bx2, by + 34]]); paint(x, cols[b % 4], 3);
    }
    // banner
    var bw = W * 0.44, bx0 = W / 2 - bw / 2;
    rr(x, bx0, H * 0.025, bw, H * 0.24, 18); paint(x, kind === 'start' ? '#2457c5' : '#e8382f', L * 1.2);
    rr(x, bx0 + 10, H * 0.025 + 10, bw - 20, H * 0.24 - 20, 12); paint(x, null, 3, '#ffffff');
    label(x, kind === 'start' ? 'DÉPART' : 'ARRIVÉE', W / 2, H * 0.15, H * 0.15, '#ffffff', L * 2, OL, bw * 0.86);
    if (kind === 'finish') {
      [W * 0.12, W * 0.88].forEach(function (fx0, i) {
        line(x, [[fx0, H * 0.07], [fx0, H * -0.1 + 30]], 6, OL);
        var dir = i ? -1 : 1;
        var flag = function () {
          x.beginPath(); x.moveTo(fx0, 4);
          x.quadraticCurveTo(fx0 + dir * 40, -6, fx0 + dir * 80, 10);
          x.lineTo(fx0 + dir * 80, 60);
          x.quadraticCurveTo(fx0 + dir * 40, 44, fx0, 56); x.closePath();
        };
        flag(); x.fillStyle = '#ffffff'; x.fill();
        clipped(x, flag, function () {
          x.fillStyle = '#15151f';
          for (var gy = 0; gy < 5; gy++) for (var gx = 0; gx < 8; gx++) if ((gx + gy) % 2) x.fillRect(Math.min(fx0, fx0 + dir * 80) + gx * 13, gy * 13, 13, 13);
        });
        flag(); paint(x, null, 4);
      });
    }
    return c;
  }

  function token() {
    var W = 128, H = 128, c = mk(W, H), x = c.getContext('2d');
    var L = 4;
    var cx = W / 2, cy = H - 6 - 54, r = 54;
    x.save(); x.shadowColor = '#fff2a0'; x.shadowBlur = 8;
    circle(x, cx, cy, r); x.fillStyle = '#f2a51a'; x.fill();
    x.restore();
    circle(x, cx, cy, r); paint(x, '#f2a51a', L);
    circle(x, cx, cy - 2, r * 0.86); x.fillStyle = '#ffc93a'; x.fill();
    circle(x, cx, cy, r * 0.74); paint(x, '#ffd84d', L * 0.6, '#d48a0a');
    chevrons(x, cx, cy, r * 0.95, r * 0.75, '#fff3b0', L * 0.7);
    x.save(); x.globalAlpha = 0.65;
    x.beginPath(); x.arc(cx, cy, r * 0.82, Math.PI * 1.1, Math.PI * 1.45); x.lineWidth = 6; x.strokeStyle = '#ffffff'; x.lineCap = 'round'; x.stroke();
    x.restore();
    star(x, cx + r * 0.62, cy - r * 0.62, 4, 13, 4, 0); x.fillStyle = '#ffffff'; x.fill();
    return c;
  }

  /* ------------------------------------------------------------ backgrounds */

  function periodic(px, W, terms) {
    var v = 0;
    for (var i = 0; i < terms.length; i++) v += terms[i][0] * Math.sin(TAU * terms[i][1] * px / W + terms[i][2]);
    return v;
  }

  function makeFar() {
    var W = 2048, H = 360, c = mk(W, H), x = c.getContext('2d');
    var terms = [[0.24, 1, 0.4], [0.16, 2, 1.9], [0.1, 3, 0.7], [0.06, 5, 2.6], [0.035, 9, 1.2], [0.018, 17, 0.3]];
    var ridge = function (px) {
      var v = periodic(px, W, terms);
      v += 0.32 * Math.pow(Math.max(0, Math.sin(TAU * 2 * px / W + 0.9)), 5);
      return Math.max(10, Math.min(H - 10, H * (0.56 - v * 0.6)));
    };
    var shape = function (off) {
      x.beginPath(); x.moveTo(-10, H + 10); x.lineTo(-10, ridge(0) + off);
      for (var px = 0; px <= W; px += 4) x.lineTo(px, ridge(px) + off);
      x.lineTo(W + 10, ridge(W) + off); x.lineTo(W + 10, H + 10); x.closePath();
    };
    shape(0); x.fillStyle = '#a7acdd'; x.fill();
    clipped(x, function () { shape(0); }, function () {
      // pale limestone crest
      x.fillStyle = '#d6d9f4';
      x.beginPath(); x.moveTo(-10, ridge(0));
      for (var px = 0; px <= W; px += 4) x.lineTo(px, ridge(px));
      for (var p2 = W; p2 >= 0; p2 -= 4) x.lineTo(p2, ridge(p2) + 16 + 8 * Math.sin(TAU * 33 * p2 / W));
      x.closePath(); x.fill();
      // striations
      var r = rng(21);
      x.strokeStyle = '#8b90c8'; x.lineCap = 'round';
      for (var i = 0; i < 70; i++) {
        var sx = 30 + r() * (W - 60), sy = ridge(sx) + 14 + r() * 10;
        x.lineWidth = 2 + r() * 2;
        x.beginPath(); x.moveTo(sx, sy); x.lineTo(sx + (r() - 0.5) * 16, sy + 20 + r() * 60); x.stroke();
      }
      x.fillStyle = '#9399cf'; x.fillRect(-10, H * 0.82, W + 20, H);
    });
    x.beginPath(); x.moveTo(0, ridge(0));
    for (var px = 0; px <= W; px += 4) x.lineTo(px, ridge(px));
    x.lineWidth = 4; x.strokeStyle = '#6b70a8'; x.lineJoin = 'round'; x.stroke();
    return c;
  }

  function makeMid() {
    var W = 2048, H = 300, c = mk(W, H), x = c.getContext('2d');
    var terms = [[0.14, 2, 0.2], [0.1, 3, 1.5], [0.07, 5, 2.2], [0.05, 7, 0.8], [0.022, 13, 1.1]];
    var ridge = function (px) { return Math.max(20, Math.min(H - 20, H * (0.52 - periodic(px, W, terms) * 0.95))); };
    var shape = function () {
      x.beginPath(); x.moveTo(-10, H + 10); x.lineTo(-10, ridge(0));
      for (var px = 0; px <= W; px += 4) x.lineTo(px, ridge(px));
      x.lineTo(W + 10, ridge(W)); x.lineTo(W + 10, H + 10); x.closePath();
    };
    shape(); x.fillStyle = '#8fbf57'; x.fill();
    var r = rng(33);
    clipped(x, shape, function () {
      // fields: lavender rows, vineyards, wheat
      var fields = [[120, '#9b78d6'], [520, '#c9b55a'], [860, '#6fa646'], [1240, '#9b78d6'], [1560, '#d9c06a'], [1880, '#6fa646']];
      fields.forEach(function (f) {
        var fx0 = f[0], fy = ridge(fx0) + 40;
        x.save(); x.translate(fx0, fy); x.transform(1, 0, -0.5, 1, 0, 0);
        x.fillStyle = f[1]; x.fillRect(-60, 0, 150, 70);
        x.strokeStyle = dark(f[1], 0.25); x.lineWidth = 3;
        for (var k = 0; k < 8; k++) { x.beginPath(); x.moveTo(-60, k * 9 + 4); x.lineTo(90, k * 9 + 4); x.stroke(); }
        x.lineWidth = 2.5; x.strokeStyle = '#4d7a33'; x.strokeRect(-60, 0, 150, 70);
        x.restore();
      });
      x.fillStyle = '#b4dc78';
      x.beginPath(); x.moveTo(-10, ridge(0));
      for (var px = 0; px <= W; px += 4) x.lineTo(px, ridge(px));
      for (var p2 = W; p2 >= 0; p2 -= 4) x.lineTo(p2, ridge(p2) + 12);
      x.closePath(); x.fill();
      x.fillStyle = '#76a647'; x.fillRect(-10, H * 0.84, W + 20, H);
    });
    x.beginPath(); x.moveTo(0, ridge(0));
    for (var px = 0; px <= W; px += 4) x.lineTo(px, ridge(px));
    x.lineWidth = 4; x.strokeStyle = '#3f5f2c'; x.lineJoin = 'round'; x.stroke();
    // tiny trees
    for (var t = 0; t < 70; t++) {
      var tx = 15 + r() * (W - 30), ty = ridge(tx) + 6 + r() * 60;
      if (ty > H - 8) continue;
      if (r() < 0.4) { ellipse(x, tx, ty - 9, 4, 11); paint(x, '#2f6e3b', 2); }
      else { circle(x, tx, ty - 5, 6); paint(x, '#3f8a3c', 2); }
    }
    // villages
    [300, 1100, 1720].forEach(function (vx, vi) {
      var houses = 5 + vi;
      for (var h = 0; h < houses; h++) {
        var hx = vx + h * 17 - houses * 8, hy = ridge(hx) + 10 + (h % 2) * 5;
        var hw = 14 + (h * 7) % 8, hh = 10 + (h * 5) % 7;
        x.beginPath(); x.rect(hx - hw / 2, hy - hh, hw, hh); paint(x, h % 3 ? '#f3dfb4' : '#f2c27c', 2);
        poly(x, [[hx - hw / 2 - 2, hy - hh], [hx, hy - hh - 6], [hx + hw / 2 + 2, hy - hh]]); paint(x, '#d8693d', 2);
        if (h === 2) {
          x.beginPath(); x.rect(hx - 4, hy - hh - 22, 8, 22); paint(x, '#f3dfb4', 2);
          poly(x, [[hx - 5, hy - hh - 22], [hx, hy - hh - 30], [hx + 5, hy - hh - 22]]); paint(x, '#d8693d', 2);
        }
      }
    });
    return c;
  }

  function makeNear() {
    var W = 2048, H = 240, c = mk(W, H), x = c.getContext('2d');
    var r = rng(77);
    var trees = [];
    var px = 0;
    while (px < W) {
      var kind = r();
      trees.push({ x: px, k: kind < 0.38 ? 'cyp' : (kind < 0.7 ? 'pine' : 'round'), s: 0.75 + r() * 0.45 });
      px += 34 + r() * 60;
    }
    var base = H * 0.78;
    function draw(t, ox) {
      var tx = t.x + ox, s = t.s;
      if (t.k === 'cyp') {
        var h = 150 * s, w = 18 * s;
        x.beginPath(); x.moveTo(tx, base - h);
        x.quadraticCurveTo(tx + w * 1.3, base - h * 0.45, tx + w * 0.7, base);
        x.lineTo(tx - w * 0.7, base);
        x.quadraticCurveTo(tx - w * 1.3, base - h * 0.45, tx, base - h);
        x.closePath(); paint(x, '#24552f', 4);
        line(x, [[tx - w * 0.3, base - h * 0.7], [tx - w * 0.5, base - h * 0.3]], 3, '#3a7a45');
      } else if (t.k === 'pine') {
        var th = 95 * s;
        rr(x, tx - 4, base - th, 8, th, 3); paint(x, '#6e3f27', 3);
        var cw = 62 * s;
        blob(x, [[tx - cw * 0.55, base - th, 20 * s], [tx, base - th - 9 * s, 26 * s], [tx + cw * 0.55, base - th, 20 * s]], '#2f6a37', '#3f8a49', null, 2);
      } else {
        var rh = 55 * s;
        rr(x, tx - 4, base - rh, 8, rh, 3); paint(x, '#6e3f27', 3);
        blob(x, [[tx, base - rh - 10 * s, 30 * s], [tx - 18 * s, base - rh + 6, 20 * s], [tx + 18 * s, base - rh + 6, 20 * s]], '#3a7a3c', '#55a052', null, 2);
      }
    }
    trees.forEach(function (t) { [-W, 0, W].forEach(function (o) { if (t.x + o > -120 && t.x + o < W + 120) draw(t, o); }); });
    x.beginPath(); x.rect(-20, base - 6, W + 40, H);
    paint(x, '#2f6235', 4);
    x.fillStyle = '#3c7a40'; x.fillRect(0, base - 4, W, 6);
    return c;
  }

  function makeSun() {
    var W = 256, H = 256, c = mk(W, H), x = c.getContext('2d');
    var cx = W / 2, cy = H / 2;
    for (var i = 0; i < 16; i++) {
      var a = i / 16 * TAU, r1 = 82, r2 = i % 2 ? 112 : 124, hw = 0.13;
      poly(x, [[cx + Math.cos(a - hw) * r1, cy + Math.sin(a - hw) * r1], [cx + Math.cos(a) * r2, cy + Math.sin(a) * r2], [cx + Math.cos(a + hw) * r1, cy + Math.sin(a + hw) * r1]]);
      paint(x, i % 2 ? '#ffc83a' : '#ffad1f', 4);
    }
    circle(x, cx, cy, 84); paint(x, '#ffd94a', 5);
    clipped(x, function () { circle(x, cx, cy, 84); }, function () {
      x.fillStyle = '#f5b82e'; circle(x, cx + 34, cy + 34, 90); x.fill();
      x.fillStyle = '#ffd94a'; circle(x, cx - 6, cy - 6, 84); x.fill();
      x.fillStyle = '#fff09a'; circle(x, cx - 40, cy - 42, 22); x.fill();
    });
    circle(x, cx, cy, 84); paint(x, null, 5);
    // sunglasses
    rr(x, cx - 58, cy - 18, 50, 32, 12); paint(x, '#15151f', 4);
    rr(x, cx + 8, cy - 18, 50, 32, 12); paint(x, '#15151f', 4);
    line(x, [[cx - 8, cy - 8], [cx + 8, cy - 8]], 5);
    line(x, [[cx - 48, cy - 10], [cx - 30, cy + 6]], 4, 'rgba(255,255,255,0.7)');
    line(x, [[cx + 18, cy - 10], [cx + 36, cy + 6]], 4, 'rgba(255,255,255,0.7)');
    x.beginPath(); x.arc(cx, cy + 22, 30, 0.12 * Math.PI, 0.88 * Math.PI); x.lineWidth = 6; x.strokeStyle = OL; x.lineCap = 'round'; x.stroke();
    ellipse(x, cx - 52, cy + 28, 13, 8); x.fillStyle = 'rgba(255,110,90,0.55)'; x.fill();
    ellipse(x, cx + 52, cy + 28, 13, 8); x.fill();
    return c;
  }

  function makeCloud(seed, W, H) {
    var c = mk(W, H), x = c.getContext('2d');
    var r = rng(seed), cs = [];
    var n = 6 + Math.floor(r() * 3);
    for (var i = 0; i < n; i++) {
      var t = i / (n - 1);
      var rad = H * (0.2 + Math.sin(t * Math.PI) * 0.18 + r() * 0.05);
      cs.push([W * (0.14 + t * 0.72), H * 0.68 - Math.sin(t * Math.PI) * H * 0.2 - r() * H * 0.06, rad]);
    }
    cs.push([W * 0.5, H * 0.66, H * 0.26]);
    var lw = 5;
    x.save();
    x.strokeStyle = '#3b4a72'; x.lineWidth = lw * 2;
    cs.forEach(function (k) { circle(x, k[0], k[1], k[2]); x.stroke(); });
    x.fillStyle = '#d4e1f3';
    cs.forEach(function (k) { circle(x, k[0], k[1], k[2]); x.fill(); });
    x.fillStyle = '#ffffff';
    cs.forEach(function (k) { circle(x, k[0], k[1] - k[2] * 0.14, k[2] * 0.86); x.fill(); });
    x.restore();
    return c;
  }

  /* -------------------------------------------------------------- effects */

  function makeBoom() {
    var W = 400, H = 320, c = mk(W, H), x = c.getContext('2d');
    var cx = W / 2, cy = H / 2, r = rng(9);
    function spiky(scale, fill) {
      x.beginPath();
      var n = 14;
      for (var i = 0; i < n * 2; i++) {
        var a = i / (n * 2) * TAU - 0.2;
        var rad = (i % 2 ? 0.66 : 1) * scale * (0.85 + r() * 0.2);
        var px = cx + Math.cos(a) * rad * W * 0.47, py = cy + Math.sin(a) * rad * H * 0.47;
        if (i === 0) x.moveTo(px, py); else x.lineTo(px, py);
      }
      x.closePath(); paint(x, fill, 6);
    }
    spiky(1, '#e8382f');
    spiky(0.8, '#ff8c1a');
    spiky(0.6, '#ffe14d');
    x.save(); x.translate(cx, cy); x.rotate(-0.12);
    label(x, 'BOUM !', 0, 4, 74, '#ffffff', 14, OL, W * 0.7);
    x.restore();
    return c;
  }

  function makeStar() {
    var c = mk(64, 64), x = c.getContext('2d');
    star(x, 32, 34, 5, 28, 12); paint(x, '#ffe14d', 4);
    star(x, 29, 30, 5, 12, 5); x.fillStyle = '#fff6b0'; x.fill();
    return c;
  }

  function makeSmoke() {
    var c = mk(128, 128), x = c.getContext('2d');
    blob(x, [[44, 70, 30], [78, 64, 34], [62, 44, 28], [86, 88, 22], [40, 92, 20]], '#d9d9e0', '#f4f4f8', '#b1b1bd', 3, '#6b6b78');
    return c;
  }

  function makePouet() {
    var W = 256, H = 160, c = mk(W, H), x = c.getContext('2d');
    var bubble = function () {
      x.beginPath();
      x.ellipse(W / 2 + 10, H * 0.44, W * 0.43, H * 0.38, 0, 0.62 * Math.PI, 0.54 * Math.PI + TAU);
      x.lineTo(W * 0.08, H * 0.96);
      x.closePath();
    };
    bubble(); paint(x, '#ffffff', 6);
    x.save(); x.translate(W / 2 + 10, H * 0.45); x.rotate(-0.08);
    label(x, 'POUET !', 0, 0, 46, '#ffd23f', 10, OL, W * 0.72);
    x.restore();
    line(x, [[W * 0.9, H * 0.08], [W * 0.98, H * 0.0 + 2]], 5);
    line(x, [[W * 0.93, H * 0.2], [W - 3, H * 0.17]], 5);
    return c;
  }

  function makeFlash() {
    var W = 256, H = 256, c = mk(W, H), x = c.getContext('2d');
    x.save();
    x.globalAlpha = 0.85;
    star(x, W / 2, H / 2, 12, W * 0.5, W * 0.18, 0);
    x.fillStyle = '#fff6a8'; x.fill();
    x.restore();
    var g = x.createRadialGradient(W / 2, H / 2, 0, W / 2, H / 2, W / 2);
    g.addColorStop(0, 'rgba(255,255,255,1)');
    g.addColorStop(0.35, 'rgba(255,255,230,0.95)');
    g.addColorStop(0.7, 'rgba(255,240,150,0.45)');
    g.addColorStop(1, 'rgba(255,240,150,0)');
    x.fillStyle = g; x.fillRect(0, 0, W, H);
    return c;
  }

  /* ------------------------------------------------------------ town sign */

  function townSign(text, ended) {
    text = String(text || '').toUpperCase();
    var key = text + (ended ? '|end' : '');
    if (townCache[key]) return townCache[key];
    var W = 320, H = 300, c = mk(W, H), x = c.getContext('2d');
    var L = lwOf(W, 1800);
    shadowEll(x, W / 2, H - 4, W * 0.36, 5, 0.25);
    pole(x, W * 0.27, H * 0.38, H - 2, 12, L);
    pole(x, W * 0.73, H * 0.38, H - 2, 12, L);
    var py = L, ph = H * 0.44;
    rr(x, L, py, W - 2 * L, ph, 12); paint(x, '#ffffff', L);
    rr(x, L + 9, py + 9, W - 2 * L - 18, ph - 18, 8); paint(x, null, 7, '#e0262b');
    var maxW = W - 2 * L - 46;
    x.font = font(56);
    var one = x.measureText(text).width;
    var parts = text.split(/[-\s]/);
    if (one * (40 / 56) > maxW && parts.length > 1) {
      // two lines, keep the hyphen on the first line
      var mid = text.lastIndexOf('-') > 0 ? text.lastIndexOf('-') + 1 : text.lastIndexOf(' ') + 1;
      var l1 = text.slice(0, mid).trim(), l2 = text.slice(mid).trim();
      label(x, l1, W / 2, py + ph * 0.36, ph * 0.26, '#15151f', 0, null, maxW);
      label(x, l2, W / 2, py + ph * 0.67, ph * 0.26, '#15151f', 0, null, maxW);
    } else {
      label(x, text, W / 2, py + ph * 0.52, ph * 0.34, '#15151f', 0, null, maxW);
    }
    if (ended) line(x, [[L + 16, py + ph - 16], [W - L - 16, py + 16]], 10, '#e0262b');
    var rec = { img: c, worldW: 1800, name: 'town:' + key };
    townCache[key] = rec;
    return rec;
  }

  /* --------------------------------------------------------------- init */

  function init() {
    if (ready) return;
    var t0 = (window.performance && performance.now) ? performance.now() : Date.now();
    var ex = { exhaust: [[0.255, 0.84], [0.745, 0.84]] };
    add('c4_c', drawC4(0, false), 1100, ex);
    add('c4_l', drawC4(-1, false), 1100, ex);
    add('c4_r', drawC4(1, false), 1100, ex);
    add('c4_c_b', drawC4(0, true), 1100, ex);
    add('c4_l_b', drawC4(-1, true), 1100, ex);
    add('c4_r_b', drawC4(1, true), 1100, ex);

    var cars = [['car_red', '#e8413c'], ['car_yellow', '#ffcf33'], ['car_white', '#f2f2ee'], ['car_green', '#4cbb5a'], ['car_purple', '#9a5ce0'], ['car_orange', '#ff8a2b']];
    cars.forEach(function (cc, i) {
      var o = { idx: i };
      if (cc[0] === 'car_white') { o.hi = '#ffffff'; o.sh = '#c9cdd4'; }
      add(cc[0] + '_rear', hatch(cc[1], 'rear', o), 1050);
      add(cc[0] + '_front', hatch(cc[1], 'front', o), 1050);
    });
    add('cv2_rear', deuche('rear'), 950);
    add('cv2_front', deuche('front'), 950);
    add('van_rear', van('rear'), 1150);
    add('van_front', van('front'), 1150);
    add('truck_rear', truck('rear'), 1350);
    add('truck_front', truck('front'), 1350);
    add('tractor_rear', tractor('rear'), 1100);
    add('tractor_front', tractor('front'), 1100);
    var pol = { idx: 4, police: true, roofY: 0.16, hi: '#4a6fc4', sh: '#142a66' };
    add('police_rear', hatch('#1d3a8f', 'rear', pol), 1050);
    add('police_front', hatch('#1d3a8f', 'front', pol), 1050);

    add('tree_cypress', cypress(), 700);
    add('tree_olive', olive(), 1800);
    add('tree_pine', pine(), 2600);
    add('tree_plane', plane(), 2200);
    add('bush', bush(false), 900);
    add('bush_flower', bush(true), 1000);
    add('lavender', trimTop(lavender()), 1600);
    add('vine', vine(), 1400);
    add('sunflower', sunflowers(), 1200);
    add('rock', rock(), 1100);
    add('house_mas', mas(), 5000);
    add('house_village', villageHouse('#f2a9a1', '#5b8fd1'), 3600);
    add('house_village2', villageHouse('#f6cf6b', '#4c9a6a'), 3600);
    add('church', church(), 3600);

    [50, 70, 90, 110, 130].forEach(function (n) { add('sign_speed_' + n, speedSign(n), 600, { speed: n }); });
    add('sign_radar', radarSign(), 800);
    add('radar_box', radarBox(), 650);
    add('sign_chevron_l', chevronSign(-1), 700);
    add('sign_chevron_r', chevronSign(1), 700);
    add('sign_roundabout', triangleSign('roundabout'), 800);
    add('sign_works', triangleSign('works'), 800);
    add('cone', cone(), 400);
    add('barrier_works', barrier(), 1500);
    add('sign_a7', signA7(), 2800);
    add('sign_dir', signDir(), 2200);
    add('sign_autoroute', signAutoroute(), 900);
    ['calisson', 'savon', 'melon', 'olive'].forEach(function (k) { add('billboard_' + k, billboard(k), 3200); });
    add('arch_start', archBase('start'), 4800, { arch: true });
    add('arch_finish', archBase('finish'), 4800, { arch: true });
    add('token', token(), 600);

    bg.far = makeFar();
    bg.mid = makeMid();
    bg.near = makeNear();
    bg.sun = makeSun();
    bg.clouds = [makeCloud(1, 400, 190), makeCloud(2, 360, 170), makeCloud(4, 440, 200)];
    fx.boom = makeBoom();
    fx.star = makeStar();
    fx.smoke = makeSmoke();
    fx.pouet = makePouet();
    fx.flash = makeFlash();

    ready = true;
    var t1 = (window.performance && performance.now) ? performance.now() : Date.now();
    api.initMs = Math.round(t1 - t0);
  }

  function getPlaceholder() {
    if (placeholder) return placeholder;
    var c = mk(64, 64), x = c.getContext('2d');
    x.fillStyle = '#ff00ff'; x.fillRect(0, 0, 64, 64);
    x.fillStyle = '#000'; x.fillRect(0, 0, 32, 32); x.fillRect(32, 32, 32, 32);
    placeholder = { img: c, worldW: 600, name: 'missing', missing: true };
    return placeholder;
  }

  function get(name) {
    if (!ready) { try { init(); } catch (e) { /* fall through to placeholder */ } }
    return store[name] || getPlaceholder();
  }

  function debugSheet() {
    if (!ready) init();
    var list = names.slice();
    var extra = [townSign('Cornillon-Confoux'), townSign('Lançon-Provence', true), townSign('La Pécette')];
    var cols = 8, cw = 190, ch = 230;
    var all = list.map(function (n) { return store[n]; }).concat(extra);
    var rows = Math.ceil(all.length / cols);
    var c = mk(cols * cw, rows * ch), x = c.getContext('2d');
    x.fillStyle = '#f0f3f7'; x.fillRect(0, 0, c.width, c.height);
    all.forEach(function (rec, i) {
      var cx = (i % cols) * cw, cy = Math.floor(i / cols) * ch;
      // checkerboard
      for (var gy = 0; gy < 18; gy++) for (var gx = 0; gx < 19; gx++) {
        x.fillStyle = (gx + gy) % 2 ? '#dde3ea' : '#eef1f5';
        x.fillRect(cx + 5 + gx * 10, cy + 5 + gy * 10, 10, 10);
      }
      var img = rec.img, s = Math.min(176 / img.width, 176 / img.height);
      var w = img.width * s, h = img.height * s;
      var bx = cx + cw / 2 - w / 2, by = cy + 184 - h;
      x.drawImage(img, bx, by, w, h);
      x.strokeStyle = 'rgba(0,0,0,0.25)'; x.lineWidth = 1; x.strokeRect(bx + 0.5, by + 0.5, w - 1, h - 1);
      x.strokeStyle = '#ff2050'; x.lineWidth = 2;
      x.beginPath(); x.moveTo(cx + 6, cy + 184); x.lineTo(cx + cw - 6, cy + 184); x.stroke();
      x.fillStyle = '#15151f'; x.font = '700 12px system-ui, sans-serif'; x.textAlign = 'center'; x.textBaseline = 'top';
      x.fillText(rec.name + ' (' + rec.worldW + ')', cx + cw / 2, cy + 192);
      x.fillStyle = '#667'; x.font = '11px system-ui, sans-serif';
      x.fillText(img.width + '×' + img.height, cx + cw / 2, cy + 208);
    });
    return c;
  }

  var api = {
    init: init,
    get: get,
    townSign: townSign,
    bg: bg,
    fx: fx,
    names: names,
    debugSheet: debugSheet,
    initMs: 0
  };
  return api;
})();
