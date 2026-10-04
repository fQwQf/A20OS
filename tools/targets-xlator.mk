# ================================================================
# Foreign-architecture translation channel — build-side assets
# ================================================================
#
# The kernel side of this feature is gated at runtime (a20.xlator=1);
# this file provides the two things it needs in the image:
#
#   1. a user-space translator native to the build host
#      (qemu-<guest> from Alpine), placed in the FAT32 staging dir so
#      tools/img.py build_fat32 ships it as /bin/qemu-<guest>;
#   2. a foreign-architecture probe binary to translate.
#
# Both are opt-in through XLATOR=1 so a default build is unchanged and
# no image grows a 3.5 MB binary nobody asked for.

# Ship them.  Off by default; the smokes that exercise the channel set
# XLATOR=1 and get the assets as prerequisites of the image rather than as a
# separate `make xlator-assets` they have to remember to run first.  See the
# note at the bottom of this file for why that ordering is not cosmetic.
XLATOR ?= 0

# Guest the channel is provisioned for on this host.  Overridable so a
# board that wants, say, aarch64 guests on a riscv64 host can say so.
XLATOR_GUEST ?= x86_64

# Cross compilers, keyed on the GUEST, not on $(ARCH): $(ARCH) is the host
# we are building for, and using it here silently produced a native binary
# named for a foreign guest -- which then "passed" the translation smoke by
# running natively.  The post-build ELF check below catches that class of
# mistake, but the variable has to mean the right thing too.
#
# This map and kernel/proc/xlator_guests.def are the two halves of "which
# guest architectures exist", and check-xlator-guests fails if they disagree
# in either direction.  Adding a guest means one line here and one there --
# NOT one here only.  That is the drift this map used to have: a
# XLATOR_GUEST_CC_riscv64 line with no matching registry entry, so
# `--guest riscv64` was rejected by argparse while the make variable
# suggested otherwise.  Nothing was lost by removing it, because the guest
# was never translatable; put it back when a riscv64 guest is actually
# registered and tested.
XLATOR_GUEST_CC_x86_64   := x86_64-linux-gnu-gcc
XLATOR_GUEST_CC_aarch64  := aarch64-linux-gnu-gcc
XLATOR_GUEST_CC := $(XLATOR_GUEST_CC_$(XLATOR_GUEST))

# The guest probe: same source as the native xlate_probe, cross-built
# static so it needs no loader or sysroot under the translator.
XLATOR_PROBE_SRC := user/cmds/core/xlate_probe.c
XLATOR_PROBE     := $(USER_BUILD_DIR)/xlate_probe-$(XLATOR_GUEST)
XLATOR_TRANSLATOR := $(USER_BUILD_DIR)/qemu-$(XLATOR_GUEST)

$(XLATOR_PROBE): $(XLATOR_PROBE_SRC) tools/targets-xlator.mk tools/xlator_fetch.py
	@mkdir -p $(dir $@)
	@if [ -z "$(XLATOR_GUEST_CC)" ] || ! command -v $(XLATOR_GUEST_CC) >/dev/null 2>&1; then \
		echo "xlate-probe: no $(XLATOR_GUEST) cross compiler (tried '$(XLATOR_GUEST_CC)')" >&2; \
		echo "xlate-probe: install one, or set XLATOR_GUEST_CC_$(XLATOR_GUEST)=<cc>" >&2; \
		exit 1; \
	fi
	$(XLATOR_GUEST_CC) -static -O2 -Wall -Wextra -o $@ $<
	@$(PYTHON) tools/xlator_fetch.py --check-guest --path $@ --guest $(XLATOR_GUEST)
	@echo "xlate-probe: built $@ (verified $(XLATOR_GUEST))"

$(XLATOR_TRANSLATOR): tools/xlator_fetch.py
	$(PYTHON) tools/xlator_fetch.py \
		--host-arch "$(ARCH)" --guest "$(XLATOR_GUEST)" \
		--dest-dir "$(USER_BUILD_DIR)"

# Everything the translation smoke needs in one dependency.
xlator-assets: $(XLATOR_PROBE) $(XLATOR_TRANSLATOR)

xlator-assets-riscv64:
	$(MAKE) ARCH=riscv64 NOMMU=$(NOMMU) XLATOR_GUEST=$(XLATOR_GUEST) xlator-assets

xlator-assets-aarch64:
	$(MAKE) ARCH=aarch64 NOMMU=$(NOMMU) XLATOR_GUEST=$(XLATOR_GUEST) xlator-assets

xlator-assets-loongarch64:
	$(MAKE) ARCH=loongarch64 NOMMU=$(NOMMU) XLATOR_GUEST=$(XLATOR_GUEST) xlator-assets

xlator-assets-x86_64:
	$(MAKE) ARCH=x86_64 NOMMU=$(NOMMU) XLATOR_GUEST=$(XLATOR_GUEST) xlator-assets

.PHONY: xlator-assets xlator-assets-riscv64 xlator-assets-aarch64 \
	xlator-assets-loongarch64 xlator-assets-x86_64

# XLATOR=1 wires the assets into the image.  This is a dependency rather than
# a `pre` step in tools/smoke_cases.py because of an ordering that is easy to
# get wrong: the assets land in $(USER_BUILD_DIR), and that directory is
# wiped by `make -C user clean` whenever USER_BUILD_STAMP decides the userspace
# build id changed.  A pre step that builds them first therefore loses them
# again one make invocation later, and the smoke fails with a missing
# /bin/xlate_probe-<guest> that looks like a kernel problem and is not.
#
# As a prerequisite of $(FAT32_IMG) the order is fixed by make: userspace
# build stamp first (it may clean), then the assets, then the image.
ifeq ($(XLATOR),1)
$(FAT32_IMG): $(XLATOR_PROBE) $(XLATOR_TRANSLATOR)
endif
