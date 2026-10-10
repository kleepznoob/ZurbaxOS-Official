# The layer's packages after glibc and the compiler; sourced by build.sh.
# "name" builds the 64-bit package, "name:32" the 32-bit one (only its
# libraries go into the layer). Steam's client is 32-bit and Proton runs
# 32-bit Windows games too, so everything a game or the client loads comes
# in both widths; programs and data only in 64-bit.

# ---- Tools the build runs (for the build machine, in build/layer/native) ----------------

# for the build machine: no layer, no cross settings
native_env() {
	unset CC CXX CFLAGS CXXFLAGS LDFLAGS PKG_CONFIG_LIBDIR PKG_CONFIG_SYSROOT_DIR
	mkdir -p "$NATIVE/bin"
}

# wayland-scanner, which turns protocol descriptions into C, in the version
# the layer's Wayland libraries are
pkg_native_wayland() {
	unpack wayland
	native_env
	meson setup _build --prefix="$NATIVE" --libdir=lib -Dlibraries=false -Ddocumentation=false -Dtests=false \
		-Ddtd_validation=false >meson.log 2>&1 || { tail -30 meson.log; exit 1; }
	ninja -C _build install >ninja.log 2>&1 || { tail -30 ninja.log; exit 1; }
}

# Mesa's OpenCL C compiler (mesa_clc) and vtn_bindgen2: Intel's drivers
# compile their internal GPU programs with them while building. They need
# LLVM and Clang, which only the build machine has.
pkg_native_mesa_clc() {
	unpack mesa
	native_env
	cat >native-llvm.ini <<-EOF
		[binaries]
		llvm-config = 'llvm-config-18'
	EOF
	meson setup _build --native-file native-llvm.ini --buildtype=release -Dplatforms= -Dgallium-drivers= -Dvulkan-drivers= \
		-Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled \
		-Dllvm=enabled -Dshared-llvm=enabled -Dmesa-clc=enabled -Dinstall-mesa-clc=true -Dbuild-tests=false \
		-Dzstd=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled -Dvideo-codecs= \
		-Dxmlconfig=disabled -Dexpat=disabled >meson.log 2>&1 || { tail -40 meson.log; exit 1; }
	ninja -C _build src/compiler/clc/mesa_clc src/compiler/spirv/vtn_bindgen2 >ninja.log 2>&1 ||
		{ grep -A12 -m3 FAILED ninja.log | head -60; exit 1; }
	cp _build/src/compiler/clc/mesa_clc _build/src/compiler/spirv/vtn_bindgen2 "$NATIVE/bin/"
}

# file's magic database is compiled by the same version of file
pkg_native_file() {
	unpack file
	native_env
	./configure --prefix="$NATIVE" --disable-shared --disable-libseccomp --disable-bzlib --disable-xzlib \
		--disable-zstdlib --disable-lzlib >configure.log 2>&1 || { tail -30 config.log; exit 1; }
	make -j"$JOBS" >make.log 2>&1 && make install >install.log 2>&1
}

# ---- Base libraries ----------------------------------------------------------------------------

pkg_zlib() {
	unpack zlib
	AR=ar ./configure --prefix=/usr --libdir=$LIBDIR --shared >configure.log 2>&1
	make -j"$JOBS" >make.log 2>&1
	make DESTDIR="$DEST" install >install.log 2>&1
	rm -f "$DEST$LIBDIR/libz.a"
	finish
}

pkg_expat() {
	unpack expat
	cd expat
	cmakebuild -DEXPAT_BUILD_TOOLS=OFF -DEXPAT_BUILD_EXAMPLES=OFF -DEXPAT_BUILD_TESTS=OFF -DEXPAT_BUILD_DOCS=OFF \
		-DEXPAT_SHARED_LIBS=ON
}

pkg_libffi() {
	unpack libffi
	autoreconf -fi >autoreconf.log 2>&1
	local inc=()
	# ffi.h differs between the widths: the 32-bit one stays with its library
	[ "$ARCH" = 32 ] && inc=(--includedir=$LIBDIR/libffi/include)
	autobuild --disable-docs --disable-multi-os-directory "${inc[@]}"
}

# libcrypt: glibc no longer has it, and Steam's client still links to it
pkg_libxcrypt() {
	unpack libxcrypt
	autoreconf -fi >autoreconf.log 2>&1
	autobuild --disable-werror --enable-hashes=strong,glibc --enable-obsolete-api=glibc --disable-failure-tokens
}

# libudev for programs that look for devices with it (Steam and SDL find game
# controllers this way); Zurbax has no udev, and this one reads /sys directly.
# zinit passes on device events in the form it listens for.
pkg_libudev_zero() {
	unpack libudev-zero
	make -j"$JOBS" CC="$CC" CFLAGS="$CFLAGS" LDFLAGS="$LDFLAGS" libudev.so.1 >make.log 2>&1
	make PREFIX=/usr LIBDIR=$LIBDIR DESTDIR="$DEST" install-shared >install.log 2>&1
	finish
}

