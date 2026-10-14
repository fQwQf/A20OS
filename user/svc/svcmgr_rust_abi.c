/* Narrow Native SDK/syscall adapters for the Rust svcmgr state machine. */
#include "liba20rt/a20_sdk.h"
#include "a20_services_idl.h"
#include "svcmgr_rust_abi.h"

/* Defined once by rust_peer_crt0.c, alongside the shared Rust Native CRT. */
a20_start_info_t *a20_get_start_info(void);

int64_t a20_svcmgr_startup(svcmgr_handle_t *out_stdout,
                           svcmgr_handle_t *out_root)
{
    if (!out_stdout || !out_root)
        return -A20_ERR_INVALID_ARGUMENT;
    a20_start_info_t *info = a20_get_start_info();
    if (!info)
        return -A20_ERR_NOT_FOUND;
    *out_stdout = info->stdout_handle;
    *out_root = info->root_dir;
    return A20_OK;
}

int64_t a20_svcmgr_registry_claim(void)
{
    return a20_registry_claim();
}

int64_t a20_svcmgr_event_queue_create(svcmgr_handle_t *out_queue)
{
    return a20_event_queue_create(out_queue);
}

int64_t a20_svcmgr_event_watch(svcmgr_handle_t queue, svcmgr_handle_t target,
                               uint64_t mask, uint64_t user_data)
{
    return a20_event_watch(queue, target, mask, user_data);
}

int64_t a20_svcmgr_event_cancel(svcmgr_handle_t queue,
                                svcmgr_handle_t target)
{
    return a20_event_cancel(queue, target);
}

int64_t a20_svcmgr_event_wait(svcmgr_handle_t queue, uint64_t timeout_ns,
                              svcmgr_event_t *out_event)
{
    if (!out_event)
        return -A20_ERR_INVALID_ARGUMENT;
    a20_event_t event;
    a20_time_t timeout = {
        .secs = timeout_ns / 1000000000ULL,
        .nsecs = timeout_ns % 1000000000ULL,
    };
    a20_status_t status = a20_event_wait(queue, timeout, &event);
    if (status == -A20_ERR_TIMED_OUT)
        return 0;
    if (status < 0)
        return status;
    out_event->user_data = event.user_data;
    out_event->data0 = event.data0;
    return 1;
}

int64_t a20_svcmgr_channel_create(svcmgr_handle_t out_endpoints[2])
{
    if (!out_endpoints)
        return -A20_ERR_INVALID_ARGUMENT;
    a20_channel_pair_t pair = {{A20_HANDLE_NULL, A20_HANDLE_NULL}};
    a20_status_t status = a20_channel_create(&pair);
    if (status >= 0) {
        out_endpoints[0] = pair.endpoints[0];
        out_endpoints[1] = pair.endpoints[1];
    }
    return status;
}

int64_t a20_svcmgr_path_open(const char *path, svcmgr_handle_t *out_image)
{
    if (!path || !out_image)
        return -A20_ERR_INVALID_ARGUMENT;
    a20_path_open_args_t open_args = {0};
    open_args.size = sizeof(open_args);
    open_args.version = 1;
    open_args.dir = A20_HANDLE_NULL;
    open_args.rights = A20_RIGHT_READ | A20_RIGHT_EXEC;
    open_args.path = (uint64_t)(uintptr_t)path;
    open_args.path_len = (uint32_t)a20_strlen(path);
    open_args.out_handle = A20_HANDLE_NULL;
    a20_status_t status = a20_path_open(&open_args);
    if (status >= 0)
        *out_image = open_args.out_handle;
    return status;
}

int64_t a20_svcmgr_task_spawn(svcmgr_handle_t image, svcmgr_handle_t root,
                              const char *const *argv, uint32_t argc,
                              svcmgr_handle_t service_child,
                              uint32_t service_slot,
                              svcmgr_handle_t ping_child, uint32_t ping_slot,
                              svcmgr_handle_t stdout_handle,
                              svcmgr_handle_t *out_task)
{
    if (!argv || argc == 0 || !out_task)
        return -A20_ERR_INVALID_ARGUMENT;
    a20_spawn_handle_t handles[2] = {
        {.handle = service_child,
         .rights = A20_RIGHT_READ | A20_RIGHT_WRITE,
         .target_slot = service_slot, .flags = 0},
        {.handle = ping_child,
         .rights = A20_RIGHT_READ | A20_RIGHT_WRITE,
         .target_slot = ping_slot, .flags = 0},
    };
    a20_task_spawn_args_t spawn = {0};
    spawn.size = sizeof(spawn);
    spawn.version = 2;
    spawn.image = image;
    spawn.root_dir = root;
    spawn.cwd_dir = A20_HANDLE_NULL;
    spawn.event_queue = A20_HANDLE_NULL;
    spawn.argv = (uint64_t)(uintptr_t)argv;
    spawn.argc = argc;
    spawn.handles = (uint64_t)(uintptr_t)handles;
    spawn.handle_count = 2;
    spawn.out_task = A20_HANDLE_NULL;
    spawn.stdin_handle = A20_HANDLE_NULL;
    spawn.stdout_handle = stdout_handle;
    spawn.stderr_handle = stdout_handle;
    a20_status_t status = a20_syscall6(A20_SYS_task_spawn,
                          (uint64_t)(uintptr_t)&spawn, 0, 0, 0, 0, 0);
    if (status >= 0)
        *out_task = spawn.out_task;
    return status;
}

