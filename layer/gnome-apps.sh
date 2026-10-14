# GNOME's session pieces and apps, built into the layer after GNOME Shell
# (gnome.sh): the settings daemon, Settings, Files, Console, Text Editor,
# the portals Flatpak apps talk to, the keyring and GVfs, then the core
# apps. Sourced by build.sh; the helpers (unpack, mesonbuild, autobuild,
# cmakebuild, rust_env, zurbax_program...) are build.sh's and gnome.sh's.

# ---- Build tools -----------------------------------------------------------------------------

# PyGObject in the build's Python (without cairo, which nothing here draws
# with): blueprint-compiler (which turns GNOME apps' interface descriptions
# into GTK's) and the weather locations' database maker run on it
pkg_native_pygobject() {
	native_env
	"$L/venv/bin/pip" install -q meson-python==0.18.0 >"$WORK/pip.log" 2>&1 &&
		PKG_CONFIG_PATH=$NATIVE/lib/pkgconfig "$L/venv/bin/pip" install -q --no-deps --no-build-isolation --no-binary pygobject \
		-Csetup-args=-Dpycairo=disabled pygobject==3.58.1 >>"$WORK/pip.log" 2>&1 || { tail -30 "$WORK/pip.log"; exit 1; }
	"$L/venv/bin/python3" -c 'from gi.repository import GLib' || { echo "PyGObject doesn't load" >&2; exit 1; }
}

pkg_native_blueprint() {
	unpack blueprint-compiler
	native_env
	"$L/venv/bin/meson" setup _build --prefix="$NATIVE" -Ddocs=false >meson.log 2>&1 || { tail -20 meson.log; exit 1; }
	ninja -C _build install >ninja.log 2>&1 || { tail -20 ninja.log; exit 1; }
	# it runs on the build machine's Python and PyGObject (its own GLib type information), reading
	# the layer's for GTK, libadwaita and the rest; meson put its module beside the native tools
	local py
	py=$(command -v python3.12 || command -v python3)
	{
		echo "#!$py"
		echo "import os, sys"
		echo "sys.path.insert(0, '$NATIVE/lib/python3/dist-packages')"
		echo "os.environ['GI_TYPELIB_PATH'] = ':'.join(p for p in ('/usr/lib/x86_64-linux-gnu/girepository-1.0', '$ROOT/usr/lib/girepository-1.0', os.environ.get('GI_TYPELIB_PATH', '')) if p)"
		sed 1d "$NATIVE/bin/blueprint-compiler" | grep -v '^import sys; sys.path.insert'
	} >"$NATIVE/bin/blueprint-compiler.new"
	chmod +x "$NATIVE/bin/blueprint-compiler.new"
	mv "$NATIVE/bin/blueprint-compiler.new" "$NATIVE/bin/blueprint-compiler"
}

# Vala: several GNOME apps are written in it (it compiles to C)
pkg_native_vala() {
	unpack vala
	native_env
	PKG_CONFIG_PATH=$NATIVE/lib/pkgconfig ./configure --prefix="$NATIVE" --disable-valadoc --disable-doc >configure.log 2>&1 ||
		{ tail -20 configure.log; exit 1; }
	make -j"$JOBS" >make.log 2>&1 && make install >install.log 2>&1 || { tail -20 make.log; exit 1; }
}

ORDER+=(native_blueprint native_vala)

# ---- Libraries ---------------------------------------------------------------------------------

pkg_libunistring() { unpack libunistring; autobuild; }
pkg_libtasn1() { unpack libtasn1; autobuild --disable-doc; }
pkg_nettle() { unpack nettle; autobuild --disable-documentation --enable-mini-gmp; }

pkg_gnutls() {
	unpack gnutls
	autobuild --disable-doc --disable-tests --disable-tools --without-tpm --without-tpm2 --with-included-unistring=no \
		--disable-guile --without-idn --with-default-trust-store-file=/etc/ssl/certs/ca-certificates.crt --enable-openssl-compatibility=no
}

