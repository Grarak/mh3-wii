// The web build's loader. Copies the user's disc image into
// the Origin Private File System once, then starts MH3.js, whose game thread
// reads it through WasmFS at /opfs/mh3.
//
// Query parameters: disc=URL fetches the image from the serving host instead of
// the file picker; autostart=1 starts without the button once an image is
// present. Development builds also take args=... (the program's arguments, as
// on the command line, instead of the launcher's menus) and
// env=NAME=VALUE,... (environment variables over the page's options), for
// scripts/web-check.mjs. The page also imports saves and custom quests.
'use strict';

// The build's configuration (runtime/web/web.cmake).
const developmentBuild = @MH3_WEB_DEVELOPMENT@;
const query = new URLSearchParams(location.search);
const statusLine = document.getElementById('status');
const logBox = document.getElementById('log');
const startButton = document.getElementById('start');
const discInput = document.getElementById('disc');
const imagePattern = /\.(rvz|wia|iso)$/i;
const logLines = [];

// The page's options, kept in this browser's storage (when it allows it).
const settingsKey = 'mh3-settings';
const checkboxes = ['fill', 'native', 'ultrawide', 'invert', 'fps'];
const sensitivityInput = document.getElementById('sensitivity');
const sensitivityValue = document.getElementById('sensitivity-value');

function loadSettings() {
  let saved = {};

  try {
    saved = JSON.parse(localStorage.getItem(settingsKey)) || {};
  } catch {
    saved = {};
  }

  for (const id of checkboxes)
    if (typeof saved[id] === 'boolean')
      document.getElementById(id).checked = saved[id];

  if (typeof saved.sensitivity === 'number')
    sensitivityInput.value = saved.sensitivity;

  sensitivityValue.textContent = Number(sensitivityInput.value).toFixed(2);
}

function saveSettings() {
  const saved = { sensitivity: Number(sensitivityInput.value) };

  for (const id of checkboxes)
    saved[id] = document.getElementById(id).checked;

  sensitivityValue.textContent = saved.sensitivity.toFixed(2);

  try {
    localStorage.setItem(settingsKey, JSON.stringify(saved));
  } catch {
    // Private windows and blocked storage: the options last for this visit.
  }
}

// The options as the runtime's environment.
function settingsEnvironment() {
  const env = {};
  const checked = (id) => document.getElementById(id).checked;

  if (checked('fill'))
    env.MH3_WEB_FILL = '1';

  if (checked('native'))
    env.MH3_RESOLUTION = 'native';

  if (checked('ultrawide'))
    env.MH3_ASPECT = 'window';

  if (checked('invert'))
    env.MH3_MOUSE_INVERT_Y = '1';

  env.MH3_MOUSE_SENSITIVITY = String(Number(sensitivityInput.value));
  return env;
}

function status(text) {
  statusLine.textContent = text;
}

function log(text) {
  console.log(text);
  logLines.push(text);

  if (logLines.length > 400)
    logLines.splice(0, logLines.length - 400);

  logBox.textContent = logLines.join('\n');
  logBox.scrollTop = logBox.scrollHeight;
}

async function storage() {
  const root = await navigator.storage.getDirectory();
  return root.getDirectoryHandle('mh3', { create: true });
}

// The name of the disc image already in storage, if any.
async function storedImage(directory) {
  for await (const [name, handle] of directory.entries())
    if (handle.kind === 'file' && imagePattern.test(name))
      return name;

  return null;
}

async function exists(directory, path) {
  try {
    let handle = directory;
    const parts = path.split('/');

    for (const part of parts.slice(0, -1))
      handle = await handle.getDirectoryHandle(part);

    await handle.getFileHandle(parts[parts.length - 1]);
    return true;
  } catch {
    return false;
  }
}

// Streams `source` (a ReadableStream of `total` bytes) into storage as `name`,
// replacing any other image.
async function importImage(directory, name, source, total) {
  for await (const [other, handle] of directory.entries())
    if (handle.kind === 'file' && imagePattern.test(other) && other !== name)
      await directory.removeEntry(other);

  const file = await directory.getFileHandle(name, { create: true });
  const writable = await file.createWritable();
  let copied = 0, shown = -1;
  const counter = new TransformStream({
    transform(chunk, controller) {
      copied += chunk.byteLength;
      const percent = total ? Math.floor(copied * 100 / total) : 0;

      if (percent !== shown) {
        shown = percent;
        status(`Copying the disc image into browser storage: ${percent}%`);
      }

      controller.enqueue(chunk);
    },
  });
  await source.pipeThrough(counter).pipeTo(writable);
  status(`Disc image stored (${(copied / 1048576).toFixed(0)} MiB)`);
}

