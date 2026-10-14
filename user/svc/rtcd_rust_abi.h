#ifndef A20_RTCD_RUST_ABI_H
#define A20_RTCD_RUST_ABI_H

#include <stdint.h>

int64_t a20_rtcd_init(uint64_t *rtc_base, uint32_t *stdout_handle);
int64_t a20_rtcd_queue_create(uint32_t *queue);
int64_t a20_rtcd_watch(uint32_t queue, uint32_t handle, uint64_t tag);
int64_t a20_rtcd_irq_listen(uint32_t queue, uint64_t tag);
int64_t a20_rtcd_recv(uint32_t endpoint, uint8_t *bytes, uint32_t *length);
int64_t a20_rtcd_send(uint32_t endpoint, const uint8_t *bytes, uint32_t length);
int64_t a20_rtcd_wait(uint32_t queue, uint64_t *tag);
uint64_t a20_rtcd_read_ns(uint64_t base);
void a20_rtcd_set_alarm(uint64_t base, uint64_t deadline_ns);
void a20_rtcd_clear_alarm(uint64_t base);
int64_t a20_rtcd_irq_ack(void);
void a20_rtcd_log(uint32_t handle, const uint8_t *bytes, uint32_t length);

#endif
