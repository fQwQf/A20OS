#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../kernel/include/uapi/a20/cluster_app_manifest.h"
static char bytes[A20_APP_MANIFEST_BYTES + 2];
static a20_app_manifest_entry_t entries[2];
static const char header[] =
    "A20APPS1\napp c5000000000000000000000000000005 /bin/test 62 64 1 1 2\n";
static int parse(const char *input, unsigned capacity, uint32_t *count) {
    strcpy(bytes, input);
    return a20_app_manifest_parse(bytes, strlen(bytes), entries, capacity,
                                  count);
}
int main(void) {
    uint32_t count = 99;
    const char *gold =
        "A20APPS1\r\n# comment\r\napp c5000000000000000000000000000005 "
        "/bin/test 62 64 1 1 2\r\narg hello world\r\narg \r\narg 多架构\r\nenv "
        "A20_TEST=cluster\r\nenv EMPTY=\r\nend\r\n";
    assert(!parse(gold, 2, &count) && count == 1);
    a20_app_manifest_entry_t *e = &entries[0];
    assert(e->argc == 4 && e->envc == 2 && !strcmp(e->argv[0], "/bin/test") &&
           !strcmp(e->argv[1], "hello world") && !e->argv[2][0] &&
           !strcmp(e->argv[3], "多架构") && !e->argv[4] &&
           !strcmp(e->envp[1], "EMPTY=") && !e->envp[2] &&
           e->variant.image == (uintptr_t)e);
    a20_execution_info_t execution = {.size = 64,
                                      .version = 1,
                                      .elf_machine = 62,
                                      .pointer_bits = 64,
                                      .byte_order = 1,
                                      .calling_abi = 1,
                                      .native_major = 1,
                                      .features = 2,
                                      .memory_model = 2,
                                      .atomic_model = 1};
    assert(a20_execution_image_matches(&execution, &e->variant.requirement));
    assert(!parse("A20APPS1\napp c5000000000000000000000000000005 /x 62 64 1 1 "
                  "2\nrequire-feature 4\nrequire-feature 255\nend\n",
                  2, &count));
    uint64_t supported[4] = {0};
    assert(!a20_app_manifest_features_match(&entries[0], supported));
    supported[0] = 1ULL << 4;
    assert(!a20_app_manifest_features_match(&entries[0], supported));
    supported[3] = 1ULL << 63;
    assert(a20_app_manifest_features_match(&entries[0], supported));
    const char *invalid[] = {
        "",
        "A20APPS1\n",
        "A20APPS2\nend\n",
        "A20APPS1\narg nope\n",
        "A20APPS1\napp f /bin/test 62 64 1 1 2\nend\n",
        "A20APPS1\napp 00000000000000000000000000000000 /x 62 64 1 1 2\nend\n",
        "A20APPS1\napp c5000000000000000000000000000005 /x 4294967296 64 1 1 "
        "2\nend\n",
        "A20APPS1\napp c5000000000000000000000000000005 /x 62 16 1 1 2\nend\n",
        "A20APPS1\napp c5000000000000000000000000000005 /x 62 64 1 1 "
        "256\nend\n"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        assert(parse(invalid[i], 2, &count) < 0);
    char input[4096];
    const char *bad_tail[] = {"arg before end\n",
                              "env BAD\nend\n",
                              "env =value\nend\n",
                              "other\nend\n",
                              "require-feature 256\nend\n",
                              "require-feature 4\nrequire-feature 4\nend\n",
                              "require-feature -1\nend\n",
                              "require-feature 4 extra\nend\n",
                              "require-feature \nend\n"};
    for (unsigned i = 0; i < sizeof(bad_tail) / sizeof(bad_tail[0]); i++) {
        strcpy(input, header);
        strcat(input, bad_tail[i]);
        assert(parse(input, 2, &count) < 0 && !count);
    }
    strcpy(input, header);
    for (unsigned i = 0; i < 16; i++)
        strcat(input, "arg extra\n");
    strcat(input, "end\n");
    assert(parse(input, 2, &count) < 0 && !count);
    strcpy(input, header);
    for (unsigned i = 0; i < 17; i++)
        strcat(input, "env KEY=value\n");
    strcat(input, "end\n");
    assert(parse(input, 2, &count) < 0 && !count);
    strcpy(input, header);
    strcat(input, "end\n");
    strcat(input, header + 9);
    strcat(input, "end\n");
    assert(parse(input, 1, &count) < 0 && !count);
    assert(!parse(input, 2, &count) && count == 2);
    assert(entries[1].variant.image == (uintptr_t)&entries[1]);
    strcpy(bytes, header);
    unsigned len = strlen(bytes);
    bytes[10] = 0;
    assert(a20_app_manifest_parse(bytes, len, entries, 2, &count) < 0 &&
           !count);
    memset(bytes, 'x', sizeof(bytes));
    assert(a20_app_manifest_parse(bytes, A20_APP_MANIFEST_BYTES + 1, entries, 2,
                                  &count) < 0);
    puts("test_clx_app_manifest: exact args/env, bounded entries, malformed "
         "input and portable image selection PASS");
    return 0;
}
