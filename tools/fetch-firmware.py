#!/usr/bin/env python3
"""Fetches the firmware Zurbax's drivers load.

    tools/fetch-firmware.py OUTDIR          what starts the screen and Wi-Fi, for the system image
    tools/fetch-firmware.py --all OUTDIR    every file, for the firmware image (firmware.sfs)

Wi-Fi chips and GPUs run vendor firmware that the kernel hands them at
start-up; it comes from the linux-firmware project (redistributable,
unmodified). This takes the newest linux-firmware release from kernel.org's
download servers, reads its index (WHENCE) for the files and links, and
keeps the files for the Wi-Fi chips common in laptops from the last few
years and for the AMD, Intel and NVIDIA GPUs in kernel/gpu-firmware.txt. They are
compressed with xz (the kernel unpacks them as it loads them, so they take
less memory). The wireless regulatory database comes the same way.

The release is streamed, never stored whole. OUTDIR remembers which release
it holds, so a second run downloads nothing; delete OUTDIR to start over.
"""
import lzma, os, re, sys, tarfile, time, urllib.request

# kernel.org's download servers: release tarballs, sturdier than fetching
# hundreds of single files from the git web front end (which turns build
# machines away with "503 Service Temporarily Unavailable")
FIRMWARE = "https://cdn.kernel.org/pub/linux/kernel/firmware/"
REGDB = "https://cdn.kernel.org/pub/software/network/wireless-regdb/"

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


def open_url(url):
    """A response for url, retried a few times: download servers have bad moments."""
    req = urllib.request.Request(url, headers={"User-Agent": "zurbax-build/1.0 (fetch-firmware)"})
    for attempt in range(5):
        try:
            return urllib.request.urlopen(req, timeout=120)
        except OSError:
            if attempt == 4:
                raise
            time.sleep(2 ** attempt * 3)


def newest(listing, pattern):
    """The newest release named in a download folder's listing."""
    with open_url(listing) as r:
        names = set(re.findall(pattern, r.read().decode("utf-8", "replace")))
    if not names:
        sys.exit(f"no release found at {listing}")
    return max(names)


def index(whence):
    """Every file in linux-firmware by the name drivers ask for (links included)."""
    files = {}
    for line in whence.decode("utf-8", "replace").splitlines():
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


def gpu_file(name, files):
    """The release file a GPU firmware name ends up at, through linked files and
    linked folders (nvidia/ad107 -> ad102, whose gsp-*.bin -> ../../ga102/...)."""
    for _ in range(8):
        if files.get(name) == name:
            return name
        if name in files:
            name = files[name]
            continue
        parts = name.split("/")
        for i in range(len(parts) - 1, 0, -1):
            head = "/".join(parts[:i])
            if files.get(head, head) != head:
                name = "/".join([files[head]] + parts[i:])
                break
        else:
            return None
    return None


def gpu_wanted():
    """The GPU drivers' files (kernel/gpu-firmware.txt)."""
    listing = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "kernel", "gpu-firmware.txt")
    with open(listing) as f:
        return [line.strip() for line in f if line.strip() and not line.startswith("#")]


def save(data, dest):
    """One file, xz-compressed as the kernel's loader expects (CRC32, 1 MiB dictionary)."""
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    with open(dest + ".tmp", "wb") as f:
        f.write(lzma.compress(data, format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC32,
                              filters=[{"id": lzma.FILTER_LZMA2, "preset": 9, "dict_size": 1 << 20}]))
    os.rename(dest + ".tmp", dest)


def plan(files, out):
    """Which files of the release to keep, and under which names: {path: [dest...]}."""
    want = {}
    for name in WANTED:
        r = resolve(name, files)
        if not r:
            print(f"   note: no firmware for {name}")
            continue
        path, local = (r, name) if isinstance(r, str) else r
        if not isinstance(r, str):
            local = os.path.join(os.path.dirname(name), local)
        want.setdefault(path, []).append(os.path.join(out, local + ".xz"))
    # GPU drivers list every file they might want; linux-firmware lacks a few
    missing = 0
    for name in gpu_wanted():
        path = gpu_file(name, files)
        if path:
            want.setdefault(path, []).append(os.path.join(out, name + ".xz"))
        else:
            missing += 1
    if missing:
        print(f"   note: {missing} GPU firmware files the drivers mention aren't in linux-firmware (optional ones)")
    return want


