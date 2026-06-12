# A20 内核驱动接口文档

## 1. 概述

A20 内核驱动模型采用**混合架构**：板级地址使用宏常量（零开销），设备操作使用函数指针虚表（单次间接），硬件访问使用内联 `readl`/`writel`（零开销）。

### 核心设计原则

- **架构无关**：驱动代码仅通过 `hwapi.h` 访问硬件，不直接使用 CSR 或架构特定指令
- **零开销 MMIO**：`readl`/`writel` 编译为单条 load/store 指令
- **单次间接调用**：`class_ops` 虚表仅一次指针解引用
- **编译时注册**：内置驱动通过链接器段 `.driver_init` 自动注册，无运行时开销
- **板级可移植**：更换开发板只需实现 `board_config_t`，驱动代码不变

### 目录结构

```
kernel/
├── drivers/
│   ├── core/               # driver_core/driver_hwapi/driver_class
│   ├── bus/                # virtio-mmio / PCI bus enumeration
│   ├── block/              # block devices: virtio-blk, loop, SDIO
│   ├── char/               # UART / PTY
│   └── net/                # virtio-net and platform NICs
└── platform/
    ├── qemu-virt-riscv64/  # QEMU RISC-V 64
    ├── qemu-virt-loongarch64/
    ├── qemu-virt-aarch64/
    ├── qemu-virt-x86_64/
    ├── visionfive2/
    └── ls2k1000/
```

---

## 2. 核心数据结构

### 2.1 device_t — 设备

```c
typedef struct device {
    const char       *name;        /* 设备名称，如 "virtio-blk0" */
    struct bus_type  *bus;         /* 所属总线 */
    struct driver    *drv;         /* 绑定的驱动 */
    void             *drv_priv;    /* 驱动私有数据 */
    resource_t       *res;         /* 资源数组 */
    int               res_count;   /* 资源数量 */
    void             *plat_data;   /* 平台特定数据 */
    int               state;       /* DEV_STATE_UNINIT/ACTIVE/SUSPENDED */
} device_t;
```

### 2.2 driver_t — 驱动

```c
typedef struct driver {
    const char       *name;        /* 驱动名称 */
    const device_id_t *id_table;   /* 支持的设备 ID 表 */
    struct bus_type  *bus;         /* 所属总线类型 */
    int  (*probe)(device_t *dev);  /* 探测并初始化设备 */
    void (*remove)(device_t *dev); /* 移除设备 */
    const void       *class_ops;   /* 设备类操作虚表 */
    int               class_type;  /* CLASS_BLOCK / CLASS_NET / CLASS_CHAR */
} driver_t;
```

### 2.3 bus_type_t — 总线类型

```c
typedef struct bus_type {
    const char *name;
    int  (*match)(device_t *dev, const driver_t *drv);
    int  (*probe)(device_t *dev);
    void (*remove)(device_t *dev);
} bus_type_t;
```

### 2.4 resource_t — 硬件资源

```c
typedef struct resource {
    int      type;       /* RES_MMIO / RES_IRQ */
    paddr_t  start;
    paddr_t  end;
    uint32_t flags;      /* IORESOURCE_MMIO_32BIT / IORESOURCE_IRQ_EDGE 等 */
} resource_t;
```

### 2.5 board_config_t — 板级配置

```c
typedef struct board_config {
    const char            *name;
    paddr_t                ram_base;
    paddr_t                ram_end;
    const irqchip_ops_t   *irqchip;
    const timer_ops_t     *timer;
    void                 (*early_init)(void);
    void                 (*poweroff)(void);
    void                 (*reboot)(void);
    void                 (*enumerate_devices)(void);
} board_config_t;
```

---

## 3. 硬件访问 API（driver_hwapi.h）

驱动必须**仅通过此 API** 访问硬件。所有函数均为内联或委托到板级实现。

### 3.1 MMIO 访问（零开销内联）

```c
/* 32 位读写 */
static inline uint32_t readl(const volatile void *addr);
static inline void     writel(uint32_t val, volatile void *addr);

/* 64 位读写 */
static inline uint64_t readq(const volatile void *addr);
static inline void     writeq(uint64_t val, volatile void *addr);
```

### 3.2 DMA 内存管理

```c
void *dma_alloc(size_t size);   /* 分配对齐的物理连续内存，清零 */
void  dma_free(void *ptr);      /* 释放 */
```

### 3.3 中断管理

```c
typedef void (*irq_handler_t)(unsigned int irq, void *data);

int  request_irq(unsigned int irq, irq_handler_t handler, void *data);
void free_irq(unsigned int irq);
```

`request_irq` 注册中断处理函数并自动启用该中断。`free_irq` 禁用并注销。

### 3.4 时钟 API

