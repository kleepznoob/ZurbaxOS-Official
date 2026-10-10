#!/bin/sh
# Runs a GNOME session on the built Linux layer without a screen, the way
# Zurbax starts one (zsys as root, then layerrun --desktop gnome as the
# person), and takes a screenshot of it.
#
#     tools/gnome-test.sh [OUT.png] [WIDTHxHEIGHT]
#
# Needs root (zsys runs as root, the layer is bound to /data/layers/steam)
# and user namespaces. The session runs as uid 1000 in a throwaway home;
# its logs are left in that folder.
set -eu
TOP=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$TOP/build/gnome-test.png}
SIZE=${2:-1920x1080}
LAYER=$TOP/build/layer/root
WORK=$(mktemp -d /tmp/gnome-test.XXXXXX)
export PATH="$TOP/build/bin:$PATH"

[ -x "$LAYER/usr/bin/gnome-shell" ] || { echo "no GNOME Shell in $LAYER (layer/build.sh)" >&2; exit 1; }
make -C "$TOP" --no-print-directory -s build/bin/zsys build/bin/zsession build/bin/layerrun build/bin/polkit-helper
# the session manager and helper as the layer build installs them
install -Dm755 "$TOP/build/bin/zsession" "$LAYER/usr/libexec/zurbax/zsession"
install -Dm755 "$TOP/build/bin/polkit-helper" "$LAYER/usr/lib/polkit-1/polkit-agent-helper-1"
install -Dm644 "$TOP/layer/files/system-bus.conf" "$LAYER/usr/share/zurbax/system-bus.conf"

mkdir -p /data/layers/steam
mountpoint -q /data/layers/steam || mount --bind "$LAYER" /data/layers/steam

# the account: uid 1000, no password
mkdir -p "$WORK/data/system" "$WORK/home"
printf 'user = tester\nfull_name = Zurbax Tester\nuid = 1000\ngid = 1000\nsetup_done = true\n' >"$WORK/data/system/users.conf"
printf 'timezone = Europe/Bucharest\nhostname = zurbax\n' >"$WORK/data/system/system.conf"
chown -R 1000:1000 "$WORK/home"
chmod 755 "$WORK"
mkdir -p /run/user/1000 /run/zurbax
chown 1000:1000 /run/user/1000
chmod 700 /run/user/1000

cleanup() {
	[ -n "${SESSION:-}" ] && kill "$SESSION" 2>/dev/null || true
	[ -n "${SYS:-}" ] && kill "$SYS" 2>/dev/null || true
	sleep 1
	setpriv --reuid=1000 --regid=1000 --clear-groups env HOME="$WORK/home" XDG_RUNTIME_DIR=/run/user/1000 layerrun --stop 2>/dev/null || true
}
trap cleanup EXIT

ZURBAX_DATA=$WORK/data ZURBAX_HOME=$WORK/home "$TOP/build/bin/zsys" >"$WORK/zsys.log" 2>&1 &
SYS=$!
for i in $(seq 50); do [ -S /run/dbus/system_bus_socket ] && break; sleep 0.2; done

setpriv --reuid=1000 --regid=1000 --clear-groups env -i PATH="$PATH" HOME="$WORK/home" USER=tester LANG=en_US.UTF-8 \
	XDG_RUNTIME_DIR=/run/user/1000 ZURBAX_GNOME_HEADLESS="$SIZE" layerrun --desktop gnome >"$WORK/session.log" 2>&1 &
SESSION=$!

# GNOME Shell is up once it answers on the session bus
shot() {
	setpriv --reuid=1000 --regid=1000 --clear-groups env -i PATH="$PATH" HOME="$WORK/home" USER=tester \
		XDG_RUNTIME_DIR=/run/user/1000 layerrun gdbus call --session --dest org.gnome.Shell.Screenshot \
		--object-path /org/gnome/Shell/Screenshot --method org.gnome.Shell.Screenshot.Screenshot false false "$WORK/shot.png"
}
for i in $(seq 90); do
	sleep 2
	kill -0 "$SESSION" 2>/dev/null || { echo "the session ended:" >&2; tail -40 "$WORK/session.log" >&2; exit 1; }
	if shot >"$WORK/shot.log" 2>&1 && [ -s "$WORK/shot.png" ]; then
		sleep 6   # the startup animation, the settings daemons
		shot >"$WORK/shot.log" 2>&1
		cp "$WORK/shot.png" "$OUT"
		echo "screenshot: $OUT (logs in $WORK)"
		exit 0
	fi
done
echo "GNOME Shell didn't answer; logs in $WORK" >&2
tail -40 "$WORK/session.log" >&2
exit 1
