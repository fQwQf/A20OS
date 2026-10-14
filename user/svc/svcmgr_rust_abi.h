#ifndef A20_SVCMGR_RUST_ABI_H
#define A20_SVCMGR_RUST_ABI_H

#include <stdint.h>

typedef uint32_t svcmgr_handle_t;

typedef struct {
    uint64_t user_data;
    uint64_t data0;
} svcmgr_event_t;

int64_t a20_svcmgr_startup(svcmgr_handle_t *out_stdout,
                           svcmgr_handle_t *out_root);
int64_t a20_svcmgr_registry_claim(void);
int64_t a20_svcmgr_event_queue_create(svcmgr_handle_t *out_queue);
int64_t a20_svcmgr_event_watch(svcmgr_handle_t queue, svcmgr_handle_t target,
                               uint64_t mask, uint64_t user_data);
int64_t a20_svcmgr_event_cancel(svcmgr_handle_t queue,
                                svcmgr_handle_t target);
/* Returns 1 for one event, 0 for timeout, and a negative SDK status on error. */
int64_t a20_svcmgr_event_wait(svcmgr_handle_t queue, uint64_t timeout_ns,
                              svcmgr_event_t *out_event);
int64_t a20_svcmgr_channel_create(svcmgr_handle_t out_endpoints[2]);
int64_t a20_svcmgr_path_open(const char *path, svcmgr_handle_t *out_image);
/* One syscall only: caller retains/cleans image and all endpoint handles. */
int64_t a20_svcmgr_task_spawn(svcmgr_handle_t image, svcmgr_handle_t root,
                              const char *const *argv, uint32_t argc,
                              svcmgr_handle_t service_child,
                              uint32_t service_slot,
                              svcmgr_handle_t ping_child, uint32_t ping_slot,
                              svcmgr_handle_t stdout_handle,
                              svcmgr_handle_t *out_task);
int64_t a20_svcmgr_channel_send(svcmgr_handle_t endpoint,
                                const void *bytes, uint32_t length);
int64_t a20_svcmgr_registry_recv(svcmgr_handle_t endpoint, void *bytes,
                                 uint32_t capacity, uint32_t *out_length,
                                 svcmgr_handle_t *handles,
                                 uint32_t handle_capacity,
                                 uint32_t *out_handle_count);
int64_t a20_svcmgr_registry_reply(svcmgr_handle_t endpoint, int64_t status,
                                  svcmgr_handle_t service_endpoint,
                                  uint32_t has_endpoint);
int64_t a20_svcmgr_close(svcmgr_handle_t handle);
int64_t a20_svcmgr_task_kill(svcmgr_handle_t task, uint32_t signal);
/* Returns 1 for exited, 0 for still running, or a negative SDK status. */
int64_t a20_svcmgr_task_wait(svcmgr_handle_t task, int32_t *out_exit_code);
uint64_t a20_svcmgr_now_ms(void);
int64_t a20_svcmgr_sleep_ms(uint64_t milliseconds);
void a20_svcmgr_write(svcmgr_handle_t output, const void *bytes,
                      uint32_t length);
/* Panic is fatal; this adapter exits the manager task and never returns. */
void a20_svcmgr_panic_exit(int32_t code);
/* Reads raw /proc/cmdline bytes; configuration policy belongs to Rust. */
int64_t a20_svcmgr_read_cmdline(uint8_t *out_bytes, uint32_t capacity,
                                uint32_t *out_length);

#endif
