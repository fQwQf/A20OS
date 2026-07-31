/*
 * A20 thread trampoline: the kernel starts the thread at this entry with
 * SP = prepared stack, whose top holds [entry, user_arg].  Pop both into
 * the first two argument registers and tail-call __mlibc_enter_thread,
 * mirroring the Linux sysdep's thread_entry.S convention.
 */
.section .text
.global __a20_thread_trampoline
.type __a20_thread_trampoline, "function"
__a20_thread_trampoline:
#if defined(__riscv)
	ld a0, 0(sp)
	ld a1, 8(sp)
	addi sp, sp, 16
	call __mlibc_enter_thread
	unimp
#elif defined(__aarch64__)
	ldp x0, x1, [sp], #16
	bl __mlibc_enter_thread
	brk #0
#elif defined(__x86_64__)
	pop %rdi
	pop %rsi
	call __mlibc_enter_thread
	ud2
#elif defined(__loongarch64)
	ld.d $a0, $sp, 0
	ld.d $a1, $sp, 8
	addi.d $sp, $sp, 16
	bl __mlibc_enter_thread
	break 0
#else
#error "Missing architecture specific code."
#endif
.section .note.GNU-stack,"",%progbits
