/*
 * A20 native ABI program entry.
 *
 * The kernel places the a20_start_info_t pointer in the first argument
 * register (a0/x0/rdi), not a classic exec stack.  Forward it to
 * __a20_mlibc_entry(const a20_start_info *, main_fn).
 */
.weak __global_pointer$
.hidden __global_pointer$

.section .text
.global _start
_start:
#if defined(__riscv)
.option push
.option norelax
	lla gp, __global_pointer$
.option pop

	/* a0 already holds start_info */
	la a1, main
	call __a20_mlibc_entry
	unimp
#elif defined(__aarch64__)
	/* x0 already holds start_info */
	adrp x1, main
	add x1, x1, :lo12:main
	bl __a20_mlibc_entry
	brk #0
#elif defined(__x86_64__)
	/* rdi already holds start_info */
	lea main(%rip), %rsi
	call __a20_mlibc_entry
	ud2
#elif defined(__loongarch64)
	/* $a0 already holds start_info */
	la $a1, main
	bl __a20_mlibc_entry
	break 0
#else
#error "Missing architecture specific code."
#endif

/* Load gp from .preinit_array since it may be used by the executable's .init_array. */
#if defined(__riscv)
load_gp:
.option push
.option norelax
	lla gp, __global_pointer$
.option pop
	ret

.section .preinit_array,"aw"
	.dword load_gp
#endif

.section .note.GNU-stack,"",%progbits
