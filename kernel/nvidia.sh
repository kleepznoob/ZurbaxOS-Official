#!/bin/sh
# NVIDIA's own driver for GeForce RTX 20 and newer: NVIDIA's open kernel
# modules, built against Ember (kernel/build.sh signs them into drivers.sfs),
# and nvidia.sfs, the layer with NVIDIA's OpenGL, Vulkan, EGL, video and CUDA
# libraries and the GPU's firmware, which Zurbax mounts as /data/layers/nvidia
# and puts into the Linux layer (layerrun) when it's there.
#
#   kernel/nvidia.sh modules     build the kernel modules (needs build/kernel)
#   kernel/nvidia.sh install DIR install them, signed, under DIR/lib/modules
#   kernel/nvidia.sh layer       make build/nvidia.sfs
#
# The driver comes from NVIDIA's download site, checked against the SHA-256
# below. NVIDIA's license lets Linux distributions carry it unmodified; it's
# repackaged here, with the license, as distributions do.
set -eu

VER=595.104.02
SHA256=e421c202e4c79f58c3c7f3161bbe71454ebb3d88936f88205a0e327cd04c59ca
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=$ROOT/build/nvidia
RUN=$WORK/NVIDIA-Linux-x86_64-$VER.run
SRC=$WORK/src
KSRC=${ZURBAX_KSRC:-$ROOT/build/linux-6.18.55}
KOUT=$ROOT/build/kernel
JOBS=${JOBS:-$(nproc)}

fetch() {
	[ -f "$SRC/.ok-$VER" ] && return
	mkdir -p "$WORK"
	if [ ! -f "$RUN" ]; then
		echo ">> fetching NVIDIA $VER"
		curl -fL --retry 3 -o "$RUN.part" "https://download.nvidia.com/XFree86/Linux-x86_64/$VER/NVIDIA-Linux-x86_64-$VER.run"
		mv "$RUN.part" "$RUN"
	fi
	echo "$SHA256  $RUN" | sha256sum -c --quiet
	rm -rf "$SRC"
	sh "$RUN" -x --target "$SRC" >/dev/null
	touch "$SRC/.ok-$VER"
}

modules() {
	fetch
	echo ">> building NVIDIA's kernel modules"
	make -s -C "$SRC/kernel-open" -j"$JOBS" SYSSRC="$KSRC" SYSOUT="$KOUT" modules >"$WORK/kbuild.log" 2>&1 ||
		{ tail -20 "$WORK/kbuild.log"; exit 1; }
}

# Kbuild signs them with Ember's key as it installs them (MODULE_SIG_ALL).
install() {
	dest=$1
	make -s -C "$KSRC" O="$KOUT" M="$SRC/kernel-open" INSTALL_MOD_PATH="$dest" INSTALL_MOD_DIR=extra/nvidia \
		INSTALL_MOD_STRIP=1 modules_install
}

# ---- nvidia.sfs ----------------------------------------------------------------------------
# root/ goes over the Linux layer's own root (same folders: /usr/lib is its
# 64-bit library folder, /usr/lib32 the 32-bit one); firmware/ is what the
# kernel module loads into the GPU.

# What games, video and GPU programs use; not what only X.Org, nvidia-settings,
# OptiX renderers, CUDA debuggers or Vulkan SC need.
LIBS64="libEGL_nvidia libGLESv1_CM_nvidia libGLESv2_nvidia libGLX_nvidia libcuda libnvcuvid
libnvidia-allocator libnvidia-api libnvidia-eglcore libnvidia-encode libnvidia-glcore libnvidia-glsi
libnvidia-glvkspirv libnvidia-gpucomp libnvidia-ml libnvidia-ngx libnvidia-nvvm libnvidia-opencl
libnvidia-opticalflow libnvidia-ptxjitcompiler libnvidia-rtcore libnvidia-tls libnvidia-egl-gbm
libnvidia-egl-wayland libnvidia-egl-wayland2 libnvidia-egl-xcb libnvidia-egl-xlib libvdpau_nvidia"
LIBS32="libEGL_nvidia libGLESv1_CM_nvidia libGLESv2_nvidia libGLX_nvidia libcuda libnvcuvid
libnvidia-allocator libnvidia-eglcore libnvidia-encode libnvidia-glcore libnvidia-glsi
libnvidia-glvkspirv libnvidia-gpucomp libnvidia-ml libnvidia-nvvm libnvidia-opencl
libnvidia-opticalflow libnvidia-ptxjitcompiler libnvidia-tls libvdpau_nvidia"

# Copies FROM/NAME.so.* into DEST, with the links its users look for: the
# soname from the library itself, and NAME.so.
put_lib() {
	from=$1 name=$2 dest=$3
	for f in "$from/$name".so.*; do
		[ -f "$f" ] && [ ! -L "$f" ] || continue
		cp "$f" "$dest/"
		base=$(basename "$f")
		soname=$(readelf -d "$f" 2>/dev/null | sed -n 's/.*(SONAME).*\[\(.*\)\]/\1/p')
		[ -n "$soname" ] && [ "$soname" != "$base" ] && ln -sf "$base" "$dest/$soname"
		[ "$name.so" != "$base" ] && ln -sf "${soname:-$base}" "$dest/$name.so"
	done
}

