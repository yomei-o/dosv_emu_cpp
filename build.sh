#!/bin/sh
set -e
cd "$(dirname "$0")"
CXX=${CXX:-g++}
CXXFLAGS=${CXXFLAGS:--std=c++17 -O2 -Wall -Wextra}
# Everything but src/main_wasm.cpp, which is the browser front end and wants
# emscripten's headers.  (tools/build_wasm.sh leaves src/main.cpp out the same
# way.)  Without this the native build stopped at
# `emscripten/emscripten.h: No such file or directory`.
SRC=""
for f in src/*.cpp; do
    case "$f" in src/main_wasm.cpp) continue;; esac
    SRC="$SRC $f"
done
$CXX $CXXFLAGS -Isrc -o dosemu $SRC
echo "built dosemu"
