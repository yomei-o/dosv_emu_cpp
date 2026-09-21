/* The page's three file controls, driven headlessly.
 *
 *     node tools/barcheck.mjs
 *
 * index.html's script is pulled out and run against a small stand-in for the
 * document and the worker, and then put through what a visitor does:
 *
 *   1  a drawing appears on the guest's disk (they saved it in JW_CAD)
 *      -- it is marked ● and becomes the one the button will take
 *   2  ダウンロード asks the worker for **that** name
 *   3  choosing another name does **not** ask the worker to boot anything
 *      (the page never opens a drawing -- 入出力 does)
 *   4  アップロード sends the bytes and still does not boot
 *
 * The third is the one this exists for.  The list started life doubling as
 * "which drawing to start on", so choosing a name to download rebooted the
 * emulator and threw the drawing away.
 */
import { readFileSync } from 'node:fs';
import vm from 'node:vm';

const html = readFileSync('index.html', 'utf8');
const script = html.slice(html.indexOf('<script>') + 8,
                          html.lastIndexOf('</script>'));

const posted = [];
const listeners = new Map();

const anchors = [];
function el(id) {
  const o = {
    id, value: '', textContent: '', disabled: false, hidden: false,
    options: [], files: [],
    append(c) { this.options.push(c); },
    addEventListener(k, f) { listeners.set(id + ':' + k, f); },
    focus() { doc.activeElement = this; },
    getBoundingClientRect: () => ({ left: 0, top: 0, width: 640, height: 480 }),
    getContext: () => ({
      fillRect() {}, putImageData() {}, createImageData() {},
      set fillStyle(v) {},
    }),
    style: {}, classList: { add() {}, remove() {} },
  };
  return o;
}

const els = {};
for (const id of ['screen', 'status', 'console', 'pick', 'up', 'down',
                  'pdf', 'png', 'ime']) {
  els[id] = el(id);
}
const body = { children: [], append(a) { this.children.push(a); } };
const doc = {
  activeElement: null,
  body,
  getElementById: id => els[id],
  createElement: tag => {
    const a = { tag, style: {}, clicked: 0, href: '', download: '',
                click() { this.clicked++; }, remove() { a.removed = true; } };
    if (tag === 'a') anchors.push(a);
    return a;
  },
  addEventListener() {},
};
globalThis.document = doc;
globalThis.window = { addEventListener() {} };
globalThis.ImageData = class { constructor() {} };
globalThis.TextDecoder = class { decode() { return '�'; } };
globalThis.performance = { now: () => Date.now() };
let revoked = 0;
globalThis.URL = {
  createObjectURL: () => 'blob:x',
  revokeObjectURL() { revoked++; },
};
globalThis.Blob = class { constructor() {} };
globalThis.Worker = class {
  constructor() { this.onmessage = null; }
  postMessage(m) { posted.push(m); }
};

vm.runInThisContext(script, { filename: 'index.html#script' });

const fire = (k, arg) => {
  const f = listeners.get(k);
  if (!f) throw new Error('no listener for ' + k);
  return f(arg);
};
const worker = globalThis.__worker;
let bad = 0;
const ok = (cond, what) => {
  console.log((cond ? '  ok   ' : '  FAIL ') + what);
  if (!cond) bad++;
};

/* The shipped drawings, then one more appearing: a save inside JW_CAD. */
worker.onmessage({ data: { files: [{ name: 'SAMPLE0.JWC', size: 1 },
                                   { name: 'TEST1.JWC', size: 1 }] } });
posted.length = 0;
worker.onmessage({ data: { files: [{ name: 'MYWORK.JWC', size: 2 },
                                   { name: 'SAMPLE0.JWC', size: 1 },
                                   { name: 'TEST1.JWC', size: 1 }] } });

ok(els.pdf.disabled === true && els.png.disabled === true,
   'PDF and PNG are dead while there is no plotter output');

const marked = els.pick.options.filter(o => o.textContent.startsWith('●'));
ok(marked.length === 1 && marked[0].value === 'MYWORK.JWC',
   'a drawing that was not there at the start is marked ●');
ok(els.pick.value === 'MYWORK.JWC',
   'and it becomes the one the button will take');
