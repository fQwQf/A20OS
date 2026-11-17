#include "core/diagnostics.h"
#include "core/errno.h"
#include "core/report.h"
#include "core/lock.h"
#include "core/timer.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/klog.h"
#include "core/version.h"
#include "drivers/core/driver_core.h"
#include "mm/slab.h"

#define PHASES 6
static const char *const phase_names[] = {"reset", "handoff", "early", "memory", "devices", "ready"};
static const char *const result_names[] = {"untested", "running", "pass", "fail", "unsupported", "degraded", "deferred"};
typedef struct diagnostic_record {
    uint64_t id, start, elapsed;
    unsigned cpu, result, clock_valid;
    int error;
    char category[24], subject[48], detail[48];
} diagnostic_record_t;
typedef struct phase_record { uint64_t start, elapsed; unsigned result, clock_valid; } phase_record_t;
static spinlock_t diag_lock = SPINLOCK_INIT;
static diagnostic_record_t records[CONFIG_A20_DIAG_RECORDS];
static phase_record_t phases[PHASES];
static uint64_t next_id = 1, overwritten, dropped, late_finishes;
static unsigned retained, cursor, current_phase;
static int initialized;

typedef struct porting_snapshot {
    phase_record_t phases[PHASES];
    uint64_t overwritten, dropped, late_finishes, now;
    unsigned retained;
    diagnostic_record_t records[];
} porting_snapshot_t;

