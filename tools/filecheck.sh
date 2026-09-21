#!/bin/sh
# The page's upload and download, checked against the native emulator.
#
#     sh tools/filecheck.sh                 # SAMPLE6, as UPLOAD.JWC
#     DRAWING=SAMPLE0 WAIT=60000000 sh tools/filecheck.sh
#
# index.html's アップロード writes the visitor's bytes into the browser build's
# own filesystem under orig/ and boots the guest on that name; ダウンロード
# reads them back out of the same place.  There is no separate code path for
# either -- the guest opens and saves through DOS, and DOS is that filesystem
# -- so the thing worth checking is that a drawing arriving that way is opened
# exactly as one that was there all along.
#
# Both sides are given the same bytes under the same name, because JW_CAD puts
# the name it was started with on the screen: comparing UPLOAD.JWC against
# SAMPLE6.JWC would differ in the band and say nothing about the file.
set -e
cd "$(dirname "$0")/.."
mkdir -p tmp/fc

[ -f dosemu.js ] || { echo "run sh tools/build_wasm.sh first" >&2; exit 2; }
[ -x dosemu.exe ] || { echo "run sh build.sh first" >&2; exit 2; }

ORIG="${ORIG:-../jwcad_dos_wasm/orig}"
FONT="${FONT:-../jwcad_dos_wasm/font}"
DRAWING="${DRAWING:-SAMPLE6}"
WAIT="${WAIT:-150000000}"
AS="${AS:-UPLOAD.JWC}"

NODE="${NODE:-}"
if [ -z "$NODE" ]; then
    command -v node > /dev/null 2>&1 && NODE=node
fi
if [ -z "$NODE" ]; then
    NODE=$(ls /c/prog/emsdk/emsdk/node/*/bin/node.exe 2>/dev/null | head -1)
fi
[ -n "$NODE" ] || { echo "no node (set NODE)" >&2; exit 2; }

"$NODE" tools/filecheck.mjs "$ORIG/$DRAWING.JWC" "$AS" "$WAIT" tmp/fc/wasm.raw

# The native side gets its own root so that nothing is written into the
# distribution, with the drawing under the uploaded name.
rm -rf tmp/fc/root
cp -r "$ORIG" tmp/fc/root
cp "$ORIG/$DRAWING.JWC" "tmp/fc/root/$AS"
printf 'wait %s\nshot tmp/fc/native.raw\n' "$WAIT" > tmp/fc/script.txt
./dosemu.exe --root tmp/fc/root --font-ank "$FONT/JWANK16.FNT" \
    --font-kanji "$FONT/JWKAN16.FNT" --script tmp/fc/script.txt \
    "tmp/fc/root/JW_CADV.EXE" "$AS" > /dev/null 2>&1

if cmp -s tmp/fc/wasm.raw tmp/fc/native.raw; then
    echo "  same   $DRAWING.JWC uploaded as $AS"
else
    echo "  DIFFER $DRAWING.JWC uploaded as $AS"
    exit 1
fi
