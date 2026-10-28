//! Checkpoint image validation and the register-frame mapping shared by
//! ptrace, coredump and the service checkpoint/restore path
//! (kernel/proc/checkpoint.c, kernel/proc/ckpt_restore.c, ptrace get/setregs
//! and coredump via kernel/arch/riscv64/include/trap_frame.h).
//!
//! Why this layer exists: the user trap-frame has two logical owners for the
//! same storage.  On rv64 frame slot `x[0]` is NOT a general register
//! (hardware x0 is hardwired) -- it is the address-space-token scratch the
//! return path reloads satp from.  A flat x[32] model lets a regset import
//! clobber the token (the restored task then resumes on the checkpointed
//! task's stale pgdir and fault-loops) and lets an export leak the kernel
//! token into ptrace results, coredumps and checkpoint images.  Modeling the
//! frame as `addrspace_token` + `gprs[31]` makes both failure modes
//! unrepresentable: export reports hardware semantics (x[0] == 0), and
//! import has no access path to the token field.
//!
//! C keeps the ptrace freeze, the fd-table walk, all VFS I/O, frame
//! allocation and page-table population.  This module owns the image-format
//! checks and the register mapping; the pure parts run as host unit tests
//! (tools/test-ckpt-image-host.sh).

#![no_std]
#![allow(non_snake_case)]

pub const A20_CKPT_MAGIC: u64 = 0x3154_504b_4330_3241; // "A20CKPT1"
pub const A20_CKPT_VERSION: u32 = 1;
pub const CKPT_R_MAX_VMA: u32 = 1024;
pub const CKPT_R_MAX_FD: u32 = 256;

// errno values mirror kernel/include/core/errno.h (Linux numbering).
const ENOEXEC: i32 = 8;
const EOPNOTSUPP: i32 = 95;

// VM_* bits mirror kernel/include/mm/vm.h.
const VM_READ: u64 = 1 << 0;
const VM_WRITE: u64 = 1 << 1;
const VM_EXEC: u64 = 1 << 2;
const VM_SHARED: u64 = 1 << 3;
const VM_GUARD: u64 = 1 << 6;

/// Mirror of proc_debug_regs_t (kernel/include/proc/debug_regs.h).
/// The C header carries static asserts for the field offsets; keep in sync.
#[repr(C)]
pub struct ProcDebugRegs {
    pub regs: [u64; 32],
    pub pc: u64,
    pub sp: u64,
    pub status: u64,
    pub orig_syscall: u64,
    pub fp: [u64; 64],
    pub fcsr: u64,
}

const _: () = assert!(core::mem::size_of::<ProcDebugRegs>() == 808);

/// rv64 trap_context_t, modeled by OWNERSHIP: slot x[0] is the
/// address-space-token scratch, not a GPR, so it is a named field the GPR
/// importer cannot reach.  Layout mirrors
/// kernel/arch/riscv64/include/trap_frame.h (asserted in ckpt_image.h).
#[repr(C, align(16))]
pub struct RestoreFrame {
    pub addrspace_token: u64, // rv64 frame slot x[0] (KScratch0): satp scratch
    pub gprs: [u64; 31],      // x[1]..x[31]
    pub sstatus: u64,
    pub sepc: u64,
    pub last_a0: u64,
    pub kernel_tp: u64,
    pub fregs: [u64; 32],
    pub fcsr: u64,
    pub reserved: u64,
}

const _: () = assert!(core::mem::size_of::<RestoreFrame>() == 560);

/// Field mapping between the exported register file and the frame.
/// Shared by RestoreFrame::install_gprs and the ptrace-facing import below:
/// slot x[0] is never touched by either.
fn import_gprs(frame: &mut RestoreFrame, regs: &ProcDebugRegs) {
    frame.gprs.copy_from_slice(&regs.regs[1..32]);
    frame.sepc = regs.pc;
    frame.gprs[1] = regs.sp; // x[2]
    frame.sstatus = regs.status;
    frame.gprs[16] = regs.orig_syscall; // x[17]
    frame.fregs.copy_from_slice(&regs.fp[..32]);
    frame.fcsr = regs.fcsr;
}

fn export_gprs(frame: &RestoreFrame, regs: &mut ProcDebugRegs) {
    *regs = ProcDebugRegs {
        regs: [0; 32],
        pc: frame.sepc,
        sp: frame.gprs[1], // x[2]
        status: frame.sstatus,
        orig_syscall: frame.gprs[16], // x[17]
        fp: [0; 64],
        fcsr: frame.fcsr,
    };
    regs.regs[1..32].copy_from_slice(&frame.gprs);
    regs.fp[..32].copy_from_slice(&frame.fregs);
    // regs.regs[0] stays 0: hardware x0 is hardwired zero, and the kernel
    // reuses the frame slot as the address-space-token scratch.  Observers
    // (ptrace, checkpoint images) must never see a kernel token.
}

