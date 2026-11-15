#ifndef A20_TTY_CONTROL_H
#define A20_TTY_CONTROL_H
#include "core/types.h"
struct task_t;
struct vfile;
#define TTY_CONSOLE_ID 1u
#define TTY_PTY_ID(index) ((uint32_t)(index) + 2u)
#define TTY_CONTROL_PTYS 64u
typedef struct tty_hangup_notice { int sid, pgrp, leader_only; } tty_hangup_notice_t;

#ifdef CONFIG_MCU
static inline void tty_control_inherit(struct task_t *t, struct task_t *p) { (void)t; (void)p; }
static inline void tty_control_task_exit(struct task_t *t) { (void)t; }
#else
void tty_control_inherit(struct task_t *, struct task_t *);
void tty_control_task_exit(struct task_t *);
#endif
void tty_control_new_session(struct task_t *);
void tty_control_reset(uint32_t id);
void tty_control_bind(struct vfile *, uint32_t id, int master, int flags);
uint32_t tty_control_current(uint64_t *io_epoch);
int tty_control_revoked(const struct vfile *);
int tty_control_access(struct vfile *, int writing, int force_tostop);
int tty_control_is_ioctl(unsigned long);
int64_t tty_control_ioctl(struct vfile *, unsigned long, void *);
int tty_control_foreground(uint32_t id);
uint64_t tty_control_io_epoch(uint32_t id);
void tty_control_signal_foreground(uint32_t id, int signal);
/* prepare can run under the pair lock. Notify must run after it is dropped. */
tty_hangup_notice_t tty_control_hangup_prepare(uint32_t id, int revoke_io);
void tty_control_hangup_notify(tty_hangup_notice_t);
int tty_control_vhangup(void);

/* Process-list helpers pin notification targets before dropping the list
 * lock. No tty control lock is held while taking process/signal locks. */
int proc_tty_group_session(int pgid, int *sid);
int proc_tty_group_orphaned(int sid, int pgid);
void proc_tty_signal_group(int sid, int pgid, int signal);
void proc_tty_signal_leader(int sid, int signal);
int proc_tty_group_exited(struct task_t *);
void proc_tty_set_ids(struct task_t *, int sid, int pgid);
#endif
