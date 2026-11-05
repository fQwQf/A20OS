/* A20OS owns CPU scheduling. F-Stack executes on one persistent worker and
 * needs its own logical CPU, not FreeBSD's hardware CPU register (x18/GS).
 * pcurthread is task-owned emulated TLS; pcpup is the executor's CPU state. */
#ifndef A20_BSD_MACHINE_PCPU_H
#define A20_BSD_MACHINE_PCPU_H

#if defined(__amd64__)
/* The vendored amd64 definition also satisfies sys/pcpu.h's page-size ABI. */
#include_next <machine/pcpu.h>
#else
struct pcpu;
struct thread;
#define PCPU_MD_FIELDS struct pcpu *pc_prvspace
extern struct pcpu *pcpup;
#ifdef A20_BSD_EXPLICIT_TLS
#include <a20_bsd_explicit_tls.h>
#else
extern __thread struct thread *pcurthread;
#endif

#define get_pcpu() pcpup
#define PCPU_GET(member) (pcpup->pc_ ## member)
#define PCPU_ADD(member, value) (pcpup->pc_ ## member += (value))
#define PCPU_INC(member) PCPU_ADD(member, 1)
#define PCPU_PTR(member) (&pcpup->pc_ ## member)
#define PCPU_SET(member, value) (pcpup->pc_ ## member = (value))
static inline struct thread *__curthread_ff(void) { return pcurthread; }
#define __curthread __curthread_ff
#define curthread __curthread_ff()
#endif

#endif