pkg_cracklib() {
	unpack cracklib
	# the dictionary of common passwords comes from the build machine's packer
	autobuild --without-python --disable-nls
}

pkg_libpwquality() { unpack libpwquality; autobuild --disable-python-bindings --disable-pam --disable-nls; }

pkg_libgtop() { unpack libgtop; autobuild --disable-gtk-doc --enable-introspection=yes --without-x; }

pkg_colord_gtk() { unpack colord-gtk; mesonbuild -Dgtk4=true -Dgtk3=false -Dintrospection=true -Dvapi=false -Ddocs=false -Dman=false -Dtests=false; }

# udisks's client library alone (zsys answers as udisks itself): made from
# its interface description with gdbus-codegen, as udisks's build does
pkg_udisks_client() {
	unpack udisks
	local inc=$DEST/usr/include/udisks2/udisks
	mkdir -p "$inc" "$DEST$LIBDIR/pkgconfig"
	cd udisks
	"$NATIVE/bin/gdbus-codegen" --interface-prefix org.freedesktop.UDisks2. --c-namespace UDisks --generate-c-code udisks-generated \
		--c-generate-object-manager --c-generate-autocleanup all ../data/org.freedesktop.UDisks2.xml
	sed -e 's/@MAJOR_VERSION@/2/; s/@MINOR_VERSION@/11/; s/@MICRO_VERSION@/1/' udisksversion.h.in >udisksversion.h
	printf '#define GETTEXT_PACKAGE "udisks2"\n' >../config.h
	"$NATIVE/bin/glib-mkenums" --template udisksenumtypes.h.template udisksenums.h >udisksenumtypes.h
	"$NATIVE/bin/glib-mkenums" --template udisksenumtypes.c.template udisksenums.h >udisksenumtypes.c
	local cflags
	cflags=$($PKG_CONFIG --cflags gio-2.0 gio-unix-2.0)
	$CC $CFLAGS -fPIC -shared -Wl,-soname,libudisks2.so.0 $LDFLAGS -I. -I.. -DUDISKS_COMPILATION -D_GNU_SOURCE $cflags \
		-o "$DEST$LIBDIR/libudisks2.so.0" udisks-generated.c udisksenumtypes.c udisksclient.c udiskserror.c udisksobjectinfo.c \
		$($PKG_CONFIG --libs gio-2.0 gio-unix-2.0) || exit 1
	ln -sf libudisks2.so.0 "$DEST$LIBDIR/libudisks2.so"
	install -m644 udisks.h udisksclient.h udiskserror.h udisksenums.h udisksenumtypes.h udisks-generated.h udisksobjectinfo.h \
		udisksversion.h udiskstypes.h "$inc/"
	cat >"$DEST$LIBDIR/pkgconfig/udisks2.pc" <<-EOF
		prefix=/usr
		libdir=$LIBDIR
		includedir=/usr/include/udisks2
		Name: UDisks
		Description: udisks's client library (zsys is the daemon on Zurbax)
		Version: 2.11.1
		Requires: gio-2.0
		Libs: -L\${libdir} -ludisks2
		Cflags: -I\${includedir}
	EOF
	finish
}

# ModemManager: its library for Settings; the daemon stays for mobile broadband
pkg_modemmanager() {
	unpack ModemManager
	mesonbuild -Dudev=true -Dudevdir=/usr/lib/udev -Dsystemdsystemunitdir=no -Dsystemd_suspend_resume=false -Dsystemd_journal=false \
		-Dpolkit=no -Dmbim=false -Dqmi=false -Dqrtr=false -Dintrospection=true -Dvapi=false -Dgtk_doc=false -Dman=false \
		-Dbash_completion=false -Dtests=false -Dexamples=false -Dpowerd_suspend_resume=false
}

