/* Does the PDF draw what the plotter file says?
 *
 *     node tools/plotvspdf.mjs tmp/flow/FLOW.PLT tmp/flow/read.json
 *
 * The first is what the original wrote through plot/WASM.JWP.  The second is
 * tools/pdfread.mjs's report on the PDF made from it -- read back out of the
 * file the way a viewer reads it, not remembered from when it was written.
 * Comparing the two is the only way to know the PDF holds the drawing and
 * not just a well-formed empty page.
 */
import { readFileSync } from 'node:fs';
import { parsePlot, plotBox } from '../plot.js';

const items = parsePlot(new Uint8Array(readFileSync(process.argv[2])));
const got = JSON.parse(readFileSync(process.argv[3], 'utf8'));
let bad = 0;
const ok = (c, w) => { console.log((c ? '  ok   ' : '  FAIL ') + w); if (!c) bad++; };

/* Lines in the plotter file against segments in the PDF.  An arc becomes a
   run of short lines and a point becomes a cross, so they are counted the
   way the writer draws them. */
const want = items.reduce((n, it) => n + (it.k === 'l' ? 1 : it.k === 'o' ? 2 : 0), 0);
const arcs = items.filter((it) => it.k === 'c').length;

ok(got.segments >= want,
   'the PDF draws the plotted lines (' + want + ' lines'
   + (arcs ? ' + ' + arcs + ' arcs' : '') + ' -> ' + got.segments + ' segments)');

const said = items.filter((it) => it.k === 't').map((it) => it.s).join('');
ok(got.texts.join('') === said,
   'and the same text, character for character (' + said.length + ')');

const [x0, y0, x1, y1] = plotBox(items);
ok(Math.abs(got.mm[0] - (x1 - x0)) < 0.2 && Math.abs(got.mm[1] - (y1 - y0)) < 0.2,
   'the page is the size of the drawing (' + got.mm[0] + ' x ' + got.mm[1] + ' mm)');

const first = items.find((it) => it.k === 'l');
if (first) {
  const near = (a, b) => Math.abs(a - b) < 0.2;

  ok(near(got.first[0], first.x0 - x0) && near(got.first[1], first.y0 - y0)
     && near(got.first[2], first.x1 - x0) && near(got.first[3], first.y1 - y0),
     'and the first line is where the plotter put it');
}
process.exit(bad ? 1 : 0);
