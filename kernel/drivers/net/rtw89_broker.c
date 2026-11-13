/* SPDX-License-Identifier: Apache-2.0
 * PCI/DMA and Ethernet transport for the rtw89 userspace service.
 * Radio, firmware and 802.11 code live in the separately linked executable. */
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "drivers/bus/pci_bus.h"
#include "core/errno.h"
#include "core/defs.h"
#include "core/fcntl.h"
#include "core/lock.h"
#include "core/mman.h"
#include "core/poll.h"
#include "core/string.h"
#include "core/sync.h"
#include "fs/vfs.h"
#include "mm/mm.h"
#include "mm/vm.h"
#include "mm/pt.h"
#include "proc/proc.h"
#include "proc/userns.h"
#include "sys/usercopy.h"
#include "uapi/a20/wifi.h"
#include "net/stack.h"

#define WIFI_DMA_SLOTS 4096
#define WIFI_DMA_BUDGET (32U << 20)
#define WIFI_PACKET_MAX 2048
#define WIFI_PACKET_QUEUE 64
#define PCI_COMMAND 4
#define PCI_MASTER 4
#define PCI_INTX_DISABLE 0x400

struct wifi_dma {
    void *memory;
    uint64_t bus, token, mapped;
    size_t size;
};
struct wifi_packets {
    uint8_t bytes[WIFI_PACKET_QUEUE][WIFI_PACKET_MAX];
    uint16_t sizes[WIFI_PACKET_QUEUE];
    unsigned head, tail;
};
static struct {
    device_t *device;
    uint64_t bar, bar_size, mmio_va, next_token;
    uint8_t mac[6];
    volatile int carrier;
    int quarantined;
    int mac_ready;
    mm_struct_t *owner;
    struct wifi_dma dma[WIFI_DMA_SLOTS];
    size_t dma_bytes;
    mutex_t control;
    spinlock_t packets_lock;
    struct wifi_packets tx, rx;
} wifi;
static mutex_t wifi_open_lock = MUTEX_INIT;

static int packet_push(struct wifi_packets *queue, const void *data, size_t length)
{
    if (length < 14 || length > WIFI_PACKET_MAX)
        return -EMSGSIZE;
    uint64_t flags = spin_lock_irqsave(&wifi.packets_lock);
    unsigned next = (queue->head + 1) % WIFI_PACKET_QUEUE;
    int result = -EAGAIN;
    if (next != queue->tail) {
        memcpy(queue->bytes[queue->head], data, length);
        queue->sizes[queue->head] = length;
        queue->head = next;
        result = (int)length;
    }
    spin_unlock_irqrestore(&wifi.packets_lock, flags);
    return result;
}

static int packet_pop(struct wifi_packets *queue, void *data, size_t capacity)
{
    uint64_t flags = spin_lock_irqsave(&wifi.packets_lock);
    int result = -EAGAIN;
    if (queue->tail != queue->head) {
        unsigned length = queue->sizes[queue->tail];
        if (length > capacity) {
            result = -EMSGSIZE;
        } else {
            memcpy(data, queue->bytes[queue->tail], length);
            queue->tail = (queue->tail + 1) % WIFI_PACKET_QUEUE;
            result = (int)length;
        }
    }
    spin_unlock_irqrestore(&wifi.packets_lock, flags);
    return result;
}

static int wifi_send(device_t *device, const void *packet, size_t length)
{
    (void)device;
    return wifi.carrier ? packet_push(&wifi.tx, packet, length) : -ENETDOWN;
}
static int wifi_recv(device_t *device, void *packet, size_t length)
{
    (void)device;
    return packet_pop(&wifi.rx, packet, length);
}
static const uint8_t *wifi_mac(device_t *device) { (void)device; return wifi.mac; }
static int wifi_carrier(device_t *device) { (void)device; return wifi.carrier; }
static int wifi_polling(device_t *device) { (void)device; return 0; }

static void wifi_bus_master(int enabled)
{
    uint16_t command = pci_cfg_read16(wifi.device, PCI_COMMAND);
    command |= PCI_INTX_DISABLE | 2;
    command = enabled ? command | PCI_MASTER : command & ~PCI_MASTER;
    pci_cfg_write16(wifi.device, PCI_COMMAND, command);
}

static int wifi_transport_available(void)
{
    return !wifi.device && !wifi.owner && !wifi.quarantined && !wifi.dma_bytes;
}

