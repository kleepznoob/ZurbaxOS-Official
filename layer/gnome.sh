# GNOME, from GNOME's own source releases (download.gnome.org) and the
# upstream projects it builds on; sourced by build.sh after packages.sh.
# Everything goes into the layer next to Mesa and Flatpak; Zurbax starts a
# GNOME session from it. The pieces of a desktop system GNOME talks to (a
# login manager, power, accounts) are Zurbax's own, not these.

# Programs the build runs that read type information (PyGObject,
# blueprint-compiler) read the layer's: it describes what's being built for
export GI_TYPELIB_PATH=$ROOT/usr/lib/girepository-1.0

# ---- Tools the build runs ---------------------------------------------------------------

# GLib for the build machine, in the layer's version: GObject Introspection's
# scanner is built against it
# A Zurbax program (system/NAME, static, no outside code) installed into the
# layer at PATH: the pieces of the GNOME session that are Zurbax's own.
zurbax_program() {
	env -u CC -u CXX -u CFLAGS -u LDFLAGS make -C "$TOP" --no-print-directory -s "build/bin/$1" ||
		{ echo "couldn't build $1" >&2; exit 1; }
	install -Dm755 "$TOP/build/bin/$1" "$ROOT$2"
}

pkg_native_glib() {
	unpack glib
	native_env
	meson setup _build --prefix="$NATIVE" --libdir=lib --buildtype=release -Dintrospection=disabled -Dtests=false \
		-Dman-pages=disabled -Ddocumentation=false -Dnls=disabled -Dselinux=disabled -Dlibmount=disabled -Dsysprof=disabled \
		>meson.log 2>&1 || { tail -30 meson.log; exit 1; }
	ninja -C _build install >ninja.log 2>&1 || { grep -A12 -m3 FAILED ninja.log | head -40; exit 1; }
}

# The scanner (g-ir-scanner) and compiler (g-ir-compiler) that turn each
# library's headers into the type information GNOME Shell's JavaScript
# reads; the layer's builds run them through cross-bin (build.sh)
pkg_native_gobject_introspection() {
	unpack gobject-introspection
	native_env
	PKG_CONFIG_PATH=$NATIVE/lib/pkgconfig meson setup _build --prefix="$NATIVE" --libdir=lib --buildtype=release \
		-Dcairo=disabled -Ddoctool=disabled -Dgtk_doc=false -Dbuild_introspection_data=false \
		>meson.log 2>&1 || { tail -30 meson.log; exit 1; }
	ninja -C _build install >ninja.log 2>&1 || { grep -A12 -m3 FAILED ninja.log | head -40; exit 1; }
}

ORDER+=(native_glib native_gobject_introspection)

# ---- GLib's type information, and the rest of GObject Introspection ---------------------

# its pkg-config file and the scanner's files, which GLib looks for before
# making its own type information; the type information of non-GObject
# libraries (cairo, fontconfig, X11...) comes once cairo is built (gi_data)
pkg_gobject_introspection() {
	unpack gobject-introspection
	# the scanner itself only runs on the build machine: no Python module for the layer
	sed -i "/check_header('Python.h'/d; s/^subdir('giscanner')/# &/" meson.build
	mesonbuild -Dgi_cross_use_prebuilt_gi=true -Dgi_cross_binary_wrapper="$L/cross-bin/run-target" \
		-Dgi_cross_ldd_wrapper="$L/cross-bin/ldd-target" -Dgi_cross_pkgconfig_sysroot_path="$ROOT" \
		-Dcairo=disabled -Ddoctool=disabled -Dgtk_doc=false -Dbuild_introspection_data=false
}

# GLib again, now with GLib's, GObject's and Gio's type information
pkg_glib_gir() {
	unpack glib
	glib_props
	mesonbuild --cross-file "$WORK/glib.ini" -Dselinux=disabled -Dlibmount=disabled -Dman-pages=disabled \
		-Ddocumentation=false -Dtests=false -Dinstalled_tests=false -Dnls=disabled -Dintrospection=enabled \
		-Ddtrace=disabled -Dsystemtap=disabled -Dsysprof=disabled -Dlibelf=disabled -Dglib_debug=disabled
}


# json-glib again, with its type information (the first build, for Flatpak, had none)
pkg_json_glib_gir() {
	unpack json-glib
	mesonbuild -Dintrospection=enabled -Ddocumentation=disabled -Dman=false -Dtests=false -Dnls=disabled \
		-Dconformance=false -Dinstalled_tests=false
}

ORDER+=(gobject_introspection glib_gir json_glib_gir)

# ---- Pictures, text and the graphics under GTK ------------------------------------------

pkg_libpng() { unpack libpng; autobuild --disable-tools; }

pkg_libjpeg_turbo() {
	unpack libjpeg-turbo
	cmakebuild -DENABLE_STATIC=OFF -DWITH_TURBOJPEG=OFF -DWITH_TOOLS=OFF -DWITH_TESTS=OFF
}

pkg_libtiff() {
	unpack libtiff
	autobuild --disable-docs --disable-tests --disable-contrib --disable-tools --disable-webp --disable-jbig --disable-lerc \
		--disable-libdeflate --disable-zstd --disable-lzma
}

pkg_lcms2() { unpack lcms2; autobuild --without-jpeg --without-tiff; }

pkg_fribidi() { unpack fribidi; mesonbuild -Ddocs=false -Dtests=false -Dbin=false; }

pkg_harfbuzz() {
	unpack harfbuzz
	mesonbuild -Dglib=enabled -Dgobject=enabled -Dintrospection=enabled -Dfreetype=enabled -Dcairo=disabled -Dicu=disabled \
		-Dtests=disabled -Ddocs=disabled -Dutilities=disabled -Dbenchmark=disabled
}

pkg_cairo() {
	unpack cairo
	mesonbuild -Dglib=enabled -Dpng=enabled -Dfreetype=enabled -Dfontconfig=enabled -Dxlib=enabled -Dxcb=enabled \
		-Dtests=disabled -Dspectre=disabled -Dsymbol-lookup=disabled -Dgtk_doc=false
}

# the type information of cairo, fontconfig, freetype, X11 and the other
# libraries GObject Introspection describes itself: its files, compiled
pkg_gi_data() {
	unpack gobject-introspection
	local gir=$DEST/usr/share/gir-1.0 typelib=$DEST$LIBDIR/girepository-1.0 f
	mkdir -p "$gir" "$typelib"
	sed 's/@CAIRO_SHARED_LIBRARY@/libcairo-gobject.so.2/' gir/cairo-1.0.gir.in >gir/cairo-1.0.gir
	for f in gir/*.gir; do
		case $f in */win32-*) continue ;; esac
		install -m644 "$f" "$gir/"
	done
	for f in "$gir"/{cairo-1.0,DBus-1.0,DBusGLib-1.0,fontconfig-2.0,freetype2-2.0,GL-1.0,libxml2-2.0,xfixes-4.0,xft-2.0,xlib-2.0,xrandr-1.3,Vulkan-1.0}.gir; do
		"$L/cross-bin/g-ir-compiler" --includedir="$gir" -o "$typelib/$(basename "${f%.gir}").typelib" "$f"
	done
}

