# Zurbax build.
#
#   make            build every library, system program and app into build/
#   make test       build and run the test suites
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
           -Ilibzr/include -Ilibzg/include -Ilibzg/src -Ilibzui/include
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
LINK_LIBS := $(foreach l,zui zg zr,$(if $(filter $(l),$(LIBNAMES)),$(OUT)/lib/lib$(l).a))

# ---- Programs: system/<name>/ and apps/<name>/ --------------------------------
SYSTEM := $(notdir $(wildcard system/*))
APPS   := $(notdir $(wildcard apps/*))

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

# ---- Tests -----------------------------------------------------------------------
TESTS := $(patsubst tests/%.c,$(OUT)/tests/%,$(wildcard tests/*.c))
$(OUT)/tests/%: $(OUT)/obj/tests/%.o $(LINK_LIBS)
	@mkdir -p $(dir $@)
	@echo "  LD    $@"
	@$(CC) $(LDFLAGS) -o $@ $< $(LINK_LIBS) $(LDLIBS)

.PHONY: all test clean devroot
.SECONDARY:
all: $(foreach l,$(LIBNAMES),$(OUT)/lib/lib$(l).a) $(ALL_PROGRAMS)

# A development root that mirrors /system, so programs find their data files.
DEVROOT := $(OUT)/root/system
devroot:
	@mkdir -p $(DEVROOT)/share
	@ln -sfn $(CURDIR)/data/icons $(DEVROOT)/share/icons

test: $(TESTS) devroot
	@for t in $(TESTS); do echo "== $$t"; ZURBAX_ROOT=$(DEVROOT) $$t || exit 1; done

clean:
	rm -rf $(OUT)/obj $(OUT)/lib $(OUT)/bin $(OUT)/apps $(OUT)/tests

-include $(shell find $(OUT)/obj -name '*.d' 2>/dev/null)
