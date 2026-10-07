/*
 * Hypervisor run loop, machine-independent half.  The frozen contract is
 * kernel/include/hyp/hyp_vcpu.h; the design record is
 * docs/hypervisor/00-design.md S5.
 *
 * This file owns three things and nothing else: vcpu lifetime, guest RAM
 * provisioning, and what a guest trap MEANS.  Register save/restore and the
 * guest trap frame are arch code (kernel/arch/riscv64/hyp/).
 *
 * SEAM WITH THE ARCH HALF -- the one thing both sides have to agree on:
 * hyp_vcpu_handle_trap() mutates vcpu->regs[]/vcpu->pc ONLY.  It never
 * touches the trap_frame it is handed; that struct is arch-shaped and this
 * file is not.  The arch side therefore owns the writeback: on a return of
 * 1 it must copy vcpu->regs[]/vcpu->pc back into the frame it takes sret
 * on, and on a return of 0 it restores arch[] and returns from
 * hyp_arch_vcpu_enter().  Guest RAM is pre-loaded through hyp_vm_load();
 * this half never installs a stage-2 mapping on a guest's behalf.
 */
#include "hyp/hyp_vcpu.h"
#include "mm/frame.h"
#include "mm/slab.h"
#include "core/cpu.h"
#include "core/klog.h"
#include "core/lock.h"
#include "core/stdio.h"
#include "core/string.h"

/*
 * RISC-V H exception codes the run loop acts on.  They are named here rather
 * than taken from kernel/<arch>/include/platform.h because that header
 * publishes the generic ECALL-from-U code but not the VS-mode one, and the
 * guest enters VS-mode with hstatus.SPV=1 (S5), so which of the two it
 * issues is a run-shape decision, not a per-arch header fact.
 *
 * All three ECALL codes are accepted because which one the guest issues
 * depends on the privilege half of the entry, and this run shape enters at
 * privilege S: hyp_vcpu_asm.S sets sstatus.SPP, and SPP is what an ECALL
 * reports against, so a guest entered this way raises 10 (from S-mode) and not
 * 9.  8 (from U-mode) and 9 (from VS-mode proper, i.e. an SPP=0 entry) stay
 * in the table so a guest entered the other way round is still served.
 *
 * The second-stage codes below are the H extension's own, and note what they
 * are NOT: 20 is the instruction guest page fault, 21 the load/AMO one, 22 is
 * the VIRTUAL INSTRUCTION fault and 23 is the store/AMO guest page fault
 * (target/riscv/cpu_bits.h, RISCV_EXCP_*_GUEST_*; QEMU composes htval for all
 * of them in raise_mmu_exception, cpu_helper.c:1773-1810).  A v2 brief that
 * calls 22 a guest page fault would send every trapped guest CSR access --
 * which is what hstatus.VTVM makes of the guest's satp write -- into the
 * demand-fill path and fault it there instead.
 *
 * 22 is the number this platform actually delivers, measured rather than
 * assumed: the boot smoke logs "scause=16" -- which the trace prints with
 * %lx, so that is 0x16 = 22 -- together with stval carrying the instruction
 * encoding (.kernel-build/smoke/hyp-a20os-riscv64.log, trap #0/#1).  stval
 * holds env->bins for exactly two causes in QEMU, illegal instruction and
 * virtual instruction fault (cpu_helper.c:2405-2407), and cause 2 is not
 * what was logged.  Read "16" out of that line as decimal and you would
 * route the guest's satp write nowhere; it is the other half of 0x16.
 *
 * Codes 12/13/15 (the guest's OWN stage-1 faults) are delegated to VS-mode in
 * v2 and therefore cannot reach this loop; they stay below only so that a
 * guest whose delegation is not in force still exits with a recorded cause
 * instead of falling through to "unhandled".
 */
#define HYP_SCAUSE_ECALL_VU   8ULL
#define HYP_SCAUSE_ECALL_VS   9ULL
#define HYP_SCAUSE_ECALL_S   10ULL
#define HYP_SCAUSE_FETCH_PF  12ULL
#define HYP_SCAUSE_LOAD_PF   13ULL
#define HYP_SCAUSE_STORE_PF  15ULL
#define HYP_SCAUSE_GPF_INST  20ULL
#define HYP_SCAUSE_GPF_LOAD  21ULL
#define HYP_SCAUSE_VIRT_INST 22ULL
#define HYP_SCAUSE_GPF_STORE 23ULL
#define HYP_SCAUSE_INTR_BIT   (1ULL << 63)

/* What a page of guest RAM loaded through hyp_vm_load() gets in stage-2.
 *
 * R|W|X, the same leaf the demand-fill window uses (hyp.c,
 * HYP_RAM_PAGE_PROT), and for the same reason: hyp_vm_load() carries no
 * per-page flags, so the loader cannot tell a code page from a data page and
 * cannot map them differently.  R|X was enough while the v1 demo guest was
 * register-only, but it is not enough for a real kernel: the guest image's
 * .data and .bss are loaded by hyp_vm_load() like everything else, and the
 * first thing A20OS's entry.S does is an LR/SC on a lock word in .data
 * (.kernel-build smoke log hyp-a20os-riscv64.log, "undecodable guest access
 * at pc=80200044", scause 23 on gpa 0x8060de78) -- a store to a page mapped
 * read-only, which faults before the guest has printed anything.  Page
 * granularity is what makes the split impossible anyway: .data and .bss share
 * pages.
 *
 * R|W|X is a reserved leaf encoding by the priv spec; that is already known
 * and argued in full above HYP_RAM_PAGE_PROT.  QEMU 10.0.13's G-stage walk
 * refuses only the R-less combinations (target/riscv/cpu_helper.c,
 * get_physical_address(): cases PTE_W|PTE_X and PTE_W), so value 7 falls
 * through to prot = PAGE_READ|PAGE_WRITE|PAGE_EXEC.  hyp_s2_map() adds
 * A|D|U on top of this.  The guest's own stage-1 has already had its say by
 * the time this leaf is walked.
 */
#define HYP_GUEST_PAGE_PROT (PTE_R | PTE_W | PTE_X)

/* The one vcpu whose guest is live kernel-wide, or NULL.  A lock is not held
 * across it, because hyp_arch_vcpu_enter() ends in sret and a lock held across
 * that would be a lock held across arbitrary guest execution.  What makes the
 * slice single-guest is the claim under g_hyp_active_lock, not that lock's
 * scope: it is taken to claim and to release, and across neither the guest nor
 * anything that can block.  The IRQ-off windows in hyp_vcpu_run() are a
 * different thing -- they cover the plain store of vcpu->running that this
 * CPU's own dispatcher reads, which is not shared state. */
static hyp_vcpu_t *hyp_active_vcpu;

/* The claim's lock.  A real cross-CPU lock, unlike arch_local_irq_disable(),
 * which only closes a window on the CPU that executes it: an IRQ-off
 * test-and-set on hyp_active_vcpu would let two CPUs each find the slot free
 * and both program hgatp/hstatus and sret into VS-mode on it. */
static spinlock_t g_hyp_active_lock = SPINLOCK_INIT;

hyp_vcpu_t *hyp_vcpu_active(void)
{
    /* Acquire, because a CPU that sees a vcpu published here must also see
     * everything hyp_vcpu_run() wrote before publishing it (hgatp, hstatus,
     * vcpu->running); a plain load would not order against the claim's store. */
    return __atomic_load_n(&hyp_active_vcpu, __ATOMIC_ACQUIRE);
}

