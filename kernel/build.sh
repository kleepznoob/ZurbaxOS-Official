#!/bin/sh
# Builds the Zurbax kernel: upstream Linux LTS + kernel/zurbax.config.
#
#   kernel/build.sh                 build bzImage with no embedded system
#   kernel/build.sh system.cpio     embed system.cpio -> one bootable EFI file
#
# Output: build/kernel/arch/x86/boot/bzImage
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

mkdir -p "$OUT"
# Reconfigure whenever the configuration files' contents change.
sig=$(cat "$ROOT/kernel/zurbax.config" ${ZURBAX_KEXTRA:+"$ZURBAX_KEXTRA"} | cksum)
if [ ! -f "$OUT/.config" ] || [ "$(cat "$OUT/.zurbax-config-sig" 2>/dev/null)" != "$sig" ]; then
	echo ">> configuring"
	make -s -C "$SRC" O="$OUT" x86_64_defconfig
	"$SRC/scripts/kconfig/merge_config.sh" -m -O "$OUT" "$OUT/.config" "$ROOT/kernel/zurbax.config" ${ZURBAX_KEXTRA:+"$ZURBAX_KEXTRA"} >"$OUT/merge.log"
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

# a graphics edition's driver settings (kernel/gpu-variants) after the usual ones
base=$(sed -n 's/^CONFIG_CMDLINE="\(.*\)"$/\1/p' "$ROOT/kernel/zurbax.config")
want="$base${ZURBAX_CMDLINE_EXTRA:+ $ZURBAX_CMDLINE_EXTRA}"
if [ "$(sed -n 's/^CONFIG_CMDLINE="\(.*\)"$/\1/p' "$OUT/.config")" != "$want" ]; then
	"$SRC/scripts/config" --file "$OUT/.config" --set-str CMDLINE "$want"
	make -s -C "$SRC" O="$OUT" olddefconfig
fi

echo ">> building with $JOBS jobs"
make -s -C "$SRC" O="$OUT" -j"$JOBS" bzImage
ls -l "$OUT/arch/x86/boot/bzImage"
