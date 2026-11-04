
# The kernel the hypervisor guest smoke boots, carried on the image as
# /boot/guest-kernel.elf.  Defined before the FAT32 rule because a
# prerequisite list is expanded when the rule is read, and the rule below needs
# it as one.  riscv64 only: it is the only architecture whose vcpu slice has a
# stage-2 to load it into.
GUEST_KERNEL_ELF := $(BUILD_DIR)/kernel-nosyms.elf
ifeq ($(ARCH),riscv64)
GUEST_KERNEL_DEP := $(GUEST_KERNEL_ELF)
else
GUEST_KERNEL_DEP :=
endif

# The SECOND guest kernel: the same source built with RAMFS_USER=1, so its
# /bin/init and userland are linked into the image and a guest with no block
# device has something to exec.  Carried as /boot/guest-kernel-ramfs.elf.
#
# It is a separate BUILD_DIR because RAMFS_USER is part of BUILD_VARIANT
# (Makefile:396), not a flag that can be turned on for one file inside another
# variant's build -- the RAMFS blobs are KERNEL_OBJ entries, so they are part
# of the link.  Hence the recursive make below, the same shape
# tools/targets-build.mk uses for its second-variant board gates. Derive the
# sibling path from this invocation's BUILD_DIR so preempt, SMP, and explicit
# feature suffixes remain in sync.
#
# Only wired in when this make is already building the plain default variant.
# BUILD_DIR ends in BUILD_VARIANT and this target only adds the ramfs-user
# component after "-dev". The guard below keeps this to the ordinary dev image;
# other build modes do not need a second guest kernel:
#   BRINGUP=1           -> BUILD_VARIANT "...-bringup-ramfs-user", not "-dev-"
#   RAMFS_USER=1        -> this make IS the ramfs-user build, no second one
#   CONFIG_SLAB_DEBUG=1 -> BUILD_VARIANT gains "-slabdbg"
# Better to pack one guest kernel than to wedge every FAT32 build behind a
# second one that packs nothing.
#
# One variable per line on purpose.  This guard used to concatenate all of them
# into a single string literal, and the literal was one character short
# ("riscv64qemu-virt-riscv640" for a value that ends "...riscv64" + 0 + 0), so
# it matched nothing at all and GUEST_KERNEL_RAMFS_DEP was silently empty for
# every invocation: the ramfs guest kernel was never built by this rule and the
# only copy inside fat32.img was whatever leftover somebody had last produced by
# hand.  A guard that cannot fail loudly is worse than no guard -- write it so a
# typo shows up as a missing prerequisite on the next `make -p`.
# Keep the companion distinct even when BUILD_DIR is an explicit path that does
# not end in the default `-dev` variant suffix.  The recursive make below gets
# this exact path so command-line BUILD_DIR overrides cannot collapse both
# kernel configurations into one object directory.
GUEST_KERNEL_RAMFS_DIR := $(BUILD_DIR)-ramfs-user
GUEST_KERNEL_RAMFS_ELF := $(GUEST_KERNEL_RAMFS_DIR)/kernel-nosyms.elf
GUEST_KERNEL_RAMFS_STAMP := $(GUEST_KERNEL_RAMFS_DIR)/.ramfs-user-elf.stamp
GUEST_KERNEL_RAMFS_DEP :=
ifeq ($(ARCH)$(BOARD),riscv64qemu-virt-riscv64)
ifeq ($(BRINGUP),0)
ifeq ($(RAMFS_USER)$(CONFIG_SLAB_DEBUG),00)
GUEST_KERNEL_RAMFS_DEP := $(GUEST_KERNEL_RAMFS_STAMP)
endif
endif
endif

