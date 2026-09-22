/* Does the page's file list notice a drawing that appears while the guest is
 * running?
 *
 *     node tools/listcheck.mjs
 *
 * This runs **dosemu-worker.js itself**, with just enough of a Worker around
 * it for node, and watches the messages it posts.  The worker is given a new
 * file on the guest's disk once it is up -- which is what a save inside
 * JW_CAD amounts to -- and has to send a listing that contains it.
 *
 * It did not, until 2026-09-21: one timestamp was doing two jobs, the frame
 * throttle resetting the clock the listing interval was measured against, so
 * after the boot the list was never sent again.  The visitor saved a drawing
 * and could not then pick it.
 *
 * The same day, the plotter's output: 入出力 → ②ﾌﾟﾛｯﾀ → ③ﾌｧｲﾙ出力 writes the
 * name it was given and nothing else, so answering `PLOT` leaves a file
 * called `PLOT`.  The listing went by `.PLT`, so it never appeared and the
 * PDF/PNG buttons stayed grey.  Both files are planted here now, and the
 * plotter one has to come back marked `plot`.
 *
 * And on 2026-09-23 the same hole for DXF: 入出力 → ①ファイル → ⑥ＤＸＦ →
 * ① 保存 writes `NAME.dxf`, the listing matched `.JWC` alone, and a file the
 * visitor had just written was not in the list to download.
 */
import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import vm from 'node:vm';

const require = createRequire(import.meta.url);

const posted = [];
globalThis.importScripts = () => {
  const real = require('../dosemu.js');
  /* The worker keeps its Module to itself; this is the one seam where it can
   * be caught, before the worker ever calls it. */
  globalThis.createDosemu = (...a) => real(...a).then(m => {
    globalThis.__FS = m.FS;
    return m;
  });
};
globalThis.postMessage = m => posted.push(m);

vm.runInThisContext(readFileSync('dosemu-worker.js', 'utf8'),
                    { filename: 'dosemu-worker.js' });

const NEW = 'MADEUP.JWC';
/* What JW_CAD leaves when it plots through plot/WASM.JWP -- an extensionless
 * name, and a bounding box on the first line. */
const PLOT = 'PLOT';
const PLOT_BODY = 'B  -59450  -42050  59450  42050\nT 1\nM  -100  -100\nD  100  100\n';
/* What ⑥ＤＸＦ → ① 保存 leaves: the drawing's name and a **lower case**
   `.dxf`, which is how the file comes back off the guest's disk. */
const DXF = 'MADEUP.dxf';
const DXF_BODY = '  0\r\nSECTION\r\n  2\r\nENTITIES\r\n  0\r\nENDSEC\r\n  0\r\nEOF\r\n';
const started = Date.now();
let planted = false;

function look() {
  const lists = posted.filter(m => m.files);
  const booted = posted.some(m => m.booted);

  /* Once the guest is up, put a drawing on its disk the way its own save
   * would, and then watch for a listing that has it. */
  if (booted && !planted) {
    const FS = globalThis.__FS;
    if (FS) {
      FS.writeFile('orig/' + NEW, new Uint8Array(readFileSync(
          '../jwcad_dos_wasm/orig/SAMPLE0.JWC')));
      FS.writeFile('orig/' + PLOT, PLOT_BODY);
      FS.writeFile('orig/' + DXF, DXF_BODY);
      planted = true;
      console.log('planted ' + NEW + ' after ' + lists.length + ' listings');
    }
  }
  const got = lists.find(m => m.files.some(f => f.name === NEW));

  if (planted && got) {
    const p = got.files.find(f => f.name === PLOT);

    console.log('the list picked it up (' + lists.length + ' listings sent)');
    if (!p) {
      console.error('but not ' + PLOT + ' -- the plotter output is invisible,'
                    + ' so the PDF and PNG buttons stay grey');
      process.exit(1);
    }
    if (!p.plot) {
      console.error(PLOT + ' is listed but not marked plot');
      process.exit(1);
    }
    console.log(PLOT + ' is there and marked plot');
    if (!got.files.some(f => f.name === DXF)) {
      console.error('but not ' + DXF + ' -- a DXF the visitor has just saved'
                    + ' is invisible, so it cannot be downloaded');
      process.exit(1);
    }
    console.log(DXF + ' is there too');
    process.exit(0);
  }
  if (Date.now() - started > 120000) {
    console.error('no listing carried ' + NEW + ' in two minutes'
                  + ' (' + lists.length + ' listings sent)');
    process.exit(1);
  }
  setTimeout(look, 200);
}

setTimeout(look, 200);
