# MCU 启动身份与持久会话

实际入口：`kernel/platform/stm32f103/cluster_identity.c`，由 MCU `kernel_main`
在专用集群固件的中断开启、协议泵开始之前调用。共用 journal 算法为
`kernel/mcu/boot_epoch.h`，板级只提供半字读写和页擦除回调。

## 1. 身份来源和 Flash 保留区

STM32 node_id = 四字节命名空间 `A20M` + 芯片 UID 的 12 字节原始值。
UID 地址与 Flash 容量寄存器地址依据 ST 官方
[CMSIS STM32F103xB 定义](https://github.com/STMicroelectronics/cmsis-device-f1/blob/master/Include/stm32f103xb.h)。
全零/全 FF UID 被拒绝；UID 是身份来源，不是密码学认证。

boot epoch 写入 Flash 最后两页。64/128 KiB 器件保留 2 KiB；高密度器件按
2 KiB 页保留 4 KiB。页组织、半字编程与忙期间的访问约束依据 ST
[PM0075](https://www.st.com/resource/en/programming_manual/pm0075-stm32f10xxx-flash-memory-microcontrollers-stmicroelectronics.pdf)。
链接脚本从可用程序 Flash 扣除保留区，提供 `_cluster_nv_start/end`；固件镜像
不包含这个区域。板级初始化核对实际容量与编译配置，页对齐或容量不符即失败。

Flash 写入和等待函数放在 `.ramfunc`，启动时和 `.data` 一起复制到 SRAM。
编程后核对硬件错误位和读回值，最后重新锁定 Flash。调用方必须在调度开始前
串行调用，不能在多个任务中并发写这个 journal。

## 2. 两页 journal 契约

记录 16 B，小端：epoch:u64、校验:u32、保留零:u16、最后写入的 seal:u16。
启动扫描所有记录，选择最大有效 epoch，并在其所在页追加 epoch+1。页满时先
擦除另一页，再提交新记录；擦除时仍保留最高有效记录所在页。只有 seal 和完整
读回都成功，epoch 才返回给 leaf 配置并出现在网络上。

半写记录没有有效 seal，不会被当成成功启动。有效 seal 对应的损坏记录可能曾是
最高 epoch，不能简单忽略后退到旧值：遇到它会拒绝开启集群。非空且没有有效
记录、容量不符、写保护/校验失败、epoch 耗尽也同样失败。系统仍可启动，leaf
被禁用，启动日志显示 `persistent identity=disabled`；不会回退到演示 ID。

该机制防止在正常记录和受测掉电模型下复用已发布的 epoch，不是任意介质损坏后
的恢复承诺。撕裂的首次初始化或损坏的已提交记录可能需要维护处理。
CRC 是损坏检测，不能防止有权限改写 Flash 的攻击者。

## 3. 更新固件与维护

正常 OpenOCD `flash write_image erase` 只覆盖 ELF 的程序区，保留 journal。
禁止在沿用身份时通过 mass erase 或其他固件覆写这两页后直接恢复集群；那会失去
历史高水位。确需清空时应重新委派 node_id 或确认旧会话和事务均已退役后再维护。
目前未提供在线 journal 擦除接口，普通应用不能通过集群请求改写启动身份。

`a20_mcu_leaf_configure` 接受板级身份与非零 epoch，仅在断开且没有待完成握手时
允许修改；同样的限制适用于算子表安装。测试用默认身份仍存在于独立 harness，
生产 `main` 必须经过板级配置成功后才运行 leaf。

## 4. 专用串口与固件配置

`STM32_CLUSTER_PORT=0` 是原有外设固件，不开启 leaf。集群固件选 2 或 3：

```sh
make ARCH=armv7m BOARD=stm32f103 STM32_CLUSTER_PORT=2 kernel-only
```

USART2 使用 PA2/TX、PA3/RX；USART3 使用 PB10/TX、PB11/RX，115200 8N1。
USART1 留给控制台，拒绝选作集群接口。接线须交叉 TX/RX 并共地，使用符合
STM32 电平的串口；集群路径直接调用原始驱动，不经过终端换行转换。
USART2/3 的原板 Wi-Fi/蓝牙连接应断开，不能在同一引脚上并联第二个发送者。

这是独立的集群固件配置：启动一个 2 KiB 栈的协议线程，每 1 ms 运行一次 leaf；
不初始化和运行面板、无线模块等可能阻塞的外设服务。中断仅收字节到 512 B 环，
周期泵每次最多取 64 B。RX 满、硬件错误和 TX 等待失败均计数或向上返回失败；
受损帧靠 CRC/SLIP 和可靠重试恢复。不会因为 UART 故障进入无限 busy-wait。
板级 `stm32_cluster_uart_rx_drops()` 提供 RX 丢弃计数。

通用 `a20_mcu_leaf_set_io` 允许其他 MCU 绑定自己的原始串口回调，只能在断开且
没有待确认握手时设置。init 的控制台 seam 仅用于独立测试；生产 main 默认禁用
leaf，必须成功绑定专用 I/O、读取持久身份并 configure 才启用。输出只有一个线程
写入；同一串口不可同时绑定其他业务或日志写入者。

64 KiB Flash / 20 KiB SRAM 的 USART2 固件实际 ELF：Flash 装载约 44088 B，
静态 SRAM 8608 B，另保留 2048 B 启动栈和 2048 B Flash journal。
`-fstack-usage` 给出的 `leaf_on_call` 局部栈 504 B、frame 分发 96 B、poll 32 B；
这些是函数局部用量，不代表完整调用链或中断峰值。实际线程栈为 2048 B，
完整物理栈水位仍须实测。链接结果和局部栈记录可从对应 build dir 的 ELF/`.su`
重建；不能把编译大小解释为硬件吞吐量。

## 5. UART 会话确认与证据边界

wire v1 UART 的双方 HELLO 都携带本方 challenge txid，ACK 回显当前 challenge。
收到 HELLO 可以应答，但不能仅凭它改变已确认的 peer epoch 或清除回复缓存。
叶子为确认对端而发出的 HELLO 使用有界重试，未分配短地址时仅允许握手族帧。
旧 ACK、旧 HELLO 或 v0 降级请求不能回滚已经建立的 v1 会话；确认重启后头节点
先发 DOWN，再发布新 UP，旧代理被取消。v0 仍保留不可靠兼容路径。

验证覆盖 1000 次启动、216 个写入/页切换掉电点、已提交记录损坏和计数耗尽；
实际 UART 主/叶代码覆盖双方重启、旧 HELLO/ACK 重放、缓存保持及 DOWN 事件。
STM32 64 KiB Flash/20 KiB SRAM 配置已经链接成功。
这些是主机存储故障模型和固件构建结果，没有物理板 Flash 写入、掉电或 UART
联调证据；实际部署仍须完成物理验证、波特率和栈水位检查。