// Writes `bytes` to `path` below /opfs/mh3 (tests stage saves with it).
async function importFile(path, bytes) {
  let directory = await storage();
  const parts = path.split('/');

  for (const part of parts.slice(0, -1))
    directory = await directory.getDirectoryHandle(part, { create: true });

  const file = await directory.getFileHandle(parts[parts.length - 1], { create: true });
  const writable = await file.createWritable();
  await writable.write(bytes);
  await writable.close();
}

window.mh3ImportFile = importFile;

// The game's save as on the Wii's NAND (docs/optimizations/system/saves-nand.md):
// its title's data directory holds the banner and, in nocopy/, the system
// file, the network data and six save files of three characters each. Custom
// quests sit beside the NAND.
const saveDirectory = 'user/nand/title/00010000/524d4850/data';
const saveFileSize = 0x14000, smallFileSize = 0x4000;
const saveStatus = document.getElementById('save-status');

// Where an imported file goes below /opfs/mh3, or null: the save's own files
// by name, from any folder but another game's (Dolphin's
// Wii/title/00010000/524d4850, this port's saves/nand/title/...), and custom
// quests.
function saveTarget(file) {
  const parts = (file.webkitRelativePath || file.name).split('/');
  const name = parts[parts.length - 1];
  const kind = parts.indexOf('title'), title = parts.indexOf('00010000');

  if (name.endsWith('.mh3quest'))
    return `user/custom-quests/${name}`;

  if (kind >= 0 && kind + 1 < parts.length - 1 && parts[kind + 1] !== '00010000')
    return null;

  if (title >= 0 && title + 1 < parts.length - 1 && parts[title + 1].toLowerCase() !== '524d4850')
    return null;

  if (/^data0[0-5]$/.test(name) || name === 'system' || name === 'network')
    return `${saveDirectory}/nocopy/${name}`;

  return name === 'banner.bin' ? `${saveDirectory}/${name}` : null;
}

// The characters of a save file (ValidSave in launcher_support.cpp): the file
// in use with its byte sum right, and each character's record filled.
function saveCharacters(bytes) {
  if (bytes.length !== saveFileSize)
    return 0;

  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  let sum = 0;

  for (let i = 8; i < bytes.length; ++i)
    sum += bytes[i];

  if (!(view.getUint32(0) & 1) || sum !== view.getUint32(4))
    return 0;

  let characters = 0;

  for (let character = 0; character < 3; ++character)
    if (view.getUint32(0x48 + character * 0x6000))
      ++characters;

  return characters;
}

async function directoryAt(path, create = false) {
  let directory = await storage();

  for (const part of path.split('/'))
    directory = await directory.getDirectoryHandle(part, { create });

  return directory;
}

// The save in this browser: its characters, by file.
async function storedSave() {
  const files = [];
  let characters = 0;

  try {
    const nocopy = await directoryAt(`${saveDirectory}/nocopy`);

    for (let index = 0; index < 6; ++index) {
      try {
        const file = await (await nocopy.getFileHandle(`data0${index}`)).getFile();
        const count = saveCharacters(new Uint8Array(await file.arrayBuffer()));

        if (count)
          files.push(`file ${index + 1}`);

        characters += count;
      } catch {
        // No such save file.
      }
    }
  } catch {
    // No save yet.
  }

  return { characters, files };
}

async function showSave() {
  const save = await storedSave();
  saveStatus.textContent = save.characters
    ? `The save in this browser: ${save.characters} character${save.characters > 1 ? 's' : ''} (${save.files.join(', ')}).`
    : 'No save in this browser yet: import one, or create a character in the game.';
}