# FORCE, and a STAMP rather than a rule on the elf itself.  Both halves of that
# are load bearing:
#
#   * Without FORCE this target is satisfied by the mere EXISTENCE of the file,
#     and since the outer make never sees a prerequisite whose timestamp could
#     change, the elf is never relinked: edit a kernel source, rebuild, and
#     /boot/guest-kernel-ramfs.elf in fat32.img is still last week's kernel.
#     That is invisible until a gate boots it and reports a guest bug that was
#     fixed days ago.  The recursive make is itself fully incremental -- ordinary
#     object rules against its own BUILD_DIR -- so re-entering it on every
#     invocation costs one make startup and compiles nothing when the tree has
#     not moved.
#
#   * A rule on $(GUEST_KERNEL_RAMFS_ELF) would ALSO collide with the real link
#     rule further down this file.  Inside the recursive make that is running
#     with RAMFS_USER=1, BUILD_DIR is the ramfs-user directory, so
#     $(KERNEL_NOSYMS_ELF) (:234, := $(GUEST_KERNEL_ELF)) names exactly the
#     path spelled above, and the later definition would silently override this
#     one -- make prints "警告：覆盖关于目标 ... 的配方" and the real link
#     recipe stops existing for that one target.  A stamp nobody else has a rule
#     for cannot collide with anything.
#
# The stamp's mtime is set FROM the elf's, not to "now", so this rule does not
# manufacture a spurious mtime bump.  It does NOT make $(FAT32_IMG) incremental:
# that target also depends on the phony drvmod-examples (tools/driver-modules.mk
# :193,:199), so the image is rebuilt on every invocation whatever this stamp
# says.  Keeping the stamp honest still matters -- it is what makes "did the
# ramfs guest kernel change?" answerable at all, which is the question this rule
# exists to answer.
$(GUEST_KERNEL_RAMFS_STAMP): FORCE
	$(MAKE) ARCH=$(ARCH) BOARD=$(BOARD) ABI=$(ABI) BRINGUP=0 \
		RAMFS_USER=1 BUILD_DIR=$(GUEST_KERNEL_RAMFS_DIR) kernel-only
	@mkdir -p $(dir $@)
	@touch -r $(GUEST_KERNEL_RAMFS_ELF) $@

$(FAT32_IMG): $(USER_BUILD_STAMP) $(NATIVE_BUILD_STAMP) $(GUEST_KERNEL_DEP) $(GUEST_KERNEL_RAMFS_DEP)
	@echo "Building FAT32 image..."
	@$(PYTHON) tools/img.py fat32 \
		--fat32-img "$(FAT32_IMG)" --fat32-mb "$(FAT32_IMAGE_MB)" \
		--user-build-dir "$(USER_BUILD_DIR)" --tag "$(NATIVE_TAG)" --mkfs-fat "$(MKFS_FAT)" \
		--runtime-drvmod "$(RUNTIME_DRVMOD_MODULES)" \
		--driver-store "$(DRIVER_STORE_USER_PACKAGES)" \
		--libc "$(if $(wildcard user/external/musl/build-$(USER_LIBC_VARIANT)/lib/libc.so),user/external/musl/build-$(USER_LIBC_VARIANT)/lib/libc.so,)" \
		--libgcc "$(LIBGCC_S_ARCH)" \
		--protocols "$(PROTOCOLS_LINES)" \
		--guest-kernel "$(GUEST_KERNEL_ELF)" \
		--guest-kernel-ramfs "$(GUEST_KERNEL_RAMFS_ELF)" \
		--os-release 'ID=A20OS\nNAME="A20OS"\nPRETTY_NAME="A20OS"\nVERSION="0.2"\nVERSION_ID="0.2"\n' \
		--test-txt 'Hello from A20OS FAT32!\n'


$(FS_TEST_IMG): $(FAT32_IMG)
	@$(PYTHON) tools/img.py copy --src "$(FAT32_IMG)" --dst "$(FS_TEST_IMG)"

$(EXT4_IMG): $(USER_BUILD_STAMP) $(NATIVE_BUILD_STAMP)
	@echo "Building ext4 image..."
	@$(PYTHON) tools/img.py ext4 \
		--ext4-img "$(EXT4_IMG)" --ext4-mb "$(EXT4_IMAGE_MB)" \
		--ext4-staging-dir "$(EXT4_STAGING_DIR)" \
		--mkfs-ext4 "$(MKFS_EXT4)" \
		--user-build-dir "$(USER_BUILD_DIR)" \
		--protocols "$(PROTOCOLS_LINES)" \
		--os-release 'ID=A20OS\nNAME="A20OS"\nPRETTY_NAME="A20OS"\nVERSION="0.2"\nVERSION_ID="0.2"\n'