hyp_vcpu_t *hyp_vcpu_create(hyp_vm_t *vm, uint64_t entry_gpa)
{
    if (!vm || vm->magic != HYP_VM_MAGIC)
        return NULL;

    hyp_vcpu_t *vcpu = kcalloc(1, sizeof(*vcpu));
    if (!vcpu)
        return NULL;

    vcpu->magic    = HYP_VCPU_MAGIC;
    vcpu->running  = 0;
    vcpu->vm       = vm;
    vcpu->refcount = 1;
    vcpu->pc       = entry_gpa;
    vcpu->exit     = HYP_EXIT_NONE;

    /* The guest's initial sp and satp are the arch half's call: only
     * hyp_arch_vcpu_setup() knows the guest RAM layout it is entering. */
    vm->refcount++;
    return vcpu;
}

/* Guest boot registers (contract): the loader puts the hartid and the DTB's
 * GPA in a0/a1, the SBI/S-mode convention _start reads them from.  Only the
 * two argument registers are described, so that is all this writes: every
 * other register, including sp, stays whatever create() left. */
int hyp_vcpu_set_boot(hyp_vcpu_t *vcpu, uint64_t hartid, uint64_t dtb_gpa)
{
    if (!vcpu || vcpu->magic != HYP_VCPU_MAGIC)
        return -EINVAL;
    vcpu->regs[10] = hartid;
    vcpu->regs[11] = dtb_gpa;
    return 0;
}

void hyp_vcpu_put(hyp_vcpu_t *vcpu)
{
    if (!vcpu || vcpu->magic != HYP_VCPU_MAGIC)
        return;

    /* A running guest owns its vcpu for the duration of the enter/exit
     * window; dropping the last reference under it would free the register
     * file the arch half is still restoring into. */
    if (vcpu->running || hyp_active_vcpu == vcpu) {
        kerr("hyp: put of a running vcpu refused\n");
        return;
    }

    if (--vcpu->refcount > 0)
        return;

    hyp_vm_t *vm = vcpu->vm;
    vcpu->vm = NULL;
    vcpu->magic = 0;
    kfree(vcpu);
    /* The VM outlives the vcpu only while someone else holds it. */
    if (vm)
        hyp_vm_put(vm);
}

int hyp_vm_load(hyp_vm_t *vm, uint64_t gpa, const void *bytes, uint64_t len)
{
    if (!vm || vm->magic != HYP_VM_MAGIC || !bytes)
        return -EINVAL;
    if (gpa & (PAGE_SIZE - 1))
        return -EINVAL;
    if (len == 0)
        return 0;
    if (gpa + len < gpa)
        return -EINVAL;

    uint64_t npages = (len + PAGE_SIZE - 1) >> PAGE_SIZE_BITS;
    pfn_t   *pfns  = kcalloc(npages, sizeof(*pfns));
    if (!pfns)
        return -ENOMEM;

    /* Copy BEFORE the mapping goes in: if hyp_s2_map() refuses the page
     * (-EEXIST), the bytes were written into a frame this call still owns
     * and nobody else can name, so a refusal never corrupts an existing
     * mapping.  pfa_alloc_page() is the reclaiming allocator and this loop
     * holds no page-table node lock, which is the condition that makes
     * reclaim safe here (S2: reclaim takes node locks). */
    uint64_t done  = 0;   /* bytes handed to a mapped page */
    uint64_t pages = 0;   /* pages currently in the table */
    pfn_t    cur   = PFN_NONE;
    int      rc    = 0;

    for (uint64_t i = 0; i < npages; i++) {
        cur = pfa_alloc_page();
        if (cur == PFN_NONE) {
            rc = -ENOMEM;
            break;
        }
        pfns[i] = cur;

        void *dst = pfn_to_virt(cur);
        if (!dst) {
            rc = -EINVAL;
            break;
        }
        uint64_t n = len - done;
        if (n > PAGE_SIZE)
            n = PAGE_SIZE;
        memcpy(dst, (const uint8_t *)bytes + done, n);
        /* The tail of a short final page is zeroed rather than left as
         * whatever the recycled frame held: the guest reads whole pages. */
        if (n < PAGE_SIZE)
            memset((uint8_t *)dst + n, 0, PAGE_SIZE - n);

        rc = hyp_s2_map(vm, gpa + i * PAGE_SIZE, cur, HYP_GUEST_PAGE_PROT);
        if (rc)
            break;

        cur = PFN_NONE;   /* the table owns this reference now */
        done += n;
        pages++;
    }

    if (rc) {
        /* Partial load: unwind, so a failed load leaves the VM's address
         * space exactly as it found it. */
        for (uint64_t j = 0; j < pages; j++) {
            hyp_s2_unmap(vm, gpa + j * PAGE_SIZE);
            frame_put(pfns[j]);
        }
        if (cur != PFN_NONE)
            frame_put(cur);
        kerr("hyp: vm_load gpa=%lx len=%lu failed after %lu page(s): %d\n",
             (unsigned long)gpa, (unsigned long)len, (unsigned long)pages, rc);
    }

    kfree(pfns);
    return rc;
}

static void hyp_vcpu_record_fault(hyp_vcpu_t *vcpu, uint64_t scause,
                                  uint64_t stval, uint64_t htval)
{
    vcpu->exit_scause = scause;
    vcpu->exit_stval  = stval;
    vcpu->exit_htval  = htval;
    vcpu->exit        = HYP_EXIT_FAULT;
}

/* ---- SBI: the closed list of calls this slice serves ----
 *
 * Enumerated from the guest side, not guessed: the riscv64 firmware shim
 * (kernel/arch/riscv64/platform/firmware.c) is the only thing that issues an
 * ecall, and it issues exactly these seven.
 *
 *   a7=0x00 legacy set_timer      firmware.c:39   -> Sstc on this platform,
 *                                                      served through vstimecmp
 *   a7=0x01 console_putchar       firmware.c:43   -> host console
 *   a7=0x02 console_getchar       firmware.c:47   -> never called on riscv64
 *                                                      (only the aarch64
 *                                                      console reads it)
 *   a7=0x08 legacy shutdown       firmware.c:51 is SRST, so this one is dead
 *                                                      code on this platform but
 *                                                      stays for an older guest
 *   a7=0x53525354 SRST shutdown   firmware.c:51 (board poweroff, kernel/main.c)
 *   a7=0x53525354 SRST reset      firmware.c:58
 *   a7=0x735049 IPI / 0x48534d HSM hart_start
 *                                  firmware.c:86/90, called only from
 *                                  CONFIG_NR_CPUS > 1 board code
 *
 * There is NO base-extension probe anywhere in the tree: no sbi_get_version /
 * get_spec_version / get_impl_id, and firmware.h declares no base EID, so the
 * SBI spec version the guest believes it is talking to is whatever the guest
 * assumes.  Nothing to implement, and nothing to advertise.
 */
#define HYP_SBI_EID_SRST     0x53525354ULL
#define HYP_SBI_EID_HSM      0x48534dULL
#define HYP_SBI_EID_IPI      0x735049ULL
#define HYP_SBI_FID_SRST_RESET 1ULL
/* SBI error return values (SBI v2.0 spec, the errors a v1 hypervisor may
 * report); delivered in a0 like every SBI return. */
#define HYP_SBI_ERR_NOT_SUPPORTED (-2)