# libelf (from elfutils), which AMD's OpenGL driver reads its GPU programs with
pkg_elfutils() {
	unpack elfutils
	autobuild_configure --disable-debuginfod --disable-libdebuginfod --without-bzlib --without-lzma --without-zstd \
		--disable-nls --disable-demangler
	make -j"$JOBS" -C lib >make.log 2>&1 && make -j"$JOBS" -C libelf >>make.log 2>&1 || { tail -30 make.log; exit 1; }
	make -C libelf DESTDIR="$DEST" install >install.log 2>&1
	install -Dm644 config/libelf.pc "$DEST$LIBDIR/pkgconfig/libelf.pc"
	rm -f "$DEST$LIBDIR/libelf.a"
	finish
}

# ./configure alone, with the same settings as autobuild
autobuild_configure() {
	./configure --build=$BUILD --host=$HOST --prefix=/usr --libdir=$LIBDIR --sysconfdir=/etc --localstatedir=/var \
		--disable-static "$@" >configure.log 2>&1 || { tail -30 config.log; exit 1; }
}

ORDER+=(
	native_wayland native_mesa_clc native_file
	zlib zlib:32 expat expat:32 libffi libffi:32 libxcrypt libxcrypt:32
	libudev_zero libudev_zero:32 elfutils elfutils:32
)

# ---- Wayland and the kernel's graphics interface --------------------------------------------

pkg_wayland() {
	unpack wayland
	mesonbuild -Dscanner=false -Ddocumentation=false -Dtests=false -Ddtd_validation=false
}

pkg_wayland_protocols() {
	unpack wayland-protocols
	mesonbuild -Dtests=false
}

pkg_libdrm() {
	unpack libdrm
	mesonbuild -Damdgpu=enabled -Dradeon=enabled -Dintel=disabled -Dnouveau=disabled -Dvmwgfx=disabled \
		-Domap=disabled -Dexynos=disabled -Dfreedreno=disabled -Dtegra=disabled -Dvc4=disabled -Detnaviv=disabled \
		-Dcairo-tests=disabled -Dman-pages=disabled -Dvalgrind=disabled -Dtests=false -Dinstall-test-programs=false
}

ORDER+=(wayland wayland:32 wayland_protocols libdrm libdrm:32)

# ---- X11's libraries ----------------------------------------------------------------------------

# options X.Org's packages share: no documentation
XDOCS=(--disable-specs --disable-devel-docs --without-xmlto --without-fop --without-xsltproc --without-doxygen)

xorg() {
	unpack "$1"
	shift
	autobuild "${XDOCS[@]}" "$@"
}

pkg_xorgproto() { unpack xorgproto; mesonbuild -Dlegacy=false; }
pkg_xcb_proto() { xorg xcb-proto; }
pkg_xtrans() { xorg xtrans; }
pkg_libXau() { xorg libXau; }

pkg_libxcb() {
	# the protocol descriptions and their Python generator, from xcb-proto in the layer
	export XCBPROTO_XCBINCLUDEDIR=$ROOT/usr/share/xcb
	XCBPROTO_XCBPYTHONDIR=$(echo "$ROOT"/usr/lib/python3*/site-packages)
	export XCBPROTO_XCBPYTHONDIR
	xorg libxcb
}

pkg_libX11() { xorg libX11 --disable-xf86bigfont; }
pkg_libXext() { xorg libXext; }
pkg_libXfixes() { xorg libXfixes; }
pkg_libXrender() { xorg libXrender; }
pkg_libXrandr() { xorg libXrandr; }
pkg_libXi() { xorg libXi; }
pkg_libXcursor() { xorg libXcursor; }
pkg_libXinerama() { xorg libXinerama; }
pkg_libXxf86vm() { xorg libXxf86vm; }
pkg_libXdamage() { xorg libXdamage; }
pkg_libXcomposite() { xorg libXcomposite; }
pkg_libXtst() { xorg libXtst; }
pkg_libxshmfence() { xorg libxshmfence; }
pkg_libxkbfile() { unpack libxkbfile; mesonbuild; }
pkg_libxcvt() { unpack libxcvt; mesonbuild; }
pkg_libfontenc() { xorg libfontenc --with-fontrootdir=/usr/share/fonts/X11; }
pkg_libXfont2() { xorg libXfont2 --disable-selective-werror; }

ORDER+=(xorgproto xcb_proto xtrans)
for p in libXau libxcb libX11 libXext libXfixes libXrender libXrandr libXi libXcursor libXinerama libXxf86vm \
	libXdamage libXcomposite libXtst libxshmfence; do
	ORDER+=("$p" "$p:32")
done

# ---- Fonts ---------------------------------------------------------------------------------------

pkg_freetype() {
	unpack freetype
	mesonbuild -Dharfbuzz=disabled -Dpng=disabled -Dbrotli=disabled -Dbzip2=disabled -Dzlib=system -Dtests=disabled
}

pkg_fontconfig() {
	unpack fontconfig
	local tools=enabled
	[ "$ARCH" = 32 ] && tools=disabled
	mesonbuild -Ddoc=disabled -Dtests=disabled -Dnls=disabled -Dcache-build=disabled -Dtools=$tools
}

