#ifdef CONFIG_MCU_NATIVE_PROGRAM
#include "program.h"
#include "core/arch.h"
#include "core/errno.h"
#include "core/panic.h"
#include "core/string.h"
#include "mm/elf.h"
#include "mm/slab.h"
#include "proc/proc.h"
#include "abi/native/syscall_entry.h"
#include "abi/native/errno.h"
#include "sys/usercopy.h"
#include "ipc/handle_table.h"
#include "core/flat_thread.h"
#include "core/flat_program.h"

#define PROGRAM_MAX (256U * 1024U)
#define PROGRAM_RESUME_SVC 0x21U
unsigned mcu_program_live;

extern void armv7m_svc_handler(uint32_t *frame);
extern void armv7m_systick_handler(void);
extern uint32_t armv7m_program_user_ticks;
#ifdef CONFIG_NATIVE_FLAT_FILES
extern int64_t sys_a20_task_spawn(const a20_syscall_args_t *args);
extern int64_t sys_a20_path_open(const a20_syscall_args_t *args);
extern int64_t sys_a20_handle_read(const a20_syscall_args_t *args);
extern int64_t sys_a20_handle_write(const a20_syscall_args_t *args);
extern int64_t sys_a20_handle_seek(const a20_syscall_args_t *args);
extern int64_t sys_a20_handle_stat(const a20_syscall_args_t *args);
#endif
extern int64_t sys_a20_task_set_sched(const a20_syscall_args_t *args);
extern int64_t sys_a20_task_get_sched(const a20_syscall_args_t *args);
extern int64_t sys_a20_thread_create(const a20_syscall_args_t *args);
extern int64_t sys_a20_task_kill(const a20_syscall_args_t *args);
extern int64_t sys_a20_signal_check(const a20_syscall_args_t *args);
extern int64_t sys_a20_signal_mask(const a20_syscall_args_t *args);
extern int64_t sys_a20_task_wait(const a20_syscall_args_t *args);
extern int64_t sys_a20_handle_close(const a20_syscall_args_t *args);
extern int64_t sys_a20_execution_info(const a20_syscall_args_t *args);
extern int64_t sys_a20_runtime_info(const a20_syscall_args_t *args);
extern int64_t sys_a20_runtime_bind(const a20_syscall_args_t *args);
extern int64_t sys_a20_handle_identity(const a20_syscall_args_t *args);
extern int64_t sys_a20_execution_domain_info(const a20_syscall_args_t *args);
extern int64_t sys_a20_futex_wait(const a20_syscall_args_t *args);
extern int64_t sys_a20_futex_wake(const a20_syscall_args_t *args);
extern int64_t sys_a20_vm_alloc(const a20_syscall_args_t *args);
extern int64_t sys_a20_vm_unmap(const a20_syscall_args_t *args);
extern int64_t sys_a20_atomic32(const a20_syscall_args_t *args);
extern int64_t sys_a20_thread_sleep(const a20_syscall_args_t *args);
extern int64_t sys_a20_thread_yield(const a20_syscall_args_t *args);
extern int64_t sys_a20_thread_get_cpu(const a20_syscall_args_t *args);
extern int64_t sys_a20_thread_exit(const a20_syscall_args_t *args);

static int within(uintptr_t base, size_t size, uintptr_t p, size_t n) {
    return p >= base && p - base <= size && n <= size - (p - base);
}

int armv7m_program_resume_stack_valid(const task_t *task, uintptr_t sp) {
    const armv7m_program_t *p = task ? task->arch_program : NULL;
    const task_context_t *ctx = task ? (const void *)(uintptr_t)task->kstack : NULL;
    /* Native SVC/tick trampolines execute privileged Thread mode on the
     * application's PSP. A blocked syscall therefore saves a PSP inside its
     * owned/pinned application stack, rather than the separate kernel MSP.
     * Admit only that state; MSP and ordinary kernel tasks retain the generic
     * kernel-stack guard, and arbitrary flat-memory addresses are rejected. */
    return p && p->owner && p->in_kernel && ctx && ctx->control == 2U &&
           p->stack && within((uintptr_t)p->stack, p->stack_size, sp, 0);
}

