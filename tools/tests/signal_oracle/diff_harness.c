/*
 * C-vs-Rust differential harness for the signal decision core
 * (kernel/proc/signal_core.rs).
 *
 * The C side below is an independent transcription of the frozen C reference
 * behaviour (kernel/proc/signal.c and the Native/Linux ABI checks as of the
 * migration baseline); the Rust side is the production a20_signal_* ABI,
 * compiled for the host. The harness sweeps the scalar input domain and
 * requires byte-identical results, so neither side can drift silently.
 *
 * Build/run is driven by tools/host-tests/test-signal-core-host.sh; this file is not part
 * of the kernel build.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

/* ---- production Rust ABI under test ---- */
int      a20_signal_core_dump_default(int sig);
int      a20_signal_default_terminate(int sig);
int      a20_signal_default_stop(int sig);
int      a20_signal_default_ignore(int sig);
int      a20_signal_wait_status_dumped(int sig, int dumped);
int      a20_signal_fatal_exit_code_dumped(int sig, int dumped);
uint64_t a20_signal_mask_apply(int how, uint64_t old, uint64_t set);
uint64_t a20_signal_mask_strip_unblockable(uint64_t mask);
uint64_t a20_signal_deliverable_mask(uint64_t pending, uint64_t thread_pending, uint64_t blocked);
uint64_t a20_signal_pending_union(uint64_t pending, uint64_t thread_pending);
uint64_t a20_signal_pending_blocked(uint64_t pending, uint64_t thread_pending, uint64_t blocked);
int      a20_signal_lowest(uint64_t mask);
int      a20_signal_action_deliverable(uint64_t blocked_mask, uint64_t handler, int sig);
int      a20_signal_action_fatal(uint64_t blocked_mask, uint64_t handler, int sig);
int      a20_signal_action_user_handler_available(uint64_t handler, int blocked);
int      a20_signal_queue_discard(int sig, uint64_t handler);
int      a20_signal_queue_update_info(int thread_directed, int thread_bit_set);
int      a20_signal_queue_info_has(int thread_directed, int thread_bit_set, int info_present);
int      a20_signal_peek_scope(int thread_bit, int thread_info_obj, int thread_has_info, int process_has_info);
int      a20_signal_clear_scope(int thread_bit);
uint64_t a20_signal_generation_cancel_mask(int sig);
int      a20_signal_restart_skips(uint64_t handler, int sig);
int      a20_signal_restart_vetoes(uint64_t handler, int flags);
int      a20_signal_sigchld_auto_reap(uint64_t handler, int flags);
int      a20_signal_sigchld_no_cldstop(int flags);
int      a20_signal_continue_pending(uint64_t pending_union);
uint64_t a20_signal_temporary_mask(uint64_t new_mask);
int      a20_signal_sigsuspend_restore_active(int sigsuspend_active, int sig_handling);
uint64_t a20_signal_exec_action_reset(uint64_t handler);
int      a20_signal_action_valid(int sig, uint64_t sigsetsize, uint64_t arch_sigset_size);
int      a20_signal_procmask_how_valid(int how);
int      a20_signal_kill_permission(int cap_kill, uint32_t self_uid, uint32_t self_euid, uint32_t target_uid, uint32_t target_suid);
int      a20_signal_sigaltstack_valid(uint32_t flags, uint64_t size, uint64_t min_size);
int      a20_signal_native_kill_valid(uint64_t mode, int64_t sig, int nsig);
int      a20_signal_native_mode_valid(uint64_t mode, uint64_t allowed);
int      a20_signal_native_control_valid(uint32_t operation, uint32_t flags, uint32_t op_check, uint32_t op_mask, uint64_t check_mask, uint64_t mask_mask);
uint64_t a20_signal_native_thread_check(uint64_t pending_ht, uint64_t pending_thread, uint64_t blocked, uint32_t mode, uint64_t selected, uint64_t *clear_thread_out, uint64_t *clear_ht_out);
uint64_t a20_signal_native_thread_mask(uint64_t new_mask);
uint64_t a20_signal_native_ht_mask(uint64_t new_mask);
int      a20_signal_fd_ready(uint64_t pending_union, uint64_t mask);
int      a20_signal_fd_read_valid(uint64_t count, uint64_t record_size);
uint64_t a20_signal_fd_read_records(uint64_t count, uint64_t record_size);

/* ---- transaction ABI under test ---- */
typedef struct a20_signal_queue_state {
    uint64_t *process_pending;
    uint8_t  *process_has_info;
    uint8_t  *process_info;
    uint64_t *thread_pending;
    uint8_t  *thread_has_info;
    uint8_t  *thread_info;
} a20_signal_queue_state_t;
typedef struct a20_signal_queue_result {
    int discard, fatal, deliverable, sigwait_match;
} a20_signal_queue_result_t;
typedef struct a20_signal_task_slots {
    int *sig_handling;
    int *sigsuspend_active;
    int *sigwait_active;
    uint64_t *sigwait_mask;
} a20_signal_task_slots_t;
enum {
    A20_DELIVERY_NONE = 0, A20_DELIVERY_DROP_IGNORED = 1, A20_DELIVERY_STOP = 2,
    A20_DELIVERY_FATAL = 3, A20_DELIVERY_HANDLER = 4, A20_DELIVERY_PTRACE_STOP = 5
};
typedef struct a20_signal_delivery_txn {
    uint64_t blocked;
    int sigsuspend_active;
    uint64_t sigsuspend_old_blocked;
    int user, traced, ptrace_deliver_sig;
    int sig, outcome;
    uint64_t handler;
    int flags;
    uint64_t sa_mask;
    uint64_t old_blocked;
    int sig_handling, reset_handler, has_info;
} a20_signal_delivery_txn_t;
int  a20_signal_peek_locked(a20_signal_queue_state_t *st, int sig, uint8_t *out, size_t out_size);
int  a20_signal_consume_locked(a20_signal_queue_state_t *st, int sig, uint8_t *out, size_t out_size);
int  a20_signal_queue_locked(a20_signal_queue_state_t *st, int signum, const uint8_t *info,
                             size_t info_size, uint64_t handler, uint64_t blocked,
                             int thread_directed, int sigwait_active, uint64_t sigwait_mask,
                             a20_signal_queue_result_t *result);
