#include "fs/procfs.h"
#include "fs/vfs/mount.h"
#include "net/netfilter.h"
#include "fs/procfs_internal.h"
#include "mm/pt.h"
#include "fs/vfs/mntns.h"
#include "proc/pidns.h"
#include "core/bootargs.h"
#include "fs/file.h"
#include "fs/fdtable.h"
#include "fs/block_cache.h"
#include "fs/ext4_journal.h"
#include "fs/page_cache.h"
#include "ipc/objstats.h"
#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "proc/lifetime.h"
#include "proc/coredump.h"
/* Unconditional on purpose: without CONFIG_XLATOR the header supplies
 * stubs, so this does not pull the channel into a cut-down build. */
#include "proc/xlator.h"
#include "mm/mm.h"
#include "mm/frame.h"
#include "mm/slab.h"
#include "mm/vm.h"
#include "mm/oom.h"
#include "core/psi.h"
#include "mm/swap.h"
#include "core/timer.h"
#include "core/timekeeping.h"
#include "core/perf.h"
#include "core/lock_counters.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/version.h"
#include "net/socket.h"
#include "net/socket_internal.h"
#include "net/net_config.h"
#include "net/lwip_stack.h"
#include "drivers/core/riscv_iommu.h"

#ifdef CONFIG_BOARD_LS2K1000
#include "platform.h"
#endif

#ifdef CONFIG_DRIVER_LIFECYCLE_TEST
#include "drivers/core/driver_lifecycle_test.h"
#endif

extern size_t  frame_free_count(void);
extern int     vfs_mount_count(void);
extern struct mount *vfs_mount_at(int index);

/*
 * On-demand content generation for the synthetic /proc files.  Split out of
 * fs/procfs.c so the vnode/file machinery and the text renderers stay
 * independently readable.  All functions are pure renderers: they take a
 * snapshot under the appropriate locks and format text into caller-owned
 * buffers; they never mutate procfs state.
 */

static const uint8_t g_proc_config_gz[] = {
    0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x03, 0x7d, 0x8f, 0x31, 0x0e, 0x02, 0x21,
    0x10, 0x45, 0x7b, 0x4f, 0x41, 0xe2, 0x11, 0x4c,
    0xec, 0x2c, 0x60, 0x58, 0x56, 0x02, 0x2c, 0x84,
    0x19, 0xd6, 0x58, 0x4d, 0x65, 0x61, 0xa3, 0xc5,
    0x6e, 0xe3, 0xed, 0x4d, 0x58, 0x13, 0x2d, 0xc0,
    0x6e, 0xfe, 0x7f, 0xaf, 0xf8, 0x03, 0x71, 0x32,
    0x76, 0xe4, 0x10, 0xca, 0xe9, 0xb5, 0x83, 0x2d,
    0xa4, 0x1c, 0x81, 0x0d, 0x7e, 0x0b, 0x0a, 0xa9,
    0xc6, 0xbd, 0xf8, 0x14, 0x0a, 0x75, 0xb5, 0x06,
    0x44, 0x96, 0x00, 0x24, 0xee, 0x8b, 0x78, 0x3c,
    0x57, 0xb1, 0xdc, 0xd6, 0xbe, 0xc4, 0xf3, 0xa1,
    0xe9, 0x91, 0x44, 0x87, 0x24, 0x09, 0x9b, 0x14,
    0xc6, 0x1c, 0x4b, 0xea, 0x30, 0x83, 0xac, 0xe4,
    0xa4, 0x2f, 0x56, 0xd3, 0xb9, 0x69, 0x58, 0xb7,
    0x1d, 0x7f, 0x61, 0x5d, 0xd9, 0x36, 0xd2, 0x7c,
    0x64, 0x04, 0x4a, 0x4d, 0xda, 0x05, 0x21, 0xea,
    0xe2, 0x87, 0xf6, 0x66, 0x27, 0xbd, 0xc7, 0x6b,
    0xe8, 0x3c, 0x14, 0x43, 0x92, 0xc4, 0x2a, 0xbb,
    0x5f, 0xfc, 0x06, 0x92, 0x96, 0xf1, 0x8c, 0xa4,
    0x01, 0x00, 0x00,
};

static char procfs_task_state_char(const task_t *task) {
    if (!task) return 'X';
    switch (task->state) {
    case PROC_READY:
    case PROC_RUNNING:
        return 'R';
    case PROC_BLOCKED:
        return 'S';
    case PROC_STOPPED:
        return 'T';
    case PROC_ZOMBIE:
        return 'Z';
    case PROC_UNUSED:
    default:
        return 'X';
    }
}

static const char *procfs_task_state_text(const task_t *task) {
    switch (procfs_task_state_char(task)) {
    case 'R': return "R (running)";
    case 'S': return "S (sleeping)";
    case 'T': return "T (stopped)";
    case 'Z': return "Z (zombie)";
    default:  return "X (dead)";
    }
}

static void appendf(char *buf, size_t bufsz, size_t *off, const char *fmt, ...) {
    if (*off >= bufsz) return;
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf + *off, bufsz - *off, fmt, args);
    va_end(args);
    if (n < 0) return;
    size_t wrote = (size_t)n;
    if (wrote >= bufsz - *off)
        *off = bufsz - 1;
    else
        *off += wrote;
}

static void append_vma_flags(char *buf, size_t bufsz, size_t *off,
                             uint64_t vm_flags) {
    appendf(buf, bufsz, off, "VmFlags:");
    if (vm_flags & VM_READ) appendf(buf, bufsz, off, " rd");
    if (vm_flags & VM_WRITE) appendf(buf, bufsz, off, " wr");
    if (vm_flags & VM_EXEC) appendf(buf, bufsz, off, " ex");
    appendf(buf, bufsz, off, " mr mw me ac");
    if (vm_flags & VM_STACK) appendf(buf, bufsz, off, " gd");
    if (vm_flags & VM_HUGEPAGE) appendf(buf, bufsz, off, " hg");
    if (vm_flags & VM_NOHUGEPAGE) appendf(buf, bufsz, off, " nh");
    appendf(buf, bufsz, off, "\n");
}

typedef struct vma_smaps_stats {
    size_t rss_pages;
    size_t shared_clean_pages;
    size_t shared_dirty_pages;
    size_t private_clean_pages;
    size_t private_dirty_pages;
    size_t anonymous_pages;
    size_t anon_huge_pages;
    size_t shmem_pmd_pages;
    size_t file_pmd_pages;
} vma_smaps_stats_t;

static void vma_collect_smaps_stats(mm_struct_t *mm, uint64_t start,
                                    uint64_t end, uint64_t vm_flags,
                                    vma_smaps_stats_t *stats) {
    if (!stats) return;
    memset(stats, 0, sizeof(*stats));
    if (!mm || !mm->pgdir) return;

    for (uint64_t va = start; va < end; ) {
        mm_leaf_info_t leaf;
        uint64_t lock_flags = spin_lock_irqsave(&mm->lock);
        int present = mm_query_leaf(mm->pgdir, va, &leaf);
        spin_unlock_irqrestore(&mm->lock, lock_flags);
        if (present) {
            size_t pages = leaf.size / PAGE_SIZE;
            int shared = (vm_flags & VM_SHARED) != 0;
            int dirty = leaf.dirty;
            int anon = (vm_flags & VM_ANON) != 0;

            stats->rss_pages += pages;
            if (shared) {
                if (dirty) stats->shared_dirty_pages += pages;
                else stats->shared_clean_pages += pages;
            } else {
                if (dirty) stats->private_dirty_pages += pages;
                else stats->private_clean_pages += pages;
            }
            if (anon)
                stats->anonymous_pages += pages;
            if (leaf.level > 0) {
                if (anon && shared)
                    stats->shmem_pmd_pages += pages;
                else if (anon)
                    stats->anon_huge_pages += pages;
                else
                    stats->file_pmd_pages += pages;
            }
            uint64_t next = leaf.base + leaf.size;
            va = next > va ? next : va + PAGE_SIZE;
        } else {
            va += PAGE_SIZE;
        }
    }
}

typedef struct procfs_vma_snapshot {
    uint64_t start;
    uint64_t end;
    uint64_t vm_flags;
    uint64_t file_offset;
    unsigned long ino;
    int thp_eligible;
    char name[MAX_PATH_LEN];
    vma_smaps_stats_t stats;
} procfs_vma_snapshot_t;

