#!/bin/sh
# Builds n3dstex for Linux (g++) and, with llvm-mingw (github.com/mstorsjo/llvm-mingw) in
# /opt/llvm-mingw or LLVM_MINGW, as a static Windows .exe. Output: ./out/
set -e
cd "$(dirname "$0")"
mkdir -p out
g++ -O2 -std=c++17 -o out/n3dstex n3dstex.cpp n3dssnd.cpp n3dsslim.cpp rg_etc1.cpp -lpthread
LM="${LLVM_MINGW:-/opt/llvm-mingw}"
if [ -x "$LM/bin/x86_64-w64-mingw32-clang++" ]; then
	"$LM/bin/x86_64-w64-mingw32-clang++" -O2 -std=c++17 -static -o out/n3dstex.exe n3dstex.cpp n3dssnd.cpp n3dsslim.cpp rg_etc1.cpp
fi
ls -la out