pkg_pango() {
	unpack pango
	mesonbuild -Dintrospection=enabled -Dbuild-testsuite=false -Dbuild-examples=false -Dxft=disabled -Dfontconfig=enabled \
		-Dcairo=enabled -Ddocumentation=false -Dman-pages=false
}

pkg_graphene() {
	unpack graphene
	mesonbuild -Dintrospection=enabled -Dtests=false -Dinstalled_tests=false -Dgtk_doc=false
}

pkg_libxkbcommon() {
	unpack libxkbcommon
	mesonbuild -Denable-x11=true -Denable-wayland=true -Denable-xkbregistry=true -Denable-docs=false -Denable-tools=false \
		-Denable-bash-completion=false -Dxkb-config-root=/usr/share/X11/xkb -Dx-locale-root=/usr/share/X11/locale
}

pkg_gdk_pixbuf() {
	unpack gdk-pixbuf
	mesonbuild -Dintrospection=enabled -Dpng=enabled -Djpeg=enabled -Dtiff=enabled -Dglycin=disabled -Dothers=disabled \
		-Dman=false -Ddocumentation=false -Dtests=false -Dinstalled_tests=false
}

ORDER+=(libpng libjpeg_turbo libtiff lcms2 cairo gi_data fribidi harfbuzz pango graphene libxkbcommon shared_mime_info gdk_pixbuf)

# ---- Desktop data and services GTK and GNOME read --------------------------------------

pkg_at_spi2_core() {
	unpack at-spi2-core
	mesonbuild -Dintrospection=enabled -Ddocs=false -Dx11=enabled -Duse_systemd=false -Ddbus_daemon=/usr/bin/dbus-daemon \
		-Ddefault_bus=dbus-daemon
}

pkg_gsettings_desktop_schemas() { unpack gsettings-desktop-schemas; mesonbuild -Dintrospection=true; }

pkg_dconf() { unpack dconf; mesonbuild -Dbash_completion=false -Dman=false -Dvapi=false -Dgtk_doc=false; }

pkg_hicolor_icon_theme() { unpack hicolor-icon-theme; mesonbuild; }

pkg_adwaita_icon_theme() { unpack adwaita-icon-theme; mesonbuild; }

pkg_adwaita_fonts() { unpack adwaita-fonts; mesonbuild; }

pkg_shared_mime_info() { unpack shared-mime-info; mesonbuild -Dupdate-mimedb=false -Dbuild-tests=false -Dbuild-tools=true; }

pkg_iso_codes() { unpack iso-codes; mesonbuild; }

ORDER+=(at_spi2_core gsettings_desktop_schemas dconf hicolor_icon_theme adwaita_icon_theme adwaita_fonts iso_codes)

# ---- GTK and the libraries around it ----------------------------------------------------

# Sass, which GNOME's style sheets are written in: libsass and its command
# line, sassc, for the build machine
pkg_native_sassc() {
	native_env
	unpack libsass
	echo 3.6.6 >VERSION
	autoreconf -fi >autoreconf.log 2>&1
	./configure --prefix="$NATIVE" --disable-static >configure.log 2>&1 || { tail -20 config.log; exit 1; }
	make -j"$JOBS" >make.log 2>&1 && make install >install.log 2>&1 || { tail -20 make.log; exit 1; }
	unpack sassc
	echo 3.6.2 >VERSION
	autoreconf -fi >autoreconf.log 2>&1
	PKG_CONFIG_PATH=$NATIVE/lib/pkgconfig ./configure --prefix="$NATIVE" --with-libsass="$NATIVE" LDFLAGS="-Wl,-rpath,$NATIVE/lib" \
		>configure.log 2>&1 || { tail -20 config.log; exit 1; }
	make -j"$JOBS" >make.log 2>&1 && make install >install.log 2>&1 || { tail -20 make.log; exit 1; }
}

# glslc, the shader compiler GTK's Vulkan renderer compiles its shaders
# with: Google's shaderc, with Khronos's glslang and SPIR-V tools inside
pkg_native_glslc() {
	native_env
	local d
	for d in glslang spirv-tools spirv-headers; do
		(unpack "$d")
	done
	unpack shaderc
	for d in glslang spirv-tools spirv-headers; do
		rm -rf "third_party/$d"
		mv "$(find "$WORK/$d" -mindepth 1 -maxdepth 1 -type d | head -1)" "third_party/$d"
	done
	cmake -S . -B _build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$NATIVE" -DSHADERC_SKIP_TESTS=ON \
		-DSHADERC_SKIP_EXAMPLES=ON -DSHADERC_SKIP_COPYRIGHT_CHECK=ON -DSPIRV_SKIP_TESTS=ON -DSPIRV_SKIP_EXECUTABLES=ON \
		-DENABLE_GLSLANG_BINARIES=OFF -DSKIP_GLSLANG_INSTALL=ON -DSKIP_SPIRV_TOOLS_INSTALL=ON >cmake.log 2>&1 ||
		{ tail -30 cmake.log; exit 1; }
	ninja -C _build glslc_exe >ninja.log 2>&1 || { grep -A12 -m3 FAILED ninja.log | head -40; exit 1; }
	install -m755 _build/glslc/glslc "$NATIVE/bin/glslc"
}

pkg_p11_kit() {
	unpack p11-kit
	mesonbuild -Dsystemd=disabled -Dbash_completion=disabled -Dgtk_doc=false -Dman=false -Dnls=false -Dtest=false \
		-Dtrust_module=disabled
}

pkg_libsecret() {
	unpack libsecret
	mesonbuild -Dgtk_doc=false -Dmanpage=false -Dvapi=false -Dintrospection=true -Dcrypto=libgcrypt -Dbash_completion=disabled
}

pkg_libgudev() { unpack libgudev; mesonbuild -Dintrospection=enabled -Dvapi=disabled -Dtests=disabled; }

pkg_vulkan_headers() { unpack vulkan-headers; cmakebuild -DVULKAN_HEADERS_ENABLE_TESTS=OFF -DVULKAN_HEADERS_ENABLE_MODULE=OFF; }

pkg_gtk() {
	unpack gtk
	mesonbuild -Dx11-backend=true -Dwayland-backend=true -Dvulkan=enabled -Dintrospection=enabled -Dmedia-gstreamer=disabled \
		-Dprint-cups=disabled -Dcolord=disabled -Dcloudproviders=disabled -Dsysprof=disabled -Daccesskit=disabled \
		-Dbuild-demos=false -Dbuild-tests=false -Dbuild-testsuite=false -Dbuild-examples=false -Ddocumentation=false \
		-Dman-pages=false -Dscreenshots=false
}

