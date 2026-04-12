#include "types.h"
#include "stdio.h"
#include "panic.h"
#include "proc.h"
#include "vfs.h"
#include "elf.h"
#include "timer.h"
#include "virtio_blk.h"
#include "fs.h"

int klog_level = 0;

static task_t g_boot_task;

void klog_init(void) {}
void klog_clear(void) {}
size_t klog_len(void) { return 0; }
int klog_read(char *buf, size_t size, size_t *pos) {
    (void)buf;
    (void)size;
    (void)pos;
    return 0;
}

void klog_write(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

void timer_init(void) {}
void timer_set_interval(uint64_t ticks) { (void)ticks; }
uint64_t timer_get_ticks(void) { return 0; }
void timer_enable(void) {}
void timer_disable(void) {}

void plic_init(void) {}
void plic_init_hart(void) {}
uint32_t plic_claim(void) { return 0; }
void plic_complete(uint32_t irq) { (void)irq; }

void mm_init(void) {}
void vfs_init(void) {}
int virtio_blk_init(uintptr_t mmio_base) {
    (void)mmio_base;
    return -1;
}
int fs_mkdir(const char *path) {
    (void)path;
    return 0;
}
int vfs_mount(const char *dev, const char *path, const char *fstype, int flags) {
    (void)dev;
    (void)path;
    (void)fstype;
    (void)flags;
    return 0;
}

void proc_init(void) {
    g_boot_task.pid = 0;
}

task_t *proc_current(void) {
    return &g_boot_task;
}

int proc_alloc(void (*entry)(void)) {
    (void)entry;
    return 1;
}

int proc_alloc_user(uint64_t entry, uint64_t sp, uint64_t *pgdir) {
    (void)entry;
    (void)sp;
    (void)pgdir;
    return 1;
}

void sched(void) {}
void proc_yield(void) {}

void idle_loop(void) {
    for (;;) {
        __asm__ volatile("nop");
    }
}

int vfs_open(const char *path, int flags, int mode) {
    (void)path;
    (void)flags;
    (void)mode;
    return -1;
}

int vfs_close(int fd) {
    (void)fd;
    return 0;
}

int elf_load(int fd, elf_load_info_t *info) {
    (void)fd;
    if (info) {
        info->entry = 0;
        info->stack_top = 0;
        info->pgdir = (uint64_t *)0;
    }
    return -1;
}

void proc_exit(int exit_code) {
    (void)exit_code;
    for (;;) {
        __asm__ volatile("nop");
    }
}

void panic(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    for (;;) {
        __asm__ volatile("nop");
    }
}

void *kmalloc(size_t size) {
    (void)size;
    return (void *)0;
}

void kfree(void *ptr) {
    (void)ptr;
}

void *krealloc(void *ptr, size_t new_size) {
    (void)ptr;
    (void)new_size;
    return (void *)0;
}

void *kcalloc(size_t nmemb, size_t size) {
    (void)nmemb;
    (void)size;
    return (void *)0;
}