# Journalled ext4 gate image: same staging, but with an internal JBD2 journal.
# The crash-consistency gate boots this, writes, and crashes the machine at a
# chosen point in the commit sequence (a20.journal_crash=...).
$(BUILD_DIR)/ext4-journal.img: $(USER_BUILD_STAMP) $(NATIVE_BUILD_STAMP)
	@echo "Building journalled ext4 image..."
	@$(PYTHON) tools/img.py ext4-journal \
		--ext4-img "$(BUILD_DIR)/ext4-journal.img" \
		--ext4-mb "$(EXT4_IMAGE_MB)" \
		--ext4-staging-dir "$(EXT4_STAGING_DIR)" \
		--mkfs-ext4 "$(MKFS_EXT4)" \
		--user-build-dir "$(USER_BUILD_DIR)" \
		--protocols "$(PROTOCOLS_LINES)" \
		--os-release 'ID=A20OS\nNAME="A20OS"\nPRETTY_NAME="A20OS"\nVERSION="0.2"\nVERSION_ID="0.2"\n'

# Legacy-journal variant: same staging, but the journal keeps mke2fs's v1/v2
# superblock (no descriptor/commit/data checksums).  The read-only downgrade
# policy is tested against it; a writable mount of it must be refused.
$(BUILD_DIR)/ext4-journal-legacy.img: $(USER_BUILD_STAMP) $(NATIVE_BUILD_STAMP)
	@echo "Building legacy-journal ext4 image..."
	@$(PYTHON) tools/img.py ext4-journal-legacy \
		--ext4-img "$(BUILD_DIR)/ext4-journal-legacy.img" \
		--ext4-mb "$(EXT4_IMAGE_MB)" \
		--ext4-staging-dir "$(EXT4_STAGING_DIR)" \
		--mkfs-ext4 "$(MKFS_EXT4)" \
		--user-build-dir "$(USER_BUILD_DIR)" \
		--protocols "$(PROTOCOLS_LINES)" \
		--os-release 'ID=A20OS\nNAME="A20OS"\nPRETTY_NAME="A20OS"\nVERSION="0.2"\nVERSION_ID="0.2"\n'

# littlefs gate image: formatted by the host mkfs_lfs tool (links the
# vendored littlefs), carrying the fixed payload user/cmds/fs/lfs_test.c
# verifies byte-for-byte.
HOST_CC ?= gcc

$(BUILD_DIR)/mkfs_lfs: tools/mkfs_lfs.c kernel/external/littlefs/lfs.c \
		kernel/external/littlefs/lfs_util.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) tools/mkfs_lfs.c kernel/external/littlefs/lfs.c \
		kernel/external/littlefs/lfs_util.c -Ikernel/external/littlefs -o $@

$(BUILD_DIR)/lfs.img: $(BUILD_DIR)/mkfs_lfs tools/mkfs_lfs.c \
		kernel/external/littlefs/lfs.c kernel/external/littlefs/lfs_util.c
	@mkdir -p $(dir $@)
	@$(BUILD_DIR)/mkfs_lfs "$@" 4

$(KERNEL_BIN): $(KERNEL_ELF)
	$(OBJCOPY) -O binary $< $@

# Stable binary symbol names keep the rootfs overlay independent of ISA tags.
$(RAMFS_USER_BLOB_DIR)/native_futex: $(NATIVE_FUTEX_BIN)
	@mkdir -p $(dir $@)
	cp $< $@
$(RAMFS_USER_BLOB_DIR)/native_spawn: $(NATIVE_SPAWN_BIN)
	@mkdir -p $(dir $@)
	cp $< $@
$(RAMFS_USER_BLOB_DIR)/native_input:
	@mkdir -p $(dir $@)
	$(PYTHON) -c 'from pathlib import Path; Path("$@").write_bytes(bytes((i*37+11)&255 for i in range(2048)))'