# cargo-c, which builds Rust libraries with a C interface (librsvg's, glycin's)
pkg_native_cargo_c() {
	native_env
	CARGO_HOME=$L/cargo-native "$HOME/.cargo/bin/cargo" install --quiet --locked --root "$NATIVE" cargo-c --version 0.10.25 \
		>"$WORK/cargo.log" 2>&1 || { tail -30 "$WORK/cargo.log"; exit 1; }
}

pkg_librsvg() {
	unpack librsvg
	rust_env
	cargo_fetch
	mesonbuild -Dintrospection=enabled -Dpixbuf=enabled -Dpixbuf-loader=enabled -Ddocs=disabled -Dvala=disabled -Dtests=false \
		-Davif=disabled -Dtriplet=x86_64-unknown-linux-gnu
}

pkg_glycin() {
	unpack glycin
	rust_env
	cargo_fetch
	# glycin's build keeps a cargo home of its own: the fetched crates' one
	mkdir -p _build && ln -s "$CARGO_HOME" _build/cargo-home
	mesonbuild -Dlibglycin=true -Dlibglycin-gtk4=true -Dglycin-loaders=true -Dloaders=glycin-image-rs,glycin-svg \
		-Dintrospection=true -Dvapi=false -Dtests=false -Dglycin-thumbnailer=true
}

pkg_libadwaita() {
	unpack libadwaita
	mesonbuild -Dintrospection=enabled -Dvapi=false -Dtests=false -Dexamples=false -Ddocumentation=false
}

pkg_gcr() {
	unpack gcr
	mesonbuild -Dgtk4=true -Dintrospection=true -Dvapi=false -Dgtk_doc=false -Dcrypto=libgcrypt -Dsystemd=disabled \
		-Dssh_agent=false
}

pkg_gnome_desktop() {
	unpack gnome-desktop
	mesonbuild -Dintrospection=true -Dgtk_doc=false -Ddesktop_docs=false -Dbuild_gtk4=true -Dlegacy_library=false \
		-Dsystemd=disabled -Dudev=enabled -Dinstalled_tests=false
}

ORDER+=(native_sassc native_glslc p11_kit libsecret libgudev vulkan_headers gtk native_cargo_c librsvg glycin libadwaita gcr gnome_desktop)

# ---- Input, displays, colour and sound for Mutter ----------------------------------------

pkg_mtdev() { unpack mtdev; autobuild; }

pkg_libevdev() { unpack libevdev; mesonbuild -Dtests=disabled -Ddocumentation=disabled; }

pkg_libwacom() { unpack libwacom; mesonbuild -Dtests=disabled -Dudev-dir=/usr/lib/udev; }

pkg_libinput() {
	unpack libinput
	mesonbuild -Dlibwacom=true -Ddebug-gui=false -Dtests=false -Ddocumentation=false -Dudev-dir=/usr/lib/udev -Dmtdev=true \
		-Dlua-plugins=disabled
}

# emulated input, for remote desktop and the input portal; its code is
# generated with Python modules the build machine's Python gets first
pkg_libei() {
	unpack libei
	"$L/venv/bin/pip" install -q attrs jinja2 >/dev/null
	mesonbuild -Dtests=disabled -Ddocumentation=[] -Dliboeffis=disabled
}

# monitors' manufacturers, by the codes in their EDID
pkg_hwdata() {
	unpack hwdata
	./configure --prefix=/usr --libdir=$LIBDIR --datadir=/usr/share >configure.log 2>&1
	make DESTDIR="$DEST" install >install.log 2>&1 || { tail -20 install.log; exit 1; }
	finish
}

pkg_libdisplay_info() {
	unpack libdisplay-info
	# it reads hwdata's monitor makers while it builds: the layer's copy, found from the build machine
	mkdir -p "$NATIVE/share/pkgconfig"
	sed "s|^prefix=.*|prefix=$ROOT/usr|; s|\${pc_sysrootdir}||" "$ROOT/usr/share/pkgconfig/hwdata.pc" >"$NATIVE/share/pkgconfig/hwdata.pc"
	mesonbuild
}

pkg_xcb_util() { xorg xcb-util; }

# glibc's realloc(NULL, n) works: the answer configure can't test across machines
pkg_startup_notification() { unpack startup-notification; lf_cv_sane_realloc=yes autobuild; }

pkg_sqlite() {
	unpack sqlite
	autobuild --enable-fts5 --enable-math --disable-readline --disable-static
}

pkg_libusb() { unpack libusb; autobuild --disable-udev --disable-examples-build --disable-tests-build; }

pkg_libgusb() { unpack libgusb; mesonbuild -Dintrospection=true -Dvapi=false -Dtests=false -Ddocs=false -Dumockdev=disabled; }

pkg_duktape() {
	unpack duktape
	make -f Makefile.sharedlibrary CC="$CC" CFLAGS="$CFLAGS -fPIC" INSTALL_PREFIX=/usr LIBDIR=/lib >make.log 2>&1 ||
		{ tail -20 make.log; exit 1; }
	make -f Makefile.sharedlibrary INSTALL_PREFIX="$DEST/usr" LIBDIR=/lib install >install.log 2>&1 || { tail -20 install.log; exit 1; }
	mkdir -p "$DEST$LIBDIR/pkgconfig"
	cat >"$DEST$LIBDIR/pkgconfig/duktape.pc" <<-EOF
		prefix=/usr
		libdir=$LIBDIR
		includedir=/usr/include
		Name: duktape
		Description: An embeddable JavaScript engine
		Version: 2.7.0
		Libs: -L\${libdir} -lduktape
		Cflags: -I\${includedir}
	EOF
	finish
}

# polkit: the agent library GNOME Shell asks for passwords with, and the
# authority's client library; the authority itself is Zurbax's
pkg_polkit() {
	unpack polkit
	mesonbuild -Dlibs-only=true -Dintrospection=true -Dexamples=false -Dtests=false -Dman=false -Dgtk_doc=false \
		-Dauthfw=shadow -Dsession_tracking=logind -Dsystemdsystemunitdir=/usr/lib/systemd/system
	# the agent's password check: Zurbax's own (system/polkit-helper), asking zsys
	zurbax_program polkit-helper /usr/lib/polkit-1/polkit-agent-helper-1
}

pkg_colord() {
	unpack colord
	mesonbuild -Ddaemon=true -Dbash_completion=false -Dsystemd=false -Dlibcolordcompat=false -Dsane=false -Dvapi=false \
		-Dprint_profiles=false -Dman=false -Ddocs=false -Dinstalled_tests=false -Dtests=false -Dudev_rules=false \
		-Dargyllcms_sensor=false
}

pkg_libogg() { unpack libogg; autobuild; }

