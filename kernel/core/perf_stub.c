/* ABI software counters remain independent of the optional measurement suite. */
#include "core/perf.h"
#include "core/stdio.h"
#include "core/errno.h"
uint64_t g_perf_sw_page_faults, g_perf_sw_page_faults_maj, g_perf_sw_context_switches;
size_t a20_perf_format(char *buffer, size_t size) {
    if (!size) return 0;
    int n = snprintf(buffer, size, "metrics_compiled: 0\n");
    return n < 0 ? 0 : (size_t)n >= size ? size - 1 : (size_t)n;
}
int a20_perf_control(const char *command, size_t length) {
    (void)command; (void)length; return -EOPNOTSUPP;
}
void a20_perf_reset(void) {}
