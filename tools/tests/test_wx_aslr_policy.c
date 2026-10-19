/*
 * Host differential harness for kernel/mm/wx_policy.rs and
 * kernel/mm/aslr_policy.rs.
 *
 * The production Rust modules are linked against these stubs, which supply the
 * entropy source, the cmdline and the task/layout facts the kernel would
 * provide.  Every assertion is about a policy decision the Rust module owns:
 * which cmdline tokens are recognised, what an unrecognised or truncated value
 * does, which W|X requests are stripped, refused or exempted, and which bound
 * each layout offset is capped by.
 *
 * This is a decision oracle, not a kernel; the guest gate that runs the real
 * wx_aslr_test is what establishes guest behavior.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4

#define EACCES 13

#define MMAP_BASE_ADDR 0x60000000UL
#define USER_TLS_BASE 0x3E000000UL
#define PAGE_SIZE 4096UL

static int failures;
static int checks;

static void check(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

/* ------------------------------------------------------------- stubs ----- */

static char g_cmdline[512];
static uint64_t g_rand;
static int g_xlator_host;
static int g_pid = 4242;

static int selected_calls;
static int selected_policy = -1;
static int unknown_calls;
static char unknown_value[64];
static int exempt_calls;
static int refused_calls;
static int refused_policy = -1;
static char refused_ctx[32];

const char *bootargs_get(void) { return g_cmdline; }
uint64_t random_u64(void) { return g_rand; }

int a20_wx_task_is_xlator_host(void) { return g_xlator_host; }
int a20_wx_current_pid(void) { return g_pid; }

void a20_wx_log_selected(int policy) { selected_calls++; selected_policy = policy; }
void a20_wx_log_unknown(const char *value)
{
    unknown_calls++;
    snprintf(unknown_value, sizeof(unknown_value), "%s", value ? value : "(null)");
}
void a20_wx_log_exempt(const char *ctx, int pid)
{
    (void)ctx;
    (void)pid;
    exempt_calls++;
}
void a20_wx_log_refused(int policy, const char *ctx, int pid)
{
    (void)pid;
    refused_calls++;
    refused_policy = policy;
    snprintf(refused_ctx, sizeof(refused_ctx), "%s", ctx ? ctx : "?");
}

static int g_aslr_nommu;
uintptr_t a20_aslr_mmap_base_addr(void) { return MMAP_BASE_ADDR; }
uintptr_t a20_aslr_user_tls_base(void) { return USER_TLS_BASE; }
int a20_aslr_is_nommu(void) { return g_aslr_nommu; }

/* ------------------------------------------------- production entry points */

extern void mm_wx_policy_init(void);
extern int mm_wx_filter_prot(int prot, const char *ctx);
extern unsigned long mm_aslr_mmap_base(void);
extern unsigned long mm_aslr_stack_offset(void);
extern unsigned long mm_aslr_brk_offset(unsigned long brk_base);

static void reset(void)
{
    g_cmdline[0] = '\0';
    g_rand = ~(uint64_t)0;
    g_xlator_host = 0;
    g_aslr_nommu = 0;
    selected_calls = unknown_calls = exempt_calls = refused_calls = 0;
    selected_policy = refused_policy = -1;
    unknown_value[0] = refused_ctx[0] = '\0';
}

/* ---------------------------------------------------------------- W^X ---- */

static void wx_only_touches_wx_requests(void)
{
    reset();
    check(mm_wx_filter_prot(PROT_READ | PROT_WRITE, "mmap") ==
              (PROT_READ | PROT_WRITE),
          "wx/off: a writable mapping is untouched");
    check(mm_wx_filter_prot(PROT_READ | PROT_EXEC, "mmap") ==
              (PROT_READ | PROT_EXEC),
          "wx/off: an executable mapping is untouched");
    check(refused_calls == 0, "wx/off: nothing is refused");
    check(exempt_calls == 0, "wx/off: nothing is exempted");
}