pkg_libvorbis() { unpack libvorbis; autobuild --disable-docs --disable-examples --disable-oggtest; }

# event sounds (the camera shutter, the bell); Mutter plays them with it
pkg_libcanberra() {
	unpack libcanberra
	CFLAGS="$CFLAGS -std=gnu17 -Wno-error=incompatible-pointer-types -Wno-error=implicit-function-declaration" \
		autobuild --disable-oss --disable-gtk --disable-gtk3 --enable-pulse --enable-alsa --disable-gstreamer --disable-lynx \
		--disable-tdb --disable-udev --with-builtin=dso
}

# only the capture library programs mark their timings with, for Sysprof
pkg_sysprof() {
	unpack sysprof
	mesonbuild -Dgtk=false -Dlibsysprof=false -Dsysprofd=none -Dpolkit-agent=disabled -Dhelp=false -Dexamples=false -Dtools=false \
		-Dtests=false -Dinstall-static=true
}

ORDER+=(sd_login mtdev libevdev libwacom libinput libei hwdata libdisplay_info xcb_util startup_notification sqlite libusb libgusb
	duktape polkit colord libogg libvorbis libcanberra sysprof)

# ---- JavaScript: SpiderMonkey (Mozilla's engine, from Firefox's ESR source) and GJS -------

# cbindgen, which writes C headers for Rust code (SpiderMonkey's build needs it)
pkg_native_cbindgen() {
	native_env
	CARGO_HOME=$L/cargo-native "$HOME/.cargo/bin/cargo" install --quiet --locked --root "$NATIVE" cbindgen --version 0.29.4 \
		>"$WORK/cargo.log" 2>&1 || { tail -30 "$WORK/cargo.log"; exit 1; }
}

pkg_mozjs() {
	unpack mozjs
	rust_env
	unset CARGO_BUILD_TARGET RUSTFLAGS
	export HOST_CC=gcc HOST_CXX=g++ AR=ar NM=nm RANLIB=ranlib MOZBUILD_STATE_PATH=$WORK/mozbuild MACH_BUILD_PYTHON_NATIVE_PACKAGE_SOURCE=none
	mkdir -p obj "$MOZBUILD_STATE_PATH"
	cd obj
	../js/src/configure --prefix=/usr --libdir=$LIBDIR --host=x86_64-pc-linux-gnu --target=x86_64-unknown-linux-gnu \
		--disable-debug --enable-optimize --enable-release --disable-jemalloc --disable-strip --disable-tests \
		--enable-shared-js --with-system-zlib --with-intl-api --disable-ctypes \
		>configure.log 2>&1 || { tail -40 configure.log; exit 1; }
	make -j"$JOBS" >make.log 2>&1 || { grep -B5 -A10 -m5 'error' make.log | head -80; exit 1; }
	make DESTDIR="$DEST" install >install.log 2>&1 || { tail -30 install.log; exit 1; }
	rm -f "$DEST$LIBDIR"/libjs_static.ajs
	# programs that embed it (GJS) build for Unix: its headers need to be told
	sed -i 's|^Cflags: \(.*\)|Cflags: \1 -DXP_UNIX|' "$DEST$LIBDIR/pkgconfig/mozjs-140.pc"
	finish
}

pkg_gjs() {
	unpack gjs
	mesonbuild -Dinstalled_tests=false -Dskip_dbus_tests=true -Dskip_gtk_tests=true -Dprofiler=disabled -Dreadline=disabled
}

ORDER+=(native_cbindgen mozjs gjs)

# ---- PipeWire: screen sharing, remote desktop and cameras ---------------------------------
# Sound stays with Zurbax's sound service (through PulseAudio's server in
# the layer); PipeWire carries video: the screen to a portal, a camera to an app.

# ---- Sound and video: codecs, FFmpeg, GStreamer ----------------------------------------
# What GNOME's apps play and record with (and GNOME Shell's screen recorder):
# the common codecs, FFmpeg, and GStreamer with its plugins.

# NASM: the assembler for the codecs' hand-written fast paths
pkg_native_nasm() {
	unpack nasm
	native_env
	./configure --prefix="$NATIVE" >configure.log 2>&1 || { tail -20 configure.log; exit 1; }
	make -j"$JOBS" >make.log 2>&1 && make install >install.log 2>&1 || { tail -20 make.log; exit 1; }
}

pkg_opus() { unpack opus; mesonbuild -Dtests=disabled -Ddocs=disabled -Dextra-programs=disabled; }

pkg_flac() { unpack flac; autobuild --disable-doxygen-docs --disable-xmms-plugin --disable-cpplibs --disable-examples --disable-programs; }

pkg_dav1d() { unpack dav1d; mesonbuild -Denable_tools=false -Denable_tests=false; }

pkg_libvpx() {
	unpack libvpx
	./configure --target=x86_64-linux-gcc --prefix=/usr --libdir=$LIBDIR --enable-shared --disable-static --disable-examples \
		--disable-tools --disable-docs --disable-unit-tests --enable-pic --enable-vp9-highbitdepth --as=nasm \
		>configure.log 2>&1 || { tail -30 configure.log; exit 1; }
	make -j"$JOBS" >make.log 2>&1 || { grep -B5 -A5 -m5 'error' make.log | head -40; exit 1; }
	make DESTDIR="$DEST" install >install.log 2>&1 || { tail -20 install.log; exit 1; }
	finish
}

# VA-API: video decoding on the GPU
pkg_libva() { unpack libva; mesonbuild -Dwith_x11=yes -Dwith_wayland=yes -Dwith_glx=no -Denable_docs=false; }

pkg_ffmpeg() {
	unpack ffmpeg
	./configure --prefix=/usr --libdir=$LIBDIR --shlibdir=$LIBDIR --enable-cross-compile --arch=x86_64 --target-os=linux \
		--cc="$CC" --cxx="$CXX" --pkg-config="$PKG_CONFIG" --host-cc=gcc --sysroot="$ROOT" --extra-cflags="$CFLAGS" \
		--extra-ldflags="$LDFLAGS" --enable-shared --disable-static --disable-doc --disable-debug --enable-pic \
		--enable-libopus --enable-libdav1d --enable-libvpx --enable-vaapi --enable-vulkan --enable-libdrm \
		--enable-libpulse --enable-alsa --disable-sdl2 --disable-xlib --enable-zlib \
		>configure.log 2>&1 || { tail -30 configure.log; tail -30 ffbuild/config.log; exit 1; }
	make -j"$JOBS" >make.log 2>&1 || { grep -B5 -A5 -m5 'error' make.log | head -40; exit 1; }
	make DESTDIR="$DEST" install >install.log 2>&1 || { tail -20 install.log; exit 1; }
	finish
}

gst_common=(-Dtests=disabled -Dexamples=disabled -Ddoc=disabled -Dnls=disabled)