static int wifi_probe_locked(device_t *device)
{
    if (!wifi_transport_available())
        return -EBUSY;
    /* The macOS platform normally supplies a D0 device to this driver.
     * Perform that platform step here before touching the radio's BAR. */
    uint16_t command = pci_cfg_read16(device, PCI_COMMAND);
    pci_cfg_write16(device, PCI_COMMAND, (command | PCI_INTX_DISABLE) & ~PCI_MASTER);
    uint8_t pm = pci_find_capability(device, 1); /* PCI Power Management */
    if (pm) {
        uint16_t state = pci_cfg_read16(device, pm + 4);
        if (state == 0xffff)
            return -ENODEV;
        if (state & 3) {
            uint32_t bars[6];
            for (unsigned i = 0; i < 6; i++)
                bars[i] = pci_cfg_read32(device, 0x10 + i * 4);
            /* Writing zero leaves PME_Status unchanged and disables PME. */
            pci_cfg_write16(device, pm + 4, 0);
            mdelay(10);
            if (pci_cfg_read16(device, pm + 4) & 3) {
                kerr("[WIFI] PCI power transition D%u -> D0 failed\n", state & 3);
                return -EIO;
            }
            /* D3hot -> D0 can reset BARs. Preserve firmware bridge routing. */
            for (unsigned i = 0; i < 6; i++)
                pci_cfg_write32(device, 0x10 + i * 4, bars[i]);
            kinfo("[WIFI] PCI power transition D%u -> D0 complete\n", state & 3);
        }
    }
    if (pci_enable_and_assign_bars(device) < 0)
        return -ENODEV;
    pci_cfg_write16(device, PCI_COMMAND,
                     (pci_cfg_read16(device, PCI_COMMAND) | PCI_INTX_DISABLE) & ~PCI_MASTER);
    resource_t *bar = pci_get_bar_resource(device, 2);
    if (!bar || bar->end < bar->start || bar->end - bar->start + 1 < 0x80000 ||
        (bar->start & (PAGE_SIZE - 1)))
        return -ENODEV;
    if (dma_set_mask(device, DMA_MASK_32BIT))
        return -EOPNOTSUPP;
    wifi.device = device;
    wifi.bar = bar->start;
    wifi.bar_size = bar->end - bar->start + 1;
    wifi.next_token = 1;
    mutex_init(&wifi.control);
    spin_init(&wifi.packets_lock);
    /* rtw89d reads the permanent address from efuse after firmware probe.
     * Do not manufacture an address and overwrite the real station identity. */
    memset(wifi.mac, 0, sizeof(wifi.mac));
    wifi.mac_ready = 0;
    wifi_bus_master(0);
    kinfo("[WIFI] RTL8852BE transport ready: BAR2=%llx size=%llu DMA=32-bit; waiting for rtw89d\n",
          (unsigned long long)wifi.bar, (unsigned long long)wifi.bar_size);
    return 0;
}

static int wifi_probe(device_t *device)
{
    mutex_lock(&wifi_open_lock);
    int ret = wifi_probe_locked(device);
    mutex_unlock(&wifi_open_lock);
    return ret;
}

static int wifi_remove(device_t *device)
{
    (void)device;
    mutex_lock(&wifi_open_lock);
    mutex_lock(&wifi.control);
    wifi.carrier = 0;
    wifi_bus_master(0);
    wifi.device = NULL;
    wifi.quarantined = wifi.owner != NULL || wifi.dma_bytes != 0;
    mutex_unlock(&wifi.control);
    mutex_unlock(&wifi_open_lock);
    return 0;
}

static const net_dev_ops_t wifi_net_ops = {
    .send = wifi_send, .recv = wifi_recv, .mac = wifi_mac,
    .link_up = wifi_carrier, .rx_irq_driven = wifi_polling,
};
static const device_id_t wifi_ids[] = {
    {.vendor = 0x10ec, .device = 0xb852, .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY},
    {.vendor = 0x10ec, .device = 0xb85b, .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY},
    {0},
};
static driver_t wifi_driver = {
    .name = "rtw89-userspace", .bus = &pci_bus, .id_table = wifi_ids,
    .probe = wifi_probe, .remove = wifi_remove,
    .class_type = DEV_CLASS_NET, .class_ops = &wifi_net_ops,
};
DRIVER_REGISTER(wifi_driver);

/* Each physical mapping is shared with this one owner, omitted from fork,
 * and revoked before its DMA allocation can be reused. */