# gcr 3: the keyring still speaks its older interface
# gcr 3 asks for the paths of ssh-add and ssh-agent even with its SSH agent
# off (they're only written into that agent), and makes Vala bindings
# whenever it makes introspection data
pkg_gcr3() {
	unpack gcr3
	sed -i -e "s|^ssh_add_path = find_program('ssh-add'.*|ssh_add_path = '/usr/bin/ssh-add'|" \
		-e "s|^ssh_agent_path = find_program('ssh-agent'.*|ssh_agent_path = '/usr/bin/ssh-agent'|" meson.build
	# and no Vala bindings: vapigen would look for GLib's in the build machine's folders
	sed -i '/gck_vapi = gnome.generate_vapi/,/^  )/d' gck/meson.build
	sed -i '/gcr_vapi = gnome.generate_vapi/,/^  )/d' gcr/meson.build
	mesonbuild -Dgtk=false -Dintrospection=true -Dgtk_doc=false -Dssh_agent=false -Dsystemd=disabled
}

# ICU, the Unicode library: first for the build machine (its data tools), then for the layer
pkg_icu() {
	unpack icu
	cd source
	mkdir -p native layer
	(cd native && env -u CC -u CXX -u CFLAGS -u CXXFLAGS -u LDFLAGS ../configure --disable-tests --disable-samples \
		>configure.log 2>&1 && make -j"$JOBS" >make.log 2>&1) || { tail -20 native/configure.log native/make.log; exit 1; }
	cd layer
	../configure --build=$BUILD --host=$HOST --prefix=/usr --libdir=$LIBDIR --with-cross-build="$PWD/../native" \
		--disable-tests --disable-samples --disable-static >configure.log 2>&1 || { tail -30 configure.log; exit 1; }
	make -j"$JOBS" >make.log 2>&1 || { grep -B5 -A5 -m5 'error' make.log | head -40; exit 1; }
	# ICU's install puts its tools' libraries before $LD_LIBRARY_PATH; left
	# empty, that adds the current folder, and the layer's library folder
	# (with the layer's C library) once the install changes into it
	LD_LIBRARY_PATH=$NATIVE/lib make DESTDIR="$DEST" install >install.log 2>&1 || { tail -20 install.log; exit 1; }
	finish
}

pkg_tinysparql() {
	unpack tinysparql
	mesonbuild -Dintrospection=enabled -Dvapi=disabled -Ddocs=false -Dman=false -Dtests=false -Dsystemd_user_services=false \
		-Dbash_completion=false -Davahi=disabled -Dstemmer=disabled
}

pkg_localsearch() {
	unpack localsearch
	mesonbuild -Dman=false -Dfunctional_tests=false -Dsystemd_user_services=false -Dlandlock=enabled -Dseccomp=true \
		-Dbattery_detection=upower -Dcharset_detection=icu -Dlibav=enabled -Dlibmediainfo=disabled -Dxml=enabled \
		-Dexif=disabled -Dgif=disabled -Diptc=disabled -Djpeg=enabled -Dpng=enabled -Dtiff=disabled -Dpdf=disabled \
		-Dxps=disabled -Draw=disabled -Dicon=true -Dplaylist=disabled -Dabiword=false -Dunzip_ps_gz_files=false \
		-Dwriteback=false
}

pkg_gnome_autoar() { unpack gnome-autoar; mesonbuild -Dintrospection=enabled -Dvapi=false -Dgtk_doc=false -Dtests=false; }

pkg_libportal() {
	unpack libportal
	mesonbuild -Dbackend-gtk4=enabled -Dbackend-gtk3=disabled -Dbackend-qt5=disabled -Dbackend-qt6=disabled \
		-Dintrospection=true -Dvapi=false -Ddocs=false -Dtests=false
}

pkg_gtksourceview() { unpack gtksourceview; mesonbuild -Dintrospection=enabled -Dvapi=false -Ddocumentation=false -Dbuild-testsuite=false -Dinstall-tests=false -Dsysprof=false; }

# LZ4, which the terminal keeps its scrollback with
pkg_lz4() {
	unpack lz4
	local mk=(CC="$CC" CFLAGS="$CFLAGS" PREFIX=/usr LIBDIR="$LIBDIR" BUILD_STATIC=no)
	make -C lib -j"$JOBS" "${mk[@]}" >make.log 2>&1 && make -C lib "${mk[@]}" DESTDIR="$DEST" install >install.log 2>&1 ||
		{ tail -20 make.log install.log; exit 1; }
	finish
}

