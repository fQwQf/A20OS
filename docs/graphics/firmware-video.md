# 固件线性显示与软件视频

## 当前支持

x86-64 原生 UEFI loader 可以保留固件已经建立的 GOP 显示模式，交给公共`simplefb` 驱动发布 `/dev/fb0`。该路径不需要 Intel GPU 模式设置驱动，不访问 QEMU `fw_cfg`，也不会在 `ExitBootServices` 后调用固件绘图函数。固件显示也接入公共 DRM/KMS：现有 apk `xfce` world 可通过 labwc/pixman 呈现桌面、终端和软件播放器，KMS 的 XRGB8888 缓冲会转换到实际显存 stride 与 RGB 掩码。具体完整介质与离线媒体部署见[华为 CT5100](../platforms/huawei-ct5100.md)。HDA 提供 ALSA 所需的真实队列查询及非阻塞提交；其他原生 PCM 后端保留 `/dev/audioN` 接口，在补齐此查询前不宣称 ALSA 播放可用。以下 RAMFS 测试程序是独立的轻量诊断入口。 BIOS/GRUB 路径目前没有同等的 GOP 交接；J1900 应选择`instances/j1900-pc-uefi-shell.toml`。

loader 复制物理基址、显存范围、分辨率、每行字节数及 RGB 掩码。当前支持 GOP RGB/BGR 32 位像素，以及连续、互不重叠的 RGB BitMask 16/24/32 位模式。`PixelBltOnly` 没有 CPU 可访问显存，因此不会发布 framebuffer。 GOP 的 reserved 位不作为透明通道使用。模式从固件继承，不支持切换分辨率、热插拔重设或 GPU 硬件解码。

描述会验证页对齐、地址溢出、行宽、映射范围和像素掩码；显存从可分配 RAM 中排除。当前 x86 后端只能接受已有安全 UC 映射的低 4 GiB 物理窗口，通常为 2–4 GiB 的 PCI 显存；映射条件不满足时驱动拒绝绑定，并保留串口诊断。用户映射采用同样的设备内存属性，避免把同一显存同时映射成 WB 与 UC。可见图像最多 16384×16384、显存 aperture 最多 256 MiB。

