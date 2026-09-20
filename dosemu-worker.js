/* The emulator's own thread.
 *
 * The guest is a real program: it sits in its own loops waiting for the mouse
 * and the keyboard, and it needs tens of millions of instructions just to put
 * its opening screen up.  A slice small enough to keep a page responsive is too
 * small to get it anywhere, so the clock lives here instead -- the worker runs
 * flat out and posts the screen when it has changed.  The page only forwards
 * input, which is why it stays smooth while the guest is busy.
 */
importScripts('dosemu.js');

let Module = null;
let running = false;
/* Each boot gets a number.  A turn that belongs to an older one stops: without
 * it a reset leaves the previous chain of setTimeouts running as well and the
 * guest is stepped twice per turn. */
let gen = 0;
/* Instructions per turn.  Big while the guest is still starting -- it has
 * forty million to get through before it puts anything up and there is nothing
 * to look at meanwhile -- and smaller once it is drawing, so the screen keeps
 * up with the pointer.  `turbo` raises both. */
const BOOT_SLICE = 8000000, RUN_SLICE = 1000000;
let fast = false;
let booted = false;
let lastSent = 0;

/* Input arrives while a slice is running, so it is queued and handed over
 * between slices -- the guest reads the mouse and the keyboard through the
 * interrupts and cannot be poked at mid-instruction. */
const queue = [];

function pump() {
  while (queue.length) {
    const m = queue.shift();
    if (m.mods !== undefined) Module._de_mods(m.mods);
    if (m.mouse) Module._de_mouse(m.mouse[0], m.mouse[1]);
    if (m.button !== undefined) Module._de_button(m.button, m.down);
    if (m.keys) for (const k of m.keys) Module._de_key(k);
  }
}

/* "Has the screen changed" -- a whole-frame comparison against the last one
 * sent.  A sampled hash was tried first and missed almost everything: a line
 * a few hundred pixels long is nothing among 307,200, so the page saw one
 * picture and then nothing.  Comparing every word costs about a millisecond
 * and stops at the first difference, which is far less than the slice it
 * follows. */
let previous = null;
function changed(px, n) {
  if (!previous || previous.length !== n) { previous = new Uint8Array(px); return true; }
  for (let i = 0; i < n; i += 4) {
    if (previous[i] !== px[i] || previous[i + 1] !== px[i + 1]
        || previous[i + 2] !== px[i + 2]) {
      previous.set(px);
      return true;
    }
  }
  return false;
}

function turn(mine) {
  if (!running || !Module || mine !== gen) return;
  pump();
  const insns = Module._de_run((booted ? RUN_SLICE : BOOT_SLICE) * (fast ? 4 : 1));
  const fb = Module._de_frame();
  const msg = { insns, booted: !!Module._de_graphics() };
  booted = msg.booted;
  if (Module._de_dead()) {
    msg.message = Module.UTF8ToString(Module._de_message());
    running = false;
  }
  if (fb) {
    const w = Module._de_width(), h = Module._de_height();
    const n = w * h * 4;
    const px = Module.HEAPU8.subarray(fb, fb + n);
    if (changed(px, n)) {
      msg.frame = new Uint8Array(px).buffer;    // the heap can move; copy out
      msg.width = w;
      msg.height = h;
    }
  }
  const now = Date.now();
  if (msg.frame || now - lastSent > 250 || msg.message) {
    lastSent = now;
    const con = Module.UTF8ToString(Module._de_console());
    if (con) msg.console = con;
    postMessage(msg, msg.frame ? [msg.frame] : []);
  }
  /* setTimeout rather than a tight loop: the worker has to come back to its
   * message queue between turns or the page's input never arrives. */
  setTimeout(() => turn(mine), 0);
}

function boot(drawing) {
  const n = Module.lengthBytesUTF8(drawing) + 1;
  const p = Module._malloc(n);
  Module.stringToUTF8(drawing, p, n);
  Module._de_boot(p);
  Module._free(p);
  previous = null;
  booted = false;
  running = true;
  const mine = ++gen;
  setTimeout(() => turn(mine), 0);
}

let pendingBoot = 'SAMPLE0.JWC';

onmessage = e => {
  const m = e.data;
  if (m.turbo !== undefined) { fast = !!m.turbo; return; }
  if (m.boot !== undefined) {
    if (Module) boot(m.boot);
    else pendingBoot = m.boot;
    return;
  }
  queue.push(m);
};

createDosemu().then(mod => {
  Module = mod;
  boot(pendingBoot);
});