pkg_gstreamer() {
	unpack gstreamer
	mesonbuild "${gst_common[@]}" -Dintrospection=enabled -Dbenchmarks=disabled -Dtools=enabled -Dlibunwind=disabled \
		-Dlibdw=disabled -Dbash-completion=disabled -Dptp-helper=disabled
}

pkg_gst_plugins_base() {
	unpack gst-plugins-base
	mesonbuild "${gst_common[@]}" -Dintrospection=enabled -Dgl=enabled -Dgl_api=opengl,gles2 -Dgl_platform=egl \
		-Dgl_winsys=wayland,egl,x11 -Dalsa=enabled -Dogg=enabled -Dvorbis=enabled -Dopus=enabled -Dtheora=disabled \
		-Dcdparanoia=disabled -Dlibvisual=disabled -Dtremor=disabled -Dqt5=disabled
}

pkg_gst_plugins_good() {
	unpack gst-plugins-good
	mesonbuild "${gst_common[@]}" -Dv4l2=enabled -Dpulse=enabled -Dvpx=enabled -Dflac=enabled -Dlame=disabled -Dmpg123=disabled \
		-Dqt5=disabled -Dqt6=disabled -Dgtk3=disabled -Ddv=disabled -Ddv1394=disabled -Daalib=disabled -Dlibcaca=disabled \
		-Djack=disabled -Dshout2=disabled -Dtaglib=disabled -Dtwolame=disabled -Dwavpack=disabled -Dspeex=disabled \
		-Damrnb=disabled -Damrwbdec=disabled -Dsoup=enabled -Dadaptivedemux2=enabled -Dximagesrc=disabled
}

pkg_gst_plugins_bad() {
	unpack gst-plugins-bad
	mesonbuild "${gst_common[@]}" -Dintrospection=enabled -Dgpl=disabled -Dva=enabled -Dvulkan=disabled -Dwayland=enabled \
		-Dopus=enabled -Dsndfile=enabled -Daom=disabled -Dsvtav1=disabled -Dx265=disabled -Dopenh264=disabled \
		-Dwebrtc=disabled -Dwebrtcdsp=disabled -Dsrtp=disabled -Dmsdk=disabled -Dqsv=disabled -Dnvcodec=disabled \
		-Dopencv=disabled -Dopenexr=disabled -Dopenjpeg=disabled -Dlibde265=disabled -Dzbar=disabled -Dzxing=disabled \
		-Dfluidsynth=disabled -Dmpeg2enc=disabled -Dmplex=disabled -Dsoundtouch=disabled -Dspandsp=disabled \
		-Dgsm=disabled -Drtmp=disabled -Dladspa=disabled -Dlv2=disabled -Dchromaprint=disabled -Dcurl=disabled \
		-Ddtls=disabled -Dhls-crypto=openssl -Dsctp=disabled -Daja=disabled -Ddecklink=disabled -Dwpe=disabled
}

pkg_gst_plugins_ugly() { unpack gst-plugins-ugly; mesonbuild -Dtests=disabled -Ddoc=disabled -Dnls=disabled -Dgpl=disabled; }

pkg_gst_libav() { unpack gst-libav; mesonbuild -Dtests=disabled -Ddoc=disabled; }

ORDER+=(native_nasm opus flac dav1d libvpx libva ffmpeg gstreamer gst_plugins_base gst_plugins_good gst_plugins_bad gst_plugins_ugly gst_libav)

pkg_pipewire() {
	unpack pipewire
	mesonbuild -Dsession-managers=[] -Dlibsystemd=disabled -Dsystemd-user-service=disabled -Dlogind=disabled -Dselinux=disabled \
		-Ddocs=disabled -Dman=disabled -Dtests=disabled -Dexamples=disabled -Dinstalled_tests=disabled -Dpipewire-alsa=disabled \
		-Dpipewire-jack=disabled -Djack=disabled -Dpipewire-v4l2=enabled -Dv4l2=enabled -Dlibcamera=disabled -Dvulkan=disabled \
		-Dbluez5=disabled -Dgstreamer=enabled -Decho-cancel-webrtc=disabled -Droc=disabled -Draop=disabled -Davahi=disabled \
		-Dlv2=disabled -Dsdl2=disabled -Dsndfile=enabled -Dlibpulse=enabled -Dlibusb=enabled -Dx11=enabled -Dlibffado=disabled \
		-Dreadline=disabled -Dopus=enabled -Dlibmysofa=disabled -Dbluez5-codec-lc3plus=disabled -Dsnap=disabled \
		-Dcompress-offload=disabled -Debur128=disabled
}

pkg_lua() {
	unpack lua
	make -j"$JOBS" linux CC="$CC -fPIC" MYCFLAGS="$CFLAGS" MYLDFLAGS="$LDFLAGS" >make.log 2>&1 || { tail -20 make.log; exit 1; }
	make INSTALL_TOP="$DEST/usr" INSTALL_LIB="$DEST$LIBDIR" install >install.log 2>&1
	# a shared library and its pkg-config file, as WirePlumber looks for it
	$CC -shared -Wl,-soname,liblua5.4.so.0 $LDFLAGS -o "$DEST$LIBDIR/liblua5.4.so.0" -Wl,--whole-archive src/liblua.a \
		-Wl,--no-whole-archive -lm -ldl
	ln -sf liblua5.4.so.0 "$DEST$LIBDIR/liblua5.4.so"
	rm -f "$DEST$LIBDIR/liblua.a"
	mkdir -p "$DEST$LIBDIR/pkgconfig"
	cat >"$DEST$LIBDIR/pkgconfig/lua5.4.pc" <<-EOF
		prefix=/usr
		libdir=$LIBDIR
		includedir=/usr/include
		Name: Lua
		Description: An extensible extension language
		Version: 5.4.8
		Libs: -L\${libdir} -llua5.4 -lm
		Cflags: -I\${includedir}
	EOF
	finish
}

pkg_wireplumber() {
	unpack wireplumber
	mesonbuild -Dsystem-lua=true -Dsystem-lua-version=5.4 -Dintrospection=disabled -Ddoc=disabled -Dtests=false \
		-Dsystemd=disabled -Delogind=disabled -Dmodules=true -Ddaemon=true -Dtools=true
}

ORDER+=(pipewire lua wireplumber)

# ---- Mutter: GNOME's compositor and window manager -----------------------------------------

# cvt, which Mutter's build runs to make its table of standard display modes
pkg_native_libxcvt() {
	unpack libxcvt
	native_env
	meson setup _build --prefix="$NATIVE" --libdir=lib --buildtype=release >meson.log 2>&1 || { tail -20 meson.log; exit 1; }
	ninja -C _build install >ninja.log 2>&1 || { tail -20 ninja.log; exit 1; }
}

