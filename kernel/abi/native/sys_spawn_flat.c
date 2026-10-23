/* Portable flat Native process launch. Storage and ISA adapters are separate;
 * a child cannot run until its own startup/handles and parent copyout succeed. */
#ifdef CONFIG_NATIVE_FLAT_FILES
#include "core/flat_program.h"
#include "core/errno.h"
#include "core/consts.h"
#include "core/string.h"
#include "mm/slab.h"
#include "fs/flat_files.h"
#include "proc/proc.h"
#include "ipc/handle_table.h"
#include "ipc/start_info.h"
#include "abi/native/syscall_entry.h"
#include "abi/native/errno.h"
#include "sys_validate.h"
#include "spawn_params.h"
#define FLAT_ARG_COUNT 32U
#define FLAT_ARG_BYTES 4096U
#define FLAT_STACK_BYTES 16384U
#define FLAT_IMAGE_BYTES (512U * 1024U)
typedef struct flat_args {
    uint32_t offset[FLAT_ARG_COUNT * 2];
    size_t used;
    char strings[FLAT_ARG_BYTES];
} flat_args_t;
static int copy_vector(uint64_t address, uint32_t count, uint32_t first,
                       flat_args_t *args) {
    if (!count)
        return 0;
    if (!address || address > UINTPTR_MAX ||
        address > UINTPTR_MAX - (count + 1) * sizeof(uintptr_t))
        return -A20_ERR_FAULT;
    uintptr_t *vector = (void *)(uintptr_t)address;
    for (uint32_t i = 0; i < count; i++) {
        uintptr_t string;
        if (copy_from_user(&string, vector + i, sizeof(string)) < 0 || !string)
            return -A20_ERR_FAULT;
        args->offset[first + i] = args->used;
        for (;;) {
            if (args->used == FLAT_ARG_BYTES)
                return -A20_ERR_INVALID_ARGUMENT;
            char c;
            if (copy_from_user(&c, (void *)string, 1) < 0)
                return -A20_ERR_FAULT;
            args->strings[args->used++] = c;
            if (!c)
                break;
            if (string == UINTPTR_MAX)
                return -A20_ERR_FAULT;
            string++;
        }
    }
    uintptr_t tail;
    if (copy_from_user(&tail, vector + count, sizeof(tail)) < 0)
        return -A20_ERR_FAULT;
    return tail ? -A20_ERR_INVALID_ARGUMENT : 0;
}
static uintptr_t prepare_start(const a20_flat_program_t *program,
                               const a20_task_spawn_args_t *args,
                               const flat_args_t *copied, a20_handle_t self) {
    uintptr_t top = (program->stack_base + program->stack_size - copied->used) &
                    ~(uintptr_t)15;
    memcpy((void *)top, copied->strings, copied->used);
    uintptr_t strings = top;
    top -= (args->envc + 1) * sizeof(uintptr_t);
    uintptr_t envp = top;
    uintptr_t *vector = (void *)top;
    for (uint32_t i = 0; i < args->envc; i++)
        vector[i] = strings + copied->offset[FLAT_ARG_COUNT + i];
    vector[args->envc] = 0;
    top -= (args->argc + 1) * sizeof(uintptr_t);
    uintptr_t argv = top;
    vector = (void *)top;
    for (uint32_t i = 0; i < args->argc; i++)
        vector[i] = strings + copied->offset[i];
    vector[args->argc] = 0;
    top = (top - sizeof(a20_start_info_t)) & ~(uintptr_t)15;
    a20_start_info_t info = {.size = sizeof(info),
                             .version = A20_START_INFO_VERSION,
                             .argc = args->argc,
                             .envc = args->envc,
                             .argv = argv,
                             .envp = envp,
                             .root_dir = A20_HANDLE_NULL,
                             .cwd_dir = A20_HANDLE_NULL,
                             .stdin_handle = A20_HANDLE_NULL,
                             .stdout_handle = A20_HANDLE_NULL,
                             .stderr_handle = A20_HANDLE_NULL,
                             .self_task = self,
                             .main_thread = A20_HANDLE_NULL,
                             .default_event_queue = A20_HANDLE_NULL,
                             .page_size = PAGE_SIZE,
                             .service_registry = A20_HANDLE_NULL};
    memcpy((void *)top, &info, sizeof(info));
    return top;
}
int64_t sys_a20_task_spawn(const a20_syscall_args_t *call) {
    a20_task_spawn_args_t *user = (void *)(uintptr_t)call->arg[0];
    a20_task_spawn_args_t args;
    int result = a20_spawn_copy_params(user, &args);
    if (result)
        return result;
    if (args.argc > FLAT_ARG_COUNT || args.envc > FLAT_ARG_COUNT)
        return -A20_ERR_INVALID_ARGUMENT;
    if (args.root_dir != A20_HANDLE_NULL || args.cwd_dir != A20_HANDLE_NULL ||
        args.event_queue != A20_HANDLE_NULL || args.handle_count ||
        (args.version == 2 && (args.stdin_handle != A20_HANDLE_NULL ||
                               args.stdout_handle != A20_HANDLE_NULL ||
                               args.stderr_handle != A20_HANDLE_NULL)))
        return -A20_ERR_NOT_SUPPORTED;
    struct a20_ht_internal *ht = task_get_a20_ht(proc_current());
    if (!ht)
        return -A20_ERR_BAD_HANDLE;
    a20_handle_entry_t image;
    int64_t status = a20_handle_lookup_ref_internal(
        ht, args.image, A20_OBJ_FILE, A20_RIGHT_READ | A20_RIGHT_EXEC, &image);
    if (status < 0)
        return status;
    flat_args_t *copied = kcalloc(1, sizeof(*copied));
    if (!copied) {
        a20_object_release(image.object, image.type);
        return -A20_ERR_NO_MEMORY;
    }
    result = copy_vector(args.argv, args.argc, 0, copied);
    if (!result)
        result = copy_vector(args.envp, args.envc, FLAT_ARG_COUNT, copied);
    void *bytes = NULL;
    size_t length = 0;
    if (!result) {
        int error = a20_flat_file_image(image.object, FLAT_IMAGE_BYTES, &bytes,
                                        &length);
        result = error == -ENOMEM ? -A20_ERR_NO_MEMORY
                 : error          ? -A20_ERR_IO
                                  : 0;
    }
    a20_object_release(image.object, image.type);
    a20_flat_program_t created = {0};
    if (!result) {
        int error =
            arch_flat_program_create(bytes, length, FLAT_STACK_BYTES, &created);
        result = error == -ENOMEM ? -A20_ERR_NO_MEMORY
                 : error < 0      ? -A20_ERR_IO
                                  : 0;
    }
    kfree(bytes);
    if (result) {
        kfree(copied);
        return result;
    }
    task_t *child = created.task;
    int64_t self = a20_handle_install_task(
        task_get_a20_ht(child), child, A20_OBJ_TASK,
        A20_RIGHT_WAIT | A20_RIGHT_SIGNAL | A20_RIGHT_STAT | A20_RIGHT_CONTROL);
    int64_t handle =
        self < 0
            ? self
            : a20_handle_install_task(
                  ht, child, A20_OBJ_TASK,
                  A20_RIGHT_WAIT | A20_RIGHT_SIGNAL | A20_RIGHT_STAT |
                      A20_RIGHT_CONTROL | A20_RIGHT_DUP | A20_RIGHT_TRANSFER |
                      A20_RIGHT_ADMIN | A20_RIGHT_READ | A20_RIGHT_WRITE);
    if (handle < 0)
        result = handle;
    if (!result) {
        uintptr_t startup = prepare_start(&created, &args, copied, self);
        arch_flat_program_set_stack(child, startup);
        if (args.flags & A20_SPAWN_PARENT_BOUND) {
            uint64_t flags = spin_lock_irqsave(&child->park_lock);
            child->pdeathsig = 9;
            spin_unlock_irqrestore(&child->park_lock, flags);
        }
        args.out_task = handle;
        result = a20_copy_struct_to_user(user, &args, sizeof(args));
    }
    kfree(copied);
    if (result) {
        if (handle >= 0)
            a20_handle_remove(ht, handle);
        proc_free_pid(child->pid);
        proc_put(child);
        return result;
    }
    proc_publish_deferred_task(child);
    proc_put(child);
    return handle;
}
#endif
