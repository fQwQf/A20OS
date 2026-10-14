#include "ipc/envelope_acquire_race_test.h"

#ifdef CONFIG_ENVELOPE_ACQUIRE_RACE_TEST
#include "core/errno.h"
#include "core/lock.h"
#include "fs/fdtable.h"
#include "fs/file.h"
#include "fs/vfs.h"
#include "proc/proc.h"

typedef struct acquire_race_arm {
    task_t *task;
    vfile_t *replacement;
    int operation;
    int armed;
} acquire_race_arm_t;

static spinlock_t g_acquire_race_lock = SPINLOCK_INIT;
static acquire_race_arm_t g_acquire_race_arm;
static a20_envelope_race_test_result_t g_acquire_race_result;
static uint64_t g_expected_incoming_identity;
static task_t *g_acquire_race_result_task;

int a20_envelope_race_test_arm(int operation, int replacement_fd)
{
    if (operation != A20_ENVELOPE_RACE_OP_SCM &&
        operation != A20_ENVELOPE_RACE_OP_PIDFD)
        return -EINVAL;
    task_t *task = proc_current();
    if (!task)
        return -ESRCH;
    task = proc_get(task);
    if (!task)
        return -ESRCH;
    vfile_t *replacement = fdtable_get_file_ref(task, replacement_fd, NULL);
    if (!replacement) {
        proc_put(task);
        return -EBADF;
    }

    uint64_t flags = spin_lock_irqsave(&g_acquire_race_lock);
    if (g_acquire_race_arm.armed || g_acquire_race_result_task) {
        spin_unlock_irqrestore(&g_acquire_race_lock, flags);
        vfs_put_file(replacement);
        proc_put(task);
        return -EBUSY;
    }
    g_acquire_race_arm.task = task;
    g_acquire_race_arm.replacement = replacement;
    g_acquire_race_arm.operation = operation;
    g_acquire_race_arm.armed = 1;
    g_acquire_race_result_task = task;
    g_expected_incoming_identity = 0;
    g_acquire_race_result = (a20_envelope_race_test_result_t){
        .operation = operation,
        .status = -EAGAIN,
        .installed_fd = -1,
        .pin_release_count = 0,
    };
    spin_unlock_irqrestore(&g_acquire_race_lock, flags);
    return 0;
}

int a20_envelope_race_test_result(a20_envelope_race_test_result_t *out)
{
    if (!out)
        return -EINVAL;
    uint64_t flags = spin_lock_irqsave(&g_acquire_race_lock);
    if (g_acquire_race_result_task != proc_current()) {
        spin_unlock_irqrestore(&g_acquire_race_lock, flags);
        return -EPERM;
    }
    *out = g_acquire_race_result;
    task_t *held_task = g_acquire_race_result_task;
    vfile_t *replacement = NULL;
    if (g_acquire_race_arm.armed && g_acquire_race_arm.task == held_task) {
        replacement = g_acquire_race_arm.replacement;
        g_acquire_race_arm = (acquire_race_arm_t){0};
    }
    g_acquire_race_result_task = NULL;
    g_expected_incoming_identity = 0;
    spin_unlock_irqrestore(&g_acquire_race_lock, flags);
    if (replacement)
        vfs_put_file(replacement);
    proc_put(held_task);
    return 0;
}

int a20_envelope_race_test_before_mediation(int operation, int installed_fd,
                                             void *incoming_vfile)
{
    task_t *current = proc_current();
    vfile_t *replacement = NULL;
    uint64_t flags = spin_lock_irqsave(&g_acquire_race_lock);
    if (!g_acquire_race_arm.armed || g_acquire_race_arm.task != current ||
        g_acquire_race_arm.operation != operation) {
        spin_unlock_irqrestore(&g_acquire_race_lock, flags);
        return 0;
    }
    replacement = g_acquire_race_arm.replacement;
    g_acquire_race_arm = (acquire_race_arm_t){0};
    g_expected_incoming_identity = ((vfile_t *)incoming_vfile)->identity;
    g_acquire_race_result.operation = operation;
    g_acquire_race_result.status = -EINPROGRESS;
    g_acquire_race_result.installed_fd = installed_fd;
    g_acquire_race_result.pin_release_count = 0;
    spin_unlock_irqrestore(&g_acquire_race_lock, flags);

    int rc = -EBADF;
    /* The pinned vfile is installed with a lock-protected compare-and-swap
     * against the newly installed incoming object. */
    rc = fdtable_test_replace_current_if_file(installed_fd,
                (vfile_t *)incoming_vfile, replacement);
    vfs_put_file(replacement);
    flags = spin_lock_irqsave(&g_acquire_race_lock);
    g_acquire_race_result.status = rc < 0 ? rc : 0;
    spin_unlock_irqrestore(&g_acquire_race_lock, flags);
    return rc < 0 ? rc : 1;
}

void a20_envelope_race_test_note_pin_release(int operation,
                                              uint64_t incoming_identity)
{
    uint64_t flags = spin_lock_irqsave(&g_acquire_race_lock);
    if (g_acquire_race_result.operation == operation &&
        g_acquire_race_result.status == 0 &&
        g_expected_incoming_identity == incoming_identity) {
        g_acquire_race_result.pin_release_count++;
    }
    spin_unlock_irqrestore(&g_acquire_race_lock, flags);
}

void a20_envelope_race_test_cancel_task(task_t *task)
{
    if (!task)
        return;
    task_t *held_task = NULL;
    vfile_t *replacement = NULL;
    uint64_t flags = spin_lock_irqsave(&g_acquire_race_lock);
    if (g_acquire_race_arm.armed && g_acquire_race_arm.task == task) {
        held_task = g_acquire_race_arm.task;
        replacement = g_acquire_race_arm.replacement;
        g_acquire_race_arm = (acquire_race_arm_t){0};
        g_acquire_race_result_task = NULL;
        g_acquire_race_result.status = -ECANCELED;
        g_expected_incoming_identity = 0;
    } else if (g_acquire_race_result_task == task) {
        held_task = g_acquire_race_result_task;
        g_acquire_race_result_task = NULL;
        g_expected_incoming_identity = 0;
    }
    spin_unlock_irqrestore(&g_acquire_race_lock, flags);
    if (replacement)
        vfs_put_file(replacement);
    if (held_task)
        proc_put(held_task);
}
#endif