$(addprefix $(RAMFS_USER_BLOB_DIR)/,native_futex.o native_spawn.o native_input.o): $(RAMFS_USER_BLOB_DIR)/%.o: $(RAMFS_USER_BLOB_DIR)/%
	cd $(RAMFS_USER_BLOB_DIR) && $(OBJCOPY) -I binary $(RAMFS_USER_OBJCOPY_$(ARCH)) \
		--rename-section .data=.rodata,alloc,load,readonly,data,contents \
		$* $(abspath $@)

# Serial-board rootfs builds only the explicitly embedded programs. Keep a
# separate userspace output root so its stamp cannot clean or invalidate the
# full development userland (nor pull fastfetch/media dependencies into a
# 64 MiB shell image).
RAMFS_USER_BUILD_DIR = user/build/$(USER_VARIANT)-ramfs
RAMFS_USER_BUILD_STAMP = $(RAMFS_USER_BUILD_DIR)/.ramfs-build-id
ifeq ($(RAMFS_NATIVE_TESTS),1)
RAMFS_USER_BUILD_DIR = $(USER_BUILD_DIR)
RAMFS_USER_BUILD_STAMP = $(USER_BUILD_STAMP)
else
$(RAMFS_USER_BUILD_STAMP): user/Makefile force_user_build | $(USER_BUILD_CHECK_DIRS)
	@$(PYTHON) tools/stamps.py user \
		--stamp "$@" --build-id "$(USER_BUILD_ID):ramfs:$(RAMFS_USER_PROGRAMS)" \
		--arch "$(ARCH)" --nommu "$(NOMMU)" \
		--user-build-dir "$(RAMFS_USER_BUILD_DIR)" --user-variant "$(USER_VARIANT)-ramfs" \
		--user-opt="$(USER_OPT)" --profile "$(PROFILE)" \
		--targets "$(RAMFS_USER_PROGRAMS)" \
		--roots "user/Makefile $(USER_BUILD_CHECK_DIRS)" \
		--skip "*/.git user/build user/external/musl/build-*"
endif

$(RAMFS_USER_BLOB_DIR)/%.o: $(RAMFS_USER_BUILD_STAMP)
	@mkdir -p $(dir $@)
	cd $(RAMFS_USER_BUILD_DIR) && $(OBJCOPY) -I binary $(RAMFS_USER_OBJCOPY_$(ARCH)) \
		--rename-section .data=.rodata,alloc,load,readonly,data,contents \
		$* $(abspath $@)

# x86_64 UEFI loader.  It is the ESP boot target rather than a GRUB image,
# because the kernel has to be handed the ACPI RSDP that GRUB 2.12 drops.
# -mabi=ms is required: UEFI passes efi_main's arguments in RCX/RDX.
# Directly bootable x86_64 UEFI disk.  Unlike pc-rescue-disk this stages
# BOOTX64.EFI rather than GRUB, because GRUB 2.12 drops the multiboot ACPI tags
# and the kernel then never learns the RSDP (see instances/vbox-x86_64.toml).
ifeq ($(RAMFS_USER),1)
$(VBOX_X86_64_IMG): $(VBOX_X86_64_EFI) tools/mk_uefi_fat_image.sh
	tools/mk_uefi_fat_image.sh $(VBOX_X86_64_EFI) $@ - BOOTX64.EFI
else
$(VBOX_X86_64_IMG): $(VBOX_X86_64_EFI) $(BUILD_DIR)/.vbox-rootfs-verified $(FAT32_IMG) tools/mk_uefi_fat_image.sh
	tools/mk_uefi_fat_image.sh $(VBOX_X86_64_EFI) $@ $(FAT32_IMG) BOOTX64.EFI
endif

.PHONY: x86_64-uefi-loader
x86_64-uefi-loader: $(VBOX_X86_64_EFI)

