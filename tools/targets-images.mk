
$(FAT32_IMG): $(USER_BUILD_STAMP) $(NATIVE_BUILD_STAMP) \
		user/contest_init/contest.sh \
		user/contest_init/final_contest.sh \
		user/contest_init/buildstorm_probe.sh \
		user/contest_init/run_ltp_resume.sh \
		user/contest_init/ltp_blacklist.txt
	@echo "Building FAT32 image..."
	@mkdir -p $(BUILD_DIR)
	dd if=/dev/zero of=$(FAT32_IMG) bs=1048576 count=$(FAT32_IMAGE_MB)
	$(MKFS_FAT) -F 32 $(FAT32_IMG)
	@set -e; \
	for f in $(USER_BUILD_DIR)/*; do \
		[ -f "$$f" ] || continue; \
		name=$$(basename "$$f"); \
		mcopy -i $(FAT32_IMG) "$$f" "::/$$name"; \
	done
	mcopy -o -i $(FAT32_IMG) $(USER_BUILD_DIR)/mksh ::/sh
	mcopy -o -i $(FAT32_IMG) $(USER_BUILD_DIR)/mksh ::/bash
	-mmd -i $(FAT32_IMG) ::/etc >/dev/null 2>&1
	-mmd -i $(FAT32_IMG) ::/lib >/dev/null 2>&1
	-mmd -i $(FAT32_IMG) ::/lib/drivers >/dev/null 2>&1
	@for m in $(RUNTIME_DRVMOD_MODULES); do \
		mcopy -o -i $(FAT32_IMG) $(USER_BUILD_DIR)/$$m ::/lib/drivers/$$m; \
	done
	@for u in $(DRIVER_STORE_USER_PACKAGES); do \
		mcopy -o -i $(FAT32_IMG) $(USER_BUILD_DIR)/$$u ::/lib/drivers/$$u; \
	done
	-mmd -i $(FAT32_IMG) ::/musl >/dev/null 2>&1
	-mmd -i $(FAT32_IMG) ::/musl/lib >/dev/null 2>&1
	@[ -f user/external/musl/build-$(USER_VARIANT)/lib/libc.so ] && \
		mcopy -o -i $(FAT32_IMG) user/external/musl/build-$(USER_VARIANT)/lib/libc.so ::/musl/lib/libc.so || true
	@[ -n "$(LIBGCC_S_ARCH)" ] && [ -f "$(LIBGCC_S_ARCH)" ] && \
		mcopy -o -i $(FAT32_IMG) "$(LIBGCC_S_ARCH)" ::/lib/libgcc_s.so.1 || true
	@printf '%s\n' $(PROTOCOLS_LINES) | mcopy -o -i $(FAT32_IMG) - ::/etc/protocols
	@printf 'ID=A20OS\nNAME="A20OS"\nPRETTY_NAME="A20OS"\nVERSION="0.2"\nVERSION_ID="0.2"\n' | mcopy -o -i $(FAT32_IMG) - ::/etc/os-release
	@printf 'Hello from A20OS FAT32!\n' | mcopy -i $(FAT32_IMG) - ::/test.txt
	mcopy -o -i $(FAT32_IMG) user/contest_init/contest.sh ::/contest.sh
	mcopy -o -i $(FAT32_IMG) user/contest_init/final_contest.sh ::/final_contest.sh
	mcopy -o -i $(FAT32_IMG) user/contest_init/buildstorm_probe.sh ::/buildstorm_probe.sh
	mcopy -o -i $(FAT32_IMG) user/contest_init/run_ltp_resume.sh ::/run_ltp_resume.sh
	mcopy -o -i $(FAT32_IMG) user/contest_init/ltp_blacklist.txt ::/etc/ltp_blacklist.txt

# Keep GUI state out of fat32.img so a later text-mode run does not inherit it.
# init uses this marker to replace the serial shell with the LVGL desktop.
$(WAYLAND_PLAYER_STAMP): user/wayland/build.sh user/wayland/player.c \
		user/wayland/desktop-shell.c user/wayland/input-method.c \
		user/wayland/stub/udev.c user/wayland/stub/mtdev.c \
		user/cmds/wayland-session.c \
		$(WAYLAND_WESTON_PATCH) \
		user/external/ffmpeg/configure kernel/include/uapi/a20/audio.h
	@if [ ! -f $(WAYLAND_FFMPEG_STAMP) ] || \
		[ user/wayland/build.sh -nt $(WAYLAND_FFMPEG_STAMP) ] || \
		[ user/external/ffmpeg/configure -nt $(WAYLAND_FFMPEG_STAMP) ]; then \
		rm -f $(WAYLAND_FFMPEG_STAMP); \
	fi
	@if [ ! -f $(WAYLAND_STUBS_STAMP) ] || \
		[ user/wayland/stub/udev.c -nt $(WAYLAND_STUBS_STAMP) ] || \
		[ user/wayland/stub/mtdev.c -nt $(WAYLAND_STUBS_STAMP) ]; then \
		rm -f $(WAYLAND_STUBS_STAMP); \
	fi
	@if [ ! -f $(WAYLAND_WESTON_STAMP) ] || \
		[ user/wayland/build.sh -nt $(WAYLAND_WESTON_STAMP) ] || \
		[ $(WAYLAND_WESTON_PATCH) -nt $(WAYLAND_WESTON_STAMP) ]; then \
		rm -f $(WAYLAND_WESTON_STAMP); \
	fi
	@rm -f $(WAYLAND_PLAYER_STAMP)
	user/wayland/build.sh $(ARCH)

$(GUI_MEDIA_STAMP): FORCE
	@mkdir -p $(dir $@)
	@set -e; \
	tmp="$@.tmp.$$$$"; \
	trap 'rm -f "$$tmp"' EXIT INT TERM; \
	printf '%s\n' '$(GUI_MEDIA)' > "$$tmp"; \
	if [ -f "$@" ] && cmp -s "$$tmp" "$@"; then \
		rm -f "$$tmp"; \
	else \
		mv -f "$$tmp" "$@"; \
	fi; \
	trap - EXIT INT TERM

$(GUI_FAT32_IMG): $(FAT32_IMG) $(GUI_WAYLAND_DEPS) $(GUI_MEDIA_STAMP)
	@set -e; \
	lock="$(GUI_FAT32_IMG).lock"; \
	tmp="$(GUI_FAT32_IMG).tmp.$$$$"; \
	exec 9>"$$lock"; \
	flock 9; \
	trap 'rm -f "$$tmp"' EXIT INT TERM; \
	cp "$(FAT32_IMG)" "$$tmp"; \
	printf '1\n' | mcopy -o -i "$$tmp" - ::/etc/a20-gui; \
	printf '1\n' | mcopy -o -i "$$tmp" - ::/a20-gui; \
	if [ "$(WAYLAND_GUI)" = 1 ]; then \
		user/wayland/install-image.sh "$$tmp" $(ARCH) "$(GUI_MEDIA)"; \
	fi; \
	mv -f "$$tmp" "$(GUI_FAT32_IMG)"; \
	trap - EXIT INT TERM

$(FS_TEST_IMG): $(FAT32_IMG)
	cp $(FAT32_IMG) $(FS_TEST_IMG)

ext4_img_only: $(EXT4_IMG)

$(EXT4_IMG): $(USER_BUILD_STAMP) $(NATIVE_BUILD_STAMP)
	@echo "Building ext4 image..."
	@rm -rf $(EXT4_STAGING_DIR) && mkdir -p $(EXT4_STAGING_DIR)
	@set -e; \
	for f in $(USER_BUILD_DIR)/*; do \
		[ -f "$$f" ] || continue; \
		cp "$$f" "$(EXT4_STAGING_DIR)/$$(basename "$$f")"; \
	done
	cp $(USER_BUILD_DIR)/mksh $(EXT4_STAGING_DIR)/sh
	cp $(USER_BUILD_DIR)/mksh $(EXT4_STAGING_DIR)/bash
	printf 'Hello from ext4!\nThis file is on the ext4 filesystem.\n' > $(EXT4_STAGING_DIR)/test.txt
	@mkdir -p $(EXT4_STAGING_DIR)/etc
	@printf '%s\n' $(PROTOCOLS_LINES) > $(EXT4_STAGING_DIR)/etc/protocols
	@printf 'ID=A20OS\nNAME="A20OS"\nPRETTY_NAME="A20OS"\nVERSION="0.2"\nVERSION_ID="0.2"\n' > $(EXT4_STAGING_DIR)/etc/os-release
	@mkdir -p $(BUILD_DIR)
	dd if=/dev/zero of=$(EXT4_IMG) bs=1048576 count=$(EXT4_IMAGE_MB)
	$(MKFS_EXT4) -F -O ^has_journal,extent,huge_file,flex_bg,uninit_bg,dir_index -d $(EXT4_STAGING_DIR) $(EXT4_IMG)
	@rm -rf $(EXT4_STAGING_DIR)

ext4_img: $(USER_BUILD_STAMP) ext4_img_only
	cp $(EXT4_IMG) $(FS_TEST_IMG)

$(KERNEL_BIN): $(KERNEL_ELF)
	$(OBJCOPY) -O binary $< $@

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
$(BUILD_DIR)/.vbox-rootfs-verified: force_vbox_rootfs_verify $(GUI_FAT32_IMG) $(USER_BUILD_STAMP)
	@set -e; \
	tmp=$$(mktemp); \
	trap 'rm -f "$$tmp"' EXIT HUP INT TERM; \
	mcopy -i $(GUI_FAT32_IMG) ::/init "$$tmp"; \
	cmp -s "$$tmp" "$(USER_BUILD_DIR)/init" || { \
		echo "[VBOX] stale /init detected; rebuilding GUI root filesystem"; \
		rm -f $(FAT32_IMG) $(GUI_FAT32_IMG); \
		$(MAKE) ARCH=$(ARCH) BOARD=$(BOARD) ABI=$(ABI) BRINGUP=$(BRINGUP) \
			NOMMU=$(NOMMU) OPT="$(OPT)" $(GUI_FAT32_IMG); \
		mcopy -i $(GUI_FAT32_IMG) ::/init "$$tmp"; \
		cmp -s "$$tmp" "$(USER_BUILD_DIR)/init"; \
	}; \
	touch $@

$(VBOX_AARCH64_IMG): $(VBOX_AARCH64_EFI) $(BUILD_DIR)/.vbox-rootfs-verified tools/mk_uefi_fat_image.sh
	tools/mk_uefi_fat_image.sh $(VBOX_AARCH64_EFI) $@ $(GUI_FAT32_IMG)

$(VBOX_AARCH64_TEXT_IMG): $(VBOX_AARCH64_EFI) $(FAT32_IMG) tools/mk_uefi_fat_image.sh
	tools/mk_uefi_fat_image.sh $(VBOX_AARCH64_EFI) $@ $(FAT32_IMG)

# ---- kallsyms: two-pass link ----
# Pass 1 links the kernel without the symbol table; tools/gen_kallsyms.py
# extracts the .text symbols and emits a compact table object; pass 2
# relinks all objects plus the table.  The table lands in .rodata after
# .text, so .text symbol addresses are identical in both passes and the
# generated table stays exact.  If python3 is unavailable the table is
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
	    echo "  KALLSYMS skipped (python3 unavailable)"; \
	    echo '/* empty */' > $@; \
	fi

$(KALLSYMS_OBJ): $(KALLSYMS_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL_ELF): $(KERNEL_OBJ) $(ASM_OBJ) $(KALLSYMS_OBJ) $(KERNEL_NOSYMS_ELF) $(LDSCRIPT)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $(KERNEL_OBJ) $(ASM_OBJ) $(KALLSYMS_OBJ) $(ARCH_LIBS) -o $@

$(BUILD_DIR)/%.o: $(KERNEL_DIR)/%.c | Makefile $(BUILD_TIME_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# lwIP lives under the kernel tree (kernel/external/lwip); the kernel compiles
# the shared sources, objects land under $(BUILD_DIR)/external/lwip as before.
$(BUILD_DIR)/external/lwip/src/%.o: $(KERNEL_DIR)/external/lwip/src/%.c | Makefile $(BUILD_TIME_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(KERNEL_DIR)/%.S Makefile | $(BUILD_TIME_HDR)
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

