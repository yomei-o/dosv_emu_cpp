#!/bin/sh
# Everything, in one go.
#
#     sh tools/check.sh              # the quick ones and the long ones
#     QUICK=1 sh tools/check.sh      # only the ones that take seconds
#
# This exists because a check that is not in a list like this one is a check
# that runs on the day it is written and never again.  tools/listcheck.sh was
# written for a bug, the bug came back, the notes said it was fixed, and the
# check had not been run since -- a whole afternoon to find the second time.
#
# The long ones drive the real program and take minutes each.
set -e
cd "$(dirname "$0")/.."

[ -x ./dosemu.exe ] || { echo "run sh build.sh first" >&2; exit 2; }
[ -f dosemu.js ] || { echo "run sh tools/build_wasm.sh first" >&2; exit 2; }

echo "=== the page's own controls (no guest)"
sh tools/barcheck.sh

echo "=== the file list the worker sends, and what it calls plotter output"
sh tools/listcheck.sh

[ -n "$QUICK" ] && { echo; echo "the quick checks passed"; exit 0; }

echo "=== a drawing uploaded, booted and drawn"
sh tools/filecheck.sh

echo "=== the browser build against the native one"
sh tools/wasmcheck.sh

echo "=== typing a name at the guest's prompt, through the page's worker"
NODE="${NODE:-$(command -v node 2>/dev/null)}"
[ -n "$NODE" ] || NODE=$(ls /c/prog/emsdk/emsdk/node/*/bin/node.exe 2>/dev/null | head -1)
"$NODE" tools/typecheck.mjs
SLOW=1 "$NODE" tools/typecheck.mjs

echo "=== プロッタ出力 → PDF, through the page's own plot.js"
sh tools/plotwebcheck.sh

echo "=== ひととおり: 起動 → 図面 → 保存 → プロッタ出力 → PDF → PDF を読む"
sh tools/flowcheck.sh

echo
echo "all checks passed"
