#include "native_runtime_shim.h"
#include "core/runtime.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
static task_t caller;
static int has_caller = 1, admin = 1, table_present = 1, copy_fault, refs,
           live = 1;
static uint16_t object_type = A20_OBJ_THREAD;
static a20_rights_t object_rights = A20_RIGHT_STAT;
static int dummy_object;
task_t *proc_current(void) {
    return has_caller ? &caller : NULL;
}
int proc_has_cap(const task_t *task, int cap) {
    assert(task == &caller && cap == CAP_SYS_ADMIN);
    return admin;
}
struct a20_ht_internal *task_get_a20_ht(task_t *task) {
    return task && table_present ? (void *)&dummy_object : NULL;
}
long copy_from_user(void *to, const void *from, size_t n) {
    if (copy_fault || (uintptr_t)from < 4096)
        return -EFAULT;
    memcpy(to, from, n);
    return 0;
}
long copy_to_user(void *to, const void *from, size_t n) {
    if (copy_fault || (uintptr_t)to < 4096)
        return -EFAULT;
    memcpy(to, from, n);
    return 0;
}
int64_t a20_handle_lookup_ref_internal(struct a20_ht_internal *ht,
                                       a20_handle_t h, uint16_t type,
                                       a20_rights_t rights,
                                       a20_handle_entry_t *out) {
    assert(ht && type == A20_OBJ_INVALID);
    if (h != 1)
        return -A20_ERR_BAD_HANDLE;
    if ((object_rights & rights) != rights)
        return -A20_ERR_ACCESS;
    *out = (a20_handle_entry_t){
        .object = &dummy_object, .type = object_type, .rights = object_rights};
    refs++;
    return 0;
}
void a20_object_release(void *p, uint16_t type) {
    assert(p == &dummy_object && type == object_type && refs == 1);
    refs--;
}
uint64_t a20_task_object_serial(void *p) {
    assert(p == &dummy_object);
    return 0xfedcba9876543210ULL;
}
task_t *a20_task_object_get_task(void *p) {
    assert(p == &dummy_object);
    return live ? &caller : NULL;
}
void proc_put(task_t *p) {
    assert(!p || p == &caller);
}
static a20_runtime_binding_t candidates[4];
static int results[4], go;
static void *bind_race(void *p) {
    uintptr_t i = (uintptr_t)p;
    while (!__atomic_load_n(&go, __ATOMIC_ACQUIRE))
        sched_yield();
    results[i] = a20_runtime_bind(&candidates[i]);
    for (unsigned k = 0; k < 10000; k++) {
        a20_runtime_info_t info;
        a20_runtime_snapshot(&info);
        assert(a20_runtime_info_valid(&info) &&
               info.state == A20_RUNTIME_BOUND);
        assert(info.boot_epoch == UINT64_MAX);
        assert(
            a20_runtime_id_equal(info.instance_id, candidates[0].instance_id));
        assert(info.domain_id[0] >= 1 && info.domain_id[0] <= 4);
    }
    return NULL;
}
static void codec(const a20_runtime_info_t *info) {
    _Static_assert(sizeof(a20_runtime_binding_t) == 64, "binding ABI");
    _Static_assert(sizeof(a20_runtime_info_t) == 64, "runtime ABI");
    _Static_assert(sizeof(a20_object_id_t) == 64, "object ABI");
    _Static_assert(sizeof(a20_handle_identity_t) == 80, "query ABI");
    a20_object_id_t id = {.serial = 0x1020304050607080ULL, .kind = 1};
    assert(a20_runtime_object_namespace(&id));
    assert(id.boot_epoch == info->boot_epoch);
    uint8_t wire[65], bad[65];
    memset(wire, 0xa5, sizeof(wire));
    assert(!a20_object_id_encode(wire, 64, &id) && wire[64] == 0xa5);
    assert(wire[32] == 255 && wire[39] == 255 && wire[40] == 0x80 &&
           wire[47] == 0x10 && wire[48] == 1);
    a20_object_id_t out = {0};
    assert(!a20_object_id_decode(wire, 64, &out));
    assert(a20_object_id_equal(&out, &id));
    a20_object_id_t before = out;
    for (unsigned len = 0; len <= 65; len++)
        if (len != 64) {
            assert(a20_object_id_decode(wire, len, &out) < 0 &&
                   !memcmp(&before, &out, sizeof(out)));
        }
    for (unsigned k = 48; k < 64; k++) {
        memcpy(bad, wire, 64);
        bad[k] = 0x80;
        assert(a20_object_id_decode(bad, 64, &out) < 0 &&
               !memcmp(&before, &out, sizeof(out)));
    }
    for (unsigned offset = 0; offset <= 40; offset += 8) {
        memcpy(bad, wire, 64);
        if (offset < 32) {
            if (offset == 8 || offset == 24)
                continue;
            memset(bad + offset, 0, 16);
        } else
            memset(bad + offset, 0, 8);
        assert(a20_object_id_decode(bad, 64, &out) < 0 &&
               !memcmp(&before, &out, sizeof(out)));
    }
    assert(a20_object_id_decode(NULL, 64, &out) < 0);
    assert(a20_object_id_decode(wire, 64, NULL) < 0);
    memcpy(bad, wire, 65);
    id.serial = 0;
    assert(a20_object_id_encode(bad, 64, &id) < 0 && !memcmp(bad, wire, 65));
    id.serial = 1;
    assert(a20_object_id_encode(bad, 63, &id) < 0 && !memcmp(bad, wire, 65));
    assert(a20_object_id_encode(NULL, 64, &id) < 0);
}
int main(void) {
    a20_runtime_info_t info;
    a20_syscall_args_t q = {.arg = {(uintptr_t)&info, 64}};
    assert(!sys_a20_runtime_info(&q) && a20_runtime_info_valid(&info) &&
           !info.state);
    a20_object_id_t id = {.serial = 7}, untouched = id;
    assert(!a20_runtime_object_namespace(&id) &&
           !memcmp(&id, &untouched, sizeof(id)));
    a20_handle_identity_t identity;
    a20_syscall_args_t h = {.arg = {1, (uintptr_t)&identity, 80}};
    assert(sys_a20_handle_identity(&h) == -A20_ERR_WOULD_BLOCK && !refs);
    candidates[0] = (a20_runtime_binding_t){.size = 64,
                                            .version = 1,
                                            .instance_id = {0xa2, 0x20},
                                            .domain_id = {1},
                                            .boot_epoch = UINT64_MAX};
    a20_runtime_binding_t invalid = candidates[0];
    invalid.reserved = 1;
    assert(a20_runtime_bind(&invalid) == -EINVAL);
    invalid = candidates[0];
    invalid.boot_epoch = 0;
    assert(a20_runtime_bind(&invalid) == -EINVAL);
    assert(a20_runtime_bind(NULL) == -EINVAL);
    q.arg[0] = (uintptr_t)&candidates[0];
    admin = 0;
    assert(sys_a20_runtime_bind(&q) == -A20_ERR_PERM);
    admin = 1;
    has_caller = 0;
    assert(sys_a20_runtime_bind(&q) == -A20_ERR_PERM);
    has_caller = 1;
    copy_fault = 1;
    assert(sys_a20_runtime_bind(&q) == -A20_ERR_FAULT);
    copy_fault = 0;
    q.arg[1] = 63;
    assert(sys_a20_runtime_bind(&q) == -A20_ERR_INVALID_ARGUMENT);
    q.arg[1] = 64;
    q.arg[0] = 0;
    assert(sys_a20_runtime_bind(&q) == -A20_ERR_FAULT);
    a20_runtime_snapshot(&info);
    assert(!info.state);
    pthread_t threads[4];
    for (uintptr_t i = 0; i < 4; i++) {
        candidates[i] = candidates[0];
        candidates[i].domain_id[0] = i + 1;
        assert(!pthread_create(&threads[i], NULL, bind_race, (void *)i));
    }
    __atomic_store_n(&go, 1, __ATOMIC_RELEASE);
    unsigned winner = 4;
    for (unsigned i = 0; i < 4; i++) {
        assert(!pthread_join(threads[i], NULL));
        if (!results[i]) {
            assert(winner == 4);
            winner = i;
        } else
            assert(results[i] == -EBUSY);
    }
    assert(winner < 4);
    q.arg[0] = (uintptr_t)&candidates[winner];
    assert(!sys_a20_runtime_bind(&q));
    assert(!sys_a20_runtime_bind(&q));
    a20_runtime_binding_t conflict = candidates[winner];
    conflict.boot_epoch--;
    q.arg[0] = (uintptr_t)&conflict;
    assert(sys_a20_runtime_bind(&q) == -A20_ERR_BUSY);
    a20_runtime_snapshot(&info);
    codec(&info);
    for (unsigned i = 0; i < 3; i++) {
        object_type =
            (uint16_t[]){A20_OBJ_TASK, A20_OBJ_THREAD, A20_OBJ_DEBUG}[i];
        live = 1;
        assert(!sys_a20_handle_identity(&h) && !refs);
        assert(identity.state == A20_OBJECT_LIVE &&
               identity.id.serial == 0xfedcba9876543210ULL);
        a20_object_id_t saved = identity.id;
        live = 0;
        assert(!sys_a20_handle_identity(&h) &&
               identity.state == A20_OBJECT_RETIRED);
        assert(a20_object_id_equal(&saved, &identity.id) && !refs);
    }
    copy_fault = 1;
    assert(sys_a20_handle_identity(&h) == -A20_ERR_FAULT && !refs);
    copy_fault = 0;
    object_type = A20_OBJ_FILE;
    assert(sys_a20_handle_identity(&h) == -A20_ERR_NOT_SUPPORTED && !refs);
    object_rights = 0;
    assert(sys_a20_handle_identity(&h) == -A20_ERR_ACCESS && !refs);
    h.arg[0] = 2;
    assert(sys_a20_handle_identity(&h) == -A20_ERR_BAD_HANDLE);
    table_present = 0;
    assert(sys_a20_handle_identity(&h) == -A20_ERR_BAD_HANDLE);
    h.arg[1] = 0;
    assert(sys_a20_handle_identity(&h) == -A20_ERR_FAULT);
    h.arg[1] = (uintptr_t)&identity;
    h.arg[2] = 79;
    assert(sys_a20_handle_identity(&h) == -A20_ERR_INVALID_ARGUMENT);
    q.arg[0] = (uintptr_t)&info;
    copy_fault = 1;
    assert(sys_a20_runtime_info(&q) == -A20_ERR_FAULT);
    copy_fault = 0;
    q.arg[1] = 63;
    assert(sys_a20_runtime_info(&q) == -A20_ERR_INVALID_ARGUMENT);
    q.arg[0] = 0;
    assert(sys_a20_runtime_info(&q) == -A20_ERR_FAULT);
    puts("PASS Native runtime: immutable concurrent binding, "
         "capability/usercopy admission, stable aliases, LE codec");
    return 0;
}
