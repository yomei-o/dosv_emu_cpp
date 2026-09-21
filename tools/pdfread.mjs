/* Read a PDF back, without plot.js.
 *
 *     node tools/pdfread.mjs tmp/flow/out.pdf
 *
 * This is the other half of the plotter check.  plotPdf() writing bytes and
 * the file being 5130 bytes long says nothing about whether a reader can
 * open it: the cross-reference table has to point at the objects, the
 * content stream's /Length has to be the real length, the page has to be
 * reachable from the trailer's /Root, and every font the text asks for has
 * to be in the page's resources.  A PDF that gets any of those wrong opens
 * as a blank page or not at all, and the machine that wrote it cannot tell.
 *
 * So this walks the file the way a viewer does -- startxref, the table, the
 * trailer, /Root, /Pages, /Kids, the page -- and refuses to guess: an offset
 * that does not land on `N 0 obj` is an error, not something to scan past.
 * Then it tokenises the content stream and reports what is actually drawn,
 * which tools/flowcheck.sh compares against the plotter file the original
 * wrote.
 *
 * Prints a JSON line and exits 1 on anything wrong.
 */
import { readFileSync } from 'node:fs';

const path = process.argv[2];
if (!path) { console.error('usage: node tools/pdfread.mjs <file.pdf>'); process.exit(2); }
const buf = readFileSync(path);
const text = buf.toString('latin1');
const die = (why) => { console.error('  pdfread: ' + why); process.exit(1); };

if (!text.startsWith('%PDF-1.')) die('no %PDF header');
if (!text.trimEnd().endsWith('%%EOF')) die('no %%EOF at the end');

/* --- the cross-reference table -------------------------------------- */
const sx = text.lastIndexOf('startxref');
if (sx < 0) die('no startxref');
const xrefAt = parseInt(text.slice(sx + 9).trim(), 10);
if (!(xrefAt > 0 && xrefAt < buf.length)) die('startxref points outside the file');
if (!text.startsWith('xref', xrefAt)) die('startxref does not point at an xref table');

const head = text.slice(xrefAt).match(/^xref\s+(\d+)\s+(\d+)\s/);
if (!head) die('the xref table has no subsection header');
const first = +head[1], count = +head[2];
if (first !== 0) die('the xref table does not start at object 0');

const rows = text.slice(xrefAt + head[0].length);
const offs = [];
for (let i = 0; i < count; i++) {
  const m = rows.slice(i * 20, i * 20 + 20).match(/^(\d{10}) (\d{5}) ([nf])/);
  if (!m) die('xref row ' + i + ' is not 20 bytes of "nnnnnnnnnn ggggg n"');
  offs[first + i] = m[3] === 'n' ? +m[1] : -1;
}
/* Every `n` entry must land on its own object.  This is the check that
   catches a writer whose offsets drifted -- the commonest way to make a PDF
   that some viewers open and others do not. */
for (let i = 1; i < count; i++) {
  if (offs[i] < 0) continue;
  if (!new RegExp('^' + i + '\\s+0\\s+obj').test(text.slice(offs[i], offs[i] + 32)))
    die('xref says object ' + i + ' is at ' + offs[i] + ', and it is not there');
}

const trailer = text.slice(xrefAt).match(/trailer\s*<<([\s\S]*?)>>/);
if (!trailer) die('no trailer');
const size = +(trailer[1].match(/\/Size\s+(\d+)/) || [])[1];
if (size !== count) die('/Size ' + size + ' but the table has ' + count + ' entries');
const rootRef = (trailer[1].match(/\/Root\s+(\d+)\s+0\s+R/) || [])[1];
if (!rootRef) die('the trailer has no /Root');

/* --- objects, by number --------------------------------------------- */
function obj(n) {
  if (!(offs[n] > 0)) die('object ' + n + ' is not in the xref table');
  const from = offs[n];
  const end = text.indexOf('endobj', from);
  if (end < 0) die('object ' + n + ' has no endobj');
  return text.slice(text.indexOf('obj', from) + 3, end);
}
const ref = (body, key) => {
  const m = body.match(new RegExp('\\/' + key + '\\s+(\\d+)\\s+0\\s+R'));
  return m ? +m[1] : 0;
};

const catalog = obj(+rootRef);
if (!/\/Type\s*\/Catalog/.test(catalog)) die('/Root is not a /Catalog');
const pagesN = ref(catalog, 'Pages');
if (!pagesN) die('the catalog has no /Pages');
const pages = obj(pagesN);
if (!/\/Type\s*\/Pages/.test(pages)) die('/Pages is not a /Pages');
const kids = (pages.match(/\/Kids\s*\[([^\]]*)\]/) || [])[1];
if (!kids) die('/Pages has no /Kids');
const kidNs = [...kids.matchAll(/(\d+)\s+0\s+R/g)].map((m) => +m[1]);
const nPages = +(pages.match(/\/Count\s+(\d+)/) || [])[1];
if (kidNs.length !== nPages) die('/Count ' + nPages + ' but ' + kidNs.length + ' kids');

