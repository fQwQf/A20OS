/* Explicit form of the same task-owned emutls ABI for compilers whose
 * LoongArch backend cannot lower __thread with -femulated-tls. */
#ifndef A20_BSD_EXPLICIT_TLS_H
#define A20_BSD_EXPLICIT_TLS_H
struct port_emutls_control {
    __UINTPTR_TYPE__ size, alignment, index;
    const void *initial;
};
void *__emutls_get_address(struct port_emutls_control *);
struct thread;
extern struct port_emutls_control port_current_thread_control;
#define pcurthread (*(struct thread **)__emutls_get_address(&port_current_thread_control))
#endif