int mcu_program_user_range(const void *pointer, size_t length) {
    task_t *task = proc_current();
    armv7m_program_t *p = task ? task->arch_program : NULL;
    if (!p)
        return 0;
    uint64_t flags = spin_lock_irqsave(&p->owner->memory.lock);
    int valid =
        a20_flat_range_locked(&p->owner->memory, (uintptr_t)pointer, length);
    spin_unlock_irqrestore(&p->owner->memory.lock, flags);
    return valid;
}

void armv7m_program_release(armv7m_program_t *p) {
    if (!p)
        return;
    armv7m_program_image_t *owner = p->owner;
    if (owner) {
        if (p->stack_pin)
            a20_flat_unpin(&owner->memory, p->stack_pin);
        if (refcount_dec_and_test(&owner->refs)) {
            if (a20_flat_destroy(&owner->memory))
                panic("program released with pinned memory");
            kfree(owner->stack);
            kfree(owner->allocation);
            kfree(owner);
        }
    }
    if (p->counted)
        __atomic_fetch_sub(&mcu_program_live, 1, __ATOMIC_RELEASE);
    kfree(p);
}

static int loaded_range(const Elf32_Phdr *ph, unsigned count, uint32_t address,
                        uint32_t size, uint32_t flags) {
    for (unsigned i = 0; i < count; i++)
        if (ph[i].p_type == PT_LOAD && (ph[i].p_flags & flags) == flags &&
            within(ph[i].p_vaddr, ph[i].p_memsz, address, size))
            return 1;
    return 0;
}

