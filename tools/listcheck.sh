#!/bin/sh
# Does the page's file list notice a drawing the guest has just saved?
#
#     sh tools/listcheck.sh
#
# The visitor saves a drawing from inside JW_CAD and then wants to pick it,
# so the list the page offers has to grow.  This runs dosemu-worker.js itself
# under a small Worker shim, puts a file on the guest's disk once it is up,
# and fails if no listing carries it (tools/listcheck.mjs).
set -e
cd "$(dirname "$0")/.."

[ -f dosemu.js ] || { echo "run sh tools/build_wasm.sh first" >&2; exit 2; }

NODE="${NODE:-}"
if [ -z "$NODE" ]; then
    command -v node > /dev/null 2>&1 && NODE=node
fi
if [ -z "$NODE" ]; then
    NODE=$(ls /c/prog/emsdk/emsdk/node/*/bin/node.exe 2>/dev/null | head -1)
fi
[ -n "$NODE" ] || { echo "no node (set NODE)" >&2; exit 2; }

"$NODE" tools/listcheck.mjs