/* ---- the legacy / v0.2 split, which is a7 and nothing else ----
 *
 * Both conventions put the call selector in a7 and pass their arguments in
 * a0.., and BOTH are issued by the same firmware shim (firmware.c:4-15 builds
 * one ecall for all of them).  They are told apart by the VALUE of a7 alone:
 * the legacy (v0.1) EIDs are the single digits 0x00..0x0F, while every v0.2+
 * extension id is a large ASCII-derived constant -- 0x10 or above -- because
 * the spec reserves the whole 0x10..0x1F band for the base extension and
 * builds everything else out of four characters.
 *
 * This is the split the first version of this dispatcher did not make, and
 * getting it wrong is silent in the worst way: a legacy console_putchar is
 * a7=1/a6=0, which read as a v0.2 call is "extension 1, function 0" -- and
 * extension 1 IS the base extension (putchar is base fid 0x01, not 0), so
 * the guest's very first printchar fell through the whole v0.2 table and
 * exited the run with "guest SBI call eid=1 fid=0 is not implemented"
 * (.kernel-build/smoke/hyp-vcpu-riscv64.log, before this table).
 *
 * So a7 < HYP_SBI_EID_V02_BASE is legacy and takes its argument in a0 with
 * a6 MEANINGLESS (the shim passes fid=0 there for every legacy call too --
 * firmware.c:43, sbi_call(SBI_CONSOLE_PUTCHAR_EID, 0, c, 0, 0)), and only
 * a7 >= HYP_SBI_EID_V02_BASE is a v0.2 extension id whose function is a6. */
#define HYP_SBI_EID_V02_BASE 0x10ULL
#define HYP_SBI_LEGACY_SET_TIMER 0x0ULL
#define HYP_SBI_CONSOLE_GETCHAR  0x02ULL

/* The guest's copy of the Sstc comparator.  Writing it from HS-mode is what
 * "inject VSTIP" means on this platform: QEMU arms the hart's timer against
 * vstimecmp and raises mip.VSTIP when it expires (write_vstimecmp ->
 * riscv_timer_write_timecmp), and with hideleg's STI bit set that is the
 * guest's vsip.STIP, so the interrupt is delivered to VS-mode by hardware and
 * the guest's own trap handler runs it.  The register and its number are the
 * arch half's (hyp_arch_vstimecmp_*), because a generic file cannot hold a CSR
 * number: 0x24d means nothing to an architecture that has no Sstc.
 *
 * This write is only ever reached on a hart that HAS Sstc: the guest reaches
 * legacy set_timer precisely because the DTB it was handed did not advertise
 * sstc (kernel/arch/riscv64/platform/timer.c:30), and the platform that
 * publishes a DTB is the one this hypervisor runs on.  On a hart without Sstc
 * the write below would raise an illegal instruction in HS-mode, which is why
 * this is documented rather than assumed away.
 *
 * The register itself belongs to the arch half: the number 0x24d and the fact
 * that this is the VIRTUAL comparator are RISC-V facts, and a file that has to
 * assemble for architectures with no such CSR cannot hold them. */
static void hyp_write_vstimecmp(uint64_t deadline)
{
    hyp_arch_vstimecmp_set(deadline);
}

/* Dispatch table.  The legacy half is keyed on a7 alone (a0 is the only
 * argument, a6 is not a function id there); the v0.2 half is keyed on the
 * (a7, a6) pair.  See the split above HYP_SBI_EID_V02_BASE for why a7 alone
 * decides which half a call belongs to.
 *
 *   legacy a7=0x00 set_timer(a0)   arm vstimecmp, return 0, resume
 *   legacy a7=0x01 putchar(a0)      host console, resume
 *   legacy a7=0x02 getchar()        -2, resume (never called on riscv64)
 *   legacy a7=0x08 shutdown()       HYP_EXIT_SHUTDOWN
 *   eid=SRST fid=0  shutdown()      HYP_EXIT_SHUTDOWN
 *   eid=SRST fid=1  reset()         no host reboot hook in this slice: a
 *                                     recorded fault, not a silent success
 *   eid=HSM  fid=0  hart_start()    -2 (see below)
 *   eid=IPI  fid=0  send_ipi()      -2 (see below)
 * Everything else is HYP_EXIT_FAULT with the extension id recorded.
 *
 * hart_start and send_ipi are answered with SBI_ERR_NOT_SUPPORTED rather than
 * faulting because they are reachable only from CONFIG_NR_CPUS > 1 board code
 * (kernel/core/smp.c guards the whole secondary path on it, and the default is
 * 1); a spec error keeps a multi-cpu guest booting single-cpu with the failure
 * printed, where a fault would kill a run that is otherwise fine.  legacy
 * getchar gets the same answer for the same kind of reason: the aarch64
 * console reads it, the riscv64 one does not, and a guest that did call it
 * has to be told "no such call" rather than killed for asking.
 */
static int hyp_guest_ecall(hyp_vcpu_t *vcpu, uint64_t scause)
{
    uint64_t eid = vcpu->regs[17];   /* a7 */
    uint64_t fid = vcpu->regs[16];   /* a6 -- a function id in v0.2 only */

    if (eid < HYP_SBI_EID_V02_BASE) {
        switch (eid) {
        case HYP_SBI_LEGACY_SET_TIMER:
            hyp_write_vstimecmp(vcpu->regs[10]);
            vcpu->regs[10] = 0;
            break;
        case HYP_SBI_CONSOLE_PUTCHAR:
            putchar((char)(vcpu->regs[10] & 0xff));
            vcpu->regs[10] = 0;    /* the legacy convention's success value */
            break;
        case HYP_SBI_CONSOLE_GETCHAR:
            vcpu->regs[10] = (uint64_t)HYP_SBI_ERR_NOT_SUPPORTED;
            break;
        case HYP_SBI_SHUTDOWN:
            vcpu->exit_scause = scause;
            vcpu->exit = HYP_EXIT_SHUTDOWN;
            return 0;
        default:
            /* The legacy list is closed for the same reason the v0.2 one is:
             * an unknown call is a guest bug, not the next feature here. */
            kerr("hyp: legacy SBI call %lu is not implemented\n",
                 (unsigned long)eid);
            hyp_vcpu_record_fault(vcpu, scause, eid, 0);
            return 0;
        }
        vcpu->pc += 4;   /* sepc past the ecall; arch writes pc back on resume */
        return 1;
    }

    if (eid == HYP_SBI_EID_SRST) {
        if (fid == 0) {
            vcpu->exit_scause = scause;
            vcpu->exit = HYP_EXIT_SHUTDOWN;
            return 0;
        }
        if (fid == HYP_SBI_FID_SRST_RESET) {
            kwarn("hyp: guest asked for a system reset, which this slice "
                  "cannot honour\n");
            hyp_vcpu_record_fault(vcpu, scause, eid, fid);
            return 0;
        }
    } else if (eid == HYP_SBI_EID_HSM && fid == 0) {
        kwarn("hyp: guest SBI hart_start (hw_id=%lu) -> NOT_SUPPORTED, "
              "single-vcpu slice\n", (unsigned long)vcpu->regs[10]);
        vcpu->regs[10] = (uint64_t)HYP_SBI_ERR_NOT_SUPPORTED;
        vcpu->pc += 4;
        return 1;
    } else if (eid == HYP_SBI_EID_IPI && fid == 0) {
        vcpu->regs[10] = (uint64_t)HYP_SBI_ERR_NOT_SUPPORTED;
        vcpu->pc += 4;
        return 1;
    }

    /* The v0.2 subset is closed on purpose: an unknown call is a guest bug,
     * not an invitation to implement the next one here.  The extension id goes
     * in stval because that is the register the guest chose it with, and it is
     * the only detail this exit has. */
    kwarn("hyp: guest SBI call eid=%lx fid=%lu is not implemented\n",
          (unsigned long)eid, (unsigned long)fid);
    hyp_vcpu_record_fault(vcpu, scause, eid, fid);
    return 0;
}

/* ---- second-stage faults: what a guest memory access that leaves stage-1
 * walk becomes ----
 *
 * The faulting instruction has not retired.  QEMU raises the second-stage
 * exception from the load/store slow path before the access (raise_mmu_exception,
 * target/riscv/cpu_helper.c:1773-1810) and leaves pc on the instruction, so
 * the guest's destination register and its store buffer still hold their old
 * contents when this C code runs.  That is what makes the two exits below
 * legal: after a successful mapping the instruction is simply retried, and
 * after an emulated MMIO access the run loop writes the result itself and
 * steps over the instruction.
 */