static int64_t wifi_map(mm_struct_t *mm, uint64_t physical, size_t length, int device)
{
    mm_seg_t *vma = mm_seg_new();
    if (!vma)
        return -ENOMEM;
    pte_t permissions = PTE_V | PTE_U | PTE_R | PTE_W | PTE_A | PTE_D | PTE_LEAF;
    if (device)
        permissions |= PTE_DEVICE;
    vma->vm_flags = VM_SHARED | VM_READ | VM_WRITE | VM_PFNMAP | VM_DONTFORK;
    vma->pte_flags = permissions;
    mm_tlb_invalidate_begin(mm);
    spin_lock(&mm->lock);
    vaddr_t address = mm_find_gap(mm, mm->mmap_base, length);
    size_t mapped = 0;
    if (!address)
        goto fail;
    while (mapped < length) {
        if (pt_map(mm->pgdir, address + mapped, physical + mapped, permissions) < 0)
            goto rollback;
        mapped += PAGE_SIZE;
    }
    vma->start = address;
    vma->end = address + length;
    mm_insert_vma(mm, vma);
    mm_tlb_note_change(mm, address, length);
    spin_unlock(&mm->lock);
    mm_tlb_invalidate_finish(mm);
    mm_vma_flush_deferred(mm);
    return address;
rollback:
    (void)pt_unmap(mm, address + mapped);
    while (mapped) {
        mapped -= PAGE_SIZE;
        (void)pt_unmap(mm, address + mapped);
    }
    mm_tlb_note_change(mm, address, length);
fail:
    spin_unlock(&mm->lock);
    mm_tlb_invalidate_finish(mm);
    vma_put(mm, vma);
    mm_vma_flush_deferred(mm);
    return -ENOMEM;
}

static void wifi_dma_free(struct wifi_dma *buffer)
{
    if (buffer->mapped)
        mm_munmap(wifi.owner, buffer->mapped, buffer->size);
    dma_free_coherent_aligned(buffer->memory, buffer->size, buffer->bus);
    wifi.dma_bytes -= buffer->size;
    memset(buffer, 0, sizeof(*buffer));
}

