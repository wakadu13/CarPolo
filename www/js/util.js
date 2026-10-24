/* CarPolo — petits outils partagés (maths, couleurs, stockage, texte cartoon) */
(function () {
  'use strict';

  const U = {
    clamp(v, a, b) { return v < a ? a : v > b ? b : v; },
    lerp(a, b, t) { return a + (b - a) * t; },
    smooth(t) { t = U.clamp(t, 0, 1); return t * t * (3 - 2 * t); },
    easeOut(t) { t = U.clamp(t, 0, 1); return 1 - (1 - t) * (1 - t); },
    easeOutBack(t) {
      t = U.clamp(t, 0, 1);
      const c1 = 1.70158, c3 = c1 + 1;
      return 1 + c3 * Math.pow(t - 1, 3) + c1 * Math.pow(t - 1, 2);
    },
    approach(v, target, rate) {
      if (v < target) return Math.min(target, v + rate);
      return Math.max(target, v - rate);
    },
    rand(a, b) { return a + Math.random() * (b - a); },
    randInt(a, b) { return Math.floor(a + Math.random() * (b - a + 1)); },
    pick(arr) { return arr[Math.floor(Math.random() * arr.length)]; },

    // Générateur pseudo-aléatoire reproductible (le décor est le même à chaque partie)
    seeded(seed) {
      let s = seed >>> 0;
      const f = function () {
        s = (s + 0x6D2B79F5) >>> 0;
        let t = s;
        t = Math.imul(t ^ (t >>> 15), t | 1);
        t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
      };
      f.range = (a, b) => a + f() * (b - a);
      f.pick = (arr) => arr[Math.floor(f() * arr.length)];
      f.weighted = (items) => {
        let total = 0;
        for (const it of items) total += it.w;
        let r = f() * total;
        for (const it of items) { r -= it.w; if (r <= 0) return it; }
        return items[items.length - 1];
      };
      return f;
    },

    hexToRgb(hex) {
      const h = hex.replace('#', '');
      const n = parseInt(h.length === 3 ? h.split('').map(c => c + c).join('') : h, 16);
      return [(n >> 16) & 255, (n >> 8) & 255, n & 255];
    },
    mixHex(a, b, t) {
      const A = U.hexToRgb(a), B = U.hexToRgb(b);
      const r = Math.round(U.lerp(A[0], B[0], t));
      const g = Math.round(U.lerp(A[1], B[1], t));
      const bl = Math.round(U.lerp(A[2], B[2], t));
      return 'rgb(' + r + ',' + g + ',' + bl + ')';
    },

    fmtTime(ms) {
      if (ms == null || !isFinite(ms)) return '--:--.--';
      ms = Math.max(0, ms);
      const m = Math.floor(ms / 60000);
      const s = Math.floor((ms % 60000) / 1000);
      const c = Math.floor((ms % 1000) / 10);
      return m + ':' + (s < 10 ? '0' : '') + s + '.' + (c < 10 ? '0' : '') + c;
    },

    store: {
      get(key, def) {
        try {
          const v = window.localStorage.getItem(key);
          return v == null ? def : JSON.parse(v);
        } catch (e) { return def; }
      },
      set(key, val) {
        try { window.localStorage.setItem(key, JSON.stringify(val)); } catch (e) { /* stockage indisponible */ }
      }
    },

    // Texte façon BD : contour épais + remplissage
    FONT: '"Arial Black", "Roboto Black", "Roboto", system-ui, sans-serif',
    text(ctx, str, x, y, size, fill, opts) {
      opts = opts || {};
      ctx.save();
      ctx.font = '900 ' + Math.round(size) + 'px ' + U.FONT;
      ctx.textAlign = opts.align || 'center';
      ctx.textBaseline = opts.baseline || 'middle';
      ctx.lineJoin = 'round';
      if (opts.alpha != null) ctx.globalAlpha = opts.alpha;
      if (opts.shadow !== false) {
        ctx.fillStyle = 'rgba(0,0,0,0.35)';
        ctx.fillText(str, x + size * 0.06, y + size * 0.09);
      }
      if (opts.stroke !== false) {
        ctx.strokeStyle = opts.stroke || '#1b1b2f';
        ctx.lineWidth = Math.max(2, size * (opts.strokeW || 0.2));
        ctx.strokeText(str, x, y);
      }
      ctx.fillStyle = fill;
      ctx.fillText(str, x, y);
      ctx.restore();
    },

    roundRect(ctx, x, y, w, h, r) {
      r = Math.min(r, w / 2, h / 2);
      ctx.beginPath();
      ctx.moveTo(x + r, y);
      ctx.arcTo(x + w, y, x + w, y + h, r);
      ctx.arcTo(x + w, y + h, x, y + h, r);
      ctx.arcTo(x, y + h, x, y, r);
      ctx.arcTo(x, y, x + w, y, r);
      ctx.closePath();
    }
  };

  window.U = U;
})();
