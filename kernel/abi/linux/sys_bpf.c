#include "syscall_impl.h"

#include "bpf/bpf.h"

static int bpf_copy_attr(void *dst, size_t dst_size, void *uattr, unsigned size)
{
    memset(dst, 0, dst_size);
    size_t n = size < dst_size ? size : dst_size;
    if (n && copy_from_user(dst, uattr, n) < 0)
        return -EFAULT;
    return 0;
}

static int64_t sys_bpf_map_create(void *uattr, unsigned size)
{
    bpf_attr_map_create_t attr;
    int r = bpf_copy_attr(&attr, sizeof(attr), uattr, size);
    if (r < 0) return r;
    return bpf_map_create(&attr);
}

static int64_t sys_bpf_map_update(void *uattr, unsigned size)
{
    bpf_attr_elem_t attr;
    int r = bpf_copy_attr(&attr, sizeof(attr), uattr, size);
    if (r < 0) return r;

    size_t key_size, value_size;
    r = bpf_map_key_value_size((int)attr.map_fd, &key_size, &value_size);
    if (r < 0) return r;
    uint8_t key[BPF_KEY_MAX];
    uint8_t value[BPF_VALUE_MAX];
    if (copy_from_user(key, (void *)(uintptr_t)attr.key, key_size) < 0)
        return -EFAULT;
    if (copy_from_user(value, (void *)(uintptr_t)attr.value, value_size) < 0)
        return -EFAULT;
    return bpf_map_update(&attr, key, value);
}

static int64_t sys_bpf_map_lookup(void *uattr, unsigned size)
{
    bpf_attr_elem_t attr;
    int r = bpf_copy_attr(&attr, sizeof(attr), uattr, size);
    if (r < 0) return r;

    size_t key_size, value_size;
    r = bpf_map_key_value_size((int)attr.map_fd, &key_size, &value_size);
    if (r < 0) return r;
    uint8_t key[BPF_KEY_MAX];
    uint8_t value[BPF_VALUE_MAX];
    if (copy_from_user(key, (void *)(uintptr_t)attr.key, key_size) < 0)
        return -EFAULT;
    r = bpf_map_lookup(&attr, key, value);
    if (r < 0) return r;
    if (copy_to_user((void *)(uintptr_t)attr.value, value, value_size) < 0)
        return -EFAULT;
    return 0;
}

static int64_t sys_bpf_map_delete(void *uattr, unsigned size)
{
    bpf_attr_elem_t attr;
    int r = bpf_copy_attr(&attr, sizeof(attr), uattr, size);
    if (r < 0) return r;

    size_t key_size;
    r = bpf_map_key_value_size((int)attr.map_fd, &key_size, NULL);
    if (r < 0) return r;
    uint8_t key[BPF_KEY_MAX];
    if (copy_from_user(key, (void *)(uintptr_t)attr.key, key_size) < 0)
        return -EFAULT;
    return bpf_map_delete(&attr, key);
}

static int64_t sys_bpf_map_get_next_key(void *uattr, unsigned size)
{
    bpf_attr_elem_t attr;
    int r = bpf_copy_attr(&attr, sizeof(attr), uattr, size);
    if (r < 0) return r;

    size_t key_size;
    r = bpf_map_key_value_size((int)attr.map_fd, &key_size, NULL);
    if (r < 0) return r;
    uint8_t key[BPF_KEY_MAX];
    uint8_t next_key[BPF_KEY_MAX];
    void *key_ptr = NULL;
    if (attr.key) {
        if (copy_from_user(key, (void *)(uintptr_t)attr.key, key_size) < 0)
            return -EFAULT;
        key_ptr = key;
    }
    r = bpf_map_get_next_key(&attr, key_ptr, next_key);
    if (r < 0) return r;
    if (copy_to_user((void *)(uintptr_t)attr.next_key, next_key, key_size) < 0)
        return -EFAULT;
    return 0;
}

static int64_t sys_bpf_prog_load(void *uattr, unsigned size)
{
    bpf_attr_prog_load_t attr;
    int r = bpf_copy_attr(&attr, sizeof(attr), uattr, size);
    if (r < 0) return r;
    if (attr.insn_cnt == 0 || attr.insn_cnt > 512 || !attr.insns)
        return -EINVAL;

    bpf_insn_t insns[BPF_PROG_LOAD_COPY_MAX];
    uint32_t copy_cnt = attr.insn_cnt < BPF_PROG_LOAD_COPY_MAX ? attr.insn_cnt : BPF_PROG_LOAD_COPY_MAX;
    if (copy_from_user(insns, (void *)(uintptr_t)attr.insns,
                       copy_cnt * sizeof(insns[0])) < 0)
        return -EFAULT;
    r = bpf_prog_load(&attr, insns, copy_cnt);
    if (r < 0 && attr.log_buf && attr.log_size) {
        const char log[] = "BPF verifier unavailable: unsupported program rejected\n";
        size_t n = sizeof(log);
        if (n > attr.log_size)
            n = attr.log_size;
        if (copy_to_user((void *)(uintptr_t)attr.log_buf, log, n) < 0)
            return -EFAULT;
    }
    return r;
}

int64_t sys_bpf(int cmd, void *attr, unsigned size)
{
    if (size && !attr) return -EFAULT;
    if (size) {
        uint8_t probe;
        if (copy_from_user(&probe, attr, 1) < 0) return -EFAULT;
    }
    switch (cmd) {
    case BPF_MAP_CREATE:
        return sys_bpf_map_create(attr, size);
    case BPF_MAP_LOOKUP_ELEM:
        return sys_bpf_map_lookup(attr, size);
    case BPF_MAP_UPDATE_ELEM:
        return sys_bpf_map_update(attr, size);
    case BPF_MAP_DELETE_ELEM:
        return sys_bpf_map_delete(attr, size);
    case BPF_MAP_GET_NEXT_KEY:
        return sys_bpf_map_get_next_key(attr, size);
    case BPF_PROG_LOAD:
        return sys_bpf_prog_load(attr, size);
    default:
        return -EINVAL;
    }
}