int  a20_signal_exec_reset_locked(a20_signal_queue_state_t *st, uint8_t *actions,
                                  size_t actions_count, size_t action_stride,
                                  size_t handler_off, size_t handler_size,
                                  size_t mask_off, size_t flags_off,
                                  a20_signal_task_slots_t *slots);
int  a20_signal_delivery_select_locked(a20_signal_queue_state_t *st, const uint8_t *actions,
                                       size_t actions_count, size_t action_stride,
                                       size_t handler_off, size_t handler_size,
                                       size_t mask_off, size_t flags_off,
                                       a20_signal_delivery_txn_t *txn,
                                       uint8_t *info_out, size_t info_out_size);
int64_t a20_signal_fd_read_locked(a20_signal_queue_state_t *st, uint64_t mask,
                                  uint8_t *records, size_t capacity, size_t record_size,
                                  size_t sifields_off);
int  a20_signal_take_matching_locked(a20_signal_queue_state_t *st, uint64_t mask,
                                     uint8_t *out, size_t out_size, int *has_info_out);
uint64_t a20_signal_native_thread_check_locked(uint64_t *pending_ht, uint64_t *pending_thread,
                                               uint64_t blocked, uint32_t mode, uint64_t selected);
uint64_t a20_signal_native_ht_take_locked(uint64_t *pending, uint64_t blocked);
void a20_signal_native_thread_pend_locked(uint64_t *pending_thread, int sig);
void a20_signal_native_ht_pend_locked(uint64_t *pending_ht, int sig);

/* ---- C reference transcription (frozen baseline behaviour) ---- */
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGKILL 9
#define SIGURG 23
#define SIGWINCH 28
#define SIGQUIT 3
#define SIGILL 4
#define SIGABRT 6
#define SIGBUS 7
#define SIGFPE 8
#define SIGSEGV 11
#define SIGSYS 31
#define SS_DISABLE 2
#define SA_NOCLDSTOP 1
#define SA_NOCLDWAIT 2
#define SA_RESTART 0x10000000

static uint64_t kb(int sig) { return (sig > 0 && sig < 64) ? (1ULL << sig) : 0; }

static int c_core_dump_default(int sig) {
    switch (sig) {
        case SIGQUIT: case SIGILL: case SIGABRT: case SIGBUS:
        case SIGFPE: case SIGSEGV: case SIGSYS: return 1;
        default: return 0;
    }
}
static int c_default_terminate(int sig) {
    switch (sig) {
        case SIGCHLD: case SIGURG: case SIGWINCH: case SIGSTOP:
        case SIGTSTP: case SIGTTIN: case SIGTTOU: case SIGCONT: return 0;
        default: return 1;
    }
}
static int c_default_stop(int sig) {
    switch (sig) {
        case SIGSTOP: case SIGTSTP: case SIGTTIN: case SIGTTOU: return 1;
        default: return 0;
    }
}
static int c_default_ignore(int sig) {
    switch (sig) {
        case SIGCHLD: case SIGURG: case SIGWINCH: case SIGCONT: return 1;
        default: return 0;
    }
}
static int c_wait_status_dumped(int sig, int dumped) {
    int status = sig & 0x7f;
    if (dumped) status |= 0x80;
    return status;
}
static uint64_t c_mask_apply(int how, uint64_t old, uint64_t set) {
    switch (how) {
        case 0: return old | set;
        case 1: return old & ~set;
        case 2: return set;
        default: return old;
    }
}
static uint64_t c_strip_unblockable(uint64_t mask) {
    return mask & ~(kb(SIGKILL) | kb(SIGSTOP));
}
static uint64_t c_deliverable_mask(uint64_t p, uint64_t tp, uint64_t b) {
    return (p | tp) & ~b;
}
static int c_lowest(uint64_t mask) {
    for (int s = 1; s < 64; s++) if (mask & (1ULL << s)) return s;
    return 0;
}
static int c_action_deliverable(uint64_t blocked, uint64_t handler, int sig) {
    if (blocked & kb(sig)) return 0;
    if (handler == 1) return 0;
    if (handler == 0 && c_default_ignore(sig)) return 0;
    return 1;
}
static int c_action_fatal(uint64_t blocked, uint64_t handler, int sig) {
    return c_action_deliverable(blocked, handler, sig) && handler == 0 &&
           c_default_terminate(sig);
}
static int c_queue_discard(int sig, uint64_t handler) {
    if (sig == SIGCONT) return 0;
    if (handler == 1) return 1;
    if (handler == 0 && c_default_ignore(sig)) return 1;
    return 0;
}
static int c_peek_scope(int tb, int tio, int thas, int phas) {
    if (tb) { if (!tio || !thas) return -2; return 0; }
    return phas ? 1 : -2;
}
static uint64_t c_gen_cancel(int sig) {
    if (sig == SIGCONT) return kb(SIGSTOP) | kb(SIGTSTP) | kb(SIGTTIN) | kb(SIGTTOU);
    if (c_default_stop(sig)) return kb(SIGCONT);
    return 0;
}
static int c_restart_skips(uint64_t handler, int sig) {
    if (handler == 1) return 1;
    if (handler == 0 && c_default_ignore(sig)) return 1;
    return 0;
}
static int c_restart_vetoes(uint64_t handler, int flags) {
    return (handler != 0 && handler != 1 && !(flags & SA_RESTART));
}
static int c_sigchld_auto_reap(uint64_t handler, int flags) {
    return handler == 1 || (flags & SA_NOCLDWAIT);
}
static int c_exec_action_reset(uint64_t handler) {
    return (handler != 1 && handler != 0) ? 0 : (int)handler;
}
static uint64_t c_native_thread_check(uint64_t ph, uint64_t pt, uint64_t b, uint32_t mode,
                                      uint64_t sel, uint64_t *ct, uint64_t *ch) {
    uint64_t pending = ph | pt;
    uint64_t deliver = pending & ~b;
    if (mode & 4) {
        deliver = pending & sel;
        if (!(mode & 2)) deliver &= 0ULL - deliver;
    }
    *ct = *ch = 0;
    if (!(mode & 2)) {
        uint64_t local = deliver & pt;
        *ct = local;
        *ch = deliver & ~local;
    }
    return deliver;
}

