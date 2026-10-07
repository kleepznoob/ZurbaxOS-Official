#!/bin/bash
# Builds the Steam layer: the standard Linux pieces Steam and Proton need,
# compiled from their official upstream sources (no distribution packages).
#
#   layer/build.sh [PACKAGE...]    all packages, or just these
#   layer/build.sh --fetch         only download the sources
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
# glibc is compiled with the build machine's compiler; everything after it
# with the layer's own GCC (pkg_toolchain), which builds against the layer
# (its system root) and never sees the build machine's headers or libraries.
# Packages are configured as cross builds, even 64-bit ones: their test
# programs would need the layer's glibc to run.

TC=$L/toolchain NATIVE=$L/native
BUILD=x86_64-pc-linux-gnu
export PATH="$NATIVE/bin:$L/venv/bin:$PATH"

use64() {
	ARCH=64 LIBDIR=/usr/lib HOST=x86_64-unknown-linux-gnu M=
	flags "-O2 -pipe -fno-plt"
}

use32() {
	ARCH=32 LIBDIR=/usr/lib32 HOST=i686-pc-linux-gnu M=-m32
	# any 64-bit processor runs the layer, so 32-bit code can count on SSE2
	flags "-O2 -pipe -march=x86-64 -mtune=generic -mfpmath=sse -mstackrealign"
}

flags() {
	OPT=$1
	export CC="$TC/bin/gcc${M:+ $M}" CXX="$TC/bin/g++${M:+ $M}"
	export CFLAGS="$OPT" CXXFLAGS="$OPT" LDFLAGS="-Wl,-O1"
	export PKG_CONFIG_LIBDIR=$ROOT$LIBDIR/pkgconfig:$ROOT/usr/share/pkgconfig PKG_CONFIG_SYSROOT_DIR=$ROOT
	unset PKG_CONFIG_PATH
	# 32-bit packages are installed here first; only their libraries go into the layer
	DEST=$ROOT
	if [ "$ARCH" = 32 ]; then
		DEST=$WORK/stage32
		rm -rf "$DEST"
	fi
}

# Moves a 32-bit package's libraries from its staging folder into the layer.
# Its programs, data and headers are the 64-bit build's (headers are only
# added where the 64-bit build has none).
settle() {
	[ "$ARCH" = 32 ] || return 0
	if [ -d "$DEST$LIBDIR" ]; then cp -a "$DEST$LIBDIR/." "$ROOT$LIBDIR/"; fi
	if [ -d "$DEST/usr/include" ]; then cp -rn "$DEST/usr/include/." "$ROOT/usr/include/"; fi
	# Vulkan drivers' manifests are named for their width
	if [ -d "$DEST/usr/share/vulkan" ]; then
		mkdir -p "$ROOT/usr/share/vulkan"
		cp -a "$DEST/usr/share/vulkan/." "$ROOT/usr/share/vulkan/"
	fi
	rm -rf "$DEST"
}

