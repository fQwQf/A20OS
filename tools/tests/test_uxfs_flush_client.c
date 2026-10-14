#include <assert.h>
#include <stdint.h>
#include <string.h>

typedef int64_t (*syscall_fn)(uint64_t, uint64_t, uint64_t, uint64_t,
                              uint64_t, uint64_t, uint64_t);
typedef struct {
    uint32_t size, version;
    int32_t block_index;
    uint32_t write;
    uint64_t lba;
    uint32_t count, pad;
    uint64_t buf;
} fs_block_args_t;

int64_t fsio_flush_supported_via(syscall_fn);
int64_t fsio_flush_query_via(syscall_fn, int32_t, uint64_t *);
int64_t a20_ufsd_set_block_index(int32_t);

static struct { uint64_t response, sentinel; } output;
static int calls;

static int64_t legacy_kernel(uint64_t nr, uint64_t arg, uint64_t b, uint64_t c,
                             uint64_t d, uint64_t e, uint64_t f)
{
    (void)b; (void)c; (void)d; (void)e; (void)f;
    assert(nr == 0x0417);
    fs_block_args_t *a = (fs_block_args_t *)(uintptr_t)arg;
    assert(a->version == 2 && a->write == 2 && a->count == 0);
    calls++;
    if (a->version > 1) return -22; /* Old validator fails closed. */
    *(uint64_t *)(uintptr_t)a->buf = 0x123456789abcdef0ull;
    return 0;
}

static int64_t current_kernel(uint64_t nr, uint64_t arg, uint64_t b, uint64_t c,
                              uint64_t d, uint64_t e, uint64_t f)
{
    (void)b; (void)c; (void)d; (void)e; (void)f;
    assert(nr == 0x0417);
    fs_block_args_t *a = (fs_block_args_t *)(uintptr_t)arg;
    assert(a->version == 2 && a->write == 2 && a->count == 0);
    *(uint64_t *)(uintptr_t)a->buf = 0x5546585300000001ull;
    return 0;
}

static int64_t capacity_ignoring_version(uint64_t nr, uint64_t arg,
                                         uint64_t b, uint64_t c, uint64_t d,
                                         uint64_t e, uint64_t f)
{
    (void)b; (void)c; (void)d; (void)e; (void)f;
    assert(nr == 0x0417);
    fs_block_args_t *a = (fs_block_args_t *)(uintptr_t)arg;
    assert(a->version == 2 && a->write == 2 && a->count == 0);
    *(uint64_t *)(uintptr_t)a->buf = 0x123456789abcdef0ull;
    return 0;
}

int main(void)
{
    assert(a20_ufsd_set_block_index(3) == 0);
    output.response = 0xfeedfacecafebeefull;
    output.sentinel = 0xfeedfacecafebeefull;
    assert(fsio_flush_query_via(legacy_kernel, 3, &output.response) == 0);
    assert(output.response == 0);
    assert(output.sentinel == 0xfeedfacecafebeefull); /* Legacy u64 write stays in response. */
    assert(calls == 1);
    assert(fsio_flush_query_via(capacity_ignoring_version, 3,
                                &output.response) == 0);
    assert(output.response == 0x123456789abcdef0ull);
    assert(output.sentinel == 0xfeedfacecafebeefull);
    assert(fsio_flush_query_via(current_kernel, 3, &output.response) == 1);
    assert(fsio_flush_supported_via(legacy_kernel) == 0);
    assert(fsio_flush_supported_via(current_kernel) == 1);
    return 0;
}