int64_t a20_svcmgr_channel_send(svcmgr_handle_t endpoint,
                                const void *bytes, uint32_t length)
{
    return a20_channel_send_flags(endpoint, bytes, length, 0, 0,
                                  A20_MSG_NONBLOCK);
}

int64_t a20_svcmgr_registry_recv(svcmgr_handle_t endpoint, void *bytes,
                                 uint32_t capacity, uint32_t *out_length,
                                 svcmgr_handle_t *handles,
                                 uint32_t handle_capacity,
                                 uint32_t *out_handle_count)
{
    if (!out_length || !out_handle_count)
        return -A20_ERR_INVALID_ARGUMENT;
    uint32_t len = capacity;
    uint32_t count = handle_capacity;
    a20_status_t status = a20_channel_recv_flags(endpoint, bytes, &len,
        handles, &count, A20_MSG_NONBLOCK);
    if (status >= 0) {
        *out_length = len;
        *out_handle_count = count;
    }
    return status;
}

int64_t a20_svcmgr_registry_reply(svcmgr_handle_t endpoint, int64_t result,
                                  svcmgr_handle_t service_endpoint,
                                  uint32_t has_endpoint)
{
    return a20_channel_send_flags(endpoint, &result, sizeof(result),
                                  has_endpoint ? &service_endpoint : 0,
                                  has_endpoint ? 1 : 0, A20_MSG_NONBLOCK);
}

int64_t a20_svcmgr_close(svcmgr_handle_t handle)
{
    if (handle == A20_HANDLE_NULL)
        return A20_OK;
    return a20_hdl_close(handle);
}

int64_t a20_svcmgr_task_kill(svcmgr_handle_t task, uint32_t signal)
{
    return a20_task_kill(task, signal);
}

int64_t a20_svcmgr_task_wait(svcmgr_handle_t task, int32_t *out_exit_code)
{
    if (!out_exit_code)
        return -A20_ERR_INVALID_ARGUMENT;
    a20_task_status_t status = {0};
    status.size = sizeof(status);
    status.version = 1;
    a20_status_t rc = a20_task_wait(task, A20_TASK_WAIT_NONBLOCK, &status);
    if (rc == -A20_ERR_WOULD_BLOCK)
        return 0;
    if (rc < 0)
        return rc;
    *out_exit_code = status.exit_code;
    return 1;
}

uint64_t a20_svcmgr_now_ms(void)
{
    a20_time_ns_t now = 0;
    if (a20_status_is_err(a20_clock_get(A20_CLOCK_MONOTONIC, &now)))
        return 0;
    return now / 1000000ULL;
}

int64_t a20_svcmgr_sleep_ms(uint64_t milliseconds)
{
    a20_time_t duration = {
        .secs = milliseconds / 1000,
        .nsecs = (milliseconds % 1000) * 1000000ULL,
    };
    return a20_thread_sleep(duration);
}

void a20_svcmgr_write(svcmgr_handle_t output, const void *bytes,
                      uint32_t length)
{
    (void)a20_hdl_write_buf(output, bytes, length, 0);
}

void a20_svcmgr_panic_exit(int32_t code)
{
    a20_task_exit(code);
}

int64_t a20_svcmgr_read_cmdline(uint8_t *out_bytes, uint32_t capacity,
                                uint32_t *out_length)
{
    if (!out_bytes || !capacity || !out_length)
        return -A20_ERR_INVALID_ARGUMENT;
    *out_length = 0;

    static const char path[] = "/proc/cmdline";
    a20_path_open_args_t open_args = {0};
    open_args.size = sizeof(open_args);
    open_args.version = 1;
    open_args.dir = A20_HANDLE_NULL;
    open_args.flags = A20_PATH_OPEN_RDONLY;
    open_args.rights = A20_RIGHT_READ;
    open_args.path = (uint64_t)(uintptr_t)path;
    open_args.path_len = sizeof(path) - 1;
    open_args.out_handle = A20_HANDLE_NULL;
    if (a20_path_open(&open_args) < 0)
        return A20_OK; /* retain the established default */

    uint64_t got = 0;
    a20_status_t status = a20_hdl_read_buf(open_args.out_handle, out_bytes,
                                            capacity, &got);
    a20_hdl_close(open_args.out_handle);
    if (status < 0)
        return A20_OK; /* same fallback as the C implementation */
    *out_length = got < capacity ? (uint32_t)got : capacity;
    return A20_OK;
}