# The machine files meson and cmake need for these cross builds.
machine_files() {
	local cpu=x86_64 family=x86_64 m='' args linkargs
	if [ "$ARCH" = 32 ]; then cpu=i686 family=x86 m=", '-m32'"; fi
	args=$(printf "'%s', " $OPT) linkargs=$(printf "'%s', " $LDFLAGS)
	cat >"$WORK/cross$ARCH.ini" <<-EOF
		[binaries]
		c = ['$TC/bin/gcc'$m]
		cpp = ['$TC/bin/g++'$m]
		ar = 'ar'
		nm = 'nm'
		strip = 'strip'
		pkg-config = 'pkg-config'
		cmake = 'cmake'
		[properties]
		sys_root = '$ROOT'
		pkg_config_libdir = '$ROOT$LIBDIR/pkgconfig:$ROOT/usr/share/pkgconfig'
		needs_exe_wrapper = true
		[built-in options]
		c_args = [${args%, }]
		cpp_args = [${args%, }]
		c_link_args = [${linkargs%, }]
		cpp_link_args = [${linkargs%, }]
		[host_machine]
		system = 'linux'
		cpu_family = '$family'
		cpu = '$cpu'
		endian = 'little'
	EOF
	# tools that run during the build come from the build machine (and $NATIVE)
	cat >"$WORK/native.ini" <<-EOF
		[binaries]
		c = 'gcc'
		cpp = 'g++'
		pkg-config = 'pkg-config'
		llvm-config = 'llvm-config-18'
		[built-in options]
		pkg_config_path = ['$NATIVE/lib/pkgconfig', '$NATIVE/share/pkgconfig', '$NATIVE/lib/x86_64-linux-gnu/pkgconfig']
	EOF
	cat >"$WORK/toolchain$ARCH.cmake" <<-EOF
		set(CMAKE_SYSTEM_NAME Linux)
		set(CMAKE_SYSTEM_PROCESSOR $cpu)
		set(CMAKE_SYSROOT $ROOT)
		set(CMAKE_C_COMPILER $TC/bin/gcc)
		set(CMAKE_CXX_COMPILER $TC/bin/g++)
		set(CMAKE_C_FLAGS_INIT "$M $OPT")
		set(CMAKE_CXX_FLAGS_INIT "$M $OPT")
		set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
		set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
		set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
		set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
	EOF
}

# configure && make && make install for autotools packages, either width.
autobuild() {
	./configure --build=$BUILD --host=$HOST --prefix=/usr --libdir=$LIBDIR --sysconfdir=/etc --localstatedir=/var \
		--disable-static "$@" >configure.log 2>&1 || { tail -30 config.log; exit 1; }
	make -j"$JOBS" >make.log 2>&1 || { grep -B5 -A5 -m5 'error' make.log | head -60; exit 1; }
	make DESTDIR="$DEST" install >install.log 2>&1 || { tail -30 install.log; exit 1; }
	finish
}

# meson setup && ninja && install, either width; options after the defaults.
mesonbuild() {
	machine_files
	env -u CC -u CXX -u CFLAGS -u CXXFLAGS -u LDFLAGS -u PKG_CONFIG_LIBDIR -u PKG_CONFIG_SYSROOT_DIR \
		meson setup _build --cross-file "$WORK/cross$ARCH.ini" --native-file "$WORK/native.ini" \
		--prefix=/usr --libdir="${LIBDIR#/usr/}" --sysconfdir=/etc --localstatedir=/var --buildtype=release \
		-Ddefault_library=shared -Db_ndebug=true --wrap-mode=nodownload "$@" >meson.log 2>&1 || { tail -40 meson.log; exit 1; }
	ninja -C _build -j"$JOBS" >ninja.log 2>&1 || { grep -A12 -m3 FAILED ninja.log | head -60; exit 1; }
	DESTDIR="$DEST" ninja -C _build install >install.log 2>&1 || { tail -30 install.log; exit 1; }
	finish
}

cmakebuild() {
	machine_files
	env -u CC -u CXX -u CFLAGS -u CXXFLAGS -u LDFLAGS cmake -S . -B _build -G Ninja \
		-DCMAKE_TOOLCHAIN_FILE="$WORK/toolchain$ARCH.cmake" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
		-DCMAKE_INSTALL_LIBDIR="${LIBDIR#/usr/}" -DCMAKE_INSTALL_SYSCONFDIR=/etc "$@" >cmake.log 2>&1 || { tail -40 cmake.log; exit 1; }
	ninja -C _build -j"$JOBS" >ninja.log 2>&1 || { grep -A12 -m3 FAILED ninja.log | head -60; exit 1; }
	DESTDIR="$DEST" ninja -C _build install >install.log 2>&1 || { tail -30 install.log; exit 1; }
	finish
}

# After an install: libtool archives point at build paths and nothing needs
# them; 32-bit libraries move into the layer.
finish() {
	find "$DEST$LIBDIR" -maxdepth 1 -name '*.la' -delete 2>/dev/null || true
	settle
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
	# the sources and build folders aren't needed any more
	find "${WORK:?}" -mindepth 1 -maxdepth 1 -exec rm -rf {} +
}

