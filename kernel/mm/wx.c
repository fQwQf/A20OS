/*
 * A20OS — user-space W^X policy
 *
 * No user VMA may be both writable and executable. Every entry point
 * (mmap/mmap_file/mmap_vmo, mprotect, ELF PT_LOAD loading) goes through
 * mm_wx_filter_prot():
 *   - deny (default): refuse a W|X combination and return -EACCES, following
 *     the Linux security-module semantics;
 *   - strip: warn and strip the writable bit, downgrading the mapping to
 *     read-only + executable;
 *   - off: no intervention (debug/compatibility fallback only).
 *
 * The policy is selected on the kernel cmdline: a20.wx=deny|strip|off.
 * Nothing in this tree's own user space (static musl programs, the in-tree
 * cmds, native svc) needs RWX -- there is no dlopen and no JIT -- so the
 * strictest policy, deny, is the default.  The single exception is a task
 * that execve re-execed through a foreign-architecture translator
 * (task_t.xlator_host), which must JIT guest code; that exemption is keyed
 * on the task, not on the policy, so it does not weaken W^X for anything
 * else on the machine.  It exists only in a CONFIG_XLATOR build; without
 * that knob there is nothing to exempt.
 */

#include "mm/vm.h"
#include "core/bootargs.h"
#include "core/string.h"
#include "core/klog.h"
#include "core/errno.h"
#include "proc/proc.h"

#define MM_WX_OFF    0
#define MM_WX_STRIP  1
#define MM_WX_DENY   2

static int g_wx_policy = MM_WX_DENY;

/* Same cmdline token scanning style as net/net_config.c */
static const char *wx_extract_value(const char *tok, const char *tok_end,
                                    const char *key, char *val, size_t valsz)
{
    size_t klen = strlen(key);
    if ((size_t)(tok_end - tok) < klen + 1)
        return NULL;
    if (strncmp(tok, key, klen) != 0 || tok[klen] != '=')
        return NULL;

    const char *vstart = tok + klen + 1;
    size_t vlen = (size_t)(tok_end - vstart);
    if (vlen >= valsz)
        vlen = valsz - 1;
    memcpy(val, vstart, vlen);
    val[vlen] = '\0';
    return tok_end;
}

void mm_wx_policy_init(void)
{
    const char *cmdline = bootargs_get();

    char val[16];
    const char *p = cmdline;
    while (p && *p) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        const char *tok_end = p;
        while (*tok_end && *tok_end != ' ' && *tok_end != '\t')
            tok_end++;

        if (wx_extract_value(p, tok_end, "a20.wx", val, sizeof(val))) {
            if (strcmp(val, "off") == 0)
                g_wx_policy = MM_WX_OFF;
            else if (strcmp(val, "strip") == 0)
                g_wx_policy = MM_WX_STRIP;
            else if (strcmp(val, "deny") == 0)
                g_wx_policy = MM_WX_DENY;
            else
                kwarn("[WX] 未知 a20.wx='%s'，保持默认 deny\n", val);
        }

        p = tok_end;
    }

    static const char *const names[] = { "off", "strip", "deny" };
    kinfo("[WX] 用户态 W^X 策略: %s\n", names[g_wx_policy]);
}

int mm_wx_filter_prot(int prot, const char *ctx)
{
    if ((prot & (PROT_WRITE | PROT_EXEC)) != (PROT_WRITE | PROT_EXEC))
        return prot;

    task_t *cur = proc_current();
    int pid = cur ? cur->pid : -1;

    #ifdef CONFIG_XLATOR
    /* The foreign-architecture translator JITs guest code, which requires
     * an RWX buffer.  execve marks exactly that task (task_t.xlator_host)
     * when it re-execs it, and nothing else can set the bit -- so this
     * exemption is per-process and does not require weakening the policy
     * for the whole system the way a20.wx=off does. */
    if (cur && cur->xlator_host) {
        kinfo("[WX] %s: pid=%d 翻译器宿主，放行 W|X\n", ctx ? ctx : "?", pid);
        return prot;
    }
#endif

    switch (g_wx_policy) {
    case MM_WX_OFF:
        return prot;
    case MM_WX_STRIP:
        kwarn("[WX] %s: pid=%d 请求 W|X 映射，已剥离可写位\n",
              ctx ? ctx : "?", pid);
        return prot & ~PROT_WRITE;
    default:
        kwarn("[WX] %s: pid=%d 请求 W|X 映射，拒绝 (-EACCES)\n",
              ctx ? ctx : "?", pid);
        return -EACCES;
    }
}