layer() {
	fetch
	echo ">> making nvidia.sfs"
	stage=$WORK/stage
	rm -rf "$stage"
	r=$stage/root
	mkdir -p "$r/usr/lib/gbm" "$r/usr/lib/vdpau" "$r/usr/lib/nvidia/wine" "$r/usr/lib32/vdpau" "$r/usr/bin" \
		"$r/usr/share/vulkan/icd.d" "$r/usr/share/vulkan/implicit_layer.d" "$r/usr/share/glvnd/egl_vendor.d" \
		"$r/usr/share/egl/egl_external_platform.d" "$r/usr/share/nvidia" "$r/etc/OpenCL/vendors" \
		"$r/usr/share/licenses/nvidia" "$stage/firmware/nvidia/$VER"
	for l in $LIBS64; do put_lib "$SRC" "$l" "$r/usr/lib"; done
	for l in $LIBS32; do put_lib "$SRC/32" "$l" "$r/usr/lib32"; done
	# VDPAU finds its drivers in a vdpau folder; GBM its backends by the DRM driver's name
	for d in lib lib32; do
		mv "$r/usr/$d"/libvdpau_nvidia.so* "$r/usr/$d/vdpau/"
	done
	ln -sf ../libnvidia-allocator.so.1 "$r/usr/lib/gbm/nvidia-drm_gbm.so"
	# Vulkan: a file per architecture with the full path, as Mesa's are, so
	# the Steam runtime's container finds both
	sed 's|"library_path": *"[^"]*"|"library_path": "/usr/lib/libGLX_nvidia.so.0"|' "$SRC/nvidia_icd.json" \
		>"$r/usr/share/vulkan/icd.d/nvidia_icd.x86_64.json"
	sed 's|"library_path": *"[^"]*"|"library_path": "/usr/lib32/libGLX_nvidia.so.0"|' "$SRC/nvidia_icd.json" \
		>"$r/usr/share/vulkan/icd.d/nvidia_icd.i686.json"
	cp "$SRC/nvidia_layers.json" "$r/usr/share/vulkan/implicit_layer.d/"
	cp "$SRC/10_nvidia.json" "$r/usr/share/glvnd/egl_vendor.d/"
	for j in "$SRC"/[0-9][0-9]_nvidia_*.json; do cp "$j" "$r/usr/share/egl/egl_external_platform.d/"; done
	cp "$SRC/nvidia.icd" "$r/etc/OpenCL/vendors/"
	# DLSS in Windows games under Proton, which looks for these next to libGLX_nvidia
	cp "$SRC/nvngx.dll" "$SRC/_nvngx.dll" "$SRC/nvngx_dlssg.dll" "$r/usr/lib/nvidia/wine/" 2>/dev/null || true
	cp "$SRC/nvidia-smi" "$SRC/nvidia-pcc" "$r/usr/bin/" 2>/dev/null || cp "$SRC/nvidia-smi" "$r/usr/bin/"
	# a program on the NVIDIA GPU of a laptop that has two (Steam: prime-run %command%)
	cat >"$r/usr/bin/prime-run" <<-'EOF'
	#!/bin/sh
	export __NV_PRIME_RENDER_OFFLOAD=1 __VK_LAYER_NV_optimus=NVIDIA_only __GLX_VENDOR_LIBRARY_NAME=nvidia
	exec "$@"
	EOF
	chmod 755 "$r/usr/bin/prime-run"
	cp "$SRC/nvidia-application-profiles-$VER-rc" "$SRC/nvidia-application-profiles-$VER-key-documentation" "$r/usr/share/nvidia/"
	cp "$SRC/LICENSE" "$r/usr/share/licenses/nvidia/"
	cp "$SRC"/firmware/gsp_*.bin "$stage/firmware/nvidia/$VER/"
	# The libraries in the layer's cache, with the layer's own: ldconfig over
	# a copy of its library folders (hard links, so it takes no room)
	if [ -d "$ROOT/build/layer/root/usr/lib" ]; then
		c=$WORK/ldcache
		rm -rf "$c"
		mkdir -p "$c/etc" "$c/usr"
		cp -al "$ROOT/build/layer/root/usr/lib" "$ROOT/build/layer/root/usr/lib32" "$c/usr/"
		cp "$ROOT/build/layer/root/etc/ld.so.conf" "$c/etc/"
		# (new files, never written through a link into the layer's own)
		cp -a --remove-destination "$r/usr/lib/." "$c/usr/lib/"
		cp -a --remove-destination "$r/usr/lib32/." "$c/usr/lib32/"
		# the layer's own ldconfig, as layer/packages.sh runs it (-r enters with chroot)
		"$ROOT/build/layer/root/usr/bin/ldconfig" -r "$c" -X
		cp "$c/etc/ld.so.cache" "$r/etc/ld.so.cache"
		rm -rf "$c"
	fi
	echo "$VER" >"$stage/VERSION"
	rm -f "$ROOT/build/nvidia.sfs.part"
	mksquashfs "$stage" "$ROOT/build/nvidia.sfs.part" -comp zstd -Xcompression-level 19 -b 1M -all-root -noappend -quiet -no-progress
	mv "$ROOT/build/nvidia.sfs.part" "$ROOT/build/nvidia.sfs"
	rm -rf "$stage"
	ls -l "$ROOT/build/nvidia.sfs"
}

case "${1:-}" in
modules) modules ;;
install) install "$2" ;;
layer) layer ;;
*) echo "usage: kernel/nvidia.sh modules | install DIR | layer" >&2; exit 2 ;;
esac
