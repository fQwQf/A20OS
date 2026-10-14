#define DRV_ENV_USER 1
#include "drivers/dual/goldfish_rtc.h"
#include "liba20rt/a20_sdk.h"
#include "drivers/driver_descriptor.h"
#include "rtcd_rust_abi.h"

a20_start_info_t *a20_get_start_info(void);

A20_DRIVER_DESCRIPTOR(A20_DRIVER_PLACEMENT_USER_SERVICE,
                      A20_DRIVER_TYPE_RTC, "goldfish-rtc", A20_DRIVER_ABI,
                      A20_DRIVER_RES_MMIO | A20_DRIVER_RES_IRQ,
                      A20_DRIVER_FLAG_SUPERVISED, 1,
                      A20_DRIVER_MATCH(A20_DRIVER_BUS_MMIO, 0x101000UL, 0));

#define GOLDFISH_RTC_IRQ 11u
#define IRQ_TAG 0x52435449ULL

int64_t a20_rtcd_init(uint64_t *rtc_base, uint32_t *stdout_handle)
{
    a20_start_info_t *si = a20_get_start_info();
    *stdout_handle = si ? si->stdout_handle : A20_HANDLE_NULL;
    a20_status_t status = a20_device_claim(GOLDFISH_RTC_BASE);
    if (status < 0) return status;
    *rtc_base = grtc_map();
    return *rtc_base ? A20_OK : -1;
}

int64_t a20_rtcd_queue_create(uint32_t *queue)
{ return a20_event_queue_create(queue); }

int64_t a20_rtcd_watch(uint32_t queue, uint32_t handle, uint64_t tag)
{ return a20_event_watch(queue, handle, A20_EVENT_MASK(A20_EVENT_MESSAGE_READY), tag); }

int64_t a20_rtcd_irq_listen(uint32_t queue, uint64_t tag)
{ return a20_device_irq_listen(GOLDFISH_RTC_IRQ, queue, tag); }

int64_t a20_rtcd_recv(uint32_t endpoint, uint8_t *bytes, uint32_t *length)
{
    uint32_t handles = 0;
    return a20_channel_recv_flags(endpoint, bytes, length, 0, &handles, A20_MSG_NONBLOCK);
}

int64_t a20_rtcd_send(uint32_t endpoint, const uint8_t *bytes, uint32_t length)
{ return a20_channel_send(endpoint, bytes, length, 0, 0); }

int64_t a20_rtcd_wait(uint32_t queue, uint64_t *tag)
{
    a20_event_t event;
    a20_time_t infinite = { .secs = (uint64_t)-1, .nsecs = 0 };
    a20_status_t status = a20_event_wait(queue, infinite, &event);
    if (status >= 0) *tag = event.user_data;
    return status;
}

uint64_t a20_rtcd_read_ns(uint64_t base) { return grtc_read_ns(base); }
void a20_rtcd_set_alarm(uint64_t base, uint64_t deadline_ns)
{ grtc_set_alarm_ns(base, deadline_ns); }
void a20_rtcd_clear_alarm(uint64_t base) { grtc_clear_alarm(base); }
int64_t a20_rtcd_irq_ack(void) { return a20_device_irq_ack(GOLDFISH_RTC_IRQ); }
void a20_rtcd_log(uint32_t handle, const uint8_t *bytes, uint32_t length)
{ (void)a20_hdl_write_buf(handle, bytes, length, 0); }
