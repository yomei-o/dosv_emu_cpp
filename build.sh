#!/bin/sh
set -e
cd "$(dirname "$0")"
CXX=${CXX:-g++}
CXXFLAGS=${CXXFLAGS:--std=c++17 -O2 -Wall -Wextra}
$CXX $CXXFLAGS -Isrc -o dosemu src/*.cpp
echo "built dosemu"
