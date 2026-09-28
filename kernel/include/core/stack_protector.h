#ifndef _CORE_STACK_PROTECTOR_H
#define _CORE_STACK_PROTECTOR_H

/* Once random_init() is available, swap __stack_chk_guard from its
 * compile-time constant to a random per-boot value. */
void stack_protector_init(void);

#endif /* _CORE_STACK_PROTECTOR_H */
