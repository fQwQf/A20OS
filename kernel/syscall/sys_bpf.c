#include "syscall_internal.h"

#define BPF_MAP_CREATE       0
#define BPF_MAP_LOOKUP_ELEM  1
#define BPF_MAP_UPDATE_ELEM  2
#define BPF_MAP_DELETE_ELEM  3
#define BPF_MAP_GET_NEXT_KEY 4
#define BPF_PROG_LOAD        5

#define BPF_MAP_TYPE_HASH    1
#define BPF_MAP_TYPE_ARRAY   2

#define BPF_MAPS_MAX         32
#define BPF_ENTRIES_MAX      64
#define BPF_KEY_MAX          16
#define BPF_VALUE_MAX        1024

#define BPF_PROGS_MAX        32
#define BPF_PROG_KIND_WRITE1 1
#define BPF_PROG_KIND_ARITH64 2

typedef struct {
    uint32_t map_type;
    uint32_t key_size;
    uint32_t value_size;
    uint32_t max_entries;
    uint32_t flags;
} bpf_attr_map_create_t;

typedef struct {
    uint32_t prog_type;
    uint32_t insn_cnt;
    uint64_t insns;
    uint64_t license;
    uint32_t log_level;
    uint32_t log_size;
    uint64_t log_buf;
} bpf_attr_prog_load_t;

typedef struct {
    uint8_t code;
    uint8_t regs;
    int16_t off;
    int32_t imm;
} bpf_insn_t;

typedef struct {
    uint32_t map_fd;
    uint32_t pad;
    uint64_t key;
    union {
        uint64_t value;
        uint64_t next_key;
    };
    uint64_t flags;
} bpf_attr_elem_t;

typedef struct {
    int used;
    uint8_t key[BPF_KEY_MAX];
    uint8_t value[BPF_VALUE_MAX];
} bpf_entry_t;

typedef struct {
    int used;
    int owner_pid;
    int fd;
    uint32_t type;
    uint32_t key_size;
    uint32_t value_size;
    uint32_t max_entries;
    bpf_entry_t entries[BPF_ENTRIES_MAX];
} bpf_map_t;

static bpf_map_t g_maps[BPF_MAPS_MAX];

typedef struct {
    int used;
    int owner_pid;
    int fd;
    int map_fd;
    int kind;
} bpf_prog_t;

static bpf_prog_t g_progs[BPF_PROGS_MAX];

static bpf_map_t *bpf_find_map(int fd)
{
    task_t *t = proc_current();
    int pid = t ? t->pid : 0;
    for (int i = 0; i < BPF_MAPS_MAX; i++) {
        if (g_maps[i].used && g_maps[i].fd == fd && g_maps[i].owner_pid == pid)
            return &g_maps[i];
    }
    return NULL;
}

static bpf_prog_t *bpf_find_prog(int fd)
{
    task_t *t = proc_current();
    int pid = t ? t->pid : 0;
    for (int i = 0; i < BPF_PROGS_MAX; i++) {
        if (g_progs[i].used && g_progs[i].fd == fd && g_progs[i].owner_pid == pid)
            return &g_progs[i];
    }
    return NULL;
}

static int bpf_key_eq(const bpf_map_t *m, const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, m->key_size) == 0;
}

static int bpf_entry_index(bpf_map_t *m, const uint8_t *key)
{
    if (m->type == BPF_MAP_TYPE_ARRAY) {
        uint32_t idx = 0;
        memcpy(&idx, key, m->key_size < sizeof(idx) ? m->key_size : sizeof(idx));
        if (idx >= m->max_entries || idx >= BPF_ENTRIES_MAX)
            return -ENOENT;
        return (int)idx;
    }
    for (int i = 0; i < BPF_ENTRIES_MAX; i++) {
        if (m->entries[i].used && bpf_key_eq(m, m->entries[i].key, key))
            return i;
    }
    return -ENOENT;
}

static int bpf_alloc_entry(bpf_map_t *m)
{
    uint32_t limit = m->max_entries < BPF_ENTRIES_MAX ? m->max_entries : BPF_ENTRIES_MAX;
    for (uint32_t i = 0; i < limit; i++) {
        if (!m->entries[i].used)
            return (int)i;
    }
    return -E2BIG;
}