/* One 64-bit word of guest memory by GUEST PHYSICAL address.  Stage-2 is the
 * only map the host has for guest addresses; a GPA it does not translate is
 * simply not readable from here. */
static int hyp_guest_read_gpa(hyp_vcpu_t *vcpu, uint64_t gpa, uint64_t *out)
{
    paddr_t pa = hyp_s2_translate(vcpu->vm, gpa);
    if (!pa)
        return 0;
    pfn_t pfn = phys_to_pfn(pa);
    if (!pfn_valid(pfn))
        return 0;
    *out = *(volatile uint64_t *)(pfn_to_virt(pfn) + (gpa & (PAGE_SIZE - 1)));
    return 1;
}

/* The guest's own stage-1, walked in software.  Once the guest kernel turns
 * paging on, its pc and its effective addresses are VS-virtual, and nothing in
 * the host can translate them: the host MMU walks the HOST's satp, and vsatp
 * roots a tree that lives in guest physical memory.  The run loop needs the
 * instruction word (to know an access's width, direction and operand) and the
 * hardware only hands it the guest PHYSICAL fault address, so the Sv39 walk
 * the guest's own MMU would do is done here, with every table fetch served by
 * stage-2.  Only the answer the decoder needs is computed: A/D/U are not
 * checked, and a walk that is wrong about permissions still yields the page the
 * hardware itself just used. */
static int hyp_guest_read_va(hyp_vcpu_t *vcpu, uint64_t va, uint64_t *out)
{
    uint64_t vsatp = hyp_arch_vsatp_get();

    uint64_t gva = va;
    uint64_t mode = (vsatp >> 60) & 0xf;
    if (mode != 0) {
        if (mode != 8)              /* Sv39 is the only guest shape this walks */
            return 0;
        uint64_t gpa = (vsatp & ((1ULL << 44) - 1)) << 12;   /* root PPN */
        /* level >= 0, not level > 0: the loop has to fetch the level-0 entry,
         * which IS the leaf.  With level > 0 it stopped after VPN2/VPN1, never
         * looked at a leaf, and `level == 0` below then refused every walk --
         * so once the guest turned paging on, nothing in it decoded and every
         * data access fell into hyp_guest_mem_fault's terminal exit.  This is
         * the shape the tree's own walks use (kernel/mm/mm.c:228,
         * kernel/mm/mm.c:554); `level > 0` is the ALLOCATION shape, where the
         * caller has already been handed the table to put a leaf in. */
        int level;
        int leaf = 0;
        unsigned leaf_level = 0;
        for (level = ARCH_PT_ROOT_LEVEL; level >= 0; level--) {
            uint64_t pte;
            if (!hyp_guest_read_gpa(vcpu, gpa +
                                    ((va >> (12 + 9 * level)) & 0x1ff) * 8, &pte))
                return 0;
            if (!(pte & PTE_V))
                return 0;
            gpa = arch_pte_addr(pte);
            if (arch_pte_is_leaf(pte)) {
                leaf = 1;
                leaf_level = (unsigned)level;
                break;
            }
        }
        if (!leaf)                /* ran out of levels without finding one */
            return 0;
        /* arch_pte_addr() is the PPN shifted up by 12, which for a leaf found
         * at level L is only the first 4KB of a 1<<(12 + 9*L) region: the rest
         * of the guest's offset inside that region lives in va itself and is
         * NOT in gpa.  ORing just the page offset therefore resolves every
         * address to the START of the superpage -- for the guest this walk
         * exists to serve (A20OS maps its whole physical window with 1GB
         * megapages) that is a different 4KB page than the one the guest is
         * executing, so the instruction could not be read back and the faulting
         * access decoded as nothing at all.  The whole covered range is what
         * goes on: PPN << 12 | va & (range - 1). */
        unsigned bits = 12 + 9 * leaf_level;
        gva = gpa | (va & ((1ULL << bits) - 1));
    }
    return hyp_guest_read_gpa(vcpu, gva, out);
}

/* What the faulting instruction was asking for.  Only the shapes a device
 * model can serve are decoded: the LOAD and STORE encodings, 32-bit and RVC,
 * with the width and signedness straight out of the RISC-V load/store funct3
 * table (lb/lh/lw and c.lw sign-extend, lbu/lhu/lwu zero-extend, ld/c.ld are
 * 8 bytes).  An AMO or an SC is NOT decoded: answering one would also have to
 * write the old value back into rd, which no device in this slice asks for,
 * so it stays a recorded fault instead of a half-implemented access.
 *
 * ilen is the ENCODED length, not the access width: an access served here is
 * stepped over rather than retried (see hyp_guest_mem_fault), and a 16-bit
 * encoding advances sepc by two.  Getting that wrong does not fault -- it just
 * resumes the guest in the middle of some other instruction. */
struct hyp_guest_access {
    uint64_t va;      /* effective address the instruction computed */
    uint64_t value;   /* store data (already the guest register) */
    unsigned rd;      /* load destination register */
    int      len;     /* access width in bytes */
    int      store;
    int      sign;    /* sign-extend the loaded value (rv64 rule) */
    int      ilen;    /* 2 for RVC, 4 for the 32-bit encodings */
};

static int hyp_guest_read_insn(hyp_vcpu_t *vcpu, uint32_t *insn)
{
    uint64_t word;
    if (!hyp_guest_read_va(vcpu, vcpu->pc, &word))
        return 0;
    /* The low halfword is enough for both encodings: a 32-bit instruction and
     * a 16-bit one are told apart by insn & 3, and every bit an RVC load/store
     * uses lives below 16.  Reading a full word at a pc whose page ends here is
     * fine too -- the G-stage fault is page granular and the tail byte is
     * never consulted. */
    *insn = (uint32_t)word;
    return 1;
}

/* Immediate field, sign-extended from its encoded width (both load and store
 * immediates are 12 bits). */
static uint64_t hyp_sext(uint64_t v, unsigned bits)
{
    return (uint64_t)((int64_t)(v << (64 - bits)) >> (64 - bits));
}

/* The 3-bit compressed register fields name x8..x15 as 0..7. */
static inline unsigned hyp_rvc_reg3(unsigned f)
{
    return 8u + (f & 7u);
}

/*
 * One 16-bit (RVC) load or store, decoded into the same shape the 32-bit
 * encodings fill in.  RVC does not give these a funct3 table to index -- every
 * encoding here carries its own immediate layout -- so the immediates are
 * assembled bit by bit from the halfword, and the 32-bit I/S-type helper
 * (hyp_sext over insn >> 20) must NOT be used on them: those fields overlap
 * differently in every one of the eight encodings.
 *
 * WHICH OF THEM EXIST IN A GUEST IS A MEASURED QUESTION.  Objdumping the
 * A20OS guest image and counting (riscv64-unknown-elf-objdump -d -M no-aliases
 * kernel-nosyms.elf | grep -oE "\bc\.[a-z0-9._]+" | sort | uniq -c) shows the
 * memory forms in use: c.ldsp 74501, c.sdsp 62465, c.ld 5848, c.lw 3699,
 * c.sd 1361, c.swsp 1301, c.sw 1065, c.lwsp 1052 -- exactly the eight decoded
 * here.  Decoding only the one instruction that first wedged the guest (a
 * c.sw) just moves the same undecodable failure to the next c.sdsp, so all
 * eight land together.
 *
 * WHAT IS DELIBERATELY NOT HERE: c.fldsp and c.fsdsp (84 and 56 occurrences).
 * The guest frame comes up with sstatus = SPP|SPIE|SIE and FS Off
 * (kernel/arch/riscv64/hyp/hyp_vcpu_asm.S programs exactly that), so a guest
 * floating-point instruction raises the GUEST's own illegal-instruction trap
 * before it can ever take a second-stage access fault -- decoding the FP forms
 * here would be unreachable code, not a missing feature.
 */
