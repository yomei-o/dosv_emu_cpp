#!/bin/sh
# The whole way through, in one run of the real program.
#
#     sh tools/flowcheck.sh                 # SAMPLE1
#     DRAWING=SAMPLE0 sh tools/flowcheck.sh
#
#   boot -> open a .JWC -> save it -> plot it to a file -> PDF -> read the PDF
#
# Every piece of this has its own check already (savegrab.sh, plotrun.sh,
# plotwebcheck.sh), and every one of them starts the program fresh.  A person
# does not: they open a drawing, work on it, save, and then plot **the same
# session**.  That is the run this makes, because the pieces passing
# separately does not mean the chain holds -- the save leaves the program in
# 入出力, and whether ②ﾌﾟﾛｯﾀ can be reached from there is not something the
# separate checks can say.
#
# The last step reads the PDF back with tools/pdfread.mjs, which knows
# nothing about plot.js: it walks the cross-reference table the way a viewer
# does and reports what the page actually draws.  Then the lines and the text
# in the PDF are compared against the plotter file the original wrote.
#
# It takes a few minutes -- about two billion instructions.
set -e
cd "$(dirname "$0")/.."

ORIG="${ORIG:-../jwcad_dos_wasm/orig}"
FONT="${FONT:-../jwcad_dos_wasm/font}"
DRAWING="${DRAWING:-SAMPLE1}"
BOOT="${BOOT:-40000000}"
OUT="${OUT:-FLOW}"

[ -x ./dosemu.exe ] || { echo "run sh build.sh first" >&2; exit 2; }

NODE="${NODE:-}"
if [ -z "$NODE" ]; then
    command -v node > /dev/null 2>&1 && NODE=node
fi
if [ -z "$NODE" ]; then
    NODE=$(ls /c/prog/emsdk/emsdk/node/*/bin/node.exe 2>/dev/null | head -1)
fi
[ -n "$NODE" ] || { echo "no node (set NODE)" >&2; exit 2; }

rm -rf tmp/flow
mkdir -p tmp/flow/root
cp "$ORIG"/* tmp/flow/root/ 2>/dev/null || true
cp plot/WASM.JWP tmp/flow/root/

# A press, with the waits the original needs between them.  These are not
# guesses: they are savegrab.sh's and plotrun.sh's, which were measured.
P='mouse %d %d\nwait 3000000\ndown left\nwait 3000000\nup left\nwait %s\n'

{
    printf 'wait %s\n' "$BOOT"
    printf 'shot tmp/flow/1_open.raw\n'

    # --- save it (入出力 ①ファイル ①保存 ①選択確定 ... ①上書きする ①実行)
    printf "$P" 30 296 24000000
    printf "$P" 110 8 40000000
    printf "$P" 100 8 90000000
    printf "$P" 200 8 90000000
    i=0
    while [ $i -lt 8 ]; do printf 'key enter\nwait 30000000\n'; i=$((i + 1)); done
    printf 'wait 60000000\n'
    printf "$P" 280 8 90000000
    printf "$P" 210 8 400000000
    printf 'shot tmp/flow/2_saved.raw\n'

    # --- and plot it, **without restarting** (入出力 ②ﾌﾟﾛｯﾀ ③ﾌｧｲﾙ出力 ...)
    printf "$P" 30 296 26000000
    printf "$P" 220 8 26000000
    printf "$P" 400 8 26000000
    printf "$P" 150 "${ROWY:-136}" 26000000
    printf "$P" 200 8 26000000
    printf 'type %s\nwait 26000000\n' "$OUT"
    printf 'key enter\nwait 26000000\n'
    printf "$P" 140 8 26000000
    printf "$P" 164 8 26000000
    printf 'wait %s\n' "${TAIL:-400000000}"
    printf 'shot tmp/flow/3_plotted.raw\n'
} > tmp/flow/script.txt

# **The clock has to run**: ②ﾌﾟﾛｯﾀ paces its output by the time of day and
# waits for ever while INT 21h/2Ch keeps answering 12:00:00.
DOSEMU_CLOCK="${DOSEMU_CLOCK:-20000}" \
    ./dosemu.exe --root tmp/flow/root --font-ank "$FONT/JWANK16.FNT" \
    --font-kanji "$FONT/JWKAN16.FNT" --script tmp/flow/script.txt \
    tmp/flow/root/JW_CADV.EXE "$DRAWING.JWC" > tmp/flow/run.txt 2>&1 || true

fail=0
say() { echo "  $1 $2"; [ "$1" = ok ] || fail=1; }

# 1. the drawing was opened -- the program went graphic and drew something
[ -s tmp/flow/1_open.raw ] && say ok "$DRAWING.JWC opened" \
    || say FAIL "the program never got as far as a screen"

# 2. the save happened.  JW_CAD renames the old file .bak as it writes, so
#    the .bak appearing is the save, not a guess about the screen.
if [ -f "tmp/flow/root/$DRAWING.bak" ]; then
    say ok "saved ($(wc -c < "tmp/flow/root/$DRAWING.JWC" | tr -d ' ') bytes, .bak kept)"
else
    say FAIL "no save -- tmp/flow/root/$DRAWING.bak is not there"
fi

# 3. the plot came out, in the same session.  It is written by the guest, so
#    it is simply there on the disk the emulator was given -- no `getfile`,
#    which is the browser build's way of reaching into a disk that lives in
#    the .wasm.
[ -f "tmp/flow/root/$OUT" ] && cp "tmp/flow/root/$OUT" "tmp/flow/$OUT.PLT"
if [ -s "tmp/flow/$OUT.PLT" ]; then
    say ok "plotted ($(wc -l < "tmp/flow/$OUT.PLT" | tr -d ' ') lines in $OUT)"
else
    say FAIL "no plotter output -- $OUT was not written"
    echo "  (the last thing on the guest's top line:)"
    tail -3 tmp/flow/run.txt
    exit 1
fi

# 4. the PDF, and 5. reading it back
"$NODE" tools/plotpdf.mjs "tmp/flow/$OUT.PLT" tmp/flow/out > tmp/flow/pdf.txt 2>&1 \
    && say ok "PDF written ($(wc -c < tmp/flow/out.pdf | tr -d ' ') bytes)" \
    || { say FAIL "plotpdf failed"; cat tmp/flow/pdf.txt; exit 1; }

"$NODE" tools/pdfread.mjs tmp/flow/out.pdf > tmp/flow/read.json \
    && say ok "the PDF reads back (xref, trailer, page, fonts all sound)" \
    || { say FAIL "the PDF does not read back"; exit 1; }

# 6. and what it draws is what the original plotted
"$NODE" tools/plotvspdf.mjs "tmp/flow/$OUT.PLT" tmp/flow/read.json || fail=1

[ $fail -eq 0 ] && echo "  the whole way through works" || exit 1
