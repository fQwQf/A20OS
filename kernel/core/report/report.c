#include "core/report.h"
#include "core/stdio.h"
#include "core/errno.h"

void report_printf(report_writer_t *writer, const char *format, ...) {
    if (!writer) return;
    if (writer->error) return;
    if (!writer->buffer || writer->length >= writer->capacity) {
        writer->error = -EINVAL;
        return;
    }
    va_list args;
    va_start(args, format);
    int n = vsnprintf(writer->buffer + writer->length,
                      writer->capacity - writer->length, format, args);
    va_end(args);
    /* The kernel formatter returns bytes stored, unlike libc's would-write
     * count. Treat a full destination as exhausted in either implementation;
     * keep one spare byte so clipped output is never accepted as complete. */
    if (n < 0 || (size_t)n >= writer->capacity - writer->length - 1) {
        writer->error = -ENOSPC;
        return;
    }
    writer->length += (size_t)n;
}

int report_sections(report_writer_t *writer, const report_section_t *sections,
                    size_t count, const void *snapshot) {
    if (!writer || !writer->buffer || !writer->capacity) return -EINVAL;
    for (size_t i = 0; i < count && !writer->error; i++) {
        int error = sections[i].render(writer, snapshot);
        if (error < 0) report_printf(writer, "section %s: unavailable (%d)\n", sections[i].name, error);
    }
    return writer->error ? writer->error : (int)writer->length;
}

void report_json_string(report_writer_t *writer, const char *text) {
    report_printf(writer, "\"");
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    for (; *p && !writer->error; p++) {
        if (*p == '"' || *p == '\\') report_printf(writer, "\\%c", *p);
        else if (*p < 32) report_printf(writer, "\\u%04x", (unsigned)*p);
        else report_printf(writer, "%c", *p);
    }
    report_printf(writer, "\"");
}