static int hyp_decode_rvc_access(hyp_vcpu_t *vcpu, uint32_t insn,
                                 struct hyp_guest_access *a)
{
    uint16_t c = (uint16_t)insn;
    unsigned funct3 = (unsigned)((c >> 13) & 7);
    unsigned rdp    = hyp_rvc_reg3((unsigned)((c >> 2) & 7));  /* inst[4:2] */
    unsigned rs1p   = hyp_rvc_reg3((unsigned)((c >> 7) & 7));  /* inst[9:7] */

    a->ilen = 2;
    a->rd   = 0;
    a->sign = 0;
    a->value = 0;

    switch (c & 3) {
    case 0:     /* quadrant 0: the *4SPN and * forms, x8..x15 only */
        switch (funct3) {
        case 0x0: return 0;    /* c.addi4spn: register-immediate, no access */
        case 0x2: {            /* c.lw  off[5:3]=inst[12:10] [2]=inst[6] [6]=inst[5] */
            uint64_t off = ((uint64_t)((c >> 10) & 7) << 3) |
                           ((uint64_t)((c >> 6) & 1) << 2) |
                           ((uint64_t)((c >> 5) & 1) << 6);
            a->va = vcpu->regs[rs1p] + off;
            a->len = 4; a->store = 0; a->sign = 1; a->rd = rdp;
            return 1;
        }
        case 0x3: {            /* c.ld  off[5:3]=inst[12:10] [7:6]=inst[6:5] */
            uint64_t off = ((uint64_t)((c >> 10) & 7) << 3) |
                           ((uint64_t)((c >> 5) & 3) << 6);
            a->va = vcpu->regs[rs1p] + off;
            a->len = 8; a->store = 0; a->sign = 0; a->rd = rdp;
            return 1;
        }
        /* Quadrant 0's floating-point slot is funct3=0x1 (c.fld); 0x5 is
         * c.fsd.  Only one of the two is spelled out here and both answer
         * the same thing -- not decoded -- so name the one this line is, and
         * let 0x1 fall to the default below rather than leaving a reader to
         * assume the FP forms were enumerated. */
        case 0x5: return 0;    /* c.fsd: floating point, see above */
        case 0x6: {            /* c.sw  same offsets as c.lw, rs2' = inst[4:2] */
            uint64_t off = ((uint64_t)((c >> 10) & 7) << 3) |
                           ((uint64_t)((c >> 6) & 1) << 2) |
                           ((uint64_t)((c >> 5) & 1) << 6);
            a->va = vcpu->regs[rs1p] + off;
            a->len = 4; a->store = 1; a->value = vcpu->regs[rdp];
            return 1;
        }
        case 0x7: {            /* c.sd  same offsets as c.ld */
            uint64_t off = ((uint64_t)((c >> 10) & 7) << 3) |
                           ((uint64_t)((c >> 5) & 3) << 6);
            a->va = vcpu->regs[rs1p] + off;
            a->len = 8; a->store = 1; a->value = vcpu->regs[rdp];
            return 1;
        }
        default: return 0;
        }

    case 2:     /* quadrant 2: the *SP forms, full 5-bit registers off x2 */
        switch (funct3) {
        case 0x0: return 0;    /* c.slli */
        case 0x1: return 0;    /* c.fldsp: floating point, see above */
        case 0x2: {            /* c.lwsp rd=inst[11:7]
                                *   off[5]=inst[12] [4:2]=inst[6:4] [7:6]=inst[3:2] */
            uint64_t off = ((uint64_t)((c >> 2) & 3) << 6) |
                           ((uint64_t)((c >> 12) & 1) << 5) |
                           ((uint64_t)((c >> 4) & 7) << 2);
            a->va = vcpu->regs[2] + off;
            a->len = 4; a->store = 0; a->sign = 1;
            a->rd = (unsigned)((c >> 7) & 0x1f);
            return 1;
        }
        case 0x3: {            /* c.ldsp rd=inst[11:7]
                                *   off[5]=inst[12] [4:3]=inst[6:5] [8:6]=inst[4:2] */
            uint64_t off = ((uint64_t)((c >> 2) & 7) << 6) |
                           ((uint64_t)((c >> 12) & 1) << 5) |
                           ((uint64_t)((c >> 5) & 3) << 3);
            a->va = vcpu->regs[2] + off;
            a->len = 8; a->store = 0; a->sign = 0;
            a->rd = (unsigned)((c >> 7) & 0x1f);
            return 1;
        }
        case 0x5: return 0;    /* c.fsdsp: floating point, see above */
        case 0x6: {            /* c.swsp rs2=inst[6:2]
                                *   off[5:2]=inst[12:9] [7:6]=inst[8:7] */
            uint64_t off = ((uint64_t)((c >> 7) & 3) << 6) |
                           ((uint64_t)((c >> 9) & 0xf) << 2);
            a->va = vcpu->regs[2] + off;
            a->len = 4; a->store = 1;
            a->value = vcpu->regs[(unsigned)((c >> 2) & 0x1f)];
            return 1;
        }
        case 0x7: {            /* c.sdsp rs2=inst[6:2]
                                *   off[5:3]=inst[12:10] [8:6]=inst[9:7] */
            uint64_t off = ((uint64_t)((c >> 7) & 7) << 6) |
                           ((uint64_t)((c >> 10) & 7) << 3);
            a->va = vcpu->regs[2] + off;
            a->len = 8; a->store = 1;
            a->value = vcpu->regs[(unsigned)((c >> 2) & 0x1f)];
            return 1;
        }
        default: return 0;
        }

    default:
        /* Quadrant 1 (c.addi, c.li, c.j, c.bnez, the MISC-ALU group, ...) has
         * no load or store in it at all, and c.addi4spn above is the only
         * quadrant-0 form that looks load-shaped and is not. */
        return 0;
    }
}

static int hyp_decode_guest_access(hyp_vcpu_t *vcpu, struct hyp_guest_access *a)
{
    uint32_t insn;
    if (!hyp_guest_read_insn(vcpu, &insn))
        return 0;
    if ((insn & 3) != 3)
        return hyp_decode_rvc_access(vcpu, insn, a);

    a->ilen = 4;

    unsigned opcode = insn & 0x7f;
    unsigned funct3 = (insn >> 12) & 7;
    unsigned rs1    = (insn >> 15) & 0x1f;
    unsigned rd     = (insn >> 7) & 0x1f;
    uint64_t base   = vcpu->regs[rs1];

    if (opcode == 0x03) {                       /* LOAD: imm is I-type, 12-bit */
        switch (funct3) {
        case 0: a->len = 1; a->sign = 1; break;  /* lb  */
        case 1: a->len = 2; a->sign = 1; break;  /* lh  */
        case 2: a->len = 4; a->sign = 1; break;  /* lw  */
        case 3: a->len = 8; a->sign = 0; break;  /* ld  */
        case 4: a->len = 1; a->sign = 0; break;  /* lbu */
        case 5: a->len = 2; a->sign = 0; break;  /* lhu */
        case 6: a->len = 4; a->sign = 0; break;  /* lwu */
        default: return 0;
        }
        a->va    = base + hyp_sext((uint64_t)(insn >> 20), 12);
        a->store = 0;
        a->rd    = rd;
        a->value = 0;
        return 1;
    }

    if (opcode == 0x23) {                       /* STORE: imm is S-type, 12-bit */
        switch (funct3) {
        case 0: a->len = 1; break;               /* sb */
        case 1: a->len = 2; break;               /* sh */
        case 2: a->len = 4; break;               /* sw */
        case 3: a->len = 8; break;               /* sd */
        default: return 0;
        }
        uint32_t imm = (uint32_t)(((insn >> 25) << 5) | ((insn >> 7) & 0x1f));
        a->va    = base + hyp_sext(imm, 12);
        a->store = 1;
        a->rd    = 0;
        a->sign  = 0;
        a->value = vcpu->regs[(insn >> 20) & 0x1f];   /* rs2 */
        return 1;
    }

    return 0;
}

