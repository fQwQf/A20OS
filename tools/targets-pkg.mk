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
#   PKG_RECIPES   要打包的 recipe 名列表（默认核心四件）
#   PKG_WORLD     packages/world/ 下的清单名（默认 base）
#   PKG_SIZE_MB   镜像大小（默认 512；桌面 world 见 PKG_SIZE_MB_GUI）
#   PKG_ALPINE    image-world 是否引入 Alpine 仓库（默认 1；纯本地组合设 0）
#   PKG_SIGN_KEY  签名私钥（默认 build/keys/a20os-dev.rsa，自动生成）
#
# vim/git/gcc 等移植软件不再走 recipe：直接从 Alpine 上游仓库按 world 清单
# 解析即可（devel / devtools world 已经包含它们）。旧的 extra 包流程已退役，
# 见 tools/targets-extra.mk。

PKG_ARCH      ?= $(ARCH)
PKG_VARIANT   ?= $(USER_VARIANT)
PKG_KEYS_DIR  ?= build/keys
PKG_SIGN_KEY  ?= $(PKG_KEYS_DIR)/a20os-dev.rsa
PKG_KEY_NAME  ?= a20os-dev.rsa.pub
PKG_OUT_DIR   := build/packages/$(PKG_ARCH)
PKG_REPO_DIR  ?= build/repo
PKG_IMAGE_DIR ?= build/images
# a20-base 与 a20-min 是**互斥的替代**用户态（都 provide a20-userland），
# 默认两个都打：CI 的 pkg-repo 由此验证两个 recipe 都能构建，并让 min.world
# 在发布的仓库里可用。只想要其中一个时覆盖本变量即可。
PKG_RECIPES   ?= a20-base a20-min a20-corpus-tools a20-drivers a20-kernel
PKG_WORLD     ?= base
PKG_SIZE_MB_DEFAULT ?= 512
PKG_SIZE_MB   ?= $(PKG_SIZE_MB_DEFAULT)
PKG_ALPINE    ?= 1

# 服务器 world 的远程入口：镜像里没有密码（密码一旦烤进镜像就等于永久泄露），
# 只能烤公钥。调用方给出自己的公钥路径，空格分隔多个；为空则镜像不带任何
# authorized_keys（dropbear 仍会启动，但没人能登进去）。
#   make image-world PKG_WORLD=server SSH_PUBKEY=~/.ssh/id_ed25519.pub
SSH_PUBKEY    ?=

# 桌面 world（xfce 等）解包后 >1 GiB，通用默认 512 MiB 装不下（mkfs.ext4 会以
# "Could not allocate block in ext2 filesystem" 失败，看起来像磁盘满）。只要调用方
# 没显式给出大小，桌面 world 就提升到 PKG_SIZE_MB_GUI；非桌面 world 仍用默认值。
PKG_SIZE_MB_GUI   ?= 4096
PKG_WORLD_GUI     ?= xfce
PKG_SIZE_MB_WORLD := $(if $(filter $(PKG_SIZE_MB_DEFAULT),$(PKG_SIZE_MB)),$(if $(filter $(PKG_WORLD_GUI),$(PKG_WORLD)),$(PKG_SIZE_MB_GUI),$(PKG_SIZE_MB)),$(PKG_SIZE_MB))

# 同理：桌面跑 Xwayland + 软件 GL，通用默认的 1G 一定会被 OOM 杀掉（Xwayland 首当其冲）。
# 调用方没显式指定内存时，GUI 入口把内存提升到这个值。
QEMU_MEMORY_DEFAULT ?= 1G
QEMU_MEMORY_GUI     ?= 4G
QEMU_MEMORY_WORLD   := $(if $(filter $(QEMU_MEMORY_DEFAULT),$(QEMU_MEMORY)),$(QEMU_MEMORY_GUI),$(QEMU_MEMORY))