static int wifi_ioctl(vfile_t *file, unsigned long request, void *arg)
{
    (void)file;
    if (proc_current()->mm != wifi.owner)
        return -EPERM;
    mutex_lock(&wifi.control);
    int result = -ENOTTY;
    if (!wifi.device) {
        result = -ENODEV;
    } else if (request == A20_WIFI_INFO) {
        uint32_t subsystem = pci_cfg_read32(wifi.device, 0x2c);
        struct a20_wifi_info info = {
            .version = A20_WIFI_ABI, .vendor = 0x10ec,
            .device = pci_cfg_read16(wifi.device, 2),
            .subvendor = subsystem, .subdevice = subsystem >> 16,
            .revision = pci_cfg_read8(wifi.device, 8), .mmio_size = wifi.bar_size,
        };
        memcpy(info.mac, wifi.mac, sizeof(info.mac));
        result = copy_to_user(arg, &info, sizeof(info)) < 0 ? -EFAULT : 0;
    } else if (request == A20_WIFI_CFG_READ || request == A20_WIFI_CFG_WRITE) {
        struct a20_wifi_cfg cfg;
        result = -EFAULT;
        if (copy_from_user(&cfg, arg, sizeof(cfg)) >= 0) {
            result = -EINVAL;
            if ((cfg.width == 1 || cfg.width == 2 || cfg.width == 4) &&
                cfg.offset < 4096 && !(cfg.offset & (cfg.width - 1)) && !cfg.reserved) {
                if (request == A20_WIFI_CFG_READ) {
                    cfg.value = pci_cfg_read32(wifi.device, cfg.offset & ~3U);
                    cfg.value >>= (cfg.offset & 3) * 8;
                    if (cfg.width != 4)
                        cfg.value &= (1U << (cfg.width * 8)) - 1;
                    result = copy_to_user(arg, &cfg, sizeof(cfg)) < 0 ? -EFAULT : 0;
                } else {
                    /* Keep PCI interrupts disabled. The service polls causes. */
                    if (cfg.offset == PCI_COMMAND)
                        cfg.value |= PCI_INTX_DISABLE;
                    else if (cfg.offset == PCI_COMMAND + 1 && cfg.width == 1)
                        cfg.value |= PCI_INTX_DISABLE >> 8;
                    if (cfg.width == 4)
                        pci_cfg_write32(wifi.device, cfg.offset, cfg.value);
                    else if (cfg.width == 2)
                        pci_cfg_write16(wifi.device, cfg.offset, cfg.value);
                    else {
                        uint16_t old = pci_cfg_read16(wifi.device, cfg.offset & ~1U);
                        unsigned shift = (cfg.offset & 1) * 8;
                        pci_cfg_write16(wifi.device, cfg.offset & ~1U,
                                        (old & ~(0xffU << shift)) | (cfg.value & 0xffU) << shift);
                    }
                    result = 0;
                }
            }
        }
    } else if (request == A20_WIFI_DMA_ALLOC) {
        struct a20_wifi_dma allocation;
        result = -EFAULT;
        if (copy_from_user(&allocation, arg, sizeof(allocation)) >= 0) {
            result = -EINVAL;
            if (allocation.size && allocation.size <= (4U << 20) &&
                !allocation.token && !allocation.bus_address) {
                size_t size = ROUND_UP(allocation.size, PAGE_SIZE);
                result = -ENOMEM;
                for (unsigned i = 0; i < WIFI_DMA_SLOTS && wifi.dma_bytes + size <= WIFI_DMA_BUDGET; i++) {
                    struct wifi_dma *buffer = &wifi.dma[i];
                    if (buffer->memory)
                        continue;
                    buffer->memory = dma_alloc_coherent_aligned(wifi.device, size, PAGE_SIZE, &buffer->bus);
                    if (!buffer->memory)
                        break;
                    memset(buffer->memory, 0, size);
                    buffer->size = size;
                    buffer->token = wifi.next_token++;
                    wifi.dma_bytes += size;
                    allocation = (struct a20_wifi_dma){size, buffer->token, buffer->bus};
                    result = copy_to_user(arg, &allocation, sizeof(allocation)) < 0 ? -EFAULT : 0;
                    if (result)
                        wifi_dma_free(buffer);
                    break;
                }
                if (result == -ENOMEM)
                    kwarn("[WIFI] DMA allocation refused: request=%lu resident=%lu budget=%u mask=%llx\n",
                          (unsigned long)size, (unsigned long)wifi.dma_bytes,
                          WIFI_DMA_BUDGET, (unsigned long long)dma_get_mask(wifi.device));
            }
        }
    } else if (request == A20_WIFI_DMA_FREE) {
        struct a20_wifi_dma allocation;
        result = -EFAULT;
        if (copy_from_user(&allocation, arg, sizeof(allocation)) >= 0) {
            result = -ENOENT;
            for (unsigned i = 0; i < WIFI_DMA_SLOTS; i++)
                if (wifi.dma[i].memory && wifi.dma[i].token == allocation.token) {
                    wifi_dma_free(&wifi.dma[i]);
                    result = 0;
                    break;
                }
        }
    } else if (request == A20_WIFI_HWADDR) {
        struct a20_wifi_hwaddr address;
        result = copy_from_user(&address, arg, sizeof(address)) < 0 ? -EFAULT : 0;
        if (!result && (address.reserved[0] || address.reserved[1] ||
            (address.mac[0] & 1U) ||
            !(address.mac[0] | address.mac[1] | address.mac[2] |
              address.mac[3] | address.mac[4] | address.mac[5])))
            result = -EINVAL;
        if (!result && (wifi.carrier || (wifi.mac_ready && memcmp(address.mac, wifi.mac, 6))))
            result = -EBUSY;
        if (!result)
            result = net_stack_device_hwaddr(wifi.device, address.mac);
        if (!result) {
            memcpy(wifi.mac, address.mac, 6);
            wifi.mac_ready = 1;
            kinfo("[WIFI] permanent station address published to network interface\n");
        }
    } else if (request == A20_WIFI_LINK) {
        struct a20_wifi_link link;
        result = copy_from_user(&link, arg, sizeof(link)) < 0 ? -EFAULT : 0;
        if (!result && (link.authorized > 1 || link.reserved))
            result = -EINVAL;
        if (!result && link.authorized && !wifi.mac_ready)
            result = -ENODATA;
        if (!result && wifi.carrier != (int)link.authorized) {
            wifi.carrier = link.authorized;
            kinfo("[WIFI] authenticated carrier=%u\n", link.authorized);
        }
    } else if (request == A20_WIFI_QUIESCE) {
        wifi.carrier = 0;
        wifi_bus_master(0);
        result = 0;
    }
    mutex_unlock(&wifi.control);
    return result;
}