/* ---- sweep ---- */
static unsigned long checks, failures;
static void eq64(const char *name, uint64_t a, uint64_t b, const char *label) {
    checks++;
    if (a != b) {
        failures++;
        if (failures <= 40)
            printf("MISMATCH %s %s: rust=%llu c=%llu\n", name, label,
                   (unsigned long long)a, (unsigned long long)b);
    }
}
static void eqi(const char *name, int a, int b, const char *label) {
    eq64(name, (uint64_t)(int64_t)a, (uint64_t)(int64_t)b, label);
}

/* ---- Transactional differential sweep ----
 *
 * The C model below is an independent transcription of the frozen queue /
 * consume / delivery / signalfd / Native-checkpoint transaction behaviour.
 * Both sides start from identical state and execute the same pseudo-random
 * op stream; after every op the full pending/siginfo state and every result
 * field are compared, so a transaction drift cannot pass silently.
 */
#define TM_NSIG 64
#define TM_INFO 128

typedef struct {
    uint64_t pending;
    uint8_t has_info[TM_NSIG];
    uint8_t info[TM_NSIG][TM_INFO];
} tm_scope_t;

typedef struct {
    uint64_t handler, mask;
    uint32_t flags;
} tm_action_t;

typedef struct {
    tm_scope_t process;
    tm_scope_t thread;
    tm_action_t actions[TM_NSIG];
    uint64_t blocked;
    int sig_handling, sigsuspend_active, sigwait_active;
    uint64_t sigsuspend_old_blocked, sigwait_mask;
    int ptrace_deliver_sig, traced;
} tm_state_t;

/* sigaction_t layout on the host (all pointers 8 bytes here). */
typedef struct { uint64_t handler; uint64_t mask; int flags; } tm_c_sigaction_t;

static void tm_init(tm_state_t *m) {
    memset(m, 0, sizeof(*m));
    for (int i = 0; i < TM_NSIG; i++)
        m->actions[i].handler = (i % 7 == 0) ? 1 : 0; /* sprinkle SIG_IGN */
}

/* --- model transactions (C transcription of the frozen reference) --- */

static void tm_clear_one(tm_state_t *m, int sig) {
    uint64_t b = kb(sig);
    if (m->thread.pending & b) {
        m->thread.pending &= ~b;
        m->thread.has_info[sig] = 0;
    } else {
        m->process.pending &= ~b;
        m->process.has_info[sig] = 0;
    }
}

static void tm_clear_both(tm_state_t *m, int sig) {
    uint64_t b = kb(sig);
    if (m->thread.pending & b) {
        m->thread.pending &= ~b;
        m->thread.has_info[sig] = 0;
    }
    if (m->process.pending & b) {
        m->process.pending &= ~b;
        m->process.has_info[sig] = 0;
    }
}

static int tm_peek(const tm_state_t *m, int sig, uint8_t *out, size_t out_size) {
    uint64_t b = kb(sig);
    const tm_scope_t *scope;
    if (m->thread.pending & b) scope = &m->thread;
    else scope = &m->process;
    if (!scope->has_info[sig]) return -2;
    if (out && out_size) {
        size_t n = out_size < TM_INFO ? out_size : TM_INFO;
        memcpy(out, scope->info[sig], n);
    }
    return 0;
}

static int tm_consume(tm_state_t *m, int sig, uint8_t *out, size_t out_size) {
    int ret = tm_peek(m, sig, out, out_size);
    tm_clear_one(m, sig);
    return ret;
}

static void tm_queue(tm_state_t *m, int signum, const uint8_t *info, int info_present,
                     int thread_directed, int *discard, int *fatal,
                     int *deliverable, int *sigwait_match) {
    uint64_t cancel = c_gen_cancel(signum);
    for (int s = 1; s < TM_NSIG; s++)
        if (cancel & (uint64_t)1 << s) tm_clear_both(m, s);

    uint64_t handler = m->actions[signum].handler;
    if (signum != SIGCONT &&
        (handler == 1 || (handler == 0 && c_default_ignore(signum)))) {
        *discard = 1; *fatal = 0; *deliverable = 0; *sigwait_match = 0;
        return;
    }
    *discard = 0;
    uint64_t b = kb(signum);
    int thread_bit_set = (m->thread.pending & b) != 0;
    if (thread_directed) {
        if (!thread_bit_set) {
            m->thread.has_info[signum] = info_present;
            if (info_present)
                memcpy(m->thread.info[signum], info, TM_INFO);
        }
        m->thread.pending |= b;
    } else {
        if (info_present)
            memcpy(m->process.info[signum], info, TM_INFO);
        m->process.has_info[signum] = info_present;
        m->process.pending |= b;
    }
    *fatal = c_action_fatal(m->blocked, handler, signum);
    *deliverable = c_action_deliverable(m->blocked, handler, signum);
    *sigwait_match = m->sigwait_active && (m->sigwait_mask & b);
}

