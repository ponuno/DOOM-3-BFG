#!/bin/sh
# Builds n3dstex for Linux (g++) and, with llvm-mingw (github.com/mstorsjo/llvm-mingw) in
# /opt/llvm-mingw or LLVM_MINGW, for Windows: n3dstex.exe (a window without arguments, the
# command line with them) and n3dstex-cli.exe (console only, for scripts). Output: ./out/
set -e
cd "$(dirname "$0")"
mkdir -p out out/obj-linux out/obj-win
SRC="n3dstex.cpp n3dssnd.cpp n3dsslim.cpp n3dsvid.cpp rg_etc1.cpp"
BINK_CPP="libbinkdec/src/BinkAudio.cpp libbinkdec/src/BinkDecoder.cpp libbinkdec/src/BinkVideo.cpp libbinkdec/src/BitReader.cpp libbinkdec/src/FileStream.cpp libbinkdec/src/HuffmanVLC.cpp libbinkdec/src/LogError.cpp libbinkdec/src/Util.cpp"
BINK_C="libbinkdec/src/avfft.c libbinkdec/src/dct.c libbinkdec/src/dct32.c libbinkdec/src/fft.c libbinkdec/src/mdct.c libbinkdec/src/rdft.c"
bink() {	# cc cxx objdir -> $BINK_OBJS
	BINK_OBJS=""
	for f in $BINK_C; do o="$3/$(basename $f).o"; $1 -O2 -Ilibbinkdec/include -c $f -o $o; BINK_OBJS="$BINK_OBJS $o"; done
	for f in $BINK_CPP; do o="$3/$(basename $f).o"; $2 -O2 -std=c++17 -Wno-multichar -Ilibbinkdec/include -c $f -o $o; BINK_OBJS="$BINK_OBJS $o"; done
}
bink gcc g++ out/obj-linux
g++ -O2 -std=c++17 -Wno-multichar -Ilibbinkdec/include -o out/n3dstex $SRC $BINK_OBJS -lpthread
LM="${LLVM_MINGW:-/opt/llvm-mingw}"
if [ -x "$LM/bin/x86_64-w64-mingw32-clang++" ]; then
	CXX="$LM/bin/x86_64-w64-mingw32-clang++"
	bink "$LM/bin/x86_64-w64-mingw32-clang" "$CXX" out/obj-win
	"$LM/bin/x86_64-w64-mingw32-windres" n3dstex.rc -O coff -o out/obj-win/n3dstex.res.o
	$CXX -O2 -std=c++17 -Wno-multichar -Ilibbinkdec/include -static -o out/n3dstex-cli.exe $SRC $BINK_OBJS out/obj-win/n3dstex.res.o
	$CXX -O2 -std=c++17 -Wno-multichar -Ilibbinkdec/include -DN3DSTEX_GUI -static -mwindows -o out/n3dstex.exe $SRC n3dsgui_win32.cpp \
		$BINK_OBJS out/obj-win/n3dstex.res.o -lcomctl32 -lole32 -luuid -lshell32 -ladvapi32
fi
ls -la out/n3dstex out/n3dstex.exe out/n3dstex-cli.exe 2>/dev/null