```c
unsigned long clock_get_cycles(void);    /* 读取当前计数器值 */
void          clock_set_deadline(unsigned long deadline);  /* 设置下次中断 */
unsigned long clock_get_freq(void);      /* 获取时钟频率 */
```

### 3.5 延时函数

```c
void udelay(unsigned int us);    /* 微秒延时 */
void mdelay(unsigned int ms);    /* 毫秒延时 */
```

---

## 4. 设备类接口（driver_class.h）

### 4.1 块设备（CLASS_BLOCK）

```c
typedef struct block_dev_ops {
    int  (*read_sector)(void *priv, uint64_t sector, void *buf, int count);
    int  (*write_sector)(void *priv, uint64_t sector, const void *buf, int count);
    int  (*ioctl)(void *priv, int cmd, void *arg);
    int  (*get_info)(void *priv, uint64_t *total_sectors, uint32_t *sector_size);
} block_dev_ops_t;
```

`ioctl` 命令：
- `BLK_GETSIZE64` — 获取设备大小（字节）
- `BLKFLSBUF` — 刷新缓冲区

### 4.2 网络设备（CLASS_NET）

```c
typedef struct net_dev_ops {
    int  (*send)(void *priv, const void *data, int len);
    int  (*recv)(void *priv, void *buf, int buf_size);
    int  (*get_mac)(void *priv, uint8_t *mac);
    int  (*set_mac)(void *priv, const uint8_t *mac);
    int  (*poll)(void *priv);
    int  (*ioctl)(void *priv, int cmd, void *arg);
} net_dev_ops_t;
```

`ioctl` 命令：
- `NET_GET_MAC` — 获取 MAC 地址
- `NET_SET_MAC` — 设置 MAC 地址
- `NET_GET_STATUS` — 获取链路状态

### 4.3 字符设备（CLASS_CHAR）

```c
typedef struct char_dev_ops {
    int  (*read)(void *priv, void *buf, int count);
    int  (*write)(void *priv, const void *buf, int count);
    int  (*ioctl)(void *priv, int cmd, void *arg);
} char_dev_ops_t;
```

---

## 5. 驱动注册

### 5.1 内置驱动注册宏

```c
DRIVER_REGISTER(my_driver);
```

此宏将 `driver_t` 指针放入链接器段 `.driver_init`，内核启动时自动调用 `driver_register()`。

### 5.2 板级注册宏

```c
BOARD_REGISTER(my_board);
```

将 `board_config_t` 指针放入 `.board_init` 段。

### 5.3 完整驱动模块宏

```c
DRIVER_MODULE(drv_name, bus_ptr, probe_fn, remove_fn, id_tbl, ops_ptr, class_type);
```

一次性定义 `driver_t` 并注册。

---

## 6. 编写驱动的步骤

### 步骤 1：定义设备 ID 表

```c
static const device_id_t my_drv_ids[] = {
    { .vendor = 0x1AF4, .device = 0x1001 },  /* VirtIO 网卡 */
    { .vendor = 0,      .device = 0 },        /* 表尾哨兵 */
};
```

使用 `VENDOR_ANY` 或 `DEVICE_ANY` 通配。

### 步骤 2：实现 probe 函数

```c
static int my_driver_probe(device_t *dev) {
    /* 1. 从 dev->res 获取 MMIO 基地址 */
    uintptr_t base = dev->res[0].start;

    /* 2. 通过 hwapi 访问硬件 */
    uint32_t status = readl((const volatile void *)base);

    /* 3. 初始化硬件 */

    /* 4. 分配驱动私有数据 */
    struct my_priv *priv = dma_alloc(sizeof(*priv));
    dev->drv_priv = priv;

    /* 5. 注册中断 */
    request_irq(dev->res[1].start, my_irq_handler, priv);

    /* 6. 设置 class_ops */
    dev->drv->class_ops = &my_dev_ops;

    return 0;
}
```

### 步骤 3：注册驱动

```c
static driver_t my_driver = {
    .name       = "my-device",
    .id_table   = my_drv_ids,
    .bus        = &my_bus,
    .probe      = my_driver_probe,
    .remove     = my_driver_remove,
    .class_ops  = &my_dev_ops,
    .class_type = CLASS_BLOCK,
};

DRIVER_REGISTER(my_driver);
```

### 步骤 4：更新 Makefile

将驱动源文件加入 `kernel/drivers/{block,char,net,bus,core}/` 中对应目录，Makefile 的 `$(wildcard)` 会自动包含。

---

## 7. 中断处理

### 注册流程