/* The rv64 rule for what a load leaves in rd: the low len bytes, sign-extended
 * for lb/lh/lw and zero-extended for lbu/lhu/lwu/ld. */
static uint64_t hyp_extend_loaded(uint64_t value, int len, int sign)
{
    uint64_t mask = (len >= 8) ? ~0ULL : ((1ULL << (len * 8)) - 1);
    value &= mask;
    if (sign && len < 8 && (value & (1ULL << (len * 8 - 1))))
        value |= ~mask;
    return value;
}

/* One guest memory access that could not be walked by stage-2.  Two exits,
 * in the order the contract fixes them:
 *
 *   gpa inside the RAM window -> hyp_ram_fill(), and the guest retries the
 *     instruction itself.  A fill that fails is NOT papered over: the window
 *     owns that address, so the answer is a recorded fault.
 *   otherwise -> hyp_dev_mmio().  The access is served HERE (the model returns
 *     the data, or takes the value), so the instruction is stepped over
 *     instead of retried -- retrying would fault again on the same unmapped
 *     page and loop forever.
 */
static int hyp_guest_mem_fault(hyp_vcpu_t *vcpu, uint64_t scause,
                               uint64_t htval)
{
    /* htval is the guest physical address of the faulting access, shifted
     * right by two.  QEMU composes it as (im_address | (address &
     * (TARGET_PAGE_SIZE - 1))) >> 2 (target/riscv/cpu_helper.c:1997, the
     * G-stage branch of riscv_cpu_tlb_fill), so htval << 2 carries the
     * INTRA-PAGE OFFSET as well as the page -- it is not a page base.  The
     * offset is masked off here because every consumer below wants a page:
     * hyp_ram_fill() rejects a non-page-aligned gpa outright (-EINVAL), and
     * the fetches of a page-granular table are keyed by page.
     *
     * The intra-page offset for the reported GPA comes from the instruction's
     * effective address instead, which is the same thing QEMU ORs in:
     * translation is page granular, so the offset the guest computed survives
     * into the GPA unchanged. */
    uint64_t gpage = (htval << 2) & ~(uint64_t)(PAGE_SIZE - 1);

    if (scause == HYP_SCAUSE_GPF_INST) {
        /* A fetch has neither width nor operand, so the device model is not
         * its place; a missing code page is the RAM window's to answer, and if
         * it cannot, executing a zero page is the guest's own illegal
         * instruction trap to report. */
        int rc = hyp_ram_fill(vcpu->vm, gpage);
        if (rc == 0)
            return 1;
        kerr("hyp: guest instruction fetch fault gpa=%lx pc=%lx rc=%d\n",
             (unsigned long)gpage, (unsigned long)vcpu->pc, rc);
        hyp_vcpu_record_fault(vcpu, scause, gpage, htval);
        return 0;
    }

    struct hyp_guest_access a;
    if (!hyp_decode_guest_access(vcpu, &a)) {
        /* The raw word rides along because this line is the only thing between
         * "the guest stopped here" and knowing WHICH encoding it stopped on:
         * a 16-bit halfword disassembled by eye names the missing form in one
         * step, and the guess is not worth making -- an access this model
         * cannot decode is a recorded fault, never an improvised one. */
        uint32_t raw = 0;
        hyp_guest_read_insn(vcpu, &raw);
        kerr("hyp: undecodable guest access at pc=%lx (insn=%x%s)\n",
             (unsigned long)vcpu->pc, raw,
             (raw & 3) == 3 ? "" : " rvc");
        hyp_vcpu_record_fault(vcpu, scause, gpage, htval);
        return 0;
    }
    uint64_t gpa = gpage | (a.va & (PAGE_SIZE - 1));

    int rc = hyp_ram_fill(vcpu->vm, gpage);
    if (rc == 0)
        return 1;                 /* RAM: the guest retries the instruction */
    if (rc != -EFAULT) {
        kerr("hyp: guest RAM fill gpa=%lx pc=%lx rc=%d\n",
             (unsigned long)gpage, (unsigned long)vcpu->pc, rc);
        hyp_vcpu_record_fault(vcpu, scause, gpa, htval);
        return 0;
    }

    uint64_t value = a.value;
    if (!hyp_dev_mmio(vcpu->vm, gpa, a.store, &value, a.len)) {
        kerr("hyp: no device for guest %s of %d byte(s) at gpa=%lx\n",
             a.store ? "store" : "load", a.len, (unsigned long)gpa);
        hyp_vcpu_record_fault(vcpu, scause, gpa, htval);
        return 0;
    }

    /* rd 0 is not a discard sink on this path, it is x0 -- hardwired zero in
     * hardware, so a load into it leaves no trace and the register file must
     * not be touched at all.  The guard is explicit rather than borrowed from
     * hyp_frame_to_vcpu() re-zeroing regs[0] on every guest trap: that is an
     * invariant in a different file, and a writeback that depended on it would
     * silently corrupt a banked x0 the day it stopped holding. */
    if (!a.store && a.rd)
        vcpu->regs[a.rd] = hyp_extend_loaded(value, a.len, a.sign);
    /* The access happened; do not redo it -- and step over the ENCODED length,
     * which is 2 for the RVC encodings.  A fixed +4 here resumes the guest
     * half-way into whatever follows, which shows up much later as a stray
     * illegal instruction or a walk into somebody else's function rather than
     * as anything to do with this line. */
    vcpu->pc += (uint64_t)a.ilen;
    return 1;
}

/* ---- scause 22: a VS-mode instruction trapped to HS-mode ----
 *
 * With hstatus.VTVM=1 the guest cannot touch its own memory-management CSRs,
 * so a real guest kernel raises this the first time it writes satp (QEMU
 * gates the satp CSR on VTVM, target/riscv/csr.c:626-634; SFENCE.VMA is gated
 * the same way).  The instruction arrives in the trap, not only in memory:
 * QEMU puts env->bins into stval for a virtual instruction fault
 * (cpu_helper.c:2405-2407), and htinst carries the same word when the guest
 * faults are reported with a transformed instruction.  Decoding stval first is
 * therefore both cheaper and more robust than a memory read -- it still works
 * when the guest's own page tables are what just broke -- and the memory read
 * stays as the fallback for a platform that leaves stval at zero.
 *
 * hstatus.SPV is 1 on this path, so this IS a guest trap: decoding and
 * re-executing one instruction on the guest's behalf is the same contract the
 * ecall path already runs under.
 */
#define HYP_CSR_SATP      0x180
#define HYP_CSR_VSTIMECMP 0x14d
#define HYP_OP_SYSTEM     0x73
/* SYSTEM funct3=0 carries the operation in imm[11:0], and the two that matter
 * here are distinct values: WFI is 0x105, SFENCE.VMA is funct7=0x09 with rs1
 * and rd riding in the operand fields (which is why the all-zero form is
 * 0x120 -- the boot code's "sfence.vma" at 0x8020011e encodes as 0x12000073,
 * the same bits a WFI-shaped imm would occupy but a different instruction).
 * ECALL (0x000) and EBREAK (0x001) are NOT ours to step over: the first
 * arrives as an ecall cause, the second is delegated. */
#define HYP_SYS_WFI        0x105u
#define HYP_SYS_FENCE_F7   0x09u

