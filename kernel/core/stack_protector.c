/*
 * A20OS — 内核栈金丝雀（stack canary）运行时
 *
 * 为 -fstack-protector-strong 提供编译器 ABI 符号：
 *   - __stack_chk_guard：全局金丝雀值。启动早期（random_init() 之前）
 *     使用编译期固定值兜底，保证从头运行的早期 C 代码也在保护之下；
 *     熵池就绪后由 stack_protector_init() 换成随机值（低字节清零，
 *     模仿 terminator canary，使字符串类溢出难以顺带覆写）。
 *   - __stack_chk_fail：校验失败即带调用现场 panic。
 *
 * 时序约束：更换 guard 时引导栈上只有 kernel_main 一帧（各架构汇编
 * 直接跳入），而 kernel_main 永不返回，因此不会出现“旧值入栈、新值
 * 校验”的误报。MCU profile 不链接 core/random.c，金丝雀保持固定值。
 */

#include "core/types.h"
#include "core/defs.h"
#include "core/panic.h"
#include "core/stack_protector.h"
#ifndef CONFIG_MCU
#include "core/random.h"
#endif

uintptr_t __stack_chk_guard = (uintptr_t)0xA20C0DEC0DE5AF00ULL;

void stack_protector_init(void)
{
#ifndef CONFIG_MCU
    __stack_chk_guard = (uintptr_t)(random_u64() & ~(uintptr_t)0xFF);
#endif
}

NORETURN void __stack_chk_fail(void)
{
    panic("kernel stack smashing detected (caller=%p)",
          __builtin_return_address(0));
}
