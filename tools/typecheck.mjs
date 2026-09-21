/* Can a name be typed at the guest's prompt, one character after another?
 *
 *     node tools/typecheck.mjs          # both ways
 *     SLOW=1 node tools/typecheck.mjs   # a pause between the keys only
 *
 * 入出力 → ②ﾌﾟﾛｯﾀ → ③ﾌｧｲﾙ出力 asks 出力ファイル名 ?, and a visitor reported
 * that only the first character of the answer arrived (2026-09-21).  The
 * script-driven runs never caught it: they go through the native front end
 * or straight into the module, and this path is **dosemu-worker.js** -- the
 * page's own worker, one message per keystroke.
 *
 * So this runs the worker itself, the way tools/listcheck.mjs does, walks
 * the menus with the same messages the page sends, and answers the prompt.
 * **Twice**: once with the four keys spaced out, and once with all four
 * sent before the guest has run at all, which is what typing at speed
 * looks like from the worker's side.  Then it reads the top line back off
 * the frame the worker sent.
 *
 * The pointer is moved out of the field before the reading: the guest draws
 * its arrow over whatever is under it, and an arrow sitting on a letter
 * reads as a letter that never arrived.
 */
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { createRequire } from 'node:module';
import { execFileSync } from 'node:child_process';
import vm from 'node:vm';

const require = createRequire(import.meta.url);
const posted = [];
globalThis.importScripts = () => {
  const real = require('../dosemu.js');
  globalThis.createDosemu = (...a) => real(...a).then(m => {
    globalThis.__M = m;
    return m;
  });
};
globalThis.postMessage = (m) => posted.push(m);
vm.runInThisContext(readFileSync('dosemu-worker.js', 'utf8'),
                    { filename: 'dosemu-worker.js' });

const send = (m) => globalThis.onmessage({ data: m });
const wait = (ms) => new Promise((r) => setTimeout(r, ms));
const insns = () => (globalThis.__M ? globalThis.__M._de_run(0) : 0);

async function until(n, why) {
  const started = Date.now();
  while (insns() < n) {
    if (Date.now() - started > 900000) throw new Error('timed out ' + why);
    await wait(50);
  }
}

const press = async (x, y, n) => {
  send({ mouse: [x, y] });
  send({ button: 0, down: 1 });
  send({ button: 0, down: 0 });
  await until(insns() + n, 'after a press');
};

mkdirSync('tmp/type', { recursive: true });
send({ boot: '' });
await until(40000000, 'booting');

await press(30, 296, 26000000);          // 入出力
await press(220, 8, 26000000);           // ②ﾌﾟﾛｯﾀ
await press(400, 8, 26000000);           // ③ﾌｧｲﾙ出力
await press(150, 136, 26000000);         // WASM.JWP
await press(200, 8, 26000000);           // ①選択確定

const NAME = 'PLOT';
const fast = !process.env.SLOW;

if (fast) {
  /* **All four before the guest has run.**  A person typing puts four
     keydowns into the page within a few tens of milliseconds, and the
     worker runs the guest in slices -- so all four are in its queue when
     the next slice starts.  This is the case the visitor hit. */
  for (const ch of NAME) send({ keys: [ch.charCodeAt(0)] });
  await until(insns() + 30000000, 'after the name');
} else {
  for (const ch of NAME) {
    send({ keys: [ch.charCodeAt(0)] });
    await until(insns() + 6000000, 'after a key');
  }
}
/* Out of the way, so the arrow is not read as a letter. */
send({ mouse: [500, 300] });
await until(insns() + 6000000, 'moving the pointer off the line');

const frame = [...posted].reverse().find((m) => m.frame);
if (!frame) { console.error('  the worker never sent a frame'); process.exit(1); }
writeFileSync('tmp/type/top.raw', Buffer.from(frame.frame));

/* readrow.py loads the same two FONTX2 files the guest is given, by a path
   relative to its own checkout, so it is run from there. */
const line = execFileSync('python',
    ['tools/readrow.py', '../dosv_emu_cpp/tmp/type/top.raw', '1'],
    { encoding: 'latin1', cwd: '../jwcad_dos_wasm' });
const shown = Buffer.from(line, 'latin1').toString('latin1');

console.log('  ' + (fast ? 'typed at speed:' : 'typed slowly:   ')
            + ' the line reads ...' + (shown.match(/\?.{0,12}/) || [shown])[0]);
if (shown.includes(NAME)) {
  console.log('  ok   all of ' + NAME + ' arrived');
  process.exit(0);
}
console.error('  FAIL ' + NAME + ' is not on the line');
process.exit(1);
