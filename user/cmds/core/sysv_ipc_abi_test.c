/*
 * SysV IPC ctl ABI conformance test.
 *
 * shmctl/msgctl/semctl are the only places where the kernel hands a
 * fixed-size reply to a libc struct it cannot see at compile time.  A size
 * mistake there fails silently in both directions: a reply longer than the
 * struct corrupts whatever the caller's buffer had next to it, and a shorter
 * one returns 0 while leaving the tail holding whatever was already there.
 *
 * The kernel-side STATIC_ASSERTs only pin the kernel's idea of the size, so
 * they cannot notice a disagreement with the libc actually linked in, and
 * cannot notice an under-sized reply at all.  This test pins the other end.
 * It fills a buffer with a canary byte, makes the call, and then checks
 * exactly which bytes the kernel touched.  Anything written past the struct is
 * an overrun; anything inside the struct still holding the canary was never
 * written.
 */

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>

#define CANARY 0xA5
#define PAD    64

#define CHK(cond, msg)                                                        \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("SYSV_IPC_ABI: FAIL %s:%d %s (errno=%d %s)\n", __func__,     \
                   __LINE__, (msg), errno, strerror(errno));                   \
            return 1;                                                          \
        }                                                                      \
    } while (0)

/* Report every byte the kernel wrote outside [0, struct_size). */
static int check_no_overrun(const char *what, const unsigned char *buf,
                            size_t struct_size, size_t total)
{
    size_t first = 0, count = 0;
    for (size_t i = struct_size; i < total; i++) {
        if (buf[i] != CANARY) {
            if (!count)
                first = i;
            count++;
        }
    }
    if (count) {
        printf("SYSV_IPC_ABI: FAIL %s overran struct by %zu bytes "
               "(offsets %zu..%zu, struct is %zu)\n",
               what, count, first, first + count - 1, struct_size);
        return 1;
    }
    return 0;
}

/* Report every byte inside the struct the kernel left untouched. */
static int check_fully_written(const char *what, const unsigned char *buf,
                               size_t from, size_t struct_size)
{
    for (size_t i = from; i < struct_size; i++) {
        if (buf[i] == CANARY) {
            printf("SYSV_IPC_ABI: FAIL %s never wrote offset %zu "
                   "(struct is %zu bytes)\n", what, i, struct_size);
            return 1;
        }
    }
    return 0;
}

static int test_shm_info(void)
{
    int id = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0600);
    CHK(id >= 0, "shmget");

    unsigned char buf[sizeof(struct shm_info) + PAD];
    memset(buf, CANARY, sizeof(buf));
    /* The prototype says shmid_ds* for every cmd; what the kernel writes is
     * the struct the cmd names, which is why the buffer is sized here. */
    CHK(shmctl(id, SHM_INFO, (struct shmid_ds *)buf) == 0, "shmctl(SHM_INFO)");
    CHK(check_no_overrun("shmctl(SHM_INFO)", buf, sizeof(struct shm_info),
                         sizeof(buf)) == 0, "shm_info overrun");
    printf("  shmctl(SHM_INFO): %zu-byte struct, no overrun\n",
           sizeof(struct shm_info));

    shmctl(id, IPC_RMID, NULL);
    return 0;
}

static int test_shm_stat(void)
{
    int id = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0600);
    CHK(id >= 0, "shmget");

    /* SHM_STAT must fill the same layout as IPC_STAT; it used to fall through
     * and return 0 with the caller's buffer untouched. */
    unsigned char buf[sizeof(struct shmid_ds) + PAD];
    memset(buf, CANARY, sizeof(buf));
    CHK(shmctl(id, SHM_STAT, (struct shmid_ds *)buf) == 0, "shmctl(SHM_STAT)");
    CHK(check_fully_written("shmctl(SHM_STAT)", buf, 0, sizeof(struct shmid_ds))
        == 0, "shmid_ds under-filled");
    CHK(check_no_overrun("shmctl(SHM_STAT)", buf, sizeof(struct shmid_ds),
                         sizeof(buf)) == 0, "shmid_ds overrun");

    unsigned char ibuf[sizeof(struct shm_info) + PAD];
    memset(ibuf, CANARY, sizeof(ibuf));
    CHK(shmctl(id, IPC_STAT, (struct shmid_ds *)ibuf) == 0, "shmctl(IPC_STAT)");
    CHK(check_no_overrun("shmctl(IPC_STAT)", ibuf, sizeof(struct shmid_ds),
                         sizeof(ibuf)) == 0, "shmid_ds overrun");
    printf("  shmctl(SHM_STAT/IPC_STAT): %zu-byte struct, fully written\n",
           sizeof(struct shmid_ds));

    shmctl(id, IPC_RMID, NULL);
    return 0;
}

