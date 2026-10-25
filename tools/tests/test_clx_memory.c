#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/include/uapi/a20/cluster_memory.h"
#include "../../user/liba20rt/a20_cluster_client.h"
static a20_status_t fixture_close(a20_handle_t h);
static uint64_t fixture_clock(void *ctx);
static a20_status_t fixture_exchange(a20_handle_t ep, const void *q, uint32_t n,
                                     uint8_t *r, uint32_t *cap,
                                     uint32_t budget);
#define a20_hdl_close fixture_close
#define a20_agent_clock_ms fixture_clock
#define a20_cluster_exchange fixture_exchange
#include "../../user/liba20rt/a20_cluster_memory.h"
#undef a20_hdl_close
#undef a20_agent_clock_ms
#undef a20_cluster_exchange

static union {
    uint64_t alignment;
    uint8_t bytes[A20_MEMORY_STORAGE];
} owner;
static uint8_t owner_id[64], client_id[64];
static unsigned calls, closes, fail_call, truncate_call;
static uint64_t clock_ms = 1;
void a20_memory_panic(void) {
    abort();
}
a20_start_info_t *a20_get_start_info(void) {
    return 0;
}
static a20_status_t fixture_close(a20_handle_t h) {
    (void)h;
    closes++;
    return 0;
}
static uint64_t fixture_clock(void *ctx) {
    (void)ctx;
    return clock_ms;
}
static a20_status_t fixture_exchange(a20_handle_t ep, const void *q, uint32_t n,
                                     uint8_t *r, uint32_t *cap,
                                     uint32_t budget) {
    (void)ep;
    assert(budget);
    calls++;
    if (calls == fail_call)
        return -A20_ERR_TIMED_OUT;
    int result =
        a20_memory_owner_dispatch(owner.bytes, q, n, r, *cap, clock_ms);
    assert(result >= 192);
    *cap = calls == truncate_call ? 1 : (uint32_t)result;
    return 0;
}
static a20_memory_session_t session(void) {
    a20_memory_session_t s = {
        .endpoint = 3,
        .permission = A20_MEMORY_READ_WRITE,
        .config = {A20_MEMORY_READ_WRITE, A20_MEMORY_MAX_PAGES, 100, 0}};
    memcpy(s.owner, owner_id, 64);
    memcpy(s.client, client_id, 64);
    return s;
}
static void initialize(void) {
    a20_memory_config_t c = {A20_MEMORY_READ_WRITE, A20_MEMORY_MAX_PAGES, 100,
                             0};
    a20_object_id_t id = {.instance_id = {1},
                          .domain_id = {2},
                          .boot_epoch = 3,
                          .serial = 1,
                          .kind = 1};
    assert(!a20_object_id_encode(owner_id, 64, &id));
    id.serial = 2;
    assert(!a20_object_id_encode(client_id, 64, &id));
    assert(
        !a20_memory_owner_init(owner.bytes, sizeof(owner.bytes), &c, owner_id));
    assert(a20_memory_owner_init(owner.bytes, 1, &c, owner_id) < 0);
    assert(a20_memory_owner_init(owner.bytes + 1, sizeof(owner.bytes) - 1, &c,
                                 owner_id) < 0);
    calls = closes = fail_call = truncate_call = 0;
    clock_ms = 1;
}
static void config_test(void) {
    const char *good = "version 1\r\n# optional by default\nmax-pages 1\n";
    a20_memory_config_t c;
    uint32_t line = 99;
    assert(!a20_memory_config_parse(good, strlen(good), &c, &line));
    assert(!c.mode && c.pages == 1 && c.transaction_ms == 5000 &&
           !c.bytes_per_second && !line);
    good = "version 1\nmode read-write\nmax-pages 1\ntransaction-ms "
           "30000\nbytes-per-second 512\n";
    assert(!a20_memory_config_parse(good, strlen(good), &c, &line));
    assert(c.mode == 2 && c.pages == 1 && c.transaction_ms == 30000 &&
           c.bytes_per_second == 512);
    const char *initial_text =
        "version 1\nmode read-only\ninitial-file /bin/model.bin\n";
    char path[A20_CONFIG_PATH];
    assert(!a20_memory_config_parse_file(initial_text, strlen(initial_text), &c,
                                         path, &line));
    assert(c.mode == 1 && !strcmp(path, "/bin/model.bin"));
    assert(a20_memory_config_parse(initial_text, strlen(initial_text), &c,
                                   &line) < 0);
    const char *bad[] = {"",
                         "mode off\n",
                         "version 2\n",
                         "version 1\nmode shared\n",
                         "version 1\nmode off\nmode read-write\n",
                         "version 1\nmax-pages 0\n",
                         "version 1\nmax-pages 65\n",
                         "version 1\ntransaction-ms 30001\n",
                         "version 1\nbytes-per-second 511\n",
                         "version 1\nbytes-per-second -1\n",
                         "version 1\nbytes-per-second 4294967296\n",
                         "version 1\nunknown 1\n",
                         "version 1\nmode off trailing\n",
                         "version 1\nmode off#comment\n"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        a20_memory_config_t before = c;
        assert(a20_memory_config_parse(bad[i], strlen(bad[i]), &c, &line) < 0);
        assert(!memcmp(&c, &before, sizeof(c)));
    }
    unsigned seed = 123;
    for (unsigned i = 0; i < 100000; i++) {
        char input[128];
        size_t n = strlen(good);
        memcpy(input, good, n);
        for (unsigned j = 0; j < 4; j++) {
            seed = seed * 1664525u + 1013904223u;
            input[seed % n] = (char)(seed >> 24);
        }
        a20_memory_config_t before = c;
        if (a20_memory_config_parse(input, n, &c, &line) < 0)
            assert(!memcmp(&c, &before, sizeof(c)));
        else
            assert(c.mode <= 2 && c.pages && c.pages <= A20_MEMORY_MAX_PAGES &&
                   c.transaction_ms && c.transaction_ms <= 30000 &&
                   (!c.bytes_per_second || c.bytes_per_second >= 512));
    }
}
static void sdk_test(void) {
    initialize();
    a20_memory_session_t s = session();
    uint8_t bytes[4096], readback[4096];
    uint64_t version = 99;
    assert(a20_memory_connect((uint8_t[16]){1}, A20_MEMORY_OFF, 100, &s) ==
           -A20_ERR_NOT_SUPPORTED);
    assert(!calls);
    uint8_t seed[512];
    memset(seed, 13, sizeof(seed));
    assert(!a20_memory_owner_seed(owner.bytes, 0, seed, sizeof(seed)));
    assert(!a20_memory_read_page(&s, 0, bytes, &version, 100) && version == 1);
    for (unsigned i = 0; i < 4096; i++)
        assert(bytes[i] == (i < 512 ? 13 : 0));
    a20_memory_ticket_t t;
    assert(!a20_memory_begin(&s, 0, version, 1, &t, 100));
    memset(bytes, 42, 4096);
    assert(!a20_memory_stage_page(&s, &t, bytes, 100));
    assert(!a20_memory_read_page(&s, 0, readback, &version, 100) &&
           version == 1);
    for (unsigned i = 0; i < 4096; i++)
        assert(readback[i] == (i < 512 ? 13 : 0));
    assert(!a20_memory_finish(&s, &t, A20_MEMORY_COMMIT, &version, 100) &&
           version == 2);
    assert(!a20_memory_finish(&s, &t, A20_MEMORY_COMMIT, &version, 100) &&
           version == 2);
    assert(!a20_memory_read_page(&s, 0, readback, &version, 100));
    assert(!memcmp(bytes, readback, 4096));
    assert(a20_memory_begin(&s, 0, 1, 2, &t, 100) == -A20_ERR_EXISTS);
    unsigned before = calls;
    s.permission = A20_MEMORY_READ_ONLY;
    assert(a20_memory_begin(&s, 0, 2, 3, &t, 100) == -A20_ERR_ACCESS &&
           calls == before);
    s.permission = A20_MEMORY_READ_WRITE;
    assert(!a20_memory_begin(&s, 0, 2, 3, &t, 100));
    clock_ms = 102;
    assert(a20_memory_finish(&s, &t, A20_MEMORY_COMMIT, &version, 100) ==
           -A20_ERR_EXPIRED);
    // A failed multi-chunk refresh cannot leak a partial snapshot to caller.
    memset(readback, 99, 4096);
    version = 77;
    fail_call = calls + 3;
    assert(a20_memory_read_page(&s, 0, readback, &version, 100) ==
           -A20_ERR_TIMED_OUT);
    assert(version == 77 && s.endpoint == A20_HANDLE_NULL && closes == 1);
    for (unsigned i = 0; i < 4096; i++)
        assert(readback[i] == 99);
    s = session();
    fail_call = 0;
    truncate_call = calls + 1;
    assert(a20_memory_read_page(&s, 0, readback, &version, 100) ==
           -A20_ERR_PROTOCOL);
    assert(s.endpoint == A20_HANDLE_NULL && closes == 2);
}
int main(void) {
    config_test();
    sdk_test();
    puts("cluster memory: C/Rust wire, config 100K mutations, SDK "
         "snapshot/transaction/error boundaries PASS");
    return 0;
}