def firmware(out):
    """Streams the newest linux-firmware release, keeping what Zurbax needs."""
    release = newest(FIRMWARE, r"linux-firmware-\d{8}\.tar\.xz")
    print(f">> firmware from {release}")
    want, kept = None, 0
    with open_url(FIRMWARE + release) as r, tarfile.open(fileobj=r, mode="r|xz") as tar:
        for m in tar:
            if not m.isfile() or "/" not in m.name:
                continue
            path = m.name.split("/", 1)[1]
            # WHENCE, the index, comes before the folders (git sorts capitals first)
            if path == "WHENCE":
                want = plan(index(tar.extractfile(m).read()), out)
            elif want is not None and path in want:
                data = tar.extractfile(m).read()
                first, *others = want.pop(path)
                save(data, first)
                kept += 1
                # names that share the file are links to it, not copies
                for dest in others:
                    os.makedirs(os.path.dirname(dest), exist_ok=True)
                    if os.path.lexists(dest):
                        os.remove(dest)
                    os.symlink(os.path.relpath(first, os.path.dirname(dest)), dest)
                    kept += 1
    if want is None:
        sys.exit(f"{release} has no WHENCE")
    for path in want:
        print(f"   note: {path} wasn't in {release}")
    return release, kept


def everything(out):
    """Every file of the newest linux-firmware release, with the links its
    index names, unpacked as they are (the image they go into compresses)."""
    release = newest(FIRMWARE, r"linux-firmware-\d{8}\.tar\.xz")
    print(f">> all firmware from {release}")
    links, kept = [], 0
    with open_url(FIRMWARE + release) as r, tarfile.open(fileobj=r, mode="r|xz") as tar:
        for m in tar:
            if "/" not in m.name:
                continue
            path = m.name.split("/", 1)[1]
            if not path or path.startswith((".", "debian/", "LICEN", "README", "Makefile", "copy-firmware", "check_whence",
                                            "build_packages", "dedup-firmware", "contrib/")) or ".." in path.split("/"):
                continue
            dest = os.path.join(out, path)
            if path == "WHENCE":
                for line in tar.extractfile(m).read().decode("utf-8", "replace").splitlines():
                    k = re.match(r"^Link:\s*(\S+)\s*->\s*(\S+)", line)
                    if k:
                        links.append((k.group(1), k.group(2)))
            elif m.isfile():
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                with open(dest + ".tmp", "wb") as f:
                    f.write(tar.extractfile(m).read())
                os.rename(dest + ".tmp", dest)
                kept += 1
            elif m.issym() and not os.path.isabs(m.linkname):
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                if os.path.lexists(dest):
                    os.remove(dest)
                os.symlink(m.linkname, dest)
    # the names drivers ask for that are links to other files
    for name, target in links:
        dest = os.path.join(out, name)
        if ".." in name.split("/") or os.path.isabs(target) or os.path.lexists(dest):
            continue
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        os.symlink(target, dest)
    return release, kept


def regdb(out):
    """The wireless regulatory database and its signature."""
    release = newest(REGDB, r"wireless-regdb-[\d.]+\d\.tar\.xz")
    with open_url(REGDB + release) as r, tarfile.open(fileobj=r, mode="r|xz") as tar:
        for m in tar:
            name = os.path.basename(m.name)
            if m.isfile() and name in ("regulatory.db", "regulatory.db.p7s"):
                with open(os.path.join(out, name), "wb") as f:
                    f.write(tar.extractfile(m).read())
    return release


def main():
    full = len(sys.argv) == 3 and sys.argv[1] == "--all"
    if len(sys.argv) != 2 and not full:
        sys.exit(__doc__)
    out = sys.argv[-1]
    os.makedirs(out, exist_ok=True)
    marker = os.path.join(out, ".release")
    have = open(marker).read().split() if os.path.exists(marker) else []
    latest = newest(FIRMWARE, r"linux-firmware-\d{8}\.tar\.xz")
    if latest in have and os.path.exists(os.path.join(out, "regulatory.db")):
        print(f">> firmware: {latest} already in {out}")
        return
    release, kept = everything(out) if full else firmware(out)
    reg = regdb(out)
    with open(marker, "w") as f:
        f.write(f"{release} {reg}\n")
    total = sum(os.lstat(os.path.join(d, n)).st_size for d, _, ns in os.walk(out) for n in ns)
    print(f">> firmware: {kept} files, {total // 1024} KiB in {out}")


if __name__ == "__main__":
    main()
