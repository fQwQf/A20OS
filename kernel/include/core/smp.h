#ifndef _CORE_SMP_H
#define _CORE_SMP_H

#include "core/cpu.h"

/*
 * 架构无关的 SMP 接口。
 *
 * 架构相关代码在 kernel/arch/ 中实现这些函数，
 * 内核其他部分通过此头文件调用，不直接依赖具体硬件机制。
 *
 * 当前 CONFIG_NR_CPUS=1 时，所有函数退化为空操作。
 */

/* 向指定 CPU 发送调度 IPI，唤醒目标 CPU 进行重新调度 */
void smp_send_reschedule(unsigned cpu);

/* 初始化 SMP 子系统（在 proc_init 之后调用） */
void smp_init(void);

/* 启动所有 secondary CPU 进入各自的 idle 循环 */
void smp_boot_secondaries(void);

/* secondary CPU 的初始化入口，由架构代码调用 */
void smp_secondary_init(unsigned cpu_id);

/* Configured CPUs may remain offline when a platform has no secondary-start
 * backend. Common code schedules only on this architecture-neutral mask. */
void smp_core_init(void);
void smp_cpu_mark_online(unsigned cpu);
unsigned smp_configured_cpu_count(void);
unsigned smp_online_cpu_count(void);
uint32_t smp_online_cpu_mask(void);
int smp_cpu_is_online(unsigned cpu);

#endif /* _CORE_SMP_H */
