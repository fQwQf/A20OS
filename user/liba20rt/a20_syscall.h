/*
 * A20OS Native SDK — Syscall invocation.
 *
 * Convention: a7=syscall_nr, a0-a5=args, return in a0.
 * Both RV64 and LA64 use the same register names.
 */
#ifndef _A20_SYSCALL_H
#define _A20_SYSCALL_H

#include <stdint.h>
#include "a20_types.h"

#if defined(__loongarch64)
#define A20_SYSCALL_INSN  "syscall 0"
#elif defined(__riscv)
#define A20_SYSCALL_INSN  "ecall"
#else
#error "Unsupported architecture for A20 syscall"
#endif

static inline int64_t a20_syscall6(uint64_t nr, uint64_t a0, uint64_t a1,
                                    uint64_t a2, uint64_t a3,
                                    uint64_t a4, uint64_t a5)
{
    register uint64_t a7 __asm__("a7") = nr;
    register uint64_t _a0 __asm__("a0") = a0;
    register uint64_t _a1 __asm__("a1") = a1;
    register uint64_t _a2 __asm__("a2") = a2;
    register uint64_t _a3 __asm__("a3") = a3;
    register uint64_t _a4 __asm__("a4") = a4;
    register uint64_t _a5 __asm__("a5") = a5;
    __asm__ volatile(A20_SYSCALL_INSN
        : "+r"(_a0)
        : "r"(a7), "r"(_a1), "r"(_a2), "r"(_a3), "r"(_a4), "r"(_a5)
        : "memory");
    return (int64_t)_a0;
}

/* ===== Core (0x0000) ===== */
#define A20_SYS_abi_info          0x0000
#define A20_SYS_feature_test      0x0001

/* ===== Handle (0x0100) ===== */
#define A20_SYS_handle_close      0x0100
#define A20_SYS_handle_dup        0x0101
#define A20_SYS_handle_query      0x0102
#define A20_SYS_handle_replace    0x0103
#define A20_SYS_handle_close_many 0x0104
#define A20_SYS_handle_seek       0x0105
#define A20_SYS_handle_transfer   0x0106
#define A20_SYS_handle_set_meta   0x0107
#define A20_SYS_handle_xattr_set  0x0108
#define A20_SYS_handle_xattr_get  0x0109
#define A20_SYS_handle_xattr_list 0x010A
#define A20_SYS_handle_xattr_remove 0x010B

/* ===== Task / Thread (0x0200) ===== */
#define A20_SYS_task_exit         0x0200
#define A20_SYS_task_spawn        0x0201
#define A20_SYS_task_wait         0x0202
#define A20_SYS_task_kill         0x0203
#define A20_SYS_task_info         0x0204
#define A20_SYS_thread_create     0x0205
#define A20_SYS_thread_exit       0x0206
#define A20_SYS_thread_sleep      0x0207
#define A20_SYS_thread_yield      0x0208
#define A20_SYS_task_set_sched    0x0209
#define A20_SYS_task_get_sched    0x020A
#define A20_SYS_task_get_limits   0x020B
#define A20_SYS_task_set_limits   0x020C
#define A20_SYS_task_get_usage    0x020D

/* ===== Memory (0x0300) ===== */
#define A20_SYS_vm_alloc          0x0300
#define A20_SYS_vm_unmap          0x0301
#define A20_SYS_vm_protect        0x0302
#define A20_SYS_vm_map            0x0303
#define A20_SYS_vm_share          0x0304
#define A20_SYS_vm_flush          0x0305
#define A20_SYS_vm_advise         0x0306
#define A20_SYS_vm_remap          0x0307
#define A20_SYS_vm_lock           0x0308
#define A20_SYS_vm_create_object  0x0309

/* ===== Path / Filesystem (0x0400) ===== */
#define A20_SYS_path_open         0x0400
#define A20_SYS_handle_read       0x0401
#define A20_SYS_handle_write      0x0402
#define A20_SYS_handle_stat       0x0403
#define A20_SYS_path_create       0x0404
#define A20_SYS_path_unlink       0x0405
#define A20_SYS_path_rename       0x0406
#define A20_SYS_handle_control    0x0407
#define A20_SYS_path_readdir      0x0408
#define A20_SYS_path_link         0x0409
#define A20_SYS_path_symlink      0x040A
#define A20_SYS_path_readlink     0x040B
#define A20_SYS_path_resolve      0x040C
#define A20_SYS_fs_stat           0x040D
#define A20_SYS_fs_mount          0x040E
#define A20_SYS_fs_umount         0x040F
#define A20_SYS_fs_sync           0x0410

/* ===== Event / IPC (0x0500) ===== */
#define A20_SYS_event_queue_create 0x0500
#define A20_SYS_event_watch       0x0501
#define A20_SYS_event_wait        0x0502
#define A20_SYS_event_cancel      0x0503
#define A20_SYS_channel_create    0x0504
#define A20_SYS_channel_send      0x0505
#define A20_SYS_channel_recv      0x0506
#define A20_SYS_event_watch_fs    0x0507

/* ===== Network (0x0600) ===== */
#define A20_SYS_net_socket        0x0600
#define A20_SYS_net_bind          0x0601
#define A20_SYS_net_connect       0x0602
#define A20_SYS_net_accept        0x0603
#define A20_SYS_net_listen        0x0604
#define A20_SYS_net_sendmsg       0x0605
#define A20_SYS_net_recvmsg       0x0606
#define A20_SYS_net_socketpair    0x0607
#define A20_SYS_net_getname       0x0608
#define A20_SYS_net_shutdown      0x0609

/* ===== Time (0x0700) ===== */
#define A20_SYS_clock_get         0x0700
#define A20_SYS_timer_create      0x0701
#define A20_SYS_timer_set         0x0702
#define A20_SYS_timer_cancel      0x0703
#define A20_SYS_clock_set         0x0704
#define A20_SYS_clock_resolution  0x0705

/* ===== Security (0x0800) ===== */
#define A20_SYS_ns_create         0x0800
#define A20_SYS_ns_apply          0x0801
#define A20_SYS_security_get_context 0x0802
#define A20_SYS_security_set_context 0x0803

/* ===== Debug (0x0900) ===== */
#define A20_SYS_debug_attach      0x0900
#define A20_SYS_debug_read_regs   0x0901
#define A20_SYS_debug_write_regs  0x0902
#define A20_SYS_debug_map_memory  0x0903

/* ===== System (0x0A00) ===== */
#define A20_SYS_system_info       0x0A00
#define A20_SYS_system_random     0x0A01
#define A20_SYS_system_reboot     0x0A02

#endif