pkg_dejavu() {
	unpack dejavu
	install -d "$ROOT/usr/share/fonts/dejavu" "$ROOT/etc/fonts/conf.d"
	install -m644 ttf/*.ttf "$ROOT/usr/share/fonts/dejavu/"
	install -m644 fontconfig/*.conf "$ROOT/usr/share/fontconfig/conf.avail/"
	for c in fontconfig/*.conf; do ln -sf "/usr/share/fontconfig/conf.avail/${c##*/}" "$ROOT/etc/fonts/conf.d/${c##*/}"; done
}

ORDER+=(freetype freetype:32 fontconfig fontconfig:32 dejavu libxkbfile libxcvt libfontenc libXfont2)

# ---- Mesa: OpenGL and Vulkan drivers for AMD and Intel graphics ------------------------------

# libglvnd sends OpenGL calls to the right driver; Steam's runtime expects it
pkg_libglvnd() {
	unpack libglvnd
	mesonbuild -Dx11=enabled -Degl=true -Dglx=enabled -Dgles1=false -Dgles2=true -Dheaders=true
}

# AMD: radeonsi (OpenGL) and RADV (Vulkan), with Mesa's own compiler (ACO);
# Intel: iris and crocus (OpenGL), ANV and HASVK (Vulkan); zink is OpenGL
# on any Vulkan driver, virgl and softpipe for virtual machines and as the
# last resort.
pkg_mesa() {
	unpack mesa
	mesonbuild -Dplatforms=x11,wayland -Dgallium-drivers=radeonsi,iris,crocus,zink,virgl,softpipe \
		-Dvulkan-drivers=amd,intel,intel_hasvk -Dvulkan-layers=device-select \
		-Dllvm=disabled -Damd-use-llvm=false -Dmesa-clc=system \
		-Dglx=dri -Degl=enabled -Dgbm=enabled -Dglvnd=enabled -Dopengl=true -Dgles1=disabled -Dgles2=enabled \
		-Dvideo-codecs= -Dgallium-va=disabled -Dgallium-rusticl=false -Dteflon=false \
		-Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled -Dzstd=disabled -Dbuild-tests=false \
		-Dexpat=enabled -Dxmlconfig=enabled
}

