/* The plotter's output, turned into a PDF and a PNG.
 *
 * 入出力 → ②ﾌﾟﾛｯﾀ → ③ﾌｧｲﾙ出力 in the real JW_CAD asks which `*.JWP` to use.
 * A .JWP is a plotter definition -- it says what language the plotter speaks
 * (orig/JWP.DOC) -- and plot/WASM.JWP is one of our own: instead of HP-GL it
 * writes one plain line per thing drawn, so nothing here has to decode a
 * plotter's command set.
 *
 *   B x0 y0 x1 y1      the drawing area the plot is bound to
 *   P n                pen n from here on
 *   T n                line type n from here on
 *   M x y              pen up, move there
 *   D x y              pen down, draw to there
 *   O x y              a point
 *   C x y r a0 a1      an arc: centre, radius, start and end in degrees
 *   A x y sx sy rot c        one half-width character
 *   K x y sx sy rot c1 c2    one full-width character
 *   E                  the end
 *
 * A hundred units is a millimetre (WASM.JWP's unit_x/unit_y), and y points
 * up, which is a plotter's own way round.
 */

const PEN_RGB = [
  null,
  [0, 255, 255],        /* 1 cyan */
  [0, 0, 0],            /* 2 white on screen, black on paper */
  [0, 200, 0],          /* 3 green */
  [200, 170, 0],        /* 4 yellow */
  [255, 0, 255],        /* 5 magenta */
  [0, 0, 255],          /* 6 blue */
  [230, 0, 0],          /* 7 red */
  [140, 140, 140]       /* 8 grey */
];

/* The nine line types, in millimetres on the paper. */
const DASH_MM = [
  [], [], [0.35, 0.35], [0.7, 0.35], [1.4, 0.35],
  [1.4, 0.35, 0.35, 0.35], [2.1, 0.35, 0.35, 0.35],
  [1.4, 0.35, 0.35, 0.35, 0.35, 0.35],
  [2.1, 0.35, 0.35, 0.35, 0.35, 0.35], [0.2, 0.5]
];

/* Shift-JIS is what the plot carries; the browser has a decoder for it. */
const SJIS = new TextDecoder('shift_jis', { fatal: false });

export function parsePlot(bytes) {
  const text = SJIS.decode(bytes);
  const items = [];
  let pen = 2, type = 1, at = null;

  for (const raw of text.split(/\r?\n/)) {
    const line = raw.trim();
    if (!line) continue;
    const f = line.split(/\s+/);
    const n = f.slice(1).map(Number);

    switch (f[0]) {
      case 'P': pen = n[0] || 2; break;
      case 'T': type = n[0] || 1; break;
      case 'M': at = [n[0] / 100, n[1] / 100]; break;
      case 'D':
        if (at) items.push({ k: 'l', x0: at[0], y0: at[1],
                             x1: n[0] / 100, y1: n[1] / 100, pen, type });
        at = [n[0] / 100, n[1] / 100];
        break;
      case 'O': items.push({ k: 'o', x: n[0] / 100, y: n[1] / 100, pen }); break;
      case 'C':
        items.push({ k: 'c', x: n[0] / 100, y: n[1] / 100, r: n[2] / 100,
                     a0: n[3], a1: n[4], pen, type });
        break;
      case 'A': case 'K': {
        /* One character a record (ANK and KANJI are per-character commands),
           and the character itself is whatever follows the five numbers --
           which can be a space, so it cannot come out of the split.  A
           Shift-JIS trail byte is never 0x20, so the decode above is safe. */
        const m = raw.match(
          /^\s*[AK]\s+(-?\d+)\s+(-?\d+)\s+(-?[\d.]+)\s+(-?[\d.]+)\s+(-?[\d.]+)\s(.*)$/);

        if (m) {
          /* The sizes are already millimetres (WASM.JWP sets ank_x and the
             rest to 1), and a full-width character comes as its **JIS**
             pair, not Shift-JIS -- `#H` is Ｈ.  KANJI's [A%c][B%c] are the
             two seven-bit bytes an HP-GL alternate character set wants. */
          const s = f[0] === 'K' ? jisPair(m[6]) : m[6];

          items.push({ k: 't', x: +m[1] / 100, y: +m[2] / 100,
                       sx: +m[3], sy: +m[4], rot: +m[5], s, pen });
        }
        break;
      }
      default: break;
    }
  }
  return items;
}

