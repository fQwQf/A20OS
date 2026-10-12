#ifndef A20_NATIVE_SPAWN_CONTRACT_H
#define A20_NATIVE_SPAWN_CONTRACT_H
#include "liba20rt/a20_task.h"
#include "liba20rt/a20_fs.h"
#include "liba20rt/a20_handle.h"
#include "liba20rt/a20_runtime.h"
static int native_spawn_contract(void) {
#define S_CHECK(x, code)                                                       \
    do {                                                                       \
        if (!(x))                                                              \
            return code;                                                       \
    } while (0)
    a20_path_open_args_t open = {.dir = A20_HANDLE_NULL,
                                 .path = (uintptr_t)"/bin/SPAWN.ELF",
                                 .path_len = 14};
    S_CHECK(a20_path_open(&open) == A20_OK, 1);
    a20_handle_t image = open.out_handle;
    char *argv[] = {"spawn", "hello cluster", "", "37", 0};
    char *envp[] = {"A20_SPAWN=1", "EMPTY=", 0};
    a20_task_spawn_args_t args = {.image = image,
                                  .root_dir = A20_HANDLE_NULL,
                                  .cwd_dir = A20_HANDLE_NULL,
                                  .event_queue = A20_HANDLE_NULL,
                                  .argv = (uintptr_t)argv,
                                  .envp = (uintptr_t)envp,
                                  .argc = 4,
                                  .envc = 2,
                                  .stdin_handle = A20_HANDLE_NULL,
                                  .stdout_handle = A20_HANDLE_NULL,
                                  .stderr_handle = A20_HANDLE_NULL};
    a20_task_spawn_args_t bad = args;
    bad.size = sizeof(bad);
    bad.version = 3;
    S_CHECK(a20_syscall6(A20_SYS_task_spawn, (uintptr_t)&bad, 0, 0, 0, 0, 0) ==
                -A20_ERR_INVALID_ARGUMENT,
            2);
    bad.version = 2;
    bad.flags = 0x80000000U;
    S_CHECK(a20_syscall6(A20_SYS_task_spawn, (uintptr_t)&bad, 0, 0, 0, 0, 0) ==
                -A20_ERR_INVALID_ARGUMENT,
            3);
#ifdef A20_SPAWN_FLAT_TEST
    bad = args;
    bad.size = sizeof(bad);
    bad.version = 2;
    bad.argv = 1;
    S_CHECK(a20_syscall6(A20_SYS_task_spawn, (uintptr_t)&bad, 0, 0, 0, 0, 0) ==
                -A20_ERR_FAULT,
            4);
    bad = args;
    bad.size = sizeof(bad);
    bad.version = 2;
    bad.handle_count = 1;
    S_CHECK(a20_syscall6(A20_SYS_task_spawn, (uintptr_t)&bad, 0, 0, 0, 0, 0) ==
                -A20_ERR_NOT_SUPPORTED,
            5);
#endif
    a20_object_id_t previous = {0};
    for (unsigned i = 0; i < 2; i++) {
        args.size = i ? sizeof(args) : 72; /* genuine old v1 and current v2 */
        args.version = i + 1;
        args.flags = i ? A20_SPAWN_PARENT_BOUND : 0;
        a20_status_t handle =
            a20_syscall6(A20_SYS_task_spawn, (uintptr_t)&args, 0, 0, 0, 0, 0);
        S_CHECK(handle >= 0 && (a20_handle_t)handle == args.out_task, 6);
        a20_handle_identity_t identity;
        S_CHECK(a20_hdl_identity(args.out_task, &identity) == A20_OK &&
                    a20_object_id_valid(&identity.id) &&
                    identity.id.serial > previous.serial,
                7);
        previous = identity.id;
        a20_task_status_t status;
        S_CHECK(a20_task_wait(args.out_task, 0, &status) == A20_OK &&
                    status.size == sizeof(status) && status.version == 1 &&
                    status.exit_code == 37,
                8);
        S_CHECK(a20_hdl_identity(args.out_task, &identity) == A20_OK &&
                    identity.state == A20_OBJECT_RETIRED &&
                    a20_object_id_equal(&previous, &identity.id),
                9);
        S_CHECK(a20_hdl_close(args.out_task) == A20_OK, 10);
    }
    S_CHECK(a20_hdl_close(image) == A20_OK, 11);
    open.rights = A20_RIGHT_READ;
    S_CHECK(a20_path_open(&open) == A20_OK, 12);
    args.image = open.out_handle;
    S_CHECK(a20_task_spawn(&args) == -A20_ERR_ACCESS, 13);
    S_CHECK(a20_hdl_close(open.out_handle) == A20_OK, 14);
    open.rights = 0;
    open.path = (uintptr_t)"/bin/INPUT.DAT";
    S_CHECK(a20_path_open(&open) == A20_OK, 15);
    args.image = open.out_handle;
    S_CHECK(a20_task_spawn(&args) == -A20_ERR_IO, 16);
    S_CHECK(a20_hdl_close(open.out_handle) == A20_OK, 17);
    return 0;
#undef S_CHECK
}
#endif
