
$(FAT32_IMG): $(USER_BUILD_STAMP) $(NATIVE_BUILD_STAMP)
	@echo "Building FAT32 image..."
	@$(PYTHON) tools/img.py fat32 \
		--fat32-img "$(FAT32_IMG)" --fat32-mb "$(FAT32_IMAGE_MB)" \
		--user-build-dir "$(USER_BUILD_DIR)" --mkfs-fat "$(MKFS_FAT)" \
		--runtime-drvmod "$(RUNTIME_DRVMOD_MODULES)" \
		--driver-store "$(DRIVER_STORE_USER_PACKAGES)" \
		--libc "$(if $(wildcard user/external/musl/build-$(USER_VARIANT)/lib/libc.so),user/external/musl/build-$(USER_VARIANT)/lib/libc.so,)" \
		--libgcc "$(LIBGCC_S_ARCH)" \
		--protocols "$(PROTOCOLS_LINES)" \
		--os-release 'ID=A20OS\nNAME="A20OS"\nPRETTY_NAME="A20OS"\nVERSION="0.2"\nVERSION_ID="0.2"\n' \
		--test-txt 'Hello from A20OS FAT32!\n'


$(FS_TEST_IMG): $(FAT32_IMG)
	@$(PYTHON) tools/img.py copy --src "$(FAT32_IMG)" --dst "$(FS_TEST_IMG)"

.PHONY: ext4_img_only ext4_img

ext4_img_only: $(EXT4_IMG)

$(EXT4_IMG): $(USER_BUILD_STAMP) $(NATIVE_BUILD_STAMP)
	@echo "Building ext4 image..."
	@$(PYTHON) tools/img.py ext4 \
		--ext4-img "$(EXT4_IMG)" --ext4-mb "$(EXT4_IMAGE_MB)" \
		--ext4-staging-dir "$(EXT4_STAGING_DIR)" \
		--mkfs-ext4 "$(MKFS_EXT4)" \
		--user-build-dir "$(USER_BUILD_DIR)" \
		--protocols "$(PROTOCOLS_LINES)" \
		--os-release 'ID=A20OS\nNAME="A20OS"\nPRETTY_NAME="A20OS"\nVERSION="0.2"\nVERSION_ID="0.2"\n'

ext4_img: $(USER_BUILD_STAMP) ext4_img_only
	@$(PYTHON) tools/img.py copy --src "$(EXT4_IMG)" --dst "$(FS_TEST_IMG)"

$(KERNEL_BIN): $(KERNEL_ELF)
	$(OBJCOPY) -O binary $< $@

$(RAMFS_USER_BLOB_DIR)/%.o: $(USER_BUILD_STAMP)
	@mkdir -p $(dir $@)
	cd $(USER_BUILD_DIR) && $(OBJCOPY) -I binary $(RAMFS_USER_OBJCOPY_$(ARCH)) \
		--rename-section .data=.rodata,alloc,load,readonly,data,contents \
		$* $(abspath $@)

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
KERNEL_NOSYMS_ELF := $(BUILD_DIR)/kernel-nosyms.elf

$(KERNEL_NOSYMS_ELF): $(KERNEL_OBJ) $(ASM_OBJ) $(LDSCRIPT)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $(KERNEL_OBJ) $(ASM_OBJ) $(ARCH_LIBS) -o $@

$(KALLSYMS_SRC): $(KERNEL_NOSYMS_ELF) tools/gen_kallsyms.py
	@mkdir -p $(dir $@)
	@if $(PYTHON) tools/gen_kallsyms.py $< $@; then \
	    echo "  KALLSYMS $@"; \
	else \
	    echo "  KALLSYMS skipped (configured Python unavailable)"; \
	    echo '/* empty */' > $@; \
	fi

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

$(BUILD_DIR)/%.o: $(KERNEL_DIR)/%.S $(BUILD_FLAGS_STAMP) Makefile | $(BUILD_TIME_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	find $(KERNEL_DIR) -name '*.o' -delete
	rm -rf .kernel-build
	rm -f kernel.elf kernel.bin fat32.img ext4.img
	rm -f kernel-rv kernel-la disk.img disk-la.img
	$(MAKE) -C user clean
	$(MAKE) -f user/extra.mk clean 2>/dev/null || true

-include $(DEP_FILES)

kernel-only: $(KERNEL_BIN)
	@echo "Kernel-only build complete: $(KERNEL_BIN)"
