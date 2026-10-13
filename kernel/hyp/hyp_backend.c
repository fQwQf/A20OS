#include "hyp/hyp_backend.h"
/* Each implemented architecture overrides this. Unsupported architectures
 * remain unavailable to both the Native and Linux adapters. */
__attribute__((weak)) const hyp_backend_t *hyp_arch_backend(void)
{
    return (const hyp_backend_t *)0;
}
