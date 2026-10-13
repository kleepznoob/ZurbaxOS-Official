# Zurbax build.
#
#   make            build every library, system program and app into build/
#   make test       build and run the test suites
#   make image      build the kernel with the system inside and the install
#                   media: build/zurbax.iso (a DVD, or write it to a USB stick)
#                   and build/zurbax.img (a plain USB stick image)
#   make iso        just build/zurbax.iso, for every computer (over 6 GB with
#                   the layer, the apps and the firmware built)
#   make iso-mini   build/zurbax-mini.iso, which downloads the rest as it installs
#   make run        boot build/zurbax.img in QEMU with UEFI firmware
#
# Every program is a static, freestanding binary: no C library, no outside code.

CC      ?= gcc
OUT     := build
OPT     ?= -O2

.DEFAULT_GOAL := all

CFLAGS  := -std=gnu11 $(OPT) -g0 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
           -ffreestanding -fno-stack-protector -fno-pie -fno-plt -fno-math-errno \
           -fno-asynchronous-unwind-tables -fno-ident -ffunction-sections -fdata-sections \
           -march=x86-64 -mtune=generic \
           -Ilibzr/include -Ilibzg/include -Ilibzg/src -Ilibzui/include -Ilibzd/include -Ilibzn/include -Ilibza/include
LDFLAGS := -nostdlib -static -no-pie -Wl,--gc-sections -Wl,--build-id=none -s
LDLIBS  := -lgcc

# ---- Libraries ---------------------------------------------------------------
LIBNAMES := $(patsubst lib%,%,$(notdir $(wildcard lib*)))

