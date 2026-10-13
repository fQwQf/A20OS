/* Failure adapters for the production handler, complementing real MCU/QEMU
 * runs. No claim that these adapters execute a child instruction stream. */
#include "native_spawn_shim.h"
#include "core/flat_program.h"
#include "abi/native/syscall_entry.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static task_t parent, child;
static struct a20_ht_internal parent_ht, child_ht;
static vfile_t file;
static unsigned char stack[16384] __attribute__((aligned(16)));
static unsigned expected_argc = 1, expected_envc = 1, expected_length;
static int mode, allocations, lookup, released, created, destroyed, put,
    published, installs, removed;
int64_t sys_a20_task_spawn(const a20_syscall_args_t *);
void *kmalloc(size_t n) {
    void *p = malloc(n);
    assert(p);
    allocations++;
    return p;
}
void *kcalloc(size_t n, size_t s) {
    if (mode == 1)
        return NULL;
    void *p = calloc(n, s);
    assert(p);
    allocations++;
    return p;
}
void kfree(void *p) {
    if (p) {
        allocations--;
        free(p);
    }
}
task_t *proc_current(void) {
    return &parent;
}
struct a20_ht_internal *task_get_a20_ht(task_t *t) {
    return t == &parent ? &parent_ht : &child_ht;
}
void proc_put(task_t *t) {
    assert(t == &child);
    put++;
}
void proc_free_pid(int pid) {
    assert(pid == 42 && !published);
    destroyed++;
}
void proc_publish_deferred_task(task_t *t) {
    assert(t == &child && installs == 2 && !destroyed && child.pdeathsig == 9);
    published++;
}
int64_t a20_handle_lookup_ref_internal(struct a20_ht_internal *ht,
                                       a20_handle_t h, uint16_t type,
                                       a20_rights_t rights,
                                       a20_handle_entry_t *out) {
    assert(ht == &parent_ht && h == 4 && type == A20_OBJ_FILE &&
           rights == (A20_RIGHT_READ | A20_RIGHT_EXEC));
    lookup++;
    *out = (a20_handle_entry_t){.object = &file, .type = A20_OBJ_FILE};
    return 0;
}
void a20_object_release(void *p, uint16_t type) {
    assert(p == &file && type == A20_OBJ_FILE);
    released++;
}
int a20_flat_file_image(vfile_t *vf, size_t limit, void **p, size_t *n) {
    assert(vf == &file && limit == 512 * 1024);
    if (mode == 2)
        return -ENOMEM;
    if (mode == 3)
        return -EIO;
    *p = kmalloc(128);
    *n = 128;
    return 0;
}
int arch_flat_program_create(const void *image, size_t n, size_t stack_size,
                             a20_flat_program_t *out) {
    assert(image && n == 128 && stack_size == sizeof(stack));
    if (mode == 4)
        return -ENOEXEC;
    if (mode == 5)
        return -ENOMEM;
    created++;
    child.pid = 42;
    *out = (a20_flat_program_t){&child, (uintptr_t)stack, sizeof(stack)};
    return 42;
}
void arch_flat_program_set_stack(task_t *t, uintptr_t startup) {
    assert(
        t == &child && !(startup & 15) && startup >= (uintptr_t)stack + 4096 &&
        startup + sizeof(a20_start_info_t) < (uintptr_t)stack + sizeof(stack));
    a20_start_info_t *s = (void *)startup;
    assert(s->size == sizeof(*s) && s->version == 1 &&
           s->argc == expected_argc && s->envc == expected_envc &&
           s->page_size == 4096 && s->self_task == 2);
    assert(s->stdin_handle == A20_HANDLE_NULL &&
           s->stdout_handle == A20_HANDLE_NULL &&
           s->root_dir == A20_HANDLE_NULL);
    char **argv = (void *)(uintptr_t)s->argv,
         **env = (void *)(uintptr_t)s->envp;
    assert(!argv[expected_argc] && !env[expected_envc]);
    for (unsigned i = 0; i < expected_argc; i++) {
        assert((uintptr_t)argv[i] > startup &&
               (uintptr_t)argv[i] < (uintptr_t)stack + sizeof(stack));
        assert(expected_length ? strlen(argv[i]) == expected_length
                               : !strcmp(argv[i], "argument"));
    }
    for (unsigned i = 0; i < expected_envc; i++) {
        assert((uintptr_t)env[i] > startup &&
               (uintptr_t)env[i] < (uintptr_t)stack + sizeof(stack));
        assert(expected_length ? strlen(env[i]) == expected_length
                               : !strcmp(env[i], "KEY=value"));
    }
}
int64_t a20_handle_install_task(struct a20_ht_internal *ht, task_t *t,
                                uint16_t type, a20_rights_t rights) {
    assert(t == &child && type == A20_OBJ_TASK && rights & A20_RIGHT_WAIT);
    if ((mode == 6 && ht == &child_ht) || (mode == 7 && ht == &parent_ht))
        return -A20_ERR_NO_SPACE;
    installs++;
    return ht == &child_ht ? 2 : 7;
}
int64_t a20_handle_remove(struct a20_ht_internal *ht, a20_handle_t h) {
    assert(ht == &parent_ht && h == 7);
    removed++;
    return 0;
}
long copy_from_user(void *d, const void *s, size_t n) {
    if (mode == 9 || (uintptr_t)s == 1)
        return -EFAULT;
    memcpy(d, s, n);
    return 0;
}
long copy_to_user(void *d, const void *s, size_t n) {
    if (mode == 8)
        return -EFAULT;
    memcpy(d, s, n);
    return 0;
}
int main(void) {
    const int expected[] = {7,
                            -A20_ERR_NO_MEMORY,
                            -A20_ERR_NO_MEMORY,
                            -A20_ERR_IO,
                            -A20_ERR_IO,
                            -A20_ERR_NO_MEMORY,
                            -A20_ERR_NO_SPACE,
                            -A20_ERR_NO_SPACE,
                            -A20_ERR_FAULT,
                            -A20_ERR_FAULT,
                            -A20_ERR_INVALID_ARGUMENT,
                            -A20_ERR_FAULT};
    for (mode = 0; mode < 12; mode++) {
        allocations = lookup = released = created = destroyed = put =
            published = installs = removed = 0;
        memset(&child, 0, sizeof(child));
        memset(stack, 0xcc, sizeof(stack));
        char *argv[] = {mode == 11 ? (char *)1 : "argument", NULL},
             *env[] = {"KEY=value", NULL};
        a20_task_spawn_args_t args = {.size = sizeof(args),
                                      .version = 2,
                                      .image = 4,
                                      .root_dir = A20_HANDLE_NULL,
                                      .cwd_dir = A20_HANDLE_NULL,
                                      .event_queue = A20_HANDLE_NULL,
                                      .argv = (uintptr_t)argv,
                                      .envp = (uintptr_t)env,
                                      .argc = 1,
                                      .envc = 1,
                                      .flags = A20_SPAWN_PARENT_BOUND,
                                      .out_task = 99,
                                      .stdin_handle = A20_HANDLE_NULL,
                                      .stdout_handle = A20_HANDLE_NULL,
                                      .stderr_handle = A20_HANDLE_NULL};
        if (mode == 10)
            args.flags = 0x80000000;
        a20_syscall_args_t call = {.arg = {(uintptr_t)&args}};
        assert(sys_a20_task_spawn(&call) == expected[mode]);
        assert(allocations == 0 && lookup == released);
        if (!mode)
            assert(created == 1 && published == 1 && put == 1 && !destroyed &&
                   !removed && args.out_task == 7);
        else if (mode >= 6 && mode <= 8)
            assert(created == 1 && !published && destroyed == 1 && put == 1 &&
                   removed == (mode == 8) && args.out_task == 99);
        else
            assert(!created && !published && !destroyed && !put && !removed &&
                   args.out_task == 99);
    }
    mode = 0;
    for (unsigned boundary = 0; boundary < 5; boundary++) {
        allocations = lookup = released = created = destroyed = put =
            published = installs = removed = 0;
        memset(&child, 0, sizeof(child));
        char text[65];
        memset(text, 'x', sizeof(text));
        text[boundary == 2 ? 64 : 63] = 0;
        char *argv[34], *env[33];
        for (unsigned i = 0; i < 33; i++)
            argv[i] = text;
        for (unsigned i = 0; i < 32; i++)
            env[i] = text;
        argv[32] = boundary == 4 ? text : NULL;
        argv[33] = NULL;
        env[32] = NULL;
        expected_argc = expected_envc = boundary ? 32 : 0;
        expected_length = 63;
        a20_task_spawn_args_t args = {.size = sizeof(args),
                                      .version = 2,
                                      .image = 4,
                                      .root_dir = A20_HANDLE_NULL,
                                      .cwd_dir = A20_HANDLE_NULL,
                                      .event_queue = A20_HANDLE_NULL,
                                      .argv = boundary ? (uintptr_t)argv : 0,
                                      .envp = boundary ? (uintptr_t)env : 0,
                                      .argc =
                                          boundary == 3 ? 33 : expected_argc,
                                      .envc = expected_envc,
                                      .flags = A20_SPAWN_PARENT_BOUND,
                                      .out_task = 99,
                                      .stdin_handle = A20_HANDLE_NULL,
                                      .stdout_handle = A20_HANDLE_NULL,
                                      .stderr_handle = A20_HANDLE_NULL};
        a20_syscall_args_t call = {.arg = {(uintptr_t)&args}};
        int64_t result = sys_a20_task_spawn(&call);
        assert(allocations == 0 && lookup == released);
        if (boundary < 2)
            assert(result == 7 && created == 1 && published == 1 && put == 1 &&
                   !destroyed && args.out_task == 7);
        else
            assert(result == -A20_ERR_INVALID_ARGUMENT && !created &&
                   !published && !destroyed && !put && args.out_task == 99);
    }
    puts("PASS Native flat spawn admission: 12 publication/rollback paths, 5 "
         "argument boundary paths");
    return 0;
}