static void tm_select(tm_state_t *m, int user, a20_signal_delivery_txn_t *txn,
                      uint8_t *info_out, size_t info_out_size) {
    uint64_t deliverable = (m->process.pending | m->thread.pending) & ~m->blocked;
    int sig = c_lowest(deliverable);
    txn->sig = sig;
    txn->outcome = A20_DELIVERY_NONE;
    txn->handler = 0; txn->flags = 0; txn->sa_mask = 0; txn->old_blocked = 0;
    txn->sig_handling = 0; txn->reset_handler = 0; txn->has_info = 0;
    if (!sig) return;
    uint64_t b = kb(sig);
    uint64_t handler = m->actions[sig].handler;
    uint32_t flags = m->actions[sig].flags;
    uint64_t action_blocked = user ? m->blocked : 0;
    if (!c_action_deliverable(action_blocked, handler, sig)) {
        tm_clear_one(m, sig);
        if (user && m->sigsuspend_active) {
            m->blocked = m->sigsuspend_old_blocked;
            m->sigsuspend_active = 0;
            txn->blocked = m->blocked;
        }
        txn->outcome = A20_DELIVERY_DROP_IGNORED;
        return;
    }
    if (user && m->traced && sig != SIGKILL) {
        if (m->ptrace_deliver_sig != sig) {
            tm_clear_one(m, sig);
            txn->outcome = A20_DELIVERY_PTRACE_STOP;
            return;
        }
        m->ptrace_deliver_sig = 0;
        txn->ptrace_deliver_sig = 0;
    }
    if (handler == 0) {
        tm_clear_one(m, sig);
        if (c_default_stop(sig)) {
            if (user && m->sigsuspend_active) {
                m->blocked = m->sigsuspend_old_blocked;
                m->sigsuspend_active = 0;
                txn->blocked = m->blocked;
            }
            txn->outcome = A20_DELIVERY_STOP;
        } else {
            txn->outcome = A20_DELIVERY_FATAL;
        }
        return;
    }
    txn->has_info = tm_peek(m, sig, info_out, info_out_size) == 0;
    tm_clear_one(m, sig);
    txn->handler = handler;
    txn->flags = (int)flags;
    txn->sa_mask = m->actions[sig].mask;
    if (user) {
        if (flags & 0x80000000u) {
            txn->reset_handler = 1;
            m->actions[sig].handler = 0;
        }
        txn->old_blocked = m->sigsuspend_active ? m->sigsuspend_old_blocked : m->blocked;
        m->blocked |= m->actions[sig].mask;
        if (!(flags & 0x40000000u)) m->blocked |= b;
        txn->blocked = m->blocked;
        m->sigsuspend_active = 0;
        txn->sigsuspend_active = 0;
        m->sig_handling = sig;
        txn->sig_handling = sig;
    }
    txn->outcome = A20_DELIVERY_HANDLER;
}

static void tm_exec_reset(tm_state_t *m) {
    for (int i = 1; i < TM_NSIG; i++) {
        m->actions[i].handler = c_exec_action_reset(m->actions[i].handler);
        m->actions[i].mask = 0;
        m->actions[i].flags = 0;
    }
    memset(&m->process, 0, sizeof(m->process));
    memset(&m->thread, 0, sizeof(m->thread));
    m->sig_handling = 0;
    m->sigsuspend_active = 0;
    m->sigwait_active = 0;
    m->sigwait_mask = 0;
}

/* --- Rust-side state wrapper --- */

typedef struct {
    tm_state_t *m;
    a20_signal_queue_state_t st;
    tm_c_sigaction_t c_actions[TM_NSIG];
} rust_side_t;

static void rust_sync_actions_in(rust_side_t *r) {
    for (int i = 0; i < TM_NSIG; i++) {
        r->c_actions[i].handler = r->m->actions[i].handler;
        r->c_actions[i].mask = r->m->actions[i].mask;
        r->c_actions[i].flags = (int)r->m->actions[i].flags;
    }
}

static void rust_sync_actions_out(rust_side_t *r) {
    for (int i = 0; i < TM_NSIG; i++) {
        r->m->actions[i].handler = r->c_actions[i].handler;
        r->m->actions[i].mask = r->c_actions[i].mask;
        r->m->actions[i].flags = (uint32_t)r->c_actions[i].flags;
    }
}

static void rust_bind(rust_side_t *r, tm_state_t *m) {
    memset(r, 0, sizeof(*r));
    r->m = m;
    rust_sync_actions_in(r);
    r->st = (a20_signal_queue_state_t){
        .process_pending = &m->process.pending,
        .process_has_info = m->process.has_info,
        .process_info = &m->process.info[0][0],
        .thread_pending = &m->thread.pending,
        .thread_has_info = m->thread.has_info,
        .thread_info = &m->thread.info[0][0],
    };
}

/* --- comparators --- */

static void cmp_scope(const char *what, const tm_scope_t *a, const tm_scope_t *b) {
    eq64(what, a->pending, b->pending, "pending");
    for (int s = 0; s < TM_NSIG; s++) {
        eq64(what, a->has_info[s], b->has_info[s], "has_info");
        if (memcmp(a->info[s], b->info[s], TM_INFO) != 0) {
            checks++;
            failures++;
            if (failures <= 40)
                printf("MISMATCH %s info bytes sig=%d\n", what, s);
        } else {
            checks++;
        }
    }
}

static void cmp_state(const char *what, const tm_state_t *a, const tm_state_t *b) {
    cmp_scope(what, &a->process, &b->process);
    cmp_scope(what, &a->thread, &b->thread);
    eq64(what, a->blocked, b->blocked, "blocked");
    eq64(what, a->sig_handling, b->sig_handling, "sig_handling");
    eq64(what, a->sigsuspend_active, b->sigsuspend_active, "sigsuspend_active");
    eq64(what, a->sigwait_active, b->sigwait_active, "sigwait_active");
    eq64(what, a->sigwait_mask, b->sigwait_mask, "sigwait_mask");
    eq64(what, a->ptrace_deliver_sig, b->ptrace_deliver_sig, "ptrace_deliver_sig");
    for (int i = 0; i < TM_NSIG; i++) {
        char lbl[24];
        snprintf(lbl, sizeof(lbl), "act=%d", i);
        eq64(what, a->actions[i].handler, b->actions[i].handler, lbl);
        eq64(what, a->actions[i].mask, b->actions[i].mask, lbl);
        eq64(what, a->actions[i].flags, b->actions[i].flags, lbl);
    }
}

static uint64_t rng_state;
static uint64_t rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

/* Shared input pools (also used by main's scalar sweep). */
static const uint64_t masks[] = {0, 1, 2, 0x400, 0x1000, 0x40000, 0x80000000ULL,
                                 0xffffffffffffffffULL, 0x800000000ULL, 0xdeadbeefULL};

