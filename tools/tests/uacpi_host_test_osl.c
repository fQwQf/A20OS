/* Single-thread host fixture services; no hardware or kernel OSL is emulated. */
#include "core/types.h"
#include <uacpi/kernel_api.h>
#include <stdlib.h>
#include <assert.h>
#include <time.h>
void *kmalloc(size_t n) { return malloc(n); }
void kfree(void *p) { free(p); }
void *uacpi_kernel_alloc(uacpi_size n) { return malloc(n); }
void uacpi_kernel_free(void *p) { free(p); }
uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC, &t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;
}
void uacpi_kernel_stall(uacpi_u8 n) { (void)n; }
void uacpi_kernel_sleep(uacpi_u64 n) { (void)n; }
uacpi_handle uacpi_kernel_create_semaphore(uacpi_u32 n) {
    unsigned *p=malloc(sizeof(*p));if(p)*p=n;return p;
}
void uacpi_kernel_free_semaphore(uacpi_handle h) { free(h); }
uacpi_status uacpi_kernel_wait_for_semaphore(uacpi_handle h,uacpi_u16 t) {
    (void)t;unsigned *p=h;if(!*p)return UACPI_STATUS_TIMEOUT;--*p;return UACPI_STATUS_OK;
}
void uacpi_kernel_signal_semaphore(uacpi_handle h) { ++*(unsigned*)h; }
uacpi_thread_id uacpi_kernel_get_thread_id(void) { return (uacpi_thread_id)1; }
uacpi_interrupt_state uacpi_kernel_disable_interrupts(void) { return 0; }
void uacpi_kernel_restore_interrupts(uacpi_interrupt_state s) { (void)s; }
uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request *r) {
    (void)r;return UACPI_STATUS_UNIMPLEMENTED;
}
uacpi_status uacpi_kernel_install_interrupt_handler(uacpi_u32 i,uacpi_interrupt_handler h,
                                                     uacpi_handle c,uacpi_handle *o) {
    (void)i;(void)h;(void)c;(void)o;return UACPI_STATUS_UNIMPLEMENTED;
}
uacpi_status uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler h,uacpi_handle o) {
    (void)h;(void)o;return UACPI_STATUS_UNIMPLEMENTED;
}
uacpi_handle uacpi_kernel_create_spinlock(void) { return malloc(1); }
void uacpi_kernel_free_spinlock(uacpi_handle h) { free(h); }
uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle h) { (void)h;return 0; }
void uacpi_kernel_unlock_spinlock(uacpi_handle h,uacpi_cpu_flags f) { (void)h;(void)f; }
uacpi_handle uacpi_kernel_create_work_item(void) { return malloc(1); }
void uacpi_kernel_free_work_item(uacpi_handle h) { free(h); }
void uacpi_kernel_schedule_work(uacpi_work_type t,uacpi_handle w,uacpi_work_handler h,uacpi_handle c) {
    (void)t;(void)w;h(c);
}
uacpi_status uacpi_kernel_wait_for_work_completion(void) { return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address a,uacpi_handle *h) {
    (void)a;(void)h;return UACPI_STATUS_NOT_FOUND;
}
void uacpi_kernel_pci_device_close(uacpi_handle h) { (void)h; }
#define PCI(width) \
uacpi_status uacpi_kernel_pci_read##width(uacpi_handle h,uacpi_size o,uacpi_u##width *v) \
{ (void)h;(void)o;*v=(uacpi_u##width)-1;return UACPI_STATUS_OK; } \
uacpi_status uacpi_kernel_pci_write##width(uacpi_handle h,uacpi_size o,uacpi_u##width v) \
{ (void)h;(void)o;(void)v;return UACPI_STATUS_NOT_FOUND; }
PCI(8) PCI(16) PCI(32)
uacpi_status uacpi_kernel_io_map(uacpi_io_addr b,uacpi_size l,uacpi_handle *o) {
    (void)b;(void)l;(void)o;return UACPI_STATUS_UNIMPLEMENTED;
}
void uacpi_kernel_io_unmap(uacpi_handle h) { (void)h; }
#define IO(width) \
uacpi_status uacpi_kernel_io_read##width(uacpi_handle h,uacpi_size o,uacpi_u##width *v) \
{ (void)h;(void)o;(void)v;return UACPI_STATUS_UNIMPLEMENTED; } \
uacpi_status uacpi_kernel_io_write##width(uacpi_handle h,uacpi_size o,uacpi_u##width v) \
{ (void)h;(void)o;(void)v;return UACPI_STATUS_UNIMPLEMENTED; }
IO(8) IO(16) IO(32)
unsigned arch_acpi_interrupt_model(void) { return 4; }
