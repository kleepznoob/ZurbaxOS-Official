# GNOME's session pieces and apps, built into the layer after GNOME Shell
# (gnome.sh): the settings daemon, Settings, Files, Console, Text Editor,
# the portals Flatpak apps talk to, the keyring and GVfs, then the core
# apps. Sourced by build.sh; the helpers (unpack, mesonbuild, autobuild,
# cmakebuild, rust_env, zurbax_program...) are build.sh's and gnome.sh's.

# ---- Build tools -----------------------------------------------------------------------------

# PyGObject in the build's Python: blueprint-compiler (which turns GNOME apps'
# interface descriptions into GTK's) runs on it
pkg_native_pygobject() {
	native_env
	PKG_CONFIG_PATH=$NATIVE/lib/pkgconfig "$L/venv/bin/pip" install -q --no-binary pygobject pycairo==1.28.0 pygobject==3.58.1 \
		>"$WORK/pip.log" 2>&1 || { tail -30 "$WORK/pip.log"; exit 1; }
}

pkg_native_blueprint() {
	unpack blueprint-compiler
	native_env
	"$L/venv/bin/meson" setup _build --prefix="$NATIVE" -Ddocs=false >meson.log 2>&1 || { tail -20 meson.log; exit 1; }
	ninja -C _build install >ninja.log 2>&1 || { tail -20 ninja.log; exit 1; }
	# it runs on the build's Python, with PyGObject reading the build machine's GTK type information
	sed -i "1s|.*|#!$L/venv/bin/python3|" "$NATIVE/bin/blueprint-compiler"
}

# Vala: several GNOME apps are written in it (it compiles to C)
pkg_native_vala() {
	unpack vala
	native_env
	PKG_CONFIG_PATH=$NATIVE/lib/pkgconfig ./configure --prefix="$NATIVE" --disable-valadoc --disable-doc >configure.log 2>&1 ||
		{ tail -20 configure.log; exit 1; }
	make -j"$JOBS" >make.log 2>&1 && make install >install.log 2>&1 || { tail -20 make.log; exit 1; }
}

ORDER+=(native_pygobject native_blueprint native_vala)

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
	sed -e 's/@UD_MAJOR_VERSION@/2/; s/@UD_MINOR_VERSION@/11/; s/@UD_MICRO_VERSION@/1/' udisksversion.h.in >udisksversion.h
	"$NATIVE/bin/glib-mkenums" --template udisksenumtypes.h.template udisksenums.h >udisksenumtypes.h
	"$NATIVE/bin/glib-mkenums" --template udisksenumtypes.c.template udisksenums.h >udisksenumtypes.c
	local cflags
	cflags=$($PKG_CONFIG --cflags gio-2.0 gio-unix-2.0)
	$CC $CFLAGS -fPIC -shared -Wl,-soname,libudisks2.so.0 $LDFLAGS -I. -I.. -DUDISKS_COMPILATION -D_GNU_SOURCE $cflags \
		-o "$DEST$LIBDIR/libudisks2.so.0" udisks-generated.c udisksenumtypes.c udisksclient.c udiskserror.c udisksobjectinfo.c \
		udisksversion.c $($PKG_CONFIG --libs gio-2.0 gio-unix-2.0) || exit 1
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
pkg_gcr3() { unpack gcr3; mesonbuild -Dgtk=false -Dintrospection=true -Dvapi=false -Dgtk_doc=false -Dssh_agent=false -Dsystemd=disabled; }

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
	make DESTDIR="$DEST" install >install.log 2>&1 || { tail -20 install.log; exit 1; }
	finish
}

pkg_tinysparql() {
	unpack tinysparql
	mesonbuild -Dintrospection=enabled -Dvapi=disabled -Ddocs=false -Dman=false -Dtests=false -Dsystemd_user_services=false \
		-Dbash_completion=false -Dsoup=soup3 -Davahi=disabled -Dstemmer=disabled
}

pkg_localsearch() {
	unpack localsearch
	mesonbuild -Dman=false -Dtests=false -Dsystemd_user_services=false -Dminer_rss=false -Dlandlock=enabled -Dseccomp=true \
		-Dbattery_detection=upower -Dcharset_detection=icu -Dgeneric_media_extractor=gstreamer -Dxml=libxml2 \
		-Dexif=disabled -Dgif=disabled -Diptc=disabled -Djpeg=enabled -Dpng=enabled -Dtiff=enabled -Dpdf=disabled \
		-Dxps=disabled -Draw=disabled -Dicon=enabled -Dplaylist=disabled -Dabiword=false -Dhtml=disabled -Dunzip_ps_gz_files=false \
		-Dwriteback=false
}