static void run_txn_sweep(void) {
    static const uint64_t flagpool[] = {0, 1, 2, 4, 0x08000000, 0x10000000,
                                        0x40000000, 0x80000000, 0xc0000000,
                                        0xffffffff};
    static const uint64_t handlerpool[] = {0, 1, 0x4000, 0xdeadbeefcafeULL};

    for (unsigned seed = 1; seed <= 120; seed++) {
        tm_state_t ma, mb;
        rust_side_t r;
        tm_init(&ma);
        mb = ma;
        rust_bind(&r, &mb);
        rng_state = seed * 0x9e3779b97f4a7c15ULL + 1;

        unsigned ops = 180;
        for (unsigned op = 0; op < ops; op++) {
            unsigned kind = rng() % 12;
            char lbl[48];
            snprintf(lbl, sizeof(lbl), "seed=%u op=%u kind=%u", seed, op, kind);
            if (kind <= 3) {
                /* queue: sig 1..40, optional info, scope, sometimes an
                 * action change first so discard/coalesce paths vary. */
                int sig = 1 + (int)(rng() % 40);
                int with_info = rng() & 1;
                int thread_directed = rng() & 1;
                if (rng() % 4 == 0) {
                    uint64_t h = handlerpool[rng() % 4];
                    uint32_t f = (uint32_t)flagpool[rng() % 10];
                    uint64_t mk = (rng() % 5 == 0) ? 0 : rng();
                    ma.actions[sig].handler = h;
                    ma.actions[sig].flags = f;
                    ma.actions[sig].mask = mk;
                    mb.actions[sig].handler = h;
                    mb.actions[sig].flags = f;
                    mb.actions[sig].mask = mk;
                    rust_sync_actions_in(&r);
                }
                uint8_t info[TM_INFO];
                memset(info, 0, sizeof(info));
                *(int *)info = sig;
                *(int *)(info + 4) = (int)rng();
                *(int *)(info + 8) = (int)rng();
                *(uint32_t *)(info + 12) = (uint32_t)rng();
                *(uint32_t *)(info + 16) = (uint32_t)rng();
                int swa = rng() & 1;
                ma.sigwait_active = swa;
                ma.sigwait_mask = rng();
                mb.sigwait_active = swa;
                mb.sigwait_mask = ma.sigwait_mask;
                int d1, f1, v1, w1;
                tm_queue(&ma, sig, info, with_info, thread_directed,
                         &d1, &f1, &v1, &w1);
                a20_signal_queue_result_t rr;
                a20_signal_queue_locked(&r.st, sig, with_info ? info : NULL,
                                        with_info ? sizeof(info) : 0,
                                        mb.actions[sig].handler, mb.blocked,
                                        thread_directed, mb.sigwait_active,
                                        mb.sigwait_mask, &rr);
                eqi("txn_queue_discard", rr.discard, d1, lbl);
                eqi("txn_queue_fatal", rr.fatal, f1, lbl);
                eqi("txn_queue_deliverable", rr.deliverable, v1, lbl);
                eqi("txn_queue_sigwait", rr.sigwait_match, w1, lbl);
            } else if (kind == 4 || kind == 5) {
                int sig = 1 + (int)(rng() % 45);
                uint8_t o1[TM_INFO], o2[TM_INFO];
                memset(o1, 0xa5, sizeof(o1));
                memset(o2, 0xa5, sizeof(o2));
                int ret1 = tm_consume(&ma, sig, o1, sizeof(o1));
                int ret2 = a20_signal_consume_locked(&r.st, sig, o2, sizeof(o2));
                eqi("txn_consume_ret", ret2, ret1, lbl);
                if (memcmp(o1, o2, TM_INFO) != 0) {
                    checks++; failures++;
                    if (failures <= 40)
                        printf("MISMATCH txn_consume_bytes sig=%d %s\n", sig, lbl);
                } else checks++;
            } else if (kind == 6) {
                uint64_t mask = rng();
                uint8_t o1[TM_INFO], o2[TM_INFO];
                memset(o1, 0, sizeof(o1));
                memset(o2, 0, sizeof(o2));
                int h1 = 0, h2 = -1;
                int s1 = 0, s2 = 0;
                uint64_t cand = (ma.process.pending | ma.thread.pending) & mask;
                if (cand) {
                    s1 = c_lowest(cand);
                    h1 = tm_consume(&ma, s1, o1, sizeof(o1)) == 0;
                }
                s2 = a20_signal_take_matching_locked(&r.st, mask, o2, sizeof(o2), &h2);
                eqi("txn_take_sig", s2, s1, lbl);
                eqi("txn_take_hasinfo", h2, h1, lbl);
                if (memcmp(o1, o2, TM_INFO) != 0) {
                    checks++; failures++;
                    if (failures <= 40)
                        printf("MISMATCH txn_take_bytes %s\n", lbl);
                } else checks++;
            } else if (kind == 7) {
                uint64_t mask = rng();
                size_t capacity = 1 + rng() % 6;
                uint8_t rec1[6 * 128], rec2[6 * 128];
                memset(rec1, 0, sizeof(rec1));
                memset(rec2, 0, sizeof(rec2));
                int64_t n1 = 0;
                for (size_t i = 0; i < capacity; i++) {
                    uint64_t cand = (ma.process.pending | ma.thread.pending) & mask & ~1ULL;
                    int sig = c_lowest(cand);
                    if (!sig) break;
                    uint8_t info[TM_INFO];
                    int has = tm_consume(&ma, sig, info, sizeof(info)) == 0;
                    memset(rec1 + i * 128, 0, 128);
                    *(uint32_t *)(rec1 + i * 128) = (uint32_t)sig;
                    if (has) {
                        *(int32_t *)(rec1 + i * 128 + 4) = *(int32_t *)(info + 4);
                        *(int32_t *)(rec1 + i * 128 + 8) = *(int32_t *)(info + 8);
                        /* 64-bit arch: _sifields at offset 16. */
                        *(uint32_t *)(rec1 + i * 128 + 12) = *(uint32_t *)(info + 16);
                        *(uint32_t *)(rec1 + i * 128 + 16) = *(uint32_t *)(info + 20);
                    }
                    n1++;
                }
                int64_t n2 = a20_signal_fd_read_locked(&r.st, mask, rec2, capacity, 128, 16);
                eq64("txn_fd_count", (uint64_t)n2, (uint64_t)n1, lbl);
                if (memcmp(rec1, rec2, sizeof(rec1)) != 0) {
                    checks++; failures++;
                    if (failures <= 40)
                        printf("MISMATCH txn_fd_records %s\n", lbl);
                } else checks++;
            } else if (kind == 8 || kind == 9) {
                int user = kind == 9;
                uint8_t i1[TM_INFO], i2[TM_INFO];
                memset(i1, 0, sizeof(i1));
                memset(i2, 0, sizeof(i2));
                a20_signal_delivery_txn_t t1, t2;
                memset(&t1, 0, sizeof(t1));
                memset(&t2, 0, sizeof(t2));
                /* Both sides start from the identical pre-state; blocked /
                 * sigsuspend_active / ptrace_deliver_sig are in-out fields,
                 * so each side gets its own copy of the inputs. */
                t1.blocked = t2.blocked = ma.blocked;
                t1.sigsuspend_active = t2.sigsuspend_active = ma.sigsuspend_active;
                t1.sigsuspend_old_blocked = t2.sigsuspend_old_blocked =
                    ma.sigsuspend_old_blocked;
                t1.user = t2.user = user;
                t1.traced = t2.traced = ma.traced;
                t1.ptrace_deliver_sig = t2.ptrace_deliver_sig = ma.ptrace_deliver_sig;
                tm_select(&ma, user, &t1, i1, sizeof(i1));
                rust_sync_actions_in(&r);
                a20_signal_delivery_select_locked(&r.st, (const uint8_t *)r.c_actions,
                    TM_NSIG, sizeof(r.c_actions[0]),
                    offsetof(tm_c_sigaction_t, handler), sizeof(uint64_t),
                    offsetof(tm_c_sigaction_t, mask), offsetof(tm_c_sigaction_t, flags),
                    &t2, i2, sizeof(i2));
                rust_sync_actions_out(&r);
                /* Apply the adapter stores exactly like signal.c does
                 * (sig_handling is stored on the user handler path only,
                 * matching the C reference's kernel-loop behaviour). */
                mb.blocked = t2.blocked;
                mb.sigsuspend_active = t2.sigsuspend_active;
                mb.ptrace_deliver_sig = t2.ptrace_deliver_sig;
                if (t2.user && t2.outcome == A20_DELIVERY_HANDLER) {
                    mb.sig_handling = t2.sig_handling;
                    if (t2.reset_handler)
                        mb.actions[t2.sig].handler = 0;
                }
                eqi("txn_sel_sig", t2.sig, t1.sig, lbl);
                eqi("txn_sel_outcome", t2.outcome, t1.outcome, lbl);
                eqi("txn_sel_handler", t2.handler == t1.handler, 1, lbl);
                eqi("txn_sel_flags", t2.flags, t1.flags, lbl);
                eq64("txn_sel_samask", t2.sa_mask, t1.sa_mask, lbl);
                eq64("txn_sel_oldblocked", t2.old_blocked, t1.old_blocked, lbl);
                eqi("txn_sel_sighandling", t2.sig_handling, t1.sig_handling, lbl);
                eqi("txn_sel_reset", t2.reset_handler, t1.reset_handler, lbl);
                eqi("txn_sel_hasinfo", t2.has_info, t1.has_info, lbl);
                eq64("txn_sel_blocked", t2.blocked, t1.blocked, lbl);
                eqi("txn_sel_ptrace", t2.ptrace_deliver_sig, t1.ptrace_deliver_sig, lbl);
                if (memcmp(i1, i2, TM_INFO) != 0) {
                    checks++; failures++;
                    if (failures <= 40)
                        printf("MISMATCH txn_sel_info %s\n", lbl);
                } else checks++;
            } else if (kind == 10) {
                int sig = 1 + (int)(rng() % 40);
                uint64_t h = handlerpool[rng() % 4];
                uint32_t f = (uint32_t)flagpool[rng() % 10];
                uint64_t mk = (rng() % 5 == 0) ? 0 : rng();
                ma.actions[sig].handler = h;
                ma.actions[sig].flags = f;
                ma.actions[sig].mask = mk;
                mb.actions[sig].handler = h;
                mb.actions[sig].flags = f;
                mb.actions[sig].mask = mk;
                rust_sync_actions_in(&r);
                /* Randomize the ptrace/sigsuspend inputs both sides share. */
                ma.traced = (int)(rng() & 1);
                mb.traced = ma.traced;
                ma.sigsuspend_active = (int)(rng() & 1);
                mb.sigsuspend_active = ma.sigsuspend_active;
                ma.sigsuspend_old_blocked = rng();
                mb.sigsuspend_old_blocked = ma.sigsuspend_old_blocked;
            } else {
                int sh1 = 0, ss1 = 0, sw1 = 0;
                uint64_t swm1 = 0;
                a20_signal_task_slots_t slots = {
                    .sig_handling = &sh1,
                    .sigsuspend_active = &ss1,
                    .sigwait_active = &sw1,
                    .sigwait_mask = &swm1,
                };
                tm_exec_reset(&ma);
                rust_sync_actions_in(&r);
                a20_signal_exec_reset_locked(&r.st, (uint8_t *)r.c_actions,
                    TM_NSIG, sizeof(r.c_actions[0]),
                    offsetof(tm_c_sigaction_t, handler), sizeof(uint64_t),
                    offsetof(tm_c_sigaction_t, mask), offsetof(tm_c_sigaction_t, flags),
                    &slots);
                rust_sync_actions_out(&r);
                mb.sig_handling = sh1;
                mb.sigsuspend_active = ss1;
                mb.sigwait_active = sw1;
                mb.sigwait_mask = swm1;
            }
            cmp_state("txn_state", &ma, &mb);
        }
    }

    /* Native checkpoint consuming transactions against the model. */
    for (unsigned a = 0; a < sizeof(masks) / sizeof(masks[0]); a++)
        for (unsigned b = 0; b < sizeof(masks) / sizeof(masks[0]); b++)
            for (uint32_t mode = 0; mode < 8; mode++) {
                uint64_t ph1 = masks[a], pt1 = masks[b];
                uint64_t ct = 0, ch = 0;
                uint64_t d1 = c_native_thread_check(ph1, pt1, masks[(a + b) % 10],
                                                    mode, masks[b], &ct, &ch);
                ph1 &= ~ch;
                pt1 &= ~ct;
                uint64_t ph2 = masks[a], pt2 = masks[b];
                uint64_t d2 = a20_signal_native_thread_check_locked(
                    &ph2, &pt2, masks[(a + b) % 10], mode, masks[b]);
                eq64("txn_ntc_deliver", d2, d1, "ntc");
                eq64("txn_ntc_ph", ph2, ph1, "ntc");
                eq64("txn_ntc_pt", pt2, pt1, "ntc");
            }
    for (unsigned a = 0; a < sizeof(masks) / sizeof(masks[0]); a++) {
        uint64_t p1 = masks[a];
        uint64_t d1 = p1 & ~masks[(a + 3) % 10];
        p1 &= masks[(a + 3) % 10];
        uint64_t p2 = masks[a];
        uint64_t d2 = a20_signal_native_ht_take_locked(&p2, masks[(a + 3) % 10]);
        eq64("txn_ht_take", d2, d1, "ht");
        eq64("txn_ht_pending", p2, p1, "ht");
    }
    for (int sig = -1; sig < 66; sig++) {
        uint64_t pt = 0, ph = 0;
        a20_signal_native_thread_pend_locked(&pt, sig);
        a20_signal_native_ht_pend_locked(&ph, sig);
        if (sig > 0 && sig < 64) {
            eq64("txn_ntc_pend", pt, 1ULL << sig, "pend");
            eq64("txn_ntc_pend_ht", ph, 1ULL << sig, "pend");
        } else {
            eq64("txn_ntc_pend", pt, 0, "pend");
            eq64("txn_ntc_pend_ht", ph, 0, "pend");
        }
    }
}