$(VBOX_X86_64_EFI): kernel/include/boot/framebuffer.h kernel/include/core/types.h $(KERNEL_BIN) $(KERNEL_ELF) tools/elf_load_span.py kernel/arch/x86_64/boot/uefi/x86_64_loader.c kernel/arch/x86_64/include/kernel_load.h kernel/arch/x86_64/boot/uefi/x86_64_kernel_blob.S kernel/arch/x86_64/boot/uefi/x86_64_efi.lds
	@mkdir -p $(dir $@)
	@memory_bytes="$$($(PYTHON) tools/elf_load_span.py $(KERNEL_ELF) --expected-base $(X86_KERNEL_LOAD_ADDRESS_CANONICAL))" && \
	$(CC) -Ikernel/include -mabi=ms -fpic -fshort-wchar -ffreestanding -fno-stack-protector \
		-fno-builtin -fvisibility=hidden \
		-DKERNEL_MEMORY_BYTES=$$memory_bytes \
		-DX86_KERNEL_LOAD_ADDRESS=$(X86_KERNEL_LOAD_ADDRESS_CANONICAL) \
		-c kernel/arch/x86_64/boot/uefi/x86_64_loader.c -o $(BUILD_DIR)/x86-64-uefi-loader.o
	$(CC) -fpic -ffreestanding -fno-stack-protector \
		-DKERNEL_BIN_PATH='"$(abspath $(KERNEL_BIN))"' \
		-c kernel/arch/x86_64/boot/uefi/x86_64_kernel_blob.S -o $(BUILD_DIR)/x86-64-uefi-kernel.o
	$(CC) -nostdlib -shared -Wl,-Bsymbolic -Wl,-e,efi_main \
		-Wl,-T,kernel/arch/x86_64/boot/uefi/x86_64_efi.lds \
		-o $(BUILD_DIR)/x86-64-uefi-loader.so \
		$(BUILD_DIR)/x86-64-uefi-loader.o $(BUILD_DIR)/x86-64-uefi-kernel.o
	$(OBJCOPY) -j .text -j .reloc -j .dynamic -j .data -j .kernel \
		-j .rela -j .rela.* -j .rodata -j .dynsym -j .dynstr \
		-O pei-x86-64 --subsystem efi-app \
		$(BUILD_DIR)/x86-64-uefi-loader.so $@

$(VBOX_AARCH64_EFI): $(KERNEL_BIN) $(KERNEL_ELF) tools/elf_load_span.py kernel/arch/aarch64/boot/uefi/aarch64_loader.c kernel/arch/aarch64/boot/uefi/handoff.h kernel/arch/aarch64/boot/uefi/loader_arch.h kernel/arch/aarch64/boot/uefi/aarch64_kernel_blob.S
	@mkdir -p $(dir $@)
	@memory_bytes="$$($(PYTHON) tools/elf_load_span.py $(KERNEL_ELF) --expected-base $(VBOX_AARCH64_LOAD_ADDRESS))" && \
	$(CC) -march=armv8-a -fpic -fshort-wchar -ffreestanding -fno-stack-protector \
		-DKERNEL_MEMORY_BYTES=$$memory_bytes \
		-fno-builtin -fvisibility=hidden -mno-outline-atomics \
		-DKERNEL_LOAD_ADDRESS=$(VBOX_AARCH64_LOAD_ADDRESS) \
		-c kernel/arch/aarch64/boot/uefi/aarch64_loader.c \
		-o $(BUILD_DIR)/uefi-loader.o
	$(CC) -march=armv8-a -fpic -ffreestanding \
		-DKERNEL_BIN_PATH='"$(abspath $(KERNEL_BIN))"' \
		-c kernel/arch/aarch64/boot/uefi/aarch64_kernel_blob.S -o $(BUILD_DIR)/uefi-kernel.o
	$(CC) -nostdlib -shared -Wl,-Bsymbolic -Wl,-e,efi_main \
		-Wl,-T,kernel/arch/aarch64/boot/uefi/aarch64_efi.lds \
		-o $(BUILD_DIR)/uefi-loader.so \
		$(BUILD_DIR)/uefi-loader.o $(BUILD_DIR)/uefi-kernel.o
	$(OBJCOPY) -j .text -j .reloc -j .dynamic -j .data -j .kernel \
		-j .rela -j .rela.* -j .rodata -j .dynsym -j .dynstr \
		-O pei-aarch64-little --subsystem efi-app \
		$(BUILD_DIR)/uefi-loader.so $@