static int load_image(const void *file, size_t size, armv7m_program_t *p) {
    Elf32_Ehdr header;
    Elf32_Phdr ph[16];
    if (size < sizeof(header))
        return -ENOEXEC;
    memcpy(&header, file, sizeof(header));
    if (memcmp(header.e_ident, "\177ELF\1\1\1", 7) || header.e_type != ET_DYN ||
        header.e_machine != EM_ARM || header.e_version != 1 ||
        header.e_ehsize != sizeof(header) ||
        header.e_phentsize != sizeof(ph[0]) || !header.e_phnum ||
        header.e_phnum > 16 || !(header.e_entry & 1U) ||
        (header.e_flags & 0xff000400U) != 0x05000000U ||
        !within(0, size, header.e_phoff, header.e_phnum * sizeof(ph[0])))
        return -ENOEXEC;
    memcpy(ph, (const uint8_t *)file + header.e_phoff,
           header.e_phnum * sizeof(ph[0]));
    uint32_t span = 0, align = 8, dynamic = 0, dynamic_size = 0;
    unsigned loads = 0;
    for (unsigned i = 0; i < header.e_phnum; i++) {
        Elf32_Phdr *s = &ph[i];
        if (s->p_type == PT_INTERP || s->p_type == PT_TLS)
            return -ENOEXEC;
        if (s->p_type == PT_DYNAMIC) {
            if (dynamic_size || !s->p_filesz || s->p_filesz % sizeof(Elf32_Dyn))
                return -ENOEXEC;
            dynamic = s->p_vaddr;
            dynamic_size = s->p_filesz;
        }
        if (s->p_type != PT_LOAD)
            continue;
        if (!s->p_memsz || s->p_filesz > s->p_memsz ||
            !within(0, size, s->p_offset, s->p_filesz) ||
            !within(0, PROGRAM_MAX, s->p_vaddr, s->p_memsz) ||
            (s->p_align &&
             ((s->p_align & (s->p_align - 1)) || s->p_align > 65536 ||
              ((s->p_vaddr - s->p_offset) & (s->p_align - 1)))))
            return -ENOEXEC;
        for (unsigned j = 0; j < i; j++)
            if (ph[j].p_type == PT_LOAD &&
                s->p_vaddr < ph[j].p_vaddr + ph[j].p_memsz &&
                ph[j].p_vaddr < s->p_vaddr + s->p_memsz)
                return -ENOEXEC;
        if (!loads && s->p_vaddr != 0)
            return -ENOEXEC;
        loads++;
        if (s->p_vaddr + s->p_memsz > span)
            span = s->p_vaddr + s->p_memsz;
        if (s->p_align > align)
            align = s->p_align;
    }
    if (!loads ||
        !loaded_range(ph, header.e_phnum, header.e_entry & ~1U, 2, PF_X) ||
        !dynamic_size ||
        !loaded_range(ph, header.e_phnum, dynamic, dynamic_size, PF_R))
        return -ENOEXEC;
    p->owner->allocation = kcalloc(1, span + align - 1);
    if (!p->owner->allocation)
        return -ENOMEM;
    p->owner->image =
        ((uintptr_t)p->owner->allocation + align - 1) & ~(uintptr_t)(align - 1);
    p->owner->image_size = span;
    for (unsigned i = 0; i < header.e_phnum; i++)
        if (ph[i].p_type == PT_LOAD) {
            if (ph[i].p_flags & PF_X) {
                unsigned index = p->owner->executable_count++;
                p->owner->executable_base[index] =
                    p->owner->image + ph[i].p_vaddr;
                p->owner->executable_size[index] = ph[i].p_memsz;
            }
            memcpy((void *)(p->owner->image + ph[i].p_vaddr),
                   (const uint8_t *)file + ph[i].p_offset, ph[i].p_filesz);
        }
    uint32_t rel = 0, rel_size = 0, rel_ent = 8, got = 0;
    int terminated = 0;
    for (unsigned off = 0; off < dynamic_size; off += sizeof(Elf32_Dyn)) {
        Elf32_Dyn d;
        memcpy(&d, (void *)(p->owner->image + dynamic + off), sizeof(d));
        if (d.d_tag == DT_NULL) {
            terminated = 1;
            break;
        }
        switch (d.d_tag) {
            case 1: /* DT_NEEDED */
            case DT_JMPREL:
            case DT_RELA:
            case DT_RELASZ:
                return -ENOEXEC;
            case 3:
                got = d.d_un;
                break; /* DT_PLTGOT */
            case DT_REL:
                rel = d.d_un;
                break;
            case DT_RELSZ:
                rel_size = d.d_un;
                break;
            case DT_RELENT:
                rel_ent = d.d_un;
                break;
            default:
                break;
        }
    }
    if (!terminated || rel_ent != 8 || rel_size % 8 || rel_size > 8192 ||
        (got && !loaded_range(ph, header.e_phnum, got, 4, PF_W)) ||
        (rel_size && !loaded_range(ph, header.e_phnum, rel, rel_size, PF_R)))
        return -ENOEXEC;
    /* Validate the entire table before applying any relocation. This profile
     * resolves only local R_ARM_RELATIVE; it never silently skips imports. */
    for (unsigned pass = 0; pass < 2; pass++)
        for (unsigned off = 0; off < rel_size; off += 8) {
            uint32_t r[2], addend;
            memcpy(r, (void *)(p->owner->image + rel + off), sizeof(r));
            if (r[1] != 23 || (r[0] & 3U) ||
                !loaded_range(ph, header.e_phnum, r[0], 4, PF_W) ||
                (r[0] < rel + rel_size && rel < r[0] + 4))
                return -ENOEXEC;
            memcpy(&addend, (void *)(p->owner->image + r[0]), 4);
            if (!pass && addend >= span)
                return -ENOEXEC;
            if (!pass)
                for (unsigned previous = 0; previous < off; previous += 8) {
                    uint32_t target;
                    memcpy(&target, (void *)(p->owner->image + rel + previous),
                           4);
                    if (target == r[0])
                        return -ENOEXEC;
                }
            if (pass) {
                addend += (uint32_t)p->owner->image;
                memcpy((void *)(p->owner->image + r[0]), &addend, 4);
            }
        }
    p->registers.pc = (uint32_t)p->owner->image + (header.e_entry & ~1U);
    p->registers.r[9] = got ? (uint32_t)p->owner->image + got : 0;
    p->registers.xpsr = 0x01000000U;
    arch_fence_i();
    return 0;
}

