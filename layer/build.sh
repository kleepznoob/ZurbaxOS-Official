#!/bin/bash
# Builds the Steam layer: the standard Linux pieces Steam and Proton need,
# compiled from their official upstream sources (no distribution packages).
#
#   layer/build.sh [PACKAGE...]    all packages, or just these
#
# ZOS itself doesn't use any of this. The layer is a separate root file
# system (glibc, the GCC runtime, a small GNU userland, X11 with Xwayland,
# and Mesa's graphics drivers, 64- and 32-bit) that ZOS starts Steam in.
#
# Everything lands in build/layer/root, laid out like most distributions
# (64-bit libraries in /usr/lib, 32-bit in /usr/lib32). Sources are pinned
# in sources.txt with their SHA-256 sums; finished packages leave a stamp in
# build/layer/stamps so a rerun picks up where it stopped.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/.." && pwd)
L=${ZOS_LAYER_DIR:-$TOP/build/layer}
SRC=$L/src WORK=$L/work ROOT=$L/root STAMPS=$L/stamps
JOBS=${JOBS:-$(nproc)}
KVER=6.18.55
mkdir -p "$SRC" "$WORK" "$ROOT" "$STAMPS"

say() { printf '>> %s\n' "$*" >&2; }

# ---- Sources ---------------------------------------------------------------------

# sources.txt: NAME URL SHA256 (one per line; "-" for a checksum not recorded yet)
fetch() {
	local name=$1 line url sum file
	line=$(grep -E "^$name " "$HERE/sources.txt") || { echo "no source for $name" >&2; exit 1; }
	read -r _ url sum <<<"$line"
	file=$SRC/$(basename "$url")
	if [ ! -f "$file" ]; then
		say "downloading $(basename "$url")"
		curl -fsSL --retry 4 -o "$file.part" "$url"
		mv "$file.part" "$file"
	fi
	local have
	have=$(sha256sum "$file" | cut -d' ' -f1)
	if [ "$sum" = "-" ]; then
		# first download: record it, so later builds get exactly the same file
		sed -i "s|^$name $url -$|$name $url $have|" "$HERE/sources.txt"
	elif [ "$have" != "$sum" ]; then
		echo "checksum mismatch for $file" >&2
		exit 1
	fi
	echo "$file"
}

# Unpacks a package into a fresh work folder and enters it.
unpack() {
	local name=$1 file dir
	file=$(fetch "$name")
	rm -rf "${WORK:?}/$name"
	mkdir -p "$WORK/$name"
	tar -xf "$file" -C "$WORK/$name"
	dir=$(find "$WORK/$name" -mindepth 1 -maxdepth 1 -type d | head -1)
	cd "$dir"
}

# ---- Toolchain settings ------------------------------------------------------------
# Packages after glibc are compiled with the build machine's compiler against
# the layer itself (--sysroot), so they use the layer's glibc and headers.

use64() {
	ARCH=64 LIBDIR=/usr/lib HOST=x86_64-pc-linux-gnu
	export CC="gcc --sysroot=$ROOT" CXX="g++ --sysroot=$ROOT"
	export PKG_CONFIG_LIBDIR=$ROOT/usr/lib/pkgconfig:$ROOT/usr/share/pkgconfig PKG_CONFIG_SYSROOT_DIR=$ROOT
	export CFLAGS="-O2 -pipe -fno-plt" CXXFLAGS="-O2 -pipe -fno-plt" LDFLAGS="-Wl,-O1"
}

use32() {
	ARCH=32 LIBDIR=/usr/lib32 HOST=i686-pc-linux-gnu
	export CC="gcc -m32 --sysroot=$ROOT" CXX="g++ -m32 --sysroot=$ROOT"
	export PKG_CONFIG_LIBDIR=$ROOT/usr/lib32/pkgconfig:$ROOT/usr/share/pkgconfig PKG_CONFIG_SYSROOT_DIR=$ROOT
	export CFLAGS="-O2 -pipe -march=i686 -mstackrealign" CXXFLAGS="-O2 -pipe -march=i686 -mstackrealign" LDFLAGS="-Wl,-O1"
}

