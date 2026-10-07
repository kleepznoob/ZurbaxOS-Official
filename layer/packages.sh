# Packages after the toolchain base; sourced by build.sh.

# ---- Sound: ALSA's library, playing through ZOS's sound service --------------------------

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
		cat >"$ROOT/etc/asound.conf" <<'CONF'
# ZOS: sound goes to zaudio, ZOS's sound service, which owns the sound card
pcm.zaudio { type zaudio }
pcm.!default { type plug slave.pcm "zaudio" }
CONF
	fi
}

ORDER+=(alsa_lib alsa_lib:32 zaudio_alsa zaudio_alsa:32)
