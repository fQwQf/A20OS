# ================================================================
# A20OS package system (apk) —— 包构建、仓库与镜像组装
# ================================================================
# 文档：docs/packaging/overview.md
#
#   make pkgs          # 将构建产物打成 .apk（默认 a20-base a20-drivers a20-kernel）
#   make pkg-repo      # pkgs + 建立本地仓库 build/repo/<arch>（签名索引）
#   make image-world   # pkg-repo + 按 world 清单组装 rootfs 镜像
#   make pkg-key       # 生成本地开发签名密钥（build/keys/，不提交）
#
# 常用变量：
#   PKG_RECIPES   要打包的 recipe 名列表（默认核心三件；extra 包见下）
#   PKG_WORLD     packages/world/ 下的清单名（默认 base）
#   PKG_SIZE_MB   镜像大小（默认 512；桌面 world 见 PKG_SIZE_MB_GUI）
#   PKG_ALPINE    image-world 是否引入 Alpine 仓库（默认 1；纯本地组合设 0）
#   PKG_SIGN_KEY  签名私钥（默认 build/keys/a20os-dev.rsa，自动生成）
#
# extra 包（vim/git 等）先用旧流程构建，再打包：
#   make ARCH=riscv64 extra-user-apps EXTRA_PACKAGES=vim
#   make ARCH=riscv64 pkgs PKG_RECIPES=a20-extra-vim

PKG_ARCH      ?= $(ARCH)
PKG_VARIANT   ?= $(USER_VARIANT)
PKG_KEYS_DIR  ?= build/keys
PKG_SIGN_KEY  ?= $(PKG_KEYS_DIR)/a20os-dev.rsa
PKG_KEY_NAME  ?= a20os-dev.rsa.pub
PKG_OUT_DIR   := build/packages/$(PKG_ARCH)
PKG_REPO_DIR  ?= build/repo
PKG_IMAGE_DIR ?= build/images
PKG_RECIPES   ?= a20-base a20-drivers a20-kernel
PKG_WORLD     ?= base
PKG_SIZE_MB_DEFAULT ?= 512
PKG_SIZE_MB   ?= $(PKG_SIZE_MB_DEFAULT)
PKG_ALPINE    ?= 1

# 桌面 world（xfce 等）解包后 >1 GiB，通用默认 512 MiB 装不下（mkfs.ext4 会以
# "Could not allocate block in ext2 filesystem" 失败）。GUI 启动入口在调用方
# 没有显式给出大小时，把「还是默认值」这一情况提升成 PKG_SIZE_MB_GUI。
PKG_SIZE_MB_GUI   ?= 4096
PKG_SIZE_MB_WORLD := $(if $(filter $(PKG_SIZE_MB_DEFAULT),$(PKG_SIZE_MB)),$(PKG_SIZE_MB_GUI),$(PKG_SIZE_MB))

# 可选：往 world 镜像里注入本地媒体文件（演示视频等）。GUI_MEDIA 可写空格
# 分隔的多个文件或目录，全部落到镜像内 $(GUI_MEDIA_DIR)/。
#   make run-gui-x86_64 GUI_MEDIA=/path/to/video.mp4
#   make run-gui-x86_64 GUI_MEDIA="/a.mp4 /b.mp4 clips/"
# 桌面里用 parole / mpv 打开，或在 Thunar 里双击即可播放。
GUI_MEDIA          ?=
GUI_MEDIA_DIR      ?= /usr/share/a20-media
GUI_MEDIA_OVERLAY  := build/overlay-media/$(PKG_WORLD)-$(PKG_ARCH)

.PHONY: pkg-key pkgs pkgs-check pkg-repo image-world run-world run-world-gui

# 本地开发签名密钥：不入库，仅用于本机/CI 内的签名验证闭环。
# 正式发布密钥由 CI secret 注入（见 docs/packaging/repository.md）。
pkg-key:
	@mkdir -p $(PKG_KEYS_DIR)
	@if [ ! -f "$(PKG_SIGN_KEY)" ]; then \
		openssl genrsa -out "$(PKG_SIGN_KEY)" 2048 2>/dev/null; \
		openssl rsa -in "$(PKG_SIGN_KEY)" -pubout \
			-out "$(PKG_KEYS_DIR)/$(PKG_KEY_NAME)" 2>/dev/null; \
		echo "[PKG] generated development signing key: $(PKG_SIGN_KEY)"; \
	fi

