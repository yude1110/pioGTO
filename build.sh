#!/bin/sh
# macOS / Linux build
cd "$(dirname "$0")"
CXX=${CXX:-c++}
case "$(uname -m)" in arm64|aarch64) ARCH="-mcpu=native";; *) ARCH="-march=native";; esac
$CXX -O3 -std=c++17 $ARCH -pthread solver.cpp -o solver && echo "OK: ./solver config.txt"