# Python with the modules Mesa's build needs, and a recent meson.
pkg_venv() {
	python3 -m venv "$L/venv"
	"$L/venv/bin/pip" install -q meson==1.9.1 mako pyyaml packaging
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
	# where the dynamic linker (and the linker, while building) finds 32-bit libraries
	printf '/usr/lib32\n' >etc/ld.so.conf
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
	# glibc is built with the build machine's compiler: it is what the layer's own compiler needs first
	unset CC CXX CFLAGS CXXFLAGS LDFLAGS PKG_CONFIG_LIBDIR PKG_CONFIG_SYSROOT_DIR
	printf 'slibdir=%s\nrtlddir=%s\nsbindir=/usr/bin\nrootsbindir=/usr/bin\n' "$libdir" "$libdir" >configparms
	CC="${cc[*]}" CXX="g++ ${cc[*]:1}" ../configure --prefix=/usr --libdir="$libdir" --libexecdir="$libdir" \
		--host="$host" --build=x86_64-pc-linux-gnu --with-headers="$ROOT/usr/include" --enable-kernel=5.10 \
		--disable-werror --enable-stack-protector=strong --enable-bind-now --disable-nscd --disable-profile --without-selinux \
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

# ---- GCC: the compiler everything else is built with, and its runtime -----------------
# A compiler of its own (in build/layer/toolchain, not the layer) whose system
# root is the layer: 64- and 32-bit (multilib), C and C++. Its shared runtime
# libraries (libgcc_s, libstdc++, libatomic) also go into the layer.

pkg_toolchain() {
	unpack gcc
	unset CC CXX CFLAGS CXXFLAGS LDFLAGS PKG_CONFIG_LIBDIR PKG_CONFIG_SYSROOT_DIR
	mkdir build && cd build
	../configure --prefix="$TC" --with-sysroot="$ROOT" --with-build-sysroot="$ROOT" \
		--with-native-system-header-dir=/usr/include --enable-languages=c,c++ --disable-bootstrap \
		--enable-multilib --with-multilib-list=m32,m64 --enable-shared --enable-threads=posix \
		--enable-__cxa_atexit --enable-default-pie --enable-default-ssp --enable-linker-build-id \
		--with-system-zlib --disable-nls --disable-libsanitizer --disable-libgomp --disable-libquadmath \
		--disable-libssp --disable-libvtv --disable-libitm --disable-werror >configure.log 2>&1 || { tail -30 config.log; exit 1; }
	make -j"$JOBS" >make.log 2>&1 || { grep -B5 -A10 -m3 'Error\|error:' make.log | head -80; exit 1; }
	rm -rf "$TC"
	make install-strip >install.log 2>&1 || { tail -30 install.log; exit 1; }
	# its runtime in the layer: lib64 holds the 64-bit libraries, lib32 the 32-bit ones
	local pair from to
	for pair in lib64:/usr/lib lib32:/usr/lib32; do
		from=$TC/${pair%%:*} to=$ROOT${pair#*:}
		cp -a "$from"/libgcc_s.so.1 "$from"/libstdc++.so.6* "$from"/libatomic.so.1* "$to/"
	done
	rm -f "$ROOT"/usr/lib*/libstdc++.so.6*-gdb.py
}

# ---- The package list ------------------------------------------------------------------

ORDER=(
	venv layout kernel_headers glibc glibc:32 toolchain
)

source "$HERE/packages.sh"

if [ "${1:-}" = --fetch ]; then
	# downloads (and checks) every source, for building offline later
	while read -r name _; do fetch "$name" >/dev/null; done <"$HERE/sources.txt"
elif [ $# -gt 0 ]; then
	for p in "$@"; do build "$p"; done
else
	for p in "${ORDER[@]}"; do build "$p"; done
	say "layer ready in $ROOT ($(du -sh "$ROOT" | cut -f1))"
fi
