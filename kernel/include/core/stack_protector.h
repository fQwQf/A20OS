#ifndef _CORE_STACK_PROTECTOR_H
#define _CORE_STACK_PROTECTOR_H

/* random_init() 就绪后把 __stack_chk_guard 从编译期固定值换成随机值。 */
void stack_protector_init(void);

#endif /* _CORE_STACK_PROTECTOR_H */