// Imports the picked files: a save replaces the one in this browser (after
// asking, when that has characters), custom quests are added.
async function importSave(picked) {
  const found = new Map();

  for (const file of picked) {
    const target = saveTarget(file);

    if (!target)
      continue;

    if (found.has(target))
      throw new Error(`Two files would be ${target.split('/').slice(-1)[0]}: pick one game's save folder`);

    found.set(target, file);
  }

  const save = [...found.keys()].filter((target) => target.startsWith(saveDirectory));
  const quests = [...found.keys()].filter((target) => !target.startsWith(saveDirectory));
  const contents = new Map();
  let characters = 0;

  for (const target of save) {
    const name = target.split('/').slice(-1)[0];
    const bytes = new Uint8Array(await found.get(target).arrayBuffer());
    const size = name.startsWith('data') ? saveFileSize : name === 'banner.bin' ? 0 : smallFileSize;

    if (size && bytes.length !== size)
      throw new Error(`${name} has ${bytes.length} bytes, not the ${size} of Monster Hunter Tri's`);

    characters += name.startsWith('data') ? saveCharacters(bytes) : 0;
    contents.set(target, bytes);
  }

  if (!save.length && !quests.length)
    throw new Error('No save files (data00 to data05, system) or custom quests among them');

  if (save.length && !contents.has(`${saveDirectory}/nocopy/system`))
    throw new Error('The save has no system file: pick the whole save folder');

  if (save.length && !characters)
    throw new Error('None of the save files has a character');

  if (save.length) {
    const stored = await storedSave();

    if (stored.characters && !confirm(`Replace the save in this browser (${stored.characters} characters) with the imported one (${characters})?`))
      return;

    try {
      const title = await directoryAt(saveDirectory.split('/').slice(0, -1).join('/'));
      await title.removeEntry('data', { recursive: true });
    } catch {
      // Nothing to replace.
    }
  }

  for (const target of quests)
    contents.set(target, new Uint8Array(await found.get(target).arrayBuffer()));

  for (const [target, bytes] of contents)
    await importFile(target, bytes);

  await showSave();
  const added = quests.length ? ` and ${quests.length} custom quest${quests.length > 1 ? 's' : ''}` : '';
  status(save.length ? `Save imported (${characters} characters${added})` : `Imported ${quests.length} custom quest${quests.length > 1 ? 's' : ''}`);
}

// The frame counter (a Display option, F2 toggles it): the frames the browser
// showed each second, and the game's own rate where it makes more than are
// shown (Unlimited, or a cap above the display's refresh rate). SDL's fill
// mode hides everything but the canvas in a hidden element, so the counter
// sits directly in the body, in the canvas's top right corner.
const fpsInput = document.getElementById('fps');
const fpsCounter = document.createElement('div');
fpsCounter.id = 'fps-counter';
fpsCounter.hidden = true;
let fpsSample = null;

function updateFrameCounter(module) {
  if (!fpsInput.checked) {
    fpsCounter.hidden = true;
    fpsSample = null;
    return;
  }

  const sample = { time: performance.now(), shown: module._mh3_web_frames_shown(), made: module._mh3_web_frames_made() };

  if (fpsSample) {
    const seconds = (sample.time - fpsSample.time) / 1000;
    const shown = Math.round(((sample.shown - fpsSample.shown) >>> 0) / seconds);
    const made = Math.round(((sample.made - fpsSample.made) >>> 0) / seconds);
    fpsCounter.textContent = made > shown + 1 ? `${shown} FPS (game ${made})` : `${shown} FPS`;

    if (fpsCounter.parentNode !== document.body)
      document.body.appendChild(fpsCounter);

    const box = document.getElementById('canvas').getBoundingClientRect();
    fpsCounter.style.top = `${Math.max(box.top, 0) + 8}px`;
    fpsCounter.style.right = `${Math.max(document.documentElement.clientWidth - box.right, 0) + 8}px`;
    fpsCounter.hidden = false;
  }

  fpsSample = sample;
}

window.addEventListener('keydown', (event) => {
  if (event.code !== 'F2' || event.repeat)
    return;

  event.preventDefault();
  fpsInput.checked = !fpsInput.checked;
  saveSettings();
}, true);

