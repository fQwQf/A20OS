/*
 * RISC-V H-extension (hypervisor extension) arch half.  The kernel runs in
 * HS-mode, which is exactly the mode this extension arms: hgatp names the
 * stage-2 root, hfence.gvma invalidates guest translations, and hstatus/
 * hedeleg shape what the guest may trap.  The vcpu enter/exit assembly is
 * the next slice (docs/hypervisor/00-design.md S5); this file is the part
 * the stage-2 memory path needs.
 */
#include "hyp/hyp_arch.h"
#include "arch/riscv64/include/platform.h"
#include "arch/riscv64/include/page_table.h"
#include "mm/frame.h"
#include "core/stdio.h"

/* hstatus exists only when the H extension is implemented, so reading it IS
 * the probe.  (csrr misa was tried first and turned out to raise an
 * illegal-instruction trap under QEMU's rv64 CPU -- the spec calls misa
 * any-privilege, so the reason is unresolved; the CSR the extension itself
 * adds is the honest probe.)  Read once at first use. */
static int hyp_probe(void)
{
    static int probed = -1;
    if (probed < 0) {
        uint64_t hstatus;
        __asm__ volatile("csrr %0, hstatus" : "=r"(hstatus));
        (void)hstatus;
        probed = 1;
    }
    return probed;
}

int hyp_supported(void)
{
    return hyp_probe();
}

/*
 * hgatp layout (RV64): MODE[63:60] | VMID[59:44] | PPN[43:0].
 * MODE 8 = Sv39x4.  VMID is implementation-defined in width; writing our
 * 16-bit counter into the WARL field keeps only the bits the hart
 * implements, so two VMs whose tags alias in the hardware VMID would share
 * a TLB tag -- hyp_next_vmid() wrapping is the documented limit that leads
 * there, and a full hfence on wrap is the cheap future fix.
 */
static uint64_t hyp_make_hgatp(pte_t *root, uint16_t vmid)
{
    uint64_t ppn = (uint64_t)virt_to_pfn(root);
    return (8ULL << 60) | ((uint64_t)vmid << 44) | ppn;
}

void hyp_arch_vmid_fenced(uint16_t vmid)
{
    /* Nothing has run under this VMID yet, so no translation can exist --
     * the fence is for the FIRST activation, which programs hgatp anyway.
     * Kept as a hook so VMID reuse (if ever adopted) has one place to
     * fence.  The hgatp value itself is staged for the run loop: */
    (void)vmid;
    (void)hyp_make_hgatp;
}

void hyp_arch_s2_fence(uint16_t vmid, uint64_t gpa)
{
    /* hfence.gvma rs1, rs2: rs1 = gpa>>2 (page-granular), rs2 = VMID.
     * Emitted through .insn because the tree-wide -march does not carry
     * the `h` extension and the assembler rejects the mnemonic; .insn is a
     * directive, not an opcode, so it encodes without the extension:
     * R-type, opcode=SYSTEM(0x73), func3=0, func7=0110001, rd=x0. */
    uint64_t page = gpa >> 2;
    __asm__ volatile(".insn r 0x73, 0, 0x31, x0, %0, %1"
                     :: "r"(page), "r"((uint64_t)vmid) : "memory");
}
