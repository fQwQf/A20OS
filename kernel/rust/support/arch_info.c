#include "core/types.h"

#if defined(RISCV64)
#include "arch/riscv64/include/platform.h"
#elif defined(LOONGARCH64)
#include "arch/loongarch64/include/platform.h"
#elif defined(AARCH64)
#include "arch/aarch64/include/platform.h"
#elif defined(X86_64)
#include "arch/x86_64/include/platform.h"
#endif

uint64_t a20_arch_timer_freq(void)
{
    return ARCH_TIMER_FREQ;
}