void arch_flat_program_enter(void) {
    /* First user entry is a task-owned, lock-free cancellation boundary. */
    proc_check_exit_pending();
    __asm__ __volatile__("svc #0x21" ::: "memory");
    panic("program enter returned");
}

static int prepare_program(const void *elf, size_t length, size_t stack_size,
                           a20_flat_program_t *out) {
    if (!elf || !out || stack_size < 4096 || stack_size > 65536)
        return -EINVAL;
    armv7m_program_t *p = kcalloc(1, sizeof(*p));
    if (!p)
        return -ENOMEM;
    p->owner = kcalloc(1, sizeof(*p->owner));
    if (!p->owner) {
        kfree(p);
        return -ENOMEM;
    }
    refcount_set(&p->owner->refs, 1);
    a20_flat_init(&p->owner->memory);
    int result = load_image(elf, length, p);
    if (result) {
        armv7m_program_release(p);
        return result;
    }
    p->stack = kcalloc(1, stack_size);
    if (!p->stack) {
        armv7m_program_release(p);
        return -ENOMEM;
    }
    p->owner->stack = p->stack;
    p->stack_size = stack_size;
    if (a20_flat_add_fixed(&p->owner->memory, &p->owner->image_region,
                           p->owner->image, p->owner->image_size) ||
        a20_flat_add_fixed(&p->owner->memory, &p->owner->stack_region,
                           (uintptr_t)p->stack, stack_size))
        panic("program memory ranges rejected");
    p->in_kernel = 1;
    result = proc_alloc_deferred(arch_flat_program_enter);
    if (result < 0) {
        armv7m_program_release(p);
        return result;
    }
    task_t *task = proc_find_get(result);
    if (!task)
        panic("deferred program task disappeared");
    task->abi_mode = 1;
    task->arch_program = p;
    task->flat_user = &p->owner->memory;
    task->a20_ht = a20_ht_create();
    if (!task->a20_ht) {
        proc_free_pid(result);
        proc_put(task);
        return -ENOMEM;
    }
    p->counted = 1;
    __atomic_fetch_add(&mcu_program_live, 1, __ATOMIC_RELEASE);
    proc_set_name(task, "native-mcu");
    *out = (a20_flat_program_t){.task = task,
                                .stack_base = (uintptr_t)p->stack,
                                .stack_size = stack_size};
    return result;
}

void arch_flat_program_set_stack(task_t *task, uintptr_t startup) {
    armv7m_program_t *p = task->arch_program;
    p->startup = (void *)startup;
    p->registers.sp = startup;
    p->registers.r[0] = startup;
}

int arch_flat_program_create(const void *elf, size_t length, size_t stack_size,
                             a20_flat_program_t *out) {
    /* Public task_spawn requires the same Native ELF marker as the full VFS
     * loader. Legacy embedded selftest images retain their explicit entry. */
    Elf32_Ehdr h;
    if (!elf || length < sizeof(h))
        return -ENOEXEC;
    memcpy(&h, elf, sizeof(h));
    if (h.e_phentsize != sizeof(Elf32_Phdr) || !h.e_phnum || h.e_phnum > 16 ||
        !within(0, length, h.e_phoff, h.e_phnum * sizeof(Elf32_Phdr)))
        return -ENOEXEC;
    int native = 0;
    for (unsigned i = 0; i < h.e_phnum; i++) {
        Elf32_Phdr ph;
        memcpy(&ph, (const uint8_t *)elf + h.e_phoff + i * sizeof(ph),
               sizeof(ph));
        if (ph.p_type == PT_A20_START_INFO)
            native = 1;
    }
    return native ? prepare_program(elf, length, stack_size, out) : -ENOEXEC;
}

