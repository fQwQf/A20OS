/*
 * RISC-V M-mode flat program support.
 *
 * The learner-facing experiments build as ordinary static-PIE RISC-V ELFs.
 * This loader gives them the same contract the Cortex-M port already has:
 * the image is validated, copied into a kernel-owned allocation, relocated
 * with R_RISCV_RELATIVE only, and published as a task whose entry enters
 * U-mode.  No dynamic linker, interpreter or TLS is accepted; on a 320 KiB
 * machine those are things to refuse, not to approximate.
 *
 * The program's memory is registered in a20_flat_space_t, so every syscall
 * copy and futex word is range-checked against what the program actually
 * owns instead of trusting a bare user pointer.
 */
#ifdef CONFIG_MCU_NATIVE_PROGRAM
#include "core/arch.h"
#include "program.h"
#include "mcu.h"
#include "core/errno.h"
#include "core/panic.h"
#include "core/string.h"
#include "core/flat_thread.h"
#include "mm/elf.h"
#include "mm/slab.h"
#include "proc/proc.h"
#include "abi/native/syscall_entry.h"
#include "abi/native/errno.h"
#include "ipc/handle_table.h"

#define PROGRAM_MAX (160U * 1024U)
#define R_RISCV_RELATIVE 3U

unsigned mcu_program_live;

extern void mcu_enter_user(uintptr_t entry, uintptr_t arg, uintptr_t sp,
                           uintptr_t status) __attribute__((noreturn));

static int within(uintptr_t base, size_t size, uintptr_t p, size_t n)
{
    return p >= base && p - base <= size && n <= size - (p - base);
}

