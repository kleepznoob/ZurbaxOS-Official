#!/usr/bin/env python3
"""Fetches the firmware ZOS's Wi-Fi and GPU drivers load, for the system image.

    tools/fetch-firmware.py OUTDIR

Wi-Fi chips and GPUs run vendor firmware that the kernel hands them at
start-up; it comes from the linux-firmware project (redistributable,
unmodified). This fetches the files for the Wi-Fi chips common in laptops
from the last few years and for the AMD and Intel GPUs in
kernel/gpu-firmware.txt, compresses them with xz (the kernel unpacks them as
it loads them, so they take less memory), and adds the wireless regulatory
database. Files already fetched are kept; delete OUTDIR to start over.
"""
import concurrent.futures, lzma, os, re, sys, urllib.request

FIRMWARE = "https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/"
REGDB = "https://git.kernel.org/pub/scm/linux/kernel/git/wens/wireless-regdb.git/plain/"

# What the kernel's built-in drivers ask for (modules.builtin.modinfo, Linux
# 6.18), trimmed to chips found in laptops: Intel AX200/AX201/AX210/AX211/
# BE200/BE201 and 9000-series, MediaTek MT7921/MT7922/MT7925, Realtek
# RTL8852BE/CE and RTL8922AE. Intel names give the newest interface version
# the driver speaks; an older file with the same prefix works too.
WANTED = """
iwlwifi-cc-a0-77.ucode
iwlwifi-Qu-b0-hr-b0-100.ucode iwlwifi-Qu-c0-hr-b0-100.ucode iwlwifi-QuZ-a0-hr-b0-100.ucode
iwlwifi-Qu-b0-jf-b0-77.ucode iwlwifi-Qu-c0-jf-b0-77.ucode iwlwifi-QuZ-a0-jf-b0-77.ucode
iwlwifi-so-a0-hr-b0-100.ucode iwlwifi-so-a0-jf-b0-77.ucode
iwlwifi-so-a0-gf-a0-100.ucode iwlwifi-so-a0-gf-a0.pnvm
iwlwifi-ty-a0-gf-a0-100.ucode iwlwifi-ty-a0-gf-a0.pnvm
iwlwifi-ma-b0-gf-a0-100.ucode iwlwifi-ma-b0-gf-a0.pnvm
iwlwifi-gl-c0-fm-c0-c99.ucode
iwlwifi-9000-pu-b0-jf-b0-46.ucode iwlwifi-9260-th-b0-jf-b0-46.ucode
mediatek/WIFI_MT7922_patch_mcu_1_1_hdr.bin mediatek/WIFI_RAM_CODE_MT7922_1.bin
mediatek/WIFI_MT7961_patch_mcu_1_2_hdr.bin mediatek/WIFI_RAM_CODE_MT7961_1.bin
mediatek/mt7925/WIFI_MT7925_PATCH_MCU_1_1_hdr.bin mediatek/mt7925/WIFI_RAM_CODE_MT7925_1_1.bin
rtw89/rtw8852b_fw-1.bin rtw89/rtw8852c_fw-2.bin rtw89/rtw8922a_fw-4.bin
""".split()


def get(url):
    req = urllib.request.Request(url, headers={"User-Agent": "zurbax-build/1.0 (fetch-firmware)"})
    with urllib.request.urlopen(req, timeout=120) as r:
        return r.read()


def index():
    """Every file in linux-firmware by the name drivers ask for (links included)."""
    files = {}
    for line in get(FIRMWARE + "WHENCE").decode("utf-8", "replace").splitlines():
        m = re.match(r"^(?:RawFile|File):\s*(\S+)", line)
        if m:
            path = m.group(1).strip('"')
            files.setdefault(path, path)
            files.setdefault(os.path.basename(path), path)
            continue
        m = re.match(r"^Link:\s*(\S+)\s*->\s*(\S+)", line)
        if m:
            name, target = m.group(1), m.group(2)
            base = os.path.dirname(name)
            files[name] = os.path.normpath(os.path.join(base, target))
    return files


def resolve(name, files):
    """The file to use for a name, allowing an older interface version for Intel."""
    if name in files:
        return files[name]
    m = re.match(r"^(iwlwifi-.*-)(c?)(\d+)\.ucode$", name)
    if not m:
        return None
    prefix, c, top = m.group(1), m.group(2), int(m.group(3))

    def newest(c, top):
        best = None
        for f in files:
            k = re.match(re.escape(prefix) + c + r"(\d+)\.ucode$", os.path.basename(f))
            if k and int(k.group(1)) <= top and (best is None or int(k.group(1)) > best[0]):
                best = (int(k.group(1)), files[f], os.path.basename(f))
        return best

    # after the core-numbered names the driver tries interface versions 101 and down
    best = newest(c, top) or (newest("", 101) if c else None)
    return best and (best[1], best[2])


def gpu_wanted():
    """The GPU drivers' files (kernel/gpu-firmware.txt)."""
    listing = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "kernel", "gpu-firmware.txt")
    with open(listing) as f:
        return [line.strip() for line in f if line.strip() and not line.startswith("#")]


def fetch(path, dest):
    """One file, xz-compressed as the kernel's loader expects (CRC32, 1 MiB dictionary)."""
    if os.path.exists(dest):
        return os.path.getsize(dest)
    data = get(FIRMWARE + path)
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    with open(dest + ".tmp", "wb") as f:
        f.write(lzma.compress(data, format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC32,
                              filters=[{"id": lzma.FILTER_LZMA2, "preset": 9, "dict_size": 1 << 20}]))
    os.rename(dest + ".tmp", dest)
    return os.path.getsize(dest)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    files = index()
    jobs = []
    for name in WANTED:
        r = resolve(name, files)
        if not r:
            print(f"   note: no firmware for {name}")
            continue
        path, local = (r, name) if isinstance(r, str) else r
        if not isinstance(r, str):
            local = os.path.join(os.path.dirname(name), local)
        jobs.append((path, os.path.join(out, local + ".xz")))
    # GPU drivers list every file they might want; linux-firmware lacks a few
    missing = 0
    for name in gpu_wanted():
        if name in files:
            jobs.append((files[name], os.path.join(out, name + ".xz")))
        else:
            missing += 1
    if missing:
        print(f"   note: {missing} GPU firmware files the drivers mention aren't in linux-firmware (optional ones)")
    with concurrent.futures.ThreadPoolExecutor(8) as pool:
        total = sum(pool.map(lambda job: fetch(*job), jobs))
    for name in ("regulatory.db", "regulatory.db.p7s"):
        dest = os.path.join(out, name)
        if not os.path.exists(dest):
            with open(dest, "wb") as f:
                f.write(get(REGDB + name))
        total += os.path.getsize(dest)
    print(f">> firmware: {total // 1024} KiB in {out}")


if __name__ == "__main__":
    main()