pkg_vulkan_loader() {
	unpack vulkan-loader
	# Debian's copy of the source leaves out the generated code; the loader's
	# own generator remakes it from the Vulkan registry that comes with it
	local version=${PWD##*-}
	mkdir -p loader/generated tests/framework/generated tests/framework/layer/generated
	python3 scripts/generate_source.py vulkan-headers/registry --generated-version "$version" --incremental >codegen.log 2>&1 ||
		{ tail -20 codegen.log; exit 1; }
	# Vulkan's headers come with the loader's source
	(cd vulkan-headers && cmake -S . -B _build -DCMAKE_INSTALL_PREFIX="$WORK/vkheaders" >cmake.log && cmake --install _build >>cmake.log)
	cmakebuild -DVulkanHeaders_DIR="$WORK/vkheaders/share/cmake/VulkanHeaders" -DVULKAN_HEADERS_INSTALL_DIR="$WORK/vkheaders" \
		-DBUILD_TESTS=OFF -DBUILD_WSI_XCB_SUPPORT=ON -DBUILD_WSI_XLIB_SUPPORT=ON -DBUILD_WSI_WAYLAND_SUPPORT=ON \
		-DUPDATE_DEPS=OFF -DCMAKE_FIND_ROOT_PATH="$WORK/vkheaders"
	[ "$ARCH" = 64 ] && cp -a "$WORK/vkheaders/include/." "$ROOT/usr/include/"
	return 0
}

# gpucheck/zos-gpucheck.c: lists the Vulkan devices and the OpenGL renderer
pkg_gpucheck() {
	$CC $CFLAGS -o "$ROOT/usr/bin/zos-gpucheck" "$HERE/gpucheck/zos-gpucheck.c" $LDFLAGS -lvulkan -lEGL
}

ORDER+=(libglvnd libglvnd:32 mesa mesa:32 vulkan_loader vulkan_loader:32 gpucheck)

# ---- Xwayland: X11 programs (Steam) on Zurbax's display -------------------------------------------

pkg_pixman() {
	unpack pixman
	mesonbuild -Dtests=disabled -Ddemos=disabled -Dgtk=disabled -Dlibpng=disabled -Dopenmp=disabled
}

pkg_nettle() {
	unpack nettle
	autobuild --disable-documentation --disable-public-key --disable-openssl
}

pkg_libepoxy() {
	unpack libepoxy
	mesonbuild -Dtests=false -Ddocs=false -Degl=yes -Dglx=yes -Dx11=true
}

pkg_xwayland() {
	unpack xwayland
	# GLX's software renderer is loaded from Mesa's driver folder, which the
	# build reads from dri.pc, where pkg-config has put the build's system
	# root in front of it; on Zurbax it is just /usr/lib/dri
	sed -i "s|dri_dep.get_variable(pkgconfig : 'dridriverdir')|'/usr/lib/dri'|" include/meson.build
	mesonbuild -Dglamor=true -Ddri3=true -Dxvfb=false -Dsecure-rpc=false -Dxdmcp=false -Dxdm-auth-1=false \
		-Dsha1=libnettle -Dlibdecor=false -Dxwayland_ei=false -Ddocs=false -Ddevel-docs=false -Dxcsecurity=true \
		-Dxkb_output_dir=/tmp -Dxkb_dir=/usr/share/X11/xkb -Dxkb_bin_dir=/usr/bin
}

pkg_xkbcomp() { xorg xkbcomp; }

# xwm/zos-xwm.c: the window manager in Xwayland's screen (full screen for
# games and Big Picture, maximized and centred windows, focus)
pkg_xwm() {
	$CC $CFLAGS -Wall -o "$ROOT/usr/bin/zos-xwm" "$HERE/xwm/zos-xwm.c" $LDFLAGS -lX11
}

pkg_xkeyboard_config() {
	unpack xkeyboard-config
	mesonbuild -Dxorg-rules-symlinks=true -Dnls=false
}

ORDER+=(pixman nettle libepoxy xwayland xkbcomp xkeyboard_config xwm)

# ---- Sound: ALSA's library, playing through Zurbax's sound service --------------------------

pkg_alsa_lib() {
	unpack alsa-lib
	autobuild --disable-python --disable-topology --without-debug
}

# zaudio-alsa/pcm_zaudio.c as an alsa-lib plugin, and the configuration that
# makes it every program's default output (alsa-lib's plug layer converts
# formats and rates for it).
pkg_zaudio_alsa() {
	mkdir -p "$ROOT$LIBDIR/alsa-lib"
	$CC $CFLAGS -shared -fPIC -o "$ROOT$LIBDIR/alsa-lib/libasound_module_pcm_zaudio.so" \
		"$HERE/zaudio-alsa/pcm_zaudio.c" $LDFLAGS -lasound
	if [ "$ARCH" = 64 ]; then
		cat >"$ROOT/etc/asound.conf" <<-'CONF'
			# Zurbax: sound goes to zaudio, Zurbax's sound service, which owns the sound card
			pcm.zaudio { type zaudio }
			pcm.!default { type plug slave.pcm "zaudio" }
		CONF
	fi
}

# PulseAudio: Proton's Wine, SDL and Steam's runtime play sound through it;
# Steam passes its socket into the containers games run in. Here it has one
# output, ALSA's zaudio plugin, so everything reaches Zurbax's sound service.
# layerrun starts it with a Steam session.
# libltdl, libtool's module loader: PulseAudio loads its modules with it
pkg_libltdl() {
	unpack libtool
	autobuild --enable-ltdl-install
}

pkg_speexdsp() {
	unpack speexdsp
	autobuild --disable-examples
}

pkg_libsndfile() {
	unpack libsndfile
	cmakebuild -DBUILD_SHARED_LIBS=ON -DBUILD_PROGRAMS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF \
		-DENABLE_EXTERNAL_LIBS=OFF -DENABLE_MPEG=OFF -DENABLE_CPACK=OFF -DINSTALL_MANPAGES=OFF
}

# The server's start-up script. Without Zurbax's sound service (it restarts, or
# a machine has no sound card) the server still runs, with a silent output.
zos_pa() {
	cat <<-'EOF'
		# Zurbax: one output, ALSA's zaudio plugin (Zurbax's sound service); see layer/packages.sh
		.nofail
		load-module module-alsa-sink device=zaudio sink_name=zos sink_properties=device.description=Zurbax tsched=0
		.fail
		load-module module-null-source source_name=silence
		load-module module-native-protocol-unix auth-anonymous=1
		load-module module-always-sink
		.nofail
		set-default-sink zos
		set-default-source silence
	EOF
}

pkg_pulseaudio() {
	unpack pulseaudio
	local off=(asyncns avahi bluez5 dbus elogind fftw glib gsettings gstreamer gtk jack lirc openssl orc samplerate
		soxr systemd tcpwrap udev valgrind x11 consolekit)
	local opts=() o
	for o in "${off[@]}"; do opts+=("-D$o=disabled"); done
	mesonbuild -Ddaemon=true -Dclient=true -Ddoxygen=false -Dman=false -Dtests=false -Ddatabase=simple \
		-Dalsa=enabled -Dspeex=enabled -Dwebrtc-aec=disabled -Dadrian-aec=false -Dhal-compat=false \
		-Dbluez5-gstreamer=disabled -Dstream-restore-clear-old-devices=true "${opts[@]}"
	zos_pa >"$ROOT/etc/pulse/zos.pa"
	cat >>"$ROOT/etc/pulse/daemon.conf" <<-'EOF'

		; Zurbax: started by layerrun for the session, no real-time priority to ask for
		exit-idle-time = -1
		realtime-scheduling = no
		flat-volumes = no
		default-sample-rate = 48000
		resample-method = speex-float-1
	EOF
	printf '
; Zurbax: layerrun starts the server
autospawn = no
' >>"$ROOT/etc/pulse/client.conf"
}

ORDER+=(alsa_lib alsa_lib:32 zaudio_alsa zaudio_alsa:32 libltdl speexdsp libsndfile pulseaudio)

# ---- The commands Steam's scripts run ----------------------------------------------------

# GNU packages refuse to configure as root unless told it's on purpose
gnu() {
	unpack "$1"
	shift
	FORCE_UNSAFE_CONFIGURE=1 autobuild --disable-nls "$@"
}

pkg_bash() {
	# what configure would find out by running programs: glibc's getcwd works
	# (bash's own stand-in loses its way across bind mounts), and the rest
	gnu bash --without-bash-malloc bash_cv_getcwd_malloc=yes bash_cv_job_control_missing=present \
		bash_cv_sys_named_pipes=present bash_cv_func_sigsetjmp=present bash_cv_printf_a_format=yes \
		bash_cv_unusable_rtsigs=no bash_cv_func_ctype_nonascii=yes bash_cv_wcwidth_broken=no
	ln -sf bash "$ROOT/usr/bin/sh"
}

pkg_coreutils() { gnu coreutils --without-openssl --without-selinux --without-libgmp --enable-no-install-program=kill,uptime; }
pkg_sed() { gnu sed; }
pkg_grep() { gnu grep --disable-perl-regexp; }
pkg_gawk() { gnu gawk --without-readline --without-mpfr; }
pkg_findutils() { gnu findutils; }
pkg_diffutils() { gnu diffutils gl_cv_func_strcasecmp_works=yes; } # its check runs a program
pkg_tar() { gnu tar; }
pkg_gzip() { gnu gzip; }
pkg_which() { gnu which; }
pkg_xz() { gnu xz --disable-doc --disable-scripts; }
pkg_file() { gnu file --disable-libseccomp --disable-bzlib --disable-zstdlib --disable-lzlib; }

# getopt (from util-linux), which the Steam runtime's setup script uses
pkg_getopt() {
	unpack util-linux
	autobuild_configure --disable-nls --without-python --without-systemd --without-udev --without-ncurses \
		--without-tinfo --without-readline --without-btrfs --without-cryptsetup --disable-liblastlog2 \
		--disable-pylibmount --disable-bash-completion --disable-makeinstall-chown --disable-makeinstall-setuid
	make -j"$JOBS" getopt >make.log 2>&1 || { tail -30 make.log; exit 1; }
	install -m755 getopt "$ROOT/usr/bin/getopt"
}

# libcap: the Steam runtime's own bwrap (its container tool) links to it
pkg_libcap() {
	unpack libcap
	local mk=(prefix=/usr lib="${LIBDIR#/usr/}" CC="$CC" BUILD_CC=gcc AR=ar RANLIB=ranlib GOLANG=no PAM_CAP=no
		USE_GPERF=no SHARED=yes DYNAMIC=yes "COPTS=$CFLAGS" "LDFLAGS=$LDFLAGS")
	make -C libcap -j"$JOBS" "${mk[@]}" >make.log 2>&1 || { tail -30 make.log; exit 1; }
	make -C libcap "${mk[@]}" DESTDIR="$DEST" RAISE_SETFCAP=no install >install.log 2>&1 || { tail -30 install.log; exit 1; }
	rm -f "$DEST$LIBDIR"/libcap.a "$DEST$LIBDIR"/libpsx.a
	finish
}

# D-Bus: layerrun starts a session bus for Steam's sessions (Steam's runtime
# launcher service and many games talk on it)
pkg_dbus() {
	unpack dbus
	mesonbuild -Dapparmor=disabled -Dselinux=disabled -Dlibaudit=disabled -Dsystemd=disabled \
		-Dx11_autolaunch=disabled -Dkqueue=disabled -Dlaunchd=disabled -Ddoxygen_docs=disabled -Dxml_docs=disabled \
		-Dducktype_docs=disabled -Dqt_help=disabled -Dmodular_tests=disabled -Dinstalled_tests=false \
		-Dmessage_bus=true -Dtools=true -Duser_session=false -Dvalgrind=disabled -Dsession_socket_dir=/tmp
}

# lspci, which Steam asks about the graphics card
pkg_pciutils() {
	unpack pciutils
	local mk=(PREFIX=/usr SBINDIR=/usr/bin SHAREDIR=/usr/share/hwdata CC="$CC" AR=ar RANLIB=ranlib HOST=x86_64-linux
		ZLIB=no DNS=no SHARED=no HWDB=no LIBKMOD=no "OPT=$CFLAGS" "LDFLAGS=$LDFLAGS")
	make -j"$JOBS" "${mk[@]}" lspci >make.log 2>&1 || { tail -30 make.log; exit 1; }
	install -Dm755 lspci "$ROOT/usr/bin/lspci"
	install -Dm644 pci.ids "$ROOT/usr/share/hwdata/pci.ids"
}

ORDER+=(bash coreutils sed grep gawk findutils diffutils tar gzip which xz file getopt libcap libcap:32 dbus dbus:32
	pciutils)

# ---- Flatpak: apps from Flathub ------------------------------------------------------------
# Flatpak runs apps in sandboxes of their own (bubblewrap), each with the
# runtime it was built for, so one app works the same on every distribution.
# Its downloads go through OSTree, whose repositories are signed with GnuPG;
# AppStream reads the catalogue of apps. Zurbax installs apps per user, in the
# home folder (the layer itself is read-only). Zurbax's own portal service
# (system/zportal) gives the apps file choosers, settings and notifications.
# All of this is 64-bit only.

# GLib's checks that would run a program, answered for glibc on x86-64
glib_props() {
	cat >"$WORK/glib.ini" <<-'EOF'
		[properties]
		have_c99_vsnprintf = true
		have_c99_snprintf = true
		have_unix98_printf = true
		va_val_copy = true
		growing_stack = false
		have_strlcpy = true
		have_proc_self_cmdline = true
	EOF
}

pkg_pcre2() {
	unpack pcre2
	autobuild --enable-pcre2-8 --disable-pcre2-16 --disable-pcre2-32 --enable-jit --disable-pcre2grep-libz \
		--disable-pcre2grep-libbz2 --disable-pcre2test-libreadline
}

pkg_glib() {
	unpack glib
	glib_props
	mesonbuild --cross-file "$WORK/glib.ini" -Dselinux=disabled -Dlibmount=disabled -Dman-pages=disabled \
		-Ddocumentation=false -Dtests=false -Dinstalled_tests=false -Dnls=disabled -Dintrospection=disabled \
		-Ddtrace=disabled -Dsystemtap=disabled -Dsysprof=disabled -Dlibelf=disabled -Dglib_debug=disabled
}

pkg_json_glib() {
	unpack json-glib
	mesonbuild -Dintrospection=disabled -Ddocumentation=disabled -Dman=false -Dtests=false -Dnls=disabled \
		-Dconformance=false -Dinstalled_tests=false
}

pkg_libxml2() {
	unpack libxml2
	autobuild --without-python --without-lzma --without-icu --without-readline --without-history --without-debug
}

pkg_zstd() {
	unpack zstd
	# CFLAGS and LDFLAGS come from the environment: on the command line they
	# would replace the -fPIC and -shared the makefile adds
	local mk=(CC="$CC" PREFIX=/usr LIBDIR=$LIBDIR HAVE_ZLIB=0 HAVE_LZMA=0 HAVE_LZ4=0)
	make -C lib -j"$JOBS" "${mk[@]}" libzstd libzstd.pc >make.log 2>&1 || { tail -30 make.log; exit 1; }
	make -C lib "${mk[@]}" DESTDIR="$DEST" install-shared install-includes install-pc >install.log 2>&1 ||
		{ tail -30 install.log; exit 1; }
	finish
}

# OpenSSL: the TLS of curl, so OSTree's downloads from Flathub are encrypted
pkg_openssl() {
	unpack openssl
	./Configure linux-x86_64 --prefix=/usr --libdir=lib --openssldir=/etc/ssl shared no-tests no-docs no-static-engine \
		no-ssl3 no-comp no-afalgeng >configure.log 2>&1 || { tail -30 configure.log; exit 1; }
	make -j"$JOBS" >make.log 2>&1 || { grep -B5 -A5 -m5 'error' make.log | head -40; exit 1; }
	make DESTDIR="$DEST" install_sw install_ssldirs >install.log 2>&1 || { tail -30 install.log; exit 1; }
	rm -f "$DEST"/usr/lib/libcrypto.a "$DEST"/usr/lib/libssl.a "$DEST"/etc/ssl/*.dist
	finish
}

# HTTP/2: OSTree fetches thousands of small files, many at once over one connection
pkg_nghttp2() {
	unpack nghttp2
	autobuild --enable-lib-only --disable-python-bindings
}

pkg_curl() {
	unpack curl
	autobuild --with-openssl --with-nghttp2 --with-zlib --with-zstd --without-libpsl --without-brotli --without-libidn2 \
		--without-librtmp --without-libssh2 --disable-ldap --disable-ldaps --disable-manual --disable-docs \
		--with-ca-bundle=/etc/ssl/certs/ca-certificates.crt --with-ca-path=/etc/ssl/certs
}

pkg_libarchive() {
	unpack libarchive
	autobuild --without-xml2 --without-expat --without-openssl --without-lz4 --without-bz2lib --without-libb2 \
		--without-mbedtls --without-nettle --disable-acl --disable-bsdtar --disable-bsdcpio --disable-bsdcat \
		--disable-bsdunzip
}

# GnuPG's libraries find each other with gpgrt-config, which would hand out
# the build machine's /usr/include; pkg-config, told about the layer, doesn't
gpgrt_config() {
	mkdir -p "$WORK/bin"
	cat >"$WORK/bin/gpgrt-config" <<-'EOF'
		#!/bin/sh
		args=
		for a in "$@"; do case $a in --libdir=*) ;; *) args="$args $a" ;; esac; done
		exec pkg-config $args
	EOF
	chmod +x "$WORK/bin/gpgrt-config"
	export GPGRT_CONFIG=$WORK/bin/gpgrt-config
}

gpg_pkg() {
	unpack "$1"
	shift
	gpgrt_config
	autobuild --disable-doc "$@"
}

pkg_libgpg_error() { gpg_pkg libgpg-error --disable-nls --disable-languages --disable-tests; }
pkg_libassuan() { gpg_pkg libassuan; }
pkg_libgcrypt() { gpg_pkg libgcrypt --disable-padlock-support --disable-jent-support; }
pkg_libksba() { gpg_pkg libksba; }
pkg_npth() { gpg_pkg npth --disable-tests; }

# gpg itself: OSTree checks Flathub's signatures with it (through GPGME);
# only what checking signatures and listing keys takes
pkg_gnupg() {
	gpg_pkg gnupg --disable-nls --disable-gpgsm --disable-scdaemon --disable-dirmngr --disable-keyboxd \
		--disable-tpm2d --disable-g13 --disable-wks-tools --disable-card-support --disable-ccid-driver \
		--disable-sqlite --disable-gnutls --disable-ldap --disable-libdns --disable-bzip2 --disable-photo-viewers \
		--disable-ntbtls --disable-tests
}

pkg_gpgme() {
	gpg_pkg gpgme --enable-languages= --disable-gpg-test --disable-gpgsm-test --disable-gpgconf-test \
		--disable-g13-test
}

pkg_libseccomp() {
	unpack libseccomp
	autobuild
}

# FUSE's library (the document portal and AppImages serve files with it);
# Zurbax has its own fusermount3, which asks layerrun's session to mount
pkg_fuse() {
	unpack fuse
	mesonbuild -Dutils=false -Dexamples=false -Dtests=false -Duseroot=false -Ddisable-mtab=true
}

# fusermount/zos-fusermount.c: the layer's fusermount3
pkg_fusermount() {
	$CC $CFLAGS -Wall -o "$ROOT/usr/bin/fusermount3" "$HERE/fusermount/zos-fusermount.c" $LDFLAGS
}

pkg_libfyaml() {
	unpack libfyaml
	cmakebuild -DBUILD_SHARED_LIBS=ON -DBUILD_TESTING=OFF -DENABLE_NETWORK=OFF -DENABLE_LIBCLANG=OFF
}

pkg_libxmlb() {
	unpack libxmlb
	mesonbuild -Dgtkdoc=false -Dintrospection=false -Dtests=false -Dstemmer=false -Dcli=false -Dlzma=enabled -Dzstd=enabled
}

pkg_appstream() {
	unpack appstream
	# data/ only describes appstreamcli itself, with the build machine's
	# appstreamcli when cross building; the tests read it
	sed -i -e "/^subdir('data\/')/d" -e "/^subdir('tests\/')/d" meson.build
	mesonbuild -Dstemming=false -Dsystemd=false -Dvapi=false -Dqt=false -Dcompose=false -Dapt-support=false \
		-Dgir=false -Dtools=true -Dzstd-support=true -Dblake3-support=false -Ddisplay-detection=none -Ddocs=false \
		-Dapidocs=false -Dinstall-docs=false -Dman=false -Dbash-completion=false
}

# e2fsprogs only for a header OSTree reads the file attribute flags from
pkg_e2fsprogs() {
	unpack e2fsprogs
	autobuild_configure --disable-nls --disable-fsck --disable-uuidd \
		--disable-debugfs --disable-imager --disable-resizer --disable-defrag --disable-e2initrd-helper --disable-fuse2fs
	make -C lib/ext2fs ext2_types.h >make.log 2>&1 || { tail -30 make.log; exit 1; }
	install -Dm644 lib/ext2fs/ext2_fs.h lib/ext2fs/ext2_types.h -t "$ROOT/usr/include/ext2fs/"
	install -Dm644 lib/e2p/e2p.pc -t "$ROOT/usr/lib/pkgconfig/"
}

pkg_ostree() {
	unpack ostree
	autobuild --with-curl --without-soup --without-soup3 --with-crypto=openssl --with-gpgme --with-libarchive \
		--without-libsystemd --without-selinux --without-libmount --without-composefs --without-avahi \
		--without-dracut --without-mkinitcpio --disable-rofiles-fuse --disable-man --disable-gtk-doc \
		--disable-introspection --without-ed25519-libsodium --disable-glibtest
}

pkg_bubblewrap() {
	unpack bubblewrap
	mesonbuild -Dman=disabled -Dselinux=disabled -Dtests=false -Dbash_completion=disabled -Dzsh_completion=disabled
}

pkg_xdg_dbus_proxy() {
	unpack xdg-dbus-proxy
	mesonbuild -Dman=disabled -Dtests=false
}

pkg_flatpak() {
	unpack flatpak
	# its icon checker only runs when building apps (flatpak build-export),
	# and is the only part that needs gdk-pixbuf
	sed -i -e "/^gdk_pixbuf_dep = /d" -e "/subdir('icon-validator')/d" meson.build
	mesonbuild -Dsystem_helper=disabled -Dsystemd=disabled -Dmalcontent=disabled -Dselinux_module=disabled \
		-Ddconf=disabled -Dgir=disabled -Dgtkdoc=disabled -Ddocbook_docs=disabled -Dman=disabled -Dtests=false \
		-Dinstalled_tests=false -Dseccomp=enabled -Dxauth=disabled -Dwayland_security_context=disabled \
		-Dlibzstd=enabled -Dauto_sideloading=false -Dsystem_bubblewrap=/usr/bin/bwrap \
		-Dsystem_dbus_proxy=/usr/bin/xdg-dbus-proxy -Dsystem_fusermount=/usr/bin/fusermount3 \
		-Dsystem_install_dir=/var/lib/flatpak
	# Flathub, ready to add to a user's installation (layerrun's session does)
	install -Dm644 "$HERE/files/flathub.flatpakrepo" "$ROOT/usr/share/zos/flathub.flatpakrepo"
}

ORDER+=(pcre2 glib json_glib libxml2 zstd openssl nghttp2 curl libarchive libgpg_error libassuan libgcrypt libksba
	npth gnupg gpgme libseccomp fuse fusermount libfyaml libxmlb appstream e2fsprogs ostree bubblewrap xdg_dbus_proxy flatpak)

# ---- System files -------------------------------------------------------------------------

pkg_tzdata() {
	# tzdata's tarball has no top folder
	local file
	file=$(fetch tzdata)
	mkdir -p "$WORK/tz" && tar -xzf "$file" -C "$WORK/tz" && cd "$WORK/tz"
	local zic=zic
	command -v zic >/dev/null || zic="$ROOT/usr/lib/ld-linux-x86-64.so.2 --library-path $ROOT/usr/lib $ROOT/usr/bin/zic"
	$zic -d "$ROOT/usr/share/zoneinfo" -b fat africa antarctica asia australasia europe northamerica southamerica etcetera backward
	install -m644 zone.tab zone1970.tab iso3166.tab "$ROOT/usr/share/zoneinfo/"
	ln -sf /usr/share/zoneinfo/UTC "$ROOT/etc/localtime"
}

pkg_etc() {
	# the certificate authorities Zurbax itself trusts, where Linux programs look for them
	install -Dm644 "$TOP/data/certs/roots.pem" "$ROOT/etc/ssl/certs/ca-certificates.crt"
	ln -sf certs/ca-certificates.crt "$ROOT/etc/ssl/cert.pem"
	install -d "$ROOT/etc/pki/tls/certs"
	ln -sf /etc/ssl/certs/ca-certificates.crt "$ROOT/etc/pki/tls/certs/ca-bundle.crt"
	cat >"$ROOT/etc/nsswitch.conf" <<-'EOF'
		passwd: files
		group: files
		shadow: files
		hosts: files dns
		networks: files
		protocols: files
		services: files
	EOF
	printf 'tcp 6 TCP\nudp 17 UDP\n' >"$ROOT/etc/protocols"
	printf 'http 80/tcp\nhttps 443/tcp\n' >"$ROOT/etc/services"
	printf '/bin/sh\n/bin/bash\n/usr/bin/bash\n' >"$ROOT/etc/shells"
	cp "$TOP/data/os-release" "$ROOT/etc/os-release"
	install -m755 "$HERE/files/lsb_release" "$ROOT/usr/bin/lsb_release"
	printf 'LANG=en_US.UTF-8\n' >"$ROOT/etc/locale.conf"
}

# ---- Steam ------------------------------------------------------------------------------------

# Valve's launcher: the steam command, and the bootstrap it unpacks into
# ~/.local/share/Steam on first start (the client then updates itself).
pkg_steam() {
	unpack steam
	install -Dm755 bin_steam.sh "$ROOT/usr/lib/steam/bin_steam.sh"
	install -m644 bootstraplinux_ubuntu12_32.tar.xz "$ROOT/usr/lib/steam/"
	# relative, so it also resolves from outside the layer (layerrun --check, the Steam app)
	ln -sf ../lib/steam/bin_steam.sh "$ROOT/usr/bin/steam"
	install -Dm644 steam.desktop "$ROOT/usr/share/applications/steam.desktop"
	install -Dm644 icons/256/steam.png "$ROOT/usr/share/icons/hicolor/256x256/apps/steam.png"
}

# Debugging information out of every program and library (some packages
# build with it); it's large and of no use in the layer.
pkg_strip() {
	find "$ROOT/usr/bin" "$ROOT/usr/lib" "$ROOT/usr/lib32" -type f \( -perm -u+x -o -name '*.so*' \) -print0 |
		while IFS= read -r -d '' f; do
			[ "$(head -c4 "$f")" = $'\x7fELF' ] && strip --strip-debug "$f" 2>/dev/null
		done
	return 0
}

# the dynamic linker's cache: Steam's runtime reads it to find the layer's
# graphics drivers
pkg_ldconfig() {
	# -r enters the layer with chroot, which takes root (or a user namespace)
	local ldc=("$ROOT/usr/bin/ldconfig" -r "$ROOT")
	if [ "$(id -u)" = 0 ]; then
		"${ldc[@]}"
	elif unshare -r true 2>/dev/null; then
		unshare -r "${ldc[@]}"
	else
		sudo -n "${ldc[@]}"
	fi
}

ORDER+=(tzdata etc steam strip ldconfig)
