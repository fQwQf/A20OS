#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "core/consts.h"
#include "core/klog.h"

static int wait_diag_count;

#define WNOHANG 1

int proc_wait4(int pid, int *status, int options)
{
    task_t *t = proc_current();

    if (wait_diag_count < 128) {
        wait_diag_count++;
        kdebug("[WAITDBG] enter cur=%d pid=%d opt=0x%x\n",
               t ? t->pid : -1, pid, options);
    }

retry:;
    int found = 0;
    for (int i = 0; i < MAX_PROCS; i++) {
        task_t *child = &proc_table[i];
        int cstate = __atomic_load_n(&child->state, __ATOMIC_ACQUIRE);
        if (cstate == PROC_UNUSED) continue;
        if (child->ppid != t->pid) continue;
        if (pid > 0 && child->pid != pid) continue;
        if (pid == 0 && child->pgid != t->pgid) continue;
        if (pid < -1 && child->pgid != (-pid)) continue;

        found = 1;
        if (cstate == PROC_ZOMBIE) {
            int code = __atomic_load_n(&child->exit_code, __ATOMIC_ACQUIRE);
            if (status) {
                if (code >= 0)
                    *status = (code & 0xFF) << 8;
                else
                    *status = (-code) & 0xFF;
            }
            int child_pid = child->pid;
            if (wait_diag_count < 128) {
                wait_diag_count++;
                kdebug("[WAITDBG] reap cur=%d child=%d status=0x%x\n",
                       t ? t->pid : -1, child_pid, status ? *status : code);
            }
            proc_free_pid(child_pid);
            return child_pid;
        }
    }

    if (!found) {
        if (wait_diag_count < 128) {
            wait_diag_count++;
            kdebug("[WAITDBG] no-child cur=%d pid=%d\n",
                   t ? t->pid : -1, pid);
        }
        return -ECHILD;
    }

    if (options & WNOHANG) {
        if (wait_diag_count < 128) {
            wait_diag_count++;
            kdebug("[WAITDBG] nohang cur=%d pid=%d\n",
                   t ? t->pid : -1, pid);
        }
        return 0;
    }

    /*
     * Avoid the classic lost-wakeup race:
     * a child can exit after the scan above but before we actually block.
     * Mark ourselves blocked first, then rescan once before scheduling.
     */
    t->state = PROC_BLOCKED;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (wait_diag_count < 128) {
        wait_diag_count++;
        kdebug("[WAITDBG] block cur=%d pid=%d\n",
               t ? t->pid : -1, pid);
    }

    for (int i = 0; i < MAX_PROCS; i++) {
        task_t *child = &proc_table[i];
        int cstate = __atomic_load_n(&child->state, __ATOMIC_ACQUIRE);
        if (cstate == PROC_UNUSED) continue;
        if (child->ppid != t->pid) continue;
        if (pid > 0 && child->pid != pid) continue;
        if (pid == 0 && child->pgid != t->pgid) continue;
        if (pid < -1 && child->pgid != (-pid)) continue;

        if (cstate == PROC_ZOMBIE) {
            int code = __atomic_load_n(&child->exit_code, __ATOMIC_ACQUIRE);
            t->state = PROC_RUNNING;
            if (status) {
                if (code >= 0)
                    *status = (code & 0xFF) << 8;
                else
                    *status = (-code) & 0xFF;
            }
            int child_pid = child->pid;
            if (wait_diag_count < 128) {
                wait_diag_count++;
                kdebug("[WAITDBG] reap-race cur=%d child=%d status=0x%x\n",
                       t ? t->pid : -1, child_pid, status ? *status : code);
            }
            proc_free_pid(child_pid);
            return child_pid;
        }
    }

    if (t->state == PROC_BLOCKED)
        sched();
    t->state = PROC_RUNNING;
    if (wait_diag_count < 128) {
        wait_diag_count++;
        kdebug("[WAITDBG] wake cur=%d pid=%d\n",
               t ? t->pid : -1, pid);
    }
    goto retry;
}

int proc_wait(int *status)
{
    return proc_wait4(-1, status, 0);
}