static int64_t wifi_mmap(vfile_t *file, uint64_t address, size_t length,
                          int prot, int flags, uint64_t offset)
{
    (void)file;
    if (proc_current()->mm != wifi.owner)
        return -EPERM;
    if (address || prot != (PROT_READ | PROT_WRITE) ||
        (flags & ~MAP_SHARED) || !(flags & MAP_SHARED))
        return -EINVAL;
    mutex_lock(&wifi.control);
    int64_t result = -EINVAL;
    if (!offset && !wifi.mmio_va && length == wifi.bar_size && wifi.device) {
        result = wifi_map(wifi.owner, wifi.bar, length, 1);
        if (result > 0)
            wifi.mmio_va = result;
    } else if (offset && !(offset % A20_WIFI_DMA_OFFSET)) {
        uint64_t token = offset / A20_WIFI_DMA_OFFSET;
        for (unsigned i = 0; i < WIFI_DMA_SLOTS; i++) {
            struct wifi_dma *buffer = &wifi.dma[i];
            if (buffer->memory && buffer->token == token && buffer->size == length && !buffer->mapped) {
                result = wifi_map(wifi.owner, va_to_pa(buffer->memory), length, 0);
                if (result > 0)
                    buffer->mapped = result;
                break;
            }
        }
    }
    mutex_unlock(&wifi.control);
    return result;
}

static int wifi_read(vfile_t *file, char *data, size_t length)
{
    (void)file;
    if (proc_current()->mm != wifi.owner)
        return -EPERM;
    return packet_pop(&wifi.tx, data, length);
}
static int wifi_write(vfile_t *file, const char *data, size_t length)
{
    (void)file;
    if (proc_current()->mm != wifi.owner)
        return -EPERM;
    return wifi.carrier ? packet_push(&wifi.rx, data, length) : -ENETDOWN;
}

static int wifi_close(vfile_t *file)
{
    (void)file;
    mutex_lock(&wifi_open_lock);
    mutex_lock(&wifi.control);
    wifi.carrier = 0;
    if (wifi.device)
        wifi_bus_master(0);
    /* If the service died with DMA outstanding, retain the backing memory
     * and refuse another owner until reboot. Hardware never sees reused RAM. */
    if (wifi.dma_bytes) {
        wifi.quarantined = 1;
        kwarn("[WIFI] service closed with %lu DMA bytes outstanding; memory quarantined\n",
              (unsigned long)wifi.dma_bytes);
        for (unsigned i = 0; i < WIFI_DMA_SLOTS; i++)
            if (wifi.dma[i].mapped)
                mm_munmap(wifi.owner, wifi.dma[i].mapped, wifi.dma[i].size);
    }
    if (wifi.mmio_va)
        mm_munmap(wifi.owner, wifi.mmio_va, wifi.bar_size);
    mm_struct_t *owner = wifi.owner;
    wifi.owner = NULL;
    wifi.mmio_va = 0;
    uint64_t flags = spin_lock_irqsave(&wifi.packets_lock);
    memset(&wifi.tx, 0, sizeof(wifi.tx));
    memset(&wifi.rx, 0, sizeof(wifi.rx));
    spin_unlock_irqrestore(&wifi.packets_lock, flags);
    mutex_unlock(&wifi.control);
    mm_destroy(owner);
    mutex_unlock(&wifi_open_lock);
    return 0;
}

static vfile_ops_t wifi_file_ops = {
    .read = wifi_read, .write = wifi_write, .ioctl = wifi_ioctl,
    .mmap = wifi_mmap, .close = wifi_close,
};

int rtw89_file_open(vfile_t *file)
{
    /* CAP_SYS_RAWIO in the initial namespace, as for other raw PCI services. */
    if (!userns_capable(proc_current(), userns_init_ns(), 17))
        return -EPERM;
    mutex_lock(&wifi_open_lock);
    int result = -ENODEV;
    if (wifi.device && !wifi.quarantined) {
        mutex_lock(&wifi.control);
        result = -EBUSY;
        if (!wifi.owner) {
            wifi.owner = mm_get(proc_current()->mm);
            wifi_bus_master(1);
            file->ops = &wifi_file_ops;
            result = 0;
        }
        mutex_unlock(&wifi.control);
    }
    mutex_unlock(&wifi_open_lock);
    return result;
}
