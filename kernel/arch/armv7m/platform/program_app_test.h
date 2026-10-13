/* Explicit firmware test adapter for the production portable app owner.
 * Real loader/task references; no host-written state and no invented IDs. */
#include "core/runtime.h"
#include "proc/task_object.h"
#include "ipc/handle_table.h"
#include "uapi/a20/cluster_apps.h"
static a20_app_owner_t app_test_owner;
static a20_app_record_t app_test_record[1];
static a20_app_variant_t app_test_variant[1];
static a20_app_request_t app_test_request;
static a20_object_id_t app_test_root;
static unsigned app_test_starts, app_test_releases;
static size_t app_test_length;
static a20_object_id_t app_test_task_id(task_t *task) {
    struct a20_ht_internal *ht = a20_ht_create();
    CHECK(ht);
    CHECK(a20_handle_install_task(ht, task, A20_OBJ_TASK, A20_RIGHT_STAT) >= 0);
    a20_object_id_t id = {.kind = A20_OBJECT_KIND_TASK};
    CHECK(a20_runtime_object_namespace(&id));
    id.serial = a20_task_object_serial(task->a20_object);
    CHECK(a20_object_id_valid(&id));
    a20_ht_destroy(ht);
    return id;
}
static int app_test_start(void *ctx, uintptr_t image, uintptr_t *object,
                          a20_object_id_t *id) {
    (void)ctx;
    CHECK(image == (uintptr_t)mcu_test_elf);
    app_test_starts++;
    mcu_program_test_record_t startup = {0};
    int pid = mcu_program_spawn((const void *)image, app_test_length, 8192,
                                &startup, sizeof(startup), 2);
    if (pid < 0)
        return pid;
    program_task = proc_find_get(pid);
    CHECK(program_task && program_task->arch_program &&
          program_task->abi_mode == 1);
    *object = (uintptr_t)program_task;
    *id = app_test_task_id(program_task);
    return 0;
}
static int app_test_poll(void *ctx, uintptr_t object, int32_t *code) {
    (void)ctx;
    task_t *task = (task_t *)object;
    uint64_t flags = spin_lock_irqsave(&task->park_lock);
    /* A published task held by this owner can be detached (UNUSED) before
     * the reaper starts destruction. Its exit code is already final and the
     * reference keeps storage valid; destroy_started is not an exit fence. */
    int exited = (task->state == PROC_ZOMBIE || task->state == PROC_UNUSED) &&
                 !task->on_cpu;
    *code = task->exit_code;
    spin_unlock_irqrestore(&task->park_lock, flags);
    return exited;
}
static int app_test_stop(void *ctx, uintptr_t object, uint32_t reason) {
    (void)ctx;
    if (reason != 9)
        return -1;
    proc_force_exit((task_t *)object, 137);
    return 0;
}
static void app_test_release(void *ctx, uintptr_t object) {
    (void)ctx;
    app_test_releases++;
    proc_put((task_t *)object);
}
static a20_app_result_t app_test_call(void) {
    uint8_t input[A20_APP_REQUEST], reply[A20_APP_REPLY];
    a20_app_result_t result;
    app_test_request.cookie++;
    CHECK(!a20_app_request_encode(input, sizeof(input), &app_test_request));
    CHECK(a20_app_owner_dispatch(&app_test_owner, input, sizeof(input), reply,
                                 sizeof(reply)) == A20_APP_REPLY);
    CHECK(!a20_app_result_decode(reply, sizeof(reply), &app_test_request,
                                 &result));
    return result;
}
static void app_test_init(task_t *control, size_t length) {
    /* Same explicit fixture as native-runtime-contract.h. The application
     * still checks binding through actual SVC, including conflict rejection. */
    a20_runtime_binding_t binding = {.size = 64,
                                     .version = 1,
                                     .instance_id = {0xa2, 0x20, 1},
                                     .domain_id = {0xa2, 0x20, 2},
                                     .boot_epoch = 0x1020304050607080ULL};
    binding.domain_id[14] = 40;
    binding.domain_id[15] = 32;
    CHECK(!a20_runtime_bind(&binding));
    a20_object_id_t controller = app_test_task_id(control);
    a20_execution_info_t execution = {
        .size = 64,
        .version = 1,
        .elf_machine = 40,
        .pointer_bits = 32,
        .byte_order = 1,
        .calling_abi = 6,
        .native_major = 1,
        .features = A20_EXEC_THREAD_BASIC | A20_EXEC_NATIVE_IMAGE |
                    A20_EXEC_FUTEX | A20_EXEC_ATOMIC32 | A20_EXEC_USER_HEAP |
                    A20_EXEC_THREAD_CREATE,
        .memory_model = A20_EXEC_MEMORY_FLAT,
        .atomic_model = A20_EXEC_ATOMIC_FABRIC_GATE};
    app_test_variant[0] = (a20_app_variant_t){
        .manifest = {0xd1},
        .requirement = {40, 32, 1, 6, 1, 0, A20_EXEC_NATIVE_IMAGE},
        .image = (uintptr_t)mcu_test_elf};
    CHECK(!a20_app_owner_init(&app_test_owner, &controller, &execution,
                              app_test_variant, 1, app_test_record, 1, 0,
                              app_test_start, app_test_poll, app_test_stop,
                              app_test_release));
    app_test_request =
        (a20_app_request_t){.op = A20_APP_START,
                            .version = 2,
                            .token = a20_app_owner_offer(&app_test_owner),
                            .nonce = 1,
                            .controller = controller,
                            .client = controller,
                            .manifest = {0xd1}};
    app_test_length = length;
}
static void app_test_launch(void) {
    a20_app_result_t result = app_test_call();
    CHECK(!result.status && result.state == A20_APP_RUNNING);
    app_test_root = result.root;
    result = app_test_call();
    CHECK(!result.status && result.state == A20_APP_RUNNING &&
          a20_object_id_equal(&result.root, &app_test_root) &&
          app_test_starts == 1);
    printf(
        "[PROGRAM-APPS] PASS common owner real ELF launch dedup root=stable\n");
}
static void app_test_finished(void) {
    app_test_request.op = A20_APP_QUERY;
    a20_app_result_t result = app_test_call();
    CHECK(!result.status && result.state == A20_APP_EXITED &&
          result.exit_code == 0 && app_test_releases == 1 &&
          a20_object_id_equal(&result.root, &app_test_root));
    app_test_request.op = A20_APP_START;
    result = app_test_call();
    CHECK(!result.status && result.state == A20_APP_EXITED &&
          app_test_starts == 1);
    app_test_request.controller.serial++;
    result = app_test_call();
    CHECK(result.status == A20_APP_STALE && app_test_starts == 1);
    printf("[PROGRAM-APPS] PASS exit tombstone stale owner release=1\n");
    app_test_request.controller.serial--;
    app_test_request.op = A20_APP_RELEASE;
    result = app_test_call();
    CHECK(!result.status && !result.occupied && !result.active &&
          app_test_releases == 1);
    uint64_t retired = app_test_request.token;
    app_test_request.op = A20_APP_START;
    result = app_test_call();
    CHECK(result.status == A20_APP_STALE && app_test_starts == 1);
    app_test_request = (a20_app_request_t){.op = A20_APP_HELLO, .version = 2};
    result = app_test_call();
    CHECK(!result.status && result.token && result.token != retired &&
          !result.occupied && !result.active);
    printf("[PROGRAM-APPS] PASS recycle old START stale slot=available "
           "release=1\n");
}