static int snapshot_pid_maps(int pid, int smaps,
                             procfs_vma_snapshot_t **records_out,
                             size_t *count_out)
{
    task_t *task = proc_find_get(pid);
    if (!task)
        return -ESRCH;
    if (!proc_task_may_access(proc_current(), task)) {
        proc_put(task);
        return -EACCES;
    }
#ifndef ARCH_NO_PMD_LEAF
    int thp_disabled = task->policy.thp_disabled;
#endif
    mm_struct_t *mm = proc_task_get_mm(task);
    proc_put(task);
    if (!mm)
        return -ESRCH;

    procfs_vma_snapshot_t *records = NULL;
    size_t capacity = 0;
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&mm->lock);
        size_t needed = 0;
        for (mm_seg_t *v = mm->mmap; v; v = v->next)
            needed++;
        spin_unlock_irqrestore(&mm->lock, flags);

        if (needed > capacity) {
            if (needed > SIZE_MAX / sizeof(*records)) {
                kfree(records);
                mm_destroy(mm);
                return -ENOMEM;
            }
            procfs_vma_snapshot_t *new_records =
                krealloc(records, needed * sizeof(*records));
            if (!new_records && needed) {
                kfree(records);
                mm_destroy(mm);
                return -ENOMEM;
            }
            records = new_records;
            capacity = needed;
        }

        flags = spin_lock_irqsave(&mm->lock);
        size_t count = 0;
        int retry = 0;
        for (mm_seg_t *v = mm->mmap; v; v = v->next) {
            if (count >= capacity) {
                retry = 1;
                break;
            }
            procfs_vma_snapshot_t *rec = &records[count++];
            memset(rec, 0, sizeof(*rec));
            rec->start = v->start;
            rec->end = v->end;
            rec->vm_flags = v->vm_flags;
            rec->file_offset = v->backing_offset;
#ifdef ARCH_NO_PMD_LEAF
            rec->thp_eligible = 0;
#else
            rec->thp_eligible = !thp_disabled &&
                !(v->vm_flags & VM_NOHUGEPAGE) &&
                (v->vm_flags & (VM_HUGEPAGE | VM_ANON)) &&
                (v->end - v->start) >= (2UL * 1024 * 1024);
#endif
            if (v->vm_flags & VM_STACK) {
                strncpy(rec->name, "[stack]", sizeof(rec->name) - 1);
            } else if (v->start >= mm->start_brk && v->start < mm->brk) {
                strncpy(rec->name, "[heap]", sizeof(rec->name) - 1);
            } else if (v->file) {
                vfile_t *vf = v->file;
                if (vf->path[0])
                    strncpy(rec->name, vf->path,
                            sizeof(rec->name) - 1);
                if (vf->vnode)
                    rec->ino = (unsigned long)vf->vnode->ino;
            }
        }
        spin_unlock_irqrestore(&mm->lock, flags);
        if (!retry) {
            if (smaps) {
                for (size_t i = 0; i < count; i++) {
                    procfs_vma_snapshot_t *rec = &records[i];
                    vma_collect_smaps_stats(mm, rec->start, rec->end,
                                            rec->vm_flags, &rec->stats);
                }
            }
            mm_destroy(mm);
            *records_out = records;
            *count_out = count;
            return 0;
        }
    }
}

int generate_pid_maps_alloc(int pid, int smaps, char **buf_out,
                                   size_t *len_out)
{
    procfs_vma_snapshot_t *records = NULL;
    size_t count = 0;
    int ret = snapshot_pid_maps(pid, smaps, &records, &count);
    if (ret < 0)
        return ret;

    size_t per_record = MAX_PATH_LEN + (smaps ? 1024 : 128);
    if (count > (SIZE_MAX - 1) / per_record) {
        kfree(records);
        return -ENOMEM;
    }
    size_t bufsz = count * per_record + 1;
    char *buf = kmalloc(bufsz);
    if (!buf) {
        kfree(records);
        return -ENOMEM;
    }
    buf[0] = '\0';
    size_t off = 0;
    for (size_t i = 0; i < count; i++) {
        procfs_vma_snapshot_t *rec = &records[i];
        char r = (rec->vm_flags & VM_READ) ? 'r' : '-';
        char w = (rec->vm_flags & VM_WRITE) ? 'w' : '-';
        char x = (rec->vm_flags & VM_EXEC) ? 'x' : '-';
        char s = (rec->vm_flags & VM_SHARED) ? 's' : 'p';
        size_t kb = (size_t)(rec->end - rec->start) / 1024;
        size_t rss_kb = rec->stats.rss_pages * PAGE_SIZE / 1024;

        if (rec->name[0]) {
            appendf(buf, bufsz, &off,
                    "%08lx-%08lx %c%c%c%c %08lx 00:00 %lu %s\n",
                    (unsigned long)rec->start, (unsigned long)rec->end,
                    r, w, x, s, (unsigned long)rec->file_offset,
                    rec->ino, rec->name);
        } else {
            appendf(buf, bufsz, &off,
                    "%08lx-%08lx %c%c%c%c %08lx 00:00 %lu\n",
                    (unsigned long)rec->start, (unsigned long)rec->end,
                    r, w, x, s, (unsigned long)rec->file_offset,
                    rec->ino);
        }
        if (!smaps)
            continue;
        vma_smaps_stats_t *st = &rec->stats;
        appendf(buf, bufsz, &off,
                 "Size:           %8lu kB\n"
                "KernelPageSize: %8lu kB\n"
                "MMUPageSize:    %8lu kB\n"
                "Rss:            %8lu kB\n"
                "Pss:            %8lu kB\n"
                "Shared_Clean:   %8lu kB\n"
                "Shared_Dirty:   %8lu kB\n"
                "Private_Clean:  %8lu kB\n"
                "Private_Dirty:  %8lu kB\n"
                "Referenced:     %8lu kB\n"
                "Anonymous:      %8lu kB\n"
                "AnonHugePages:  %8lu kB\n"
                "ShmemPmdMapped: %8lu kB\n"
                "FilePmdMapped:  %8lu kB\n"
                "THPeligible:    %8d\n",
                (unsigned long)kb,
                (unsigned long)(PAGE_SIZE / 1024),
                (unsigned long)(PAGE_SIZE / 1024),
                (unsigned long)rss_kb,
                (unsigned long)rss_kb,
                (unsigned long)(st->shared_clean_pages * PAGE_SIZE / 1024),
                (unsigned long)(st->shared_dirty_pages * PAGE_SIZE / 1024),
                (unsigned long)(st->private_clean_pages * PAGE_SIZE / 1024),
                (unsigned long)(st->private_dirty_pages * PAGE_SIZE / 1024),
                (unsigned long)rss_kb,
                (unsigned long)(st->anonymous_pages * PAGE_SIZE / 1024),
                (unsigned long)(st->anon_huge_pages * PAGE_SIZE / 1024),
                (unsigned long)(st->shmem_pmd_pages * PAGE_SIZE / 1024),
                (unsigned long)(st->file_pmd_pages * PAGE_SIZE / 1024),
                rec->thp_eligible);
        append_vma_flags(buf, bufsz, &off, rec->vm_flags);
    }
    kfree(records);
    *buf_out = buf;
    *len_out = off;
    return 0;
}

/*
 * /proc/net/{tcp,udp,unix} rows.
 *
 * net_socket_table_walk() hands each socket over while holding g_net_lock, so
 * a row is built in a fixed local buffer and copied out with a running offset
 * instead of formatted straight into the destination; once the destination is
 * full the walk keeps running but every visitor bails before formatting.
 */
#define PROCFS_NET_ROW_MAX  256
#define PROCFS_NET_ADDR_MAX 40

typedef struct {
    char   *buf;
    size_t  bufsz;
    size_t  off;
    int     datagram;   /* /proc/net/udp: a datagram socket has no TCP state */
    char    row[PROCFS_NET_ROW_MAX];
} procfs_net_rows_t;

/* Linux prints a /proc/net address as the raw network-order 32-bit word, so
 * 127.0.0.1 reads 0100007F, while the port is printed in host order -- a
 * sockaddr keeps sin_port in network order, hence the swap.  AF_INET6 takes
 * the tcp6 layout of four 32-bit words, which is also how an IPv4-mapped
 * address renders (0000000000000000FFFF0000<IPv4>). */