# A meson cross file for 32-bit builds (pkg-config and the compilers above).
meson_cross32() {
	cat >"$WORK/cross32.ini" <<EOF
[binaries]
c = ['gcc', '-m32', '--sysroot=$ROOT']
cpp = ['g++', '-m32', '--sysroot=$ROOT']
pkg-config = 'pkg-config'
strip = 'strip'
[built-in options]
libdir = 'lib32'
[host_machine]
system = 'linux'
cpu_family = 'x86'
cpu = 'i686'
endian = 'little'
EOF
	echo "$WORK/cross32.ini"
}

# configure && make && make install for autotools packages, either width.
autobuild() {
	local extra=("$@")
	./configure --prefix=/usr --libdir=$LIBDIR --host=$HOST --sysconfdir=/etc --localstatedir=/var \
		--disable-static "${extra[@]}" >configure.log
	make -j"$JOBS" >make.log
	make DESTDIR="$ROOT" install >install.log
	# libtool archives point at build paths; nothing needs them
	find "$ROOT$LIBDIR" -maxdepth 1 -name '*.la' -delete
}

mesonbuild() {
	local opts=("$@") cross=()
	[ "$ARCH" = 32 ] && cross=(--cross-file "$(meson_cross32)")
	meson setup _build --prefix=/usr --libdir="${LIBDIR#/usr/}" --sysconfdir=/etc --localstatedir=/var \
		--buildtype=release -Ddefault_library=shared --wrap-mode=nodownload "${cross[@]}" "${opts[@]}" >meson.log
	ninja -C _build -j"$JOBS" >ninja.log
	DESTDIR="$ROOT" ninja -C _build install >install.log
}