impl RestoreFrame {
    pub fn zeroed() -> Self {
        // SAFETY: plain-old-data frame; all-zero is the architectural reset
        // state and the reserved field must start cleared.
        unsafe { core::mem::zeroed() }
    }

    /// Import the user register file.  The address-space token field is
    /// structurally out of reach here.
    pub fn install_gprs(&mut self, regs: &ProcDebugRegs) {
        import_gprs(self, regs);
    }

    pub fn install_kernel_tp(&mut self, tp: u64) {
        self.kernel_tp = tp;
    }

    /// The token outranks the dumped register file: the image records the
    /// OLD task's address space, which must never win over the freshly built
    /// one.  Always call after install_gprs().
    pub fn install_addrspace_token(&mut self, token: u64) {
        self.addrspace_token = token;
    }
}

/// ptrace/checkpoint-facing export: the observer gets hardware semantics
/// (x[0] == 0), never the kernel's address-space token.  `ctx`/`out` must
/// reference a valid trap frame / proc_debug_regs_t.
#[no_mangle]
pub unsafe extern "C" fn a20_regs_frame_export_rv64(
    ctx: *const u8,
    out: *mut ProcDebugRegs,
) {
    export_gprs(&*(ctx as *const RestoreFrame), &mut *out);
}

/// ptrace-facing import: writes ONLY the architectural register file.  The
/// address-space-token slot belongs to the kernel, so a regset supplied by
/// a debugger (or a checkpoint image) cannot steer the task onto a foreign
/// address space -- the same bug class RestoreFrame's type model prevents
/// on the restore path.
#[no_mangle]
pub unsafe extern "C" fn a20_regs_frame_import_rv64(
    ctx: *mut u8,
    regs: *const ProcDebugRegs,
) {
    let frame = &mut *(ctx as *mut RestoreFrame);
    import_gprs(frame, &*regs);
}

/// Header checks shared by the fd-based and path-based restore entries.
/// Returns 0 or a negative errno.
#[no_mangle]
pub extern "C" fn a20_ckpt_validate_header(
    magic: u64,
    version: u32,
    arch: u32,
    regset_size: u64,
    nr_vma: u32,
    nr_fd: u32,
    expect_arch: u32,
) -> i32 {
    if magic != A20_CKPT_MAGIC
        || version != A20_CKPT_VERSION
        || arch != expect_arch
        || regset_size != core::mem::size_of::<ProcDebugRegs>() as u64
    {
        return -ENOEXEC;
    }
    if nr_vma == 0 || nr_vma > CKPT_R_MAX_VMA || nr_fd > CKPT_R_MAX_FD {
        return -ENOEXEC;
    }
    0
}

