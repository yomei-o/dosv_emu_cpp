#!/bin/sh
# The page's three file controls, driven headlessly (tools/barcheck.mjs).
#
#     sh tools/barcheck.sh
#
# index.html's script is run against a stand-in document and worker, and put
# through what a visitor does: a drawing appears on the guest's disk, it is
# marked and picked out, ダウンロード takes it, choosing another name does
# **not** boot anything, and アップロード does not either.
#
# That last pair is what this is for.  The list started life doubling as
# "which drawing to start on", so choosing a name to download rebooted the
# emulator and threw away whatever was being drawn.
set -e
cd "$(dirname "$0")/.."
NODE="${NODE:-}"
if [ -z "$NODE" ]; then
    command -v node > /dev/null 2>&1 && NODE=node
fi
if [ -z "$NODE" ]; then
    NODE=$(ls /c/prog/emsdk/emsdk/node/*/bin/node.exe 2>/dev/null | head -1)
fi
[ -n "$NODE" ] || { echo "no node (set NODE)" >&2; exit 2; }
"$NODE" tools/barcheck.mjs
