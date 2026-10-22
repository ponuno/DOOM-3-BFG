#!/bin/sh
# Builds n3dstex for Linux (g++) and, with llvm-mingw (github.com/mstorsjo/llvm-mingw) in
# /opt/llvm-mingw or LLVM_MINGW, as a static Windows .exe. Output: ./out/
set -e
cd "$(dirname "$0")"
mkdir -p out out/obj-linux out/obj-win
SRC="n3dstex.cpp n3dssnd.cpp n3dsslim.cpp n3dsvid.cpp rg_etc1.cpp"
BINK_CPP="libbinkdec/src/BinkAudio.cpp libbinkdec/src/BinkDecoder.cpp libbinkdec/src/BinkVideo.cpp libbinkdec/src/BitReader.cpp libbinkdec/src/FileStream.cpp libbinkdec/src/HuffmanVLC.cpp libbinkdec/src/LogError.cpp libbinkdec/src/Util.cpp"
BINK_C="libbinkdec/src/avfft.c libbinkdec/src/dct.c libbinkdec/src/dct32.c libbinkdec/src/fft.c libbinkdec/src/mdct.c libbinkdec/src/rdft.c"
build() {	# cc cxx objdir out
	objs=""
	for f in $BINK_C; do o="$3/$(basename $f).o"; $1 -O2 -Ilibbinkdec/include -c $f -o $o; objs="$objs $o"; done
	for f in $BINK_CPP; do o="$3/$(basename $f).o"; $2 -O2 -std=c++17 -Ilibbinkdec/include -c $f -o $o; objs="$objs $o"; done
	$2 -O2 -std=c++17 -Ilibbinkdec/include $5 -o $4 $SRC $objs $6
}
build gcc g++ out/obj-linux out/n3dstex "" -lpthread
LM="${LLVM_MINGW:-/opt/llvm-mingw}"
if [ -x "$LM/bin/x86_64-w64-mingw32-clang++" ]; then
	build "$LM/bin/x86_64-w64-mingw32-clang" "$LM/bin/x86_64-w64-mingw32-clang++" out/obj-win out/n3dstex.exe -static ""
fi
ls -la out/n3dstex out/n3dstex.exe 2>/dev/null