# The user build stamp is intentionally refreshed by a recipe, so a user binary
# can become newer than an already-created FAT image during the same checkout.
# Verify the staged /init byte-for-byte every time a VBox image is requested;
# otherwise make's timestamp graph can leave a bootable but stale userspace in
# place after interrupted or manually-invoked sub-builds.
$(BUILD_DIR)/.vbox-rootfs-verified: force_vbox_rootfs_verify $(FAT32_IMG) $(USER_BUILD_STAMP)
	@$(PYTHON) tools/img.py verify-vbox \
		--fat32-img "$(FAT32_IMG)" \
		--user-build-dir "$(USER_BUILD_DIR)" \
		--stamp "$@" \
		--arch "$(ARCH)" --board "$(BOARD)" --abi "$(ABI)" \
		--bringup "$(BRINGUP)" --nommu "$(NOMMU)" --opt="$(OPT)"

$(VBOX_AARCH64_IMG): $(VBOX_AARCH64_EFI) $(BUILD_DIR)/.vbox-rootfs-verified tools/mk_uefi_fat_image.sh
	tools/mk_uefi_fat_image.sh $(VBOX_AARCH64_EFI) $@ $(FAT32_IMG)

$(VBOX_AARCH64_TEXT_IMG): $(VBOX_AARCH64_EFI) $(FAT32_IMG) tools/mk_uefi_fat_image.sh
	tools/mk_uefi_fat_image.sh $(VBOX_AARCH64_EFI) $@ $(FAT32_IMG)

# ---- kallsyms: two-pass link ----
# Pass 1 links the kernel without the symbol table; tools/gen_kallsyms.py
# extracts the .text symbols and emits a compact table object; pass 2
# relinks all objects plus the table.  The table lands in .rodata after
# .text, so .text symbol addresses are identical in both passes and the
# generated table stays exact.  If the configured Python is unavailable the table is
# skipped and the weak fallbacks in kernel/core/kallsyms.c keep the kernel
# linkable.
KALLSYMS_SRC      := $(BUILD_DIR)/kallsyms/kallsyms.c
KALLSYMS_OBJ      := $(BUILD_DIR)/kallsyms/kallsyms.o
KERNEL_NOSYMS_ELF := $(GUEST_KERNEL_ELF)

$(KERNEL_NOSYMS_ELF): $(KERNEL_OBJ) $(ASM_OBJ) $(RUST_KERNEL_RUNTIME_LINK) $(LDSCRIPT)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $(KERNEL_OBJ) $(ASM_OBJ) $(RUST_KERNEL_RUNTIME_LINK) $(ARCH_LIBS) -o $@

$(KALLSYMS_SRC): $(KERNEL_NOSYMS_ELF) tools/gen_kallsyms.py
	@$(PYTHON) tools/gensync.py kallsyms \
		--elf "$<" --out "$@"

$(KALLSYMS_OBJ): $(KALLSYMS_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL_ELF): $(KERNEL_OBJ) $(ASM_OBJ) $(RUST_KERNEL_RUNTIME_LINK) $(KALLSYMS_OBJ) $(KERNEL_NOSYMS_ELF) $(LDSCRIPT)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $(KERNEL_OBJ) $(ASM_OBJ) $(KALLSYMS_OBJ) $(RUST_KERNEL_RUNTIME_LINK) $(ARCH_LIBS) -o $@

ifneq ($(strip $(BSD_BOOTSTRAP_OBJECT)),)
$(KERNEL_NOSYMS_ELF) $(KERNEL_ELF): kernel/net/backends/bsd/sets.ld
endif

# Kernel objects treat Makefile as an order-only prerequisite, so a change in
# compiler flags (e.g. the CONFIG_UBSAN choice that PROFILE flips between
# development and benchmark/final builds) leaves stale objects from a previous
# invocation in place; the final link then fails with undefined
# __ubsan_handle_* references.  Track a per-build-dir signature stamp: when the
# flags change the stamp is touched and every kernel object is rebuilt with the
# new flags, so build directories can never mix objects from two flag sets.
TIMER_HEAP_TEST_CFLAGS :=
ifeq ($(WAIT_TIMER_HEAP_TEST_ARRAY_BOUNDS),1)
ifneq ($(WAIT_TIMER_HEAP_MAX),1)
$(error WAIT_TIMER_HEAP_TEST_ARRAY_BOUNDS=1 requires WAIT_TIMER_HEAP_MAX=1)
endif
TIMER_HEAP_TEST_CFLAGS := -Wno-array-bounds
endif
BUILD_FLAGS_SIG := $(CC) $(CFLAGS) $(LDFLAGS) timer_heap_test_cflags=$(TIMER_HEAP_TEST_CFLAGS)
BUILD_FLAGS_STAMP := $(BUILD_DIR)/.build-flags

