#!/bin/sh
# winusb end to end, in a virtual machine with Zurbax's kernel: a
# Windows-shaped ISO (UDF, like Microsoft's) goes onto a virtual 8 GB USB
# stick that was formatted and mounted before. Then, on this side: the
# partition table, fsck, every file against the ISO's, and the machine's
# other disk (not USB, refused) unchanged.
#
#   tools/winusb-test.sh        (after make kernel; needs qemu, pycdlib,
#                                mtools and dosfstools)
set -eu
cd "$(dirname "$0")/.."
KERNEL=build/kernel/arch/x86/boot/bzImage
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

make -s build/bin/winusb build/lib/libzr.a
# the virtual machine's first program, built like any other
cc=$(make -n -B build/obj/system/winusb/winusb.o | grep ' -c system/winusb/winusb.c')
eval "$(echo "$cc" | sed "s| -MMD -MP||; s| -c system/winusb/winusb.c| -c tools/winusb-test-init.c|; s| -o [^ ]*$| -o $T/init.o|")"
ld=$(make -n -B build/bin/winusb | grep -- '-o build/bin/winusb')
eval "$(echo "$ld" | sed "s|-o build/bin/winusb build/obj/system/winusb/winusb.o|-o $T/init $T/init.o|")"

mkdir -p "$T/tree" "$T/root"
# Zurbax's kernel stands in for Windows' boot program: both are UEFI programs
python3 -I - "$T/tree" "$T/root/win.iso" build/zurbax.efi <<'PY'
import io, os, sys, pycdlib
tree, out, efi = sys.argv[1:]
files = {
    'bootmgr': b'b' * 400000, 'bootmgr.efi': b'e' * 1500000, 'setup.exe': b'MZ' + os.urandom(80000),
    'autorun.inf': b'[AutoRun.Amd64]\r\nopen=setup.exe\r\n', 'efi/boot/bootx64.efi': open(efi, 'rb').read(),
    'efi/microsoft/boot/fonts/chs_boot.ttf': os.urandom(3000000), 'sources/background_cli.bmp': os.urandom(120000),
    'sources/Lang.ini': b'[Available UI Languages]\r\nen-US = 3\r\n', 'boot/BCD': os.urandom(16384),
    'sources/empty.txt': b'', 'sources/install.wim': os.urandom(1 << 20) * 40 + os.urandom(12345),
}
iso = pycdlib.PyCdlib()
iso.new(udf='2.60', interchange_level=4)
dirs = sorted({os.path.dirname(p) for p in files for _ in [0]} | {'/'.join(p.split('/')[:i]) for p in files for i in range(1, p.count('/') + 1)})
for i, d in enumerate(x for x in dirs if x):
    os.makedirs(f'{tree}/{d}', exist_ok=True)
    iso.add_directory(f'/D{i}', udf_path='/' + d)
for i, (p, data) in enumerate(files.items()):
    open(f'{tree}/{p}', 'wb').write(data)
    iso.add_fp(io.BytesIO(data), len(data), f'/F{i}.;1', udf_path='/' + p)
iso.write(out)
PY
cp build/bin/winusb "$T/root/winusb"
cp "$T/init" "$T/root/winusb-test-init"
(cd "$T/root" && ls | cpio -o -H newc --quiet) >"$T/initrd"

# the stick, as it came: an old FAT partition with a file on it
truncate -s 8G "$T/stick.img"
python3 -I - "$T/stick.img" <<'PY'
import struct, sys
f = open(sys.argv[1], 'r+b')
mbr = bytearray(512)
mbr[446:462] = bytes([0, 0, 0, 0, 0x0c, 0, 0, 0]) + struct.pack('<II', 2048, 4 * 2048 * 1024)
mbr[510:] = b'\x55\xaa'
f.write(mbr)
PY
mkfs.vfat --offset 2048 -n OLDSTUFF "$T/stick.img" $((4 * 1024 * 1024)) >/dev/null 2>&1
echo hi >"$T/old.txt"; mcopy -i "$T/stick.img@@1M" "$T/old.txt" ::old.txt
# the machine's own disk
head -c 64M /dev/urandom >"$T/disk.img"; cp "$T/disk.img" "$T/disk.orig"

timeout 900 qemu-system-x86_64 -machine q35 -m 2048 -smp 2 -accel tcg,thread=multi -cpu max \
	-kernel "$KERNEL" -initrd "$T/initrd" -append "rdinit=/winusb-test-init console=ttyS0 loglevel=3" \
	-drive file="$T/disk.img",if=virtio,format=raw \
	-device qemu-xhci -drive if=none,id=stick,format=raw,file="$T/stick.img" -device usb-storage,drive=stick,removable=on \
	-display none -serial file:"$T/serial.log" -no-reboot -nic none
tr -d '\r' <"$T/serial.log" | grep -E '^(winusb|  |Opening|Everything|Erasing|USB|Done)' | sed 's/.*\(Copying Windows: 100%\)/  \1/' | grep -v 'Copying Windows: *[0-9]\{1,2\}%' || true

fail() { echo "FAIL: $*"; exit 1; }
grep -q 'winusb exited with 256' "$T/serial.log" || fail "winusb didn't refuse the virtio disk"
grep -q 'winusb exited with 0' "$T/serial.log" || fail "winusb didn't finish"
cmp -s "$T/disk.img" "$T/disk.orig" || fail "the machine's own disk changed"
python3 -I - "$T/stick.img" <<'PY'
import struct, sys
mbr = open(sys.argv[1], 'rb').read(512)
assert mbr[510:] == b'\x55\xaa', 'no MBR signature'
st, ty, start, size = mbr[446], mbr[450], *struct.unpack('<II', mbr[454:462])
assert (st, ty, start) == (0x80, 0x0c, 2048), (st, ty, start)
assert size % 2048 == 0 and size > 0 and mbr[462:510] == bytes(48)
print(f'partition: FAT32, active, from 1 MiB, {size // 2048} MiB')
PY
size=$(python3 -I -c "import struct,sys; print(struct.unpack('<I', open(sys.argv[1],'rb').read(512)[458:462])[0])" "$T/stick.img")
dd if="$T/stick.img" of="$T/part.img" bs=1M skip=1 count=$((size / 2048)) conv=sparse status=none
fsck.fat -n "$T/part.img" | tail -1
mlabel -s -i "$T/part.img" ::
mkdir "$T/out"
mcopy -s -n -i "$T/part.img" '::*' "$T/out/"
diff -r "$T/tree" "$T/out" || fail "the stick's files differ from the ISO's"
echo "files: all $(find "$T/out" -type f | wc -l) identical to the ISO's"

# a PC's firmware boots it: OVMF finds the stick and starts its BOOTX64.EFI
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$T/vars.fd"
timeout 120 qemu-system-x86_64 -machine q35 -m 1024 -accel tcg,thread=multi -cpu max \
	-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd -drive if=pflash,format=raw,file="$T/vars.fd" \
	-device qemu-xhci -drive if=none,id=stick,format=raw,file="$T/stick.img" -device usb-storage,drive=stick,removable=on \
	-display none -serial file:"$T/boot.log" -nic none || true
tr -d '\r' <"$T/boot.log" | grep -o 'BdsDxe: starting .*USB.*' | head -1 | grep . || fail "the firmware didn't boot the stick"
echo "winusb: all ok"
