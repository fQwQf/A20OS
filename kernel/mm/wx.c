/*
 * A20OS — 用户态 W^X 策略
 *
 * 任何用户 VMA 不得同时可写且可执行。所有入口（mmap/mmap_file/mmap_vmo、
 * mprotect、ELF PT_LOAD 装载）统一经过 mm_wx_filter_prot()：
 *   - deny（默认）：拒绝 W|X 组合，返回 -EACCES（类 Linux 安全模块语义）；
 *   - strip：告警并剥离可写位，映射降级为只读+可执行；
 *   - off：不干预（仅调试/兼容性兜底）。
 *
 * 策略通过内核 cmdline 选择：a20.wx=deny|strip|off。
 * 仓库用户态（musl 静态程序、自研 cmds、native svc）均无 RWX 需求
 * （无 dlopen/JIT），因此默认最严格的 deny。
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

/* 与 net/net_config.c 相同的 cmdline token 扫描风格 */
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