pkg_vte() {
	unpack vte
	# the libraries VTE builds in, from their releases, with VTE's build files for them
	local sp
	for sp in fast_float fmt simdutf; do
		mkdir -p "subprojects/$sp"
		tar -xf "$(fetch "vte-$sp")" -C "subprojects/$sp" --strip-components=1
		cp -r "subprojects/packagefiles/$sp/." "subprojects/$sp/"
	done
	# VTE refuses to build with assertions off
	mesonbuild -Db_ndebug=false -Dgtk3=false -Dgtk4=true -Dgir=true -Dvapi=false -Ddocs=false -D_systemd=false -Dgnutls=true -Dicu=true -Dglade=false
}

ORDER+=(libunistring libtasn1 gmp nettle gnutls cracklib libpwquality libgtop colord_gtk udisks_client modemmanager gcr3 tinysparql
	localsearch gnome_autoar libportal gtksourceview lz4 vte)

# ---- The session's services ----------------------------------------------------------------------

pkg_xdg_desktop_portal() {
	unpack xdg-desktop-portal
	mesonbuild -Dsystemd=disabled -Ddocumentation=disabled -Dtests=disabled -Dgeoclue=enabled -Dflatpak-interfaces=enabled \
		-Dsandboxed-image-validation=disabled -Dsandboxed-sound-validation=disabled -Dinstalled-tests=false
}

pkg_xdg_desktop_portal_gnome() { unpack xdg-desktop-portal-gnome; mesonbuild -Dsystemduserunitdir=no; }

pkg_gnome_keyring() {
	unpack gnome-keyring
	mesonbuild -Dsystemd=disabled -Dpam=false -Dmanpage=false -Dssh-agent=false -Dselinux=disabled
}

pkg_gvfs() {
	unpack gvfs
	mesonbuild -Dsystemduserunitdir=no -Dtmpfilesdir=no -Dadmin=false -Dafc=false -Dafp=false -Darchive=true -Dcdda=false \
		-Ddnssd=false -Dgoa=true -Dgoogle=false -Dgphoto2=false -Dhttp=true -Dmtp=false -Dnfs=false -Dsftp=true -Dsmb=false \
		-Dudisks2=true -Dbluray=false -Dfuse=true -Dgcr=true -Dgcrypt=true -Dgudev=true -Dkeyring=true -Dlogind=false \
		-Dlibusb=true -Dman=false -Donedrive=false -Dwsdd=false
}

pkg_gnome_settings_daemon() {
	unpack gnome-settings-daemon
	mesonbuild -Dsystemd=true -Dalsa=true -Dgudev=true -Dcups=false -Dnetwork_manager=true -Drfkill=true -Dsmartcard=false \
		-Dusb-protection=false -Dwwan=false -Dgcr3=false -Dcolord=true -Dxwayland=false
}

ORDER+=(xdg_desktop_portal xdg_desktop_portal_gnome gnome_keyring gvfs gnome_settings_daemon tecla)

# ---- Apps ------------------------------------------------------------------------------------------

# Tecla: the keyboard layout viewer Settings shows
pkg_tecla() { unpack tecla; mesonbuild; }

# MIT Kerberos, for Settings' enterprise login (its configure can't run test
# programs in a cross build, so it's told what they'd find; its older C needs
# the standard before GCC 15's default)
pkg_krb5() {
	unpack krb5
	cd src
	CFLAGS="${CFLAGS:-} -std=gnu17" krb5_cv_attr_constructor_destructor=yes,yes ac_cv_func_regcomp=yes ac_cv_printf_positional=yes \
		autobuild --without-system-verto --disable-rpath --without-keyutils --without-ldap --without-tcl
}

# Apps' manuals are left out: building them needs itstool, and reading them yelp.
nohelp() { sed -i "/subdir('help')/d" meson.build; }

