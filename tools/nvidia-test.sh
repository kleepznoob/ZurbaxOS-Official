#!/bin/sh
# NVIDIA's driver in a virtual machine with Zurbax's kernel (no NVIDIA GPU
# there): modprobe picks nouveau for a GeForce RTX 4060 without nvidia.sfs
# and NVIDIA's modules with it, nouveau for an older card either way, and
# the kernel takes NVIDIA's modules (signed, built against it): they load,
# find no GPU and step back.
#
#   tools/nvidia-test.sh        (after kernel/build.sh and make nvidia)
set -eu
cd "$(dirname "$0")/.."
KERNEL=build/kernel/arch/x86/boot/bzImage
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

make -s build/bin/modprobe build/lib/libzr.a
cc=$(make -n -B build/obj/system/modprobe/modprobe.o | grep ' -c system/modprobe/modprobe.c')
eval "$(echo "$cc" | sed "s| -MMD -MP||; s| -c system/modprobe/modprobe.c| -c tools/nvidia-test-init.c|; s| -o [^ ]*$| -o $T/init.o|")"
ld=$(make -n -B build/bin/modprobe | grep -- '-o build/bin/modprobe')
eval "$(echo "$ld" | sed "s|-o build/bin/modprobe build/obj/system/modprobe/modprobe.o|-o $T/init $T/init.o|")"

mkdir -p "$T/root"
cp build/bin/modprobe "$T/root/modprobe"
cp "$T/init" "$T/root/nvidia-test-init"
(cd "$T/root" && ls | cpio -o -H newc --quiet) >"$T/initrd"

timeout 600 qemu-system-x86_64 -machine q35 -m 2048 -smp 2 -accel tcg,thread=multi -cpu max \
	-kernel "$KERNEL" -initrd "$T/initrd" -append "rdinit=/nvidia-test-init console=ttyS0 loglevel=3" \
	-drive file=build/kernel/drivers.sfs,if=virtio,format=raw,readonly=on \
	-drive file=build/nvidia.sfs,if=virtio,format=raw,readonly=on \
	-display none -serial file:"$T/serial.log" -no-reboot -nic none
tr -d '\r' <"$T/serial.log" | grep -E '^(nvidia-test|kernel|extra|kernel/|[a-z_]+ [0-9]+ )' | grep -v '^kernel/.*(loaded)' || true

fail() { echo "FAIL: $*"; exit 1; }
log=$(tr -d '\r' <"$T/serial.log")
without=$(echo "$log" | sed -n '/without NVIDIA/,/nvidia.sfs/p')
with=$(echo "$log" | sed -n '/: with it/,/a Pascal/p')
pascal=$(echo "$log" | sed -n '/a Pascal/,/loading for/p')
echo "$without" | grep -q 'nouveau.ko' || fail "nouveau wasn't picked without nvidia.sfs"
echo "$without" | grep -q 'extra/nvidia' && fail "NVIDIA's driver was picked without nvidia.sfs"
echo "$with" | grep -q 'extra/nvidia/nvidia.ko' || fail "NVIDIA's driver wasn't picked with nvidia.sfs"
echo "$with" | grep -q 'nvidia-drm.ko' || fail "nvidia-drm wasn't loaded with it"
echo "$with" | grep -q 'nouveau.ko' && fail "nouveau was loaded alongside NVIDIA's driver"
echo "$pascal" | grep -q 'extra/nvidia' && fail "NVIDIA's driver was picked for a Pascal card"
echo "$log" | grep -q 'NVRM' || fail "the kernel didn't run NVIDIA's module"
echo "$log" | grep -qi 'rejected\|Required key' && fail "the kernel refused NVIDIA's module"
echo "$log" | grep -q '^nouveau ' || fail "nouveau didn't take over when NVIDIA's driver found no GPU"
echo "nvidia: all ok"