int mcu_program_spawn(const void *elf, size_t length, size_t stack_size,
                      const void *startup, size_t startup_size,
                      uint32_t affinity) {
    if (startup_size > 256 || (startup_size && !startup) || !affinity ||
        (affinity & ~((1U << CONFIG_NR_CPUS) - 1)))
        return -EINVAL;
    a20_flat_program_t created;
    int pid = prepare_program(elf, length, stack_size, &created);
    if (pid < 0)
        return pid;
    uintptr_t sp =
        (created.stack_base + stack_size - startup_size) & ~(uintptr_t)15;
    if (startup_size)
        memcpy((void *)sp, startup, startup_size);
    arch_flat_program_set_stack(created.task, sp);
    proc_sched_config_t config = {.fields = PROC_SCHED_AFFINITY,
                                  .affinity = affinity};
    if (proc_sched_set(created.task, &config))
        panic("program affinity rejected");
    proc_publish_deferred_task(created.task);
    proc_put(created.task);
    return pid;
}

/* ISA adapter: distinct registers/PSP, common owned space and task lifetime.
 * The current static-PIE Thumb profile has a PIC r9, but no TLS segment. */
int arch_flat_thread_attach(task_t *parent, task_t *child, uintptr_t entry,
                            uintptr_t arg, uintptr_t top, size_t size,
                            uintptr_t tls) {
    armv7m_program_t *source = parent->arch_program;
    if (!source || !source->owner || !size || top < size || (top & 15U) ||
        size < 128 || !(entry & 1U))
        return -EINVAL;
    if (tls)
        return -EOPNOTSUPP;
    int executable = 0;
    for (unsigned i = 0; i < source->owner->executable_count; i++)
        if (within(source->owner->executable_base[i],
                   source->owner->executable_size[i], entry & ~1U, 2))
            executable = 1;
    if (!executable)
        return -EFAULT;
    a20_flat_region_t *pin =
        a20_flat_pin(&source->owner->memory, top - size, size);
    if (!pin)
        return -EFAULT;
    armv7m_program_t *p = kcalloc(1, sizeof(*p));
    if (!p) {
        a20_flat_unpin(&source->owner->memory, pin);
        return -ENOMEM;
    }
    p->owner = source->owner;
    refcount_inc(&p->owner->refs);
    p->stack_pin = pin;
    p->stack = (void *)(top - size);
    p->stack_size = size;
    p->registers.sp = top;
    p->registers.pc = entry & ~1U;
    p->registers.r[0] = arg;
    p->registers.r[9] = source->registers.r[9];
    p->registers.xpsr = 0x01000000U;
    p->in_kernel = 1;
    p->counted = 1;
    __atomic_fetch_add(&mcu_program_live, 1, __ATOMIC_RELEASE);
    child->arch_program = p;
    child->flat_user = &p->owner->memory;
    return 0;
}