# 只校验 recipe 与产物路径，不写包（CI 快速门禁用）。
pkgs-check:
	@set -e; for name in $(PKG_RECIPES); do \
		recipe="packages/recipes/$$name.toml"; \
		[ -f "$$recipe" ] || { echo "[PKG] unknown recipe: $$name"; exit 1; }; \
		$(PYTHON) tools/mka20pkg.py "$$recipe" --arch $(PKG_ARCH) \
			--variant $(PKG_VARIANT) --kernel-build-dir $(BUILD_DIR) --check; \
	done

pkgs: $(USER_BUILD_STAMP) $(KERNEL_ELF) $(if $(PKG_SIGN_KEY),pkg-key,)
	@set -e; for name in $(PKG_RECIPES); do \
		recipe="packages/recipes/$$name.toml"; \
		[ -f "$$recipe" ] || { echo "[PKG] unknown recipe: $$name"; exit 1; }; \
		$(PYTHON) tools/mka20pkg.py "$$recipe" --arch $(PKG_ARCH) \
			--variant $(PKG_VARIANT) --kernel-build-dir $(BUILD_DIR) \
			$(if $(PKG_SIGN_KEY),--sign-key $(abspath $(PKG_SIGN_KEY)) --key-name $(PKG_KEY_NAME),) \
			-o $(PKG_OUT_DIR); \
	done

