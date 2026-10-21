#!/usr/bin/env bash
# Builds a devkitARM-equivalent toolchain for the 3DS port on Linux from sources
# that are reachable through GitHub only (no pkg.devkitpro.org, no gnu.org, no apt):
#   binutils 2.46 (devkitPro fork), GCC 16.1.0 (gcc-mirror) + devkitPro patch,
#   newlib (devkitPro fork), crtls, rules, CMake files, libctru, citro3d,
#   picasso, 3dsxtool/smdhtool, bin2s.
# Host libraries GCC needs (gmp, mpfr, mpc) and flex are built into $HOST.
# Only the default and armv6k/fpu (3DS) multilibs are built to save time.
#
# Result: /opt/devkitpro laid out like the real devkitPro; then
#   . ~/devkitpro-env.sh
#   cmake -S neo -B build -G "Unix Makefiles" -DCMAKE_MAKE_PROGRAM=/usr/bin/make \
#         -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake
#   cmake --build build -- -k -j3
#
# Needs: gcc/g++, make, cmake, autoconf, automake, libtool, bison, m4, perl, pkg-config, git, curl.
set -euo pipefail

SRC=${SRC:-$HOME/src}
BLD=${BLD:-$HOME/build}
HOST=${HOST:-$HOME/opt/host}
DKP=/opt/devkitpro
DKA=$DKP/devkitARM
T=arm-none-eabi
J=${J:-$(nproc)}
GCC_VER=16.1.0

mkdir -p "$SRC" "$BLD" "$HOST" "$DKA" "$DKP/tools/bin" "$DKP/portlibs/3ds/bin" "$DKP/portlibs/3ds/lib" "$DKP/portlibs/3ds/include"
clone() { [ -d "$SRC/$2" ] || GIT_LFS_SKIP_SMUDGE=1 git clone -q --depth 1 ${3:+-b "$3"} "https://github.com/$1" "$SRC/$2"; }

# ---- sources ----
clone devkitPro/buildscripts buildscripts
clone alisw/GMP gmp v6.2.1
clone BrianGladman/mpfr bg-mpfr
clone BrianGladman/mpc bg-mpc
clone devkitPro/binutils-gdb binutils devkitPro-binutils-2.46
clone gcc-mirror/gcc gcc-$GCC_VER releases/gcc-$GCC_VER
clone devkitPro/newlib newlib devkitPro
for r in libctru citro3d picasso 3dstools general-tools devkitarm-rules devkitarm-crtls pacman-packages; do clone devkitPro/$r $r; done
if [ ! -d "$SRC/flex-2.6.4" ]; then
	curl -sSL -o "$SRC/flex-2.6.4.tar.gz" https://github.com/westes/flex/releases/download/v2.6.4/flex-2.6.4.tar.gz
	tar -C "$SRC" -xf "$SRC/flex-2.6.4.tar.gz"
fi

# ---- host libraries and flex ----
hostlib() {	# name srcdir [configure args]
	local n=$1 s=$2; shift 2
	[ -f "$BLD/$n/.done" ] && return
	mkdir -p "$BLD/$n"; ( cd "$BLD/$n" && "$s/configure" --prefix="$HOST" --disable-shared "$@" MAKEINFO=true \
		&& make -j"$J" MAKEINFO=true && make install MAKEINFO=true ) && touch "$BLD/$n/.done"
}
hostlib gmp "$SRC/gmp" --enable-static
[ -f "$SRC/bg-mpfr/configure" ] || ( cd "$SRC/bg-mpfr" && autoreconf -fi )
hostlib mpfr "$SRC/bg-mpfr" --with-gmp="$HOST"
[ -f "$SRC/bg-mpc/configure" ] || ( cd "$SRC/bg-mpc" && autoreconf -fi )
hostlib mpc "$SRC/bg-mpc" --with-gmp="$HOST" --with-mpfr="$HOST"
hostlib flex "$SRC/flex-2.6.4" --disable-nls
export PATH=$DKA/bin:$DKP/tools/bin:$HOST/bin:$PATH

# ---- binutils ----
if [ ! -f "$BLD/binutils/.done" ]; then
	mkdir -p "$BLD/binutils"; cd "$BLD/binutils"
	"$SRC/binutils/configure" --prefix=$DKA --target=$T --disable-nls --disable-werror --disable-shared \
		--disable-debug --enable-lto --enable-plugins --enable-poison-system-directories \
		--disable-gdb --disable-gdbserver --disable-sim --disable-gprofng --disable-readline \
		--disable-libdecnumber --with-system-zlib MAKEINFO=true
	make -j"$J" MAKEINFO=true && make install MAKEINFO=true && touch .done
fi

# ---- gcc: devkitPro patch, only default + armv6k/fpu multilibs ----
G=$SRC/gcc-$GCC_VER
if [ ! -f "$G/.dkp-patched" ]; then
	( cd "$G" && patch -p1 -s < "$SRC/buildscripts/patches/gcc-$GCC_VER-1.patch" )
	sed -i -e 's#^MULTILIB_REQUIRED .*#MULTILIB_REQUIRED    = march=armv6k/mfloat-abi=hard#' \
	       -e '/^MULTILIB_REUSE/d' "$G/gcc/config/arm/t-arm-elf"
	touch "$G/.dkp-patched"