const page = obj(kidNs[0]);
if (!/\/Type\s*\/Page[^s]/.test(page)) die('the first kid is not a /Page');
const box = (page.match(/\/MediaBox\s*\[([^\]]*)\]/) || [])[1];
if (!box) die('the page has no /MediaBox');
const mb = box.trim().split(/\s+/).map(Number);
if (mb.length !== 4 || mb.some((v) => !isFinite(v))) die('/MediaBox is not four numbers');
if (!(mb[2] > mb[0] && mb[3] > mb[1])) die('/MediaBox has no area');

/* --- the content stream --------------------------------------------- */
const contentN = ref(page, 'Contents');
if (!contentN) die('the page has no /Contents');
const cbody = obj(contentN);
const declared = +(cbody.match(/\/Length\s+(\d+)/) || [])[1];
if (!isFinite(declared)) die('the content stream has no /Length');
const sAt = cbody.indexOf('stream');
const eAt = cbody.lastIndexOf('endstream');
if (sAt < 0 || eAt < 0) die('the content stream has no stream/endstream');
/* A viewer takes /Length bytes from just after the EOL that follows the
   `stream` keyword, and then expects `endstream`.  Trimming whitespace off
   the end instead would hide a /Length one too many -- and one too many is
   exactly the mistake a writer makes when its data ends in a newline. */
if (!/[\r\n]/.test(cbody[sAt + 6])) die('no EOL after the `stream` keyword');
const from = sAt + 6 + (cbody.startsWith('\r\n', sAt + 6) ? 2 : 1);
const body = cbody.slice(from, from + declared);
if (body.length !== declared)
  die('/Length says ' + declared + ' and only ' + body.length + ' bytes follow');
if (!/^\s*endstream/.test(cbody.slice(from + declared)))
  die('/Length ' + declared + ' does not reach endstream');
if (/\/Filter/.test(cbody)) die('unexpected /Filter -- this reader takes plain text');

/* --- what it draws --------------------------------------------------- */
const segs = [];
const texts = [];
const fontsUsed = new Set();
let cur = null;

for (const tok of body.split(/\r?\n/)) {
  const m = tok.match(/^([-\d.]+) ([-\d.]+) m ([-\d.]+) ([-\d.]+) l S$/);
  if (m) { segs.push(m.slice(1).map(Number)); continue; }
  const one = tok.match(/^([-\d.]+) ([-\d.]+) (m|l)$/);
  if (one) {
    const p = [+one[1], +one[2]];
    if (one[3] === 'm') cur = p;
    else { segs.push([cur[0], cur[1], p[0], p[1]]); cur = p; }
    continue;
  }
  const tf = tok.match(/\/(F\d+)\s+[-\d.]+\s+Tf/);
  if (tf) fontsUsed.add(tf[1]);
  const tj = tok.match(/^<([0-9A-Fa-f]*)> Tj/);
  if (tj) {
    const bytes = Buffer.from(tj[1], 'hex');
    texts.push(new TextDecoder('shift_jis').decode(bytes));
  }
}
if (!segs.length && !texts.length) die('the page draws nothing at all');

/* Every font the text names has to be in the page's own resources. */
const res = (page.match(/\/Resources\s*<<([\s\S]*?)>>\s*>>/) || [])[1] || page;
for (const f of fontsUsed) {
  const fn = ref(res, f);
  if (!fn) die('the text uses /' + f + ' and the page does not have it');
  const font = obj(fn);
  if (!/\/Type\s*\/Font/.test(font)) die('/' + f + ' is not a /Font');
  /* A Type0 font must name the descendant it delegates to, or a viewer has
     nothing to measure the Shift-JIS with. */
  if (/\/Subtype\s*\/Type0/.test(font)) {
    const d = (font.match(/\/DescendantFonts\s*\[\s*(\d+)\s+0\s+R/) || [])[1];
    if (!d) die('/' + f + ' is a Type0 font with no /DescendantFonts');
    if (!/\/Type\s*\/Font/.test(obj(+d))) die('/' + f + "'s descendant is not a font");
    if (!/\/Encoding\s*\/90ms-RKSJ-H/.test(font))
      die('/' + f + ' does not say its text is Shift-JIS');
  }
}

const pt = 25.4 / 72;
console.log(JSON.stringify({
  bytes: buf.length,
  objects: count - 1,
  pages: nPages,
  mm: [+((mb[2] - mb[0]) * pt).toFixed(1), +((mb[3] - mb[1]) * pt).toFixed(1)],
  segments: segs.length,
  texts,
  /* in millimetres from the corner of the page, for flowcheck to compare */
  first: segs.length ? segs[0].map((v) => +(v * pt).toFixed(2)) : null,
  last: segs.length ? segs[segs.length - 1].map((v) => +(v * pt).toFixed(2)) : null,
}));
