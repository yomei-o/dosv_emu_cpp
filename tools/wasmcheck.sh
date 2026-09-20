#!/bin/sh
# Does the browser build run the guest exactly as the native one does?
#
#     sh tools/wasmcheck.sh
#
# The two front ends share every line of the emulator, so the same script has
# to leave the same screen -- byte for byte, not nearly.  If it ever does not,
# the bug is in a front end (a slice boundary, an event queued at the wrong
# instruction) and not in the guest, which is worth knowing straight away.
set -e
cd "$(dirname "$0")/.."
mkdir -p tmp/wc

[ -f dosemu.js ] || { echo "run sh tools/build_wasm.sh first" >&2; exit 2; }
[ -x dosemu.exe ] || { echo "run sh build.sh first" >&2; exit 2; }

ORIG="${ORIG:-../jwcad_dos_wasm/orig}"
FONT="${FONT:-../jwcad_dos_wasm/font}"
NODE="${NODE:-}"
if [ -z "$NODE" ]; then
    command -v node > /dev/null 2>&1 && NODE=node
fi
if [ -z "$NODE" ]; then
    NODE=$(ls /c/prog/emsdk/emsdk/node/*/bin/node.exe 2>/dev/null | head -1)
fi
[ -n "$NODE" ] || { echo "no node (set NODE)" >&2; exit 2; }

# name | drawing | the script, with the shot left out (each side adds its own)
run() {
    name=$1; drawing=$2; body=$3
    printf '%s' "$body" > tmp/wc/$name.txt
    printf '%sshot tmp/wc/%s_wasm.raw\n' "$body" "$name" > tmp/wc/${name}_w.txt
    printf '%sshot tmp/wc/%s_native.raw\n' "$body" "$name" > tmp/wc/${name}_n.txt
    "$NODE" tools/wasmshot.mjs tmp/wc/${name}_w.txt "$drawing" > /dev/null 2>&1
    ./dosemu.exe --root "$ORIG" --font-ank "$FONT/JWANK16.FNT" \
        --font-kanji "$FONT/JWKAN16.FNT" --script tmp/wc/${name}_n.txt \
        "$ORIG/JW_CADV.EXE" "$drawing" > /dev/null 2>&1
    if cmp -s tmp/wc/${name}_wasm.raw tmp/wc/${name}_native.raw; then
        echo "  same   $name"
    else
        echo "  DIFFER $name"
        bad=1
    fi
}

bad=0
echo "=== the browser build against the native one, same script"
run opening SAMPLE0.JWC 'wait 60000000
'
run drawing SAMPLE0.JWC 'wait 40000000
mouse 90 104
wait 2000000
click left
wait 24000000
mouse 300 200
wait 3000000
down left
wait 3000000
up left
wait 14000000
mouse 400 250
wait 3000000
down left
wait 3000000
up left
wait 20000000
'
run read SAMPLE6.JWC 'wait 150000000
mouse 90 104
wait 2000000
click left
wait 24000000
mouse 470 305
wait 3000000
down right
wait 3000000
up right
wait 20000000
'
run japanese SAMPLE0.JWC 'wait 40000000
mouse 90 264
wait 2000000
click left
wait 60000000
mouse 250 200
wait 3000000
down left
wait 3000000
up left
wait 30000000
key x82
key xA0
wait 10000000
key x82
key xA2
wait 10000000
key enter
wait 40000000
'
[ "$bad" = 0 ] || exit 1
echo "the two front ends agree"
