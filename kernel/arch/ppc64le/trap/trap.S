    .abiversion 2

    .section .text
    .globl __trap_from_user
    .globl __return_to_user
    .globl __trap_from_kernel
    .globl user_trap_return

__trap_from_user:
1:  b 1b

__return_to_user:
2:  b 2b

__trap_from_kernel:
3:  b 3b

user_trap_return:
    b __return_to_user