1. 驱动在 `probe` 中调用 `request_irq(irq_num, handler, priv_data)`
2. `driver_hwapi.c` 记录处理函数并调用 `board->irqchip->enable_irq(irq_num)`
3. 中断触发时，架构中断入口调用 `driver_irq_dispatch(irq_num)`
4. `driver_irq_dispatch` 依次调用 `ack` → `handler` → `eoi`

### 中断处理函数约定

```c
void my_irq_handler(unsigned int irq, void *data) {
    /* 1. 读取中断状态（通过 MMIO） */
    /* 2. 处理事件 */
    /* 3. 清除中断（通过 MMIO 写） */
    /* 不要调用 eoi/ack — driver_irq_dispatch 已处理 */
}
```

---

## 8. 总线驱动开发

总线驱动负责枚举总线上的设备并创建 `device_t`。

### 实现要点

1. 定义 `bus_type_t`，实现 `match` 函数
2. 实现 `enumerate` 函数，扫描总线创建设备
3. 调用 `bus_register()` 注册总线
4. 对每个发现的设备调用 `device_register()`

### 现有总线

| 总线 | 文件 | 用途 |
|------|------|------|
| virtio-mmio | `kernel/drivers/bus/virtio_mmio_bus.c` | RISC-V/ARM64 QEMU |
| pci | `kernel/drivers/bus/pci_bus.c` | LoongArch QEMU, PC 平台 |

---

## 9. 板级适配

### 适配新开发板的步骤

1. 在 `kernel/platform/` 下创建目录（如 `myboard/`）
2. 创建 `platform.h`，定义所有 MMIO 基地址常量
3. 创建 `board.c`，实现 `board_config_t`：
   - `irqchip_ops`：中断控制器操作
   - `timer_ops`：定时器操作
   - `early_init`：UART 初始化等
   - `poweroff`/`reboot`：电源管理
   - `enumerate_devices`：总线/平台设备枚举
4. 在 Makefile 中添加 `BOARD=myboard` 支持

### irqchip_ops 接口

```c
typedef struct irqchip_ops {
    void (*enable_irq)(unsigned int irq);
    void (*disable_irq)(unsigned int irq);
    void (*ack)(unsigned int irq);
    void (*mask)(unsigned int irq);
    void (*unmask)(unsigned int irq);
    void (*eoi)(unsigned int irq);
} irqchip_ops_t;
```

### timer_ops 接口

```c
typedef struct timer_ops {
    unsigned long (*read_counter)(void);
    void          (*set_deadline)(unsigned long deadline);
    unsigned long (*get_freq)(void);
} timer_ops_t;
```

---

## 10. 构建系统

### 选择目标板

```makefile
make ARCH=riscv64 BOARD=qemu-virt-rv64    # 默认
make ARCH=riscv64 BOARD=visionfive2        # VisionFive2
make ARCH=loongarch64 BOARD=ls2k1000       # Loongson 2K1000
```

### 链接器段

驱动注册使用 `.driver_init` 链接器段。链接脚本需包含：

```
.driver_init : {
    __driver_init_start = .;
    KEEP(*(.driver_init))
    __driver_init_end = .;
}
```

### 启动流程

```
arch_early_init()          ← 架构特定早期初始化
  → board->early_init()    ← UART 等
driver_core_init()         ← 初始化驱动核心
board->enumerate_devices() ← 枚举总线设备
driver_probe_all()         ← 遍历 .driver_init 段，匹配并探测
subsystem_init()           ← 文件系统、网络等子系统
```

---

## 11. 示例：最小块设备驱动

```c
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_class.h"

static int my_blk_read(void *priv, uint64_t sector, void *buf, int count) {
    struct my_priv *p = priv;
    uintptr_t base = p->mmio_base;
    /* 通过 readl/writel 操作硬件 */
    return count;
}

static int my_blk_write(void *priv, uint64_t sector, const void *buf, int count) {
    struct my_priv *p = priv;
    uintptr_t base = p->mmio_base;
    /* 通过 readl/writel 操作硬件 */
    return count;
}

static const block_dev_ops_t my_blk_ops = {
    .read_sector  = my_blk_read,
    .write_sector = my_blk_write,
};

static int my_blk_probe(device_t *dev) {
    struct my_priv *p = dma_alloc(sizeof(*p));
    p->mmio_base = dev->res[0].start;
    dev->drv_priv = p;
    return 0;
}

static const device_id_t my_blk_ids[] = {
    { .vendor = 0x1AF4, .device = 0x1002 },
    { 0 },
};

static driver_t my_blk_driver = {
    .name       = "my-blk",
    .id_table   = my_blk_ids,
    .bus        = NULL, /* 由总线驱动填充 */
    .probe      = my_blk_probe,
    .class_ops  = &my_blk_ops,
    .class_type = CLASS_BLOCK,
};

DRIVER_REGISTER(my_blk_driver);
```
