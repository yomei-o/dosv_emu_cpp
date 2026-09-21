#!/bin/sh
# The whole plotter road, through the **browser build**.
#
#     sh tools/plotwebcheck.sh
#     DRAWING=TEST5 sh tools/plotwebcheck.sh
#
# Drives the guest from 入出力 to ① 実行, takes the file it wrote off the
# guest's disk the way the page's buttons do, and turns it into a PDF with
# plot.js.  The native side of the same road is tools/plotrun.sh; this is
# the one a visitor actually walks.
set -e
cd "$(dirname "$0")/.."
NODE="${NODE:-}"
[ -n "$NODE" ] || { command -v node > /dev/null 2>&1 && NODE=node; }
[ -n "$NODE" ] || NODE=$(ls /c/prog/emsdk/emsdk/node/*/bin/node.exe 2>/dev/null | head -1)
[ -n "$NODE" ] || { echo "no node (set NODE)" >&2; exit 2; }
DRAWING="${DRAWING:-SAMPLE0}"
BOOT="${BOOT:-40000000}"
TAIL="${TAIL:-600000000}"
mkdir -p tmp/plot

# The presses, with the columns the original's own bars give (the tree is in
# dosv_emu_cpp's RESUME and jwcad_dos_wasm's RESUME 4.44).
press() {
    printf 'mouse %s %s\nwait 3000000\ndown left\nwait 3000000\nup left\nwait 40000000\n' "$1" "$2"
}
{
    printf 'wait %s\n' "$BOOT"
    press 30 296            # 入出力
    press 220 8             # ②ﾌﾟﾛｯﾀ
    press 400 8             # ③ﾌｧｲﾙ出力
    press 150 136           # the second *.JWP in the list -- WASM.JWP
    press 200 8             # ①選択確定
    printf 'type PLOT\nwait 40000000\nkey enter\nwait 40000000\n'
    press 140 8             # ①確定
    press 164 8             # ① 実行
    printf 'wait %s\n' "$TAIL"
    printf 'getfile PLOT tmp/plot/web.plt\nshot tmp/plot/web.raw\n'
} > tmp/plot/web.txt

rm -f tmp/plot/web.plt
# **The clock has to run.**  ②ﾌﾟﾛｯﾀ paces its output by the time of day.
DOSEMU_CLOCK="${DOSEMU_CLOCK:-20000}" \
    "$NODE" tools/wasmshot.mjs tmp/plot/web.txt "$DRAWING.JWC" tmp/plot/web.raw \
    > /dev/null 2>&1 || true

[ -s tmp/plot/web.plt ] || { echo "no plot came out"; exit 1; }
"$NODE" tools/plotpdf.mjs tmp/plot/web.plt tmp/plot/web
