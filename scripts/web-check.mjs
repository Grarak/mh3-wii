// Runs the web build in headless Chromium with WebGPU and
// records it: the page's log, a canvas screenshot every --interval seconds and
// a heartbeat (the latest "MH3 stats" line) every minute.
//
//   node scripts/web-check.mjs --url http://localhost:8080/ --disc /disc/IMAGE \
//     --profile DIR --shots DIR [--args "--quest 19001 --area 1 --no-damage"] \
//     [--env MH3_STATS=1,...] [--nand DIR --data00 FILE] [--seconds 300] [--interval 20]
//     [--keys SECONDS:KEY,...] [--mouse SECONDS:ACTION,...]
//     [--options ID=VALUE,...] [--viewport WIDTHxHEIGHT] [--save DIR] [--prompt TEXT]
//     [--browser-args "FLAG ..."] [--page-shots 1] [--cdp URL]
//
// --args and --env reach the game from a development build's page only
// (MH3_DEVELOPMENT=ON scripts/build-web.sh, build/web-dev); a release page
// always opens the launcher. --profile keeps the browser profile, and with it
// the imported disc image, between runs. --nand and --data00 stage a NAND save before the start. --keys presses keys (Playwright
// key names, docs/vita-controls.md for the mapping; KeyQ+KeyE holds Q and presses E;
// down:KeyQ and up:KeyQ hold Q across other actions)
// that many seconds after it; --mouse's
// actions: click (the picture's middle, which locks the pointer for mouse
// look in the game), click:FX:FY, right:FX:FY and move:FX:FY (at fractions of
// the canvas, for the launcher), wheel:DY (pixels) or DX:DY (the locked
// mouse's motion in pixels). --options sets the page's options before the start (a
// checkbox's ID=1 or 0, the camera speed's sensitivity=1.5), --viewport the
// window's size (default 1000x900). --save imports a save folder through the
// page's own picker; --prompt answers the launcher's text prompts (other
// dialogs are accepted). Without --args the launcher's menus start: --keys
// drives them. --browser-args adds Chromium flags. The log takes the frame
// counter's text (--options fps=1) with each screenshot; --page-shots 1 also
// saves the page's own screenshot (its elements over the canvas, which stays
// blank there) as page-N.png. --cdp attaches to a browser already running
// with --remote-debugging-port (a desktop browser on another machine, through
// an SSH tunnel) instead of starting headless Chromium: its window and flags
// stay as started, --profile is not needed, and the shots are the page's own
// screenshots (the canvas shows there; no renderer read-back). Needs
// Playwright (npm install
// playwright; npx playwright install chromium); PLAYWRIGHT_DIR names the
// directory it is installed in when that is not on the module path.
import { createRequire } from 'module';
import fs from 'fs';
import path from 'path';

const require = createRequire(process.env.PLAYWRIGHT_DIR ? path.join(process.env.PLAYWRIGHT_DIR, 'x.js') : import.meta.url);
const { chromium } = require('playwright');
const options = { url: 'http://localhost:8080/', seconds: '300', interval: '20', args: '', env: '', viewport: '1000x900' };

for (let i = 2; i + 1 < process.argv.length; i += 2)
  options[process.argv[i].replace(/^--/, '')] = process.argv[i + 1];

