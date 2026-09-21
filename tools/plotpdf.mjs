/* A plotter file -> a PDF, through the page's own plot.js.
 *
 *     node tools/plotpdf.mjs tmp/plot/web.plt tmp/plot/web
 *
 * Leaves <stem>.pdf and prints what it found.  The PNG cannot be made here
 * -- plot.js draws it on a canvas, which is the browser's -- so this checks
 * the parse and the PDF, and the page does the rest.
 */
import { readFileSync, writeFileSync } from 'node:fs';
import { parsePlot, plotBox, plotPdf } from '../plot.js';

const src = process.argv[2] || 'tmp/plot/web.plt';
const stem = process.argv[3] || 'tmp/plot/web';
const items = parsePlot(new Uint8Array(readFileSync(src)));
const kinds = {};

for (const it of items) kinds[it.k] = (kinds[it.k] || 0) + 1;
const box = plotBox(items);
console.log('  %d items %j', items.length, kinds);
console.log('  page %s x %s mm', (box[2] - box[0]).toFixed(1),
            (box[3] - box[1]).toFixed(1));
console.log('  text %j',
            items.filter(i => i.k === 't').map(i => i.s).join(''));

const buf = Buffer.from(await plotPdf(items).arrayBuffer());
writeFileSync(stem + '.pdf', buf);
const head = buf.subarray(0, 8).toString('latin1');
const tail = buf.subarray(buf.length - 8).toString('latin1');
const ok = head === '%PDF-1.4' && tail.includes('%%EOF') && items.length > 0;

console.log('  %s.pdf %d bytes  %s', stem, buf.length, ok ? 'ok' : 'BAD');
process.exit(ok ? 0 : 1);