pkg_mutter() {
	unpack mutter
	# its manual pages are always made, with docutils' rst2man
	"$L/venv/bin/pip" install -q docutils==0.21.2 >"$WORK/pip.log" 2>&1 || { tail -20 "$WORK/pip.log"; exit 1; }
	mesonbuild -Dopengl=true -Dgles2=true -Degl=true -Dxwayland=true -Dremote_desktop=true -Dlibgnome_desktop=true \
		-Dlibwacom=true -Dsound_player=true -Dstartup_notification=true -Dintrospection=true -Dprofiler=false \
		-Dtests=disabled -Dcogl_tests=false -Dclutter_tests=false -Dmutter_tests=false -Dinstalled_tests=false \
		-Dkvm_tests=false -Dtty_tests=false -Ddocs=false -Dudev_dir=/usr/lib/udev -Dxwayland_path=/usr/bin/Xwayland \
		-Dfonts=true -Dbash_completion=false -Ddevkit=enabled
}

ORDER+=(native_libxcvt mutter)

# ---- Calendars, contacts, weather and online accounts --------------------------------------
# Evolution Data Server keeps calendars and contacts; GNOME Shell shows the
# day's events from it. Mozilla's NSS does its certificates and S/MIME.

pkg_nspr() {
	unpack nspr
	cd nspr
	HOST_CC=gcc autobuild --enable-64bit --disable-debug --enable-optimize
	rm -f "$ROOT$LIBDIR"/libnspr4.a "$ROOT$LIBDIR"/libplc4.a "$ROOT$LIBDIR"/libplds4.a
}

pkg_nss() {
	unpack nss
	cd nss
	local args=(BUILD_OPT=1 USE_64=1 NSS_DISABLE_GTESTS=1 NSS_USE_SYSTEM_SQLITE=1 NSS_ENABLE_WERROR=0 USE_SYSTEM_ZLIB=1
		ZLIB_LIBS=-lz CC="$CC" CCC="$CXX" NATIVE_CC=gcc NATIVE_FLAGS= OS_ARCH=Linux OS_TEST=x86_64 CPU_ARCH=x86_64
		CROSS_COMPILE=1 NSPR_INCLUDE_DIR="$ROOT/usr/include/nspr" NSPR_LIB_DIR="$ROOT$LIBDIR" XCFLAGS="$CFLAGS"
		SOURCE_PREFIX="$PWD/../dist")
	make -C coreconf "${args[@]}" >make.log 2>&1 && make "${args[@]}" >>make.log 2>&1 ||
		{ grep -B5 -A10 -m5 'rror' make.log | head -60; exit 1; }
	local out
	out=$(echo ../dist/Linux*_OPT.OBJ)
	mkdir -p "$DEST$LIBDIR/pkgconfig" "$DEST/usr/include/nss" "$DEST/usr/bin"
	cp -L "$out"/lib/*.so "$DEST$LIBDIR/"
	cp -rL ../dist/public/nss/. "$DEST/usr/include/nss/"
	cp -L "$out"/bin/certutil "$out"/bin/pk12util "$out"/bin/modutil "$DEST/usr/bin/"
	local v
	v=$(sed -n 's/^#define NSS_VERSION "\([0-9.]*\).*/\1/p' lib/nss/nss.h)
	cat >"$DEST$LIBDIR/pkgconfig/nss.pc" <<-EOF
		prefix=/usr
		libdir=$LIBDIR
		includedir=/usr/include/nss
		Name: NSS
		Description: Mozilla's Network Security Services
		Version: $v
		Requires: nspr
		Libs: -L\${libdir} -lssl3 -lsmime3 -lnss3 -lnssutil3
		Cflags: -I\${includedir}
	EOF
	finish
}

pkg_libical() {
	unpack libical
	# libical-glib's sources are generated by a program of its own, which a
	# cross build takes from a build for this machine
	gcc -O2 -o "$WORK/ical-glib-src-generator" src/libical-glib/tools/generator.c src/libical-glib/tools/xml-parser.c \
		-DG_LOG_DOMAIN='"src-generator"' $(env -u PKG_CONFIG_LIBDIR -u PKG_CONFIG_SYSROOT_DIR /usr/bin/pkg-config \
		--cflags --libs glib-2.0 libxml-2.0) || { echo "couldn't build libical's generator" >&2; exit 1; }
	printf 'add_executable(native-ical-glib-src-generator IMPORTED)\nset_target_properties(native-ical-glib-src-generator PROPERTIES IMPORTED_LOCATION "%s")\n' \
		"$WORK/ical-glib-src-generator" >"$WORK/ical-generator.cmake"
	# the introspection scanner links a program to libical-glib, which needs libical beside it
	export LDFLAGS="$LDFLAGS -Wl,-rpath-link=$PWD/_build/lib"
	cmakebuild -DIMPORT_ICAL_GLIB_SRC_GENERATOR="$WORK/ical-generator.cmake" -DICAL_GLIB=ON -DGOBJECT_INTROSPECTION=ON -DICAL_GLIB_VAPI=OFF -DICAL_BUILD_DOCS=OFF -DSTATIC_ONLY=OFF \
		-DLIBICAL_BUILD_TESTING=OFF -DWITH_CXX_BINDINGS=OFF -DENABLE_GTK_DOC=OFF -DUSE_BUILTIN_TZDATA=OFF \
		-DGObjectIntrospection_SCANNER="$L/cross-bin/g-ir-scanner" -DGObjectIntrospection_COMPILER="$L/cross-bin/g-ir-compiler"
}

# libuuid (from util-linux), for Evolution Data Server
pkg_libuuid() {
	unpack util-linux
	autobuild --disable-all-programs --enable-libuuid --disable-nls --without-python --without-systemd --without-udev \
		--without-ncurses --without-tinfo --without-readline --disable-bash-completion --disable-liblastlog2 \
		--disable-pylibmount --disable-makeinstall-chown --disable-makeinstall-setuid
}

pkg_libpsl() { unpack libpsl; mesonbuild -Druntime=no -Dbuiltin=true -Dtests=false -Ddocs=false; }

pkg_libsoup() {
	unpack libsoup
	mesonbuild -Dintrospection=enabled -Dvapi=disabled -Dtests=false -Dautobahn=disabled -Dsysprof=disabled \
		-Dpkcs11_tests=disabled -Dtls_check=false -Dgssapi=disabled -Dntlm=disabled -Ddocs=disabled -Dbrotli=disabled
}

pkg_glib_networking() {
	unpack glib-networking
	mesonbuild -Dopenssl=enabled -Dgnutls=disabled -Dlibproxy=disabled -Dgnome_proxy=enabled -Dinstalled_tests=false
}

pkg_geocode_glib() {
	unpack geocode-glib
	mesonbuild -Denable-introspection=true -Denable-installed-tests=false -Denable-gtk-doc=false -Dsoup2=false
}