static void hyp_write_vsatp(uint64_t v)
{
    hyp_arch_vsatp_set(v);
    /* Changing the VS-stage root does NOT implicitly drop the entries cached
     * under the old one.  The guest's satp write arrives here rather than
     * reaching the CSR directly (hstatus.VTVM traps it), so this is the only
     * place the switch happens and the only place the matching fence can be
     * issued: without it this hart keeps resolving guest addresses through the
     * page tables the guest just abandoned.  The guest's own SFENCE.VMA (also
     * trapped, see hyp_emulate_virt_inst) covers the edits it makes after the
     * switch; this covers the switch itself. */
    hyp_arch_host_tlb_fence();
}

/* The trapped instruction, from the trap report when it carries one.
 * QEMU's stval for this cause is env->bins, so a word whose low two bits are
 * 11 and whose opcode is SYSTEM is the instruction itself; anything else (a
 * zero, or a transformed-instruction pseudo-op such as the 0x00003000 the
 * G-stage walk failures carry) is not decodable and the memory read decides. */
static int hyp_virt_inst_word(hyp_vcpu_t *vcpu, uint64_t stval,
                              uint32_t *insn)
{
    if (stval && (stval & 3) == 3 &&
        (stval & 0x7f) == HYP_OP_SYSTEM) {
        *insn = (uint32_t)stval;
        return 1;
    }
    return hyp_guest_read_insn(vcpu, insn);
}

static int hyp_emulate_virt_inst(hyp_vcpu_t *vcpu, uint64_t stval)
{
    uint32_t insn;
    if (!hyp_virt_inst_word(vcpu, stval, &insn))
        return -1;

    unsigned opcode = insn & 0x7f;
    unsigned funct3 = (insn >> 12) & 7;
    if (opcode != HYP_OP_SYSTEM)
        return -1;

    if (funct3 == 0) {
        unsigned imm   = (insn >> 20) & 0xfff;
        unsigned f7    = (insn >> 25) & 0x7f;

        if (imm == HYP_SYS_WFI) {
            /* WFI, treated as recoverable and stepped over.  The semantic
             * trade-off is deliberate and narrow: this slice gives the
             * guest's idle loop NO power semantics -- the instruction is
             * consumed and the guest keeps running.  That is sound here
             * because hstatus.VTW is left clear, so a guest WFI is not
             * trapped by the hardware in the first place and this branch
             * only exists for a platform that does gate it.  Where it
             * matters, the guest's idle spin is woken by its own DELEGATED
             * interrupts (hideleg carries the VS timer, bits 2/6/10), so
             * "do not sleep" costs a spinning guest, not a lost wakeup. */
            vcpu->pc += 4;
            return 0;
        }
        if (f7 == HYP_SYS_FENCE_F7) {
            /* SFENCE.VMA.  In HS-mode it is not gated, and it invalidates the
             * whole hart's translations, which is a superset of what the guest
             * asked for. */
            hyp_arch_host_tlb_fence();
            vcpu->pc += 4;
            return 0;
        }
        if (imm == 0 || imm == 1)
            return -1;       /* ECALL / EBREAK: not this path's to step over */
        kerr("hyp: guest virtual instruction %08lx is not a SYSTEM CSR op "
             "this slice emulates\n", (unsigned long)insn);
        return -1;
    }

    if (funct3 != 1 && funct3 != 2 && funct3 != 3 &&
        funct3 != 5 && funct3 != 6 && funct3 != 7)
        return -1;

    unsigned rs1 = (insn >> 15) & 0x1f;
    unsigned rd  = (insn >> 7) & 0x1f;
    unsigned csr = insn >> 20;
    uint64_t src = (funct3 >= 5) ? (uint64_t)rs1 : vcpu->regs[rs1];
    uint64_t old, next;

    if (csr == HYP_CSR_SATP) {
        uint64_t v = hyp_arch_vsatp_get();
        old = v;
        next = (funct3 == 1) ? src
             : (funct3 == 2) ? (old | src)
             : (old & ~src);
        hyp_write_vsatp(next);
    } else if (csr == HYP_CSR_VSTIMECMP) {
        /* A set/clear form on a CSR that only ever takes a value is refused
         * rather than approximated.  Reading the old value back is only done
         * when the instruction asks for it in rd, and it is safe to ask: a
         * virtual instruction fault out of this CSR is only reachable when the
         * Sstc extension is implemented at all (the gate checks ext_sstc
         * before the henvcfg test, target/riscv/csr.c:572-614). */
        if (funct3 != 1 && funct3 != 5)
            return -1;
        if (rd) {
            old = hyp_arch_vstimecmp_get();
        } else {
            old = 0;
        }
        next = src;
        hyp_write_vstimecmp(next);
    } else {
        /* Every other CSR is a recorded fault, never a silent pass-through:
         * the alternative is to let the guest believe a VS-mode CSR write
         * took effect when it did not, which is the failure mode this whole
         * emulation path exists to avoid.  The CSR number goes to stval --
         * that is the register the guest chose it with -- and the kerr names
         * it so the log says which one. */
        kerr("hyp: guest virtual instruction wrote CSR 0x%03x, which this "
             "slice does not emulate\n", csr);
        return -1;
    }

    if (rd)
        vcpu->regs[rd] = old;
    vcpu->pc += 4;
    return 0;
}

/* ---- bounded guest-trap trace ----
 *
 * A guest that stops talking looks identical from outside whether it is
 * wedged in one of its own loops or re-executing a trapped instruction the
 * host keeps re-trapping, and neither shape prints anything: the run loop
 * only speaks on a terminal exit, which neither of them reaches.  So the
 * dispatcher says where the guest was, twice over and both bounded:
 *
 *   - the first HYP_TRAP_TRACE_HEAD guest traps, in order, which bounds how
 *     far into the boot the guest actually got;
 *   - one line the first time any single (pc, scause) pair recurs
 *     HYP_TRAP_TRACE_REPEAT times, which names a trap loop outright.
 *
 * Both are counters on a slice that runs one guest on one CPU, which is the
 * same scope hyp_active_vcpu already claims for itself, so no locking is
 * needed and none is taken. */
#define HYP_TRAP_TRACE_HEAD   24
#define HYP_TRAP_TRACE_REPEAT 256

static uint64_t g_hyp_trap_seen;      /* guest traps since the run started   */
static uint64_t g_hyp_trap_loop_pc;
static uint64_t g_hyp_trap_loop_scause;
static uint64_t g_hyp_trap_loop_run;