for (const required of ['disc', 'shots', ...(options.cdp ? [] : ['profile'])])
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
const attached = options.cdp ? await chromium.connectOverCDP(options.cdp) : null;
const context = attached ? attached.contexts()[0] : await chromium.launchPersistentContext(options.profile, {
  headless: true,
  viewport: { width: Number(options.viewport.split('x')[0]), height: Number(options.viewport.split('x')[1]) },
  // SwiftShader GL for the compositor keeps the device alive across canvas
  // presents in headless Chromium (Vulkan compositing loses it).
  args: ['--enable-unsafe-webgpu', '--enable-features=Vulkan,WebGPU', '--ignore-gpu-blocklist', '--use-angle=swiftshader', '--autoplay-policy=no-user-gesture-required',
    ...(options['browser-args'] || '').split(' ').filter(Boolean)],
});
// Whole stacks in page errors (an abort's allocation site is deep).
await context.addInitScript(() => { Error.stackTraceLimit = 100; });
const page = attached ? await context.newPage() : context.pages()[0] || await context.newPage();
page.on('console', (message) => {
  const text = message.text();
  logFile.write(`${elapsed()} ${text}\n`);

  if (text.includes('MH3 stats'))
    latestStats = text;

  if (!failure && /runtime stopped|Aborted|RuntimeError|WebGPU error|validation error|device lost/i.test(text))
    failure = text;
});
page.on('dialog', async (dialog) => {
  logFile.write(`${elapsed()} dialog ${dialog.type()}: ${dialog.message()}\n`);
  await dialog.accept(dialog.type() === 'prompt' ? (options.prompt ?? dialog.defaultValue()) : undefined);
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

// The page's own import, as a user picks a folder.
if (options.save) {
  await page.setInputFiles('#save-folder', options.save);
  await page.waitForFunction(() => /Save imported|Save not imported|custom quest/.test(document.getElementById('status').textContent));
  console.log(`${elapsed()}s ${await page.textContent('#status')} ${await page.textContent('#save-status')}`);
}

for (const entry of (options.options || '').split(',').filter(Boolean)) {
  const [id, value] = entry.split('=');
  await page.evaluate(([which, setting]) => {
    const input = document.getElementById(which);

    if (input.type === 'checkbox')
      input.checked = setting === '1';
    else
      input.value = setting;

    input.dispatchEvent(new Event('input'));
  }, [id, value]);
}

await page.click('#start');
const started = Date.now();
const keys = (options.keys || '').split(',').filter(Boolean).map((entry) => {
  const [at, ...key] = entry.split(':');
  return { at: Number(at), key: key.join(':') };
});
const mouse = (options.mouse || '').split(',').filter(Boolean).map((entry) => {
  const [at, ...action] = entry.split(':');
  return { at: Number(at), action };
});
let pointer = null;
let shot = 0, lastBeat = Date.now(), nextShot = Number(options.interval);

while (elapsed() < Number(options.seconds)) {
  await page.waitForTimeout(500);
  const since = (Date.now() - started) / 1000;

  // Scripted key presses on the canvas, held for 150 ms; KEY+KEY holds the
  // first while it presses the second, down:KEY and up:KEY hold one across
  // other actions.
  while (keys.length && keys[0].at <= since) {
    const { key } = keys.shift();
    const [hold, held] = key.split(':');

    if (held) {
      await page.keyboard[hold === 'up' ? 'up' : 'down'](held);
      logFile.write(`${elapsed()} key ${key}\n`);
      continue;
    }

    const chord = key.split('+');
    await page.focus('#canvas');

    for (const part of chord) {
      await page.keyboard.down(part);
      await page.waitForTimeout(150);
    }

    for (const part of chord.reverse())
      await page.keyboard.up(part);

    logFile.write(`${elapsed()} key ${key}\n`);
  }

  // Clicks, moves and the wheel at points of the canvas, or locked motion in
  // 10 moves.
  while (mouse.length && mouse[0].at <= since) {
    const { action } = mouse.shift();
    const box = await page.locator('#canvas').boundingBox();
    pointer = pointer || { x: box.x + box.width / 2, y: box.y + box.height / 2 };

    if (['click', 'right', 'move'].includes(action[0])) {
      const at = action.length > 2 ? { x: box.x + Number(action[1]) * box.width, y: box.y + Number(action[2]) * box.height } : pointer;
      await page.mouse.move(at.x, at.y, { steps: 4 });

      if (action[0] !== 'move') {
        const button = action[0] === 'right' ? 'right' : 'left';
        await page.mouse.down({ button });
        await page.waitForTimeout(100);
        await page.mouse.up({ button });
      }
    } else if (action[0] === 'wheel')
      await page.mouse.wheel(0, Number(action[1]));
    else
      // Under the pointer lock headless Chromium reports the protocol's moves
      // as no movement: dispatch pointer events with the movement instead.
      for (let step = 1; step <= 10; ++step) {
        await page.evaluate(([dx, dy]) => document.getElementById('canvas').dispatchEvent(new PointerEvent('pointermove', {
          bubbles: true, pointerType: 'mouse', isPrimary: true, movementX: dx, movementY: dy })),
        [Number(action[0]) / 10, Number(action[1]) / 10]);
        await page.waitForTimeout(30);
      }

    const locked = await page.evaluate(() => document.pointerLockElement?.id || 'none');
    logFile.write(`${elapsed()} mouse ${action.join(' ')} (pointer lock: ${locked})\n`);
  }

  if (since < nextShot)
    continue;

  nextShot += Number(options.interval);

  // The renderer's own read-back: headless Chromium does not show the canvas.
  // A desktop browser does, so there the page's screenshot is the shot.
  const file = path.join(options.shots, `shot-${String(++shot).padStart(3, '0')}-${elapsed()}s.png`);
  const url = attached ? null : await page.evaluate(() => Promise.race([window.mh3CaptureFrame(), new Promise((resolve) => setTimeout(() => resolve(null), 10000))]));

  if (url)
    fs.writeFileSync(file, Buffer.from(url.split(',')[1], 'base64'));
  else if (attached)
    await page.screenshot({ path: file });

  const counter = await page.evaluate(() => {
    const element = document.getElementById('fps-counter');
    return element && !element.hidden ? element.textContent : null;
  });

  if (counter)
    logFile.write(`${elapsed()} frame counter: ${counter}\n`);

  if (options['page-shots'] === '1')
    await page.screenshot({ path: path.join(options.shots, `page-${String(shot).padStart(3, '0')}-${elapsed()}s.png`) });

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

// An attached browser keeps running: only the run's page closes.
if (attached) {
  await page.close();
  await attached.close();
} else
  await context.close();
logFile.end();
