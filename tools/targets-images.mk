
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

$(FAT32_IMG): $(USER_BUILD_STAMP) $(NATIVE_BUILD_STAMP) $(GUEST_KERNEL_DEP)
	@echo "Building FAT32 image..."
	@$(PYTHON) tools/img.py fat32 \
		--fat32-img "$(FAT32_IMG)" --fat32-mb "$(FAT32_IMAGE_MB)" \
		--user-build-dir "$(USER_BUILD_DIR)" --mkfs-fat "$(MKFS_FAT)" \
		--runtime-drvmod "$(RUNTIME_DRVMOD_MODULES)" \
		--driver-store "$(DRIVER_STORE_USER_PACKAGES)" \
		--libc "$(if $(wildcard user/external/musl/build-$(USER_VARIANT)/lib/libc.so),user/external/musl/build-$(USER_VARIANT)/lib/libc.so,)" \
		--libgcc "$(LIBGCC_S_ARCH)" \
		--protocols "$(PROTOCOLS_LINES)" \
		--guest-kernel "$(GUEST_KERNEL_ELF)" \
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

$(RAMFS_USER_BLOB_DIR)/%.o: $(USER_BUILD_STAMP)
	@mkdir -p $(dir $@)
	cd $(USER_BUILD_DIR) && $(OBJCOPY) -I binary $(RAMFS_USER_OBJCOPY_$(ARCH)) \
		--rename-section .data=.rodata,alloc,load,readonly,data,contents \
		$* $(abspath $@)

# x86_64 UEFI loader.  It is the ESP boot target rather than a GRUB image,
# because the kernel has to be handed the ACPI RSDP that GRUB 2.12 drops.
# -mabi=ms is required: UEFI passes efi_main's arguments in RCX/RDX.
# Directly bootable x86_64 UEFI disk.  Unlike pc-rescue-disk this stages
# BOOTX64.EFI rather than GRUB, because GRUB 2.12 drops the multiboot ACPI tags
# and the kernel then never learns the RSDP (see instances/vbox-x86_64.toml).
$(VBOX_X86_64_IMG): $(VBOX_X86_64_EFI) $(BUILD_DIR)/.vbox-rootfs-verified $(FAT32_IMG) tools/mk_uefi_fat_image.sh
	tools/mk_uefi_fat_image.sh $(VBOX_X86_64_EFI) $@ $(FAT32_IMG) BOOTX64.EFI

$(VBOX_X86_64_EFI): $(KERNEL_BIN) kernel/boot/uefi/x86_64_loader.c kernel/boot/uefi/x86_64_kernel_blob.S kernel/boot/uefi/x86_64_efi.lds
	@mkdir -p $(dir $@)
	$(CC) -mabi=ms -fpic -fshort-wchar -ffreestanding -fno-stack-protector \
		-fno-builtin -fvisibility=hidden \
		-c kernel/boot/uefi/x86_64_loader.c -o $(BUILD_DIR)/x86-64-uefi-loader.o
	$(CC) -fpic -ffreestanding -fno-stack-protector \
		-DKERNEL_BIN_PATH='"$(abspath $(KERNEL_BIN))"' \
		-c kernel/boot/uefi/x86_64_kernel_blob.S -o $(BUILD_DIR)/x86-64-uefi-kernel.o
	$(CC) -nostdlib -shared -Wl,-Bsymbolic -Wl,-e,efi_main \
		-Wl,-T,kernel/boot/uefi/x86_64_efi.lds \
		-o $(BUILD_DIR)/x86-64-uefi-loader.so \
		$(BUILD_DIR)/x86-64-uefi-loader.o $(BUILD_DIR)/x86-64-uefi-kernel.o
	$(OBJCOPY) -j .text -j .reloc -j .dynamic -j .data -j .kernel \
		-j .rela -j .rela.* -j .rodata -j .dynsym -j .dynstr \
		-O pei-x86-64 --subsystem efi-app \
		$(BUILD_DIR)/x86-64-uefi-loader.so $@

