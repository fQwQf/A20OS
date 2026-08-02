# 音频子系统

A20OS 音频栈把设备发现、通用用户接口和具体硬件协议分开。驱动通过 `DEV_CLASS_AUDIO` 发布能力，devfs 为已绑定实例创建 `/dev/audioN`；用户态必须查询能力，不能假定 `audio0` 一定是 PCM 设备。

## 代码边界

- `kernel/include/uapi/a20/audio.h`：用户态可见的 capability、PCM format 和 ioctl。
- `kernel/include/drivers/audio/audio_core.h` 与 `kernel/drivers/audio/audio_core.c`：内核内部 `audio_dev_ops_t`、通用 ioctl 参数封送、capability/format 校验和 close 策略。
- `kernel/drivers/core/driver_class.c`：audio class 编号与发布。
- `kernel/fs/devfs/devfs.c`：`/dev/audioN` 的 read/write、ioctl 和 close 转发。
- `kernel/drivers/audio/hda.c`：PCI Intel HDA controller、输出 stream 和 PCM DMA。
- `kernel/drivers/audio/hda_codec.c`：HDA immediate command、codec topology 和输出路径配置。
- `kernel/drivers/audio/virtio_snd.c`：VirtIO 1.2 sound control/event/TX/RX queue 与 PCM playback。
- `kernel/drivers/audio/pc_speaker.c`：只支持 tone 的 x86 PC Speaker。
- `user/cmds/audioplay.c`：WAV、raw PCM 和测试 tone 客户端。
- `user/wayland/player.c`：FFmpeg 解码、48 kHz 重采样和异步 PCM 输出。

HDA 是架构无关的 PCI class 驱动，匹配 PCI class `04:03:00`。源码不得包含 CPU 架构门禁；平台负责提供 ECAM、可映射 BAR、DMA handle、cache 同步和必要的中断或轮询能力。

## 用户接口

客户端依次打开 `/dev/audio0`、`/dev/audio1` 等节点，并首先调用 `A20_AUDIO_IOCTL_GET_CAPS`：

- `A20_AUDIO_CAP_TONE`：支持 `A20_AUDIO_IOCTL_TONE`，例如 PC Speaker。
- `A20_AUDIO_CAP_PCM`：支持 PCM write 和 `A20_AUDIO_IOCTL_SET_FORMAT`，例如 HDA 与 virtio-sound。
- `A20_AUDIO_IOCTL_STOP`：立即中止当前 generation，丢弃尚未播放的数据。
- `A20_AUDIO_IOCTL_DRAIN`：等待已接受的 PCM 播放完毕；不足一个 PCM frame 的尾部会补零。

当前两个 PCM 后端都只接受 48 kHz、双声道、S16_LE，即每帧四字节。write 成功返回实际接受的字节数，客户端必须处理短写和 `EINTR`。正常关闭 PCM 节点由 audio core 自动 drain；需要立即退出的错误路径应先调用 STOP。

通用层不假定 PCM ring、DMA descriptor 或硬件状态机。驱动以声明式 `caps` 和 `pcm_format` 描述固定能力，只实现 write、stop、drain 等硬件动作。PC Speaker 只复用 capability、TONE 和 STOP 分派，不承担 PCM format 或 ring 语义。

## HDA 初始化

probe 依次完成以下工作：

1. 启用 PCI memory decoding 并取得 BAR0。
2. 复位 controller，通过 immediate command 接口发现 codec 和 Audio Function Group。
3. 搜索支持 PCM 的 DAC、可输出 pin 以及它们之间的 widget path。
4. 配置 converter stream/channel、pin output、EAPD 和 amplifier gain。
5. 分配 coherent BDL 与 PCM buffer，并发布 audio class。

任一步失败都必须停止 stream、撤销 codec 配置并释放 DMA。remove 先增加 generation 阻止新 I/O，再 quiesce controller。

## PCM 环形 DMA

当前输出使用 64 KiB PCM ring，由两个 32 KiB BDL period 组成；每个 descriptor 设置 IOC。累计到半个 ring 后启动 stream，降低解码和调度抖动造成的启动欠载。

软件用 `write_pos` 和 `queued_bytes` 管理生产者位置，并只依据 HDA 的 LPIB 与 BCIS 更新已消费字节。不能根据墙钟时间推测 DMA 跨圈：PulseAudio、PipeWire 或宿主设备背压时，时间可以继续前进而 LPIB 不动，推测消费会覆盖仍在播放的数据，表现为重复或跳音。