static uint32_t procfs_net_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void procfs_net_addr(char *out, size_t outsz, const uint8_t *addr,
                            size_t addrlen, int domain)
{
    uint16_t sport = 0;
    if (net_sockaddr_port(addr, addrlen, &sport) < 0)
        sport = 0;

    if (domain == AF_INET6) {
        /* The layout follows the socket's family, NOT whether this particular
         * address happens to be filled in.  A peerless listener has no peer
         * address at all, and keying the width off addrlen rendered its remote
         * column in the tcp (8 hex digit) layout inside a tcp6 file -- so the
         * one row that proves IPv6 inbound listen works was the one row whose
         * columns did not line up.  Linux prints the all-zero address and port
         * 0000 in the full four-word width for a listener with no peer: the
         * column width of a /proc/net/tcp6 file is uniform by construction. */
        uint32_t w[4] = { 0, 0, 0, 0 };
        if (addr && addrlen >= sizeof(net_sockaddr_in6_t)) {
            const uint8_t *a = ((const net_sockaddr_in6_t *)addr)->sin6_addr;
            for (int i = 0; i < 4; i++)
                w[i] = procfs_net_le32(a + 4 * i);
        }
        snprintf(out, outsz, "%08X%08X%08X%08X:%04X",
                 w[0], w[1], w[2], w[3], (unsigned)net_ntohs(sport));
        return;
    }
    /* An unbound socket has no address yet; the column reads as the wildcard,
     * which is what Linux shows for a socket that has not been bound. */
    uint32_t v4 = 0;
    if (addrlen >= sizeof(net_sockaddr_in_t) &&
        *(const uint16_t *)addr == AF_INET)
        v4 = ((const net_sockaddr_in_t *)addr)->sin_addr;
    snprintf(out, outsz, "%08X:%04X", v4, (unsigned)net_ntohs(sport));
}

/* Linux TCP state numbers, derived only from flags the socket layer maintains
 * itself.  The lwIP pcb's own state machine lives under g_lwip_lock, which a
 * table walk must never take, so the FIN_WAIT / TIME_WAIT / CLOSING half of
 * the state space is not observable here and is left unmapped rather than
 * guessed.  `syn_sent` is false for AF_UNIX, which has no handshake. */
#define PROCFS_TCP_ESTABLISHED 0x01
#define PROCFS_TCP_SYN_SENT    0x02
#define PROCFS_TCP_CLOSE       0x07
#define PROCFS_TCP_CLOSE_WAIT  0x08
#define PROCFS_TCP_LISTEN      0x0A

static unsigned procfs_net_state(const net_socket_t *s, int syn_sent)
{
    if (s->listening)
        return PROCFS_TCP_LISTEN;
    if (syn_sent && s->tcp_connecting)
        return PROCFS_TCP_SYN_SENT;
    if (s->connected)
        return s->peer_closed ? PROCFS_TCP_CLOSE_WAIT
                              : PROCFS_TCP_ESTABLISHED;
    /* Bound but neither listening nor connecting is exactly Linux's CLOSE. */
    return PROCFS_TCP_CLOSE;
}

/* Unread bytes in the receive queue -- the same accounting FIONREAD reports.
 * Recomputed here because net_socket_rx_available() takes g_net_lock itself
 * and this walk already holds it. */
static unsigned procfs_net_rx_queued(const net_socket_t *s)
{
    size_t total = 0;
    for (const net_msg_t *m = s->rx_head; m; m = m->next)
        total += (s->type == SOCK_STREAM) ? (m->len - m->off) : m->len;
    return (unsigned)total;
}

/*
 * One /proc/net/tcp or /proc/net/udp row:
 *   "%4d: %s %s %02X %08X:%08X %02X:%08X %08X %d %ld %d\n"
 *   sl, local_address:port, rem_address:port, st, tx_queue:rx_queue,
 *   tr:tm->when, retrnsmt, uid, timeout, inode.
 *
 * Real: the two endpoint addresses, the state derived from the socket flags,
 * the unread receive-queue bytes, and SO_RCVTIMEO.
 *
 * Emitted as 0 because the data does not exist, not because it is zero:
 *   tx_queue   unsent bytes (write_seq - snd_una) live in the lwIP pcb, and
 *              reading them would mean taking g_lwip_lock inside this
 *              g_net_lock section, which the lock contract forbids.  For an
 *              unsent/idle socket Linux prints 00000000 here too.
 *   tr, retrnsmt
 *              retransmit timer and count: same lock problem.  00:00000000 and
 *              00000000 are exactly Linux's output for an inactive timer.
 *   uid        net_socket_t.owner_uid is only recorded by the AF_UNIX bind
 *              path, so an inet socket carries no owner.  This 0 means
 *              "unrecorded", NOT root -- do not read it as an euid.
 *   inode      A20OS net_socket_t has no socket inode, and the procfs
 *              namespace entry is not reachable from a g_net_lock section.
 */
static void procfs_net_inet_row(net_socket_t *s, void *arg)
{
    procfs_net_rows_t *r = arg;
    if (r->off + 1 >= r->bufsz)
        return;

    char local[PROCFS_NET_ADDR_MAX], peer[PROCFS_NET_ADDR_MAX];
    procfs_net_addr(local, sizeof(local), s->local, s->local_len, s->domain);
    procfs_net_addr(peer, sizeof(peer), s->peer_addr, s->peer_len, s->domain);
    unsigned st = r->datagram ? PROCFS_TCP_CLOSE
                              : procfs_net_state(s, 1);
    snprintf(r->row, sizeof(r->row),
             "%4d: %s %s %02X %08X:%08X %02X:%08X %08X %d %ld %d\n",
             s->reg_idx, local, peer, st,
             0u, procfs_net_rx_queued(s),
             0u, 0u, 0u,
             s->owner_uid, (long)s->recv_timeout_ticks, 0);
    appendf(r->buf, r->bufsz, &r->off, "%s", r->row);
}

/*
 * One /proc/net/unix row.  Num is the socket netns cookie, RefCount a
 * per-socket refcount and Flags the vfile's open flags in Linux; A20OS has
 * none of the three reachable from a net_socket_t under g_net_lock (a vfile
 * reference cannot be taken here), so they read 0 rather than a guess.  The
 * path is s->local's sun_path, which net_unix_sockaddr_prepare() stores
 * absolute and NUL-terminated; a socketpair has no path and shows none.
 */
static void procfs_net_unix_row(net_socket_t *s, void *arg)
{
    procfs_net_rows_t *r = arg;
    if (r->off + 1 >= r->bufsz)
        return;

    const char *path = "";
    if (s->local_len > sizeof(uint16_t) && s->local[sizeof(uint16_t)] != '\0')
        path = (const char *)s->local + sizeof(uint16_t);
    snprintf(r->row, sizeof(r->row),
             "%08X%08X: %08X %08X %08X %04X %02X %d %s\n",
             0u, 0u, 0u, 0u, 0u,
             (unsigned)s->type, procfs_net_state(s, 0), 0, path);
    appendf(r->buf, r->bufsz, &r->off, "%s", r->row);
}