pkg-repo: pkgs
	@mkdir -p $(PKG_REPO_DIR)/$(PKG_ARCH)
	@cp -f $(PKG_OUT_DIR)/*.apk $(PKG_REPO_DIR)/$(PKG_ARCH)/
	tools/mka20repo.sh $(if $(PKG_SIGN_KEY),--sign-key $(PKG_SIGN_KEY) --key-name $(PKG_KEY_NAME),) \
		$(PKG_REPO_DIR)/$(PKG_ARCH)

# 生成只含媒体文件的临时 overlay，由 mkrootfs 的 --overlay 机制合入镜像。
.PHONY: pkg-media-overlay
pkg-media-overlay:
	@set -e; \
	rm -rf "$(GUI_MEDIA_OVERLAY)"; \
	mkdir -p "$(GUI_MEDIA_OVERLAY)$(GUI_MEDIA_DIR)" "$(GUI_MEDIA_OVERLAY)/root/Desktop"; \
	for m in $(GUI_MEDIA); do \
		[ -e "$$m" ] || { echo "[PKG] GUI_MEDIA not found: $$m" >&2; exit 1; }; \
		cp -a "$$m" "$(GUI_MEDIA_OVERLAY)$(GUI_MEDIA_DIR)/"; \
		echo "[PKG] media: $$m -> $(GUI_MEDIA_DIR)/$$(basename "$$m")"; \
	done; \
	ln -sfn "$(GUI_MEDIA_DIR)" "$(GUI_MEDIA_OVERLAY)/root/Desktop/a20-media"

image-world: pkg-repo $(if $(GUI_MEDIA),pkg-media-overlay)
	$(PYTHON) tools/mkrootfs.py --arch $(PKG_ARCH) \
		--world packages/world/$(PKG_WORLD).world \
		--repo $(abspath $(PKG_REPO_DIR)) \
		$(if $(wildcard packages/overlay/$(PKG_WORLD)),--overlay packages/overlay/$(PKG_WORLD),) \
		$(if $(GUI_MEDIA),--overlay $(abspath $(GUI_MEDIA_OVERLAY)),) \
		$(if $(PKG_SIGN_KEY),--keys-dir $(PKG_KEYS_DIR),--allow-untrusted) \
		$(if $(filter 0,$(PKG_ALPINE)),--no-alpine,) \
		$(if $(filter-out 0,$(shell id -u)),--usermode,) \
		--output $(PKG_IMAGE_DIR)/$(PKG_WORLD)-$(PKG_ARCH).img \
		--size-mb $(PKG_SIZE_MB)

# GUI variant for desktop worlds (xfce, ...): same second-disk distro boot,
# but with the virtio-gpu display stack and audio.  The image is built here (in
# its own make invocation) rather than as a prerequisite so that the desktop
# size default above actually applies.
run-world-gui: $(FAT32_IMG)
	$(MAKE) ARCH=$(ARCH) PKG_WORLD=$(PKG_WORLD) GUI_MEDIA="$(GUI_MEDIA)" \
		PKG_SIZE_MB=$(PKG_SIZE_MB_WORLD) image-world
	$(QEMU) $(patsubst -nographic,-display $(QEMU_GUI_DISPLAY) $(QEMU_GUI_DEVICES) $(QEMU_GUI_AUDIO) -serial stdio,$(QEMU_FLAGS_NO_SDCARD)) \
		-drive file=$(abspath $(PKG_IMAGE_DIR)/$(PKG_WORLD)-$(PKG_ARCH).img),if=none,format=raw,id=xworld \
		-device $(QEMU_BLK_SECOND),drive=xworld \
		-kernel $(KERNEL_ELF)

# 传统入口：以 GUI + 音频启动 XFCE 桌面实例（等价 tools/a20 run xfce-<arch>）。
# GUI_MEDIA 会在启动前注入镜像，例如：
#   make run-gui-x86_64 GUI_MEDIA=~/Videos/demo.mp4
.PHONY: run-gui run-gui-riscv64 run-gui-aarch64 run-gui-x86_64 run-gui-loongarch64
run-gui-riscv64:
	$(MAKE) ARCH=riscv64 PKG_WORLD=xfce GUI_MEDIA="$(GUI_MEDIA)" run-world-gui
run-gui-aarch64:
	$(MAKE) ARCH=aarch64 PKG_WORLD=xfce GUI_MEDIA="$(GUI_MEDIA)" run-world-gui
run-gui-x86_64:
	$(MAKE) ARCH=x86_64 PKG_WORLD=xfce GUI_MEDIA="$(GUI_MEDIA)" run-world-gui
run-gui-loongarch64:
	$(MAKE) ARCH=loongarch64 PKG_WORLD=xfce GUI_MEDIA="$(GUI_MEDIA)" run-world-gui

run-gui:
	$(MAKE) ARCH=$(ARCH) PKG_WORLD=xfce GUI_MEDIA="$(GUI_MEDIA)" run-world-gui

# 组装并直接启动：world 镜像作为第二块盘（内核挂载为 /extra；
# 含 /usr/lib/a20/init 标记的镜像会被 init chroot 接管，即 distro 模式）。
# 根盘仍是常规开发镜像（FAT32 → /bin）。
run-world: image-world $(FAT32_IMG)
	$(QEMU) $(QEMU_FLAGS_NO_SDCARD) \
		-drive file=$(abspath $(PKG_IMAGE_DIR)/$(PKG_WORLD)-$(PKG_ARCH).img),if=none,format=raw,id=xworld \
		-device $(QEMU_BLK_SECOND),drive=xworld \
		-kernel $(KERNEL_ELF)

# 回归门禁：上游 Alpine 包（gcc/fastfetch）经 chroot 在 guest 内真实运行。
# 守护 trap.S freemap 的 pfn 翻译修复（历史上 gcc 级负载触发
# "corrupted kernel stack pointer" 误报 panic）。需要网络拉取上游包
#（之后走 build/cache/apk 缓存）。
.PHONY: smoke-devtools
SMOKE_DEVTOOLS_IMG := $(PKG_IMAGE_DIR)/devtools-smoke-$(ARCH).img

smoke-devtools: $(FAT32_IMG) $(KERNEL_ELF)
	$(PYTHON) tools/mkrootfs.py --arch $(ARCH) \
		--world packages/world/devtools-smoke.world \
		--overlay tools/tests/devtools-overlay \
		$(if $(filter-out 0,$(shell id -u)),--usermode,) \
		$(if $(PKG_SIGN_KEY),--keys-dir $(abspath $(PKG_KEYS_DIR)),--allow-untrusted) \
		--output $(SMOKE_DEVTOOLS_IMG) --size-mb 768
	@mkdir -p $(SMOKE_LOG_DIR)
	@set -e; \
	log="$(SMOKE_LOG_DIR)/devtools-$(ARCH).log"; \
	status=0; \
	{ sleep $(SMOKE_INPUT_DELAY); \
	  printf 'chroot /extra /bin/sh /devtools-smoke.sh\npoweroff\n'; } | \
	$(TIMEOUT) 900s $(QEMU) $(QEMU_FLAGS_NO_SDCARD) \
		-drive file=$(abspath $(SMOKE_DEVTOOLS_IMG)),if=none,format=raw,id=xdevtools \
		-device $(QEMU_BLK_SECOND),drive=xdevtools \
		-kernel $(KERNEL_ELF) \
		> "$$log" 2>&1 || status=$$?; \
	if grep -q 'DEVTOOLS_SMOKE: PASS' "$$log"; then \
		echo "smoke-devtools: PASS; log saved to $$log"; \
	else \
		echo "smoke-devtools: failed (status $$status); tail of $$log:"; \
		tail -n 80 "$$log"; \
		exit 1; \
	fi
