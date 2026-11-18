#ifndef A20_CORE_DIAGNOSTICS_H
#define A20_CORE_DIAGNOSTICS_H
#include "core/types.h"
#ifndef CONFIG_A20_DIAG_BOOT
#define CONFIG_A20_DIAG_BOOT 0
#endif
#ifndef CONFIG_A20_DIAG_RECORDS
#define CONFIG_A20_DIAG_RECORDS 128
#endif
#if CONFIG_A20_DIAG_RECORDS < 16 || CONFIG_A20_DIAG_RECORDS > 1024
#error "DIAG_RECORDS must be 16..1024"
#endif
/* Results describe an explicit check or one driver probe attempt. A successful
 * probe does not prove working DMA/interrupts or a passing functional test. */
enum a20_diag_result {
    A20_DIAG_UNTESTED, A20_DIAG_RUNNING, A20_DIAG_PASS,
    A20_DIAG_FAIL, A20_DIAG_UNSUPPORTED, A20_DIAG_DEGRADED, A20_DIAG_DEFERRED
};
struct report_writer;
#if CONFIG_A20_DIAG_BOOT
void a20_diag_init(unsigned phase);
void a20_diag_phase(unsigned phase);
uint64_t a20_diag_begin(const char *category, const char *subject, const char *detail);
void a20_diag_end(uint64_t id, enum a20_diag_result result, int error);
int a20_diag_format(char *buffer, size_t capacity);
int a20_diag_summary(struct report_writer *writer);
#else
static inline void a20_diag_init(unsigned phase) { (void)phase; }
static inline void a20_diag_phase(unsigned phase) { (void)phase; }
static inline uint64_t a20_diag_begin(const char *category, const char *subject, const char *detail) {
    (void)category; (void)subject; (void)detail; return 0;
}
static inline void a20_diag_end(uint64_t id, enum a20_diag_result result, int error) {
    (void)id; (void)result; (void)error;
}
#endif
#endif