协议依据：[UEFI GOP 规范](https://uefi.org/specs/UEFI/2.10/12_Protocols_Console_Support.html#graphics-output-protocol)。

## 构建与启动

在仓库根目录执行：

```sh
tools/a20 check j1900-pc-uefi-shell
tools/a20 build j1900-pc-uefi-shell
tools/a20 package j1900-pc-uefi-shell
```

实例使用 RAM-only 串口 shell，内嵌 `videoplay` 和 `audioplay`，不依赖硬盘根目录。 USB 写盘、串口接线、波特率及主板差异见该硬件的 [J1900 文档](../platforms/j1900.md)。实机需启用 64 位 UEFI 与连接显示器；只有 32 位 UEFI 的机器不能执行 BOOTX64.EFI。

没有媒体文件时，在目标 shell 验证十秒动态测试图：

```sh
videoplay --test 320 240 30
```

程序将画面居中，按真实 pitch 和通道掩码逐像素转换，通过单调时钟控制目标帧率。帧内白色标记移动；正常结束报告 `presented 300 frames`。输入尺寸必须不超过当前显示尺寸，支持 1–120 的整数帧率。

## 播放预转换视频

下述 `videoplay` 诊断程序接受逐帧、无头部、无行填充的 **RGB24** 字节流，尚未移植 MP4 容器与 H.264/HEVC 等压缩视频解码器。先在宿主使用 FFmpeg 解码和缩放：

```sh
ffmpeg -i input.mp4 -an \
  -vf 'fps=24,scale=320:240:flags=bilinear,format=rgb24' \
  -c:v rawvideo -f rawvideo clip.rgb
ffmpeg -i input.mp4 -vn -ar 48000 -ac 2 -c:a pcm_s16le clip.wav
```

`clip.rgb` 大小应为 `320 × 240 × 3 × 帧数`，十秒、24 fps 约 55.3 MB。在构建主机上，把文件加入已打包启动盘的 ESP。分区从 1 MiB 开始，下面操作只修改工作区内的镜像：

```sh
image=build/x86_64-pc/a20os-j1900-shell-uefi.img
mcopy -o -i "$image@@1048576" clip.rgb ::/clip.rgb
mcopy -o -i "$image@@1048576" clip.wav ::/clip.wav
sha256sum "$image" > "$image.sha256"
tools/a20 ledger j1900-pc-uefi-shell
```

之后按照平台文档使用 `tools/a20 deploy ... --no-build` 写入目标启动盘。再次运行 `package` 会重建镜像并删除后加的媒体文件，需重新执行 `mcopy`。主机需要 `parted` 和 `mtools`，转码另需 FFmpeg。

`RAMFS_MEDIA=1` 会在启动时保留嵌入的 `/bin`，从前 16 个稳定块设备索引中选择首个有效 FAT32 卷，只读挂载到 `/media`。支持裸 FAT32 及每盘前 16 个 GPT 分区项；当前仅接受 512 字节逻辑扇区，不自动解析 MBR/扩展分区。 GPT header 和分区项数组均检查 CRC 与范围。除 VFS 只读标记外，底层块视图也拒绝写请求；没有合适介质时串口 RAM shell 继续启动。

在目标 shell 执行：

```sh
videoplay /media/clip.rgb 320 240 24
```

无需手动挂载 ESP，媒体输入与 `/bin` 的 RAM 可执行程序互不遮挡。也可以从标准输入读取 RGB24（文件名参数为 `-`）。完整帧边界上的 EOF 为正常结束；空文件、截断帧、错误像素布局和越界尺寸会失败，不会提交半帧。程序按每帧读取和显示，仅分配一个 RGB24 帧；帧率仍受 CPU、显存访问和输入速度限制。

rawvideo 格式依据：[FFmpeg rawvideo 文档](https://www.ffmpeg.org/ffmpeg-formats.html#rawvideo)。

## 音频

embedded 与模块配置复用同一 Intel HDA 驱动，匹配 PCI class `040300`。驱动查询 codec、DAC、pin 与连接路径，要求实际支持 PCM 48 kHz、双声道、16 位格式；不按“Intel/J1900”名称猜测 pin。无法识别的 codec 路径会失败，板载放大器专用 GPIO、 HDMI codec 路径及主板专用勘误尚无通用覆盖。

```sh
audioplay --tone 1000 --duration 500
audioplay /media/clip.wav
```

视频和音频可以作为两个进程运行，但当前没有共享媒体时钟或音画同步机制，不保证同步。 HDA 协议依据：[Intel High Definition Audio Specification](https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf)。

## 可重复验证

构建与实例一致的独立 EFI，之后用实际 OVMF GOP 和 QMP 检查屏幕：

```sh
make ARCH=x86_64 BOARD=x86_64-pc ABI=linux BRINGUP=0 \
  RAMFS_USER=1 RAMFS_MEDIA=1 DRIVER_DEPLOYMENT=embedded \
  CONFIG_UBSAN=0 CONFIG_SWAP=0 x86_64-uefi-loader
build_dir=$(make -s ARCH=x86_64 BOARD=x86_64-pc ABI=linux BRINGUP=0 \
  RAMFS_USER=1 RAMFS_MEDIA=1 DRIVER_DEPLOYMENT=embedded \
  CONFIG_UBSAN=0 CONFIG_SWAP=0 print-build-dir)
python3 tools/check_uefi_framebuffer.py --efi "$build_dir/BOOTX64.EFI" --audio --media-files
```

检查会生成一个临时 GPT ESP，通过 AHCI 读取其中的 `clip.rgb` 和 `clip.wav`；串口 shell 执行真实播放器，完成 12 帧文件播放，并检查 QMP `screendump` 的实际屏幕 RGB 像素、HDA PCM 输出的 WAV 样本和正常关机。此外，向 `/media` 创建文件必须返回只读错误，启动前后整盘 SHA256 必须相同。不加 `--media-files` 时保留十秒、300 帧的内建动态图与测试音路径。输出在 `.kernel-build/smoke/uefi-media/`。宿主需要 QEMU、OVMF 和足够资源；没有 OVMF 时本检查不可替代为模拟 descriptor 或串口字符串通过。

宿主边界回归：

```sh
python3 -m unittest tools.tests.test_boot_framebuffer tools.tests.test_videoplay \
  tools.tests.test_media_mount tools.tests.test_uefi_esp_image
```

这些检查区分模拟器运行与真实 J1900 主板支持；真实显示器、codec 和主板仍需实机验收。

## 设备树固件显示

DT 平台可以通过启用的 `simple-framebuffer` 节点交接固件已初始化的扫描输出。公共解析器接受 `/chosen` 物理 `reg`，或指向 `/reserved-memory` 的`memory-region`；后者优先。节点必须给出 `width`、`height`、`stride`、`format`。优先选择 `stdout-path` 指向的 framebuffer，其次 `display0` 别名；否则只能存在一个受支持的候选。多个候选而无明确选择会拒绝启动，避免猜测屏幕。所有启用的 framebuffer aperture 都在页分配器启动前保留，即使未被选中。合法但不支持的格式也保留显存，并继续串口启动，不发布 fbdev。坏地址、溢出或覆盖内核仍拒绝启动。

支持 `r5g6b5`、`x1r5g5b5`、`r8g8b8`、`x8r8g8b8`、`x8b8g8r8`、`x2r10g10b10`。带 alpha 的 `a8*` 模式目前不支持。显存 aperture 必须包含页扩展后的可见范围；不足时保留资源、跳过显示。尺寸和 stride 使用共同验证规则。这条路径沿用固件模式，不设置 DE2、TCON、HDMI PHY 或 VOP2 寄存器。 PINE/V3s/Quartz64 的固件和接线条件见各自平台文档；只有 DT 节点并不证明硬件输出已工作。

AArch64 RAM framebuffer 要求现有映射确认为 WB、inner-shareable；内核和用户态映射保持相同缓存属性。固件从 `/memory` 裁掉的显存也按实际映射和平台 RAM 边界验证，不会当成普通 MMIO 映射。`FBIO_FLUSH` 清理到 PoC 并完成同步屏障。 ARM32 RAM 路径目前只支持已确认 PIPT 的 Cortex-A7/A15，并保留覆盖 aperture 的完整 1 MiB section；验证不覆盖内核和活动 DTB 后，统一所有引导 section 别名的 shareable 属性。无法证明别名安全时拒绝映射。现有 ARM32 权限编码不能表达 shareable 只读用户映射，因此仅接受可写 framebuffer mmap；NoMMU RAM framebuffer 不支持。

固件扫描输出的 `clocks`、`resets`、`power-domains` 引用在驱动绑定期间保留，阻止提供者在尚无原生显示接管协议时首次绑定并重设资源。卸载或 probe 失败会释放保留，并重试延迟提供者；已绑定提供者不能倒退成固件保留模式。当前不涵盖任意 `*-supply`稳压器属性。平台 MMC/音频时钟代码仍须按位修改，不能关闭显示已使用的时钟。

绑定依据：[Linux simple-framebuffer binding](https://github.com/torvalds/linux/blob/master/Documentation/devicetree/bindings/display/simple-framebuffer.yaml)、[U-Boot sunxi DE2 交接](https://github.com/u-boot/u-boot/blob/master/drivers/video/sunxi/sunxi_de2.c)。

QEMU 的可重复测试使用主机固件夹具初始化 ramfb，再将 DT 交给内核；内核只读取 DT，没有 fw_cfg 显示后门。在仓库根目录构建并运行独立 RAMFS media 配置（Bash）：

```sh
fb_arch=aarch64 # ARM32 改为 arm32，并安装对应交叉工具链
fb_make=(ARCH="$fb_arch" BOARD="qemu-virt-$fb_arch" ABI=linux BRINGUP=0
         RAMFS_USER=1 RAMFS_MEDIA=1 DRIVER_DEPLOYMENT=embedded CONFIG_UBSAN=0 CONFIG_SWAP=0)
make "${fb_make[@]}" kernel-only -j2
fb_build_dir=$(make -s "${fb_make[@]}" print-build-dir)
python3 tools/check_dt_framebuffer.py --kernel "$fb_build_dir/kernel.bin" \
  --platform "kernel/arch/$fb_arch/boot/tests/ramfb.json" \
  --output ".kernel-build/smoke/dt-framebuffer-$fb_arch"
python3 -m unittest tools.tests.test_dt_framebuffer tools.tests.test_simplefb_lifecycle \
  tools.tests.test_sleep_abi
```

检查真实 fbdev mmap、`FBIO_FLUSH`、300 帧用户程序、QMP 屏幕 RGB 像素及正常关机。它验证固件交接和缓存路径，不能代替物理板显示控制器及面板验收。
