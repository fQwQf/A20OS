/*
 * A20OS — user-space W^X policy.
 *
 * The policy state, the cmdline contract and the per-call decision live in
 * kernel/mm/wx_policy.rs; mm_wx_policy_init() and mm_wx_filter_prot() are
 * exported from there.  What is left here is what Rust cannot name: the task
 * field the translator exemption is keyed on, the current pid, and the four
 * log lines.
 *
 * Keeping W^X a cmdline option rather than a mandate is a deliberate Linux
 * -compatibility choice: V8 mprotects its code pages W|X and then writes to
 * them, so a mandatory `deny` would stop node from starting instead of making
 * the kernel harder to break.
 */

#include "mm/vm.h"
#include "core/klog.h"
#include "proc/proc.h"

#define MM_WX_OFF    0
#define MM_WX_STRIP  1
#define MM_WX_DENY   2

/* Per-process, set by execve: the foreign-architecture translator JITs guest
 * code and needs an RWX buffer whatever the policy is.  Nothing else can set
 * the bit, so this does not weaken W^X for the rest of the machine. */
int a20_wx_task_is_xlator_host(void)
{
#ifdef CONFIG_XLATOR
    task_t *cur = proc_current();
    return cur && cur->xlator_host;
#else
    return 0;
#endif
}

int a20_wx_current_pid(void)
{
    task_t *cur = proc_current();
    return cur ? cur->pid : -1;
}

void a20_wx_log_selected(int policy)
{
    static const char *const names[] = { "off", "strip", "deny" };
    kinfo("[WX] 用户态 W^X 策略: %s\n",
          (policy >= MM_WX_OFF && policy <= MM_WX_DENY) ? names[policy] : "?");
}

void a20_wx_log_unknown(const char *value)
{
    kwarn("[WX] 未知 a20.wx='%s'，保持默认 off\n", value ? value : "?");
}

void a20_wx_log_exempt(const char *ctx, int pid)
{
    kinfo("[WX] %s: pid=%d 翻译器宿主，放行 W|X\n", ctx ? ctx : "?", pid);
}

void a20_wx_log_refused(int policy, const char *ctx, int pid)
{
    if (policy == MM_WX_STRIP)
        kwarn("[WX] %s: pid=%d 请求 W|X 映射，已剥离可写位\n",
              ctx ? ctx : "?", pid);
    else
        kwarn("[WX] %s: pid=%d 请求 W|X 映射，拒绝 (-EACCES)\n",
              ctx ? ctx : "?", pid);
}
