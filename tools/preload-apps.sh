#!/bin/sh
# Builds the images of apps that come with Zurbax, ready to use without an
# internet connection: Flathub's apps and the runtimes they run on, as
# offline Flatpak repositories (flatpak create-usb), one squashfs image per
# group so each stays under the 4 GiB a FAT32 file may have.
#
#     tools/preload-apps.sh OUTDIR        OUTDIR/apps.sfs, OUTDIR/apps2.sfs
#
# Needs flatpak and squashfs-tools on the build machine. On the installed
# system, layerrun installs them into the person's Flatpak installation
# from these images the first time the Linux session starts.
set -eu
OUT=$1
# everyday apps and gaming launchers (GNOME and freedesktop runtimes)
GROUP1="org.mozilla.firefox org.gimp.GIMP com.heroicgameslauncher.hgl net.lutris.Lutris com.usebottles.bottles
com.vysp3r.ProtonPlus io.github.celluloid_player.Celluloid org.gnome.Boxes"
# office, video and streaming, emulation (KDE and freedesktop runtimes)
GROUP2="org.libreoffice.LibreOffice org.videolan.VLC com.obsproject.Studio org.libretro.RetroArch org.kde.kdenlive"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
export FLATPAK_USER_DIR="$WORK/install"
flatpak --user remote-add --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
mkdir -p "$OUT"
n=1
for group in "$GROUP1" "$GROUP2"; do
	name=apps; [ "$n" = 1 ] || name=apps$n
	# shellcheck disable=SC2086
	flatpak --user install -y --noninteractive flathub $group
	rm -rf "$WORK/usb" && mkdir -p "$WORK/usb"
	# shellcheck disable=SC2086
	flatpak --user create-usb "$WORK/usb" $group
	printf '%s\n' $group >"$WORK/usb/apps.list"
	rm -f "$OUT/$name.sfs"
	mksquashfs "$WORK/usb" "$OUT/$name.sfs" -comp zstd -Xcompression-level 15 -b 1M -all-root -noappend -quiet
	ls -l "$OUT/$name.sfs"
	n=$((n + 1))
done