$(VBOX_AARCH64_EFI): $(KERNEL_BIN) kernel/boot/uefi/aarch64_loader.c kernel/boot/uefi/aarch64_kernel_blob.S
	@mkdir -p $(dir $@)
	$(CC) -march=armv8-a -fpic -fshort-wchar -ffreestanding -fno-stack-protector \
		-fno-builtin -fvisibility=hidden -mno-outline-atomics \
		-DKERNEL_LOAD_ADDRESS=$(VBOX_AARCH64_LOAD_ADDRESS) \
		-c kernel/boot/uefi/aarch64_loader.c \
		-o $(BUILD_DIR)/uefi-loader.o
	$(CC) -march=armv8-a -fpic -ffreestanding \
		-DKERNEL_BIN_PATH='"$(abspath $(KERNEL_BIN))"' \
		-c kernel/boot/uefi/aarch64_kernel_blob.S -o $(BUILD_DIR)/uefi-kernel.o
	$(CC) -nostdlib -shared -Wl,-Bsymbolic -Wl,-e,efi_main \
		-Wl,-T,kernel/boot/uefi/aarch64_efi.lds \
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

$(KERNEL_NOSYMS_ELF): $(KERNEL_OBJ) $(ASM_OBJ) $(LDSCRIPT)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $(KERNEL_OBJ) $(ASM_OBJ) $(ARCH_LIBS) -o $@

$(KALLSYMS_SRC): $(KERNEL_NOSYMS_ELF) tools/gen_kallsyms.py
	@$(PYTHON) tools/gensync.py kallsyms \
		--elf "$<" --out "$@"

$(KALLSYMS_OBJ): $(KALLSYMS_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL_ELF): $(KERNEL_OBJ) $(ASM_OBJ) $(KALLSYMS_OBJ) $(KERNEL_NOSYMS_ELF) $(LDSCRIPT)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $(KERNEL_OBJ) $(ASM_OBJ) $(KALLSYMS_OBJ) $(ARCH_LIBS) -o $@

# Kernel objects treat Makefile as an order-only prerequisite, so a change in
# compiler flags (e.g. the CONFIG_UBSAN choice that PROFILE flips between
# development and benchmark/final builds) leaves stale objects from a previous
# invocation in place; the final link then fails with undefined
# __ubsan_handle_* references.  Track a per-build-dir signature stamp: when the
# flags change the stamp is touched and every kernel object is rebuilt with the
# new flags, so build directories can never mix objects from two flag sets.
BUILD_FLAGS_SIG := $(CC) $(CFLAGS) $(LDFLAGS)
BUILD_FLAGS_STAMP := $(BUILD_DIR)/.build-flags

$(BUILD_FLAGS_STAMP): FORCE
	@$(PYTHON) tools/stamps.py build-flags \
		--stamp "$@" \
		--build-flags-sig '$(BUILD_FLAGS_SIG)'

$(BUILD_DIR)/%.o: $(KERNEL_DIR)/%.c $(BUILD_FLAGS_STAMP) | Makefile $(BUILD_TIME_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# lwIP lives under the kernel tree (kernel/external/lwip); the kernel compiles
# the shared sources, objects land under $(BUILD_DIR)/external/lwip as before.
# vendored 代码不参与内核栈金丝雀加固，保持其编译行为不变。
$(BUILD_DIR)/external/lwip/src/%.o: $(KERNEL_DIR)/external/lwip/src/%.c $(BUILD_FLAGS_STAMP) | Makefile $(BUILD_TIME_HDR)
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
	@$(PYTHON) tools/stamps.py clean \
		--find-root "$(KERNEL_DIR)" \
		--rm-rf .kernel-build \
		$(foreach f,kernel.elf kernel.bin fat32.img ext4.img \
			kernel-rv kernel-la disk.img disk-la.img,--rm-f $(f))
	$(MAKE) -C user clean

-include $(DEP_FILES)

kernel-only: $(KERNEL_BIN)
	@echo "Kernel-only build complete: $(KERNEL_BIN)"