static void program_syscall_thread(void) {
    task_t *task = proc_current();
    armv7m_program_t *p = task ? task->arch_program : NULL;
    if (!p || !p->in_kernel)
        panic("program syscall state");
    a20_syscall_args_t args = {.nr = p->registers.r[7], .ctx = &p->registers};
    for (unsigned i = 0; i < 6; i++)
        args.arg[i] = p->registers.r[i];
    /* Consume persistent remote exit requests in Thread mode, never inside
     * exception entry or while a syscall still owns a futex/handle lease. */
    proc_check_exit_pending();
    int64_t result;
    switch (args.nr) {
#ifdef CONFIG_NATIVE_FLAT_FILES
        case A20_SYS_task_spawn:
            result = sys_a20_task_spawn(&args);
            break;
        case A20_SYS_path_open:
            result = sys_a20_path_open(&args);
            break;
        case A20_SYS_handle_read:
            result = sys_a20_handle_read(&args);
            break;
        case A20_SYS_handle_write:
            result = sys_a20_handle_write(&args);
            break;
        case A20_SYS_handle_seek:
            result = sys_a20_handle_seek(&args);
            break;
        case A20_SYS_handle_stat:
            result = sys_a20_handle_stat(&args);
            break;
#endif
        case A20_SYS_task_set_sched:
            result = sys_a20_task_set_sched(&args);
            break;
        case A20_SYS_task_get_sched:
            result = sys_a20_task_get_sched(&args);
            break;
        case A20_SYS_thread_create:
            result = sys_a20_thread_create(&args);
            break;
        case A20_SYS_task_kill:
            result = sys_a20_task_kill(&args);
            break;
        case A20_SYS_signal_check:
            result = sys_a20_signal_check(&args);
            break;
        case A20_SYS_signal_mask:
            result = sys_a20_signal_mask(&args);
            break;
        case A20_SYS_task_wait:
            result = sys_a20_task_wait(&args);
            break;
        case A20_SYS_handle_close:
            result = sys_a20_handle_close(&args);
            break;
        case A20_SYS_vm_alloc:
            result = sys_a20_vm_alloc(&args);
            break;
        case A20_SYS_vm_unmap:
            result = sys_a20_vm_unmap(&args);
            break;
        case A20_SYS_atomic32:
            result = sys_a20_atomic32(&args);
            break;
        case A20_SYS_futex_wait:
            result = sys_a20_futex_wait(&args);
            break;
        case A20_SYS_futex_wake:
            result = sys_a20_futex_wake(&args);
            break;
        case A20_SYS_runtime_info:
            result = sys_a20_runtime_info(&args);
            break;
        case A20_SYS_runtime_bind:
            result = sys_a20_runtime_bind(&args);
            break;
        case A20_SYS_handle_identity:
            result = sys_a20_handle_identity(&args);
            break;
        case A20_SYS_execution_domain_info:
            result = sys_a20_execution_domain_info(&args);
            break;
        case A20_SYS_execution_info:
            result = sys_a20_execution_info(&args);
            break;
        case A20_SYS_task_exit:
            proc_exit_group((int32_t)args.arg[0]);
        case A20_SYS_thread_exit:
            result = sys_a20_thread_exit(&args);
            break;
        case A20_SYS_thread_sleep:
            result = sys_a20_thread_sleep(&args);
            break;
        case A20_SYS_thread_yield:
            result = sys_a20_thread_yield(&args);
            break;
        case A20_SYS_thread_get_cpu:
            result = sys_a20_thread_get_cpu(&args);
            break;
        default:
            result = -A20_ERR_NOT_SUPPORTED;
            break;
    }
    /* Blocking syscalls may have been woken by task_kill. Their cleanup has
     * completed here, so an exit cannot strand a wait node or memory pin. */
    proc_check_exit_pending();
    p->registers.r[0] = (uint32_t)result;
    __asm__ __volatile__("svc #0x21" ::: "memory");
    panic("program syscall resume returned");
}

/* The exception hook only captures registers and selects a Thread-mode entry.
 * Scheduling/exit happens after EXC_RETURN, on this task's own PSP. */