void riscv_mcu_program_release(riscv_mcu_program_t *p)
{
    if (!p)
        return;
    riscv_mcu_program_image_t *owner = p->owner;
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

int mcu_program_user_range(const void *pointer, size_t length)
{
    task_t *task = proc_current();
    riscv_mcu_program_t *p = task ? task->arch_program : NULL;
    if (!p)
        return 0;
    uint64_t flags = spin_lock_irqsave(&p->owner->memory.lock);
    int valid = a20_flat_range_locked(&p->owner->memory,
                                      (uintptr_t)pointer, length);
    spin_unlock_irqrestore(&p->owner->memory.lock, flags);
    return valid;
}

static int loaded_range(const Elf32_Phdr *ph, unsigned count, uint32_t address,
                        uint32_t size, uint32_t flags)
{
    for (unsigned i = 0; i < count; i++)
        if (ph[i].p_type == PT_LOAD && (ph[i].p_flags & flags) == flags &&
            within(ph[i].p_vaddr, ph[i].p_memsz, address, size))
            return 1;
    return 0;
}

/* Validate, copy and relocate.  Returns 0 or a negative errno. */
static int load_image(const void *file, size_t size, riscv_mcu_program_t *p)
{
    Elf32_Ehdr header;
    Elf32_Phdr ph[8];
    if (size < sizeof(header))
        return -ENOEXEC;
    memcpy(&header, file, sizeof(header));
    if (memcmp(header.e_ident, "\177ELF\1\1\1", 7) || header.e_type != ET_DYN ||
        header.e_machine != EM_RISCV || header.e_version != 1 ||
        header.e_ehsize != sizeof(header) ||
        header.e_phentsize != sizeof(ph[0]) || !header.e_phnum ||
        header.e_phnum > 8 || /* bits 1..2 are the float ABI; bit 0 (RVC) is fine */
        (header.e_flags & 0x6U) != 0 ||
        header.e_entry >= PROGRAM_MAX ||
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
            if (dynamic_size || !s->p_filesz ||
                s->p_filesz % sizeof(Elf32_Dyn))
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
             ((s->p_align & (s->p_align - 1)) || s->p_align > 4096 ||
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
    if (!loads || !dynamic_size ||
        !loaded_range(ph, header.e_phnum, header.e_entry, 2, PF_X) ||
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
                if (index >= 8)
                    return -ENOEXEC;
                p->owner->executable_base[index] =
                    p->owner->image + ph[i].p_vaddr;
                p->owner->executable_size[index] = ph[i].p_memsz;
            }
            memcpy((void *)(p->owner->image + ph[i].p_vaddr),
                   (const uint8_t *)file + ph[i].p_offset, ph[i].p_filesz);
        }

    uint32_t rela = 0, rela_size = 0, rela_ent = 12;
    int terminated = 0;
    for (unsigned off = 0; off < dynamic_size; off += sizeof(Elf32_Dyn)) {
        Elf32_Dyn d;
        memcpy(&d, (void *)(p->owner->image + dynamic + off), sizeof(d));
        if (d.d_tag == 0) { /* DT_NULL */
            terminated = 1;
            break;
        }
        switch (d.d_tag) {
        case 7:  /* DT_RELA */
            rela = d.d_un;
            break;
        case 8: /* DT_RELASZ */
            rela_size = d.d_un;
            break;
        case 9: /* DT_RELAENT */
            rela_ent = d.d_un;
            break;
        case 1:  /* DT_NEEDED */
        case 2:  /* DT_PLTRELSZ (unexpected without PLT relocations) */
        case 23: /* DT_JMPREL */
            return -ENOEXEC;
        default:
            break;
        }
    }
    if (!terminated ||
        (rela_size && (rela_ent != sizeof(Elf32_Rela) ||
                       rela_size % sizeof(Elf32_Rela) || rela_size > 16384 ||
                       !loaded_range(ph, header.e_phnum, rela, rela_size,
                                     PF_R))))
        return -ENOEXEC;

    /* Two passes: validate the whole table before touching memory, so a bad
     * entry cannot leave the image half-relocated. */
    for (unsigned pass = 0; pass < 2; pass++)
        for (unsigned off = 0; off < rela_size; off += sizeof(Elf32_Rela)) {
            Elf32_Rela r;
            memcpy(&r, (void *)(p->owner->image + rela + off), sizeof(r));
            if ((r.r_info & 0xffU) != R_RISCV_RELATIVE ||
                !loaded_range(ph, header.e_phnum, r.r_offset, 4, PF_W))
                return -ENOEXEC;
            if (!pass) {
                if (r.r_addend < 0 ||
                    (uint32_t)r.r_addend >= p->owner->image_size)
                    return -ENOEXEC;
            } else {
                uint32_t value =
                    (uint32_t)p->owner->image + (uint32_t)r.r_addend;
                memcpy((void *)(p->owner->image + r.r_offset), &value, 4);
            }
        }

    p->entry = p->owner->image + header.e_entry;
    arch_fence_i();
    return 0;
}

void arch_flat_program_enter(void)
{
    task_t *task = proc_current();
    riscv_mcu_program_t *p = task ? task->arch_program : NULL;
    if (!p)
        panic("program enter without a program");
    /* A task-owned cancellation checkpoint before the first user instruction. */
    proc_check_exit_pending();
    arch_local_irq_disable();
    /* Every trap from U-mode lands on this task's kernel stack. */
    uintptr_t ksp =
        (uintptr_t)task->kstack_base + CONFIG_MCU_KERNEL_STACK_SIZE;
    __asm__ volatile("csrw mscratch, %0" ::"r"(ksp));
    /* entry.S starts with a full-access PMP; replace it with this program's
     * allow-list before the first user instruction. */
    riscv_mcu_pmp_update();
    /* MPIE=1, MPP=0 (U-mode): mret enables interrupts and drops privilege. */
    mcu_enter_user(p->entry, p->arg, p->sp, 0x80);
}

static int prepare_program(const void *elf, size_t length, size_t stack_size,
                           riscv_mcu_program_t **out)
{
    if (!elf || !out || stack_size < 4096 || stack_size > 65536)
        return -EINVAL;
    riscv_mcu_program_t *p = kcalloc(1, sizeof(*p));
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
        riscv_mcu_program_release(p);
        return result;
    }
    p->stack = kcalloc(1, stack_size);
    if (!p->stack) {
        riscv_mcu_program_release(p);
        return -ENOMEM;
    }
    p->stack_size = stack_size;
    p->owner->stack = p->stack;
    if (a20_flat_add_fixed(&p->owner->memory, &p->owner->image_region,
                           p->owner->image, p->owner->image_size) ||
        a20_flat_add_fixed(&p->owner->memory, &p->owner->stack_region,
                           (uintptr_t)p->stack, stack_size))
        panic("program memory ranges rejected");
    *out = p;
    return 0;
}

int mcu_program_spawn(const void *elf, size_t length, size_t stack_size,
                      const void *startup, size_t startup_size,
                      uint32_t affinity)
{
    /* The launcher copies its run record onto the child stack; cap it well
     * below the minimum stack size so a caller cannot exhaust it. */
    if (startup_size > 8192 || (startup_size && !startup) || !affinity ||
        (affinity & ~((1U << CONFIG_NR_CPUS) - 1)))
        return -EINVAL;
    riscv_mcu_program_t *p = NULL;
    int result = prepare_program(elf, length, stack_size, &p);
    if (result)
        return result;

    int pid = proc_alloc_deferred(arch_flat_program_enter);
    if (pid < 0) {
        riscv_mcu_program_release(p);
        return pid;
    }
    task_t *task = proc_find_get(pid);
    if (!task)
        panic("deferred program task disappeared");
    task->abi_mode = 1;
    task->arch_program = p;
    task->flat_user = &p->owner->memory;
    task->a20_ht = a20_ht_create();
    if (!task->a20_ht) {
        proc_free_pid(pid);
        proc_put(task);
        return -ENOMEM;
    }
    uintptr_t sp = ((uintptr_t)p->stack + p->stack_size - startup_size) &
                   ~(uintptr_t)15;
    if (startup_size)
        memcpy((void *)sp, startup, startup_size);
    p->startup = (void *)sp;
    p->sp = sp;
    p->arg = sp;
    p->counted = 1;
    __atomic_fetch_add(&mcu_program_live, 1, __ATOMIC_RELEASE);
    proc_set_name(task, "native-mcu");

    proc_sched_config_t config = {.fields = PROC_SCHED_AFFINITY,
                                  .affinity = affinity};
    if (proc_sched_set(task, &config))
        panic("program affinity rejected");
    proc_publish_deferred_task(task);
    proc_put(task);
    return pid;
}

uintptr_t arch_flat_current_stack_base(void)
{
    task_t *t = proc_current();
    if (!t || !t->arch_program)
        return 0;
    riscv_mcu_program_t *p = t->arch_program;
    return (uintptr_t)p->stack;
}

/* Permission for one registered range: the loaded image needs R/W/X (it holds
 * code and writable data), the program stack and every dynamic region are
 * data-only.  Treating a data region as executable would widen the boundary
 * for no benefit. */
static uint32_t flat_region_perms(const riscv_mcu_program_image_t *owner,
                                  uintptr_t base, size_t size)
{
    if (size == owner->image_size && base == owner->image)
        return ARCH_FLAT_PERM_R | ARCH_FLAT_PERM_W | ARCH_FLAT_PERM_X;
    if (size == (size_t)0 && base == 0)
        return 0;
    return ARCH_FLAT_PERM_R | ARCH_FLAT_PERM_W;
}

int arch_flat_region_snapshot(arch_flat_region_t *out, int max)
{
    task_t *t = proc_current();
    riscv_mcu_program_t *p = t ? t->arch_program : NULL;
    if (!p || !p->owner || !out || max <= 0)
        return 0;

    riscv_mcu_program_image_t *owner = p->owner;
    int count = 0;
    uint64_t flags = spin_lock_irqsave(&owner->memory.lock);
    for (a20_flat_region_t *r = owner->memory.regions; r; r = r->next) {
        if (count == max) {
            count = -ENOSPC;
            break;
        }
        out[count].base = r->base;
        out[count].size = r->size;
        out[count].perms = flat_region_perms(owner, r->base, r->size);
        count++;
    }
    spin_unlock_irqrestore(&owner->memory.lock, flags);
    return count;
}

size_t arch_flat_current_span(uintptr_t *base)
{
    task_t *t = proc_current();
    riscv_mcu_program_t *p = t ? t->arch_program : NULL;
    if (!p || !p->owner || !base)
        return 0;

    riscv_mcu_program_image_t *owner = p->owner;
    uintptr_t low = UINTPTR_MAX, high = 0;
    uint64_t flags = spin_lock_irqsave(&owner->memory.lock);
    for (a20_flat_region_t *r = owner->memory.regions; r; r = r->next) {
        if (r->base < low)
            low = r->base;
        if (r->base + r->size > high)
            high = r->base + r->size;
    }
    spin_unlock_irqrestore(&owner->memory.lock, flags);
    if (low >= high)
        return 0;
    *base = low;
    return (size_t)(high - low);
}

int arch_flat_thread_attach(task_t *parent, task_t *child, uintptr_t entry,
                            uintptr_t arg, uintptr_t top, size_t size,
                            uintptr_t tls)
{
    riscv_mcu_program_t *source = parent->arch_program;
    if (!source || !source->owner || !size || top < size ||
        (top & 15U) || size < 128 || !entry) {
        return -EINVAL;
    }
    if (tls)
        return -EOPNOTSUPP;
    int executable = 0;
    for (unsigned i = 0; i < source->owner->executable_count; i++)
        if (within(source->owner->executable_base[i],
                   source->owner->executable_size[i], entry & ~1U, 2))
            executable = 1;
    if (!executable) {
        return -EFAULT;
    }
    a20_flat_region_t *pin =
        a20_flat_pin(&source->owner->memory, top - size, size);
    if (!pin) {
        return -EFAULT;
    }
    riscv_mcu_program_t *p = kcalloc(1, sizeof(*p));
    if (!p) {
        a20_flat_unpin(&source->owner->memory, pin);
        return -ENOMEM;
    }
    p->owner = source->owner;
    refcount_inc(&p->owner->refs);
    p->stack_pin = pin;
    p->stack = (void *)(top - size);
    p->stack_size = size;
    p->entry = entry & ~1U;
    p->arg = arg;
    p->sp = top & ~(uintptr_t)15;
    p->counted = 1;
    __atomic_fetch_add(&mcu_program_live, 1, __ATOMIC_RELEASE);
    child->arch_program = p;
    child->flat_user = &p->owner->memory;
    return 0;
}
#endif /* CONFIG_MCU_NATIVE_PROGRAM */