pkg_gnome_control_center() {
	polkit_its
	unpack gnome-control-center
	# no Printers panel: Zurbax has no print service for it to manage
	sed -i "/^  'printers',$/d" panels/meson.build
	sed -i "/^assert(cups_dep.found()/d; /CUPS headers not found/d" meson.build
	sed -i '/cc_printers_panel_get_type/d' shell/cc-panel-loader.c
	XDG_DATA_DIRS=$ROOT/usr/share mesonbuild -Dibus=true -Dmalcontent=false -Dsnap=false -Dlocation-services=enabled -Dtests=false -Ddocumentation=false \
		-Ddistributor_logo=/usr/share/pixmaps/zurbax-logo.svg -Ddark_mode_distributor_logo=/usr/share/pixmaps/zurbax-logo.svg
}

pkg_nautilus() {
	unpack nautilus
	mesonbuild -Dextensions=false -Dselinux=disabled -Dcloudproviders=disabled -Dtests=none -Ddocs=false -Dintrospection=true
}

pkg_gnome_console() { unpack gnome-console; mesonbuild -Dtests=false; }

# Spell checking (Text Editor's, and any app's through libspelling): Hunspell
# with English (US) words, behind Enchant
pkg_hunspell() { unpack hunspell; autoreconf -fi >autoreconf.log 2>&1; autobuild --disable-nls --without-ui --without-readline; }
pkg_enchant() {
	unpack enchant
	# its manuals as HTML need groff's HTML device, which build machines often lack
	sed -i 's/^nodist_doc_DATA = .*/nodist_doc_DATA =/' lib/Makefile.in src/Makefile.in
	autobuild --with-hunspell --without-aspell --without-nuspell --without-voikko --without-hspell --without-applespell --disable-relocatable; }

pkg_hunspell_en_us() {
	local zip
	zip=$(fetch hunspell-en-us)
	mkdir -p "$ROOT/usr/share/hunspell"
	python3 -c 'import sys, zipfile; z = zipfile.ZipFile(sys.argv[1]); [z.extract(n, sys.argv[2]) for n in ("en_US.aff", "en_US.dic")]' \
		"$zip" "$ROOT/usr/share/hunspell"
}

pkg_libspelling() { unpack libspelling; mesonbuild -Denchant=enabled -Dintrospection=enabled -Dvapi=false -Ddocs=false -Dsysprof=false; }

pkg_gnome_text_editor() { unpack gnome-text-editor; nohelp; mesonbuild -Ddevelopment=false -Deditorconfig=disabled; }

ORDER+=(krb5 gnome_control_center nautilus gnome_console hunspell enchant hunspell_en_us libspelling gnome_text_editor)

# ---- More apps -------------------------------------------------------------------------------------
# Weather, Calendar, Characters, Fonts, Disk Usage, Archive Manager, System
# Monitor, Image Viewer, Document Viewer, Maps, the welcome tour and Software
# (Flathub's apps). Zurbax's own Calculator and Clock stand in for GNOME's.

# GNU's arbitrary precision arithmetic, which Nettle (under GnuTLS) computes with;
# fat: the fastest code for whichever processor it runs on
# GMP 6.3's configure tests are older C than GCC 15 assumes
pkg_gmp() { unpack gmp; CFLAGS="${CFLAGS:-} -std=gnu17" autobuild --enable-fat; }

# The Vala apps' view of the libraries they use, made from the libraries'
# introspection data (those were built before Vala was)
vapi() {
	local name=$1 gir=$2 meta=$3 dir=$ROOT/usr/share/vala/vapi
	shift 3
	vapigen --library "$name" --girdir "$ROOT/usr/share/gir-1.0" --vapidir "$dir" ${meta:+--metadatadir "$meta"} \
		$(printf -- '--pkg %s ' "$@") -d "$dir" "$ROOT/usr/share/gir-1.0/$gir.gir" >>"$WORK/vapigen.log" 2>&1 ||
		{ tail -20 "$WORK/vapigen.log"; exit 1; }
	printf '%s\n' "$@" >"$dir/$name.deps"
}

