/* Submit a bounded text file to the local coordinator; agents may be remote. */
#include "liba20rt/a20_cluster_client.h"
#include "liba20rt/crt0_a20.h"
#include "../../kernel/include/uapi/a20/cluster_word_job.h"
static a20_handle_t output = A20_HANDLE_NULL;
static int error(const char *message) {
    if (output != A20_HANDLE_NULL)
        a20_hdl_write_buf(output, message, a20_strlen(message), 0);
    return 1;
}
int main(int argc, char **argv, char **envp) {
    (void)envp;
    a20_start_info_t *start = a20_get_start_info();
    if (start)
        output = start->stdout_handle;
    if (argc != 2)
        return error(
            "usage: cluster-wordcount <text-file; at most 768 bytes>\n");
    a20_path_open_args_t file = {.dir = A20_HANDLE_NULL,
                                 .path = (uint64_t)(uintptr_t)argv[1],
                                 .path_len = (uint32_t)a20_strlen(argv[1]),
                                 .rights = A20_RIGHT_READ};
    if (a20_path_open(&file) < 0)
        return error("wordcount: cannot open input\n");
    uint8_t request[32 + A20_JOB_MAX_TEXT + 1] = {0};
    unsigned used = 0;
    for (;;) {
        uint64_t got = 0;
        if (a20_hdl_read_buf(file.out_handle, request + 32 + used,
                             A20_JOB_MAX_TEXT + 1 - used, &got) < 0 ||
            got > A20_JOB_MAX_TEXT + 1 - used) {
            a20_hdl_close(file.out_handle);
            return error("wordcount: input read failed\n");
        }
        used += got;
        if (used > A20_JOB_MAX_TEXT) {
            a20_hdl_close(file.out_handle);
            return error("wordcount: input too large\n");
        }
        if (!got)
            break;
    }
    a20_hdl_close(file.out_handle);
    uint64_t id = a20_agent_clock_ms(0) + 1;
    if (!id)
        return error("wordcount: clock exhausted\n");
    request[0] = 'C';
    request[1] = 'J';
    request[2] = 1;
    request[3] = A20_JOB_WORDCOUNT;
    a20_work_put64(request + 4, id);
    a20_work_put32(request + 12, 10000);
    a20_work_put32(request + 16, used);
    a20_handle_t ep = A20_HANDLE_NULL;
    if (a20_cluster_connect(0, 0, "cluster.jobs", 0, 0, &ep) < 0)
        return error("wordcount: coordinator unavailable\n");
    uint8_t reply[32 + A20_WORD_RESULT];
    uint32_t len = sizeof(reply);
    int r = a20_cluster_exchange(ep, request, 32 + used, reply, &len, 11000);
    a20_hdl_close(ep);
    if (r < 0 || len < 32 || reply[0] != 'C' || reply[1] != 'J' ||
        reply[2] != 1 || reply[3] != A20_JOB_WORDCOUNT ||
        a20_work_u64(reply + 4) != id || a20_work_u32(reply + 12) ||
        a20_work_u32(reply + 28) != len - 32 ||
        !a20_word_dictionary_valid(reply + 32, len - 32))
        return error("wordcount: job failed\n");
    const uint8_t *dictionary = reply + 32;
    for (unsigned i = 0; i < a20_work_u32(dictionary); i++) {
        const uint8_t *entry = dictionary + 4 + i * 16;
        char line[24], digits[10];
        unsigned at = 0, count = 0;
        while (at < 12 && entry[at]) {
            line[at] = entry[at];
            at++;
        }
        line[at++] = ' ';
        uint32_t value = a20_work_u32(entry + 12);
        do {
            digits[count++] = (char)('0' + value % 10);
            value /= 10;
        } while (value);
        while (count)
            line[at++] = digits[--count];
        line[at++] = '\n';
        if (output != A20_HANDLE_NULL &&
            a20_hdl_write_buf(output, line, at, 0) < 0)
            return 1;
    }
    return 0;
}