int generate_content(pf_type_t type, int pid, char *buf, size_t bufsz) {
    buf[0] = '\0';
    switch (type) {
    case PF_STAT: {
        size_t off = 0;
        uint64_t tot_user = 0, tot_sys = 0, tot_idle = 0;
        uint64_t u[CONFIG_NR_CPUS], s[CONFIG_NR_CPUS], idl[CONFIG_NR_CPUS];
        for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS; cpu++) {
            proc_get_cpu_times(cpu, &u[cpu], &s[cpu], &idl[cpu]);
            tot_user += u[cpu];
            tot_sys += s[cpu];
            tot_idle += idl[cpu];
        }
        appendf(buf, bufsz, &off, "cpu  %llu 0 %llu %llu 0 0 0 0 0 0\n",
                (unsigned long long)tot_user, (unsigned long long)tot_sys,
                (unsigned long long)tot_idle);
        for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS; cpu++)
            appendf(buf, bufsz, &off, "cpu%u %llu 0 %llu %llu 0 0 0 0 0 0\n",
                    cpu, (unsigned long long)u[cpu],
                    (unsigned long long)s[cpu], (unsigned long long)idl[cpu]);
        uint64_t rt[2], mono[2];
        timekeeping_get_realtime(rt);
        timekeeping_get_monotonic(mono);
        proc_lifetime_stats_t lt;
        proc_lifetime_snapshot(&lt);
        appendf(buf, bufsz, &off,
                "intr 0\n"
                "ctxt 0\n"
                "btime %llu\n"
                "processes %lu\n"
                "procs_running %llu\n"
                "procs_blocked 0\n",
                (unsigned long long)(rt[0] > mono[0] ? rt[0] - mono[0] : 0),
                lt.tasks_created,
                (unsigned long long)proc_runq_load_sum());
        break;
    }
    case PF_MEMINFO: {
        size_t free_frames = frame_free_count();
        size_t total_kb = pfa.total_frames * PAGE_SIZE / 1024;
        size_t free_kb = free_frames * PAGE_SIZE / 1024;
        slab_stats_t slab;
        bcache_stats_t bc;
        page_cache_stats_t pc;
        proc_vm_stats_t vmstats;
        pfa_huge_stats_t huge;
        slab_get_stats(&slab);
        bcache_get_stats(&bc);
        page_cache_get_stats(&pc);
        proc_get_vm_stats(&vmstats);
        pfa_get_huge_stats(&huge);
        size_t buffers_kb = bc.block_pool_bytes / 1024;
        /* Cached = block cache + VFS page cache; both are reclaimable and
         * must be visible to userspace monitors, otherwise page-cache
         * growth (up to RAM/8) looks exactly like a memory leak. */
        size_t cached_kb = (bc.valid_pages * PCACHE_PAGE_SIZE +
                            pc.valid * PAGE_SIZE) / 1024;
        size_t dirty_kb = (bc.dirty_blocks * BCACHE_BLOCK_SIZE +
                           bc.dirty_pages * PCACHE_PAGE_SIZE +
                           pc.dirty * PAGE_SIZE) / 1024;
        size_t slab_kb = slab.total_bytes / 1024;
        size_t sreclaim_kb = slab.reclaimable_bytes / 1024;
        size_t sunreclaim_kb = slab_kb > sreclaim_kb ? slab_kb - sreclaim_kb : 0;
        size_t available_kb = free_kb + cached_kb + sreclaim_kb;
#ifdef CONFIG_SWAP
        size_t swap_total_kb = total_swap_pages * PAGE_SIZE / 1024;
        size_t swap_free_kb = nr_swap_pages * PAGE_SIZE / 1024;
#else
        size_t swap_total_kb = 0;
        size_t swap_free_kb = 0;
#endif
        if (available_kb > total_kb)
            available_kb = total_kb;
        snprintf(buf, bufsz,
            "MemTotal:       %lu kB\n"
            "MemFree:        %lu kB\n"
            "MemAvailable:   %lu kB\n"
            "Buffers:        %lu kB\n"
            "Cached:         %lu kB\n"
            "SwapTotal:      %lu kB\n"
            "SwapFree:       %lu kB\n"
            "Shmem:          0 kB\n"
            "Dirty:          %lu kB\n"
            "Slab:           %lu kB\n"
            "SReclaimable:   %lu kB\n"
            "SUnreclaim:     %lu kB\n"
            "AnonHugePages:  %lu kB\n"
            "ShmemHugePages: %lu kB\n"
            "FileHugePages:  %lu kB\n"
            "HugePages_Total: %lu\n"
            "HugePages_Free:  %lu\n"
            "Hugepagesize:   2048 kB\n",
            (unsigned long)total_kb,
            (unsigned long)free_kb,
            (unsigned long)available_kb,
            (unsigned long)buffers_kb,
            (unsigned long)cached_kb,
            (unsigned long)swap_total_kb,
            (unsigned long)swap_free_kb,
            (unsigned long)dirty_kb,
            (unsigned long)slab_kb,
            (unsigned long)sreclaim_kb,
            (unsigned long)sunreclaim_kb,
            (unsigned long)(vmstats.anon_huge_pages * PAGE_SIZE / 1024),
            (unsigned long)(vmstats.shmem_huge_pages * PAGE_SIZE / 1024),
            (unsigned long)(vmstats.file_huge_pages * PAGE_SIZE / 1024),
            (unsigned long)huge.total_huge_pages,
            (unsigned long)huge.free_huge_pages);
        break;
    }
    case PF_SWAPS: {
        size_t off = 0;
        appendf(buf, bufsz, &off,
                "Filename\t\t\t\tType\t\tSize\tUsed\tPriority\n");
#ifdef CONFIG_SWAP
        for (int type = 0; type < MAX_SWAPFILES; type++) {
            swap_listing_t sl;
            if (swap_list_area(type, &sl) != 0)
                continue;
            /* core/printf.c has no '-' flag support; emit unaligned fields. */
            appendf(buf, bufsz, &off, "%s\tpartition\t%llu\t%lu\t-2\n",
                    sl.name,
                    (unsigned long long)sl.pages,
                    (unsigned long)sl.inuse_pages);
        }
#endif
        break;
    }
    case PF_VERSION:
        snprintf(buf, bufsz, "%s version %s (%s) (%s)\n",
                 A20OS_SYSNAME, A20OS_RELEASE, ARCH_NAME, A20OS_VERSION_FULL);
        break;
    case PF_UPTIME: {
        uint64_t ticks = timer_get_ticks();
        uint64_t sec = ticks / TICKS_PER_SEC;
        uint64_t frac = (ticks % TICKS_PER_SEC) * 100 / TICKS_PER_SEC;
        snprintf(buf, bufsz, "%lu.%02lu\n", (unsigned long)sec, (unsigned long)frac);
        break;
    }
    case PF_CMDLINE:
        /* Real kernel command line (a20.* keys included); Linux exposes
         * the same content here. */
        snprintf(buf, bufsz, "%s\n", bootargs_get() ? bootargs_get() : "");
        break;
    case PF_CPUINFO:
        snprintf(buf, bufsz,
            "processor\t: 0\n"
            "hart\t\t: 0\n"
            "isa\t\t: rv64gc\n"
            "mmu\t\t: sv39\n\n");
        break;
    case PF_MOUNTS: {
        buf[0] = '\0';
        int pos = 0;
        for (int i = 0; i < vfs_mount_count(); i++) {
            struct mount *m = vfs_mount_at(i);
            if (!m || !m->path[0]) continue;
            const char *fstype = m->fstype[0] ? m->fstype : "unknown";
            const char *dev = m->dev[0] ? m->dev : "none";
            const char *opts = m->opts[0] ? m->opts : "rw";
            int n = snprintf(buf + pos, bufsz - pos,
                "%s %s %s %s 0 0\n", dev, m->path, fstype, opts);
            if (n < 0 || (size_t)n >= bufsz - pos) break;
            pos += n;
        }
        break;
    }
    case PF_LOADAVG:
    {
        uint64_t a1, a5, a15;
        unsigned running, total;
        int max_pid;
        proc_loadavg_snapshot(&a1, &a5, &a15, &running, &total, &max_pid);
        /* Linux prints load in 1/100ths with two decimals, unpadded. */
        unsigned l1 = (unsigned)((a1 * 100ULL) >> 16);
        unsigned l5 = (unsigned)((a5 * 100ULL) >> 16);
        unsigned l15 = (unsigned)((a15 * 100ULL) >> 16);
        snprintf(buf, bufsz, "%u.%02u %u.%02u %u.%02u %u/%u %d\n",
                 l1 / 100, l1 % 100, l5 / 100, l5 % 100,
                 l15 / 100, l15 % 100, running, total, max_pid);
        break;
    }
        break;
    case PF_NET:
        net_format_status(buf, bufsz);
        break;
    case PF_NET_STATUS:
        net_format_status(buf, bufsz);
        break;
    case PF_NET_TCP:
    case PF_NET_UDP:
    case PF_NET_TCP6:
    case PF_NET_UDP6: {
        snprintf(buf, bufsz,
                 "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n");
        /* Linux splits the two families into separate files, and so does this:
         * tcp/udp list AF_INET rows, tcp6/udp6 list AF_INET6 rows.  The row
         * format is identical -- procfs_net_addr() picks the address layout
         * from the socket's own family, so a v6 row is four 32-bit words and a
         * v4 row is one, exactly as on Linux. */
        int datagram = (type == PF_NET_UDP || type == PF_NET_UDP6);
        int family = (type == PF_NET_TCP6 || type == PF_NET_UDP6) ? AF_INET6
                                                                 : AF_INET;
        procfs_net_rows_t rows = {
            .buf = buf, .bufsz = bufsz, .off = (size_t)strlen(buf),
            .datagram = datagram,
        };
        (void)net_socket_table_walk(datagram ? NET_TABLE_UDP : NET_TABLE_TCP,
                                    family, procfs_net_inet_row, &rows);
        break;
    }
    case PF_NET_UNIX: {
        snprintf(buf, bufsz,
                 "Num       RefCount Protocol Flags    Type St Inode Path\n");
        procfs_net_rows_t rows = {
            .buf = buf, .bufsz = bufsz, .off = (size_t)strlen(buf),
        };
        (void)net_socket_table_walk(NET_TABLE_UNIX, AF_UNSPEC,
                                    procfs_net_unix_row, &rows);
        break;
    }
    case PF_NET_CONFIG:
        a20_net_config_format(buf, bufsz);
        break;
    case PF_NET_DEV:
        a20_lwip_format_net_dev(buf, bufsz);
        break;
    case PF_ROUTE:
        a20_lwip_format_route(buf, bufsz);
        break;
    case PF_CONFIG_GZ: {
        size_t n = sizeof(g_proc_config_gz) < bufsz ? sizeof(g_proc_config_gz) : bufsz;
        memcpy(buf, g_proc_config_gz, n);
        return (int)n;
    }
    case PF_A20_ANONPROV:
        snprintf(buf, bufsz, "%u\n", mm_pt_anon_prov_max());
        return (int)strlen(buf);
    case PF_A20_SCHED_BASE_SLICE:
        snprintf(buf, bufsz, "%d\n", g_sched_base_slice_ms);
        return (int)strlen(buf);