pkg_vala_bindings() {
	native_env
	mkdir -p "$ROOT/usr/share/vala/vapi"
	: >"$WORK/vapigen.log"
	vapi gsettings-desktop-schemas GDesktopEnums-3.0 "" gobject-2.0
	# libadwaita's adjustments for Vala come with its source
	unpack libadwaita
	vapi libadwaita-1 Adw-1 "$PWD/src" gtk4 gio-2.0
	vapi gnome-desktop-4 GnomeDesktop-4.0 "" gtk4 gio-2.0 gsettings-desktop-schemas
	vapi gweather4 GWeather-4.0 "" gio-2.0
	vapi geocode-glib-2.0 GeocodeGlib-2.0 "" gio-2.0 json-glib-1.0
	vapi libgeoclue-2.0 Geoclue-2.0 "" gio-2.0
	vapi gsound GSound-1.0 "" gio-2.0
}

# Vala apps: the build machine's compiler, which finds the bindings above
vala_env() {
	printf "[binaries]\nvala = '%s'\n" "$NATIVE/bin/valac" >"$WORK/vala.ini"
	export XDG_DATA_DIRS=$ROOT/usr/share:$NATIVE/share
	VALA=(--cross-file "$WORK/vala.ini")
}

pkg_baobab() { unpack baobab; nohelp; vala_env; mesonbuild "${VALA[@]}"; }

pkg_gnome_weather() {
	unpack gnome-weather
	# newer TypeScript compilers want these spelled out
	sed -i 's|"compilerOptions": {|"compilerOptions": {\n        "rootDir": "src",\n        "ignoreDeprecations": "6.0",|' tsconfig.json
	mesonbuild
}
pkg_gnome_calendar() { unpack gnome-calendar; mesonbuild; }
pkg_gnome_characters() { unpack gnome-characters; mesonbuild -Dinstalled_tests=false; }
pkg_gnome_font_viewer() { unpack gnome-font-viewer; mesonbuild; }
pkg_file_roller() { unpack file-roller; nohelp; mesonbuild -Dpackagekit=false -Dnautilus-actions=disabled -Dintrospection=disabled; }

ORDER+=(vala_bindings baobab gnome_weather gnome_calendar gnome_characters
	gnome_font_viewer file_roller)

# System Monitor is written in C++ with GTK's C++ interface
pkg_libsigcpp() { unpack libsigcpp; mesonbuild -Dbuild-examples=false -Dbuild-tests=false -Dbuild-documentation=false; }
pkg_glibmm() { unpack glibmm; mesonbuild -Dbuild-examples=false -Dbuild-documentation=false; }
pkg_cairomm() { unpack cairomm; mesonbuild -Dbuild-examples=false -Dbuild-tests=false -Dbuild-documentation=false; }
pkg_pangomm() { unpack pangomm; mesonbuild -Dbuild-documentation=false; }
pkg_gtkmm() { unpack gtkmm; mesonbuild -Dbuild-demos=false -Dbuild-tests=false -Dbuild-documentation=false; }
pkg_gnome_system_monitor() { unpack gnome-system-monitor; nohelp; mesonbuild -Dsystemd=false; }

ORDER+=(libsigcpp glibmm cairomm pangomm gtkmm gnome_system_monitor)

# Rust apps meson builds: cargo compiles them for the layer (its target
# folder then has them under the target's name), and a wrapper copies the
# programs to where the apps' build files look for them
rust_app_env() {
	rust_env
	cargo_fetch
	mkdir -p _build "$WORK/cargo-bin"
	ln -sfn "$CARGO_HOME" _build/cargo-home
	cat >"$WORK/cargo-bin/cargo" <<-'EOS'
		#!/bin/bash
		"$HOME/.cargo/bin/cargo" "$@" || exit
		[ "$1" = build ] || exit 0
		dir=target profile=debug
		while [ $# -gt 0 ]; do
			case $1 in
			--target-dir) dir=$2; shift ;;
			--target-dir=*) dir=${1#*=} ;;
			--release) profile=release ;;
			--profile) profile=$2; shift ;;
			--profile=*) profile=${1#*=} ;;
			esac
			shift
		done
		[ "$profile" = dev ] && profile=debug
		mkdir -p "$dir/$profile"
		find "$dir/x86_64-unknown-linux-gnu/$profile" -maxdepth 1 -type f -perm -u+x -exec cp -p {} "$dir/$profile/" \;
	EOS
	chmod +x "$WORK/cargo-bin/cargo"
	export PATH="$WORK/cargo-bin:$PATH"
}

