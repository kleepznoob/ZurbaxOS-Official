#!/bin/sh
# Builds the images of apps that come with Zurbax, ready to use without an
# internet connection: Flathub's apps and the runtimes they run on, as
# offline Flatpak repositories (flatpak create-usb), one squashfs image per
# group. Each stays under the 4 GiB a file may have on the install stick's
# FAT32.
#
#     tools/preload-apps.sh OUTDIR        OUTDIR/apps.sfs, apps2.sfs ...
#
# Needs flatpak and squashfs-tools on the build machine. On the installed
# system, layerrun installs them into the person's Flatpak installation
# from these images the first time the Linux session starts.
set -eu
OUT=$1
LIMIT=4000000000
# one line per image; a group shares its runtimes
# (Lutris, Bottles, OBS, RetroArch and the rest are a click away in Harbor)
GROUPS="org.mozilla.firefox io.github.celluloid_player.Celluloid org.gnome.Boxes
org.gimp.GIMP com.heroicgameslauncher.hgl com.vysp3r.ProtonPlus
org.libreoffice.LibreOffice org.videolan.VLC"
WORK=$(mktemp -d "${TMPDIR:-/var/tmp}/zurbax-apps.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$OUT"
n=1
printf '%s\n' "$GROUPS" | while read -r group; do
	name=apps; [ "$n" = 1 ] || name=apps$n
	if [ -s "$OUT/$name.sfs" ] && [ "$(cat "$OUT/$name.list" 2>/dev/null)" = "$group" ]; then
		echo ">> $name.sfs is up to date"; n=$((n + 1)); continue
	fi
	echo ">> $name: $group"
	# a fresh installation per group, so the disk only ever holds one
	rm -rf "$WORK/install" "$WORK/usb" && mkdir -p "$WORK/usb"
	export FLATPAK_USER_DIR="$WORK/install"
	flatpak --user remote-add --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
	# offline repositories need the remote's collection ID (Flathub's)
	flatpak --user remote-modify --collection-id=org.flathub.Stable flathub
	# shellcheck disable=SC2086
	flatpak --user install -y --noninteractive flathub $group
	# shellcheck disable=SC2086
	flatpak --user create-usb "$WORK/usb" $group
	printf '%s\n' $group >"$WORK/usb/apps.list"
	rm -rf "$WORK/install"
	rm -f "$OUT/$name.sfs"
	mksquashfs "$WORK/usb" "$OUT/$name.sfs" -comp zstd -Xcompression-level 15 -b 1M -all-root -noappend -quiet
	rm -rf "$WORK/usb"
	size=$(stat -c %s "$OUT/$name.sfs")
	if [ "$size" -ge "$LIMIT" ]; then
		echo "$name.sfs is $size bytes, too big for FAT32: split its group" >&2
		exit 1
	fi
	printf '%s\n' "$group" >"$OUT/$name.list"
	ls -l "$OUT/$name.sfs"
	n=$((n + 1))
done