fi
mkdir -p "$BLD/gcc"; cd "$BLD/gcc"
if [ ! -f configured ]; then
	CFLAGS_FOR_TARGET="-O2 -ffunction-sections -fdata-sections" \
	CXXFLAGS_FOR_TARGET="-O2 -ffunction-sections -fdata-sections" LDFLAGS_FOR_TARGET="" \
	"$G/configure" --target=$T --prefix=$DKA --enable-languages=c,c++,lto \
		--with-gnu-as --with-gnu-ld --with-gcc --enable-cxx-flags='-ffunction-sections' \
		--disable-libstdcxx-verbose --enable-poison-system-directories --enable-threads=posix \
		--disable-win32-registry --disable-nls --disable-debug --disable-libmudflap --disable-libssp \
		--disable-libgomp --disable-libstdcxx-pch --enable-libstdcxx-time=yes \
		--enable-libstdcxx-filesystem-ts --with-newlib=yes --with-native-system-header-dir=/include \
		--with-sysroot=$DKA/$T --enable-lto --disable-tm-clone-registry --disable-__cxa_atexit \
		--with-bugurl="https://devkitpro.org" --with-march=armv4t --enable-interwork --enable-multilib \
		--with-pkgversion="devkitARM" --with-gmp="$HOST" --with-mpfr="$HOST" --with-mpc="$HOST" \
		--without-isl --disable-bootstrap --with-system-zlib MAKEINFO=true
	touch configured
fi
[ -f s1 ] || { make -j"$J" all-gcc MAKEINFO=true && make install-gcc MAKEINFO=true && touch s1; }

# ---- newlib ----
mkdir -p "$BLD/newlib"; cd "$BLD/newlib"
[ -f .done ] || { env -u CC -u CXX -u CFLAGS CFLAGS_FOR_TARGET="-O2 -ffunction-sections -fdata-sections" \
	"$SRC/newlib/configure" --disable-newlib-supplied-syscalls --enable-newlib-mb \
	--disable-newlib-wide-orient --enable-newlib-register-fini --target=$T --prefix=$DKA MAKEINFO=true \
	&& make -j"$J" MAKEINFO=true && make install -j1 MAKEINFO=true && touch .done; }

# ---- gcc stage 2; regenerate limits.h so it chains to newlib's (PATH_MAX) ----
cd "$BLD/gcc"
if [ ! -f s2 ]; then
	rm -f gcc/stmp-int-hdrs gcc/stmp-fixinc gcc/include/limits.h; rm -rf gcc/include-fixed
	make -j"$J" MAKEINFO=true && make install-strip MAKEINFO=true && touch s2
fi

# ---- devkitPro pieces ----
export DEVKITPRO=$DKP DEVKITARM=$DKA
( cd "$SRC/devkitarm-rules" && make install )
( cd "$SRC/devkitarm-crtls" && make install )
P=$SRC/pacman-packages/cmake
mkdir -p $DKP/cmake/Platform
cp $P/common-utils/dkp-*.cmake $DKP/cmake/; cp $P/common-utils/Generic-dkP.cmake $DKP/cmake/Platform/
cp $P/devkitarm/devkitARM.cmake $P/devkitarm/dkp-arm-mode-wrapper $P/devkitarm/dkp-gba-ds-*.cmake $DKP/cmake/
cp $P/devkitarm/arm-none-eabi-cmake $DKA/bin/
cp $P/3ds/3DS.cmake $DKP/cmake/; cp $P/3ds/Nintendo3DS.cmake $DKP/cmake/Platform/
cp $P/3ds/arm-none-eabi-cmake $DKP/portlibs/3ds/bin/
install -m755 "$SRC/pacman-packages/3ds/pkg-config/pkg-config.in" $DKP/portlibs/3ds/bin/arm-none-eabi-pkg-config

( cd "$SRC/picasso" && g++ -O1 -std=gnu++11 -DPACKAGE_STRING='"picasso"' -DPACKAGE_VERSION='"2.7"' \
	-o $DKP/tools/bin/picasso source/*.cpp )
for t in 3dstools general-tools; do
	[ -f "$BLD/$t/.done" ] && continue
	( cd "$SRC/$t" && ./autogen.sh ) >/dev/null
	mkdir -p "$BLD/$t"; ( cd "$BLD/$t" && "$SRC/$t/configure" --prefix=$DKP/tools && make -j"$J" && make install ) && touch "$BLD/$t/.done"
done

# CIA tools: makerom (3DSGuy/Project_CTR) and bannertool (diasurgical/bannertool)
clone 3DSGuy/Project_CTR Project_CTR
[ -d "$SRC/bannertool" ] || git clone -q --depth 1 --recurse-submodules --shallow-submodules https://github.com/diasurgical/bannertool "$SRC/bannertool"
( cd "$SRC/Project_CTR/makerom" && make deps -j"$J" && make -j"$J" ) && install -m755 "$SRC/Project_CTR/makerom/bin/makerom" $DKP/tools/bin/
( cd "$SRC/bannertool" && make -j"$J" ) && install -m755 "$SRC/bannertool/output/linux-x86_64/bannertool" $DKP/tools/bin/

( cd "$SRC/libctru/libctru" && make -j"$J" && make install )
( cd "$SRC/citro3d" && make -j"$J" && make install )

cat > "$HOME/devkitpro-env.sh" <<EOF
export DEVKITPRO=$DKP
export DEVKITARM=$DKA
export PATH=$DKA/bin:$DKP/tools/bin:\$PATH
EOF
echo "devkitARM ready: . ~/devkitpro-env.sh"