pkg_loupe() { unpack loupe; nohelp; rust_app_env; mesonbuild; }

# Document Viewer: PDF through Poppler, and the XMP metadata library it reads
pkg_exempi() { unpack exempi; autobuild --enable-unittest=no --enable-samples=no; }

pkg_poppler() {
	unpack poppler
	cmakebuild -DENABLE_QT5=OFF -DENABLE_QT6=OFF -DENABLE_BOOST=OFF -DENABLE_LIBOPENJPEG=OFF -DENABLE_GPGME=OFF \
		-DENABLE_NSS3=ON -DENABLE_LCMS=ON -DENABLE_BROTLI=OFF -DENABLE_LIBCURL=ON -DENABLE_GLIB=ON -DENABLE_GOBJECT_INTROSPECTION=OFF \
		-DENABLE_CPP=ON -DENABLE_UTILS=ON -DBUILD_GTK_TESTS=OFF -DBUILD_QT5_TESTS=OFF -DBUILD_QT6_TESTS=OFF \
		-DBUILD_CPP_TESTS=OFF -DBUILD_MANUAL_TESTS=OFF -DENABLE_GTK_DOC=OFF -DTESTDATADIR=/nonexistent
}

pkg_papers() {
	unpack papers
	# appstreamcli only checks its metadata, which the build can do without
	sed -i "s|find_program('appstreamcli', required: true)|find_program('appstreamcli', required: false)|" meson.build
	rust_app_env
	mesonbuild -Dtests=false -Ddocumentation=false -Duser_doc=false -Dfile_tests=false -Dsysprof=disabled \
		-Dspell_check=disabled -Ddjvu=disabled -Dcomics=enabled -Dtiff=enabled -Dkeyring=enabled -Dintrospection=disabled
}

ORDER+=(loupe exempi poppler papers)

# libshumate's map tiles come as protocol buffers; only the C library is needed
pkg_protobuf_c() { unpack protobuf-c; autobuild --disable-protoc; }

pkg_libshumate() {
	unpack libshumate
	mesonbuild -Dgir=true -Dvapi=false -Dgtk_doc=false -Ddemos=false -Dvector_renderer=false
}

pkg_gnome_maps() {
	unpack gnome-maps
	nohelp
	# gjs runs it in the layer; the build machine has none
	sed -i "/^subdir('tests')/d" meson.build
	sed -i "s|^gjs = find_program('gjs')|gjs = find_program('gjs', required: false)|" meson.build
	sed -i "s|script_conf.set('GJS', gjs.full_path())|script_conf.set('GJS', '/usr/bin/gjs')|" src/meson.build
	mesonbuild
}
pkg_gnome_tour() { unpack gnome-tour; rust_app_env; mesonbuild; }

# Software, for the apps on Flathub (Flatpak); no distribution packages
pkg_gnome_software() {
	unpack gnome-software
	nohelp
	mesonbuild -Dpackagekit=false -Dfwupd=false -Dmalcontent=false -Dwebapps=false -Dhardcoded_foss_webapps=false \
		-Dhardcoded_proprietary_webapps=false -Dtests=false -Dman=false -Dgtk_doc=false -Dsysprof=disabled -Dpolkit=true \
		-Dgudev=true -Dflatpak=true -Dexternal_appstream=false
}

ORDER+=(protobuf_c libshumate gnome_maps gnome_tour gnome_software)
