#!/bin/sh
# The browser build: the same emulator, with the guest's files baked in.
#
#     sh tools/build_wasm.sh        # writes dosemu.js + dosemu.wasm
#
# The guest files come from the port's checkout next door (the pair of repos
# is meant to sit side by side), because that is where the JW_CAD distribution
# and the DOS/V fonts are.  Nothing is copied into this repo: --embed-file puts
# them into the .wasm, under the same names the native runs use, so the page
# needs no server and no upload.
set -e
cd "$(dirname "$0")/.."

EMSDK="${EMSDK:-/c/prog/emsdk/emsdk}"
EMCC="$EMSDK/upstream/emscripten/emcc"
[ -f "$EMCC" ] || { echo "emcc not found at $EMCC (set EMSDK)" >&2; exit 2; }

ORIG="${ORIG:-../jwcad_dos_wasm/orig}"
FONT="${FONT:-../jwcad_dos_wasm/font}"
[ -f "$ORIG/JW_CADV.EXE" ] || { echo "$ORIG/JW_CADV.EXE not found (set ORIG)" >&2; exit 2; }
[ -f "$FONT/JWANK16.FNT" ] || { echo "$FONT/JWANK16.FNT not found (set FONT)" >&2; exit 2; }

EXPORTS=_main,_de_boot,_de_run,_de_frame,_de_width,_de_height,_de_graphics
EXPORTS=$EXPORTS,_de_dead,_de_message,_de_console,_de_load_seg
EXPORTS=$EXPORTS,_de_mouse,_de_button,_de_key,_de_mods
EXPORTS=$EXPORTS,_malloc,_free

# Everything but src/main.cpp, which is the native front end.
SRC=""
for f in src/*.cpp; do
    case "$f" in src/main.cpp) continue;; esac
    SRC="$SRC $f"
done

EMBED=""
# The program, its help and its palette, plus the drawings that ship with it.
for f in "$ORIG"/JW_CADV.EXE "$ORIG"/JW_PAL.DAT "$ORIG"/*.JWC "$ORIG"/JW_CADV.HLP; do
    [ -f "$f" ] || continue
    EMBED="$EMBED --embed-file $f@/orig/$(basename "$f")"
done
for f in "$FONT"/JWANK16.FNT "$FONT"/JWKAN16.FNT; do
    EMBED="$EMBED --embed-file $f@/font/$(basename "$f")"
done

STAMP=tmp/wasm.stamp
mkdir -p tmp
: > "$STAMP"

"$EMCC" -std=c++17 -O3 -Isrc $SRC $EMBED \
    -o dosemu.js \
    -s MODULARIZE=1 -s EXPORT_NAME=createDosemu \
    -s ENVIRONMENT=web,worker,node \
    -s ALLOW_MEMORY_GROWTH=1 -s INITIAL_MEMORY=67108864 \
    -s EXPORTED_RUNTIME_METHODS=UTF8ToString,stringToUTF8,lengthBytesUTF8,HEAPU8 \
    -s EXPORTED_FUNCTIONS="$EXPORTS" \
    -s DISABLE_EXCEPTION_CATCHING=0

if [ ! -f dosemu.wasm ] || [ ! dosemu.wasm -nt "$STAMP" ]; then
    echo "emcc did not rewrite dosemu.wasm - the build failed" >&2
    exit 1
fi
echo "built dosemu.js + dosemu.wasm"