# 可选：往 world 镜像里注入本地媒体文件（演示视频等）。GUI_MEDIA 可写空格
# 分隔的多个文件或目录，全部落到镜像内 $(GUI_MEDIA_DIR)/。
#   make run-gui-x86_64 GUI_MEDIA=/path/to/video.mp4
#   make run-gui-x86_64 GUI_MEDIA="/a.mp4 /b.mp4 clips/"
# 桌面里用 parole / mpv 打开，或在 Thunar 里双击即可播放。
# GUI_MEDIA 默认是「粘性」的：桌面镜像在每次 run 时都会重建，而 overlay 只在给出
# GUI_MEDIA 时才生成 —— 于是 `make run-gui-x86_64`（不带变量）会把上次注入的
# /usr/share/a20-media 悄悄丢掉，看起来像"媒体没进镜像"。默认从已取件的
# build/minecraft/* 注入；要显式关掉就传 `GUI_MEDIA=`。
GUI_MEDIA_AUTO     := $(wildcard build/minecraft/*)
GUI_MEDIA          ?= $(GUI_MEDIA_AUTO)
GUI_MEDIA_DIR      ?= /usr/share/a20-media
GUI_MEDIA_OVERLAY  := build/overlay-media/$(PKG_WORLD)-$(PKG_ARCH)

.PHONY: pkg-key pkgs pkgs-check pkg-repo image-world run-world run-world-gui

# 本地开发签名密钥：不入库，仅用于本机/CI 内的签名验证闭环。
# 正式发布密钥由 CI secret 注入（见 docs/packaging/repository.md）。
pkg-key:
	$(PYTHON) tools/pkg.py pkg-key  \
	--recipes "$(PKG_RECIPES)" --arch "$(PKG_ARCH)" --variant "$(PKG_VARIANT)" \
	--build-dir "$(BUILD_DIR)" --out-dir "$(PKG_OUT_DIR)" \
	--repo-dir "$(PKG_REPO_DIR)" --image-dir "$(PKG_IMAGE_DIR)" \
	--keys-dir "$(PKG_KEYS_DIR)" --sign-key "$(PKG_SIGN_KEY)" \
	--key-name "$(PKG_KEY_NAME)" --world "$(PKG_WORLD)" \
	--size-mb "$(PKG_SIZE_MB_WORLD)" --alpine "$(PKG_ALPINE)"
# 只校验 recipe 与产物路径，不写包（CI 快速门禁用）。
pkgs-check:
	$(PYTHON) tools/pkg.py pkgs-check  \
	--recipes "$(PKG_RECIPES)" --arch "$(PKG_ARCH)" --variant "$(PKG_VARIANT)" \
	--build-dir "$(BUILD_DIR)" --out-dir "$(PKG_OUT_DIR)" \
	--repo-dir "$(PKG_REPO_DIR)" --image-dir "$(PKG_IMAGE_DIR)" \
	--keys-dir "$(PKG_KEYS_DIR)" --sign-key "$(PKG_SIGN_KEY)" \
	--key-name "$(PKG_KEY_NAME)" --world "$(PKG_WORLD)" \
	--size-mb "$(PKG_SIZE_MB_WORLD)" --alpine "$(PKG_ALPINE)"
# 链式语义（与 docs/packaging/overview.md 一致，也是 CI 的调用方式）：
#   pkg-key 生成签名密钥 → pkgs 打包（需先有内核与用户态构建产物）→
#   pkg-repo 建 repo → image-world 组镜像。重构进 pkg.py 时这些边曾被丢掉，
#   导致干净树上 `make pkg-repo` 复制空目录并让 mka20repo.sh 直接失败。
pkgs: $(USER_BUILD_STAMP) $(KERNEL_ELF) $(if $(PKG_SIGN_KEY),pkg-key,)
pkgs:
	$(PYTHON) tools/pkg.py pkgs  \
	--recipes "$(PKG_RECIPES)" --arch "$(PKG_ARCH)" --variant "$(PKG_VARIANT)" \
	--build-dir "$(BUILD_DIR)" --out-dir "$(PKG_OUT_DIR)" \
	--repo-dir "$(PKG_REPO_DIR)" --image-dir "$(PKG_IMAGE_DIR)" \
	--keys-dir "$(PKG_KEYS_DIR)" --sign-key "$(PKG_SIGN_KEY)" \
	--key-name "$(PKG_KEY_NAME)" --world "$(PKG_WORLD)" \
	--size-mb "$(PKG_SIZE_MB_WORLD)" --alpine "$(PKG_ALPINE)"
pkg-repo: pkgs
	$(PYTHON) tools/pkg.py pkg-repo  \
	--recipes "$(PKG_RECIPES)" --arch "$(PKG_ARCH)" --variant "$(PKG_VARIANT)" \
	--build-dir "$(BUILD_DIR)" --out-dir "$(PKG_OUT_DIR)" \
	--repo-dir "$(PKG_REPO_DIR)" --image-dir "$(PKG_IMAGE_DIR)" \
	--keys-dir "$(PKG_KEYS_DIR)" --sign-key "$(PKG_SIGN_KEY)" \
	--key-name "$(PKG_KEY_NAME)" --world "$(PKG_WORLD)" \
	--size-mb "$(PKG_SIZE_MB_WORLD)" --alpine "$(PKG_ALPINE)"
# 生成只含媒体文件的临时 overlay，由 mkrootfs 的 --overlay 机制合入镜像。
.PHONY: pkg-media-overlay
pkg-media-overlay:
	$(PYTHON) tools/pkg.py media-overlay --media "$(GUI_MEDIA)" --media-dir "$(GUI_MEDIA_DIR)" --media-overlay "$(GUI_MEDIA_OVERLAY)" \
	--recipes "$(PKG_RECIPES)" --arch "$(PKG_ARCH)" --variant "$(PKG_VARIANT)" \
	--build-dir "$(BUILD_DIR)" --out-dir "$(PKG_OUT_DIR)" \
	--repo-dir "$(PKG_REPO_DIR)" --image-dir "$(PKG_IMAGE_DIR)" \
	--keys-dir "$(PKG_KEYS_DIR)" --sign-key "$(PKG_SIGN_KEY)" \
	--key-name "$(PKG_KEY_NAME)" --world "$(PKG_WORLD)" \
	--size-mb "$(PKG_SIZE_MB_WORLD)" --alpine "$(PKG_ALPINE)"
image-world: pkg-repo $(if $(GUI_MEDIA),pkg-media-overlay)
	$(PYTHON) tools/pkg.py image-world --media "$(GUI_MEDIA)" --media-dir "$(GUI_MEDIA_DIR)" --media-overlay "$(GUI_MEDIA_OVERLAY)" \
	--recipes "$(PKG_RECIPES)" --arch "$(PKG_ARCH)" --variant "$(PKG_VARIANT)" \
	--build-dir "$(BUILD_DIR)" --out-dir "$(PKG_OUT_DIR)" \
	--repo-dir "$(PKG_REPO_DIR)" --image-dir "$(PKG_IMAGE_DIR)" \
	--keys-dir "$(PKG_KEYS_DIR)" --sign-key "$(PKG_SIGN_KEY)" \
	--key-name "$(PKG_KEY_NAME)" --world "$(PKG_WORLD)" \
	--size-mb "$(PKG_SIZE_MB_WORLD)" --alpine "$(PKG_ALPINE)" \
	--ssh-pubkey "$(SSH_PUBKEY)"
# GUI variant for desktop worlds (xfce, ...): same second-disk distro boot,
# but with the virtio-gpu display stack and audio.  The image is built here (in
# its own make invocation) rather than as a prerequisite so that the desktop
# size default above actually applies.
run-world-gui: $(FAT32_IMG)
	$(MAKE) ARCH=$(ARCH) PKG_WORLD=$(PKG_WORLD) GUI_MEDIA="$(GUI_MEDIA)" \
		PKG_SIZE_MB=$(PKG_SIZE_MB_WORLD) image-world
	$(QEMU) $(patsubst -nographic,-display $(QEMU_GUI_DISPLAY) $(QEMU_GUI_DEVICES) $(QEMU_GUI_AUDIO) -serial stdio,$(patsubst -m $(QEMU_MEMORY),-m $(QEMU_MEMORY_WORLD),$(QEMU_FLAGS_NO_SDCARD))) \
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

# 门禁：stock Mesa 能否 attach 到我们的 virtio-gpu（3D 传输层之外的另一半）。
#
# 与 smoke-gpu3d-* 的区别是本质的：那些门禁里说话的是 gpu3d_test 自己，它直接
# 发 DRM_IOCTL_VIRTGPU_* 并自己回读像素，所以只证明**传输层**通。决定桌面能否用上
# 3D 的客户端是 libEGL/libgbm 去加载 virtio_gpu_dri.so，那是 libdrm 设备枚举 +
# DRI device 创建，另一条路径另一组期待。
#
# 镜像刻意不带 /etc/a10-distro 标记（因此不带 packages/overlay），内核便不会
# chroot 进去 exec 桌面；它只作为 /extra 挂载，由 smoke.py 像 smoke-devtools
# 那样手动 chroot，于是串口上始终有一个读 stdin 的 shell。
#
# GPU_3D=1 是必需的：GPU_3D=0 时 QEMU 提供 2D-only 设备，Mesa 找不到 virgl，
# 测的是另一件事。
.PHONY: smoke-mesa-attach
SMOKE_MESA_IMG := $(PKG_IMAGE_DIR)/mesa-probe-$(ARCH).img

# GPU_3D=1 is what asks QEMU for the virgl-capable device, and DISPLAY_MODE=gui
# is what attaches a GPU device *at all* -- under -nographic QEMU attaches none,
# so without it the guest has no virtio-gpu and every VIRTGPU ioctl answers
# -ENODEV.  Both are required, and the failure without them is silent: the probe
# still runs, still reports, and reports a device that is simply not there.
smoke-mesa-attach: DISPLAY_MODE=gui
# egl-headless,gl=on rather than the gtk default: gtk needs an X display, and
# without gl=on QEMU refuses to create the device at all.  Run it under
# tools/with-virgl-display.sh on a host with two GPU drivers.
smoke-mesa-attach: QEMU_GUI_DISPLAY=egl-headless
# Whether "Mesa bound virgl" is a hard failure. On by default because it now
# holds: stock Mesa reaches virgl and the renderer string says so. Set 0 to
# downgrade to reporting-only while bisecting an attach regression.
MESA_REQUIRE_VIRGL_ATTACH ?= 1
smoke-mesa-attach: $(FAT32_IMG) $(KERNEL_ELF)
	$(PYTHON) tools/mkrootfs.py --arch $(ARCH) \
		--world packages/world/mesa-probe.world \
		--overlay tools/tests/mesa-probe-overlay \
		$(if $(filter-out 0,$(shell id -u)),--usermode,) \
		$(if $(PKG_SIGN_KEY),--keys-dir $(abspath $(PKG_KEYS_DIR)),--allow-untrusted) \
		--output $(SMOKE_MESA_IMG) --size-mb 1024
	@$(PYTHON) tools/smoke.py mesa-attach \
		--label "$(ARCH)" \
		--log-dir "$(SMOKE_LOG_DIR)" \
		--qemu "$(QEMU)" \
		$(addprefix --qemu-flag=,$(QEMU_FLAGS_NO_SDCARD)) \
		--img "$(SMOKE_MESA_IMG)" \
		--blk-second "$(QEMU_BLK_SECOND)" \
		--kernel "$(KERNEL_ELF)" \
		--timeout 900s \
		$(if $(filter 1,$(MESA_REQUIRE_VIRGL_ATTACH)),--require-virgl-attach,) \
		--input-delay "$(SMOKE_INPUT_DELAY)"

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
	@$(PYTHON) tools/smoke.py devtools \
		--label "$(ARCH)" \
		--log-dir "$(SMOKE_LOG_DIR)" \
		--qemu "$(QEMU)" \
		$(addprefix --qemu-flag=,$(QEMU_FLAGS_NO_SDCARD)) \
		--img "$(SMOKE_DEVTOOLS_IMG)" \
		--blk-second "$(QEMU_BLK_SECOND)" \
		--kernel "$(KERNEL_ELF)" \
		--timeout 900s \
		--input-delay "$(SMOKE_INPUT_DELAY)"
