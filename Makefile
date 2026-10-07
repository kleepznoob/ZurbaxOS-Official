# ZOS build.
#
#   make            build every library, system program and app into build/
#   make test       build and run the test suites
#   make image      build the kernel with the system inside and the install
#                   media: build/zos.iso (a DVD, or write it to a USB stick)
#                   and build/zos.img (a plain USB stick image)
#   make run        boot build/zos.img in QEMU with UEFI firmware
#
# Every program is a static, freestanding binary: no C library, no outside code.

CC      ?= gcc
OUT     := build
OPT     ?= -O2

.DEFAULT_GOAL := all

CFLAGS  := -std=gnu11 $(OPT) -g0 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
           -ffreestanding -fno-stack-protector -fno-pie -fno-plt -fno-math-errno \
           -fno-asynchronous-unwind-tables -fno-ident -ffunction-sections -fdata-sections \
           -march=x86-64-v2 -mtune=generic \
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

$(OUT)/obj/%.o: %.c
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

.PHONY: all test clean devroot devroot-links image run
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
image: all
	@python3 tools/fetch-firmware.py $(OUT)/firmware || echo "   note: no Wi-Fi firmware fetched (offline?)"
	@$(OUT)/tools/mkimage cpio $(OUT)/system.cpio
	@kernel/build.sh $(OUT)/system.cpio
	@$(OUT)/tools/mkimage disk $(OUT)/zos.img $(EFI_FILE)
	@$(OUT)/tools/mkimage iso $(OUT)/zos.iso $(EFI_FILE)

OVMF_CODE ?= /usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS ?= /usr/share/OVMF/OVMF_VARS_4M.fd
run:
	@cp -n $(OVMF_VARS) $(OUT)/ovmf-vars.fd 2>/dev/null || true
	qemu-system-x86_64 -machine q35 -m 2G -smp 2 $(if $(wildcard /dev/kvm),-enable-kvm -cpu host,-cpu max) \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(OUT)/ovmf-vars.fd \
		-drive file=$(OUT)/zos.img,format=raw,if=virtio \
		-device virtio-vga -device qemu-xhci -device usb-tablet -device usb-kbd \
		-nic user,model=virtio-net-pci -audiodev none,id=snd -device intel-hda -device hda-output,audiodev=snd

clean:
	rm -rf $(OUT)/obj $(OUT)/lib $(OUT)/bin $(OUT)/apps $(OUT)/tests $(OUT)/tools $(OUT)/store $(OUT)/repo

-include $(shell find $(OUT)/obj -name '*.d' 2>/dev/null)