# the places the weather knows, as a database libgweather reads
pkg_gweather_locations() { unpack gweather-locations; mesonbuild; }

pkg_libgweather() {
	unpack libgweather
	# its introspection data (GNOME Shell's and Weather's way in) works across too
	sed -i "s/ and not meson.is_cross_build()//" meson.build
	mesonbuild -Denable_vala=false -Dintrospection=true -Dgtk_doc=false -Dtests=false
}

pkg_rest() { unpack rest; mesonbuild -Dintrospection=true -Dvapi=false -Dexamples=false -Dtests=false -Dgtk_doc=false; }

pkg_gnome_online_accounts() {
	unpack gnome-online-accounts
	mesonbuild -Dintrospection=true -Dvapi=false -Ddocumentation=false -Dman=false -Dkerberos=false -Dgoabackend=true
}

pkg_evolution_data_server() {
	unpack evolution-data-server
	# its configuration runs test programs (character set conversion, the
	# OAuth2 client names): the layer's way, through its dynamic linker, with
	# the layer's character set converters
	printf '#!/bin/sh\nGCONV_PATH="%s" exec "%s" "$@"\n' "$ROOT/usr/lib/gconv" "$L/cross-bin/run-target" >"$WORK/run-target"
	chmod +x "$WORK/run-target"
	# the introspection tools: the build's, not the layer's (which its pkg-config file names);
	# the scanner links against libraries that need EDS's other libraries beside them
	cat >"$WORK/g-ir-scanner" <<-EOF
		#!/bin/sh
		export LDFLAGS="\$LDFLAGS \$(find "$PWD/_build" -name '*.so*' -printf '-Wl,-rpath-link=%h\n' | sort -u | tr '\n' ' ')"
		exec "$L/cross-bin/g-ir-scanner" "\$@"
	EOF
	chmod +x "$WORK/g-ir-scanner"
	sed -i -e "s|pkg_check_variable(G_IR_SCANNER gobject-introspection-1.0 g_ir_scanner)|set(G_IR_SCANNER $WORK/g-ir-scanner)|" \
		-e "s|pkg_check_variable(G_IR_COMPILER gobject-introspection-1.0 g_ir_compiler)|set(G_IR_COMPILER $L/cross-bin/g-ir-compiler)|" \
		cmake/modules/GObjectIntrospection.cmake
	# its table generators run by target name, so they go through the same way
	sed -i 's|COMMAND ${CMAKE_CURRENT_BINARY_DIR}/gen-western-table|COMMAND gen-western-table|' src/addressbook/libebook-contacts/CMakeLists.txt
	sed -i 's|COMMAND ${CMAKE_CURRENT_BINARY_DIR}/camel-gen-tables|COMMAND camel-gen-tables|' src/camel/CMakeLists.txt
	cmakebuild -DCMAKE_CROSSCOMPILING_EMULATOR="$WORK/run-target" -DENABLE_GTK=OFF -DENABLE_GTK4=ON -DENABLE_OAUTH2_WEBKITGTK=OFF -DENABLE_OAUTH2_WEBKITGTK4=OFF \
		-DENABLE_EXAMPLES=OFF -DENABLE_GOA=ON -DENABLE_WEATHER=ON -DENABLE_CANBERRA=OFF -DENABLE_INTROSPECTION=ON \
		-DENABLE_VALA_BINDINGS=OFF -DWITH_PHONENUMBER=OFF -DENABLE_TESTS=OFF -DENABLE_GTK_DOC=OFF -DWITH_LIBDB=OFF \
		-DWITH_OPENLDAP=OFF -DWITH_KRB5=OFF -DWITH_SYSTEMDUSERUNITDIR=OFF -DENABLE_INSTALLED_TESTS=OFF \
		-DWITH_GWEATHER4=ON -DLIBEXEC_INSTALL_DIR=/usr/libexec
}

ORDER+=(icu libuuid nspr nss libical libpsl libsoup glib_networking geocode_glib native_pygobject gweather_locations libgweather rest gnome_online_accounts evolution_data_server)

# ---- NetworkManager's library ----------------------------------------------------------------
# GNOME Shell's network menu and Settings talk to NetworkManager's D-Bus
# interface through libnm. Zurbax answers on that interface itself (its
# network service), so only the library is used, not NetworkManager's daemon.

pkg_libndp() {
	unpack libndp
	./autogen.sh >autogen.log 2>&1
	autobuild
}

pkg_networkmanager() {
	unpack NetworkManager
	mesonbuild -Dsystemd_journal=false -Dsession_tracking=systemd -Dsuspend_resume=auto -Dlibaudit=no -Dselinux=false \
		-Dpolkit=false -Dmodem_manager=false -Dppp=false -Dnmcli=true -Dnmtui=false -Dbluez5_dun=false -Debpf=false \
		-Dconfig_dhcp_default=internal -Ddhcpcd=no -Dintrospection=true -Dvapi=false -Ddocs=false -Dtests=no \
		-Dqt=false -Dcrypto=null -Dlibpsl=true -Dnm_cloud_setup=false -Dovs=false -Dteamdctl=false -Dwext=false -Diwd=false \
		-Dofono=false -Dfirewalld_zone=false -Dreadline=none -Dsystemdsystemunitdir=no -Dudev_dir=/usr/lib/udev \
		-Dmore_asserts=no -Dconcheck=true
	# zsys answers for NetworkManager (with znet underneath): the library and
	# nmcli stay, the daemon goes
	rm -f "$ROOT"/usr/bin/NetworkManager "$ROOT"/usr/sbin/NetworkManager "$ROOT"/usr/libexec/nm-*
	rm -f "$ROOT"/usr/share/dbus-1/system-services/org.freedesktop.nm_* "$ROOT"/usr/share/dbus-1/system.d/org.freedesktop.NetworkManager.conf
}

ORDER+=(libndp networkmanager)

# ---- Client libraries of system services ------------------------------------------------------
# GNOME's programs reach the system's services through these libraries;
# on Zurbax zsys is the service behind each of them, so only the libraries
# (and their type information) stay in the layer.

pkg_accountsservice() {
	unpack accountsservice
	mesonbuild -Dintrospection=true -Dvapi=false -Ddocbook=false -Dgtk_doc=false -Dsystemdsystemunitdir=no \
		-Dadmin_group=wheel
	rm -f "$ROOT"/usr/libexec/accounts-daemon "$ROOT"/usr/share/dbus-1/system-services/org.freedesktop.Accounts.service
}

pkg_upower() {
	unpack upower
	mesonbuild -Dintrospection=enabled -Dgtk-doc=false -Dman=false -Didevice=disabled -Dsystemdsystemunitdir=no \
		-Dos_backend=linux -Dpolkit=disabled
	rm -f "$ROOT"/usr/libexec/upowerd "$ROOT"/usr/share/dbus-1/system-services/org.freedesktop.UPower.service
}

