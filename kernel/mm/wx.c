/*
 * A20OS — user-space W^X policy
 *
 * No user VMA may be both writable and executable. Every entry point
 * (mmap/mmap_file/mmap_vmo, mprotect, ELF PT_LOAD loading) goes through
 * mm_wx_filter_prot():
 *   - off (default): no intervention, which is what a stock Linux kernel
 *     does -- both mmap and mprotect honour PROT_WRITE|PROT_EXEC, and W^X is a
 *     hardening *option* rather than a mandate;
 *   - strip: warn and strip the writable bit, downgrading the mapping to
 *     read-only + executable;
 *   - deny: refuse a W|X combination and return -EACCES, following the Linux
 *     security-module semantics.
 *
 * The policy is selected on the kernel cmdline: a20.wx=off|strip|deny.
 *
 * The default used to be deny, justified here by the claim that "nothing in
 * this tree's user space needs RWX -- there is no dlopen and no JIT".
 * nodejs falsified that claim in one line: V8 mprotects its code pages W|X
 * and then *writes* to them, so strip does not rescue it either -- the run
 * segfaults on the first store instead of trapping at the mprotect.  A kernel
 * that stops V8 at startup does not have a stricter policy, it cannot run a
 * JIT, and the premise behind the default was wrong rather than the policy.
 *
 * This is a statement about W^X, not about the memory model: node's failure
 * under deny reproduces identically on the pre-migration code.
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

/* Vanilla-Linux semantics: honour what the mapping asks for, and let an image
 * that wants the guarantee opt into deny or strip on the cmdline. */
static int g_wx_policy = MM_WX_OFF;

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
                kwarn("[WX] 未知 a20.wx='%s'，保持默认 off\n", val);
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
