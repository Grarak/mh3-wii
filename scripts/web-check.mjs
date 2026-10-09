// Runs the web build (docs/optimizations/platforms/web-build.md) in headless Chromium with WebGPU and
// records it: the page's log, a canvas screenshot every --interval seconds and
// a heartbeat (the latest "MH3 stats" line) every minute.
//
//   node scripts/web-check.mjs --url http://localhost:8080/ --disc /disc/IMAGE \
//     --profile DIR --shots DIR [--args "--quest 19001 --area 1 --no-damage"] \
//     [--env MH3_STATS=1,...] [--nand DIR --data00 FILE] [--seconds 300] [--interval 20]
//     [--keys SECONDS:KEY,...]
//
// --profile keeps the browser profile, and with it the imported disc image,
// between runs. --nand and --data00 stage a NAND save before the start. --keys presses keys (Playwright
// key names, docs/vita-controls.md for the mapping) that many seconds after it. Needs Playwright (npm install
// playwright; npx playwright install chromium); PLAYWRIGHT_DIR names the
// directory it is installed in when that is not on the module path.
import { createRequire } from 'module';
import fs from 'fs';
import path from 'path';

const require = createRequire(process.env.PLAYWRIGHT_DIR ? path.join(process.env.PLAYWRIGHT_DIR, 'x.js') : import.meta.url);
const { chromium } = require('playwright');
const options = { url: 'http://localhost:8080/', seconds: '300', interval: '20', args: '', env: '' };

for (let i = 2; i + 1 < process.argv.length; i += 2)
  options[process.argv[i].replace(/^--/, '')] = process.argv[i + 1];

for (const required of ['disc', 'profile', 'shots'])
  if (!options[required])
    throw new Error(`--${required} is required`);

fs.mkdirSync(options.shots, { recursive: true });
const logPath = path.join(options.shots, 'page.log');
const logFile = fs.createWriteStream(logPath);
let latestStats = '';
let failure = '';
const start = Date.now();
const elapsed = () => Math.round((Date.now() - start) / 1000);

// WebGPU on Linux needs Vulkan; without a GPU Chromium uses SwiftShader.
const context = await chromium.launchPersistentContext(options.profile, {
  headless: true,
  viewport: { width: 1000, height: 900 },
  // SwiftShader GL for the compositor keeps the device alive across canvas
  // presents in headless Chromium (Vulkan compositing loses it).
  args: ['--enable-unsafe-webgpu', '--enable-features=Vulkan,WebGPU', '--ignore-gpu-blocklist', '--use-angle=swiftshader', '--autoplay-policy=no-user-gesture-required'],
});
const page = context.pages()[0] || await context.newPage();
page.on('console', (message) => {
  const text = message.text();
  logFile.write(`${elapsed()} ${text}\n`);

  if (text.includes('MH3 stats'))
    latestStats = text;

  if (!failure && /runtime stopped|Aborted|RuntimeError|WebGPU error|validation error|device lost/i.test(text))
    failure = text;
});
page.on('pageerror', (error) => {
  logFile.write(`${elapsed()} pageerror ${error.stack || error.message}\n`);
  failure = failure || error.message;
});
const query = new URLSearchParams({ disc: options.disc, args: options.args, env: options.env });
await page.goto(`${options.url}?${query}`);
await page.waitForFunction(() => /Disc image in browser storage|needs|no WebGPU/.test(document.getElementById('status').textContent), null, { timeout: 30 * 60 * 1000 });
console.log(`${elapsed()}s ${await page.textContent('#status')}`);

const upload = async (target, file) => {
  const bytes = [...fs.readFileSync(file)];
  await page.evaluate(([where, data]) => window.mh3ImportFile(where, new Uint8Array(data)), [target, bytes]);
};

if (options.nand) {
  const walk = (directory) => fs.readdirSync(directory, { withFileTypes: true }).flatMap((entry) =>
    entry.isDirectory() ? walk(path.join(directory, entry.name)) : [path.join(directory, entry.name)]);

  for (const file of walk(options.nand))
    await upload(`user/nand/${path.relative(options.nand, file)}`, file);

  if (options.data00)
    await upload('user/nand/title/00010000/524d4850/data/nocopy/data00', options.data00);

  console.log(`${elapsed()}s staged the NAND save from ${options.nand}`);
}

await page.click('#start');
const started = Date.now();
const keys = (options.keys || '').split(',').filter(Boolean).map((entry) => {
  const [at, key] = entry.split(':');
  return { at: Number(at), key };
});
let shot = 0, lastBeat = Date.now(), nextShot = Number(options.interval);

while (elapsed() < Number(options.seconds)) {
  await page.waitForTimeout(500);
  const since = (Date.now() - started) / 1000;

  // Scripted key presses on the canvas, held for 150 ms.
  while (keys.length && keys[0].at <= since) {
    const { key } = keys.shift();
    await page.focus('#canvas');
    await page.keyboard.down(key);
    await page.waitForTimeout(150);
    await page.keyboard.up(key);
    logFile.write(`${elapsed()} key ${key}\n`);
  }

  if (since < nextShot)
    continue;

  nextShot += Number(options.interval);

  // The renderer's own read-back: headless Chromium does not show the canvas.
  const file = path.join(options.shots, `shot-${String(++shot).padStart(3, '0')}-${elapsed()}s.png`);
  const url = await page.evaluate(() => Promise.race([window.mh3CaptureFrame(), new Promise((resolve) => setTimeout(() => resolve(null), 10000))]));

  if (url)
    fs.writeFileSync(file, Buffer.from(url.split(',')[1], 'base64'));

  if (Date.now() - lastBeat >= 60000) {
    console.log(`HB t=${elapsed()}s ${latestStats.slice(0, 160)}`);
    lastBeat = Date.now();
  }

  const exited = await page.evaluate(() => window.mh3Exited);

  if (exited !== null && exited !== undefined) {
    console.log(`${elapsed()}s MH3 exited (${exited})`);
    break;
  }
}

if (failure)
  console.log(`ERR ${failure.slice(0, 300)}`);

console.log(`EXIT t=${elapsed()}s, ${shot} screenshots and the log in ${options.shots}`);
await context.close();
logFile.end();