int hyp_vcpu_handle_trap(hyp_vcpu_t *vcpu, void *trap_frame,
                         uint64_t scause, uint64_t stval, uint64_t htval)
{
    (void)trap_frame;

    if (!vcpu || vcpu->magic != HYP_VCPU_MAGIC || !vcpu->running)
        return 0;

    /* Move host keystrokes into the guest's ingress ring, here, on every guest
     * trap -- before the cause is looked at at all.  Two things pin the spot,
     * and both are easy to get wrong:
     *
     *   BEFORE the host-IRQ early return below.  A host UART IRQ reaches HS
     *   with the interrupt bit set in scause, so that early return is exactly
     *   the path a new byte takes, and it is also the path the counters below
     *   never see (they start after it).  Pumping after it would mean a byte
     *   is invisible to the guest until some LATER trap came along, which is
     *   not a latency anyone can reason about.
     *
     *   AFTER the magic/running check above, because it dereferences
     *   vcpu->vm.  That is why this is not "pump once per run" somewhere
     *   convenient: by this point the VM is known good, and the call cannot
     *   fail.
     *
     * It is a drain of a non-blocking reader (hyp_dev.c spells out which one
     * and why), so an empty host ring costs one atomic load and a return. */
    hyp_dev_pump_rx(vcpu->vm);

    /* Host IRQ.  The host IRQ machinery has already run against this frame
     * by the time the dispatcher gets here; the guest just resumes. */
    if (scause & HYP_SCAUSE_INTR_BIT)
        return 1;

    /* A guest ecall is the guest TALKING, and the SBI dispatcher answers it
     * every time -- the vcpu test's guest spells one line this way.  Printing
     * a trace line between the character and the next one does not just add
     * noise, it splits the guest's own output across host lines: the line
     * smoke-hyp-vcpu anchors on (^HYP$) can then never appear, however
     * correct the SBI path underneath it is.  The trace exists to say where a
     * guest that STOPS got to, and an ecall is not that, so the handled
     * ecall causes stay out of both counters below.  Everything else -- page
     * faults, virtual instructions, unhandled causes -- is still traced. */
    uint64_t cause = scause & ~HYP_SCAUSE_INTR_BIT;
    int traced = (cause != HYP_SCAUSE_ECALL_VU &&
                  cause != HYP_SCAUSE_ECALL_VS &&
                  cause != HYP_SCAUSE_ECALL_S);

    if (traced && g_hyp_trap_seen < HYP_TRAP_TRACE_HEAD) {
        kerr("hyp: trap #%lu scause=%lx pc=%lx stval=%lx htval=%lx\n",
             (unsigned long)g_hyp_trap_seen, (unsigned long)scause,
             (unsigned long)vcpu->pc, (unsigned long)stval,
             (unsigned long)htval);
    }
    if (traced)
        g_hyp_trap_seen++;

    if (traced && vcpu->pc == g_hyp_trap_loop_pc &&
        scause == g_hyp_trap_loop_scause) {
        if (++g_hyp_trap_loop_run == HYP_TRAP_TRACE_REPEAT) {
            kerr("hyp: trap loop at pc=%lx scause=%lx (%lu traps so far)\n",
                 (unsigned long)vcpu->pc, (unsigned long)scause,
                 (unsigned long)g_hyp_trap_seen);
        }
    } else {
        g_hyp_trap_loop_pc    = vcpu->pc;
        g_hyp_trap_loop_scause = scause;
        g_hyp_trap_loop_run   = 1;
    }

    switch (scause & ~HYP_SCAUSE_INTR_BIT) {
    case HYP_SCAUSE_ECALL_VU:
    case HYP_SCAUSE_ECALL_VS:
    case HYP_SCAUSE_ECALL_S:
        return hyp_guest_ecall(vcpu, scause);

    case HYP_SCAUSE_GPF_INST:
    case HYP_SCAUSE_GPF_LOAD:
    case HYP_SCAUSE_GPF_STORE:
        return hyp_guest_mem_fault(vcpu, scause, htval);

    case HYP_SCAUSE_VIRT_INST:
        if (hyp_emulate_virt_inst(vcpu, stval) == 0)
            return 1;
        /* Whatever else the guest did that traps here, this loop does not
         * know how to be the hypervisor for. */
        kerr("hyp: guest virtual instruction fault not emulated "
             "(pc=%lx stval=%lx)\n", (unsigned long)vcpu->pc,
             (unsigned long)stval);
        hyp_vcpu_record_fault(vcpu, scause, stval, htval);
        return 0;

    case HYP_SCAUSE_FETCH_PF:
    case HYP_SCAUSE_LOAD_PF:
    case HYP_SCAUSE_STORE_PF:
        /* The guest's own stage-1 fault.  v2 delegates these to VS-mode, so
         * reaching here means the delegation is not in force; the host has no
         * second page table to fill for the guest's user address space, so the
         * run exits with the fault recorded rather than pretending. */
        kerr("hyp: undelegated guest page fault scause=%lu stval=%lx\n",
             (unsigned long)scause, (unsigned long)stval);
        hyp_vcpu_record_fault(vcpu, scause, stval, htval);
        return 0;

    default:
        kerr("hyp: unhandled guest trap scause=%lu stval=%lx\n",
             (unsigned long)scause, (unsigned long)stval);
        hyp_vcpu_record_fault(vcpu, scause, stval, htval);
        return 0;
    }
}

int hyp_vcpu_run(hyp_vcpu_t *vcpu)
{
    if (!vcpu || vcpu->magic != HYP_VCPU_MAGIC)
        return -EINVAL;
    if (!vcpu->vm || vcpu->vm->magic != HYP_VM_MAGIC)
        return -EINVAL;

    /* Claim the single guest slot before anything else: a setup that is
     * about to program this CPU's virtualization state must not race a
     * second CPU claiming the same slice.  The window is a test-and-set, and
     * it has to be a LOCKED one: arch_local_irq_disable() only closes the
     * window on the CPU that runs it, so a plain IRQ-off read-modify-write
     * leaves two CPUs free to both find the slot free and then both program
     * hgatp/hstatus and sret into VS-mode on the same guest slot.  This is
     * the same idiom the frame allocator uses for its own exclusive claim
     * (frame.c, spin_lock_irqsave(&pfa.lock)), and for the same reason: the
     * lock is a real cross-CPU lock, not a local one.
     *
     * Nothing holds the lock across the guest -- it is taken to claim and
     * released before hyp_arch_vcpu_enter() -- so this is not a lock held
     * across arbitrary guest execution, which is why trylock (never sleep,
     * never wait for the owner) is the right shape here: losing the race
     * means answering -EBUSY, not waiting. */
    uint64_t claim_flags;
    if (!spin_trylock_irqsave(&g_hyp_active_lock, &claim_flags)) {
        kerr("hyp: vcpu_run refused, guest slot claimed elsewhere\n");
        return -EBUSY;
    }
    if (hyp_active_vcpu) {
        spin_unlock_irqrestore(&g_hyp_active_lock, claim_flags);
        kerr("hyp: vcpu_run refused, guest already live\n");
        return -EBUSY;
    }
    hyp_active_vcpu = vcpu;
    spin_unlock_irqrestore(&g_hyp_active_lock, claim_flags);

    /* Kept for the two IRQ-off windows around vcpu->running below, which
     * guard a plain store this CPU's own dispatcher reads, not shared state. */
    int irqs_were_on = arch_irqs_enabled();

    vcpu->exit        = HYP_EXIT_NONE;
    vcpu->exit_scause = 0;
    vcpu->exit_stval  = 0;
    vcpu->exit_htval  = 0;

    /* The trace describes one run, so it starts empty with it. */
    g_hyp_trap_seen      = 0;
    g_hyp_trap_loop_pc   = 0;
    g_hyp_trap_loop_scause = 0;
    g_hyp_trap_loop_run  = 0;

    int rc = hyp_arch_vcpu_setup(vcpu);
    if (rc != 0) {
        kerr("hyp: arch_vcpu_setup failed: %d\n", rc);
        goto out;
    }

    /* running and active go up together: the trap dispatcher reads the pair,
     * and between them the host must see a complete "a guest is live"
     * statement, never half of one. */
    arch_local_irq_disable();
    vcpu->running = 1;
    if (irqs_were_on)
        arch_local_irq_enable();

    hyp_arch_vcpu_enter(vcpu);   /* returns only through the exit path */

    arch_local_irq_disable();
    vcpu->running = 0;
    if (irqs_were_on)
        arch_local_irq_enable();

    /* A guest shutdown and a guest fault are both terminal exits, and both
     * are reported through vcpu->exit; a negative return is reserved for the
     * host failing to run the guest at all.  Enter coming back with nothing
     * recorded is exactly that. */
    if (vcpu->exit == HYP_EXIT_NONE || vcpu->exit == HYP_EXIT_ERROR)
        rc = -EIO;
    else
        rc = 0;

out:
    /* Release the claim under the lock it was taken with, so another CPU's
     * claim cannot interleave with this one.  The restore flags come from
     * this lock acquisition, not from the claim above: the interrupt state
     * then is not the one the claim found. */
    uint64_t release_flags = spin_lock_irqsave(&g_hyp_active_lock);
    hyp_active_vcpu = NULL;
    spin_unlock_irqrestore(&g_hyp_active_lock, release_flags);
    return rc;
}