/* JIS X 0208 as a pair of seven-bit bytes -> the character.  The plot writes
   kanji that way (KANJI's [A%c][B%c]), and the browser can only decode
   Shift-JIS, so it goes through the usual shuffle. */
function jisPair(two) {
  if (two.length < 2) return two;
  const j1 = two.charCodeAt(0), j2 = two.charCodeAt(1);
  let s1, s2;

  if (j1 % 2) s2 = j2 + 0x1f + (j2 >= 0x60 ? 1 : 0);
  else s2 = j2 + 0x7e;
  s1 = ((j1 - 0x21) >> 1) + 0x81;
  if (s1 > 0x9f) s1 += 0x40;
  return SJIS.decode(new Uint8Array([s1, s2]));
}

/* What the plot covers, with ten millimetres round it -- the same rule the
   C port uses (jwcad_dos_wasm's src/plot.c), and the same the reference at
   yomei-o/dosbox_wasm does. */
export function plotBox(items, margin = 10) {
  let x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
  const see = (x, y) => {
    if (x < x0) x0 = x;
    if (y < y0) y0 = y;
    if (x > x1) x1 = x;
    if (y > y1) y1 = y;
  };

  for (const it of items) {
    if (it.k === 'l') { see(it.x0, it.y0); see(it.x1, it.y1); }
    else if (it.k === 'c') { see(it.x - it.r, it.y - it.r); see(it.x + it.r, it.y + it.r); }
    else if (it.k === 't') { see(it.x, it.y); see(it.x + it.sx * 2, it.y + it.sy); }
    else see(it.x, it.y);
  }
  if (x1 < x0) { x0 = y0 = 0; x1 = y1 = 100; }
  return [x0 - margin, y0 - margin, x1 + margin, y1 + margin];
}

/* ------------------------------------------------------------------ PNG */

/* The browser has a PNG encoder in every canvas, so the raster is a canvas
   and `toBlob` does the rest. */
export function plotPng(items, dpmm = 4) {
  const [bx0, by0, bx1, by1] = plotBox(items);
  const w = Math.max(1, Math.round((bx1 - bx0) * dpmm));
  const h = Math.max(1, Math.round((by1 - by0) * dpmm));
  const c = document.createElement('canvas');
  const g = c.getContext('2d');

  c.width = w;
  c.height = h;
  g.fillStyle = '#fff';
  g.fillRect(0, 0, w, h);
  g.lineWidth = Math.max(1, 0.24 * dpmm);
  g.lineCap = 'round';
  const X = (x) => (x - bx0) * dpmm;
  const Y = (y) => h - (y - by0) * dpmm;

  for (const it of items) {
    const rgb = PEN_RGB[it.pen] || PEN_RGB[2];
    const css = `rgb(${rgb[0]},${rgb[1]},${rgb[2]})`;

    g.strokeStyle = css;
    g.fillStyle = css;
    g.setLineDash((DASH_MM[it.type] || []).map((m) => m * dpmm));
    if (it.k === 'l') {
      g.beginPath();
      g.moveTo(X(it.x0), Y(it.y0));
      g.lineTo(X(it.x1), Y(it.y1));
      g.stroke();
    } else if (it.k === 'c') {
      g.beginPath();
      g.arc(X(it.x), Y(it.y), it.r * dpmm, -it.a1 * Math.PI / 180,
            -it.a0 * Math.PI / 180);
      g.stroke();
    } else if (it.k === 'o') {
      g.setLineDash([]);
      g.beginPath();
      g.moveTo(X(it.x) - 2, Y(it.y));
      g.lineTo(X(it.x) + 2, Y(it.y));
      g.moveTo(X(it.x), Y(it.y) - 2);
      g.lineTo(X(it.x), Y(it.y) + 2);
      g.stroke();
    } else if (it.k === 't' && it.s) {
      g.save();
      g.translate(X(it.x), Y(it.y));
      g.rotate(-it.rot * Math.PI / 180);
      g.font = `${Math.max(1, it.sy * dpmm)}px sans-serif`;
      g.textBaseline = 'alphabetic';
      g.fillText(it.s, 0, 0);
      g.restore();
    }
  }
  return new Promise((ok) => c.toBlob(ok, 'image/png'));
}