$(BUILD_FLAGS_STAMP): FORCE
	@$(PYTHON) tools/stamps.py build-flags \
		--stamp "$@" \
		--build-flags-sig '$(BUILD_FLAGS_SIG)'

# The MCU trim links the generated syscall subset, not the full Native table.
# One manifest drives both the emitted table and the library feature header.
CMSIS_TRIM_DIR := $(BUILD_DIR)/generated/cmsis
$(CMSIS_TRIM_DIR)/.stamp: user/cmsis_rtos2/cmsis_rtos2.toml tools/cmsis_trim.py
	@mkdir -p $(CMSIS_TRIM_DIR)
	$(PYTHON) tools/cmsis_trim.py emit --profile mcu --out $(CMSIS_TRIM_DIR)
	@touch $@

$(CMSIS_TRIM_DIR)/syscall_table_mcu.def $(CMSIS_TRIM_DIR)/cmsis_config.h: $(CMSIS_TRIM_DIR)/.stamp
	@test -f $@

$(BUILD_DIR)/abi/native/syscall_table.o: $(CMSIS_TRIM_DIR)/.stamp

# Learner demo image: standard CMSIS-RTOS2 core + Native backend, linked as a
# static PIE against the freestanding demo runtime.  The kernel embeds it via
# demo_image.S, so a board flash/run stays a single artefact.
MCU_DEMO_SRC := \
    user/tests/liteos-demos/start.c \
    user/tests/liteos-demos/runtime.c \
    user/tests/liteos-demos/demo.c \
    user/tests/liteos-demos/interrupt.c \
    user/tests/liteos-demos/rt.c \
    user/port/hi3861_hal/src/hi_isr.c \
    user/port/hi3861_hal/src/iot_gpio.c \
    user/port/hi3861_hal/src/iot_pwm.c \
    user/cmsis_rtos2/src/os_kernel.c \
    user/cmsis_rtos2/src/os_thread.c \
    user/cmsis_rtos2/src/os_sync.c \
    user/cmsis_rtos2/src/os_timer.c \
    user/cmsis_rtos2/src/os_queue.c \
    user/cmsis_rtos2/src/backend_native.c \
    user/cmsis_rtos2/src/native_platform_a20.c \
    user/liba20rt/a20_malloc.c
MCU_DEMO_CC := $(RISCV_ELF_PREFIX)gcc
# OS_NATIVE_TICK_HZ is the CMSIS tick used for osDelay/timeouts; it must be
# the kernel's ARCH_TIMER_FREQ or the personality reports the wrong frequency.
MCU_TICK_HZ ?= 1000
MCU_DEMO_CFLAGS := -march=rv32imc_zicsr_zifencei -mabi=ilp32 \
    -mcmodel=medany -msmall-data-limit=0 -mno-relax \
    -Os -ffreestanding -fno-builtin -fno-stack-protector -fPIE \
    -ffunction-sections -fdata-sections -DOS_CFG_TIMER_THREAD_STACK=4096 \
    -DOS_NATIVE_TICK_HZ=$(MCU_TICK_HZ) \
    -Iuser/liba20rt -Iuser/cmsis_rtos2/include -Iuser/cmsis_rtos2/src \
    -Iuser/port/hi3861_hal/include -Ikernel/include/uapi/a20 -I$(CMSIS_TRIM_DIR)
MCU_DEMO_LDFLAGS := -nostdlib \
    -Wl,-pie,--no-dynamic-linker,--build-id=none,-z,max-page-size=4096 \
    -Wl,-T,user/tests/liteos-demos/link.ld