void a20_diag_init(unsigned phase) {
    spin_init(&diag_lock);
    current_phase = phase < PHASES ? phase : 0;
    phases[current_phase].start = timer_get_ticks();
    phases[current_phase].result = A20_DIAG_RUNNING;
    phases[current_phase].clock_valid = 1;
    initialized = 1;
}
void a20_diag_phase(unsigned phase) {
    if (!initialized || phase >= PHASES) return;
    uint64_t now = timer_get_ticks();
    uint64_t flags = spin_lock_irqsave(&diag_lock);
    if (phase != current_phase) {
        phase_record_t *previous = &phases[current_phase];
        previous->clock_valid = now >= previous->start;
        previous->elapsed = previous->clock_valid ? now - previous->start : 0;
        previous->result = A20_DIAG_PASS;
        phases[phase].start = now;
        phases[phase].result = A20_DIAG_RUNNING;
        phases[phase].clock_valid = 1;
        current_phase = phase;
    }
    /* READY is a reached checkpoint, not a service or hardware health test. */
    if (phase == PHASES - 1) phases[phase].result = A20_DIAG_PASS;
    spin_unlock_irqrestore(&diag_lock, flags);
    klog_event(KLOG_INFO, "porting", "checkpoint phase=%s\n", phase_names[phase]);
}
uint64_t a20_diag_begin(const char *category, const char *subject, const char *detail) {
    if (!initialized) return 0;
    diagnostic_record_t record = {.start = timer_get_ticks(), .result = A20_DIAG_RUNNING, .clock_valid = 1,
                                  .cpu = arch_current_cpu_id()};
    snprintf(record.category, sizeof(record.category), "%s", category ? category : "check");
    snprintf(record.subject, sizeof(record.subject), "%s", subject ? subject : "unknown");
    snprintf(record.detail, sizeof(record.detail), "%s", detail ? detail : "");
    uint64_t flags = spin_lock_irqsave(&diag_lock);
    unsigned slot = CONFIG_A20_DIAG_RECORDS;
    for (unsigned i = 0; i < CONFIG_A20_DIAG_RECORDS; i++) {
        unsigned candidate = (cursor + i) % CONFIG_A20_DIAG_RECORDS;
        if (records[candidate].result != A20_DIAG_RUNNING) { slot = candidate; break; }
    }
    if (slot == CONFIG_A20_DIAG_RECORDS) {
        dropped++;
        spin_unlock_irqrestore(&diag_lock, flags);
        return 0;
    }
    record.id = next_id++;
    if (!next_id) next_id = 1;
    if (records[slot].id) overwritten++;
    else retained++;
    records[slot] = record;
    cursor = (slot + 1) % CONFIG_A20_DIAG_RECORDS;
    spin_unlock_irqrestore(&diag_lock, flags);
    klog_event(KLOG_INFO, "porting", "begin id=%llu category=%s subject=%s detail=%s\n",
               (unsigned long long)record.id, record.category, record.subject, record.detail);
    return record.id;
}
void a20_diag_end(uint64_t id, enum a20_diag_result result, int error) {
    if (!id || result < A20_DIAG_UNTESTED || result == A20_DIAG_RUNNING || result > A20_DIAG_DEFERRED) return;
    uint64_t now = timer_get_ticks();
    uint64_t flags = spin_lock_irqsave(&diag_lock);
    for (unsigned i = 0; i < CONFIG_A20_DIAG_RECORDS; i++) {
        diagnostic_record_t *record = &records[i];
        if (record->id == id && record->result == A20_DIAG_RUNNING) {
            record->clock_valid = now >= record->start;
            record->elapsed = record->clock_valid ? now - record->start : 0;
            record->result = result;
            record->error = error;
            spin_unlock_irqrestore(&diag_lock, flags);
            return;
        }
    }
    late_finishes++;
    spin_unlock_irqrestore(&diag_lock, flags);
}
static porting_snapshot_t *snapshot(void) {
    size_t bytes = sizeof(porting_snapshot_t) + sizeof(records);
    porting_snapshot_t *out = kmalloc(bytes);
    if (!out) return NULL;
    uint64_t flags = spin_lock_irqsave(&diag_lock);
    memcpy(out->phases, phases, sizeof(phases));
    out->overwritten = overwritten; out->dropped = dropped; out->late_finishes = late_finishes;
    out->retained = 0; out->now = timer_get_ticks();
    for (unsigned i = 0; i < CONFIG_A20_DIAG_RECORDS; i++)
        if (records[i].id) out->records[out->retained++] = records[i];
    spin_unlock_irqrestore(&diag_lock, flags);
    for (unsigned i = 1; i < out->retained; i++) {
        diagnostic_record_t record = out->records[i];
        unsigned j = i;
        while (j && out->records[j - 1].id > record.id) { out->records[j] = out->records[j - 1]; j--; }
        out->records[j] = record;
    }
    return out;
}
int a20_diag_format(char *buffer, size_t capacity) {
    if (!buffer || !capacity) return -EINVAL;
    porting_snapshot_t *data = snapshot();
    if (!data) return -ENOMEM;
    report_writer_t w = {.buffer = buffer, .capacity = capacity};
    report_printf(&w, "{\"schema\":1,\"kind\":\"porting\",\"boot\":");
    report_json_string(&w, klog_boot_id());
    report_printf(&w, ",\"kernel\":"); report_json_string(&w, A20OS_VERSION);
    report_printf(&w, ",\"board\":"); report_json_string(&w, current_board ? current_board->name : "unknown");
    report_printf(&w, ",\"ticks_per_second\":%llu,\"capacity\":%u,\"retained\":%u,\"overwritten\":%llu,\"dropped\":%llu,\"late_finishes\":%llu}\n",
                  (unsigned long long)TICKS_PER_SEC, CONFIG_A20_DIAG_RECORDS, data->retained,
                  (unsigned long long)data->overwritten, (unsigned long long)data->dropped,
                  (unsigned long long)data->late_finishes);
    for (unsigned i = 0; i < PHASES; i++) {
        phase_record_t *phase = &data->phases[i];
        uint64_t elapsed = phase->result == A20_DIAG_RUNNING && data->now >= phase->start ? data->now - phase->start : phase->elapsed;
        report_printf(&w, "{\"kind\":\"phase\",\"name\":\"%s\",\"state\":\"%s\",\"elapsed_ticks\":%llu,\"clock_valid\":%u}\n",
                      phase_names[i], result_names[phase->result], (unsigned long long)elapsed,
                      phase->clock_valid && (phase->result != A20_DIAG_RUNNING || data->now >= phase->start));
    }
    for (unsigned i = 0; i < data->retained; i++) {
        diagnostic_record_t *record = &data->records[i];
        uint64_t elapsed = record->result == A20_DIAG_RUNNING && data->now >= record->start ? data->now - record->start : record->elapsed;
        report_printf(&w, "{\"kind\":\"check\",\"id\":%llu,\"category\":", (unsigned long long)record->id);
        report_json_string(&w, record->category);
        report_printf(&w, ",\"subject\":"); report_json_string(&w, record->subject);
        report_printf(&w, ",\"detail\":"); report_json_string(&w, record->detail);
        report_printf(&w, ",\"cpu\":%u,\"state\":\"%s\",\"error\":%d,\"elapsed_ticks\":%llu,\"clock_valid\":%u}\n",
                      record->cpu, result_names[record->result], record->error, (unsigned long long)elapsed,
                      record->clock_valid && (record->result != A20_DIAG_RUNNING || data->now >= record->start));
    }
    report_printf(&w, "{\"kind\":\"end\",\"checks\":%u,\"complete\":true}\n", data->retained);
    kfree(data);
    return w.error == -ENOSPC ? (int)(capacity - 1) : w.error ? w.error : (int)w.length;
}
int a20_diag_summary(struct report_writer *writer) {
    porting_snapshot_t *data = snapshot();
    if (!data) return -ENOMEM;
    unsigned running = 0, failed = 0, shown = 0;
    for (unsigned i = 0; i < data->retained; i++) {
        diagnostic_record_t *record = &data->records[i];
        running += record->result == A20_DIAG_RUNNING;
        failed += record->result == A20_DIAG_FAIL;
        if (record->result == A20_DIAG_RUNNING && shown++ < 8)
            report_printf(writer, "porting: running %s subject=%s detail=%s elapsed_ticks=%llu\n",
                          record->category, record->subject, record->detail,
                          (unsigned long long)(data->now >= record->start ? data->now - record->start : 0));
    }
    report_printf(writer, "porting: retained=%u running=%u failed_attempts=%u overwritten=%llu dropped=%llu (probe success is not functional readiness)\n",
                  data->retained, running, failed, (unsigned long long)data->overwritten, (unsigned long long)data->dropped);
    kfree(data);
    return 0;
}
