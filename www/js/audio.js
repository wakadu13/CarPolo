/*
 * CarPolo — synthesized audio (WebAudio only, no audio files).
 * Classic script: exposes window.AudioFX. Every public method is a safe no-op
 * when WebAudio is unavailable.
 *
 *   AudioFX.unlock()                 call from a user gesture (idempotent)
 *   AudioFX.update(dt, state)        every frame (engine + music scheduler)
 *   AudioFX.play(name, {volume, pan}) one-shot sound effect
 *   AudioFX.music('menu'|'race'|null)
 *   AudioFX.pauseAll() / resumeAll()
 *   AudioFX.setMusicEnabled(b) / setSfxEnabled(b) / isMusicEnabled() / isSfxEnabled()
 */
(function () {
  'use strict';

  var STORE_KEY = 'carpolo.audio';
  var MUSIC_VOL = 0.2;   // music bus level (kept under the sound effects)
  var SFX_VOL = 0.9;     // sound effects bus level (engine included)

  // Speed thresholds of the 5 gears (speed 1 = normal top speed, >1 = turbo).
  var GEARS = [0, 0.16, 0.34, 0.55, 0.78, 1.6];

  var ctx = null;
  var master = null, sfxBus = null, musicBus = null, engineBus = null;
  var noiseBuf = null, pulseWave = null;
  var engine = null;
  var musicOn = true, sfxOn = true, paused = false, unlocked = false;
  var wanted = null;              // music requested by the game
  var resumeStep = {};            // song position kept across pauseAll/resumeAll
  var lookahead = 0.15;           // music scheduling window (seconds)
  var lastPlay = {};
  var songIssues = [];

  loadPrefs();

  /* ------------------------------------------------------------ helpers */

  function loadPrefs() {
    try {
      var raw = window.localStorage && window.localStorage.getItem(STORE_KEY);
      if (raw) {
        var p = JSON.parse(raw);
        if (typeof p.music === 'boolean') musicOn = p.music;
        if (typeof p.sfx === 'boolean') sfxOn = p.sfx;
      }
    } catch (e) { /* storage unavailable */ }
  }

  function savePrefs() {
    try {
      window.localStorage.setItem(STORE_KEY, JSON.stringify({ music: musicOn, sfx: sfxOn }));
    } catch (e) { /* storage unavailable */ }
  }

  function clamp(v, a, b) { return v < a ? a : (v > b ? b : v); }
  function mtof(m) { return 440 * Math.pow(2, (m - 69) / 12); }

  // Smoothly move an AudioParam toward a value (no clicks).
  function glide(param, value, tc) {
    param.setTargetAtTime(value, ctx.currentTime, tc);
  }

  function makeGain(v, dest) {
    var g = ctx.createGain();
    g.gain.value = v;
    if (dest) g.connect(dest);
    return g;
  }

  function makeFilter(type, freq, q, dest) {
    var f = ctx.createBiquadFilter();
    f.type = type;
    f.frequency.value = freq;
    f.Q.value = q == null ? 1 : q;
    if (dest) f.connect(dest);
    return f;
  }

  function makeOsc(type, freq) {
    var o = ctx.createOscillator();
    o.type = type;
    o.frequency.value = freq;
    return o;
  }

  function makeNoiseBuffer(seconds) {
    var len = Math.floor(ctx.sampleRate * seconds);
    var b = ctx.createBuffer(1, len, ctx.sampleRate);
    var d = b.getChannelData(0);
    for (var i = 0; i < len; i++) d[i] = Math.random() * 2 - 1;
    return b;
  }

  function makePulseWave(duty) {
    var n = 32, real = new Float32Array(n), imag = new Float32Array(n);
    for (var k = 1; k < n; k++) real[k] = (2 / (k * Math.PI)) * Math.sin(k * Math.PI * duty);
    return ctx.createPeriodicWave(real, imag);
  }

  function disconnectAll(list) {
    for (var i = 0; i < list.length; i++) {
      try { list[i].disconnect(); } catch (e) { /* already gone */ }
    }
  }

  /* ------------------------------------------------------------- graph */

  function ensureContext() {
    if (ctx) return ctx;
    var AC = window.AudioContext || window.webkitAudioContext;
    if (!AC) return null;
    try {
      ctx = new AC({ latencyHint: 'interactive' });
    } catch (e) {
      try { ctx = new AC(); } catch (e2) { ctx = null; return null; }
    }
    try {
      buildGraph();
    } catch (e3) {
      ctx = null;
      return null;
    }
    return ctx;
  }

  function buildGraph() {
    var comp = ctx.createDynamicsCompressor();
    comp.threshold.value = -12;
    comp.knee.value = 8;
    comp.ratio.value = 8;
    comp.attack.value = 0.003;
    comp.release.value = 0.18;
    comp.connect(ctx.destination);

    master = makeGain(0.9, comp);
    sfxBus = makeGain(sfxOn ? SFX_VOL : 0, master);
    musicBus = makeGain(musicOn && !paused ? MUSIC_VOL : 0, master);
    engineBus = makeGain(paused ? 0 : 1, sfxBus);

    noiseBuf = makeNoiseBuffer(1.5);
    try { pulseWave = makePulseWave(0.25); } catch (e) { pulseWave = null; }
    buildEngine();
  }

  /* ------------------------------------------------------------ engine */

  function noiseLoop(offset) {
    var s = ctx.createBufferSource();
    s.buffer = noiseBuf;
    s.loop = true;
    s.start(0, offset || 0);
    return s;
  }

  function buildEngine() {
    var e = {};
    e.out = makeGain(0, engineBus);

    // Main "vroum": sawtooth + sub square -> lowpass -> tremolo (chug) -> level
    e.toneGain = makeGain(0, e.out);
    e.trem = makeGain(0.75, e.toneGain);
    e.filter = makeFilter('lowpass', 400, 2.2, e.trem);
    e.osc1 = makeOsc('sawtooth', 55);
    e.osc2 = makeOsc('square', 27.5);
    e.osc2Gain = makeGain(0.45, e.filter);
    e.osc1.connect(e.filter);
    e.osc2.connect(e.osc2Gain);

    // Pitch wobble
    e.lfo = makeOsc('sine', 5.5);
    e.lfoAmt1 = makeGain(1.6, e.osc1.frequency);
    e.lfoAmt2 = makeGain(0.8, e.osc2.frequency);
    e.lfo.connect(e.lfoAmt1);
    e.lfo.connect(e.lfoAmt2);

    // Amplitude chug (cylinder pulses), follows rpm
    e.chug = makeOsc('sine', 14);
    e.chugAmt = makeGain(0.25, e.trem.gain);
    e.chug.connect(e.chugAmt);

    // Turbo whine
    e.turboGain = makeGain(0, e.out);
    e.turbo = makeOsc('triangle', 1200);
    e.turbo.connect(e.turboGain);

    // Off-road rumble
    e.rumbleGain = makeGain(0, e.out);
    e.rumbleF = makeFilter('lowpass', 200, 1.2, e.rumbleGain);
    e.rumbleSrc = noiseLoop(0);
    e.rumbleSrc.connect(e.rumbleF);

    // Tyre screech
    e.skidGain = makeGain(0, e.out);
    e.skidF = makeFilter('bandpass', 2500, 5, e.skidGain);
    e.skidSrc = noiseLoop(0.7);
    e.skidSrc.connect(e.skidF);
    e.skidToneGain = makeGain(0.22, e.skidGain);
    e.skidTone = makeOsc('triangle', 1900);
    e.skidTone.connect(e.skidToneGain);

    e.osc1.start();
    e.osc2.start();
    e.lfo.start();
    e.chug.start();
    e.turbo.start();
    e.skidTone.start();

    e.gear = 0;
    e.shift = 0;
    e.rpm = 0.12;
    e.acc = 1;
    engine = e;
  }

  function updateEngine(dt, s) {
    var e = engine;
    var running = !!s.running && !paused;
    var speed = clamp(+s.speed || 0, 0, 1.6);
    var thr = clamp(+s.throttle || 0, 0, 1);
    var skid = clamp(+s.skid || 0, 0, 1);
    var boost = !!s.boost;

    var g = 0;
    while (g < GEARS.length - 2 && speed >= GEARS[g + 1]) g++;
    if (g > e.gear && running) e.shift = 0.11;   // short dip on up-shift
    e.gear = g;
    e.shift = Math.max(0, e.shift - dt);

    var lo = GEARS[g], hi = GEARS[g + 1];
    var t = clamp((speed - lo) / (hi - lo), 0, 1);
    var rpm = g === 0 ? 0.12 + 0.88 * t : 0.42 + 0.58 * t;
    if (speed < 0.02) rpm = 0.12 + thr * 0.3;          // idle / free revving
    if (speed > 1) rpm = Math.min(1.2, rpm + (speed - 1) * 0.6);
    e.rpm += (rpm - e.rpm) * Math.min(1, dt * 12);

    // Update the audio params at ~30 Hz, not every frame.
    e.acc += dt;
    if (e.acc < 0.03) return;
    e.acc = 0;

    var r = e.rpm;
    var shiftK = e.shift > 0 ? 0.5 : 1;
    var f = 40 + r * 100 + g * 6;

    glide(e.osc1.frequency, f, 0.05);
    glide(e.osc2.frequency, f * 0.5, 0.05);
    glide(e.chug.frequency, Math.max(6, f * 0.25), 0.05);
    glide(e.chugAmt.gain, 0.3 - 0.2 * Math.min(1, r), 0.1);
    glide(e.filter.frequency, 260 + r * 900 + thr * 900, 0.06);
    glide(e.toneGain.gain, (0.06 + 0.05 * thr + 0.03 * Math.min(1, r)) * shiftK, 0.05);
    glide(e.out.gain, running ? 1 : 0, running ? 0.08 : 0.15);

    var turbo = running && (boost || speed > 1.01) ? (boost ? 0.04 : 0.022) : 0;
    glide(e.turboGain.gain, turbo, 0.12);
    glide(e.turbo.frequency, 1100 + speed * 900, 0.1);

    var rumble = running && s.offroad ? 0.4 * clamp(speed * 1.6 + 0.15, 0, 1) : 0;
    glide(e.rumbleGain.gain, rumble, 0.06);
    glide(e.rumbleF.frequency, 150 + speed * 300, 0.1);

    var screech = running && skid > 0.3 ? ((skid - 0.3) / 0.7) * 0.09 : 0;
    glide(e.skidGain.gain, screech, 0.05);
    glide(e.skidF.frequency, 2200 + Math.random() * 700, 0.03);
    glide(e.skidTone.frequency, 1750 + Math.random() * 300, 0.03);
  }

  /* -------------------------------------------------- sound primitives */

  // Oscillator note with an envelope. o: {type|wave, f, f2, fT, t, d, v, a, hold,
  //   filter, ff, ff2, q, vib, vibRate, detune}
  function tone(dest, o) {
    var t0 = o.t, d = o.d, v = o.v == null ? 0.3 : o.v, a = o.a || 0.005;
    var osc = ctx.createOscillator();
    if (o.wave) osc.setPeriodicWave(o.wave); else osc.type = o.type || 'sine';
    osc.frequency.setValueAtTime(o.f, t0);
    if (o.f2) osc.frequency.exponentialRampToValueAtTime(o.f2, t0 + (o.fT || d));
    if (o.detune) osc.detune.value = o.detune;

    var g = ctx.createGain();
    g.gain.setValueAtTime(0.0001, t0);
    g.gain.linearRampToValueAtTime(v, t0 + a);
    if (o.hold) g.gain.setValueAtTime(v, t0 + Math.max(a, d * o.hold));
    g.gain.exponentialRampToValueAtTime(0.0001, t0 + d);
    osc.connect(g);

    var nodes = [osc, g], last = g, filt = null;
    if (o.filter) {
      filt = makeFilter(o.filter, o.ff || 2000, o.q);
      if (o.ff2) {
        filt.frequency.setValueAtTime(o.ff, t0);
        filt.frequency.exponentialRampToValueAtTime(o.ff2, t0 + d);
      }
      g.connect(filt);
      last = filt;
      nodes.push(filt);
    }
    if (o.vib) {
      var lfo = makeOsc('sine', o.vibRate || 5.5);
      var la = ctx.createGain();
      la.gain.setValueAtTime(0, t0);
      la.gain.linearRampToValueAtTime(o.vib, t0 + Math.min(0.25, d * 0.5));
      lfo.connect(la);
      la.connect(osc.detune);
      lfo.start(t0);
      lfo.stop(t0 + d + 0.05);
      nodes.push(lfo, la);
    }
    last.connect(dest);
    osc.start(t0);
    osc.stop(t0 + d + 0.05);
    osc.onended = function () { disconnectAll(nodes); };
    return { osc: osc, gain: g, filter: filt };
  }

  // Filtered noise burst. o: {t, d, v, a, hold, type, f, f2, fpeak, peakAt, q}
  function noise(dest, o) {
    var t0 = o.t, d = o.d, v = o.v == null ? 0.3 : o.v, a = o.a || 0.003;
    var src = ctx.createBufferSource();
    src.buffer = noiseBuf;
    src.loop = true;
    var fl = makeFilter(o.type || 'bandpass', o.f, o.q);
    fl.frequency.setValueAtTime(o.f, t0);
    if (o.fpeak) {
      var tp = t0 + d * (o.peakAt || 0.5);
      fl.frequency.exponentialRampToValueAtTime(o.fpeak, tp);
      if (o.f2) fl.frequency.exponentialRampToValueAtTime(o.f2, t0 + d);
    } else if (o.f2) {
      fl.frequency.exponentialRampToValueAtTime(o.f2, t0 + d);
    }
    var g = ctx.createGain();
    g.gain.setValueAtTime(0.0001, t0);
    g.gain.linearRampToValueAtTime(v, t0 + a);
    if (o.hold) g.gain.setValueAtTime(v, t0 + Math.max(a, d * o.hold));
    g.gain.exponentialRampToValueAtTime(0.0001, t0 + d);
    src.connect(fl);
    fl.connect(g);
    g.connect(dest);
    src.start(t0, Math.random() * 1.2);
    src.stop(t0 + d + 0.05);
    src.onended = function () { disconnectAll([src, fl, g]); };
  }

  // Cartoon "boiiing": falling sine with a wobble that slows down.
  function boing(dest, t, d, f1, f2, v) {
    var osc = makeOsc('sine', f1);
    osc.frequency.setValueAtTime(f1, t);
    osc.frequency.exponentialRampToValueAtTime(f2, t + d);
    var lfo = makeOsc('sine', 16);
    lfo.frequency.setValueAtTime(16, t);
    lfo.frequency.linearRampToValueAtTime(6, t + d);
    var la = ctx.createGain();
    la.gain.setValueAtTime(f1 * 0.12, t);
    la.gain.linearRampToValueAtTime(f2 * 0.1, t + d);
    lfo.connect(la);
    la.connect(osc.frequency);
    var g = ctx.createGain();
    g.gain.setValueAtTime(0.0001, t);
    g.gain.linearRampToValueAtTime(v, t + 0.012);
    g.gain.setValueAtTime(v, t + d * 0.35);
    g.gain.exponentialRampToValueAtTime(0.0001, t + d);
    osc.connect(g);
    g.connect(dest);
    osc.start(t);
    lfo.start(t);
    osc.stop(t + d + 0.05);
    lfo.stop(t + d + 0.05);
    osc.onended = function () { disconnectAll([osc, lfo, la, g]); };
  }

  // Per-sound output: volume + optional stereo pan, auto-disconnected later.
  function sfxOut(opts, life) {
    var g = makeGain(opts && opts.volume != null ? clamp(+opts.volume || 0, 0, 2) : 1);
    var nodes = [g];
    if (opts && opts.pan && ctx.createStereoPanner) {
      var p = ctx.createStereoPanner();
      p.pan.value = clamp(+opts.pan || 0, -1, 1);
      g.connect(p);
      p.connect(sfxBus);
      nodes.push(p);
    } else {
      g.connect(sfxBus);
    }
    setTimeout(function () { disconnectAll(nodes); }, (life + 0.5) * 1000);
    return g;
  }

  /* ------------------------------------------------------ sound effects */

  var SFX = {
    crash: [1.0, function (o, t) {
      noise(o, { t: t, d: 0.45, v: 0.7, type: 'lowpass', f: 4000, f2: 300, q: 0.7, a: 0.002 });
      noise(o, { t: t, d: 0.14, v: 0.4, type: 'highpass', f: 2500, q: 0.5, a: 0.001 });
      noise(o, { t: t + 0.07, d: 0.2, v: 0.35, type: 'bandpass', f: 1200, f2: 500, q: 1.2 });
      tone(o, { type: 'triangle', f: 180, f2: 55, t: t, d: 0.25, v: 0.5, a: 0.002 });
      var clank = [523, 787, 1187, 1663];
      for (var i = 0; i < clank.length; i++) {
        tone(o, { type: 'square', f: clank[i], t: t + 0.01 + i * 0.013, d: 0.22 + i * 0.04,
          v: 0.05, a: 0.001, filter: 'lowpass', ff: 3500 });
      }
      boing(o, t + 0.16, 0.65, 560, 130, 0.32);
    }],

    horn: [0.5, function (o, t) {
      var honk = function (t1, d, up) {
        var fs = [370, 466];
        for (var i = 0; i < fs.length; i++) {
          tone(o, { type: 'square', f: fs[i] * 0.88, f2: fs[i] * up, fT: 0.04, t: t1, d: d,
            v: 0.13, a: 0.01, hold: 0.8, filter: 'lowpass', ff: 1900, q: 1 });
        }
      };
      honk(t, 0.17, 1);
      honk(t + 0.23, 0.22, 1.03);
    }],

    pickup: [0.7, function (o, t) {
      var notes = [88, 92, 95, 100];
      for (var i = 0; i < notes.length; i++) {
        var f = mtof(notes[i]), tt = t + i * 0.055;
        tone(o, { type: 'sine', f: f, t: tt, d: 0.2, v: 0.22, a: 0.003 });
        tone(o, { type: 'square', f: f, t: tt, d: 0.12, v: 0.035, a: 0.003, filter: 'lowpass', ff: 5000 });
      }
      tone(o, { type: 'triangle', f: mtof(100), t: t + 0.22, d: 0.45, v: 0.1, a: 0.01, vib: 25, vibRate: 9 });
    }],

    nearmiss: [0.6, function (o, t) {
      noise(o, { t: t, d: 0.32, v: 0.35, a: 0.11, type: 'bandpass', f: 600, fpeak: 3200, peakAt: 0.4, f2: 900, q: 1.4 });
      tone(o, { type: 'sine', f: mtof(91), t: t + 0.14, d: 0.25, v: 0.12 });
      tone(o, { type: 'sine', f: mtof(96), t: t + 0.2, d: 0.32, v: 0.1 });
    }],

    boost: [1.0, function (o, t) {
      noise(o, { t: t, d: 0.85, v: 0.38, a: 0.22, hold: 0.45, type: 'bandpass', f: 300, f2: 4500, q: 1.5 });
      tone(o, { type: 'sawtooth', f: 110, f2: 520, t: t, d: 0.8, v: 0.12, a: 0.05, hold: 0.6,
        filter: 'lowpass', ff: 1500, q: 2 });
      tone(o, { type: 'sine', f: 880, f2: 1760, t: t + 0.05, d: 0.7, v: 0.05, a: 0.1, hold: 0.5 });
    }],

    flash: [0.4, function (o, t) {
      noise(o, { t: t, d: 0.025, v: 0.5, a: 0.001, type: 'highpass', f: 3500, q: 0.7 });
      noise(o, { t: t + 0.06, d: 0.035, v: 0.35, a: 0.001, type: 'highpass', f: 2800, q: 0.7 });
      tone(o, { type: 'square', f: 1976, t: t + 0.11, d: 0.14, v: 0.09, a: 0.003, hold: 0.7, filter: 'lowpass', ff: 6000 });
    }],

    beep: [0.25, function (o, t) {
      tone(o, { type: 'square', f: 660, t: t, d: 0.17, v: 0.15, a: 0.004, hold: 0.7, filter: 'lowpass', ff: 4000 });
    }],

    go: [0.6, function (o, t) {
      tone(o, { type: 'square', f: 1320, t: t, d: 0.5, v: 0.15, a: 0.004, hold: 0.75, filter: 'lowpass', ff: 5000 });
      tone(o, { type: 'sine', f: 660, t: t, d: 0.5, v: 0.12, a: 0.004, hold: 0.75 });
    }],

    finish: [2.2, function (o, t) {
      var lead = [[67, 0, 0.12], [72, 0.12, 0.12], [76, 0.24, 0.12], [79, 0.36, 0.28],
        [76, 0.66, 0.12], [79, 0.8, 1.15], [84, 0.8, 1.15]];
      for (var i = 0; i < lead.length; i++) {
        var n = lead[i];
        tone(o, { wave: pulseWave, type: 'square', f: mtof(n[0]), t: t + n[1], d: n[2], v: 0.11, a: 0.005,
          hold: 0.7, filter: 'lowpass', ff: 5000, vib: n[2] > 0.5 ? 18 : 0 });
      }
      var chord = [60, 64, 67, 48];
      for (var j = 0; j < chord.length; j++) {
        tone(o, { type: 'triangle', f: mtof(chord[j]), t: t + 0.8, d: 1.2, v: 0.12, a: 0.01, hold: 0.5 });
      }
      for (var k = 0; k < 4; k++) {
        noise(o, { t: t + 0.36 + k * 0.075, d: 0.08, v: 0.12, type: 'bandpass', f: 1900, q: 0.8 });
      }
      noise(o, { t: t + 0.8, d: 0.9, v: 0.1, type: 'highpass', f: 6000, q: 0.5 });
      tone(o, { type: 'sine', f: 120, f2: 45, t: t + 0.8, d: 0.2, v: 0.4 });
    }],

    gameover: [2.5, function (o, t) {
      var notes = [67, 66, 65, 64], start = [0, 0.42, 0.84, 1.26], dur = [0.38, 0.38, 0.38, 1.1];
      for (var i = 0; i < notes.length; i++) {
        var t1 = t + start[i], d = dur[i];
        var n = tone(o, { type: 'sawtooth', f: mtof(notes[i]), t: t1, d: d, v: 0.18, a: 0.03, hold: 0.7,
          filter: 'lowpass', ff: 400, q: 4, vib: i === 3 ? 40 : 0, vibRate: 5 });
        // "wah": filter opens then closes on each note
        n.filter.frequency.setValueAtTime(400, t1);
        n.filter.frequency.exponentialRampToValueAtTime(1700, t1 + Math.min(0.15, d * 0.4));
        n.filter.frequency.exponentialRampToValueAtTime(500, t1 + d);
      }
    }],

    click: [0.1, function (o, t) {
      tone(o, { type: 'sine', f: 950, f2: 620, t: t, d: 0.06, v: 0.4, a: 0.001 });
    }],

    bump: [0.25, function (o, t) {
      tone(o, { type: 'sine', f: 120, f2: 45, t: t, d: 0.16, v: 0.6, a: 0.002 });
      noise(o, { t: t, d: 0.08, v: 0.22, type: 'lowpass', f: 450, q: 0.8 });
    }],

    checkpoint: [0.6, function (o, t) {
      tone(o, { type: 'sine', f: mtof(83), t: t, d: 0.16, v: 0.18 });
      tone(o, { type: 'triangle', f: mtof(95), t: t, d: 0.12, v: 0.05 });
      tone(o, { type: 'sine', f: mtof(88), t: t + 0.12, d: 0.4, v: 0.2 });
      tone(o, { type: 'triangle', f: mtof(100), t: t + 0.12, d: 0.3, v: 0.05 });
    }],

    whoosh: [0.6, function (o, t) {
      noise(o, { t: t, d: 0.5, v: 0.3, a: 0.2, type: 'bandpass', f: 350, fpeak: 1500, peakAt: 0.45, f2: 300, q: 1.2 });
    }]
  };

  // Minimum spacing between two plays of the same sound (seconds).
  var MIN_GAP = { whoosh: 0.08, nearmiss: 0.12, bump: 0.08, crash: 0.25, pickup: 0.04 };

  function play(name, opts) {
    if (!ctx || !sfxOn || ctx.state === 'closed') return;
    var def = SFX[name];
    if (!def) return;
    var now = ctx.currentTime;
    var gap = MIN_GAP[name] || 0.03;
    if (lastPlay[name] != null && now - lastPlay[name] < gap && now >= lastPlay[name]) return;
    lastPlay[name] = now;
    var out = sfxOut(opts, def[0]);
    def[1](out, now + 0.005);
  }

  /* ------------------------------------------------------------- music */

  var PC = { C: 0, D: 2, E: 4, F: 5, G: 7, A: 9, B: 11 };

  function noteToMidi(s) {
    var m = /^([A-G])(#|b)?(-?\d)$/.exec(s);
    if (!m) return null;
    return 12 * (+m[3] + 1) + PC[m[1]] + (m[2] === '#' ? 1 : (m[2] === 'b' ? -1 : 0));
  }

  function parseChord(s) {
    var m = /^([A-G])(#|b)?(m)?$/.exec(s);
    if (!m) return { pc: 0, minor: false };
    var pc = (PC[m[1]] + (m[2] === '#' ? 1 : (m[2] === 'b' ? -1 : 0)) + 12) % 12;
    return { pc: pc, minor: !!m[3] };
  }

  // Distance (in steps) from step s to the next non-'.' char of a 16-step pattern.
  function gapAfter(pattern, s) {
    for (var k = 1; k <= 16; k++) {
      if (pattern.charAt((s + k) % 16) !== '.') return k;
    }
    return 16;
  }

  function compileSong(name, def) {
    var bars = def.lead.length, len = bars * 16, ev = [], i, s;
    for (i = 0; i < len; i++) ev.push([]);

    // Lead melody ("A4:2 -:4 ...", durations in 16th notes)
    for (var bi = 0; bi < bars; bi++) {
      var pos = bi * 16, toks = def.lead[bi].trim().split(/\s+/);
      for (i = 0; i < toks.length; i++) {
        var parts = toks[i].split(':'), d = +parts[1] || 1;
        if (parts[0] !== '-') {
          var m = noteToMidi(parts[0]);
          if (m == null) songIssues.push(name + ' bar ' + (bi + 1) + ': bad note ' + parts[0]);
          else if (pos < len) ev[pos].push({ v: 'lead', m: m, d: d });
        }
        pos += d;
      }
      if (pos !== (bi + 1) * 16) songIssues.push(name + ' bar ' + (bi + 1) + ': ' + (pos - bi * 16) + ' steps');
    }

    // Harmony: bass + chord stabs / arpeggio
    if (def.chords.length !== bars) songIssues.push(name + ': chords/lead bar count mismatch');
    for (bi = 0; bi < def.chords.length; bi++) {
      var halves = def.chords[bi].split(' ').map(parseChord);
      for (s = 0; s < 16; s++) {
        var ch = halves.length > 1 && s >= 8 ? halves[1] : halves[0];
        var third = ch.minor ? 3 : 4;
        var root = 36 + ch.pc;
        if (root < 40) root += 12;
        var at = bi * 16 + s;

        var b = def.bass.charAt(s);
        if (b !== '.') {
          var bm = b === 'R' ? root : b === 'F' ? (root + 7 <= 52 ? root + 7 : root - 5)
            : b === 'O' ? root + 12 : root + third;
          ev[at].push({ v: 'bass', m: bm, d: gapAfter(def.bass, s) });
        }

        var a = def.arp.charAt(s);
        if (a !== '.') {
          var base = 57 + ((ch.pc - 9 + 12) % 12);
          var tones = [base, base + third, base + 7, base + 12];
          if (a === 'c') ev[at].push({ v: 'stab', ms: [base + third, base + 7, base + 12] });
          else ev[at].push({ v: 'arp', m: tones[+a] || base, d: gapAfter(def.arp, s) });
        }
      }
    }

    // Drums
    for (bi = 0; bi < bars; bi++) {
      var dr = def.fillEvery && (bi + 1) % def.fillEvery === 0 ? def.fill : def.drums;
      for (s = 0; s < 16; s++) {
        var at2 = bi * 16 + s;
        if (dr.kick.charAt(s) === 'x') ev[at2].push({ v: 'kick' });
        var sn = dr.snare.charAt(s);
        if (sn === 'x') ev[at2].push({ v: 'snare', g: 1 });
        else if (sn === 'o') ev[at2].push({ v: 'snare', g: 0.4 });
        var hh = dr.hat.charAt(s);
        if (hh === 'x') ev[at2].push({ v: 'hat', g: 1 });
        else if (hh === 'o') ev[at2].push({ v: 'hat', g: 0.5 });
      }
    }

    return {
      name: name, bpm: def.bpm, len: len, ev: ev, leadWave: def.leadWave, leadVol: def.leadVol,
      bassVol: def.bassVol, arpVol: def.arpVol, drumVol: def.drumVol, echo: def.echo
    };
  }

  var SONG_DEFS = {
    // Bouncy cartoon chiptune, D major, oom-pah "farandole" feel.
    race: {
      bpm: 142, leadWave: 'pulse', leadVol: 0.15, bassVol: 0.22, arpVol: 0.04, drumVol: 1, echo: 0.2,
      lead: [
        'A4:2 D5:2 F#5:2 A5:2 F#5:2 D5:2 A4:4',
        'C#5:2 E5:2 A5:2 E5:2 C#5:2 E5:2 A4:4',
        'D5:3 E5:1 F#5:2 G5:2 A5:4 F#5:4',
        'G5:3 F#5:1 E5:2 D5:2 E5:3 F#5:1 E5:2 C#5:2',
        'A4:2 D5:2 F#5:2 A5:2 B5:2 A5:2 F#5:4',
        'G5:2 B5:2 D6:2 B5:2 G5:2 B5:2 D6:4',
        'C#6:3 B5:1 A5:2 G5:2 F#5:2 E5:2 C#5:4',
        'D5:4 A5:2 F#5:2 D5:4 -:4',
        'F#5:2 B5:2 D6:2 B5:2 F#5:2 B5:2 C#6:2 D6:2',
        'B5:3 A5:1 G5:2 B5:2 D6:4 B5:4',
        'A5:3 G5:1 F#5:2 A5:2 D6:4 A5:4',
        'E5:2 A5:2 C#6:2 A5:2 E6:4 C#6:4',
        'D6:2 C#6:2 B5:2 F#5:2 D6:2 C#6:2 B5:4',
        'G5:2 B5:2 D6:2 G6:2 F#6:2 E6:2 D6:4',
        'E6:3 D6:1 B5:2 G5:2 C#6:3 B5:1 A5:2 E5:2',
        'D6:4 A5:2 F#5:2 D5:2 A4:2 D5:4'
      ],
      chords: ['D', 'A', 'D', 'G A', 'D', 'G', 'A', 'D', 'Bm', 'G', 'D', 'A', 'Bm', 'G', 'Em A', 'D'],
      bass: 'R...F...R...F.O.',
      arp: '..c...c...c...c.',
      drums: { kick: 'x.......x.x.....', snare: '....x.......x...', hat: 'x.o.x.o.x.o.x.o.' },
      fill: { kick: 'x.......x.......', snare: '....x...x.x.xxxx', hat: 'x.o.x.o.x.o.....' },
      fillEvery: 8
    },
    // Calm, sunny, G major.
    menu: {
      bpm: 100, leadWave: 'triangle', leadVol: 0.24, bassVol: 0.18, arpVol: 0.07, drumVol: 0.6, echo: 0.25,
      lead: [
        'B4:4 D5:4 G5:6 F#5:2',
        'A5:4 F#5:4 D5:8',
        'E5:4 G5:4 B5:6 A5:2',
        'G5:4 E5:4 C5:8',
        'D5:4 G5:4 B5:4 A5:2 G5:2',
        'F#5:4 A5:4 D6:8',
        'E5:4 C6:4 D6:2 C6:2 A5:4',
        'G5:12 -:4',
        'B5:2 A5:2 G5:4 E5:8',
        'C6:2 B5:2 A5:4 E5:8',
        'D5:2 G5:2 B5:2 D6:2 B5:4 G5:4',
        'A5:6 F#5:2 D5:8',
        'E5:2 G5:2 B5:4 C6:2 B5:2 G5:4',
        'A5:2 G5:2 E5:4 G5:8',
        'A5:4 C6:4 B5:4 A5:4',
        'G5:8 D5:4 -:4'
      ],
      chords: ['G', 'D', 'Em', 'C', 'G', 'D', 'C D', 'G', 'Em', 'C', 'G', 'D', 'Em', 'C', 'Am D', 'G'],
      bass: 'R.......F.......',
      arp: '0.1.2.1.0.1.2.1.',
      drums: { kick: 'x.......x.......', snare: '................', hat: '..x...x...x...x.' },
      fill: { kick: 'x.......x.......', snare: '............o.o.', hat: '..x...x...x...x.' },
      fillEvery: 8
    }
  };

  var SONGS = {};
  for (var sn in SONG_DEFS) {
    if (Object.prototype.hasOwnProperty.call(SONG_DEFS, sn)) SONGS[sn] = compileSong(sn, SONG_DEFS[sn]);
  }

  var seq = { name: null, song: null, step: 0, loop: 0, next: 0, timer: null, out: null, lead: null, nodes: [] };

  function startSeq(name, step) {
    stopSeq();
    var song = SONGS[name];
    if (!song || !ctx) return;
    var now = ctx.currentTime;
    var out = makeGain(0, musicBus);
    out.gain.setValueAtTime(0, now);
    out.gain.linearRampToValueAtTime(1, now + 0.3);
    var lead = makeGain(1, out);
    // Light echo on the lead
    var dl = ctx.createDelay(1);
    dl.delayTime.value = (60 / song.bpm) * 0.75;
    var lp = makeFilter('lowpass', 2200, 0.7);
    var fb = makeGain(0.28, dl);
    var wet = makeGain(song.echo || 0.2, out);
    lead.connect(dl);
    dl.connect(lp);
    lp.connect(fb);
    lp.connect(wet);

    seq = {
      name: name, song: song, step: (step || 0) % song.len, loop: 0, next: now + 0.06,
      out: out, lead: lead, nodes: [out, lead, dl, lp, fb, wet], timer: null
    };
    seq.timer = setInterval(schedule, 25);
    schedule();
  }

  function stopSeq() {
    if (!seq.song) return;
    var old = seq;
    clearInterval(old.timer);
    if (ctx) {
      var now = ctx.currentTime;
      old.out.gain.cancelScheduledValues(now);
      old.out.gain.setValueAtTime(old.out.gain.value, now);
      old.out.gain.linearRampToValueAtTime(0, now + 0.25);
    }
    setTimeout(function () { disconnectAll(old.nodes); }, 1500);
    seq = { name: null, song: null, step: 0, loop: 0, next: 0, timer: null, out: null, lead: null, nodes: [] };
  }

  function schedule() {
    try {
      if (!ctx || !seq.song) return;
      var song = seq.song, spb = 60 / song.bpm / 4, now = ctx.currentTime, n = 0;
      if (seq.next < now - 0.2) seq.next = now + 0.03;   // timers were throttled: resync, no burst
      while (seq.next < now + lookahead && n++ < 512) {
        playStep(song, seq.step, seq.next, spb);
        seq.next += spb;
        seq.step++;
        if (seq.step >= song.len) { seq.step = 0; seq.loop++; }
      }
    } catch (e) { /* never break the game loop */ }
  }

  function playStep(song, step, t, spb) {
    var list = song.ev[step], alt = seq.loop % 2 === 1;
    for (var i = 0; i < list.length; i++) {
      var e = list[i];
      switch (e.v) {
        case 'lead': {
          var dur = e.d * spb;
          tone(seq.lead, {
            wave: song.leadWave === 'pulse' ? pulseWave : null,
            type: song.leadWave === 'pulse' ? 'square' : song.leadWave,
            f: mtof(e.m), t: t, d: dur * 0.92, v: song.leadVol, a: 0.008, hold: 0.6,
            filter: 'lowpass', ff: 4200, vib: dur > 0.3 ? 14 : 0
          });
          if (alt) {   // second pass: thicker lead, an octave below
            tone(seq.out, { type: 'triangle', f: mtof(e.m - 12), t: t, d: dur * 0.9, v: song.leadVol * 0.7, hold: 0.6 });
          }
          break;
        }
        case 'bass':
          tone(seq.out, { type: 'square', f: mtof(e.m), t: t, d: e.d * spb * 0.85, v: song.bassVol,
            a: 0.006, hold: 0.5, filter: 'lowpass', ff: 650, q: 1 });
          break;
        case 'stab':
          for (var k = 0; k < e.ms.length; k++) {
            tone(seq.out, { wave: pulseWave, type: 'square', f: mtof(e.ms[k]), t: t, d: spb * 0.9,
              v: song.arpVol, a: 0.003, filter: 'lowpass', ff: 2600 });
          }
          break;
        case 'arp':
          tone(seq.out, { type: 'triangle', f: mtof(e.m), t: t, d: e.d * spb * 0.95, v: song.arpVol, a: 0.004 });
          break;
        case 'kick':
          tone(seq.out, { type: 'sine', f: 160, f2: 45, fT: 0.12, t: t, d: 0.16, v: 0.45 * song.drumVol, a: 0.002 });
          break;
        case 'snare':
          noise(seq.out, { t: t, d: 0.12, v: 0.2 * e.g * song.drumVol, type: 'bandpass', f: 1900, q: 0.7, a: 0.001 });
          tone(seq.out, { type: 'triangle', f: 220, f2: 170, t: t, d: 0.07, v: 0.12 * e.g * song.drumVol, a: 0.001 });
          break;
        case 'hat':
          noise(seq.out, { t: t, d: 0.035, v: 0.06 * e.g * song.drumVol, type: 'highpass', f: 7500, q: 0.7, a: 0.001 });
          break;
      }
    }
    // Second pass of the race tune: extra 16th hats for drive
    if (alt && song.name === 'race' && step % 2 === 1) {
      noise(seq.out, { t: t, d: 0.025, v: 0.03, type: 'highpass', f: 8000, q: 0.7, a: 0.001 });
    }
  }

  function music(name) {
    wanted = name || null;
    if (!ctx) return;
    if (!wanted) { stopSeq(); return; }
    if (seq.name === wanted && seq.timer) return;
    if (musicOn && !paused) startSeq(wanted, 0);
  }

  /* --------------------------------------------------- state & lifecycle */

  function unlock() {
    var c = ensureContext();
    if (!c) return;
    if (c.state === 'suspended' && c.resume) {
      var p = c.resume();
      if (p && p.catch) p.catch(function () {});
    }
    if (!unlocked) {
      unlocked = true;
      // Silent buffer: fully wakes audio on some mobile browsers.
      var b = c.createBuffer(1, 1, 22050), s = c.createBufferSource();
      s.buffer = b;
      s.connect(c.destination);
      s.start(0);
    }
    if (wanted && musicOn && !paused && !seq.timer) startSeq(wanted, 0);
  }

  function update(dt, s) {
    if (!ctx) return;
    if (seq.timer) schedule();
    if (engine) updateEngine(+dt > 0 ? Math.min(+dt, 0.25) : 0.016, s || {});
  }

  function setMusicEnabled(on) {
    musicOn = !!on;
    savePrefs();
    if (!ctx) return;
    glide(musicBus.gain, musicOn && !paused ? MUSIC_VOL : 0, 0.05);
    if (!musicOn) stopSeq();
    else if (wanted && !paused && !seq.timer) startSeq(wanted, 0);
  }

  function setSfxEnabled(on) {
    sfxOn = !!on;
    savePrefs();
    if (!ctx) return;
    glide(sfxBus.gain, sfxOn ? SFX_VOL : 0, 0.03);
  }

  function pauseAll() {
    paused = true;
    if (!ctx) return;
    glide(engineBus.gain, 0, 0.04);
    glide(musicBus.gain, 0, 0.05);
    if (seq.song) resumeStep[seq.name] = seq.step;
    stopSeq();
  }

  function resumeAll() {
    paused = false;
    if (!ctx) return;
    if (ctx.state === 'suspended' && unlocked && ctx.resume) {
      var p = ctx.resume();
      if (p && p.catch) p.catch(function () {});
    }
    glide(engineBus.gain, 1, 0.08);
    glide(musicBus.gain, musicOn ? MUSIC_VOL : 0, 0.1);
    if (wanted && musicOn && !seq.timer) startSeq(wanted, resumeStep[wanted] || 0);
  }

  // Background tab / app: suspend the whole context to save battery.
  try {
    document.addEventListener('visibilitychange', function () {
      try {
        if (!ctx || !unlocked) return;
        if (document.hidden) {
          if (ctx.state === 'running' && ctx.suspend) ctx.suspend().catch(function () {});
        } else if (ctx.state === 'suspended' && ctx.resume) {
          ctx.resume().catch(function () {});
        }
      } catch (e) { /* ignore */ }
    });
  } catch (e) { /* no document */ }

  /* ---------------------------------------------------------- public API */

  function safe(fn, fallback) {
    return function () {
      try { return fn.apply(null, arguments); } catch (e) { return fallback; }
    };
  }

  window.AudioFX = {
    unlock: safe(unlock),
    update: safe(update),
    play: safe(play),
    music: safe(music),
    pauseAll: safe(pauseAll),
    resumeAll: safe(resumeAll),
    setMusicEnabled: safe(setMusicEnabled),
    setSfxEnabled: safe(setSfxEnabled),
    isMusicEnabled: function () { return musicOn; },
    isSfxEnabled: function () { return sfxOn; },
    isAvailable: function () { return !!(window.AudioContext || window.webkitAudioContext); },
    sounds: Object.keys(SFX),
    // Test hooks (not needed by the game)
    _debug: {
      context: function () { return ctx; },
      master: function () { return master; },
      setLookahead: function (s) { lookahead = s; },
      songIssues: function () { return songIssues.slice(); },
      songInfo: function () {
        var r = {};
        for (var k in SONGS) {
          if (Object.prototype.hasOwnProperty.call(SONGS, k)) {
            r[k] = { bars: SONGS[k].len / 16, seconds: SONGS[k].len * 60 / SONGS[k].bpm / 4 };
          }
        }
        return r;
      }
    }
  };
})();
