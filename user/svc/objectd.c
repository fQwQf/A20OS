/* Optional owner shard for globally named Native task identities. */
#include "liba20rt/a20_objects.h"
#include "liba20rt/crt0_a20.h"
#define RECORDS 32u
#define CONNECTIONS 8u
static a20_object_record_t records[RECORDS];
static a20_object_directory_t directory;
static a20_handle_t local[CONNECTIONS], exported;
static int identify(void *ctx, uintptr_t object, a20_handle_identity_t *out) {
    (void)ctx;
    return a20_hdl_identity((a20_handle_t)object, out) < 0 ? -1 : 0;
}
static void release(void *ctx, uintptr_t object) {
    (void)ctx;
    a20_hdl_close((a20_handle_t)object);
}
static void close_local(unsigned i) {
    a20_object_directory_disconnect(&directory, (uint64_t)i + 1);
    a20_hdl_close(local[i]);
    local[i] = A20_HANDLE_NULL;
}
int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;
    a20_runtime_info_t runtime;
    a20_cluster_snapshot_args_t self;
    if (a20_runtime_info(&runtime) < 0 || runtime.state != A20_RUNTIME_BOUND ||
        a20_cluster_snapshot(&self) < 0 ||
        !a20_runtime_id_equal(runtime.domain_id, self.node_id))
        return 2;
    if (a20_object_directory_init(&directory, &runtime, records, RECORDS,
                                  identify, release, 0))
        return 2;
    a20_channel_pair_t pair;
    if (a20_channel_create(&pair) < 0 ||
        a20_cluster_export(pair.endpoints[0], A20_OBJECT_SERVICE, 0, 0) < 0)
        return 3;
    a20_hdl_close(pair.endpoints[1]);
    exported = pair.endpoints[0];
    for (unsigned i = 0; i < CONNECTIONS; i++)
        local[i] = A20_HANDLE_NULL;
    a20_start_info_t *start = a20_get_start_info();
    if (start && start->stdout_handle != A20_HANDLE_NULL)
        a20_hdl_write_buf(start->stdout_handle, "OBJECTD: ready\n", 15, 0);
    int result = 0;
    for (;;) {
        if (a20_signal_check() & (1ULL << 15))
            break;
        uint64_t now = a20_agent_clock_ms(0);
        a20_object_directory_expire(&directory, now);
        for (unsigned i = 0; i <= CONNECTIONS; i++) {
            a20_handle_t ep = i ? local[i - 1] : exported;
            if (ep == A20_HANDLE_NULL)
                continue;
            uint8_t input[96], reply[96];
            uint32_t n = 96, count = 1;
            a20_handle_t incoming = A20_HANDLE_NULL;
            a20_status_t status = a20_channel_recv_flags(
                ep, input, &n, &incoming, &count, A20_MSG_NONBLOCK);
            if (status == -A20_ERR_WOULD_BLOCK)
                continue;
            if (status < 0) {
                if (!i) {
                    result = 4;
                    goto done;
                }
                close_local(i - 1);
                continue;
            }
            if (!i && count && !n) {
                for (unsigned j = 0; j < CONNECTIONS; j++)
                    if (local[j] == A20_HANDLE_NULL) {
                        local[j] = incoming;
                        incoming = A20_HANDLE_NULL;
                        break;
                    }
                if (incoming != A20_HANDLE_NULL)
                    a20_hdl_close(incoming);
                continue;
            }
            int bytes = a20_object_directory_dispatch(
                &directory, i ? i : 0, (uintptr_t)incoming, count != 0, input,
                n, reply, 96, a20_agent_clock_ms(0));
            if (bytes < 0 ||
                a20_channel_send_flags(ep, reply, (uint32_t)bytes, 0, 0,
                                       A20_MSG_NONBLOCK) < 0) {
                if (!i) {
                    result = 5;
                    goto done;
                }
                close_local(i - 1);
            }
        }
        a20_thread_sleep((a20_time_t){.nsecs = 1000000});
    }
done:
    for (unsigned i = 0; i < CONNECTIONS; i++)
        if (local[i] != A20_HANDLE_NULL)
            close_local(i);
    for (unsigned i = 0; i < RECORDS; i++)
        a20_object_directory_remove(&directory, i);
    a20_hdl_close(exported);
    return result;
}