#ifdef CONFIG_XLATOR
    case PF_A20_XLATOR:
        /* xlator_render() formats the switch state, the forwarding counter
         * and the per-guest table, so an administrator can tell "off" from
         * "on but nothing configured" without reading the boot log. */
        return xlator_render(buf, bufsz);
#endif
    case PF_A20_BCACHE: {
        bcache_stats_t bc;
        bcache_get_stats(&bc);
        snprintf(buf, bufsz,
            "caches: %lu\n"
            "block_pool_bytes: %lu\n"
            "page_pool_bytes: %lu\n"
            "valid_blocks: %lu\n"
            "dirty_blocks: %lu\n"
            "valid_pages: %lu\n"
            "dirty_pages: %lu\n",
            (unsigned long)bc.caches,
            (unsigned long)bc.block_pool_bytes,
            (unsigned long)bc.page_pool_bytes,
            (unsigned long)bc.valid_blocks,
            (unsigned long)bc.dirty_blocks,
            (unsigned long)bc.valid_pages,
            (unsigned long)bc.dirty_pages);
        break;
    }
    case PF_A20_PAGE_CACHE: {
        page_cache_stats_t pc;
        page_cache_get_stats(&pc);
        snprintf(buf, bufsz,
            "capacity: %lu\n"
            "allocated: %lu\n"
            "bytes: %lu\n"
            "valid: %lu\n"
            "dirty: %lu\n"
            "pinned: %lu\n",
            (unsigned long)pc.capacity,
            (unsigned long)pc.allocated,
            (unsigned long)pc.bytes,
            (unsigned long)pc.valid,
            (unsigned long)pc.dirty,
            (unsigned long)pc.pinned);
        break;
    }
    case PF_A20_OOM: {
        oom_stats_t os;
        oom_get_stats(&os);
        snprintf(buf, bufsz,
            "kills: %lu\n"
            "last_kill_tick: %lu\n"
            "last_victim_pid: %d\n"
            "last_victim_score: %d\n"
            "free_pages_at_kill: %lu\n"
            "free_pages_now: %lu\n"
            "in_progress: %d\n"
            "kswapd_passes: %lu\n"
            "kswapd_pages_freed: %lu\n"
            "kswapd_last_pass_tick: %lu\n",
            os.kills,
            os.last_kill_tick,
            os.last_victim_pid,
            os.last_victim_score,
            os.free_pages_at_kill,
            os.free_pages_now,
            os.in_progress,
            os.kswapd_passes,
            os.kswapd_pages_freed,
            os.kswapd_last_pass_tick);
        break;
    }
    case PF_A20_TASK_LIFETIME:
        return (int)proc_lifetime_format(buf, bufsz);
    case PF_A20_PERF:
        return (int)a20_perf_format(buf, bufsz);
    case PF_A20_LOCK_CONTENTION:
        return (int)lock_counters_format(buf, bufsz);
    case PF_A20_NETFILTER:
        netfilter_format(buf, bufsz);
        return (int)strlen(buf);
    case PF_A20_NETMEM:
        return a20_lwip_format_memp(buf, bufsz);
    case PF_A20_JOURNAL:
        return ext4_journal_crash_points_format(buf, bufsz);
    case PF_A20_OBJECTS:
        snprintf(buf, bufsz,
            "handles: %lu\n"
            "channel_eps: %lu\n"
            "eventqs: %lu\n"
            "vmos: %lu\n"
            "vmo_pages: %lu\n"
            "irq_bindings: %lu\n"
            "vfiles: %lu\n"
            "vmo_dirty_frames: %lu\n",
            (unsigned long)__atomic_load_n(&g_a20_objstats.handles, __ATOMIC_RELAXED),
            (unsigned long)__atomic_load_n(&g_a20_objstats.channel_eps, __ATOMIC_RELAXED),
            (unsigned long)__atomic_load_n(&g_a20_objstats.eventqs, __ATOMIC_RELAXED),
            (unsigned long)__atomic_load_n(&g_a20_objstats.vmos, __ATOMIC_RELAXED),
            (unsigned long)__atomic_load_n(&g_a20_objstats.vmo_pages, __ATOMIC_RELAXED),
            (unsigned long)__atomic_load_n(&g_a20_objstats.irq_bindings, __ATOMIC_RELAXED),
            (unsigned long)__atomic_load_n(&g_a20_objstats.vfiles, __ATOMIC_RELAXED),
            (unsigned long)__atomic_load_n(&g_a20_objstats.vmo_dirty_frames, __ATOMIC_RELAXED));
        break;
    case PF_A20_DRIVER_LIFECYCLE:
        buf[0] = '\0';
        return 0;
    case PF_A20_IOMMU: {
        riscv_iommu_stats_t st;
        riscv_iommu_get_stats(&st);
        snprintf(buf, bufsz,
            "enabled: %d\n"
            "domains_claimed: %lu\n"
            "domains_released: %lu\n"
            "maps: %lu\n"
            "map_failures: %lu\n"
            "unmaps: %lu\n"
            "mapped_pages: %lu\n"
            "fault_records: %lu\n"
            "faults: %lu\n"
            "blocked_events: %lu\n"
            "last_fault_devid: %u\n"
            "last_fault_cause: %lu\n"
            "last_fault_iova: 0x%lx\n"
            "last_fault_owner: %d\n",
            st.enabled,
            (unsigned long)st.domains_claimed,
            (unsigned long)st.domains_released,
            (unsigned long)st.maps,
            (unsigned long)st.map_failures,
            (unsigned long)st.unmaps,
            (unsigned long)st.mapped_pages,
            (unsigned long)st.fault_records,
            (unsigned long)st.faults,
            (unsigned long)st.blocked_events,
            (unsigned)st.last_fault_devid,
            (unsigned long)st.last_fault_cause,
            (unsigned long)st.last_fault_iova,
            st.last_fault_owner);
        break;
    }
    case PF_SYSRQ_TRIGGER:
        return snprintf(buf, bufsz,
                        "sysrq commands: c (crash — deliberate kernel panic)\n");
    case PF_PID_STAT: {
        task_t *t = proc_find_get(pid);
        if (!t) { snprintf(buf, bufsz, "%d (unknown) S 0 0\n", pid); break; }
        /* Linux field order: pid comm state ppid pgrp session tty_nr tpgid
         * flags minflt cminflt majflt cmajflt utime stime cutime cstime
         * priority nice num_threads itrealvalue starttime vsize rss */
        size_t vsize = t->mm ? t->mm->total_vm * PAGE_SIZE : 0;
        long rss_pages = (long)mm_rss_get(t->mm);
        snprintf(buf, bufsz,
            "%d (%s) %c %d %d %d 0 0 0 0 0 0 0 %lu %lu %ld %ld %d %d %d 0 %lu %lu %ld\n",
            t->pid, t->name, procfs_task_state_char(t),
            t->ppid, t->pgid, t->sid,
            (unsigned long)t->utime_ticks,
            (unsigned long)t->stime_ticks,
            (long)t->child_utime,
            (long)t->child_stime,
            t->priority, 0, 1,
            (unsigned long)t->start_jiffies,
            (unsigned long)vsize, rss_pages);
        proc_put(t);
        break;
    }
    case PF_PID_STATUS: {
        task_t *t = proc_find_get(pid);
        if (!t) { snprintf(buf, bufsz, "Name:\tunknown\nPid:\t%d\n", pid); break; }
        const char *state = procfs_task_state_text(t);
        char groups[160];
        size_t glen = 0;
        groups[0] = '\0';
        for (int i = 0; i < t->cred.ngroups && i < MAX_GROUPS; i++) {
            int n = snprintf(groups + glen, sizeof(groups) - glen, "%s%d",
                             i ? " " : "", t->cred.groups[i]);
            if (n < 0 || (size_t)n >= sizeof(groups) - glen)
                break;
            glen += (size_t)n;
        }

        size_t rss_kb = 0;
        size_t vmlck_kb = 0;
        size_t vmdata_kb = 0;
        if (t->mm) {
            rss_kb = mm_rss_get(t->mm) * PAGE_SIZE / 1024;
            vmlck_kb = t->mm->locked_vm / 1024;
            mm_seg_t *vma = t->mm->mmap;
            while (vma) {
                if ((vma->vm_flags & VM_WRITE) && !(vma->vm_flags & VM_STACK)) {
                    vmdata_kb += (vma->end - vma->start) / 1024;
                }
                vma = vma->next;
            }
        }

        snprintf(buf, bufsz,
            "Name:\t%s\n"
            "Pid:\t%d\n"
            "PPid:\t%d\n"
            "PGid:\t%d\n"
            "Sid:\t%d\n"
            "Tgid:\t%d\n"
            "Ngid:\t0\n"
            "State:\t%s\n"
            "Uid:\t%d\t%d\t%d\t%d\n"
            "Gid:\t%d\t%d\t%d\t%d\n"
            "Groups:\t%s\n"
            "CapInh:\t%016lx\n"
            "CapPrm:\t%016lx\n"
            "CapEff:\t%016lx\n"
            "CapBnd:\t%016lx\n"
            "Threads:\t1\n"
            "Rss:\t%lu kB\n"
            "VmLck:\t%lu kB\n"
            "VmData:\t%lu kB\n",
            t->name, t->pid, t->ppid, t->pgid, t->sid, t->pid, state,
            t->cred.uid, t->cred.euid, t->cred.suid, t->cred.fsuid,
            t->cred.gid, t->cred.egid, t->cred.sgid, t->cred.fsgid,
            groups,
            (unsigned long)t->cred.cap_inheritable,
            (unsigned long)t->cred.cap_permitted,
            (unsigned long)t->cred.cap_effective,
            (unsigned long)t->cred.cap_bounding,
            (unsigned long)rss_kb,
            (unsigned long)vmlck_kb,
            (unsigned long)vmdata_kb);
        proc_put(t);
        break;
    }
    case PF_PID_STATM: {
        task_t *t = proc_find_get(pid);
        size_t total = t && t->mm ? t->mm->total_vm : 0;
        size_t rss = t ? mm_rss_get(t->mm) : 0;
        snprintf(buf, bufsz, "%lu %lu 0 0 0 0 0\n",
                 (unsigned long)total, (unsigned long)rss);
        proc_put(t);
        break;
    }
    case PF_PID_MAPS: {
        buf[0] = '\0';
        break;
    }
    case PF_PID_SMAPS: {
        buf[0] = '\0';
        break;
    }
    case PF_PID_OOM_SCORE_ADJ: {
        task_t *t = proc_find_get(pid);
        snprintf(buf, bufsz, "%d\n", t ? t->policy.oom_score_adj : 0);
        proc_put(t);
        break;
    }
    case PF_PID_OOM_SCORE: {
        task_t *t = proc_find_get(pid);
        snprintf(buf, bufsz, "%d\n", t ? (t->policy.oom_score_adj >= 0 ? t->policy.oom_score_adj : 0) : 0);
        proc_put(t);
        break;
    }
    case PF_PID_CGROUP:
        snprintf(buf, bufsz,
            "0::/init.scope\n");
        break;
    case PF_PID_CMDLINE: {
        task_t *t = proc_find_get(pid);
        if (!t || !t->exec_path[0]) {
            buf[0] = '\0';
            proc_put(t);
            return 1;
        }
        size_t len = strlen(t->exec_path);
        if (len + 1 > bufsz) len = bufsz - 1;
        memcpy(buf, t->exec_path, len);
        buf[len] = '\0';
        proc_put(t);
        return (int)(len + 1);
    }
    case PF_PID_COMM: {
        task_t *t = proc_find_get(pid);
        snprintf(buf, bufsz, "%s\n", t ? t->name : "unknown");
        proc_put(t);
        break;
    }
    case PF_PID_EXE: {
        task_t *t = proc_find_get(pid);
        snprintf(buf, bufsz, "%s\n", t && t->exec_path[0] ? t->exec_path : "/sbin/init");
        proc_put(t);
        break;
    }
    case PF_PID_CWD: {
        task_t *t = proc_find_get(pid);
        snprintf(buf, bufsz, "%s\n", t ? t->fs.cwd : "/");
        proc_put(t);
        break;
    }
    case PF_PID_ENVIRON:
        buf[0] = '\0';
        return 0;
    case PF_PID_IO: {
        task_t *t = proc_find_get(pid);
        unsigned long rchar = 0, wchar = 0, syscr = 0, syscw = 0;
        unsigned long rd_bytes = 0, wr_bytes = 0;
        if (t) {
            rchar = (unsigned long)__atomic_load_n(&t->io_rchar, __ATOMIC_RELAXED);
            wchar = (unsigned long)__atomic_load_n(&t->io_wchar, __ATOMIC_RELAXED);
            syscr = (unsigned long)__atomic_load_n(&t->io_syscr, __ATOMIC_RELAXED);
            syscw = (unsigned long)__atomic_load_n(&t->io_syscw, __ATOMIC_RELAXED);
            rd_bytes = (unsigned long)__atomic_load_n(&t->io_read_bytes, __ATOMIC_RELAXED);
            wr_bytes = (unsigned long)__atomic_load_n(&t->io_write_bytes, __ATOMIC_RELAXED);
            proc_put(t);
        }
        snprintf(buf, bufsz,
            "rchar: %lu\n"
            "wchar: %lu\n"
            "syscr: %lu\n"
            "syscw: %lu\n"
            "read_bytes: %lu\n"
            "write_bytes: %lu\n"
            "cancelled_write_bytes: 0\n",
            rchar, wchar, syscr, syscw, rd_bytes, wr_bytes);
        break;
    }
    case PF_PID_LOGINUID:
        snprintf(buf, bufsz, "4294967295\n");
        break;
    case PF_PID_SESSIONID: {
        task_t *t = proc_find_get(pid);
        snprintf(buf, bufsz, "%d\n", t ? t->sid : 0);
        proc_put(t);
        break;
    }
    case PF_PID_NS_PID: {
        /* Real pid namespaces: report the target task's own namespace.  A
         * task that is not visible from the reader's namespace renders as
         * the initial id, which is what the reader would have seen anyway
         * rather than leaking the container's identity. */
        task_t *t = proc_find_get(pid);
        snprintf(buf, bufsz, "pid:[%llu]\n",
                 (unsigned long long)(t ? pidns_task_ino(t) : PIDNS_INIT_INO));
        proc_put(t);
        break;
    }
    case PF_PID_NS_PID_FOR_CHILDREN: {
        /* The namespace this task's NEXT child joins, which is what
         * unshare(CLONE_NEWPID) changes and /proc/<pid>/ns/pid does not. */
        task_t *t = proc_find_get(pid);
        uint64_t ino = PIDNS_INIT_INO;
        if (t) {
            pid_namespace_t *ns = (pid_namespace_t *)__atomic_load_n(
                &t->pid_ns_for_children, __ATOMIC_ACQUIRE);
            ino = ns ? ns->ino : PIDNS_INIT_INO;
        }
        snprintf(buf, bufsz, "pid:[%llu]\n", (unsigned long long)ino);
        proc_put(t);
        break;
    }
    case PF_PID_NS_UTS:
        snprintf(buf, bufsz, "uts:[%llu]\n",
                 (unsigned long long)MNTNS_INIT_INO_UTS);
        break;
    case PF_PID_NS_USER: {
        /* Real user namespaces: report the target task's namespace ino, so a
         * container and the host are distinguishable by reading
         * /proc/<pid>/ns/user. */
        task_t *t = proc_find_get(pid);
        snprintf(buf, bufsz, "user:[%llu]\n",
                 (unsigned long long)userns_task_ino(t));
        proc_put(t);
        break;
    }
    case PF_PID_NS_IPC:
        snprintf(buf, bufsz, "ipc:[%llu]\n",
                 (unsigned long long)MNTNS_INIT_INO_IPC);
        break;
    case PF_PID_NS_MNT: {
        /* Real mount namespaces: report the target task's namespace ino. */
        task_t *t = proc_find_get(pid);
        snprintf(buf, bufsz, "mnt:[%llu]\n",
                 (unsigned long long)mntns_task_ino(t));
        proc_put(t);
        break;
    }
    case PF_PID_NS_NET:
        snprintf(buf, bufsz, "net:[%llu]\n",
                 (unsigned long long)MNTNS_INIT_INO_NET);
        break;
    case PF_PID_NS_CGROUP:
        snprintf(buf, bufsz, "cgroup:[%llu]\n",
                 (unsigned long long)MNTNS_INIT_INO_CGROUP);
        break;
    case PF_PID_MOUNTINFO: {
        buf[0] = '\0';
        int pos = 0;
        for (int i = 0; i < vfs_mount_count(); i++) {
            struct mount *m = vfs_mount_at(i);
            if (!m || !m->path[0]) continue;
            const char *fstype = m->fstype[0] ? m->fstype : "unknown";
            const char *dev = m->dev[0] ? m->dev : "none";
            const char *opts = m->opts[0] ? m->opts : "rw";
            /* Real mount ids and the real parent, straight from the mount
             * tree.  A mount pivot_root cut loose reports parent 0 and no
             * root, which is how mountinfo spells "unreachable". */
            char root_field[MAX_PATH_LEN];
            if (m->flags & VFS_MOUNT_DETACHED)
                strncpy(root_field, "none", sizeof(root_field) - 1);
            else
                strncpy(root_field, "/", sizeof(root_field) - 1);
            root_field[sizeof(root_field) - 1] = '\0';
            int n = snprintf(buf + pos, bufsz - pos,
                "%u %u 0:%u %s %s %s - %s %s %s\n",
                (unsigned)m->mnt_id, vfs_mount_parent_id(m),
                (unsigned)m->mnt_id, root_field, m->path, opts,
                fstype, dev, opts);
            if (n < 0 || (size_t)n >= bufsz - pos) break;
            pos += n;
        }
        break;
    }
    case PF_SYS_KERNEL_PID_MAX:
        snprintf(buf, bufsz, "%d\n", proc_pid_max());
        break;
    case PF_SYS_KERNEL_OSRELEASE:
        snprintf(buf, bufsz, "%s\n", A20OS_RELEASE);
        break;
    case PF_SYS_KERNEL_PIDMAP:
        proc_format_pidmap(buf, bufsz);
        break;
    case PF_SYS_KERNEL_TAINTED:
        snprintf(buf, bufsz, "0\n");
        break;
    case PF_SYS_KERNEL_SCHED_AUTOGROUP:
        snprintf(buf, bufsz, "0\n");
        break;
    case PF_SYS_KERNEL_CORE_PATTERN:
        coredump_get_pattern(buf, bufsz);
        break;
    case PF_SYS_KERNEL_IO_URING_DISABLED:
        snprintf(buf, bufsz, "0\n");
        break;
    case PF_SYS_FS_PIPE_MAX_SIZE:
        snprintf(buf, bufsz, "%d\n", g_procfs_pipe_max_size);
        break;
    case PF_SYS_FS_LEASE_BREAK_TIME:
        snprintf(buf, bufsz, "%d\n", g_procfs_lease_break_time);
        break;
    case PF_SYS_FS_INOTIFY_MAX_QUEUED_EVENTS:
        snprintf(buf, bufsz, "16384\n");
        break;
    case PF_SYS_FS_INOTIFY_MAX_USER_INSTANCES:
        snprintf(buf, bufsz, "128\n");
        break;
    case PF_SYS_VM_DROP_CACHES:
        snprintf(buf, bufsz, "0\n");
        break;
    case PF_INTERRUPTS:
#ifdef CONFIG_BOARD_LS2K1000
        snprintf(buf, bufsz,
            "           CPU0\n"
            "  0: %12lu   LS2K-LIOINTC   edge   ttyS0\n"
            "CAS: %12lu   LS2K HWI1 cascades\n"
            "SPU: %12lu   LS2K spurious cascades\n"
            "STM: %12lu   LS2K masked UART storms\n",
            (unsigned long)ls2k1000_irq_source_count(UART0_IRQ),
            (unsigned long)ls2k1000_irq_cascade_count(),
            (unsigned long)ls2k1000_irq_spurious_count(),
            (unsigned long)ls2k1000_irq_storm_count());
#else
        snprintf(buf, bufsz,
            "           CPU0\n"
            "  0:         %lu   IO-APIC   2-edge   timer\n"
            "  1:         %lu   IO-APIC   1-edge   i8042\n"
            "RES:         %lu   Rescheduling interrupts\n"
            "CAL:         %lu   Function call interrupts\n",
            (unsigned long)0, (unsigned long)0,
            (unsigned long)0, (unsigned long)0);
#endif
        break;
    case PF_SELF: {
        task_t *t = proc_current();
        snprintf(buf, bufsz, "%d\n", t ? t->pid : 0);
        break;
    }
    case PF_FSTYPE:
        snprintf(buf, bufsz, "nodev\tproc\nnodev\tcgroup\nnodev\tcgroup2\n\text4\n\tvfat\n\tramfs\n\ttmpfs\n");
        break;
    case PF_CGROUPS:
        snprintf(buf, bufsz,
            "#subsys_name\thierarchy\tnum_cgroups\tenabled\n"
            "cpuset\t1\t1\t1\n"
            "cpu\t1\t1\t1\n"
            "cpuacct\t1\t1\t1\n"
            "memory\t1\t1\t1\n");
        break;
    case PF_PID_PAGEMAP:
        buf[0] = '\0';
        return 0;
    case PF_BOOT_ID: {
        /* Linux /proc/boot_id: a stable per-boot UUID.  Derive it from the
         * boot clock so it is stable for the lifetime of the kernel. */
        uint64_t ticks = timer_get_ticks();
        char uuid[64];
        snprintf(uuid, sizeof(uuid),
                 "%08x-%04x-%04x-%04x-%08x%08x\n",
                 (unsigned)(ticks & 0xffffffff),
                 (unsigned)((ticks >> 32) & 0xffff),
                 (unsigned)((ticks >> 48) & 0xffff),
                 0x4000 | (unsigned)((ticks >> 16) & 0x0fff),
                 (unsigned)(ticks >> 32),
                 (unsigned)(ticks & 0xffffffff));
        snprintf(buf, bufsz, "%s", uuid);
        break;
    }
    case PF_CAP_LAST_CAP:
        snprintf(buf, bufsz, "%d\n", 63);
        break;
    case PF_NR_OPEN:
        snprintf(buf, bufsz, "%d\n", MAX_FILES);
        break;
    case PF_PRESSURE:
    case PF_PRESSURE_CPU:
    case PF_PRESSURE_MEM:
    case PF_PRESSURE_IO: {
        /* Linux exposes /proc/pressure as a directory of cpu|memory|io
         * files; systemd and pressure-stall tooling read those paths, so a
         * single flat file is not substitutable.  The parent carries the
         * cpu line so a whole-tree cat still shows something useful. */
        if (bufsz < 64)
            return 0;
        char tmp[64];
        switch (type) {
        case PF_PRESSURE_MEM: psi_render_mem(tmp, sizeof(tmp)); break;
        case PF_PRESSURE_IO:  psi_render_io(tmp, sizeof(tmp)); break;
        default:              psi_render_cpu(tmp, sizeof(tmp)); break;
        }
        int n = snprintf(buf, bufsz, "%s", tmp);
        return n < 0 ? 0 : n;
    }

    case PF_UID_MAP:
    case PF_GID_MAP: {
        /* Real mapping content, "<inside> <outside> <length>" per extent.
         * The initial namespace is the single identity extent, so this reads
         * exactly as before for a system with no user namespaces, and a
         * container shows the mapping its creator actually installed.
         *
         * pid <= 0 means the entry came from /proc/ rather than /proc/<pid>/:
         * Linux makes /proc/uid_map the caller's own file, so resolve it
         * against the reading task instead of against process 0. */
        task_t *t = (pid > 0) ? proc_find_get(pid) : proc_current();
        if (!t) return 0;
        user_namespace_t *ns = userns_task_own(t);
        uint64_t f = spin_lock_irqsave(&ns->lock);
        int is_gid = (type == PF_GID_MAP);
        const uid_gid_extent_t *map = is_gid ? ns->gid_map : ns->uid_map;
        int n = is_gid ? ns->gid_map_extents : ns->uid_map_extents;
        int off = 0;
        buf[0] = '\0';
        for (int i = 0; i < n && off < (int)bufsz - 1; i++) {
            int w = snprintf(buf + off, bufsz - (size_t)off,
                             "%10u %10u %10u\n",
                             (unsigned)map[i].lower,
                             (unsigned)map[i].parent_lower,
                             (unsigned)map[i].count);
            if (w < 0) break;
            off += w;
        }
        spin_unlock_irqrestore(&ns->lock, f);
        if (pid > 0) proc_put(t);
        return off;
    }
    case PF_SETGROUPS: {
        task_t *t = (pid > 0) ? proc_find_get(pid) : proc_current();
        if (!t) return 0;
        user_namespace_t *ns = userns_task_own(t);
        uint64_t f = spin_lock_irqsave(&ns->lock);
        int allowed = ns->setgroups_allowed;
        spin_unlock_irqrestore(&ns->lock, f);
        if (pid > 0) proc_put(t);
        snprintf(buf, bufsz, allowed ? "allow\n" : "deny\n");
        break;
    }
    case PF_SYSVIPC: {
        /* /proc/sysvipc/{msg,sem,shm} directory is represented as a summary
         * of live SysV objects.  We report counts via the IPC layers. */
        extern int sysv_msg_count(void);
        extern int sysv_sem_count(void);
        extern int sysv_shm_count(void);
        snprintf(buf, bufsz,
                 "msg\t%d sem\t%d shm\t%d\n",
                 sysv_msg_count(), sysv_sem_count(), sysv_shm_count());
        break;
    }
    case PF_SYS_KERNEL_HOSTNAME: {
        extern const char *linux_kernel_hostname(void);
        snprintf(buf, bufsz, "%s\n", linux_kernel_hostname());
        break;
    }
    case PF_SYS_KERNEL_DOMAINNAME: {
        extern const char *linux_kernel_domainname(void);
        snprintf(buf, bufsz, "%s\n", linux_kernel_domainname());
        break;
    }
    case PF_DEVICES: {
        /* Character and block device major numbers (Linux format). */
        snprintf(buf, bufsz,
                 "Character devices:\n"
                 "  1 mem\n  4 tty\n  5 /dev/tty\n  5 /dev/console\n"
                 "  10 misc\n  13 input\n  29 fb\n  254 ptmx\n\n"
                 "Block devices:\n  7 loop\n");
        break;
    }
    case PF_PARTITIONS: {
        /* major minor #blocks name */
        snprintf(buf, bufsz,
                 "major minor  #blocks  name\n\n");
        break;
    }
    case PF_DISKSTATS: {
        /* Linux /proc/diskstats: per-disk I/O statistics. */
        snprintf(buf, bufsz, "");
        break;
    }
    case PF_MODULES: {
        /* Loaded kernel modules (drvmod).  Report the framework modules. */
        extern int drvmod_list(char *buf, size_t sz);
        int n = drvmod_list(buf, bufsz);
        if (n < 0)
            return 0;
        break;
    }
    case PF_MISC: {
        /* /proc/misc: misc character devices (Linux format). */
        snprintf(buf, bufsz,
                 " 10 cpu_dma_latency\n 59 random\n");
        break;
    }
    case PF_IOMEM: {
        /* Physical memory map (Linux format). */
        snprintf(buf, bufsz,
                 "00000000-00000fff : reserved\n"
                 "00001000-0fffffff : System RAM\n"
                 "10000000-1fffffff : PCI Bus 0000:00\n");
        break;
    }
    case PF_IOPORTS: {
        snprintf(buf, bufsz,
                 "0000-0cf7 : PCI Bus 0000:00\n");
        break;
    }
    case PF_SOFTIRQS: {
        /* Per-CPU softirq counts. */
        snprintf(buf, bufsz,
                 "    HI: 0\n TIMER: 0\n NET_TX: 0\n NET_RX: 0\n"
                 " BLOCK: 0\n IRQ_POLL: 0\n TASKLET: 0\n SCHED: 0\n"
                 " HRTIMER: 0\n RCU: 0\n");
        break;
    }
    case PF_ARP: {
        /* ARP table header; no neighbours. */
        snprintf(buf, bufsz,
                 "IP address       HW type     Flags       HW address            Mask     Device\n");
        break;
    }
    case PF_TTY: {
        /* /proc/tty/driver/ style: one line per driver. */
        snprintf(buf, bufsz,
                 "driver               refcnt\n"
                 "pty_slave               0\n"
                 "pty_master              0\n"
                 "ttyS0                   0\n"
                 "/dev/console            0\n");
        break;
    }
    case PF_LDISCS: {
        /* Line disciplines. */
        snprintf(buf, bufsz,
                 "n_tty        0\n"
                 "n_gsm        5\n");
        break;
    }
    case PF_DRIVERS: {
        extern int drvmod_drivers(char *buf, size_t sz);
        int n = drvmod_drivers(buf, bufsz);
        if (n < 0)
            return 0;
        break;
    }
    case PF_PID_LIMITS: {
        task_t *t = proc_find_get(pid);
        if (!t)
            return -ESRCH;
        snprintf(buf, bufsz,
                 "Max open files             %5lu                %lu files\n"
                 "Max stack size                unlimited            unlimited bytes\n"
                 "Max locked memory       %5llu              %llu bytes\n",
                 (unsigned long)t->limits.nofile,
                 (unsigned long)t->limits.nofile,
                 (unsigned long long)t->limits.memlock,
                 (unsigned long long)t->limits.memlock);
        proc_put(t);
        break;
    }
    case PF_PID_WCHAN: {
        task_t *t = proc_find_get(pid);
        if (!t)
            return -ESRCH;
        /* Report "0" (running) or the wait state name. */
        const char *w = "0";
        if (t->state == PROC_BLOCKED)
            w = "do_wait";
        else if (t->state == PROC_STOPPED)
            w = "do_signal_stop";
        snprintf(buf, bufsz, "%s\n", w);
        proc_put(t);
        break;
    }
    case PF_PID_STACK: {
        task_t *t = proc_find_get(pid);
        if (!t)
            return -ESRCH;
        /* Kernel stack trace: report the saved return addresses if tracked,
         * otherwise an empty stack. */
        snprintf(buf, bufsz, "[<0000000000000000>] 0\n");
        proc_put(t);
        break;
    }
    default:
        break;
    }
    return (int)strlen(buf);
}

int generate_pid_fdinfo(int pid, int fd, char *buf, size_t bufsz)
{
    task_t *task = proc_find_get(pid);
    if (!task)
        return -ESRCH;
    if (!proc_task_may_access(proc_current(), task)) {
        proc_put(task);
        return -EACCES;
    }
    int cloexec = 0;
    vfile_t *target = fdtable_get_file_ref(task, fd, &cloexec);
    proc_put(task);
    if (!target)
        return -ENOENT;

    mutex_lock(&target->offset_lock);
    size_t pos = target->offset;
    mutex_unlock(&target->offset_lock);
    unsigned int open_flags = (unsigned int)target->flags;
    if (cloexec)
        open_flags |= O_CLOEXEC;
    unsigned long ino = target->vnode ?
        (unsigned long)target->vnode->ino : 0;
    int len = snprintf(buf, bufsz,
                       "pos:\t%lu\nflags:\t0%o\nino:\t%lu\n",
                       (unsigned long)pos, open_flags, ino);
    vfs_put_file(target);
    return len < 0 ? 0 : len;
}
