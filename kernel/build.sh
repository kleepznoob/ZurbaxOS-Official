#!/bin/sh
# Builds Ember, the Zurbax kernel: upstream Linux LTS with kernel/patches,
# kernel/modules.config and kernel/zurbax.config.
#
#   kernel/build.sh                 build bzImage with no embedded system
#   kernel/build.sh system.cpio     embed system.cpio -> one bootable EFI file
#
# Output: build/kernel/arch/x86/boot/bzImage, and build/kernel/drivers.sfs:
# every driver not built in, as signed modules (ZURBAX_MODULES=0 skips them).
set -eu

KVER=6.18.55
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=${ZURBAX_KSRC:-$ROOT/build/linux-$KVER}
# ZURBAX_KOUT and ZURBAX_KEXTRA (a config fragment) build test kernels elsewhere.
OUT=${ZURBAX_KOUT:-$ROOT/build/kernel}
JOBS=${JOBS:-$(nproc)}

if [ ! -f "$SRC/Makefile" ]; then
	mkdir -p "$(dirname "$SRC")"
	echo ">> fetching linux-$KVER"
	curl -fsSL "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$KVER.tar.xz" |
		tar -xJ -C "$(dirname "$SRC")"
fi

# Ember: Zurbax's own changes to the kernel (kernel/patches), each applied once
for p in "$ROOT"/kernel/patches/*.patch; do
	[ -e "$p" ] || continue
	patch -d "$SRC" -p1 -R -f -s --dry-run <"$p" >/dev/null 2>&1 && continue # already in
	echo ">> applying $(basename "$p")"
	patch -d "$SRC" -p1 -f -s <"$p"
done

mkdir -p "$OUT"
# Reconfigure whenever the configuration files' contents change.
sig=$(cat "$ROOT/kernel/modules.config" "$ROOT/kernel/zurbax.config" ${ZURBAX_KEXTRA:+"$ZURBAX_KEXTRA"} | cksum)
if [ ! -f "$OUT/.config" ] || [ "$(cat "$OUT/.zurbax-config-sig" 2>/dev/null)" != "$sig" ]; then
	echo ">> configuring"
	make -s -C "$SRC" O="$OUT" x86_64_defconfig
	# (from the build folder, where the merge keeps its temporary files)
	extra=${ZURBAX_KEXTRA:+$(cd "$(dirname "$ZURBAX_KEXTRA")" && pwd)/$(basename "$ZURBAX_KEXTRA")}
	(cd "$OUT" && "$SRC/scripts/kconfig/merge_config.sh" -m -O "$OUT" "$OUT/.config" "$ROOT/kernel/modules.config" \
		"$ROOT/kernel/zurbax.config" ${extra:+"$extra"} >"$OUT/merge.log")
	make -s -C "$SRC" O="$OUT" olddefconfig
	# Report any requested option that Kconfig refused (missing dependency, renamed symbol).
	grep -E '^(CONFIG_[A-Z0-9_]+=|# CONFIG_[A-Z0-9_]+ is not set)' "$ROOT/kernel/zurbax.config" |
	while read -r line; do
		case "$line" in
		"# "*) sym=${line#\# }; sym=${sym%% *}; want=n ;;
		*) sym=${line%%=*}; want=${line#*=} ;;
		esac
		have=$(grep -E "^$sym=" "$OUT/.config" | cut -d= -f2- || true)
		[ -z "$have" ] && have=n
		[ "$want" = n ] && [ "$have" = n ] && continue
		[ "$have" = "$want" ] || echo "   note: $sym wanted $want, got $have"
	done
	echo "$sig" >"$OUT/.zurbax-config-sig"
fi

if [ $# -ge 1 ]; then
	cpio=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
	"$SRC/scripts/config" --file "$OUT/.config" --set-str INITRAMFS_SOURCE "$cpio" \
		--enable INITRAMFS_COMPRESSION_NONE --disable INITRAMFS_COMPRESSION_GZIP
	make -s -C "$SRC" O="$OUT" olddefconfig
fi

# the command line from zurbax.config, with any ZURBAX_CMDLINE_EXTRA after it
base=$(sed -n 's/^CONFIG_CMDLINE="\(.*\)"$/\1/p' "$ROOT/kernel/zurbax.config")
want="$base${ZURBAX_CMDLINE_EXTRA:+ $ZURBAX_CMDLINE_EXTRA}"
if [ "$(sed -n 's/^CONFIG_CMDLINE="\(.*\)"$/\1/p' "$OUT/.config")" != "$want" ]; then
	"$SRC/scripts/config" --file "$OUT/.config" --set-str CMDLINE "$want"
	make -s -C "$SRC" O="$OUT" olddefconfig
fi

echo ">> building with $JOBS jobs"
make -s -C "$SRC" O="$OUT" -j"$JOBS" bzImage
ls -l "$OUT/arch/x86/boot/bzImage"

# The other drivers: modules, signed as they're installed, in one compressed
# image that Zurbax mounts as /data/layers/drivers. Test kernels go without.
if [ "${ZURBAX_MODULES:-1}" != 0 ] && [ -z "${ZURBAX_KOUT:-}" ]; then
	echo ">> building the drivers"
	make -s -C "$SRC" O="$OUT" -j"$JOBS" modules
	# and NVIDIA's (kernel/nvidia.sh; ZURBAX_NVIDIA=0 goes without)
	nv=
	if [ "${ZURBAX_NVIDIA:-1}" != 0 ]; then
		"$ROOT/kernel/nvidia.sh" modules
		nv=$ROOT/build/nvidia/src/kernel-open
	fi
	if [ ! -f "$OUT/drivers.sfs" ] || [ -n "$(find "$OUT" $nv -name '*.ko' -newer "$OUT/drivers.sfs" -print -quit)" ]; then
		rel=$(cat "$OUT/include/config/kernel.release")
		rm -rf "$OUT/modroot"
		make -s -C "$SRC" O="$OUT" INSTALL_MOD_PATH="$OUT/modroot" INSTALL_MOD_STRIP=1 modules_install
		if [ -n "$nv" ]; then
			"$ROOT/kernel/nvidia.sh" install "$OUT/modroot"
			depmod -b "$OUT/modroot" "$rel"
		fi
		rm -f "$OUT/modroot/lib/modules/$rel/build" "$OUT/modroot/lib/modules/$rel/source"
		rm -f "$OUT/drivers.sfs.part"
		mksquashfs "$OUT/modroot" "$OUT/drivers.sfs.part" -comp zstd -Xcompression-level 19 -b 1M -all-root -noappend -quiet
		mv "$OUT/drivers.sfs.part" "$OUT/drivers.sfs"
		rm -rf "$OUT/modroot"
		echo "   $(find "$OUT" -name '*.ko' | wc -l) drivers"
	fi
	ls -l "$OUT/drivers.sfs"
fi
