/* BSD-private runtime services. Compile with the pinned BSD kernel headers;
 * none of these types or globals is visible to the Linux socket frontend. */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/limits.h>
#include <sys/elf.h>
#include <machine/stdarg.h>
#include "ff_host_interface.h"
#include "ff_config.h"
#include "ff_log.h"

extern void port_host_log(const char *, ...);
extern void port_host_panic(const char *, ...) __dead2;
extern void port_host_random(void *, size_t);
extern void *port_host_copy(void *, const void *, size_t);
extern void *port_host_move(void *, const void *, size_t);
struct ff_config ff_global_cfg;
/* A statically linked BSD image has no DT_NEEDED or dynamic relocations.
 * Give its built-in ELF metadata reader the real terminating entry rather
 * than relying on a zero-address extern that compilers assume is non-null. */
const Elf64_Dyn _DYNAMIC[] = {{DT_NULL, {0}}};

struct environment_entry { char name[64], value[256]; };
static __thread struct environment_entry environment[16];
static __thread int last_error;
static size_t bounded_length(const char *s, size_t maximum)
{
    size_t n = 0;
    while (n < maximum && s[n]) ++n;
    return n;
}
char *ff_getenv(const char *name)
{
    if (!name) return NULL;
    for (unsigned i = 0; i < nitems(environment); ++i)
        if (environment[i].name[0] && !strcmp(environment[i].name, name))
            return environment[i].value;
    return NULL;
}
int ff_setenv(const char *name, const char *value)
{
    if (!name || !value || !name[0]) return EINVAL;
    size_t key = bounded_length(name, sizeof(environment[0].name));
    size_t length = bounded_length(value, sizeof(environment[0].value));
    if (key == sizeof(environment[0].name) || length == sizeof(environment[0].value)) return EINVAL;
    struct environment_entry *empty = NULL;
    for (unsigned i = 0; i < nitems(environment); ++i) {
        if (!environment[i].name[0] && !empty) empty = &environment[i];
        if (environment[i].name[0] && !strcmp(environment[i].name, name)) {
            memcpy(environment[i].value, value, length + 1);
            return 0;
        }
    }
    if (!empty) return ENOSPC;
    memcpy(empty->value, value, length + 1);
    memcpy(empty->name, name, key + 1);
    return 0;
}
void ff_os_errno(int error) { last_error = error; }
int port_last_error(void) { return last_error; }

struct format_sink { char *buffer; size_t size, count; };
static void format_character(int character, void *opaque)
{
    struct format_sink *sink = opaque;
    if (sink->size && sink->count < sink->size - 1)
        sink->buffer[sink->count] = (char)character;
    ++sink->count;
}
int vsnprintf(char *buffer, size_t size, const char *format, va_list args)
{
    struct format_sink sink = {buffer, size, 0};
    int count = kvprintf(format, format_character, &sink, 10, args);
    if (size) buffer[sink.count < size ? sink.count : size - 1] = 0;
    return count;
}
int snprintf(char *buffer, size_t size, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int count = vsnprintf(buffer, size, format, args);
    va_end(args);
    return count;
}
int sprintf(char *buffer, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int count = vsnprintf(buffer, (size_t)-1, format, args);
    va_end(args);
    return count;
}
int asprintf(char **buffer, struct malloc_type *type, const char *format, ...)
{
    va_list args, copy;
    (void)type; /* The A20OS allocator accounts all BSD types under one budget. */
    *buffer = NULL;
    va_start(args, format);
    va_copy(copy, args);
    int count = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (count < 0) { va_end(args); return -1; }
    char *result = ff_malloc((size_t)count + 1);
    if (!result) { va_end(args); return -1; }
    vsnprintf(result, (size_t)count + 1, format, args);
    va_end(args);
    *buffer = result;
    return count;
}
int ff_log(uint32_t level, uint32_t logtype, const char *format, ...)
{
    (void)level; (void)logtype;
    char buffer[1024];
    va_list args;
    va_start(args, format);
    int count = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    port_host_log("[BSD] %s", buffer);
    return count;
}
void panic(const char *format, ...)
{
    char buffer[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    port_host_panic("BSD: %s", buffer);
}
void ff_arc4rand(void *buffer, unsigned int size, int reseed)
{
    (void)reseed; /* A20OS owns entropy reseeding, not the BSD worker. */
    port_host_random(buffer, size);
}
uint32_t ff_arc4random(void)
{
    uint32_t value;
    ff_arc4rand(&value, sizeof(value), 0);
    return value;
}
uint32_t arc4random(void) { return ff_arc4random(); }
u_long random(void) { return ff_arc4random() & 0x7fffffff; }
/* POSIX compatibility only. BSD cryptographic randomness must not use this. */
unsigned rand_r(unsigned *seed)
{
    *seed = *seed * 1103515245U + 12345U;
    return (*seed >> 1) & 0x7fffffffU;
}
#undef memcpy
#undef memmove
void *memcpy(void *out, const void *in, size_t size) { return port_host_copy(out, in, size); }
void *memmove(void *out, const void *in, size_t size) { return port_host_move(out, in, size); }

/* The A20OS port initially has one serialized protocol executor. No DPDK RSS
 * steering/port table or custom source-address callback is configured. These
 * are the upstream's documented disabled/default paths, not fake RSS tables. */
int ff_rss_check(void *softc, uint32_t source, uint32_t destination, uint16_t sport, uint16_t dport)
{
    (void)softc; (void)source; (void)destination; (void)sport; (void)dport;
    return 1;
}
int ff_rss_tbl_get_portrange(uint32_t source, uint32_t destination, uint16_t sport,
                            uint16_t *first, uint16_t *last, uint16_t **ports)
{
    (void)source; (void)destination; (void)sport; (void)first; (void)last; (void)ports;
    return -1;
}
int ff_rss_tbl_set_portrange(uint16_t first, uint16_t last) { (void)first; (void)last; return -1; }
int ff_in_pcbladdr(uint16_t family, void *remote, uint16_t port, void *local)
{
    (void)family; (void)remote; (void)port; (void)local;
    return 0; /* No override: BSD performs its ordinary route/source selection. */
}


/* First-stage bootstrap configuration, called only by the owning executor
 * before SYSINIT. BSD physmem is a page count, never an invented host RAM size. */
int port_boot_config(size_t byte_budget)
{
    if (byte_budget < 1024 * 1024 || byte_budget / PAGE_SIZE > LONG_MAX) return EINVAL;
    ff_global_cfg.freebsd.hz = 100;
    ff_global_cfg.freebsd.physmem = (long)(byte_budget / PAGE_SIZE);
    ff_global_cfg.freebsd.mem_size = (int)(byte_budget / (1024 * 1024));
    ff_global_cfg.freebsd.fd_reserve = 0;
    return 0;
}
