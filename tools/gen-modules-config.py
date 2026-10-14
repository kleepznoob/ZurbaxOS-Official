#!/usr/bin/env python3
"""
gen-modules-config.py: writes kernel/modules.config, the drivers Ember
builds as modules: every one a PC or laptop can have.

    tools/gen-modules-config.py REFERENCE.config KERNEL_SOURCE BUILTIN.config > kernel/modules.config

REFERENCE is a general-purpose distribution's configuration for the same
kernel series (Arch Linux's linux-lts config.x86_64, which builds nearly
every driver); its modules are kept when the Kconfig file that defines them
sits in a part of the kernel for PCs. What is left out is hardware a PC
doesn't have (data-centre fabrics and NICs, industrial converters, phone
and board chips, CAN and amateur radio buses, device-side USB) and test or
debugging code. Nor does it name what Ember builds in: BUILTIN is
x86_64_defconfig with kernel/zurbax.config merged (make O=DIR
x86_64_defconfig, merge_config.sh, make olddefconfig), and whatever is =y
there stays out of this list, so the CD drive, the PS/2 mouse and the rest
that starting needs aren't turned into modules.
"""
import os
import re
import sys

KEEP_UNDER = ("drivers/", "sound/", "net/", "fs/", "crypto/", "lib/", "arch/x86/", "block/", "security/keys", "virt/")

LEAVE_OUT = (
    # data-centre fabrics, storage networks and their adapters
    "drivers/infiniband", "drivers/target", "drivers/nvme/target", "drivers/scsi/qla2xxx", "drivers/scsi/qla4xxx",
    "drivers/scsi/lpfc", "drivers/scsi/fcoe", "drivers/scsi/libfc", "drivers/scsi/bnx2fc", "drivers/scsi/bnx2i",
    "drivers/scsi/csiostor", "drivers/scsi/qedf", "drivers/scsi/qedi", "drivers/scsi/elx", "drivers/scsi/cxgbi",
    "drivers/scsi/be2iscsi", "drivers/net/ethernet/mellanox", "drivers/net/ethernet/chelsio", "drivers/net/ethernet/qlogic",
    "drivers/net/ethernet/huawei", "drivers/net/ethernet/cavium", "drivers/net/ethernet/netronome",
    "drivers/net/ethernet/pensando", "drivers/net/ethernet/sfc", "drivers/net/ethernet/emulex", "drivers/net/ethernet/brocade",
    "drivers/net/ethernet/fungible", "drivers/net/ethernet/microsoft", "drivers/net/ethernet/intel/ice",
    "drivers/net/ethernet/intel/idpf", "drivers/net/ethernet/broadcom/bnxt", "drivers/net/ethernet/marvell/octeon",
    "drivers/net/ethernet/amd/pds_core", "drivers/net/ethernet/meta", "drivers/vdpa", "drivers/vfio/pci/mlx5",
    "drivers/net/dsa", "net/dsa", "drivers/net/ipa", "drivers/net/fddi", "drivers/net/arcnet", "drivers/net/plip",
    "drivers/net/slip", "drivers/net/wan", "drivers/atm", "net/atm",
    # buses and radios a PC doesn't have
    "drivers/net/can", "net/can", "drivers/net/hamradio", "net/ax25", "net/netrom", "net/rose", "drivers/isdn",
    "drivers/net/ieee802154", "net/ieee802154", "net/mac802154", "drivers/greybus", "drivers/most", "drivers/fpga",
    "drivers/comedi", "drivers/ipack", "drivers/siox", "drivers/slimbus", "drivers/counter", "drivers/w1",
    # the device side of USB, for gadgets rather than computers
    "drivers/usb/gadget",
    # sensors and converters for industrial and embedded boards (a laptop's
    # accelerometer, gyroscope, compass and light sensor stay)
    "drivers/iio/adc", "drivers/iio/dac", "drivers/iio/addac", "drivers/iio/afe", "drivers/iio/amplifiers",
    "drivers/iio/cdc", "drivers/iio/chemical", "drivers/iio/frequency", "drivers/iio/health", "drivers/iio/humidity",
    "drivers/iio/potentiometer", "drivers/iio/potentiostat", "drivers/iio/pressure", "drivers/iio/proximity",
    "drivers/iio/resolver", "drivers/iio/temperature", "drivers/iio/filter", "drivers/iio/position",
    "drivers/hwmon/pmbus", "drivers/regulator", "drivers/rtc", "drivers/watchdog", "drivers/mfd",
    # unfinished drivers
    "drivers/staging",
)

# what is kept of the above anyway: a Wi-Fi chip in many cheap tablets
KEEP_ANYWAY = ("RTL8723BS",)

NOT_THESE = re.compile(r"(^|_)(TEST|TESTS|KUNIT|SAMPLE|SAMPLES|DEBUG|DEBUGFS|KPROBE|FTRACE|BPF)(_|$)")


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    reference, src, builtin_cfg = sys.argv[1], sys.argv[2], sys.argv[3]
    builtin = set()
    for line in open(builtin_cfg):
        m = re.match(r"CONFIG_([A-Z0-9_]+)=y$", line.strip())
        if m:
            builtin.add(m.group(1))
    where = {}
    for root, _, files in os.walk(src):
        for f in files:
            if not f.startswith("Kconfig"):
                continue
            path = os.path.join(root, f)
            rel = os.path.relpath(path, src)
            with open(path, errors="replace") as fh:
                for line in fh:
                    m = re.match(r"\s*(?:menu)?config\s+([A-Z0-9_]+)", line)
                    if m:
                        where.setdefault(m.group(1), rel)
    kept = []
    for line in open(reference):
        m = re.match(r"CONFIG_([A-Z0-9_]+)=m$", line.strip())
        if not m:
            continue
        sym = m.group(1)
        path = where.get(sym)
        if not path or NOT_THESE.search(sym) or sym in builtin:
            continue
        if sym not in KEEP_ANYWAY:
            if not path.startswith(KEEP_UNDER) or path.startswith(LEAVE_OUT):
                continue
        kept.append((path, sym))
    kept.sort()
    print("# Ember's drivers built as modules: every one a PC or laptop can have.")
    print("# Written by tools/gen-modules-config.py from Arch Linux's linux-lts")
    print("# config.x86_64 for this kernel series; don't edit, run it again.")
    print("# kernel/zurbax.config comes after, so what it builds in stays built in.")
    last = None
    for path, sym in kept:
        d = os.path.dirname(path)
        if d != last:
            print("# %s" % d)
            last = d
        print("CONFIG_%s=m" % sym)
    print("# %d modules" % len(kept), file=sys.stderr)


if __name__ == "__main__":
    main()
