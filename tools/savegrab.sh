#!/bin/sh
# Have the guest **save a drawing inside the browser build**, then take the
# file off its disk the way the page's ダウンロード does, and check it against
# the same save done natively.
#
#     sh tools/savegrab.sh                       # SAMPLE0
#     DRAWING=TEST2 BOOT=60000000 sh tools/savegrab.sh
#
# This is the half of the page's file support that upload does not cover: the
# bytes come from the guest rather than from the visitor, and they have to
# reach the visitor's machine unchanged.  Both sides run the identical script
# -- 入出力 ①ファイル ①保存 ①選択確定, the memo lines, ①上書きする, ①実行 --
# and the two .JWC files must be byte for byte the same.
#
# It takes about a billion instructions, so it is slow (a couple of minutes).
set -e
cd "$(dirname "$0")/.."
mkdir -p tmp/sg

[ -f dosemu.js ] || { echo "run sh tools/build_wasm.sh first" >&2; exit 2; }
[ -x dosemu.exe ] || { echo "run sh build.sh first" >&2; exit 2; }

ORIG="${ORIG:-../jwcad_dos_wasm/orig}"
FONT="${FONT:-../jwcad_dos_wasm/font}"
DRAWING="${DRAWING:-SAMPLE0}"
BOOT="${BOOT:-40000000}"

NODE="${NODE:-}"
if [ -z "$NODE" ]; then
    command -v node > /dev/null 2>&1 && NODE=node
fi
if [ -z "$NODE" ]; then
    NODE=$(ls /c/prog/emsdk/emsdk/node/*/bin/node.exe 2>/dev/null | head -1)
fi
[ -n "$NODE" ] || { echo "no node (set NODE)" >&2; exit 2; }

# The way through the menus, read off the original's own top line -- the same
# one jwcad_dos_wasm/tools/save.sh takes, with its waits.  They are what the
# run needs, not guesses: the overwrite question only goes up about seven
# million instructions after the press before it.
P='mouse %d %d\nwait 3000000\ndown left\nwait 3000000\nup left\nwait %s\n'
{
    printf 'wait %s\n' "$BOOT"
    printf 'mouse 30 296\nwait 2000000\nclick left\nwait 24000000\n'  # 入出力
    printf "$P" 110 8 40000000                                        # ①ファイル
    printf "$P" 100 8 90000000                                        # ①保存
    printf "$P" 200 8 90000000                                        # ①選択確定
    i=0
    while [ $i -lt 8 ]; do printf 'key enter\nwait 30000000\n'; i=$((i + 1)); done
    printf 'wait 60000000\n'
    printf "$P" 280 8 90000000                                        # ①上書きする
    printf "$P" 210 8 400000000                                       # ①実行
} > tmp/sg/steps.txt

cp tmp/sg/steps.txt tmp/sg/wasm.txt
printf 'shot tmp/sg/wasm.raw\ngetfile %s.JWC tmp/sg/wasm.JWC\n' "$DRAWING" \
    >> tmp/sg/wasm.txt
"$NODE" tools/wasmshot.mjs tmp/sg/wasm.txt "$DRAWING.JWC" > /dev/null 2>&1

rm -rf tmp/sg/root
cp -r "$ORIG" tmp/sg/root
cp tmp/sg/steps.txt tmp/sg/native.txt
printf 'shot tmp/sg/native.raw\n' >> tmp/sg/native.txt
./dosemu.exe --root tmp/sg/root --font-ank "$FONT/JWANK16.FNT" \
    --font-kanji "$FONT/JWKAN16.FNT" --script tmp/sg/native.txt \
    tmp/sg/root/JW_CADV.EXE "$DRAWING.JWC" > /dev/null 2>&1

[ -f "tmp/sg/root/$DRAWING.bak" ] || {
    echo "  the native save did not happen -- the waits are short" >&2; exit 1; }
[ -f tmp/sg/wasm.JWC ] || {
    echo "  nothing came off the browser build's disk" >&2; exit 1; }

if cmp -s tmp/sg/wasm.JWC "tmp/sg/root/$DRAWING.JWC"; then
    echo "  same   $DRAWING.JWC saved by the guest, taken off its disk" \
         "($(wc -c < tmp/sg/wasm.JWC | tr -d ' ') bytes)"
else
    echo "  DIFFER $DRAWING.JWC"
    exit 1
fi
