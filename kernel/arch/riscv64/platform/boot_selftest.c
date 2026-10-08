#include "core/bootargs.h"
#include "core/panic.h"
#include "core/stdio.h"
#include "core/string.h"

int riscv64_trap_t0_selftest(void);
int riscv64_sched_park_yield_selftest(void);

void arch_run_bootarg_selftests(void)
{
    const char *args = bootargs_get();
    if (args && strstr(args, "a20.trap_t0_selftest=1")) {
        if (riscv64_trap_t0_selftest())
            printf("RV64_TRAP_T0: PASS (timer IRQ preserved t0)\n");
        else
            panic("RV64_TRAP_T0: FAIL (timer IRQ missing or t0 changed)");
    }
    if (args && strstr(args, "a20.sched_park_yield_selftest=1")) {
        if (riscv64_sched_park_yield_selftest())
            panic("RV64_SCHED_PARK_YIELD: FAIL");
        printf("RV64_SCHED_PARK_YIELD: PASS (PREPARING survived yield and wake)\n");
    }
}
