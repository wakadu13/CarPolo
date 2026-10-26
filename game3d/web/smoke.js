// Headless browser smoke test of the web build:  node web/smoke.js <dir-with-index.html> [out.png]
// Serves the directory, opens index.html?test (autopilot) in Chromium with software WebGL and waits
// for the "TEST: done" line the game prints. Exit code 0 on success.
const http = require('http'), fs = require('fs'), path = require('path');
const { chromium } = require('playwright');

const dir = path.resolve(process.argv[2] || 'build-web-bin');
const shot = process.argv[3] || null;
const types = { '.html': 'text/html', '.js': 'application/javascript', '.wasm': 'application/wasm', '.data': 'application/octet-stream', '.png': 'image/png' };

const server = http.createServer((req, res) => {
  let p = decodeURIComponent(req.url.split('?')[0]);
  if (p === '/') p = '/index.html';
  const file = path.join(dir, p);
  if (!file.startsWith(dir) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) { res.writeHead(404); res.end(); return; }
  res.writeHead(200, { 'Content-Type': types[path.extname(file)] || 'application/octet-stream' });
  fs.createReadStream(file).pipe(res);
});

(async () => {
  await new Promise(r => server.listen(0, '127.0.0.1', r));
  const url = `http://127.0.0.1:${server.address().port}/index.html?test`;
  const browser = await chromium.launch({ args: ['--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist', '--autoplay-policy=no-user-gesture-required'] });
  const page = await browser.newPage({ viewport: { width: 960, height: 540 } });
  let done = null, lines = [], seen = new Set();
  const verbose = !!process.env.SMOKE_VERBOSE, maxMs = (parseInt(process.env.SMOKE_SECONDS || '900') * 1000);
  page.on('console', m => { const t = m.text(); lines.push(t); if (t.includes('TEST: done')) done = t;
    const key = t.replace(/[0-9.]+/g, '#').slice(0, 120);
    if (/ERROR|TEST:|TEXTURES|SCENE:|GL: OpenGL|Renderer|Version|WebGL|INVALID|warning/i.test(t) && !seen.has(key)) { seen.add(key); console.log(t.slice(0, 220)); }
    else if (verbose && !seen.has(key)) { seen.add(key); console.log(t.slice(0, 160)); } });
  page.on('pageerror', e => { console.log('PAGE ERROR: ' + e.message); });
  await page.goto(url);
  const t0 = Date.now();
  while (!done && Date.now() - t0 < maxMs) {
    await page.waitForTimeout(2000);
    if (shot && Date.now() - t0 > 20000 && !fs.existsSync(shot)) await page.screenshot({ path: shot });
  }
  if (shot) await page.screenshot({ path: shot.replace(/\.png$/, '_end.png') });
  await browser.close();
  server.close();
  if (!done && process.env.SMOKE_SECONDS) { console.log('stopped early (SMOKE_SECONDS)'); process.exit(0); }
  if (!done) { console.log('SMOKE FAILED: no "TEST: done" after timeout; last lines:\n' + lines.slice(-15).join('\n')); process.exit(1); }
  console.log('SMOKE OK: ' + done);
})();