LPIB 与 BCIS 必须作为稳定快照读取。启动前清除旧状态并把 LPIB 基线设为零；若读取期间 BCIS 从零变为一，则重新采样，避免同一次 period completion 在本次由 LPIB、下次又由残留 BCIS 重复计算。

写、drain、stop 和 close 由 controller mutex 串行化。STOP 先增加 generation，使正在等待 ring 空间的旧 write 返回，再停止硬件并清理软件队列。正常 drain 不计为 underrun。

## VirtIO Sound

virtio-sound 匹配协议 device ID 25，同时支持 VirtIO-MMIO、modern PCI `1af4:1059` 和 transitional PCI `1af4:1019` 身份。驱动只协商 `VIRTIO_F_VERSION_1`，配置 controlq、eventq、txq 和 rxq，并通过 `PCM_INFO` 选择支持 48 kHz、双声道 S16 的 output stream。

controlq 串行执行 SET_PARAMS、PREPARE、START、STOP 和 RELEASE。txq 使用八个驱动自有的 4 KiB DMA period；每个请求由只读 stream header、只读 interleaved PCM 和可写 status 组成。用户缓冲区和内核栈都不会直接暴露给设备。驱动先预缓冲两个 period 再 START；DRAIN 提交最后一个短 period、等待所有 TX completion，然后 STOP/RELEASE，使下一次 write 从干净状态重新 PREPARE。STOP 在取得 mutex 前递增 generation，正在等待空闲 descriptor 的 write 会返回 `EINTR`。

## 当前限制

- 每个 controller 只有一个全局 playback stream；尚未提供多客户端混音或 per-open session。
- 不支持 capture、音量控制、mixer、动态采样率或除 S16_LE stereo 之外的格式。
- HDA ring 和 virtio-sound completion 目前均由 write/drain 路径轮询。HDA 尚未启用 period IRQ；若生产者停止时间超过整个 ring，硬件可能在下一次 refresh 前重放旧数据。
- QEMU RISC-V64 使用 coherent DMA 假设；移植到非一致性缓存硬件前必须实现真实的 `dma_sync_for_device()`。

## 平台与 QEMU

`run-gui-riscv64`、`run-gui-x86_64` 和 `run-gui-loongarch64` 默认挂载 QEMU `intel-hda` 与 `hda-duplex`。设置 `QEMU_GUI_AUDIO_DEVICE=virtio` 后，RISC-V64 改用 MMIO `virtio-sound-device`，x86_64 和 LoongArch64 改用 `virtio-sound-pci`。宿主 backend 由 `QEMU_GUI_AUDIO_DRIVER` 选择，Linux 默认 `pa`，也可使用 `pipewire`、`alsa`、`sdl` 或 `none`。

RISC-V64 的 HDA 运行依赖 `kernel/arch/riscv64/platform/pci_host.c` 提供 ECAM 和 BAR 映射，以及 `kernel/platform/qemu-virt-riscv64/board.c` 启动 PCI 枚举。AArch64 QEMU virt 尚未提供相同 PCI host 路径，因此目前只有编译覆盖。

## 验证

```bash
make smoke-hda
make smoke-audio-userspace
make PYTHON='conda run -n a20os python' smoke-virtio-sound
make run-gui-riscv64 GUI_MEDIA=/path/to/video.mp4
make run-gui-riscv64 QEMU_GUI_AUDIO_DEVICE=virtio
```

`smoke-hda` 验证 codec topology、driver binding 和 BDL DMA。`smoke-audio-userspace` 播放五秒 tone，通过 QEMU WAV backend 检查 HDA 的帧数、非静音、采样连续性，并要求一次 stream start、零 underrun。`smoke-virtio-sound` 在同样的用户态负载下只挂载 `virtio-sound-pci`，验证协议发现、control/TX queue、DRAIN/RELEASE 和 WAV 输出。

完整媒体验证应同时检查来宾日志中的 `PLAYER_STATUS:0` 和 `[HDA] playback starts=1 underruns=0`。WAV backend 适合自动测试；最终还应至少使用一次实际 PulseAudio/PipeWire backend，覆盖宿主背压行为。