static void program_capture_user(armv7m_program_t *p, const uint32_t *frame,
                                 const uint32_t *callee) {
    unsigned padding = (frame[7] & 0x200U) ? 4 : 0;
    if (!within((uintptr_t)p->stack, p->stack_size, (uintptr_t)frame,
                32 + padding))
        panic("program exception stack bounds");
    trap_context_t *r = &p->registers;
    memcpy(&r->r[0], frame, 16);
    memcpy(&r->r[4], callee, 32);
    r->r[12] = frame[4];
    r->lr = frame[5];
    r->pc = frame[6];
    r->xpsr = frame[7];
    r->sp = (uintptr_t)frame + 32 + padding;
    p->in_kernel = 1;
}
static void program_redirect_user(uint32_t *frame, void (*entry)(void)) {
    unsigned padding = frame[7] & 0x200U;
    frame[6] = (uintptr_t)entry & ~1U;
    frame[7] = 0x01000000U | padding;
    uint32_t control;
    __asm__ __volatile__("mrs %0, control" : "=r"(control));
    control &= ~1U;
    __asm__ __volatile__("msr control, %0\nisb" ::"r"(control) : "memory");
}
static void program_interrupt_thread(void) {
    proc_check_exit_pending();
    proc_yield();
    proc_check_exit_pending();
    __asm__ __volatile__("svc #0x21" ::: "memory");
    panic("program interrupt resume returned");
}
void armv7m_program_tick(uint32_t *frame, const uint32_t *callee,
                         uint32_t exception_return) {
    armv7m_systick_handler();
    task_t *task = proc_current();
    armv7m_program_t *p = task ? task->arch_program : NULL;
    uint32_t control;
    __asm__ __volatile__("mrs %0, control" : "=r"(control));
    if (!frame || !p || p->in_kernel || !(control & 1U) ||
        !(exception_return & 4U) || task->state != PROC_RUNNING)
        return;
    int redirect = __atomic_load_n(&task->exit_pending, __ATOMIC_ACQUIRE);
    /* User preemption is separate from CONFIG_KERNEL_PREEMPT: the MCU
     * intentionally keeps privileged kernel code cooperative. */
    if (++armv7m_program_user_ticks >= 10U) {
        armv7m_program_user_ticks = 0;
        redirect = 1;
    }
    if (!redirect)
        return;
    program_capture_user(p, frame, callee);
    program_redirect_user(frame, program_interrupt_thread);
}

uint32_t *armv7m_program_svc(uint32_t *frame, const uint32_t *callee,
                             uint32_t exception_return) {
    task_t *task = proc_current();
    armv7m_program_t *p = task ? task->arch_program : NULL;
    if (!frame || frame[6] < 2)
        panic("invalid SVC frame");
    const uint8_t *instruction = (void *)(uintptr_t)(frame[6] - 2);
    uint32_t control;
    __asm__ __volatile__("mrs %0, control" : "=r"(control));
    if (instruction[1] != 0xdf)
        panic("invalid SVC instruction");
    if (p && p->in_kernel && !(control & 1U) &&
        instruction[0] == PROGRAM_RESUME_SVC) {
        trap_context_t *r = &p->registers;
        unsigned padding = (r->sp & 4U) ? 4 : 0;
        uintptr_t address = r->sp - 64 - padding;
        if ((r->sp & 3U) ||
            !within((uintptr_t)p->stack, p->stack_size, address, 64 + padding))
            panic("program return stack bounds");
        uint32_t *out = (void *)address;
        memcpy(out, &r->r[4], 32);
        memcpy(out + 8, &r->r[0], 16);
        out[12] = r->r[12];
        out[13] = r->lr;
        out[14] = r->pc;
        out[15] = (r->xpsr & ~0x200U) | (padding ? 0x200U : 0);
        p->in_kernel = 0;
        return out;
    }
    if (p && !p->in_kernel && (control & 1U) && (exception_return & 4U)) {
        if (instruction[0] != 0) {
            frame[0] = (uint32_t)-A20_ERR_NOT_SUPPORTED;
            return NULL;
        }
        program_capture_user(p, frame, callee);
        program_redirect_user(frame, program_syscall_thread);
        return NULL;
    }
    armv7m_svc_handler(frame);
    return NULL;
}
#endif
