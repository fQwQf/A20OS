#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "uapi/a20/cluster_config.h"
static const char config[] =
    "# bounded deployment\r\nversion 1\nnode "
    "00000000000000000000000000000001\nrole relay\nseed "
    "00000000000000000000000000000002 uart:1\nseed "
    "00000000000000000000000000000003 udp:10.0.3.3:44020\nservice agent "
    "/bin/agent\nservice control /bin/control\nservice jobs /bin/jobs\nprocess "
    "/bin/probe\n";
int main(void) {
    a20_cluster_config_t c, before;
    uint32_t line;
    assert(!a20_cluster_config_parse(config, sizeof(config) - 1, &c, &line));
    assert(c.node[15] == 1 && c.caps == 3 && c.seed_count == 2);
    assert(c.seeds[0].transport == 2 && c.seeds[1].hop[4] == 0xab &&
           c.seeds[1].hop[5] == 0xf4);
    before = c;
    const char *enabled = "version 1\nfeature oneway\nnode "
                          "00000000000000000000000000000001\nrole relay\n";
    assert(!a20_cluster_config_parse(enabled, strlen(enabled), &c, &line));
    assert(c.caps == 19);
    c = before;
    const char *bad[] = {
        "",
        "version 2\n",
        "version 1\nnode ffffffffffffffffffffffffffffffff\nrole worker\n",
        "version 1\nnode 00000000000000000000000000000001\nrole worker\nseed "
        "00000000000000000000000000000001 uart:1\n",
        "version 1\nnode 00000000000000000000000000000001\nrole worker\nrole "
        "relay\n",
        "version 1\nnode 00000000000000000000000000000001\nrole "
        "worker\nservice agent relative\n",
        "version 1\nnode 00000000000000000000000000000001\nrole "
        "worker\nprocess /bin/probe\n",
        "version 1\nnode 00000000000000000000000000000001\nrole worker\n"
        "feature unknown\n",
        "version 1\nnode 00000000000000000000000000000001\nrole worker\n"
        "feature oneway\nfeature oneway\n"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        assert(a20_cluster_config_parse(bad[i], strlen(bad[i]), &c, &line) < 0);
        assert(!memcmp(&c, &before, sizeof(c)));
    }
    char mutation[sizeof(config)];
    uint32_t random = 1;
    for (unsigned i = 0; i < 100000; i++) {
        memcpy(mutation, config, sizeof(config));
        random = random * 1664525u + 1013904223u;
        unsigned len = random % sizeof(config);
        mutation[random % sizeof(config)] = random >> 24;
        (void)a20_cluster_config_parse(mutation, len, &c, &line);
    }
    a20_restart_policy_t p = {.starts = 1, .started = 1};
    a20_restart_failed(&p, 10);
    assert(p.next_start == 110 && p.failures == 1);
    for (unsigned i = 0; i < 1000; i++)
        a20_restart_failed(&p, 10);
    assert(p.next_start == 6410 && p.failures == 7 && !p.exhausted);
    a20_restart_failed(&p, 60001);
    assert(p.failures == 1);
    p.exits = UINT64_MAX;
    a20_restart_failed(&p, 60002);
    assert(p.exhausted);
    puts("test_clx_config: atomic config, strict identity/address/manifest, "
         "100K mutations and restart bounds PASS");
}
