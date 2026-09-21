/* The emulator's own thread.
 *
 * The guest is a real program: it sits in its own loops waiting for the mouse
 * and the keyboard, and it needs tens of millions of instructions just to put
 * its opening screen up.  A slice small enough to keep a page responsive is too
 * small to get it anywhere, so the clock lives here instead -- the worker runs
 * flat out and posts the screen when it has changed.  The page only forwards
 * input, which is why it stays smooth while the guest is busy.
 *
 * The guest's disk lives here too.  It is emscripten's in-memory filesystem,
 * with the JW_CAD distribution baked in under `orig/` at build time, and it is
 * writable: when the guest saves a drawing the bytes land in `orig/` like any
 * other file.  That is what the page's upload and download talk to -- a file
 * from the visitor's machine is written into `orig/` before the guest is
 * booted on it, and a file the guest wrote is read back out.  Nothing else
 * would do: the program opens and saves through DOS, and DOS is this.
 */
/* The stamp the page put on this file's own URL, passed on to the two it
   loads -- otherwise a new worker would pull an old .js and .wasm. */
const DE_BUILD = (() => {
  /* `self` is the worker's own global; the checks run this file in node,
     where there is none, so it falls back to no stamp. */
  try {
    return (self.location.search.match(/[?&]v=([^&]*)/) || [])[1] || '0';
  } catch (err) {
    return '0';
  }
})();
importScripts(DE_BUILD === '0' ? 'dosemu.js' : 'dosemu.js?v=' + DE_BUILD);

const ROOT = 'orig';

let Module = null;
let running = false;
/* Each boot gets a number.  A turn that belongs to an older one stops: without
 * it a reset leaves the previous chain of setTimeouts running as well and the
 * guest is stepped twice per turn. */
let gen = 0;
/* Instructions per turn.  Big while the guest is still starting -- it has
 * forty million to get through before it puts anything up and there is nothing
 * to look at meanwhile -- and smaller once it is drawing, so the screen keeps
 * up with the pointer. */
const BOOT_SLICE = 8000000, RUN_SLICE = 1000000;
let booted = false;
let lastSent = 0;
/* Its own clock.  Sharing lastSent with the throttle below meant the
 * listing was never sent again after the boot: a frame goes up every
 * 250ms or so and resets lastSent, so `now - lastSent > 2000` was
 * almost never true and a drawing the guest had just saved never
 * turned up in the page's list. */
let lastList = 0;
/* Which drawing the guest was booted on -- the page shows it as the one
 * selected, and it is the one a download takes. */
let current = '';

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
  Module._de_run(booted ? RUN_SLICE : BOOT_SLICE);
  const fb = Module._de_frame();
  const msg = { booted: !!Module._de_graphics() };
  const wasBooted = booted;
  booted = msg.booted;
  if (Module._de_dead()) {
    msg.message = Module.UTF8ToString(Module._de_message());
    msg.console = Module.UTF8ToString(Module._de_console());
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
  /* The disk is only worth re-listing once the guest is running and only now
   * and then: the guest writes a drawing when the visitor asks it to, and the
   * page's list of what it could download has to notice. */
  const now = Date.now();
  if (booted && (!wasBooted || now - lastList > 2000)) {
    lastList = now;
    msg.files = listing();
    msg.current = current;
  }
  if (msg.frame || msg.files || now - lastSent > 250 || msg.message) {
    lastSent = now;
    postMessage(msg, msg.frame ? [msg.frame] : []);
  }
  /* setTimeout rather than a tight loop: the worker has to come back to its
   * message queue between turns or the page's input never arrives. */
  setTimeout(() => turn(mine), 0);
}

/* The drawings on the guest's disk, by name.  Only the drawings: the page
 * offers these to open and to download, and the program, its help file and
 * the fonts are not the visitor's to take. */
function listing() {
  const out = [];
  try {
    for (const name of Module.FS.readdir(ROOT)) {
      let size = 0;
      try { size = Module.FS.stat(ROOT + '/' + name).size; } catch (err) { continue; }
      const plot = isPlot(name, size);

      if (!plot && !/\.JWC$/i.test(name)) continue;
      out.push({ name, size, plot });
    }
  } catch (err) {
    return [];
  }
  out.sort((a, b) => a.name.localeCompare(b.name));
  return out;
}