ok(els.down.disabled === false, 'ダウンロード is live once there is a file');
ok(!posted.some(m => m.boot), 'a new drawing appearing does not boot anything');

posted.length = 0;
fire('down:click');
ok(posted.length === 1 && posted[0].download === 'MYWORK.JWC',
   'ダウンロード asks for the name in the list');
ok(doc.activeElement === els.ime, 'and the keyboard goes back to the guest');

posted.length = 0;
els.pick.value = 'TEST1.JWC';
fire('pick:change');
ok(!posted.some(m => m.boot), 'choosing another name does NOT boot');
fire('down:click');
ok(posted.some(m => m.download === 'TEST1.JWC'),
   'and ダウンロード follows the choice');

posted.length = 0;
els.up.files = [{ name: 'other.jwc', arrayBuffer: async () => new ArrayBuffer(4) }];
await fire('up:change');
await new Promise(r => setTimeout(r, 10));
ok(posted.some(m => m.upload && m.upload[0].name === 'OTHER.JWC'),
   'アップロード sends the bytes under a name DOS can spell');
ok(!posted.some(m => m.boot), 'and does not boot either');

/* The download itself.  Both of these were wrong once and neither shows up
   on the machine it was written on: a detached <a> does not download in
   every browser, and revoking the object URL in the same turn as the click
   cancels the download where the browser has not started reading yet. */
posted.length = 0;
anchors.length = 0;
revoked = 0;
worker.onmessage({ data: { file: { name: 'MYWORK.JWC', buf: new ArrayBuffer(8) } } });
ok(anchors.length === 1 && anchors[0].download === 'MYWORK.JWC',
   'the download makes an <a download="the name">');
ok(body.children.length === 1 && body.children[0] === anchors[0],
   'and puts it in the document before clicking (a detached one does nothing)');
ok(anchors[0].clicked === 1, 'and clicks it');
ok(revoked === 0, 'and does NOT revoke the object URL in the same turn');
await new Promise(r => setTimeout(r, 30));
ok(revoked === 0, 'nor a moment later -- the browser is still reading it');

/* The plotter's output.  **The name tells you nothing**: 入出力 → ②ﾌﾟﾛｯﾀ →
   ③ﾌｧｲﾙ出力 writes exactly the name it was given, so answering `PLOT` leaves
   a file called `PLOT` with no extension at all.  The worker says which
   files are plotter output by reading them; the page must go by that flag
   and not by `.PLT`, or the buttons stay grey and the visitor is told to
   press something that cannot be pressed. */
posted.length = 0;
worker.onmessage({ data: { files: [{ name: 'MYWORK.JWC', size: 2 },
                                   { name: 'PLOT', size: 900, plot: true },
                                   { name: 'SAMPLE0.JWC', size: 1 }] } });
ok(els.pdf.disabled === false && els.png.disabled === false,
   'PDF and PNG come alive when plotter output appears, extension or not');
const plotted = els.pick.options.filter(o => o.value === 'PLOT');
ok(plotted.length === 1 && plotted[0].textContent.includes('プロッタ出力'),
   'and the list says which one it is');

posted.length = 0;
els.pick.value = 'MYWORK.JWC';
fire('pick:change');
fire('pdf:click');
ok(posted.some(m => m.download === 'PLOT' && m.as === 'pdf'),
   'PDF takes the plotter output even when a drawing is the one picked');
posted.length = 0;
fire('png:click');
ok(posted.some(m => m.download === 'PLOT' && m.as === 'png'), 'and so does PNG');

/* What the guest was booted on is what the list should show -- not whatever
   sorts first.  This is how AUTO.JWC came to look like the opening drawing. */
worker.onmessage({ data: { current: 'SAMPLE2.JWC',
                           files: [{ name: 'AAA.JWC', size: 1 },
                                   { name: 'SAMPLE2.JWC', size: 1 }] } });
els.pick.value = '';
worker.onmessage({ data: { current: 'SAMPLE2.JWC',
                           files: [{ name: 'AAA.JWC', size: 1 },
                                   { name: 'SAMPLE2.JWC', size: 1 }] } });
ok(els.pick.value === 'SAMPLE2.JWC',
   'the list shows the drawing the guest actually has open');

process.exit(bad ? 1 : 0);