static int test_msg_stat(void)
{
    int id = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
    CHK(id >= 0, "msgget");

    /* msg_lspid, msg_lrpid and __unused sit past the 96 bytes the kernel used
     * to write, so they came back holding the caller's own stack garbage. */
    unsigned char buf[sizeof(struct msqid_ds) + PAD];
    memset(buf, CANARY, sizeof(buf));
    CHK(msgctl(id, IPC_STAT, (struct msqid_ds *)buf) == 0, "msgctl(IPC_STAT)");
    CHK(check_fully_written("msgctl(IPC_STAT)", buf,
                            offsetof(struct msqid_ds, msg_lspid),
                            sizeof(struct msqid_ds)) == 0,
        "msqid_ds tail under-filled");
    CHK(check_no_overrun("msgctl(IPC_STAT)", buf, sizeof(struct msqid_ds),
                         sizeof(buf)) == 0, "msqid_ds overrun");
    printf("  msgctl(IPC_STAT): %zu-byte struct, tail written from offset %zu\n",
           sizeof(struct msqid_ds), offsetof(struct msqid_ds, msg_lspid));

    /* MSG_STAT returns the id rather than 0. */
    struct msqid_ds ds;
    memset(&ds, 0, sizeof(ds));
    CHK(msgctl(id, MSG_STAT, &ds) == id, "msgctl(MSG_STAT) returns id");

    msgctl(id, IPC_RMID, NULL);
    return 0;
}

static int test_sem_stat(void)
{
    int id = semget(IPC_PRIVATE, 4, IPC_CREAT | 0600);
    CHK(id >= 0, "semget");

    unsigned char buf[sizeof(struct semid_ds) + PAD];
    memset(buf, CANARY, sizeof(buf));
    CHK(semctl(id, 0, SEM_STAT, buf) == id, "semctl(SEM_STAT)");
    CHK(check_fully_written("semctl(SEM_STAT)", buf, 0, sizeof(struct semid_ds))
        == 0, "semid_ds under-filled");
    CHK(check_no_overrun("semctl(SEM_STAT)", buf, sizeof(struct semid_ds),
                         sizeof(buf)) == 0, "semid_ds overrun");
    printf("  semctl(SEM_STAT): %zu-byte struct, fully written\n",
           sizeof(struct semid_ds));

    unsigned char ibuf[sizeof(struct seminfo) + PAD];
    memset(ibuf, CANARY, sizeof(ibuf));
    CHK(semctl(id, 0, SEM_INFO, (struct seminfo *)ibuf) == id,
        "semctl(SEM_INFO)");
    CHK(check_no_overrun("semctl(SEM_INFO)", ibuf, sizeof(struct seminfo),
                         sizeof(ibuf)) == 0, "seminfo overrun");
    const struct seminfo *info = (const struct seminfo *)ibuf;
    printf("  semctl(SEM_INFO): %zu-byte struct, semmni=%d semusz=%d\n",
           sizeof(struct seminfo), info->semmni, info->semusz);

    semctl(id, 0, IPC_RMID);
    return 0;
}

int main(void)
{
    if (test_shm_info() || test_shm_stat() || test_msg_stat() ||
        test_sem_stat()) {
        printf("SYSV_IPC_ABI: FAIL\n");
        return 1;
    }
    printf("SYSV_IPC_ABI: PASS\n");
    return 0;
}