/* Is this file what the program's plotter output left behind?
 *
 * **Not by its name.**  JW_CAD writes the name it was given, exactly --
 * answer `PLOT` at the 出力ファイル名 ? prompt and the file on the disk is
 * `PLOT`, with no extension at all.  Going by `.PLT` meant the file never
 * showed up in the list and the PDF/PNG buttons stayed grey, which is what
 * a visitor hit first.  So look at what is in it: plot/WASM.JWP makes the
 * plotter start with a bounding box -- `B` and four numbers.
 *
 * The answer is cached per name+size, because the listing runs every two
 * seconds and the guest's disk does not change nearly that often. */
const plotSeen = new Map();

function isPlot(name, size) {
  const key = name + ':' + size;
  const had = plotSeen.get(key);

  if (had !== undefined) return had;
  let yes = false;

  try {
    const st = Module.FS.open(ROOT + '/' + name, 'r');
    const head = new Uint8Array(48);
    const n = Module.FS.read(st, head, 0, head.length, 0);

    Module.FS.close(st);
    const line = String.fromCharCode(...head.subarray(0, n)).split(/[\r\n]/)[0];

    yes = /^B(\s+-?\d+){4}\s*$/.test(line);
  } catch (err) {
    yes = false;
  }
  plotSeen.set(key, yes);
  return yes;
}

function boot(drawing) {
  current = drawing;
  const n = Module.lengthBytesUTF8(drawing) + 1;
  const p = Module._malloc(n);
  Module.stringToUTF8(drawing, p, n);
  Module._de_boot(p);
  Module._free(p);
  /* **The guest's clock has to run here.**  入出力 → ②ﾌﾟﾛｯﾀ paces its
     output by the time of day and waits for ever while INT 21h/2Ch keeps
     answering 12:00:00.  It stands still by default because every screen
     the repo is measured against was taken that way; a person at the page
     is not comparing screens, so the page turns it on.  The number is
     instructions to a hundredth of a second. */
  if (Module._de_clock) Module._de_clock(20000);
  previous = null;
  booted = false;
  running = true;
  const mine = ++gen;
  setTimeout(() => turn(mine), 0);
}

/* **No drawing.**  Started with no file on its command line, JW_CAD draws
   its menu, its counts and an empty sheet and waits -- so that is how the
   page starts it.  Opening one for the visitor made the list at the top
   look like a statement about what was on the screen, which it is not.
   Opening a drawing is 入出力 → ①ﾌｧｲﾙ → ②読込, inside the program. */
let pendingBoot = '';

onmessage = e => {
  const m = e.data;
  if (m.boot !== undefined) {
    if (Module) boot(m.boot);
    else pendingBoot = m.boot;
    return;
  }
  /* A file from the visitor's machine, written onto the guest's disk under the
   * name DOS will see.  JW_CAD's own file list shows whatever is there, so an
   * uploaded drawing can be opened from inside the program as well as booted
   * into. */
  if (m.upload) {
    const done = [];
    for (const f of m.upload) {
      try {
        Module.FS.writeFile(ROOT + '/' + f.name, new Uint8Array(f.buf));
        done.push(f.name);
      } catch (err) {
        postMessage({ message: f.name + ' を書けませんでした: ' + err });
      }
    }
    postMessage({ files: listing(), current, uploaded: done });
    return;
  }
  if (m.download) {
    try {
      const bytes = Module.FS.readFile(ROOT + '/' + m.download);
      const buf = bytes.buffer.slice(bytes.byteOffset,
                                     bytes.byteOffset + bytes.byteLength);
      postMessage({ file: { name: m.download, buf, as: m.as } }, [buf]);
    } catch (err) {
      postMessage({ message: m.download + ' が読めませんでした' });
    }
    return;
  }
  if (m.list) { postMessage({ files: listing(), current }); return; }
  queue.push(m);
};

/* The .wasm is fetched by dosemu.js, by name, so it wants the stamp too. */
createDosemu({
  /* Only in a browser: under node there is no stamp and no cache, and
     `dosemu.wasm?v=0` is a file name that does not exist. */
  locateFile: (f) => (DE_BUILD === '0' ? f : f + '?v=' + DE_BUILD),
}).then(mod => {
  Module = mod;
  boot(pendingBoot);
  /* The list goes up before the guest has drawn anything: if it refuses to
   * start there will never be a running frame to hang it off, and the page
   * still has to offer the other drawings. */
  postMessage({ files: listing(), current });
});
