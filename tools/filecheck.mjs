/* Does a drawing **uploaded into the browser build** open the way one baked
 * into it does?
 *
 *     node tools/filecheck.mjs ../jwcad_dos_wasm/orig/SAMPLE6.JWC UPLOAD.JWC \
 *          150000000 tmp/fc/wasm.raw
 *
 * The page's upload writes the visitor's bytes into the module's own
 * filesystem under `orig/`, and then boots the guest on that name -- there is
 * no second code path, because the guest opens files through DOS and DOS is
 * that filesystem.  This drives exactly that: put the bytes there by hand,
 * boot, run, and write the screen.  tools/filecheck.sh compares it with the
 * native emulator started on the same bytes under the same name.
 *
 * It also reads the file back out the way the page's download does and checks
 * the bytes are the ones that went in.
 */
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { dirname } from 'node:path';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const createDosemu = require('../dosemu.js');

const [source, name, insnsArg, out] = process.argv.slice(2);
const want = Number(insnsArg);
const bytes = new Uint8Array(readFileSync(source));

const Module = await createDosemu();

if (!Module.FS) {
  console.error('FS is not exported -- the page cannot upload or download');
  process.exit(1);
}

/* Upload, the way index.html does it. */
Module.FS.writeFile('orig/' + name, bytes);

/* Download, the way index.html does it. */
const back = Module.FS.readFile('orig/' + name);
if (back.length !== bytes.length || back.some((b, i) => b !== bytes[i])) {
  console.error('what came back out is not what went in');
  process.exit(1);
}

const p = Module._malloc(name.length + 1);
Module.stringToUTF8(name, p, name.length + 1);
if (!Module._de_boot(p)) {
  console.error('boot: ' + Module.UTF8ToString(Module._de_message()));
  process.exit(1);
}
Module._free(p);

while (Module._de_run(2000000) < want) {
  if (Module._de_dead()) {
    console.error('stopped: ' + Module.UTF8ToString(Module._de_message()));
    process.exit(1);
  }
}

const w = Module._de_width(), h = Module._de_height();
const fb = Module._de_frame();
mkdirSync(dirname(out), { recursive: true });
writeFileSync(out, Buffer.from(Module.HEAPU8.subarray(fb, fb + w * h * 4)));
console.log(`${name}: ${bytes.length} bytes in and back out, ${w}x${h} written`);
