/* Configure once, then exec an ordinary Linux binary with unchanged argv. */
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>
#include "../../../kernel/include/uapi/a20/cluster_ssi.h"

static volatile sig_atomic_t stop;
static void stopped(int sig) {
    (void)sig;
    stop = 1;
}
static const char *const names[] = {
    "version",    "role",        "shared-memory", "automatic-threads",
    "max-pages",  "max-threads", "max-slices",    "quantum-ticks",
    "timeout-ms", "max-bytes",   "address",       "port",
    "token"};
static int parse(const char *path, struct a20_ssi_config *c) {
    *c = (struct a20_ssi_config){.version = 1,
                                 .max_pages = 32,
                                 .max_threads = 4,
                                 .max_slices = 64,
                                 .quantum_ticks = 2,
                                 .timeout_ms = 10000,
                                 .max_bytes = 524288,
                                 .port = 47220};
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char line[256];
    unsigned seen = 0, number = 0, total = 0;
    while (fgets(line, sizeof(line), f)) {
        number++;
        total += strlen(line);
        if (total > 4096 || (!strchr(line, '\n') && !feof(f)))
            goto bad;
        char *comment = strchr(line, '#');
        if (comment)
            *comment = 0;
        char key[64], value[128], extra[2];
        int n = sscanf(line, "%63s %127s %1s", key, value, extra);
        if (n < 1)
            continue;
        if (n != 2)
            goto bad;
        unsigned field;
        for (field = 0; field < sizeof(names) / sizeof(names[0]); field++)
            if (!strcmp(key, names[field]))
                break;
        if (field == sizeof(names) / sizeof(names[0]) || (seen & (1U << field)))
            goto bad;
        seen |= 1U << field;
        if (field == 1) {
            if (!strcmp(value, "off"))
                c->role = A20_SSI_OFF;
            else if (!strcmp(value, "home"))
                c->role = A20_SSI_HOME;
            else if (!strcmp(value, "worker"))
                c->role = A20_SSI_WORKER;
            else
                goto bad;
        } else if (field == 10) {
            if (inet_pton(AF_INET, value, c->address) != 1)
                goto bad;
        } else if (field == 12) {
            if (strlen(value) != 64)
                goto bad;
            for (unsigned i = 0; i < 32; i++) {
                unsigned byte = 0;
                for (unsigned j = 0; j < 2; j++) {
                    char ch = value[i * 2 + j];
                    unsigned digit =
                        ch >= '0' && ch <= '9'   ? (unsigned)(ch - '0')
                        : ch >= 'a' && ch <= 'f' ? (unsigned)(ch - 'a' + 10)
                                                 : 16;
                    if (digit == 16)
                        goto bad;
                    byte = byte * 16 + digit;
                }
                c->token[i] = byte;
            }
        } else {
            uint64_t value_num = 0;
            for (const char *p = value; *p; p++) {
                if (*p < '0' || *p > '9' || value_num > UINT32_MAX / 10U)
                    goto bad;
                value_num = value_num * 10 + (*p - '0');
                if (value_num > UINT32_MAX)
                    goto bad;
            }
            if (field == 11) {
                if (value_num > 65535)
                    goto bad;
                c->port = value_num;
            } else {
                uint32_t *fields[] = {&c->version,       NULL,
                                      &c->shared_memory, &c->automatic_threads,
                                      &c->max_pages,     &c->max_threads,
                                      &c->max_slices,    &c->quantum_ticks,
                                      &c->timeout_ms,    &c->max_bytes};
                *fields[field] = value_num;
            }
        }
    }
    if (ferror(f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    uint8_t token = 0;
    for (unsigned i = 0; i < 32; i++)
        token |= c->token[i];
    if ((seen & 3) != 3 || c->version != 1 || c->shared_memory > 1 ||
        c->automatic_threads > 1 || c->max_pages < 1 || c->max_pages > 64 ||
        c->max_threads < 1 || c->max_threads > 16 || c->max_slices < 1 ||
        c->max_slices > 100000 || c->quantum_ticks < 1 ||
        c->quantum_ticks > 100 || c->timeout_ms < 100 ||
        c->timeout_ms > 60000 || c->max_bytes < 8192 ||
        c->max_bytes > 1048576 || !c->port ||
        (c->role != A20_SSI_OFF && !token) ||
        (c->role != A20_SSI_OFF &&
         (!(seen & (1U << 12)) || !(seen & (1U << 10))))) {
        errno = EINVAL;
        return -1;
    }
    return 0;
bad:
    fprintf(stderr, "%s:%u: invalid, duplicate or unknown SSI setting\n", path,
            number);
    fclose(f);
    errno = EINVAL;
    return -1;
}
int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: a20ssi CONFIG [PROGRAM [ARG...]]\n");
        return 2;
    }
    struct a20_ssi_config c;
    if (parse(argv[1], &c)) {
        perror("a20ssi config");
        return 2;
    }
    if (c.role != A20_SSI_WORKER && argc < 3) {
        fprintf(stderr, "a20ssi: home/off requires a program\n");
        return 2;
    }
    if (c.role == A20_SSI_WORKER && argc != 2) {
        fprintf(stderr, "a20ssi: worker takes no program\n");
        return 2;
    }
    /* Explicit local-only configurations work on kernels compiled without
     * SSI as well, and perform no discovery/listen/connect operations. */
    int active =
        c.role == A20_SSI_WORKER ||
        (c.role == A20_SSI_HOME && c.shared_memory && c.automatic_threads);
    sigset_t wait_mask, old_mask;
    if (c.role == A20_SSI_WORKER) {
        signal(SIGINT, stopped);
        signal(SIGTERM, stopped);
        sigemptyset(&wait_mask);
        sigaddset(&wait_mask, SIGINT);
        sigaddset(&wait_mask, SIGTERM);
        if (sigprocmask(SIG_BLOCK, &wait_mask, &old_mask)) {
            perror("a20ssi signals");
            return 1;
        }
    }
    if (active && prctl(A20_PR_CLUSTER_SSI, &c, 0L, 0L, 0L)) {
        perror("a20ssi configure");
        return 1;
    }
    if (c.role == A20_SSI_WORKER) {
        puts("SSI_WORKER: READY");
        fflush(stdout);
        while (!stop)
            sigsuspend(&old_mask);
        c.role = A20_SSI_OFF;
        if (prctl(A20_PR_CLUSTER_SSI, &c, 0L, 0L, 0L)) {
            perror("a20ssi stop");
            return 1;
        }
        return 0;
    }
    execvp(argv[2], argv + 2);
    perror("a20ssi exec");
    return 1;
}