function splitArguments(text) {
  return (text.match(/"[^"]*"|\S+/g) || []).map((part) => part.replace(/^"|"$/g, ''));
}

async function start() {
  startButton.disabled = true;
  const directory = await storage();
  const image = await storedImage(directory);

  if (!image) {
    status('No disc image in browser storage');
    return;
  }

  // The launcher's menus come first, as on the Vita (a development build's
  // args replace them). The first start records the image; later ones reuse
  // disc/.
  const args = developmentBuild ? splitArguments(query.get('args') || '') : [];

  if (!args.length)
    args.push('--launcher');

  if (!(await exists(directory, 'disc/rom.txt')))
    args.push('--rom', `/opfs/mh3/${image}`);

  const env = settingsEnvironment();

  for (const pair of (developmentBuild && query.get('env') || '').split(',').filter(Boolean)) {
    const equals = pair.indexOf('=');
    env[pair.slice(0, equals)] = pair.slice(equals + 1);
  }

  document.getElementById('setup').hidden = true;
  status(`Starting: MH3 ${args.join(' ')}`);
  window.mh3Exited = null;
  let captured = null;
  const module = await createMH3({
    canvas: document.getElementById('canvas'),
    arguments: args,
    preRun: [(m) => Object.assign(m.ENV, env)],
    print: log,
    printErr: log,
    onExit: (code) => {
      window.mh3Exited = code;
      status(`MH3 exited (${code})`);
    },
    // The renderer's read-back frames as PNG data URLs (mh3CaptureFrame).
    onFrameCapture: (width, height, pixels) => {
      const canvas = document.createElement('canvas');
      canvas.width = width;
      canvas.height = height;
      canvas.getContext('2d').putImageData(new ImageData(new Uint8ClampedArray(pixels.buffer), width, height), 0, 0);

      if (captured)
        captured(canvas.toDataURL('image/png'));

      captured = null;
    },
  });

  // The next presented frame, read back from the renderer (the canvas itself
  // may stay empty in a headless browser).
  window.mh3CaptureFrame = () => new Promise((resolve) => {
    captured = resolve;
    module._aurora_webgpu_capture_frame();
  });
  window.mh3Module = module;
  setInterval(() => updateFrameCounter(module), 500);

  // Mouse look: a click on the picture locks the pointer (Alt held keeps it
  // free for the Wii Remote's pointer), and the runtime turns the camera only
  // while the lock holds (Esc drops it). Not while the launcher's menus take
  // the mouse (SetMouseLook sets mh3MouseLook).
  const canvas = document.getElementById('canvas');
  let lockMoves = 0;
  canvas.addEventListener('mousedown', (event) => {
    if (!event.altKey && module.mh3MouseLook !== false && document.pointerLockElement !== canvas)
      canvas.requestPointerLock();
  });
  document.addEventListener('pointerlockchange', () => {
    const locked = document.pointerLockElement === canvas;
    lockMoves = locked ? 1 : 0;
    module._mh3_web_pointer_locked(locked ? 1 : 0);
  });
  // Chromium reports the cursor's jump into the lock as movement (minus its
  // position on the page), on the first move after the lock engages and on
  // some moves after it: such moves never reach SDL's canvas listener.
  window.addEventListener('pointermove', (event) => {
    if (document.pointerLockElement !== canvas)
      return;

    const jump = (event.movementX || event.movementY) && event.movementX === -Math.round(event.clientX) &&
      event.movementY === -Math.round(event.clientY);

    if (lockMoves || jump) {
      lockMoves = 0;
      event.stopImmediatePropagation();
    }
  }, true);
  status('Running');
  canvas.focus();
}

// Pthreads need SharedArrayBuffer, which only a cross-origin isolated page
// has. Where the host sends no COOP/COEP headers, the service worker
// isolation.js adds them, and the page reloads once under it.
async function isolate() {
  if (!window.isSecureContext || !navigator.serviceWorker || navigator.serviceWorker.controller)
    return false;

  status('Enabling cross-origin isolation');
  await navigator.serviceWorker.register('isolation.js');
  await navigator.serviceWorker.ready;
  location.reload();
  return true;
}

async function setup() {
  if (!crossOriginIsolated) {
    if (!(await isolate()))
      status('This page needs cross-origin isolation (SharedArrayBuffer): serve it over HTTPS or from localhost, with service workers allowed.');

    return;
  }

  // As the runtime asks: the compatibility level admits GPUs that only it
  // fits (WebGPU on OpenGL ES, such as the Raspberry Pi 5's).
  if (!navigator.gpu || !(await navigator.gpu.requestAdapter({ featureLevel: 'compatibility', powerPreference: 'high-performance' }))) {
    status('This browser has no WebGPU adapter. On Linux, start Chrome with --enable-unsafe-webgpu.');
    return;
  }

  loadSettings();

  for (const id of [...checkboxes, 'sensitivity'])
    document.getElementById(id).addEventListener('input', saveSettings);

  const directory = await storage();
  const url = query.get('disc');

  if (url) {
    const name = decodeURIComponent(url.split('/').pop().split('?')[0]);

    if ((await storedImage(directory)) !== name) {
      const response = await fetch(url);

      if (!response.ok)
        throw new Error(`Cannot fetch ${url}: ${response.status}`);

      await importImage(directory, name, response.body, Number(response.headers.get('Content-Length')));
    }
  }

  discInput.addEventListener('change', async () => {
    const file = discInput.files[0];

    if (!file)
      return;

    startButton.disabled = true;
    await importImage(directory, file.name, file.stream(), file.size);
    startButton.disabled = false;
  });

  for (const id of ['save-folder', 'save-files']) {
    const input = document.getElementById(id);
    input.addEventListener('change', () => {
      importSave([...input.files]).catch((error) => status(`Save not imported: ${error.message}`)).finally(() => {
        input.value = '';
      });
    });
  }

  await showSave();
  startButton.addEventListener('click', () => start().catch((error) => status(String(error))));
  const image = await storedImage(directory);

  if (image) {
    status(`Disc image in browser storage: ${image}`);
    startButton.disabled = false;

    if (query.get('autostart') === '1')
      await start();
  } else
    status('Choose your disc image');
}

setup().catch((error) => status(String(error)));
