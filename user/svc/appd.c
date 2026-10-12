/* Optional single-owner application launcher. Manifest entries are explicit
 * command-line groups: UUID path machine bits endian ABI required-features.
 * Same manifest UUID may have several ISA variants. No architecture #ifdef. */
#include "liba20rt/a20_apps.h"
#include "liba20rt/crt0_a20.h"
#include "../../kernel/include/uapi/a20/cluster_app_manifest.h"
#define VARIANTS 8u
#define APPLICATIONS 8u
static a20_app_variant_t variants[VARIANTS];
static a20_app_record_t records[APPLICATIONS];
static a20_app_owner_t owner;
static a20_start_info_t *startup;
static a20_app_manifest_entry_t entries[VARIANTS];
static char manifest_bytes[A20_APP_MANIFEST_BYTES + 1];
static int load_manifest(const char *path, uint32_t *count) {
    a20_path_open_args_t file = {.dir = A20_HANDLE_NULL,
                                 .path = (uint64_t)(uintptr_t)path,
                                 .path_len = (uint32_t)a20_strlen(path),
                                 .rights = A20_RIGHT_READ};
    if (a20_path_open(&file) < 0)
        return -1;
    uint32_t used = 0;
    int result = -1;
    while (used < sizeof(manifest_bytes)) {
        uint64_t got = 0;
        if (a20_hdl_read_buf(file.out_handle, manifest_bytes + used,
                             sizeof(manifest_bytes) - used, &got) < 0 ||
            got > sizeof(manifest_bytes) - used)
            break;
        if (!got) {
            result = a20_app_manifest_parse(manifest_bytes, used, entries,
                                            VARIANTS, count);
            break;
        }
        used += (uint32_t)got;
    }
    a20_hdl_close(file.out_handle);
    return result;
}
static int start(void *ctx, uintptr_t image, uintptr_t *object,
                 a20_object_id_t *root) {
    (void)ctx;
    const a20_app_manifest_entry_t *entry = (const void *)image;
    const char *path = entry->argv[0];
    a20_path_open_args_t file = {.dir = A20_HANDLE_NULL,
                                 .path = (uint64_t)(uintptr_t)path,
                                 .path_len = (uint32_t)a20_strlen(path),
                                 .rights = A20_RIGHT_READ | A20_RIGHT_EXEC};
    a20_status_t status = a20_path_open(&file);
    if (status < 0)
        return (int)status;
    a20_task_spawn_args_t args = {.size = sizeof(args),
                                  .version = 2,
                                  .image = file.out_handle,
                                  .root_dir = startup->root_dir,
                                  .cwd_dir = startup->cwd_dir,
                                  .event_queue = A20_HANDLE_NULL,
                                  .argv = (uint64_t)(uintptr_t)entry->argv,
                                  .argc = entry->argc,
                                  .envp = (uint64_t)(uintptr_t)entry->envp,
                                  .envc = entry->envc,
                                  .flags = A20_SPAWN_PARENT_BOUND,
                                  .stdin_handle = startup->stdin_handle,
                                  .stdout_handle = startup->stdout_handle,
                                  .stderr_handle = startup->stderr_handle};
    status = a20_syscall6(A20_SYS_task_spawn, (uintptr_t)&args, 0, 0, 0, 0, 0);
    a20_hdl_close(file.out_handle);
    if (status < 0)
        return (int)status;
    *object = args.out_task;
    a20_handle_identity_t identity;
    if (a20_hdl_identity(args.out_task, &identity) < 0)
        return 1;
    *root = identity.id;
    return 0;
}
static int poll(void *ctx, uintptr_t object, int32_t *code) {
    (void)ctx;
    a20_task_status_t status = {.size = sizeof(status), .version = 1};
    a20_status_t result =
        a20_task_wait((a20_handle_t)object, A20_TASK_WAIT_NONBLOCK, &status);
    if (result == -A20_ERR_WOULD_BLOCK)
        return 0;
    if (result < 0)
        return -1;
    *code = status.exit_code;
    return 1;
}
static int stop(void *ctx, uintptr_t object, uint32_t reason) {
    (void)ctx;
    return a20_task_kill((a20_handle_t)object, reason) < 0 ? -1 : 0;
}
static void release(void *ctx, uintptr_t object) {
    (void)ctx;
    a20_hdl_close((a20_handle_t)object);
}
static int dispatch(void *ctx, const uint8_t *q, uint32_t n, uint8_t *reply,
                    uint32_t cap) {
    return a20_app_owner_dispatch(ctx, q, n, reply, cap);
}
int main(int argc, char **argv, char **envp) {
    (void)envp;
    startup = a20_get_start_info();
    if (!startup)
        return 2;
    uint32_t count = 0;
    if (argc == 3 && a20_app_text_equal(argv[1], "--manifest")) {
        if (load_manifest(argv[2], &count))
            return 2;
    } else {
        if (argc < 8 || (argc - 1) % 7 || (unsigned)(argc - 1) / 7 > VARIANTS)
            return 2;
        count = (unsigned)(argc - 1) / 7;
        for (unsigned i = 0; i < count; i++) {
            char **a = argv + 1 + i * 7;
            a20_app_manifest_entry_t *entry = &entries[i];
            if (a20_app_manifest_uuid(a[0], entry->variant.manifest) || !a[1] ||
                a[1][0] != '/')
                return 2;
            entry->argv[0] = a[1];
            entry->argc = 1;
            entry->variant.image = (uintptr_t)entry;
            a20_execution_requirement_t *v = &entry->variant.requirement;
            v->native_major = 1;
            if (a20_app_decimal(a[2], &v->elf_machine) ||
                a20_app_decimal(a[3], &v->pointer_bits) ||
                a20_app_decimal(a[4], &v->byte_order) ||
                a20_app_decimal(a[5], &v->calling_abi) ||
                a20_app_decimal(a[6], &v->required_features))
                return 2;
        }
    }
    for (unsigned i = 0; i < count; i++)
        variants[i] = entries[i].variant;
    a20_runtime_info_t runtime;
    a20_handle_identity_t identity;
    a20_execution_info_t execution;
    a20_cluster_snapshot_args_t self;
    if (a20_runtime_info(&runtime) < 0 || runtime.state != A20_RUNTIME_BOUND ||
        a20_cluster_snapshot(&self) < 0 ||
        !a20_runtime_id_equal(runtime.domain_id, self.node_id) ||
        a20_hdl_identity(startup->self_task, &identity) < 0 ||
        a20_execution_info(&execution) < 0 ||
        a20_app_owner_init(&owner, &identity.id, &execution, variants, count,
                           records, APPLICATIONS, 0, start, poll, stop,
                           release))
        return 3;
    a20_channel_pair_t pair;
    if (a20_channel_create(&pair) < 0 ||
        a20_cluster_export(pair.endpoints[0], A20_APP_SERVICE, 0, 0) < 0)
        return 4;
    a20_hdl_close(pair.endpoints[1]);
    a20_cluster_agent_server_t server;
    a20_work_agent_t unused = {0};
    a20_cluster_agent_init(&server, pair.endpoints[0], &unused);
    server.dispatch = dispatch;
    server.dispatch_ctx = &owner;
    if (startup->stdout_handle != A20_HANDLE_NULL)
        a20_hdl_write_buf(startup->stdout_handle, "APPD: ready\n", 12, 0);
    int result = 0;
    for (;;) {
        if (a20_signal_check() & (1ULL << 15))
            break;
        a20_app_owner_poll(&owner);
        if (a20_cluster_agent_pump(&server) < 0) {
            result = 5;
            break;
        }
        a20_thread_sleep((a20_time_t){.nsecs = 1000000});
    }
    for (unsigned i = 0; i < APPLICATIONS; i++)
        if (records[i].has_object) {
            stop(0, records[i].object, 9);
            release(0, records[i].object);
        }
    for (unsigned i = 0; i < A20_AGENT_LOCAL_CONNECTIONS; i++)
        if (server.local[i] != A20_HANDLE_NULL)
            a20_hdl_close(server.local[i]);
    a20_hdl_close(server.exported);
    return result;
}
