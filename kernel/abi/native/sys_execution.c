/* Shared Native query, also used by the constrained MCU SVC dispatcher. */
#if !defined(CONFIG_MCU) || defined(CONFIG_MCU_NATIVE_PROGRAM)
#include "uapi/a20/execution.h"
#include "abi/native/types.h"
#include "abi/native/errno.h"
#include "abi/native/syscall_entry.h"
#include "sys/usercopy.h"

int64_t sys_a20_execution_info(const a20_syscall_args_t *args) {
    void *out = (void *)(uintptr_t)args->arg[0];
    if (!out)
        return -A20_ERR_FAULT;
    if (args->arg[1] < sizeof(a20_execution_info_t))
        return -A20_ERR_INVALID_ARGUMENT;
    a20_execution_info_t info = {
        .size = sizeof(info),
        .version = A20_EXECUTION_VERSION,
        .elf_machine = ARCH_ELF_MACHINE,
        .pointer_bits = sizeof(void *) * 8,
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        .byte_order = A20_EXEC_LITTLE_ENDIAN,
#else
        .byte_order = A20_EXEC_BIG_ENDIAN,
#endif
        .calling_abi = ARCH_NATIVE_CALL_ABI,
        .native_major = A20_ABI_MAJOR,
        .native_minor = A20_ABI_MINOR,
        .features = A20_EXEC_THREAD_BASIC | A20_EXEC_NATIVE_IMAGE,
#ifdef CONFIG_NOMMU
        .memory_model = A20_EXEC_MEMORY_FLAT,
#else
        .memory_model = A20_EXEC_MEMORY_PRIVATE_VM,
#endif
#ifdef CONFIG_SHARED_FABRIC_ATOMICS
        .atomic_model = A20_EXEC_ATOMIC_FABRIC_GATE,
#else
        .atomic_model = A20_EXEC_ATOMIC_NATIVE,
#endif
    };
#ifndef CONFIG_MCU
    info.features |= A20_EXEC_HANDLES | A20_EXEC_FUTEX;
    if (sizeof(void *) == 8)
        info.features |= A20_EXEC_WIDE_ARGS;
#endif
    if (copy_to_user(out, &info, sizeof(info)) < 0)
        return -A20_ERR_FAULT;
    return A20_OK;
}
#endif