# The demo one-shot compile does not emit .d files, so list the headers it
# includes.  Without this a changed syscall number (or CMSIS header) leaves a
# stale ELF that still calls the old ABI value.
MCU_DEMO_HDRS := $(wildcard user/cmsis_rtos2/include/*.h) \
                 $(wildcard user/cmsis_rtos2/src/*.h) \
                 $(wildcard user/liba20rt/*.h) \
                 $(wildcard user/port/hi3861_hal/include/*.h) \
                 $(wildcard kernel/include/uapi/a20/*.h) \
                 user/tests/liteos-demos/link.ld

$(BUILD_DIR)/liteos-demo.elf: $(MCU_DEMO_SRC) $(MCU_DEMO_HDRS) \
        $(CMSIS_TRIM_DIR)/.stamp user/cmsis_rtos2/cmsis_rtos2.toml
	@mkdir -p $(@D)
	$(MCU_DEMO_CC) $(MCU_DEMO_CFLAGS) \
	    -include $(CMSIS_TRIM_DIR)/cmsis_config.h $(MCU_DEMO_LDFLAGS) \
	    $(MCU_DEMO_SRC) -o $@

$(BUILD_DIR)/arch/riscv32/mcu/demo_image.o: $(BUILD_DIR)/liteos-demo.elf

$(BUILD_DIR)/%.o: $(KERNEL_DIR)/%.c $(BUILD_FLAGS_STAMP) | Makefile $(BUILD_TIME_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# The one-slot park/wait diagnostic specializes both timer arrays to one item.
# GCC's -O3 range analysis warns on guarded heap-index paths in this object;
# keep the test-only suppression local to timer_heap.o, with -Werror intact for
# every other translation unit.
$(BUILD_DIR)/proc/timer_heap.o: $(KERNEL_DIR)/proc/timer_heap.c $(BUILD_FLAGS_STAMP) | Makefile $(BUILD_TIME_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(TIMER_HEAP_TEST_CFLAGS) -c $< -o $@

# Protocol sources retain their existing no-stack-protector compilation policy.
# Native owns the derived implementation; pristine lwIP stays in its submodule.
$(BUILD_DIR)/net/backends/native/protocol/src/%.o: $(KERNEL_DIR)/net/backends/native/protocol/src/%.c $(BUILD_FLAGS_STAMP) | Makefile $(BUILD_TIME_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fno-stack-protector -c $< -o $@

$(BUILD_DIR)/external/lwip-upstream/src/%.o: $(KERNEL_DIR)/external/lwip-upstream/src/%.c $(BUILD_FLAGS_STAMP) | Makefile $(BUILD_TIME_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fno-stack-protector -c $< -o $@

# littlefs (kernel/external/littlefs) — same vendored-source policy as lwIP:
# objects live under $(BUILD_DIR)/external/littlefs, no stack-canary
# hardening, and the compat include dir goes FIRST so <stdlib.h>/<string.h>
# resolve to the littlefs shims instead of the lwIP port's.
$(BUILD_DIR)/external/littlefs/%.o: $(KERNEL_DIR)/external/littlefs/%.c $(BUILD_FLAGS_STAMP) | Makefile $(BUILD_TIME_HDR)
	@mkdir -p $(dir $@)
	$(CC) -I$(KERNEL_DIR)/external/littlefs/compat $(CFLAGS) -fno-stack-protector -c $< -o $@

$(BUILD_DIR)/%.o: $(KERNEL_DIR)/%.S $(BUILD_FLAGS_STAMP) Makefile | $(BUILD_TIME_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	$(MAKE) -C user clean
	@$(PYTHON) tools/stamps.py clean \
		--find-root "$(KERNEL_DIR)" \
		--rm-rf .kernel-build \
		--rm-rf build/tmp \
		$(foreach f,kernel.elf kernel.bin fat32.img ext4.img \
			kernel-rv kernel-la disk.img disk-la.img,--rm-f $(f))

-include $(DEP_FILES)

kernel-only: $(KERNEL_BIN)
	@echo "Kernel-only build complete: $(KERNEL_BIN)"