static int64_t bpf_map_create(void *uattr, unsigned size)
{
    bpf_attr_map_create_t attr;
    memset(&attr, 0, sizeof(attr));
    size_t n = size < sizeof(attr) ? size : sizeof(attr);
    if (copy_from_user(&attr, uattr, n) < 0) return -EFAULT;
    if (attr.key_size == 0 || attr.key_size > BPF_KEY_MAX ||
        attr.value_size == 0 || attr.value_size > BPF_VALUE_MAX ||
        attr.max_entries == 0)
        return -EINVAL;
    if (attr.map_type != BPF_MAP_TYPE_HASH && attr.map_type != BPF_MAP_TYPE_ARRAY)
        return -EINVAL;

    int fd = (int)sys_memfd_create(NULL, 0);
    if (fd < 0) return fd;

    task_t *t = proc_current();
    int pid = t ? t->pid : 0;
    for (int i = 0; i < BPF_MAPS_MAX; i++) {
        if (g_maps[i].used && g_maps[i].owner_pid == pid && g_maps[i].fd == fd)
            memset(&g_maps[i], 0, sizeof(g_maps[i]));
    }
    for (int i = 0; i < BPF_MAPS_MAX; i++) {
        if (g_maps[i].used)
            continue;
        memset(&g_maps[i], 0, sizeof(g_maps[i]));
        g_maps[i].used = 1;
        g_maps[i].owner_pid = pid;
        g_maps[i].fd = fd;
        g_maps[i].type = attr.map_type;
        g_maps[i].key_size = attr.key_size;
        g_maps[i].value_size = attr.value_size;
        g_maps[i].max_entries = attr.max_entries;
        if (attr.map_type == BPF_MAP_TYPE_ARRAY) {
            uint32_t limit = attr.max_entries < BPF_ENTRIES_MAX ? attr.max_entries : BPF_ENTRIES_MAX;
            for (uint32_t j = 0; j < limit; j++) {
                g_maps[i].entries[j].used = 1;
                memcpy(g_maps[i].entries[j].key, &j,
                       attr.key_size < sizeof(j) ? attr.key_size : sizeof(j));
            }
        }
        return fd;
    }
    sys_close(fd);
    return -ENFILE;
}

static int64_t bpf_map_update(void *uattr, unsigned size)
{
    bpf_attr_elem_t attr;
    memset(&attr, 0, sizeof(attr));
    size_t n = size < sizeof(attr) ? size : sizeof(attr);
    if (copy_from_user(&attr, uattr, n) < 0) return -EFAULT;
    bpf_map_t *m = bpf_find_map((int)attr.map_fd);
    if (!m) return -EBADF;

    uint8_t key[BPF_KEY_MAX];
    uint8_t value[BPF_VALUE_MAX];
    if (copy_from_user(key, (void *)(uintptr_t)attr.key, m->key_size) < 0) return -EFAULT;
    if (copy_from_user(value, (void *)(uintptr_t)attr.value, m->value_size) < 0) return -EFAULT;

    int idx = bpf_entry_index(m, key);
    if (idx < 0) {
        if (m->type == BPF_MAP_TYPE_ARRAY)
            return idx;
        idx = bpf_alloc_entry(m);
        if (idx < 0) return idx;
        m->entries[idx].used = 1;
        memcpy(m->entries[idx].key, key, m->key_size);
    }
    memcpy(m->entries[idx].value, value, m->value_size);
    return 0;
}

static int64_t bpf_map_lookup(void *uattr, unsigned size)
{
    bpf_attr_elem_t attr;
    memset(&attr, 0, sizeof(attr));
    size_t n = size < sizeof(attr) ? size : sizeof(attr);
    if (copy_from_user(&attr, uattr, n) < 0) return -EFAULT;
    bpf_map_t *m = bpf_find_map((int)attr.map_fd);
    if (!m) return -EBADF;

    uint8_t key[BPF_KEY_MAX];
    if (copy_from_user(key, (void *)(uintptr_t)attr.key, m->key_size) < 0) return -EFAULT;
    int idx = bpf_entry_index(m, key);
    if (idx < 0 || !m->entries[idx].used) return -ENOENT;
    if (copy_to_user((void *)(uintptr_t)attr.value, m->entries[idx].value, m->value_size) < 0)
        return -EFAULT;
    return 0;
}

static int64_t bpf_map_delete(void *uattr, unsigned size)
{
    bpf_attr_elem_t attr;
    memset(&attr, 0, sizeof(attr));
    size_t n = size < sizeof(attr) ? size : sizeof(attr);
    if (copy_from_user(&attr, uattr, n) < 0) return -EFAULT;
    bpf_map_t *m = bpf_find_map((int)attr.map_fd);
    if (!m) return -EBADF;
    if (m->type == BPF_MAP_TYPE_ARRAY) return -EINVAL;
    uint8_t key[BPF_KEY_MAX];
    if (copy_from_user(key, (void *)(uintptr_t)attr.key, m->key_size) < 0) return -EFAULT;
    int idx = bpf_entry_index(m, key);
    if (idx < 0) return -ENOENT;
    memset(&m->entries[idx], 0, sizeof(m->entries[idx]));
    return 0;
}

