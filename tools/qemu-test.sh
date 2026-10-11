#!/bin/bash
# Boots build/zurbax.img (or IMAGE, e.g. build/zurbax-mini.iso) under UEFI firmware with no window and saves
# screenshots of the boot, for checking the splash, setup and desktop.
#
#   tools/qemu-test.sh OUTDIR [SECONDS...]     screenshots at those times
#
# Extra QEMU arguments can be passed in QEMU_EXTRA. KERNEL_APPEND boots
# build/kernel's bzImage directly with those extra kernel arguments, e.g.
# KERNEL_APPEND="console=ttyS0 loglevel=7". The serial console is logged to
# OUTDIR/serial.log.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$1
shift
[ $# -gt 0 ] || set -- 5 10 20 30
mkdir -p "$OUT"
cp "${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}" "$OUT/vars.fd"
MON=$OUT/monitor.sock
rm -f "$MON"
accel=tcg
[ -e /dev/kvm ] && accel=kvm
direct=()
[ -n "${KERNEL_APPEND:-}" ] && direct=(-kernel "$ROOT/build/kernel/arch/x86/boot/bzImage" -append "$KERNEL_APPEND")
# shellcheck disable=SC2086
qemu-system-x86_64 -machine q35,accel=$accel -cpu max -m 2G -smp 2 \
	-drive if=pflash,format=raw,readonly=on,file="${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}" \
	-drive if=pflash,format=raw,file="$OUT/vars.fd" \
	-drive file="${IMAGE:-$ROOT/build/zurbax.img}",format=raw,if=virtio,snapshot=on \
	-device "${QEMU_VGA:-virtio-vga}" -device qemu-xhci -device usb-tablet -device usb-kbd \
	-display none -serial file:"$OUT/serial.log" -monitor unix:"$MON",server,nowait \
	${QEMU_EXTRA:-} "${direct[@]}" &
pid=$!
trap 'kill $pid 2>/dev/null || true' EXIT INT TERM
start=$(date +%s)
for t in "$@"; do
	while [ $(($(date +%s) - start)) -lt "$t" ]; do sleep 0.5; done
	printf 'screendump %s/shot-%03d.ppm\n' "$OUT" "$t" | python3 -c '
import socket, sys
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); s.recv(4096)
s.sendall(sys.stdin.read().encode()); import time; time.sleep(1); s.close()' "$MON"
	if command -v convert >/dev/null && [ -f "$OUT/shot-$(printf %03d "$t").ppm" ]; then
		convert "$OUT/shot-$(printf %03d "$t").ppm" "$OUT/shot-$(printf %03d "$t").png" && rm -f "$OUT/shot-$(printf %03d "$t").ppm"
	fi
	echo "  shot at ${t}s"
done
