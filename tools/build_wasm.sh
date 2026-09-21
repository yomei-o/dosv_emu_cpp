#!/bin/sh
# The browser build: the same emulator, with the guest's files baked in.
#
#     sh tools/build_wasm.sh        # writes dosemu.js + dosemu.wasm
#
# The guest files come from the port's checkout next door (the pair of repos
# is meant to sit side by side), because that is where the JW_CAD distribution
# and the DOS/V fonts are.  Nothing is copied into this repo: --embed-file puts
# them into the .wasm, under the same names the native runs use, so the page
# needs no server to start.
#
# `FS` is exported because the guest's disk is the page's disk: a drawing the
# visitor uploads is written into orig/ before the guest is booted on it, and a
# drawing the guest saved is read back out of orig/ to download.  Going through
# the filesystem rather than adding an entry point is deliberate -- the program
# opens and saves through DOS, so a file that arrives this way is a file that
# arrives the way a real one would.
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
EXPORTS=$EXPORTS,_de_mouse,_de_button,_de_key,_de_mods,_de_clock
EXPORTS=$EXPORTS,_malloc,_free

# Everything but src/main.cpp, which is the native front end.
SRC=""
for f in src/*.cpp; do
    case "$f" in src/main.cpp) continue;; esac
    SRC="$SRC $f"
done

EMBED=""
# The program, its help and its palette, plus the drawings that ship with it.
#
# **SAMPLE*/TEST* and not *.JWC.** jwcv222h.lzh holds exactly fourteen
# drawings, SAMPLE0-6 and TEST1-7, and those are the ones a visitor should
# find. But the port's orig/ is also where the drawings written while
# analysing go -- what JW_CAD saves when it runs here (AUTO.JWC), and the
# question drawings made to ask the original something (QPICK, QBYTES,
# ONE2). They are in the port's .gitignore, so they are not in either
# repository; a plain *.JWC baked them into the .wasm anyway, and then the
# page listed AUTO.JWC first -- alphabetical -- and it looked for all the
# world as though the program had opened it at boot.
for f in "$ORIG"/JW_CADV.EXE "$ORIG"/JW_PAL.DAT "$ORIG"/SAMPLE*.JWC \
         "$ORIG"/TEST*.JWC "$ORIG"/JW_CADV.HLP; do
    [ -f "$f" ] || continue
    EMBED="$EMBED --embed-file $f@/orig/$(basename "$f")"
done
# And the plotter definitions: the one that ships plus **our own**, which is
# what makes 入出力 → ②ﾌﾟﾛｯﾀ → ③ﾌｧｲﾙ出力 write something the page can turn
# into a PDF and a PNG (plot/WASM.JWP, plot.js).
for f in "$ORIG"/*.JWP plot/*.JWP; do
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
    -s EXPORTED_RUNTIME_METHODS=UTF8ToString,stringToUTF8,lengthBytesUTF8,HEAPU8,FS \
    -s EXPORTED_FUNCTIONS="$EXPORTS" \
    -s DISABLE_EXCEPTION_CATCHING=0

if [ ! -f dosemu.wasm ] || [ ! dosemu.wasm -nt "$STAMP" ]; then
    echo "emcc did not rewrite dosemu.wasm - the build failed" >&2
    exit 1
fi
# **The stamp the page puts on the file names.**  Without it a browser that
# has read dosemu-worker.js, dosemu.js and dosemu.wasm once keeps using them,
# and a push changes nothing for anyone who has been to the page before.
BUILD=$(date +%Y%m%d%H%M%S)
sed -i "s/const DE_BUILD = '[^']*'/const DE_BUILD = '$BUILD'/" index.html
grep -q "DE_BUILD = '$BUILD'" index.html || {
    echo "the build stamp did not go into index.html" >&2; exit 1; }

echo "built dosemu.js + dosemu.wasm"