int main(void) {
    static const uint64_t handlers[] = {0, 1, 2, 0x4000, 0xdeadbeefULL, 0xffffffffffffffffULL};
    static const int flagsets[] = {0, SA_NOCLDSTOP, SA_NOCLDWAIT, SA_RESTART,
                                   SA_RESTART | SA_NOCLDWAIT, 4, 0x80000000, 0x7fffffff};
    static const int hows[] = {-2, -1, 0, 1, 2, 3, 7};

    for (int sig = 0; sig < 70; sig++) {
        char lbl[32];
        snprintf(lbl, sizeof(lbl), "sig=%d", sig);
        eqi("core_dump", a20_signal_core_dump_default(sig), c_core_dump_default(sig), lbl);
        eqi("terminate", a20_signal_default_terminate(sig), c_default_terminate(sig), lbl);
        eqi("stop", a20_signal_default_stop(sig), c_default_stop(sig), lbl);
        eqi("ignore", a20_signal_default_ignore(sig), c_default_ignore(sig), lbl);
        eq64("gen_cancel", a20_signal_generation_cancel_mask(sig), c_gen_cancel(sig), lbl);
        eqi("wait0", a20_signal_wait_status_dumped(sig, 0), c_wait_status_dumped(sig, 0), lbl);
        eqi("wait1", a20_signal_wait_status_dumped(sig, 1), c_wait_status_dumped(sig, 1), lbl);
        eqi("fatal_exit", a20_signal_fatal_exit_code_dumped(sig, 1),
            -c_wait_status_dumped(sig, 1), lbl);
        eqi("procmask_how_valid", a20_signal_procmask_how_valid(sig),
            (uint32_t)sig > 2 ? -22 : 0, lbl);
        for (unsigned h = 0; h < sizeof(handlers) / sizeof(handlers[0]); h++) {
            uint64_t hd = handlers[h];
            eqi("queue_discard", a20_signal_queue_discard(sig, hd), c_queue_discard(sig, hd), lbl);
            eqi("restart_skips", a20_signal_restart_skips(hd, sig), c_restart_skips(hd, sig), lbl);
            eqi("user_handler", a20_signal_action_user_handler_available(hd, 0),
                (hd != 0 && hd != 1), lbl);
            eqi("user_handler_b", a20_signal_action_user_handler_available(hd, 1), 0, lbl);
            for (unsigned m = 0; m < sizeof(masks) / sizeof(masks[0]); m++) {
                uint64_t mk = masks[m];
                eqi("action_deliv", a20_signal_action_deliverable(mk, hd, sig),
                    c_action_deliverable(mk, hd, sig), lbl);
                eqi("action_fatal", a20_signal_action_fatal(mk, hd, sig),
                    c_action_fatal(mk, hd, sig), lbl);
            }
            for (unsigned f = 0; f < sizeof(flagsets) / sizeof(flagsets[0]); f++) {
                int fl = flagsets[f];
                eqi("restart_vetoes", a20_signal_restart_vetoes(hd, fl),
                    c_restart_vetoes(hd, fl), lbl);
                eqi("sigchld_auto", a20_signal_sigchld_auto_reap(hd, fl),
                    c_sigchld_auto_reap(hd, fl), lbl);
                eqi("sigchld_nocldstop", a20_signal_sigchld_no_cldstop(fl),
                    (fl & SA_NOCLDSTOP) != 0, lbl);
            }
            eq64("exec_reset", a20_signal_exec_action_reset(hd), (uint64_t)c_exec_action_reset(hd), lbl);
        }
    }

    for (unsigned h = 0; h < sizeof(hows) / sizeof(hows[0]); h++) {
        for (unsigned a = 0; a < sizeof(masks) / sizeof(masks[0]); a++) {
            for (unsigned b = 0; b < sizeof(masks) / sizeof(masks[0]); b++) {
                char lbl[48];
                snprintf(lbl, sizeof(lbl), "how=%d", hows[h]);
                eq64("mask_apply", a20_signal_mask_apply(hows[h], masks[a], masks[b]),
                     c_mask_apply(hows[h], masks[a], masks[b]), lbl);
            }
        }
    }

    for (unsigned a = 0; a < sizeof(masks) / sizeof(masks[0]); a++) {
        for (unsigned b = 0; b < sizeof(masks) / sizeof(masks[0]); b++) {
            uint64_t x = masks[a], y = masks[b];
            eq64("deliverable", a20_signal_deliverable_mask(x, y, masks[(a + b) % 10]),
                 c_deliverable_mask(x, y, masks[(a + b) % 10]), "masks");
            eq64("pending_union", a20_signal_pending_union(x, y), x | y, "masks");
            eq64("pending_blocked", a20_signal_pending_blocked(x, y, masks[(a + 3) % 10]),
                 (x | y) & masks[(a + 3) % 10], "masks");
            eq64("strip", a20_signal_mask_strip_unblockable(x), c_strip_unblockable(x), "masks");
            eq64("temporary", a20_signal_temporary_mask(x), c_strip_unblockable(x), "masks");
            eqi("lowest", a20_signal_lowest(x), c_lowest(x), "masks");
            eqi("continue_pending", a20_signal_continue_pending(x),
                (x & kb(SIGCONT)) != 0, "masks");
            eq64("native_thread_mask", a20_signal_native_thread_mask(x),
                 x & ~(kb(SIGKILL) | kb(SIGSTOP) | 1), "masks");
            eq64("native_ht_mask", a20_signal_native_ht_mask(x), x & ~kb(SIGKILL), "masks");
            for (uint64_t rec = 1; rec <= 128; rec *= 8) {
                eqi("fd_read_valid", a20_signal_fd_read_valid(x, rec), x >= rec, "masks");
                eq64("fd_read_records", a20_signal_fd_read_records(x, rec), x / rec, "masks");
            }
            eqi("fd_ready", a20_signal_fd_ready(x, y), (x & y & ~1ULL) != 0, "masks");
        }
    }

    for (int td = 0; td <= 1; td++)
        for (int bs = 0; bs <= 1; bs++)
            for (int ip = 0; ip <= 1; ip++) {
                char lbl[32];
                snprintf(lbl, sizeof(lbl), "td=%d bs=%d ip=%d", td, bs, ip);
                eqi("queue_update_info", a20_signal_queue_update_info(td, bs),
                    td ? !bs : 1, lbl);
                eqi("queue_info_has", a20_signal_queue_info_has(td, bs, ip),
                    td ? (ip && !bs) : !!ip, lbl);
            }
    for (int tb = 0; tb <= 1; tb++)
        for (int tio = 0; tio <= 1; tio++)
            for (int th = 0; th <= 1; th++)
                for (int ph = 0; ph <= 1; ph++) {
                    char lbl[40];
                    snprintf(lbl, sizeof(lbl), "tb=%d tio=%d th=%d ph=%d", tb, tio, th, ph);
                    eqi("peek_scope", a20_signal_peek_scope(tb, tio, th, ph),
                        c_peek_scope(tb, tio, th, ph), lbl);
                    eqi("clear_scope", a20_signal_clear_scope(tb), tb ? 0 : 1, lbl);
                }

    for (int ss = 0; ss <= 1; ss++)
        for (int sh = 0; sh <= 1; sh++)
            eqi("sigsuspend_restore", a20_signal_sigsuspend_restore_active(ss, sh),
                ss && !sh, "sigsuspend");

    static const uint64_t sizes[] = {0, 1, 127, 128, 2048, 4096, 1ULL << 20};
    static const uint32_t altflags[] = {0, 1, 2, 3, 4, 0xffffffffu};
    for (unsigned f = 0; f < sizeof(altflags) / sizeof(altflags[0]); f++)
        for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
            uint32_t fl = altflags[f];
            uint64_t sz = sizes[s];
            int expect;
            if (fl & ~(uint32_t)SS_DISABLE) expect = -22;
            else if (!(fl & SS_DISABLE) && sz < 2048) expect = -12;
            else expect = 0;
            eqi("sigaltstack", a20_signal_sigaltstack_valid(fl, sz, 2048), expect, "altstack");
        }

    for (int sig = -1; sig < 70; sig++) {
        int av = a20_signal_action_valid(sig, 8, 8);
        int cv = (sig <= 0 || sig >= 64 || sig == SIGKILL || sig == SIGSTOP) ? -22 : 0;
        eqi("action_valid", av, cv, "sigvalid");
        eqi("action_valid_sz", a20_signal_action_valid(sig, 4, 8), -22, "sigvalid");
    }

    static const uint32_t uids[] = {0, 1, 7, 1000, 0xffffffffu};
    for (unsigned i = 0; i < 5; i++)
        for (unsigned j = 0; j < 5; j++)
            eqi("kill_perm", a20_signal_kill_permission(0, uids[i], uids[j], 9, 9),
                (uids[i] == 9 || uids[j] == 9) ? 0 : -1, "kill");
    eqi("kill_cap", a20_signal_kill_permission(1, 1, 1, 2, 2), 0, "kill");

    static const uint64_t modes[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 0xffffffffffffffffULL};
    for (unsigned m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
        eqi("native_kill_valid", a20_signal_native_kill_valid(modes[m], 10, 64),
            ((modes[m] & ~(uint64_t)1) != 0 || 10 < 0 || 10 >= 64), "nk");
        eqi("native_mode_check", a20_signal_native_mode_valid(modes[m], 7),
            ((modes[m] & ~(uint64_t)7) != 0 || (modes[m] && !(modes[m] & 1))), "nm");
        eqi("native_mode_mask", a20_signal_native_mode_valid(modes[m], 3),
            ((modes[m] & ~(uint64_t)3) != 0 || (modes[m] && !(modes[m] & 1))), "nm");
    }
    for (uint32_t op = 0; op < 4; op++)
        for (uint32_t fl = 0; fl < 10; fl++) {
            int expect;
            if (op == 0) expect = (fl & ~(uint32_t)7) != 0;
            else if (op == 1) expect = (fl & ~(uint32_t)3) != 0;
            else expect = 1;
            eqi("native_control", a20_signal_native_control_valid(op, fl, 0, 1, 7, 3),
                expect, "nc");
        }

    for (unsigned a = 0; a < sizeof(masks) / sizeof(masks[0]); a++)
        for (unsigned b = 0; b < sizeof(masks) / sizeof(masks[0]); b++)
            for (uint32_t mode = 0; mode < 8; mode++) {
                uint64_t ct1 = 111, ch1 = 222, ct2 = 111, ch2 = 222;
                uint64_t r1 = a20_signal_native_thread_check(
                    masks[a], masks[b], masks[(a + b) % 10], mode, masks[b], &ct1, &ch1);
                uint64_t r2 = c_native_thread_check(
                    masks[a], masks[b], masks[(a + b) % 10], mode, masks[b], &ct2, &ch2);
                eq64("native_thread_check", r1, r2, "ntc");
                eq64("native_thread_clear_t", ct1, ct2, "ntc");
                eq64("native_thread_clear_h", ch1, ch2, "ntc");
            }

    run_txn_sweep();

    printf("signal-core differential: %lu checks, %lu mismatches\n", checks, failures);
    return failures ? 1 : 0;
}