/* ------------------------------------------------------------------ PDF */

function pdfEscape(bytes) {
  let out = '';
  for (const b of bytes) out += b.toString(16).padStart(2, '0');
  return out;
}

export function plotPdf(items) {
  const [bx0, by0, bx1, by1] = plotBox(items);
  const pt = 72 / 25.4;
  const W = (bx1 - bx0) * pt, H = (by1 - by0) * pt;
  const X = (x) => ((x - bx0) * pt).toFixed(2);
  const Y = (y) => ((y - by0) * pt).toFixed(2);
  const enc = new TextEncoder();
  let s = `${(0.24 * pt).toFixed(2)} w 1 J 1 j\n`;
  let pen = -1, type = -1;

  for (const it of items) {
    if (it.pen !== pen) {
      const rgb = PEN_RGB[it.pen] || PEN_RGB[2];
      s += `${(rgb[0] / 255).toFixed(3)} ${(rgb[1] / 255).toFixed(3)} `
         + `${(rgb[2] / 255).toFixed(3)} RG\n`;
      pen = it.pen;
    }
    if (it.k !== 't' && it.type !== type) {
      const d = (DASH_MM[it.type] || []).map((m) => (m * pt).toFixed(2));
      s += `[${d.join(' ')}] 0 d\n`;
      type = it.type;
    }
    if (it.k === 'l') {
      s += `${X(it.x0)} ${Y(it.y0)} m ${X(it.x1)} ${Y(it.y1)} l S\n`;
    } else if (it.k === 'c') {
      /* An arc as a run of short lines: a plotter's paper never shows the
         joins at two degrees a step. */
      const n = Math.max(8, Math.ceil(Math.abs(it.a1 - it.a0) / 2));
      for (let i = 0; i <= n; i++) {
        const a = (it.a0 + (it.a1 - it.a0) * i / n) * Math.PI / 180;
        const px = it.x + it.r * Math.cos(a), py = it.y + it.r * Math.sin(a);
        s += `${X(px)} ${Y(py)} ${i ? 'l' : 'm'}\n`;
      }
      s += 'S\n';
    } else if (it.k === 'o') {
      s += `${X(it.x - 0.4)} ${Y(it.y)} m ${X(it.x + 0.4)} ${Y(it.y)} l S\n`;
      s += `${X(it.x)} ${Y(it.y - 0.4)} m ${X(it.x)} ${Y(it.y + 0.4)} l S\n`;
    } else if (it.k === 't' && it.s) {
      const rgb = PEN_RGB[it.pen] || PEN_RGB[2];
      const r = it.rot * Math.PI / 180;

      s += `q ${(rgb[0] / 255).toFixed(3)} ${(rgb[1] / 255).toFixed(3)} `
         + `${(rgb[2] / 255).toFixed(3)} rg BT /F1 ${(it.sy * pt).toFixed(2)} Tf\n`;
      s += `${Math.cos(r).toFixed(4)} ${Math.sin(r).toFixed(4)} `
         + `${(-Math.sin(r)).toFixed(4)} ${Math.cos(r).toFixed(4)} `
         + `${X(it.x)} ${Y(it.y)} Tm\n`;
      s += `<${pdfEscape(sjisOf(it.s))}> Tj ET Q\n`;
      pen = -1;
      type = -1;
    }
  }

  /* The objects, with the offsets the cross-reference table needs. */
  const parts = [];
  const off = [];
  let at = 0;
  const put = (t) => { parts.push(enc.encode(t)); at += enc.encode(t).length; };

  put('%PDF-1.4\n');
  off[1] = at; put('1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj\n');
  off[2] = at; put('2 0 obj<</Type/Pages/Kids[3 0 R]/Count 1>>endobj\n');
  off[3] = at; put(`3 0 obj<</Type/Page/Parent 2 0 R/MediaBox[0 0 ${W.toFixed(2)} ${H.toFixed(2)}]`
                   + '/Resources<</Font<</F1 5 0 R>>>>/Contents 4 0 R>>endobj\n');
  off[4] = at; put(`4 0 obj<</Length ${enc.encode(s).length}>>stream\n${s}endstream endobj\n`);
  off[5] = at; put('5 0 obj<</Type/Font/Subtype/Type0/BaseFont/Ryumin-Light'
                   + '/Encoding/90ms-RKSJ-H/DescendantFonts[6 0 R]>>endobj\n');
  off[6] = at; put('6 0 obj<</Type/Font/Subtype/CIDFontType0/BaseFont/Ryumin-Light'
                   + '/CIDSystemInfo<</Registry(Adobe)/Ordering(Japan1)/Supplement 2>>'
                   + '/FontDescriptor 7 0 R/DW 1000>>endobj\n');
  off[7] = at; put('7 0 obj<</Type/FontDescriptor/FontName/Ryumin-Light/Flags 6'
                   + '/FontBBox[-170 -331 1024 903]/ItalicAngle 0/Ascent 903'
                   + '/Descent -331/CapHeight 709/StemV 69>>endobj\n');
  const xref = at;
  let tail = 'xref\n0 8\n0000000000 65535 f \n';
  for (let i = 1; i <= 7; i++) tail += String(off[i]).padStart(10, '0') + ' 00000 n \n';
  tail += `trailer<</Size 8/Root 1 0 R>>\nstartxref\n${xref}\n%%EOF\n`;
  put(tail);
  return new Blob(parts, { type: 'application/pdf' });
}