define LIBRARY
$(1)_SRC := $$(wildcard lib$(1)/src/*.c)
$(1)_OBJ := $$(patsubst %.c,$(OUT)/obj/%.o,$$($(1)_SRC))
$(OUT)/lib/lib$(1).a: $$($(1)_OBJ)
	@mkdir -p $$(dir $$@)
	@rm -f $$@
	@echo "  AR    $$@"
	@ar rcs $$@ $$^
endef
$(foreach l,$(LIBNAMES),$(eval $(call LIBRARY,$(l))))

# Everything is compiled again when the flags change (a different -march, say).
FLAGS_STAMP := $(OUT)/obj/.flags
$(shell mkdir -p $(OUT)/obj; [ "$$(cat $(FLAGS_STAMP) 2>/dev/null)" = '$(CC) $(CFLAGS)' ] || printf '%s' '$(CC) $(CFLAGS)' >$(FLAGS_STAMP))

$(OUT)/obj/%.o: %.c $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	@echo "  CC    $<"
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# Link order: higher-level libraries first.
LINK_LIBS := $(foreach l,zui zd zn za zg zr,$(if $(filter $(l),$(LIBNAMES)),$(OUT)/lib/lib$(l).a))

# ---- Programs: system/<name>/, apps/<name>/ and tools/<name>/ ----------------
SYSTEM := $(notdir $(wildcard system/*))
APPS   := $(notdir $(wildcard apps/*))
TOOLS  := $(patsubst tools/%/,%,$(wildcard tools/*/))
STORE  := $(notdir $(wildcard store/*))

define PROGRAM
$(2)_SRC := $$(wildcard $(1)/$(2)/*.c)
$(2)_OBJ := $$(patsubst %.c,$(OUT)/obj/%.o,$$($(2)_SRC))
$(3): $$($(2)_OBJ) $(LINK_LIBS)
	@mkdir -p $$(dir $$@)
	@echo "  LD    $$@"
	@$(CC) $(LDFLAGS) -o $$@ $$($(2)_OBJ) $(LINK_LIBS) $(LDLIBS)
ALL_PROGRAMS += $(3)
endef
$(foreach p,$(SYSTEM),$(eval $(call PROGRAM,system,$(p),$(OUT)/bin/$(p))))
$(foreach p,$(APPS),$(eval $(call PROGRAM,apps,$(p),$(OUT)/apps/$(p)/$(p))))
$(foreach p,$(TOOLS),$(eval $(call PROGRAM,tools,$(p),$(OUT)/tools/$(p))))
$(foreach p,$(STORE),$(eval $(call PROGRAM,store,$(p),$(OUT)/store/$(p)/$(p))))

# ---- Harbor's built-in source: apps in store/ become packages ---------------------
# Each store app also renders its first screen to a picture for the store page.
REPO_DEPS := $(foreach p,$(STORE),$(OUT)/store/$(p)/$(p) $(wildcard store/$(p)/*.conf store/$(p)/*.svg))
$(OUT)/repo/index: $(REPO_DEPS) $(OUT)/tools/mkimage
	@echo "  REPO  $(OUT)/repo"
	@$(MAKE) --no-print-directory devroot-links
	@for p in $(STORE); do ZURBAX_ROOT=$(CURDIR)/$(OUT)/root/system HOME=/nonexistent \
		ZUI_SNAPSHOT=$(OUT)/store/$$p/screenshot.png $(OUT)/store/$$p/$$p >/dev/null 2>&1 || true; done
	@rm -rf $(OUT)/repo && $(OUT)/tools/mkimage repo $(OUT)/repo >/dev/null

# ---- Tests -----------------------------------------------------------------------
TESTS := $(patsubst tests/%.c,$(OUT)/tests/%,$(wildcard tests/*.c))
$(OUT)/tests/%: $(OUT)/obj/tests/%.o $(LINK_LIBS)
	@mkdir -p $(dir $@)
	@echo "  LD    $@"
	@$(CC) $(LDFLAGS) -o $@ $< $(LINK_LIBS) $(LDLIBS)

.PHONY: media-prep system-efi iso iso-mini downloads downloads-files apps firmware all test clean devroot devroot-links image run
.SECONDARY:
all: $(foreach l,$(LIBNAMES),$(OUT)/lib/lib$(l).a) $(ALL_PROGRAMS) $(if $(STORE),$(OUT)/repo/index)

# A development root that mirrors /system, so programs find their data files.
DEVROOT := $(OUT)/root/system
devroot: all devroot-links
devroot-links:
	@mkdir -p $(DEVROOT)/share $(DEVROOT)/bin
	@ln -sfn $(CURDIR)/$(OUT)/repo $(DEVROOT)/share/harbor-repo
	@ln -sfn $(CURDIR)/data/icons $(DEVROOT)/share/icons
	@for f in $(CURDIR)/data/*; do case $$f in */icons) ;; *) ln -sfn $$f $(DEVROOT)/share/$$(basename $$f);; esac; done
	@for p in $(SYSTEM); do ln -sfn $(CURDIR)/$(OUT)/bin/$$p $(DEVROOT)/bin/$$p; done
	@for a in $(APPS); do \
		mkdir -p $(DEVROOT)/apps/$$a; \
		ln -sfn $(CURDIR)/$(OUT)/apps/$$a/$$a $(DEVROOT)/apps/$$a/$$a; \
		for f in apps/$$a/*; do case $$f in *.c|*.h) ;; *) ln -sfn $(CURDIR)/$$f $(DEVROOT)/apps/$$a/$$(basename $$f);; esac; done; \
	done

test: $(TESTS) devroot
	@for t in $(TESTS); do echo "== $$t"; ZURBAX_ROOT=$(DEVROOT) $$t || exit 1; done

# ---- Bootable image ----------------------------------------------------------------
# The system archive is embedded in the kernel, so the one EFI file is the
# whole OS: firmware loads it directly, with no boot loader in between.
EFI_FILE := $(OUT)/kernel/arch/x86/boot/bzImage
# The Linux layer (layer/build.sh makes it), the apps (make apps) and the
# firmware of every device (make firmware) go on the install media when they
# have been built.
LAYER ?= $(wildcard $(OUT)/layer/steam.sfs $(OUT)/preload/apps*.sfs $(OUT)/firmware.sfs)

# Every file of linux-firmware in one compressed image: zinit points the
# kernel's firmware loader at it (the system image keeps only what starts
# the screen and Wi-Fi).
firmware: $(OUT)/firmware.sfs
$(OUT)/firmware.sfs: tools/fetch-firmware.py
	@python3 tools/fetch-firmware.py --all $(OUT)/firmware-all
	@rm -f $@.part
	@mksquashfs $(OUT)/firmware-all $@.part -comp zstd -Xcompression-level 19 -b 1M -all-root -noappend -quiet
	@mv $@.part $@
	@ls -l $@

media-prep: all
	@python3 tools/fetch-firmware.py $(OUT)/firmware || echo "   note: no Wi-Fi or GPU firmware fetched (offline?)"
	@$(OUT)/tools/mkimage cpio $(OUT)/system.cpio

# The system alone, as the one EFI file the install media and the download
# place carry.
system-efi: media-prep
	@ZURBAX_CMDLINE_EXTRA= kernel/build.sh $(OUT)/system.cpio
	@mkdir -p $(OUT)/downloads && cp $(EFI_FILE) $(OUT)/downloads/zurbax.efi

# The install media, for every computer: the system, the Linux layer, the
# apps that come ready to use and every device's firmware, so any graphics
# card, Wi-Fi chip or controller works from the first start.
image: system-efi
	@$(OUT)/tools/mkimage disk $(OUT)/zurbax.img $(EFI_FILE) $(LAYER)
	@$(OUT)/tools/mkimage iso $(OUT)/zurbax.iso $(EFI_FILE) $(LAYER)
iso: system-efi
	@$(OUT)/tools/mkimage iso $(OUT)/zurbax.iso $(EFI_FILE) $(LAYER)

# The apps that come ready to use (tools/preload-apps.sh needs flatpak).
apps: $(OUT)/preload/apps.sfs
$(OUT)/preload/apps.sfs: tools/preload-apps.sh
	@tools/preload-apps.sh $(OUT)/preload

# The mini install image: the system alone (69 MB, for any USB stick). Its
# installer downloads the rest from DOWNLOADS, which zurbax.com sends on to
# where `make downloads` files are kept; when that can't be reached, it
# installs the stick's own system.
DOWNLOADS ?= https://zurbax.com/download/2.0/
iso-mini: system-efi
	@mkdir -p $(OUT)/mini && printf '%s\n' '$(DOWNLOADS)' >$(OUT)/mini/NETINST.TXT
	@$(OUT)/tools/mkimage iso $(OUT)/zurbax-mini.iso $(EFI_FILE) $(OUT)/mini/NETINST.TXT

# What the download place serves: the full image, the mini one, and the
# pieces the mini installer downloads, with a manifest of their sizes and
# SHA-256 sums (linked, not copied: the full image alone is over 6 GB).
downloads: iso iso-mini downloads-files
downloads-files:
	@mkdir -p $(OUT)/downloads && ln -f $(LAYER) $(OUT)/zurbax.iso $(OUT)/zurbax-mini.iso $(OUT)/downloads/
	@[ -f $(OUT)/downloads/zurbax.efi ] || ln -f $(OUT)/zurbax.efi $(OUT)/downloads/zurbax.efi
	@cd $(OUT)/downloads && for f in *.efi *.sfs; do printf '%s %s %s\n' "$$f" "$$(stat -c %s "$$f")" "$$(sha256sum "$$f" | cut -d' ' -f1)"; done >manifest
	@cd $(OUT)/downloads && sha256sum zurbax.iso zurbax-mini.iso >SHA256SUMS
	@cat $(OUT)/downloads/manifest

OVMF_CODE ?= /usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS ?= /usr/share/OVMF/OVMF_VARS_4M.fd
run:
	@cp -n $(OVMF_VARS) $(OUT)/ovmf-vars.fd 2>/dev/null || true
	qemu-system-x86_64 -machine q35 -m 2G -smp 2 $(if $(wildcard /dev/kvm),-enable-kvm -cpu host,-cpu max) \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(OUT)/ovmf-vars.fd \
		-drive file=$(OUT)/zurbax.img,format=raw,if=virtio \
		-device virtio-vga -device qemu-xhci -device usb-tablet -device usb-kbd \
		-nic user,model=virtio-net-pci -audiodev none,id=snd -device intel-hda -device hda-output,audiodev=snd

clean:
	rm -rf $(OUT)/obj $(OUT)/lib $(OUT)/bin $(OUT)/apps $(OUT)/tests $(OUT)/tools $(OUT)/store $(OUT)/repo

-include $(shell find $(OUT)/obj -name '*.d' 2>/dev/null)
