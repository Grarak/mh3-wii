// The web build's loader (docs/optimizations/platforms/web-build.md). Copies the user's disc image into
// the Origin Private File System once, then starts MH3.js, whose game thread
// reads it through WasmFS at /opfs/mh3.
//
// Query parameters: disc=URL fetches the image from the serving host instead of
// the file picker; args=... are the program's arguments (as on the command
// line); env=NAME=VALUE,... sets environment variables; autostart=1 starts
// without the button once an image is present.
'use strict';

const query = new URLSearchParams(location.search);
const statusLine = document.getElementById('status');
const logBox = document.getElementById('log');
const startButton = document.getElementById('start');
const discInput = document.getElementById('disc');
const argsInput = document.getElementById('args');
const imagePattern = /\.(rvz|wia|iso)$/i;
const logLines = [];

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

  // The first start records the image; later ones reuse disc/.
  const args = splitArguments(argsInput.value);

  if (!(await exists(directory, 'disc/rom.txt')))
    args.push('--rom', `/opfs/mh3/${image}`);

  const env = {};

  for (const pair of (query.get('env') || '').split(',').filter(Boolean)) {
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
  status('Running');
  document.getElementById('canvas').focus();
}

async function setup() {
  if (!crossOriginIsolated) {
    status('This page needs cross-origin isolation (SharedArrayBuffer): serve it with the COOP/COEP headers of scripts/serve-web.py.');
    return;
  }

  if (!navigator.gpu || !(await navigator.gpu.requestAdapter())) {
    status('This browser has no WebGPU adapter.');
    return;
  }

  argsInput.value = query.get('args') || '';
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