static void wx_cmdline_contract(void)
{
    reset();
    snprintf(g_cmdline, sizeof(g_cmdline),
             "quiet a20.wx=strip splash a20.wx=deny");
    mm_wx_policy_init();
    check(selected_calls == 1 && selected_policy == 2,
          "wx: the last token wins and is reported once");
    check(unknown_calls == 0, "wx: recognised tokens are not reported as unknown");
    check(mm_wx_filter_prot(PROT_READ | PROT_WRITE | PROT_EXEC, "mprotect") == -EACCES,
          "wx/deny: a W|X request is refused with EACCES");
    check(refused_policy == 2, "wx/deny: the refusal is reported as deny");
    check(strcmp(refused_ctx, "mprotect") == 0, "wx/deny: the refusal names its caller");

    reset();
    snprintf(g_cmdline, sizeof(g_cmdline), "a20.wx=strip");
    mm_wx_policy_init();
    check(selected_policy == 1, "wx/strip: the policy is selected");
    check(mm_wx_filter_prot(PROT_READ | PROT_WRITE | PROT_EXEC, "mmap") ==
              (PROT_READ | PROT_EXEC),
          "wx/strip: the writable bit is stripped and exec kept");
    check(refused_policy == 1, "wx/strip: the strip is reported as strip");

    /* An unrecognised value must not look like the option took effect. */
    reset();
    snprintf(g_cmdline, sizeof(g_cmdline), "a20.wx=bogus");
    mm_wx_policy_init();
    check(selected_policy == 0, "wx: an unknown value keeps the default");
    check(unknown_calls == 1 && strcmp(unknown_value, "bogus") == 0,
          "wx: the unknown value is reported verbatim");

    /* The key without '=' is not this option at all. */
    reset();
    snprintf(g_cmdline, sizeof(g_cmdline), "a20.wx a20.wxquiet=deny");
    mm_wx_policy_init();
    check(unknown_calls == 0, "wx: a token without '=' is not an option value");
    check(selected_policy == 0, "wx: a token without '=' leaves the default");

    /* An empty value is reported rather than silently ignored. */
    reset();
    snprintf(g_cmdline, sizeof(g_cmdline), "a20.wx=");
    mm_wx_policy_init();
    check(unknown_calls == 1 && strcmp(unknown_value, "") == 0,
          "wx: an empty value is reported as unknown");

    /* A value longer than the 15-byte buffer is truncated, not overrun. */
    reset();
    snprintf(g_cmdline, sizeof(g_cmdline), "a20.wx=0123456789abcdefghij");
    mm_wx_policy_init();
    check(unknown_calls == 1 && strcmp(unknown_value, "0123456789abcde") == 0,
          "wx: an over-long value is truncated at 15 bytes");

    /* A cmdline with no option at all keeps the default and logs it. */
    reset();
    snprintf(g_cmdline, sizeof(g_cmdline), "root=/dev/vda console=ttyS0");
    mm_wx_policy_init();
    check(selected_calls == 1 && selected_policy == 0,
          "wx: an unrelated cmdline selects the default");
}

static void wx_translator_exemption(void)
{
    reset();
    snprintf(g_cmdline, sizeof(g_cmdline), "a20.wx=deny");
    mm_wx_policy_init();
    g_xlator_host = 1;
    check(mm_wx_filter_prot(PROT_READ | PROT_WRITE | PROT_EXEC, "mmap") ==
              (PROT_READ | PROT_WRITE | PROT_EXEC),
          "wx/deny: the translator host keeps its W|X request");
    check(exempt_calls == 1, "wx/deny: the exemption is reported");
    check(refused_calls == 0, "wx/deny: the exempted request is not refused");

    /* The exemption is per request, not a policy change. */
    g_xlator_host = 0;
    check(mm_wx_filter_prot(PROT_READ | PROT_WRITE | PROT_EXEC, "mmap") == -EACCES,
          "wx/deny: a non-translator task is still refused");
}

/* --------------------------------------------------------------- ASLR ---- */

static void aslr_bounds(void)
{
    /* All ones exercises every mask's full width. */
    reset();
    unsigned long mmap_addr = mm_aslr_mmap_base();
    check(mmap_addr == MMAP_BASE_ADDR + 0xFFFFFUL * PAGE_SIZE,
          "aslr: the mmap window is 20 bits wide");
    check(mmap_addr % PAGE_SIZE == 0, "aslr: the mmap offset is page aligned");

    unsigned long stack_off = mm_aslr_stack_offset();
    check(stack_off == 0x3FFUL * PAGE_SIZE,
          "aslr: the stack window is 10 bits wide");

    /* The brk cap is min(space to the TLS block, 13 bits).  With 8192 and
     * unlimited room the honest answer is 8192 % 8191 == 1 page; without the
     * 13-bit cap it would be 8192 % 253951 == 8192 pages, so this proves the
     * cap rather than the space. */
    reset();
    g_rand = 8192;
    unsigned long brk_off = mm_aslr_brk_offset(0x1000);
    check(brk_off == PAGE_SIZE, "aslr: the brk offset is capped by the 13-bit budget");

    /* Symmetrically, a brk close to the TLS block is capped by the space. */
    reset();
    g_rand = 100000;
    brk_off = mm_aslr_brk_offset(USER_TLS_BASE - 100 * PAGE_SIZE);
    check(brk_off % PAGE_SIZE == 0 && brk_off / PAGE_SIZE < 100,
          "aslr: the brk offset is capped by the space to the TLS block");

    /* No room at all means no offset, and no division by zero. */
    reset();
    check(mm_aslr_brk_offset(USER_TLS_BASE) == 0, "aslr: a brk at the TLS block gets no offset");
    check(mm_aslr_brk_offset(USER_TLS_BASE + PAGE_SIZE) == 0,
          "aslr: a brk past the TLS block gets no offset");
}

static void aslr_nommu_is_flat(void)
{
    reset();
    g_aslr_nommu = 1;
    check(mm_aslr_mmap_base() == MMAP_BASE_ADDR, "aslr/nommu: the mmap base is the constant");
    check(mm_aslr_stack_offset() == 0, "aslr/nommu: the stack offset is zero");
    check(mm_aslr_brk_offset(0x1000) == 0, "aslr/nommu: the brk offset is zero");
}

int main(void)
{
    wx_only_touches_wx_requests();
    wx_cmdline_contract();
    wx_translator_exemption();
    aslr_bounds();
    aslr_nommu_is_flat();
    reset();

    if (failures) {
        printf("WX_ASLR_POLICY_HOST: FAIL %d/%d checks\n", failures, checks);
        return 1;
    }
    printf("WX_ASLR_POLICY_HOST: PASS %d checks\n", checks);
    return 0;
}
