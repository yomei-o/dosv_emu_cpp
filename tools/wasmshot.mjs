/* Replay an emulator script against the **browser build** and write the screen.
 *
 *     node tools/wasmshot.mjs scripts/drawing.txt SAMPLE0.JWC tmp/wasm.raw
 *
 * The point is to compare it with the native build's picture of the same
 * script, byte for byte: the two front ends share every line of the emulator,
 * so the screens have to be identical, and if they ever are not it is the
 * front end that is wrong rather than the guest.
 *
 * Only the steps a screen comparison needs are here -- wait/run, mouse, down,
 * up, click, key, type, shot, end -- with the same meaning src/main.cpp gives
 * them: the clock is the instruction counter, not the wall clock.  One step is
 * this runner's own: `getfile GUEST.JWC local/path` takes a file off the
 * guest's disk, the way the page's ダウンロード does.
 */
import { readFileSync, writeFileSync } from 'node:fs';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const createDosemu = require('../dosemu.js');

const BUTTON = { l: 0, left: 0, L: 0, r: 1, right: 1, R: 1, m: 2, middle: 2, M: 2 };

/* The key words `key` takes, as src/main.cpp's key_word() has them: the scan
 * code in the high byte and the character in the low one. */
const KEYS = {
  enter: 0x1C0D, esc: 0x011B, escape: 0x011B, tab: 0x0F09, bs: 0x0E08,
  space: 0x3920, up: 0x4800, down: 0x5000, left: 0x4B00, right: 0x4D00,
  home: 0x4700, end: 0x4F00, pgup: 0x4900, pgdn: 0x5100,
  ins: 0x5200, del: 0x5300, xfer: 0xB200, nfer: 0xB300,
};
for (let i = 1; i <= 12; i++) KEYS['f' + i] = (0x3A + i) << 8;

function keyWord(word) {
  const w = word.trim();
  if (w in KEYS) return KEYS[w];
  if (/^x[0-9a-fA-F]{1,4}$/.test(w)) return parseInt(w.slice(1), 16);
  if (w.length === 1) return w.charCodeAt(0);
  throw new Error('unknown key ' + w);
}

const script = readFileSync(process.argv[2], 'utf8');
const drawing = process.argv[3] || 'SAMPLE0.JWC';
const out = process.argv[4] || 'tmp/wasm.raw';

const Module = await createDosemu();
const n = Module.lengthBytesUTF8(drawing) + 1, p = Module._malloc(n);
Module.stringToUTF8(drawing, p, n);
if (!Module._de_boot(p)) {
  console.error('boot failed:', Module.UTF8ToString(Module._de_message()));
  process.exit(1);
}
Module._free(p);

/* A `wait` is not a step but the clock between two of them, exactly as
 * run_script() has it. */
let clock = 0;
const at = [];
for (const raw of script.split(/\r?\n/)) {
  const line = raw.replace(/#.*/, '').trim();
  if (!line) continue;
  const sp = line.search(/\s/);
  const op = sp < 0 ? line : line.slice(0, sp);
  const arg = sp < 0 ? '' : line.slice(sp).trim();
  if (op === 'wait' || op === 'run') clock += parseInt(arg, 10);
  else at.push([clock, op, arg]);
}

let done = 0;
function fire(insns) {
  while (done < at.length && at[done][0] <= insns) {
    const [, op, arg] = at[done++];
    if (op === 'mouse') {
      const [x, y] = arg.split(/\s+/).map(Number);
      Module._de_mouse(x, y);
    } else if (op === 'down' || op === 'up' || op === 'click') {
      const b = BUTTON[arg.trim()];
      if (b === undefined) throw new Error('unknown button ' + arg);
      if (op !== 'up') Module._de_button(b, 1);
      if (op !== 'down') Module._de_button(b, 0);
    } else if (op === 'key') {
      Module._de_key(keyWord(arg));
    } else if (op === 'type') {
      for (const ch of arg) Module._de_key(ch.charCodeAt(0));
    } else if (op === 'shot') {
      save(arg);
    } else if (op === 'getfile') {
      /* `getfile GUEST.JWC local/path` -- take a file off the guest's disk,
       * which is what index.html's ダウンロード does.  It is here rather than
       * in the native runner because only the browser build has a disk that
       * lives in memory; the native one writes into a real directory and the
       * file is simply there afterwards. */
      const [from, to] = arg.split(/\s+/);
      writeFileSync(to, Buffer.from(Module.FS.readFile('orig/' + from)));
      console.error('got ' + from + ' -> ' + to);
    } else if (op === 'end') {
      done = at.length;
      return true;
    }
  }
  return false;
}

function save(path) {
  const fb = Module._de_frame();
  if (!fb) { console.error('no graphics screen for', path); return; }
  const w = Module._de_width(), h = Module._de_height();
  writeFileSync(path, Buffer.from(Module.HEAPU8.subarray(fb, fb + w * h * 4)));
  console.error('wrote ' + path + '  ' + w + 'x' + h);
}

/* One step at a time would be honest and far too slow; the slice is small
 * enough that no scripted event lands more than that many instructions late,
 * which is what the native runner does as well. */
const SLICE = 100000;
let insns = 0;
while (done < at.length) {
  const want = at[done][0];
  while (insns < want && !Module._de_dead()) insns = Module._de_run(SLICE);
  if (Module._de_dead()) break;
  if (fire(insns)) break;
}
if (Module._de_dead()) {
  console.error('stopped:', Module.UTF8ToString(Module._de_message()));
}
if (out && !script.includes('shot ')) save(out);