# Runs a package build once; "name" or "name:32" for the 32-bit build.
build() {
	local pkg=$1 stamp=$STAMPS/$1
	[ -f "$stamp" ] && return 0
	say "building $pkg"
	local fn=pkg_${pkg%%:*} width=${pkg#*:}
	[ "$width" = "$pkg" ] && width=64
	(
		if [ "$width" = 32 ]; then use32; else use64; fi
		"$fn"
	)
	touch "$stamp"
	rm -rf "${WORK:?}/${pkg%%:*}"
}

# ---- The file system layout -----------------------------------------------------

pkg_layout() {
	cd "$ROOT"
	# data: ZOS's home folders; .zos: layerrun's private files for each session
	mkdir -p usr/bin usr/lib usr/lib32 usr/include usr/share etc var/tmp tmp home root proc sys dev run data .zos
	for l in bin:usr/bin sbin:usr/bin lib:usr/lib lib64:usr/lib lib32:usr/lib32 usr/sbin:bin usr/lib64:lib; do
		[ -e "${l%%:*}" ] || ln -s "${l#*:}" "${l%%:*}"
	done
	chmod 1777 tmp var/tmp
	# layerrun lays its own versions of these over the layer's for each session
	for f in passwd group hosts hostname resolv.conf machine-id; do
		[ -e "etc/$f" ] || : >"etc/$f"
	done
}

pkg_kernel_headers() {
	local ksrc=$TOP/build/linux-$KVER
	if [ ! -f "$ksrc/Makefile" ]; then
		mkdir -p "$TOP/build"
		curl -fsSL "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$KVER.tar.xz" | tar -xJ -C "$TOP/build"
	fi
	make -s -C "$ksrc" O="$WORK/kheaders" headers_install INSTALL_HDR_PATH="$ROOT/usr"
}

# ---- glibc ---------------------------------------------------------------------------

pkg_glibc() {
	unpack glibc
	mkdir build && cd build
	local cc=(gcc) libdir=/usr/lib host=x86_64-pc-linux-gnu
	if [ "$ARCH" = 32 ]; then
		cc=(gcc -m32 -march=i686) libdir=/usr/lib32 host=i686-pc-linux-gnu
	fi
	# glibc is built with the plain compiler: it is what the sysroot provides
	unset CC CXX CFLAGS CXXFLAGS LDFLAGS PKG_CONFIG_LIBDIR PKG_CONFIG_SYSROOT_DIR
	printf 'slibdir=%s\nrtlddir=%s\nsbindir=/usr/bin\nrootsbindir=/usr/bin\n' "$libdir" "$libdir" >configparms
	CC="${cc[*]}" CXX="g++ ${cc[*]:1}" ../configure --prefix=/usr --libdir="$libdir" --libexecdir="$libdir" \
		--host="$host" --build=x86_64-pc-linux-gnu --with-headers="$ROOT/usr/include" --enable-kernel=5.10 \
		--disable-werror --enable-stack-protector=strong --enable-bind-now --disable-nscd --disable-profile \
		CFLAGS="-O2 -pipe" >configure.log
	make -j"$JOBS" >make.log 2>&1 || { tail -40 make.log; exit 1; }
	make install DESTDIR="$ROOT" >install.log 2>&1 || { tail -40 install.log; exit 1; }
	if [ "$ARCH" = 32 ]; then
		# the 32-bit dynamic linker is looked for as /lib/ld-linux.so.2
		ln -sf ../lib32/ld-linux.so.2 "$ROOT/usr/lib/ld-linux.so.2"
	else
		# a UTF-8 locale for programs that ask for one, made by the new glibc's own localedef
		mkdir -p "$ROOT/usr/lib/locale"
		I18NPATH=$(pwd)/../localedata "$ROOT/usr/lib/ld-linux-x86-64.so.2" --library-path "$ROOT/usr/lib" "$ROOT/usr/bin/localedef" \
			--no-archive -i en_US -f UTF-8 "$ROOT/usr/lib/locale/en_US.UTF-8" >locale.log 2>&1 || cat locale.log
	fi
}

# ---- GCC's runtime libraries (libgcc_s, libstdc++), not the compiler ------------------

pkg_gcc_runtime() {
	unpack gcc
	mkdir build && cd build
	unset CC CXX CFLAGS CXXFLAGS LDFLAGS PKG_CONFIG_LIBDIR PKG_CONFIG_SYSROOT_DIR
	../configure --prefix=/usr --libdir=/usr/lib --enable-languages=c,c++ --disable-bootstrap --enable-multilib \
		--with-multilib-list=m32,m64 --with-build-sysroot="$ROOT" --disable-libsanitizer --disable-libgomp \
		--disable-libquadmath --disable-libssp --disable-libvtv --disable-libitm --disable-libatomic --disable-nls \
		--enable-shared --disable-static --with-system-zlib >configure.log
	make -j"$JOBS" >make.log 2>&1 || { tail -40 make.log; exit 1; }
	# only the shared runtime goes into the layer
	local stage=$WORK/gcc-stage
	rm -rf "$stage"
	make install-target-libgcc install-target-libstdc++-v3 DESTDIR="$stage" >install.log 2>&1
	for d in lib lib32 lib64; do
		[ -d "$stage/usr/$d" ] || continue
		local to=/usr/lib
		[ "$d" = lib32 ] && to=/usr/lib32
		cp -a "$stage/usr/$d"/libgcc_s.so* "$stage/usr/$d"/libstdc++.so.6* "$ROOT$to/" 2>/dev/null || true
	done
	rm -f "$ROOT"/usr/lib*/libstdc++.so.6*-gdb.py
}

# ---- The package list ------------------------------------------------------------------

ORDER=(
	layout kernel_headers glibc glibc:32 gcc_runtime
)

source "$HERE/packages.sh"

if [ $# -gt 0 ]; then
	for p in "$@"; do build "$p"; done
else
	for p in "${ORDER[@]}"; do build "$p"; done
	say "layer ready in $ROOT ($(du -sh "$ROOT" | cut -f1))"
fi