/// Per-VMA checks.  Returns 0 or a negative errno.
#[no_mangle]
pub extern "C" fn a20_ckpt_validate_vma(
    start: u64,
    end: u64,
    vm_flags: u64,
    data_offset: u64,
    file_size: u64,
    user_va_limit: u64,
    page_size: u64,
) -> i32 {
    if start == 0 || end == 0 || end <= start {
        return -ENOEXEC;
    }
    if (start | end) & (page_size - 1) != 0 {
        return -ENOEXEC;
    }
    let len = end - start;
    if start >= user_va_limit || len > user_va_limit - start {
        return -ENOEXEC;
    }
    if vm_flags & VM_SHARED != 0 {
        return -EOPNOTSUPP;
    }
    if vm_flags & VM_GUARD != 0 || vm_flags & (VM_READ | VM_WRITE | VM_EXEC) == 0 {
        return -ENOEXEC;
    }
    if data_offset.checked_add(len).map_or(true, |e| e > file_size) {
        return -ENOEXEC;
    }
    0
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample_regs() -> ProcDebugRegs {
        let mut r = ProcDebugRegs {
            regs: [0; 32],
            pc: 0x14c84,
            sp: 0x3ff61cc0,
            status: 0x200004020,
            orig_syscall: 82,
            fp: [0; 64],
            fcsr: 0,
        };
        for i in 0..32 {
            r.regs[i] = 0xdead0000 + i as u64;
        }
        r.regs[0] = 0x80007000000bf4f6; // stale token of the dumped task
        r.fp[0] = 0x1122334455667788;
        r
    }

    #[test]
    fn frame_install_keeps_token_after_gpr_import() {
        let mut frame = RestoreFrame::zeroed();
        let regs = sample_regs();
        frame.install_gprs(&regs);
        frame.install_kernel_tp(0xc0ffee);
        frame.install_addrspace_token(0x80010000000bf582);
        assert_eq!(frame.addrspace_token, 0x80010000000bf582);
        assert_eq!(frame.sepc, 0x14c84);
        assert_eq!(frame.gprs[1], 0x3ff61cc0); // x[2] = sp
        assert_eq!(frame.gprs[16], 82); // x[17] = orig syscall
        assert_eq!(frame.sstatus, 0x200004020);
        assert_eq!(frame.fregs[0], 0x1122334455667788);
        assert_eq!(frame.kernel_tp, 0xc0ffee);
        // x[1]..x[31] came from the image; slot x[0] did NOT.
        assert_eq!(frame.gprs[0], 0xdead0001);
        assert_eq!(frame.gprs[30], 0xdead001f);
        assert_eq!(frame.reserved, 0);
    }

    #[test]
    fn token_survives_regardless_of_call_order() {
        // Even if a caller installs the token first, the GPR path must not
        // be able to overwrite it -- the exact regression this layer exists
        // to prevent (restore resumed on the dumped task's stale pgdir).
        let mut frame = RestoreFrame::zeroed();
        frame.install_addrspace_token(0xaaaa);
        frame.install_gprs(&sample_regs());
        assert_eq!(frame.addrspace_token, 0xaaaa);
    }

    #[test]
    fn export_pins_x0_and_maps_fields() {
        let mut frame = RestoreFrame::zeroed();
        frame.addrspace_token = 0x80010000000bf582; // kernel scratch content
        frame.gprs[0] = 0x1111; // x[1]
        frame.gprs[1] = 0x2222; // x[2] = sp
        frame.gprs[16] = 64; // x[17]
        frame.sepc = 0x10000;
        frame.sstatus = 0x200004020;
        frame.fregs[0] = 0xabcdef;
        let mut out = ProcDebugRegs {
            regs: [9; 32],
            pc: 9,
            sp: 9,
            status: 9,
            orig_syscall: 9,
            fp: [9; 64],
            fcsr: 9,
        };
        export_gprs(&frame, &mut out);
        assert_eq!(out.regs[0], 0); // no kernel token leaks to observers
        assert_eq!(out.regs[1], 0x1111);
        assert_eq!(out.regs[2], 0x2222);
        assert_eq!(out.regs[17], 64);
        assert_eq!(out.pc, 0x10000);
        assert_eq!(out.sp, 0x2222);
        assert_eq!(out.status, 0x200004020);
        assert_eq!(out.fp[0], 0xabcdef);
        assert_eq!(out.fp[32], 0);
    }

    #[test]
    fn import_never_touches_addrspace_token() {
        let mut frame = RestoreFrame::zeroed();
        frame.addrspace_token = 0x80010000000bf582;
        let mut regs = sample_regs();
        regs.regs[0] = 0xdeadbeefdeadbeef; // hostile crafted slot
        import_gprs(&mut frame, &regs);
        assert_eq!(frame.addrspace_token, 0x80010000000bf582);
        assert_eq!(frame.gprs[0], 0xdead0001);
        assert_eq!(frame.gprs[16], 82);
    }

    #[test]
    fn header_validation() {
        let regs_size = core::mem::size_of::<ProcDebugRegs>() as u64;
        assert_eq!(a20_ckpt_validate_header(A20_CKPT_MAGIC, 1, 243, regs_size, 6, 4, 243), 0);
        assert!(a20_ckpt_validate_header(0, 1, 243, regs_size, 6, 4, 243) < 0);
        assert!(a20_ckpt_validate_header(A20_CKPT_MAGIC, 2, 243, regs_size, 6, 4, 243) < 0);
        assert!(a20_ckpt_validate_header(A20_CKPT_MAGIC, 1, 62, regs_size, 6, 4, 243) < 0);
        assert!(a20_ckpt_validate_header(A20_CKPT_MAGIC, 1, 243, 560, 6, 4, 243) < 0);
        assert!(a20_ckpt_validate_header(A20_CKPT_MAGIC, 1, 243, regs_size, 0, 4, 243) < 0);
        assert!(a20_ckpt_validate_header(A20_CKPT_MAGIC, 1, 243, regs_size, 9999, 4, 243) < 0);
    }

    #[test]
    fn vma_validation() {
        let limit = 0x4000_0000_0000u64;
        assert_eq!(a20_ckpt_validate_vma(0x10000, 0x19000, VM_READ | VM_EXEC, 0x9c8, 0x100000, limit, 4096), 0);
        assert!(a20_ckpt_validate_vma(0, 0x19000, VM_READ, 0, 0x100000, limit, 4096) < 0);
        assert!(a20_ckpt_validate_vma(0x10001, 0x19000, VM_READ, 0, 0x100000, limit, 4096) < 0);
        assert!(a20_ckpt_validate_vma(limit, limit + 0x1000, VM_READ, 0, 0x100000, limit, 4096) < 0);
        assert_eq!(a20_ckpt_validate_vma(0x10000, 0x19000, VM_READ | VM_SHARED, 0, 0x100000, limit, 4096), -EOPNOTSUPP);
        assert!(a20_ckpt_validate_vma(0x10000, 0x19000, VM_GUARD, 0, 0x100000, limit, 4096) < 0);
        assert!(a20_ckpt_validate_vma(0x10000, 0x19000, VM_READ, 0xffff0000, 0x100000, limit, 4096) < 0);
        assert!(a20_ckpt_validate_vma(0x10000, 0x19000, 0, 0, 0x100000, limit, 4096) < 0);
    }
}
