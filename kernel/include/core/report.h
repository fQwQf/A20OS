#ifndef A20_CORE_REPORT_H
#define A20_CORE_REPORT_H
#include "core/types.h"

/* A report owns its output. Providers run in process context, outside logging
 * locks; they must use subsystem snapshot APIs, never live private globals. */
typedef struct report_writer {
    char *buffer;
    size_t capacity, length;
    int error;
} report_writer_t;
typedef struct report_section {
    const char *name;
    int (*render)(report_writer_t *writer, const void *snapshot);
} report_section_t;
void report_json_string(report_writer_t *writer, const char *text);
void report_printf(report_writer_t *writer, const char *format, ...);
int report_sections(report_writer_t *writer, const report_section_t *sections,
                    size_t count, const void *snapshot);
#endif
