#!/bin/sh
# Drive the real JW_CAD through its plotter output and keep what it writes.
#
#     sh tools/plotrun.sh                       # SAMPLE0 through plot/WASM.JWP
#     DRAWING=TEST6 sh tools/plotrun.sh
#     STEPS=4 sh tools/plotrun.sh               # stop after four presses
#
# 入出力 → ②ﾌﾟﾛｯﾀ → ③ﾌｧｲﾙ出力 asks which `*.JWP` to use -- a plotter
# definition, which says what language the plotter speaks (orig/JWP.DOC).
# plot/WASM.JWP is one of our own: instead of HP-GL it writes one plain line
# per thing drawn, which is what the browser turns into a PDF and a PNG.
#
# Everything happens in tmp/proot, a copy of the original's directory, so the
# files that ship are never written to.
set -e
cd "$(dirname "$0")/.."
ORIG="${ORIG:-../jwcad_dos_wasm/orig}"
FONT="${FONT:-../jwcad_dos_wasm/font}"
DRAWING="${DRAWING-SAMPLE0}"
# DRAWING= (empty) starts the program with no drawing, the way the page does.
ARG=""
[ -n "$DRAWING" ] && ARG="$DRAWING.JWC"
BOOT="${BOOT:-40000000}"
WAIT="${WAIT:-26000000}"
STEPS="${STEPS:-99}"
[ -x ./dosemu.exe ] || { echo "build first (sh build.sh)" >&2; exit 2; }

rm -rf tmp/proot
mkdir -p tmp/proot tmp/plot
cp "$ORIG"/* tmp/proot/ 2>/dev/null || true
cp plot/WASM.JWP tmp/proot/

# What to do, in order: "x y button" for a press, "key NAME" for a key.  The
# columns come from the bars the original writes (jwcad_dos_wasm's RESUME
# 4.44); the row at y=136 is the second file in ③ﾌｧｲﾙ出力's list.
: > tmp/plot/steps.txt
cat >> tmp/plot/steps.txt <<STEPS
30 296 left
220 8 left
400 8 left
150 ${ROWY:-136} left
200 8 left
type PLOT
key enter
140 8 left
164 8 left
STEPS

{
    printf 'wait %s\n' "$BOOT"
    n=0
    while read -r x y b; do
        n=$((n + 1))
        [ "$n" -le "$STEPS" ] || break
        if [ "$x" = type ]; then
            printf 'type %s
wait %s
' "$y" "$WAIT"
        elif [ "$x" = key ]; then
            printf 'key %s\nwait %s\n' "$y" "$WAIT"
        else
            printf 'mouse %s %s\nwait 3000000\ndown %s\nwait 3000000\nup %s\nwait %s\n' \
                "$x" "$y" "$b" "$b" "$WAIT"
        fi
        printf 'shot ./tmp/plot/step%s.raw\n' "$n"
    done < tmp/plot/steps.txt
    # The plot itself takes a while, and the file is only closed when it is
    # finished, so the run has to outlast it.
    printf 'wait %s
shot ./tmp/plot/done.raw
' "${TAIL:-400000000}"
} > tmp/plot/script.txt

# **The clock has to run.**  ②ﾌﾟﾛｯﾀ paces its output by the time of day and
# waits for ever while INT 21h/2Ch keeps answering 12:00:00 -- which is what
# it does unless DOSEMU_CLOCK says how many instructions a hundredth of a
# second is worth.  Everything else in this repo was measured with the clock
# standing still, so it stays off by default.
DOSEMU_CLOCK="${DOSEMU_CLOCK:-20000}" \
DOSEMU_BP='+0DEF:23C5' DOSEMU_BPSTR=2 DOSEMU_BPN=20000 \
    ./dosemu.exe --root tmp/proot --font-ank "$FONT/JWANK16.FNT" \
    --font-kanji "$FONT/JWKAN16.FNT" \
    --script tmp/plot/script.txt tmp/proot/JW_CADV.EXE $ARG \
    > tmp/plot/str.txt 2>&1 || true

grep '\[bp\]' tmp/plot/str.txt \
  | sed 's/.*args [0-9A-F]* [0-9A-F]* [0-9A-F]* \([0-9A-F]*\) \([0-9A-F]*\) \([0-9A-F]*\) \([0-9A-F]*\).*\("[^"]*"\)$/col=\1 row=\2 fg=\3 \5/' \
  | tail -20 | iconv -f CP932 -t UTF-8 2>/dev/null || true
echo "--- what is in tmp/proot that was not in $ORIG ---"
for f in tmp/proot/*; do
    b=$(basename "$f")
    [ -f "$ORIG/$b" ] || echo "  new: $b  $(wc -c < "$f") bytes"
done