pkg_geoclue() {
	unpack geoclue
	mesonbuild -Denable-backend=false -Dlibgeoclue=true -Dintrospection=true -Dvapi=false -Dgtk-doc=false -Ddemo-agent=false
}

# PAM: GDM's library needs it to build; Zurbax checks passwords itself
pkg_linux_pam() {
	unpack linux-pam
	mesonbuild -Ddocs=disabled -Dexamples=false -Daudit=disabled -Dselinux=disabled -Dnis=disabled -Deconf=disabled \
		-Dlogind=disabled -Delogind=disabled -Dxtests=false -Di18n=disabled
}

# GDM: GNOME Shell's login and unlock code is written against its library
# (libgdm); zsys answers for the display manager itself
pkg_gdm() {
	unpack gdm
	mesonbuild -Dlogind-provider=systemd -Dsystemdsystemunitdir=no -Dsystemduserunitdir=no -Dx11-support=false \
		-Dplymouth=disabled -Dselinux=disabled -Dlibaudit=disabled -Dgdm-xsession=false -Ddefault-pam-config=lfs \
		-Dsystemd-journal=false -Dpam-mod-dir=/usr/lib/security
	rm -f "$ROOT"/usr/sbin/gdm "$ROOT"/usr/bin/gdm "$ROOT"/usr/libexec/gdm-* "$ROOT"/usr/bin/gdmflexiserver
	rm -rf "$ROOT"/usr/lib/security/pam_gdm.so "$ROOT"/etc/pam.d/gdm*
}

pkg_libnotify() { unpack libnotify; mesonbuild -Dtests=false -Dintrospection=enabled -Dman=false -Dgtk_doc=false -Ddocbook_docs=disabled; }

pkg_gsound() { unpack gsound; mesonbuild -Dintrospection=true -Denable_vala=false -Dgtk_doc=false; }

pkg_gnome_bluetooth() { unpack gnome-bluetooth; mesonbuild -Dintrospection=true -Dgtk_doc=false; }

pkg_libnma() {
	unpack libnma
	mesonbuild -Dlibnma_gtk4=true -Dintrospection=true -Dvapi=false -Dgtk_doc=false -Dmobile_broadband_provider_info=false \
		-Diso_codes=true -Dgcr=true
}

# IBus: input methods (Chinese, Japanese, Korean and more), which GNOME Shell drives
pkg_ibus() {
	unpack ibus
	autobuild --disable-gtk2 --disable-gtk3 --enable-gtk4 --disable-xim --enable-wayland --disable-appindicator \
		--disable-emoji-dict --disable-unicode-dict --disable-python2 --disable-python-library --disable-setup \
		--disable-ui --disable-tests --enable-introspection --disable-vala --disable-gtk-doc --disable-memconf \
		--enable-dconf --disable-systemd-services --with-python=python3
}

ORDER+=(accountsservice upower geoclue linux_pam gdm libnotify gsound gnome_bluetooth libnma ibus)

# ---- GNOME Shell -----------------------------------------------------------------------------

pkg_gnome_shell() {
	unpack gnome-shell
	mesonbuild -Dcamera_monitor=true -Dextensions_tool=true -Dgtk_doc=false -Dman=false -Dtests=false -Dnetworkmanager=true \
		-Dportal_helper=false -Dsystemd=false
	# Zurbax's session manager for GNOME (system/zsession)
	zurbax_program zsession /usr/libexec/zurbax/zsession
}

# Zurbax's touches for GNOME: its wallpapers (rendered by tools/wallpaper,
# all of them offered in GNOME's Appearance settings) and its defaults
# (layer/files/90_zurbax.gschema.override)
pkg_zurbax_gnome() {
	env -u CC -u CXX -u CFLAGS -u LDFLAGS make -C "$TOP" --no-print-directory -s build/tools/wallpaper ||
		{ echo "couldn't build the wallpaper tool" >&2; exit 1; }
	mkdir -p "$ROOT/usr/share/backgrounds/zurbax" "$ROOT/usr/share/gnome-background-properties"
	"$TOP/build/tools/wallpaper" --gnome "$ROOT/usr/share/backgrounds/zurbax" \
		"$ROOT/usr/share/gnome-background-properties/zurbax.xml" /usr/share/backgrounds/zurbax 3840 2160
	install -Dm644 "$HERE/files/90_zurbax.gschema.override" "$ROOT/usr/share/glib-2.0/schemas/90_zurbax.gschema.override"
	# the Zurbax Taskbar, GNOME Shell's side of the Zurbax desktop's layout
	install -Dm644 -t "$ROOT/usr/share/gnome-shell/extensions/taskbar@zurbax.org" "$HERE"/files/taskbar@zurbax.org/*
	# the logo GNOME's About page and os-release name
	install -Dm644 "$TOP/data/zurbax-logo.svg" "$ROOT/usr/share/pixmaps/zurbax-logo.svg"
	install -Dm644 "$TOP/data/zurbax-logo.svg" "$ROOT/usr/share/icons/hicolor/scalable/apps/zurbax-logo.svg"
	"$NATIVE/bin/glib-compile-schemas" "$ROOT/usr/share/glib-2.0/schemas"
}

ORDER+=(gnome_shell zurbax_gnome)

# ---- Zurbax's libsystemd ---------------------------------------------------------------
# The session questions GNOME asks systemd's library, answered by Zurbax's own
# (layer/sd-login): Mutter, GNOME Shell and the settings daemon link to it.

pkg_sd_login() {
	local src=$HERE/sd-login
	mkdir -p "$DEST$LIBDIR/pkgconfig" "$DEST/usr/include/systemd" "$DEST/usr/share/pkgconfig"
	$CC $CFLAGS -fPIC -fvisibility=hidden -shared -Wl,-soname,libsystemd.so.0 $LDFLAGS -o "$DEST$LIBDIR/libsystemd.so.0" \
		"$src/sd-login.c"
	ln -sf libsystemd.so.0 "$DEST$LIBDIR/libsystemd.so"
	install -m644 "$src"/sd-login.h "$src"/sd-journal.h "$src"/sd-daemon.h "$DEST/usr/include/systemd/"
	cat >"$DEST$LIBDIR/pkgconfig/libsystemd.pc" <<-EOF
		prefix=/usr
		libdir=$LIBDIR
		includedir=/usr/include
		Name: systemd
		Description: Zurbax's libsystemd: sessions, the journal and service notifications without systemd
		Version: 258
		Libs: -L\${libdir} -lsystemd
		Cflags: -I\${includedir}
	EOF
	# udev's own description: where its rules would go
	cat >"$DEST/usr/share/pkgconfig/udev.pc" <<-EOF
		prefix=/usr
		udevdir=/usr/lib/udev
		Name: udev
		Description: udev's file locations (Zurbax reads devices with libudev-zero)
		Version: 258
	EOF
	finish
}


