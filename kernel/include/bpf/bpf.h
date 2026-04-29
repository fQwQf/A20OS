#ifndef _BPF_BPF_H
#define _BPF_BPF_H

#include "core/types.h"

#define BPF_MAP_CREATE       0
#define BPF_MAP_LOOKUP_ELEM  1
#define BPF_MAP_UPDATE_ELEM  2
#define BPF_MAP_DELETE_ELEM  3
#define BPF_MAP_GET_NEXT_KEY 4
#define BPF_PROG_LOAD        5

#define BPF_MAP_TYPE_HASH    1
#define BPF_MAP_TYPE_ARRAY   2

#define BPF_KEY_MAX          16
#define BPF_VALUE_MAX        1024
#define BPF_PROG_LOAD_COPY_MAX 64

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

int bpf_map_value_size(int fd, size_t *value_size);
int bpf_map_key_value_size(int fd, size_t *key_size, size_t *value_size);
int64_t bpf_map_create(const bpf_attr_map_create_t *attr);
int64_t bpf_map_update(const bpf_attr_elem_t *attr, const void *key, const void *value);
int64_t bpf_map_lookup(const bpf_attr_elem_t *attr, const void *key, void *value);
int64_t bpf_map_delete(const bpf_attr_elem_t *attr, const void *key);
int64_t bpf_map_get_next_key(const bpf_attr_elem_t *attr, const void *key, void *next_key);
int64_t bpf_prog_load(const bpf_attr_prog_load_t *attr, const bpf_insn_t *insns, uint32_t insn_count);

int bpf_prog_is_loaded(int fd);
int bpf_run_socket_filter(int fd);
void bpf_release_process(int pid);

#endif /* _BPF_BPF_H */