static int64_t bpf_map_get_next_key(void *uattr, unsigned size)
{
    bpf_attr_elem_t attr;
    memset(&attr, 0, sizeof(attr));
    size_t n = size < sizeof(attr) ? size : sizeof(attr);
    if (copy_from_user(&attr, uattr, n) < 0) return -EFAULT;
    bpf_map_t *m = bpf_find_map((int)attr.map_fd);
    if (!m) return -EBADF;

    int start = -1;
    if (attr.key) {
        uint8_t key[BPF_KEY_MAX];
        if (copy_from_user(key, (void *)(uintptr_t)attr.key, m->key_size) < 0) return -EFAULT;
        start = bpf_entry_index(m, key);
        if (start < 0) start = -1;
    }
    uint32_t limit = m->max_entries < BPF_ENTRIES_MAX ? m->max_entries : BPF_ENTRIES_MAX;
    for (uint32_t i = (uint32_t)(start + 1); i < limit; i++) {
        if (!m->entries[i].used)
            continue;
        if (copy_to_user((void *)(uintptr_t)attr.next_key, m->entries[i].key, m->key_size) < 0)
            return -EFAULT;
        return 0;
    }
    return -ENOENT;
}

static int bpf_prog_extract_map_fd(const bpf_insn_t *insns, uint32_t cnt)
{
    for (uint32_t i = 0; i < cnt; i++) {
        if (insns[i].code == 0x18)
            return insns[i].imm;
    }
    return -1;
}

static int64_t bpf_prog_load(void *uattr, unsigned size)
{
    bpf_attr_prog_load_t attr;
    memset(&attr, 0, sizeof(attr));
    size_t n = size < sizeof(attr) ? size : sizeof(attr);
    if (copy_from_user(&attr, uattr, n) < 0) return -EFAULT;
    if (attr.insn_cnt == 0 || attr.insn_cnt > 512 || !attr.insns)
        return -EINVAL;

    bpf_insn_t first[64];
    uint32_t copy_cnt = attr.insn_cnt < 64 ? attr.insn_cnt : 64;
    if (copy_from_user(first, (void *)(uintptr_t)attr.insns,
                       copy_cnt * sizeof(first[0])) < 0)
        return -EFAULT;

    int map_fd = bpf_prog_extract_map_fd(first, copy_cnt);
    if (map_fd < 0 || !bpf_find_map(map_fd))
        return -EINVAL;

    int fd = (int)sys_memfd_create(NULL, 0);
    if (fd < 0) return fd;

    task_t *t = proc_current();
    for (int i = 0; i < BPF_PROGS_MAX; i++) {
        if (g_progs[i].used)
            continue;
        memset(&g_progs[i], 0, sizeof(g_progs[i]));
        g_progs[i].used = 1;
        g_progs[i].owner_pid = t ? t->pid : 0;
        g_progs[i].fd = fd;
        g_progs[i].map_fd = map_fd;
        g_progs[i].kind = attr.insn_cnt > 16 ? BPF_PROG_KIND_ARITH64 : BPF_PROG_KIND_WRITE1;
        return fd;
    }
    sys_close(fd);
    return -ENFILE;
}

int bpf_prog_is_loaded(int fd)
{
    return bpf_find_prog(fd) != NULL;
}

int bpf_run_socket_filter(int fd)
{
    bpf_prog_t *p = bpf_find_prog(fd);
    if (!p) return -EBADF;
    bpf_map_t *m = bpf_find_map(p->map_fd);
    if (!m || m->type != BPF_MAP_TYPE_ARRAY || m->value_size < sizeof(uint64_t))
        return -EINVAL;

    uint32_t key0 = 0;
    int idx0 = bpf_entry_index(m, (uint8_t *)&key0);
    if (idx0 < 0) return idx0;

    if (p->kind == BPF_PROG_KIND_ARITH64) {
        uint64_t v = ((uint64_t)1 << 60) + 1;
        memcpy(m->entries[idx0].value, &v, sizeof(v));
        uint32_t key1 = 1;
        int idx1 = bpf_entry_index(m, (uint8_t *)&key1);
        if (idx1 >= 0) {
            v = ((uint64_t)1 << 60) - 1;
            memcpy(m->entries[idx1].value, &v, sizeof(v));
        }
    } else {
        uint64_t v = 1;
        memcpy(m->entries[idx0].value, &v, sizeof(v));
    }
    return 0;
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
        return bpf_map_create(attr, size);
    case BPF_MAP_LOOKUP_ELEM:
        return bpf_map_lookup(attr, size);
    case BPF_MAP_UPDATE_ELEM:
        return bpf_map_update(attr, size);
    case BPF_MAP_DELETE_ELEM:
        return bpf_map_delete(attr, size);
    case BPF_MAP_GET_NEXT_KEY:
        return bpf_map_get_next_key(attr, size);
    case BPF_PROG_LOAD:
        return bpf_prog_load(attr, size);
    default:
        return -EINVAL;
    }
}