/* Shift-JIS again on the way out: the PDF's 90ms-RKSJ-H encoding takes the
   same bytes the plot carried, so the letters go through unchanged and no
   font has to be embedded.  The browser has no Shift-JIS *encoder*, so the
   text is kept as bytes from the start where it can be. */
let SJIS_MAP = null;

function sjisOf(s) {
  if (s.bytes) return s.bytes;
  if (!SJIS_MAP) {
    SJIS_MAP = new Map();
    const one = new Uint8Array(2);
    for (let hi = 0x81; hi <= 0xfc; hi++) {
      if (hi > 0x9f && hi < 0xe0) continue;
      for (let lo = 0x40; lo <= 0xfc; lo++) {
        if (lo === 0x7f) continue;
        one[0] = hi;
        one[1] = lo;
        const c = SJIS.decode(one);
        if (c.length === 1 && c !== '�' && !SJIS_MAP.has(c)) {
          SJIS_MAP.set(c, [hi, lo]);
        }
      }
    }
  }
  const out = [];
  for (const ch of s) {
    const code = ch.codePointAt(0);
    if (code < 0x80) out.push(code);
    else if (SJIS_MAP.has(ch)) out.push(...SJIS_MAP.get(ch));
    else if (code >= 0xff61 && code <= 0xff9f) out.push(code - 0xff61 + 0xa1);
    else out.push(0x3f);        /* '?' for anything Shift-JIS has no room for */
  }
  return out;
}
