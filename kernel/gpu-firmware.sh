#!/bin/sh
# Remakes kernel/gpu-firmware.txt from a built kernel's list of the firmware
# its drivers load (after a kernel update): kernel/gpu-firmware.sh [KERNEL_BUILD_DIR]
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$ROOT/build/kernel}
{
	sed -n '/^#/p' "$ROOT/kernel/gpu-firmware.txt"
	tr '\0' '\n' <"$OUT/modules.builtin.modinfo" | sed -n 's/^\(amdgpu\|i915\|xe\)\.firmware=//p' | awk '!seen[$0]++' |
		grep -Ev 'amdgpu/.*(tahiti|pitcairn|verde|oland|hainan|si58|banks|bonaire|hawaii|kaveri|kabini|mullins|topaz|tonga|carrizo|fiji|stoney|vega20|arcturus|aldebaran|cyan_skillfish|gc_9_4_[34]|gc_9_5_|psp_13_0_(6|12|14)|sdma_4_4_[245]|vcn_4_0_3|vcn_5_0_1|isp_|smu_13_0_6|smu_13_0_12|smu_13_0_14|vce_1_0|mes_v12_1|gc_12_1|psp_15_0_8)' |
		grep -Ev 'i915/(skl|bxt|kbl|glk|cml)_(guc|huc)'
	# (nouveau is a module now: its GSP firmware comes from firmware.sfs, like NVIDIA's from nvidia.sfs)
} >"$ROOT/kernel/gpu-firmware.txt.new"
mv "$ROOT/kernel/gpu-firmware.txt.new" "$ROOT/kernel/gpu-firmware.txt"
grep -vc '^#' "$ROOT/kernel/gpu-firmware.txt"