pkg_gnome_autoar() { unpack gnome-autoar; mesonbuild -Dintrospection=enabled -Dvapi=false -Dgtk_doc=false -Dtests=false; }

pkg_libportal() {
	unpack libportal
	mesonbuild -Dbackend-gtk4=enabled -Dbackend-gtk3=disabled -Dbackend-qt5=disabled -Dbackend-qt6=disabled \
		-Dintrospection=true -Dvapi=false -Ddocs=false -Dtests=false
}

pkg_gtksourceview() { unpack gtksourceview; mesonbuild -Dintrospection=enabled -Dvapi=false -Dgtk_doc=false -Dinstall_tests=false -Dsysprof=false; }

pkg_vte() {
	unpack vte
	mesonbuild -Dgtk3=false -Dgtk4=true -Dgir=true -Dvapi=false -Ddocs=false -D_systemd=false -Dgnutls=true -Dicu=true -Dglade=false \
		-Dsixel=true
}

ORDER+=(libunistring libtasn1 nettle gnutls cracklib libpwquality libgtop colord_gtk udisks_client modemmanager gcr3 icu tinysparql
	localsearch gnome_autoar libportal gtksourceview vte)

# ---- The session's services ----------------------------------------------------------------------

pkg_xdg_desktop_portal() {
	unpack xdg-desktop-portal
	mesonbuild -Dsystemd=disabled -Ddocumentation=disabled -Dtests=disabled -Dgeoclue=enabled -Dflatpak-interfaces=enabled \
		-Dlibportal=enabled -Dsandboxed-image-validation=disabled -Dsandboxed-sound-validation=disabled -Dinstalled-tests=false
}

pkg_xdg_desktop_portal_gnome() { unpack xdg-desktop-portal-gnome; mesonbuild -Dsystemduserunitdir=no; }

pkg_gnome_keyring() {
	unpack gnome-keyring
	mesonbuild -Dsystemd=disabled -Dpam=false -Dmanpage=false -Dssh-agent=true -Dselinux=disabled
}

pkg_gvfs() {
	unpack gvfs
	mesonbuild -Dsystemduserunitdir=no -Dtmpfilesdir=no -Dadmin=false -Dafc=false -Dafp=false -Darchive=true -Dcdda=false \
		-Ddnssd=false -Dgoa=true -Dgoogle=false -Dgphoto2=false -Dhttp=true -Dmtp=false -Dnfs=false -Dsftp=true -Dsmb=false \
		-Dudisks2=true -Dbluray=false -Dfuse=true -Dgcr=true -Dgcrypt=true -Dgudev=true -Dkeyring=true -Dlogind=false \
		-Dlibusb=true -Dman=false -Donedrive=false -Dwsdd=false -Ddeprecated_apis=false
}

pkg_gnome_settings_daemon() {
	unpack gnome-settings-daemon
	mesonbuild -Dsystemd=true -Dalsa=true -Dgudev=true -Dcups=false -Dnetwork_manager=true -Drfkill=true -Dsmartcard=false \
		-Dusb-protection=false -Dwwan=false -Dgcr3=false -Dcolord=true -Dxwayland=false
}

ORDER+=(xdg_desktop_portal xdg_desktop_portal_gnome gnome_keyring gvfs gnome_settings_daemon)

# ---- Apps ------------------------------------------------------------------------------------------

pkg_gnome_control_center() {
	unpack gnome-control-center
	mesonbuild -Dibus=true -Dmalcontent=false -Dsnap=false -Dlocation-services=enabled -Dtests=false -Ddocumentation=false \
		-Ddistributor_logo=/usr/share/pixmaps/zurbax-logo.svg -Ddark_mode_distributor_logo=/usr/share/pixmaps/zurbax-logo.svg
}

pkg_nautilus() {
	unpack nautilus
	mesonbuild -Dextensions=false -Dselinux=disabled -Dcloudproviders=disabled -Dtests=none -Ddocs=false -Dintrospection=true
}

pkg_gnome_console() { unpack gnome-console; mesonbuild -Dtests=false; }

pkg_gnome_text_editor() { unpack gnome-text-editor; mesonbuild -Ddevelopment=false -Dspellcheck=disabled; }

ORDER+=(gnome_control_center nautilus gnome_console gnome_text_editor)
