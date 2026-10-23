//! Rust-owned ELF-loading and exec-replacement decision core.
//!
//! This module owns every *decision* on the exec path that the C loader used to
//! make inline: ELF header and program-header validation, wrap-safe bounds
//! checks, `PT_LOAD` permission/layout planning, W^X policy, relocation-policy
//! selection, `PT_INTERP`/`PT_TLS`/`PT_DYNAMIC` discovery, the native-ABI
//! marker test, initial-stack and auxiliary-vector construction, and the
//! setuid/setgid/capability transition.
//!
//! The C side keeps low-level file-read, page/frame allocation, direct-map,
//! lock, usercopy, path, randomness, and architecture-entry primitives. It
//! does not duplicate ELF planning or exec policy decisions.
//!
//! The parsing functions work on caller-supplied byte slices, so they are
//! exercised directly by the host `--test` build with malformed, truncated and
//! overflowing fixtures. On a kernel target the same functions run over the
//! header/program-header bytes the adapter read.
#![cfg_attr(not(test), no_std)]

use core::mem::size_of;

// ------------------------------------------------------------------ errno
//
// Convention: every fallible function in this module returns a *negative*
// errno (`Err(-ENOEXEC)`), and every `#[no_mangle]` FFI entry returns either
// zero/success-data or a negative errno. One convention, both sides of the
// C bridge (see tools/tests/elf-fixtures/README.md, “Boundary contract”).
pub const EPERM: i32 = 1;
pub const ENOENT: i32 = 2;
pub const EIO: i32 = 5;
pub const E2BIG: i32 = 7;
pub const ENOEXEC: i32 = 8;
pub const EBADF: i32 = 9;
pub const ENOMEM: i32 = 12;
pub const EACCES: i32 = 13;
pub const EFAULT: i32 = 14;
pub const EINVAL: i32 = 22;
pub const ELOOP: i32 = 40;
pub const EADDRNOTAVAIL: i32 = 99;

// ------------------------------------------------------------------ ELF
pub const ELFCLASS32: u8 = 1;
pub const ELFCLASS64: u8 = 2;
pub const ELFDATA2LSB: u8 = 1;

pub const ET_EXEC: u16 = 2;
pub const ET_DYN: u16 = 3;

pub const EM_ARM: u16 = 40;
pub const EM_X86_64: u16 = 62;
pub const EM_RISCV: u16 = 243;
pub const EM_LOONGARCH: u16 = 258;
pub const EM_AARCH64: u16 = 183;
pub const EM_PPC64: u16 = 21;

pub const PT_NULL: u32 = 0;
pub const PT_LOAD: u32 = 1;
pub const PT_DYNAMIC: u32 = 2;
pub const PT_INTERP: u32 = 3;
pub const PT_TLS: u32 = 7;
pub const PT_A20_START_INFO: u32 = 0x6a20_a200;

pub const PF_X: u32 = 1;
pub const PF_W: u32 = 2;
pub const PF_R: u32 = 4;

pub const PROT_NONE: i32 = 0;
pub const PROT_READ: i32 = 1;
pub const PROT_WRITE: i32 = 2;
pub const PROT_EXEC: i32 = 4;

pub const S_ISUID: u32 = 0o4000;
pub const S_ISGID: u32 = 0o2000;

pub const EHDR32_SIZE: usize = 52;
pub const EHDR64_SIZE: usize = 64;
pub const PHDR32_SIZE: usize = 32;
pub const PHDR64_SIZE: usize = 56;

/// Mirrors `MAX_PHDRS` in `kernel/mm/elf.c`: the most headers one image may
/// carry is bounded so the fixed plan arrays cannot overflow.
pub const MAX_PHDRS: usize = 64;
/// Mirrors `MAX_ARG_STRINGS` in `kernel/include/core/consts.h`.
pub const MAX_ARG_STRINGS: usize = 256;
/// Mirrors `A20_AUXV_MAX_PAIRS` in `kernel/include/mm/vm.h`.
pub const AUXV_MAX_PAIRS: usize = 32;

/** W^X policy selector, mirroring `a20.wx=off|strip|deny`. */
pub const MM_WX_OFF: i32 = 0;
pub const MM_WX_STRIP: i32 = 1;
pub const MM_WX_DENY: i32 = 2;

// ================================================================== readers

#[inline]
fn rd_u16(b: &[u8], off: usize) -> Option<u16> {
    let s = b.get(off..off.checked_add(2)?)?;
    Some(u16::from_le_bytes([s[0], s[1]]))
}

#[inline]
fn rd_u32(b: &[u8], off: usize) -> Option<u32> {
    let s = b.get(off..off.checked_add(4)?)?;
    Some(u32::from_le_bytes([s[0], s[1], s[2], s[3]]))
}

#[inline]
fn rd_u64(b: &[u8], off: usize) -> Option<u64> {
    let s = b.get(off..off.checked_add(8)?)?;
    Some(u64::from_le_bytes([
        s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7],
    ]))
}

// ================================================================== ehdr

/// Parsed ELF identification and fixed header fields, little-endian only.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Ehdr {
    pub class: u8,
    pub data: u8,
    pub e_type: u16,
    pub e_machine: u16,
    pub e_entry: u64,
    pub e_phoff: u64,
    pub e_phentsize: u16,
    pub e_phnum: u16,
}

/// 32-bit header parse; the offsets are the ELF32 layout.
fn parse_ehdr32(b: &[u8]) -> Option<Ehdr> {
    if b.len() < EHDR32_SIZE {
        return None;
    }
    Some(Ehdr {
        class: *b.get(4)?,
        data: *b.get(5)?,
        e_type: rd_u16(b, 16)?,
        e_machine: rd_u16(b, 18)?,
        e_entry: rd_u32(b, 24)? as u64,
        e_phoff: rd_u32(b, 28)? as u64,
        e_phentsize: rd_u16(b, 42)?,
        e_phnum: rd_u16(b, 44)?,
    })
}

fn parse_ehdr64(b: &[u8]) -> Option<Ehdr> {
    if b.len() < EHDR64_SIZE {
        return None;
    }
    Some(Ehdr {
        class: *b.get(4)?,
        data: *b.get(5)?,
        e_type: rd_u16(b, 16)?,
        e_machine: rd_u16(b, 18)?,
        e_entry: rd_u64(b, 24)?,
        e_phoff: rd_u64(b, 32)?,
        e_phentsize: rd_u16(b, 54)?,
        e_phnum: rd_u16(b, 56)?,
    })
}

/// Parse whichever class the ident byte names. Returns `None` for an unknown
/// class or a truncated buffer.
pub fn parse_ehdr(b: &[u8]) -> Option<Ehdr> {
    if b.len() < 16 || b.get(0..4)? != [0x7f, b'E', b'L', b'F'] {
        return None;
    }
    match *b.get(4)? {
        ELFCLASS32 => parse_ehdr32(b),
        ELFCLASS64 => parse_ehdr64(b),
        _ => None,
    }
}

/// Reject every header the kernel cannot load. Mirrors `elf_check_header()`:
/// ident/class/endianness/type/machine plus a program-header table whose entry
/// size and count are in range.
pub fn validate_ehdr(eh: &Ehdr, expect_class: u8, expect_machine: u16) -> Result<(), i32> {
    if eh.class != expect_class {
        return Err(-ENOEXEC);
    }
    if eh.data != ELFDATA2LSB {
        return Err(-ENOEXEC);
    }
    if eh.e_type != ET_EXEC && eh.e_type != ET_DYN {
        return Err(-ENOEXEC);
    }
    if eh.e_machine != expect_machine {
        return Err(-ENOEXEC);
    }
    let min_phdr = if eh.class == ELFCLASS32 {
        PHDR32_SIZE
    } else {
        PHDR64_SIZE
    };
    if (eh.e_phentsize as usize) < min_phdr {
        return Err(-ENOEXEC);
    }
    if eh.e_phnum == 0 || eh.e_phnum as usize > MAX_PHDRS {
        return Err(-ENOEXEC);
    }
    Ok(())
}

// ================================================================== phdr

#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq)]
pub struct Phdr {
    pub p_type: u32,
    pub p_flags: u32,
    pub p_offset: u64,
    pub p_vaddr: u64,
    pub p_filesz: u64,
    pub p_memsz: u64,
    pub p_align: u64,
}

fn parse_phdr(b: &[u8], index: usize, entsize: usize, elf32: bool) -> Option<Phdr> {
    let off = index.checked_mul(entsize)?;
    let end = off.checked_add(entsize)?;
    if end > b.len() {
        return None;
    }
    if elf32 {
        Some(Phdr {
            p_type: rd_u32(b, off)?,
            p_offset: rd_u32(b, off + 4)? as u64,
            p_vaddr: rd_u32(b, off + 8)? as u64,
            p_filesz: rd_u32(b, off + 16)? as u64,
            p_memsz: rd_u32(b, off + 20)? as u64,
            p_flags: rd_u32(b, off + 24)?,
            p_align: rd_u32(b, off + 28)? as u64,
        })
    } else {
        Some(Phdr {
            p_type: rd_u32(b, off)?,
            p_flags: rd_u32(b, off + 4)?,
            p_offset: rd_u64(b, off + 8)?,
            p_vaddr: rd_u64(b, off + 16)?,
            p_filesz: rd_u64(b, off + 32)?,
            p_memsz: rd_u64(b, off + 40)?,
            p_align: rd_u64(b, off + 48)?,
        })
    }
}

// ================================================================== plan

/// One `PT_LOAD` segment, already validated and page-layout-planned.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SegmentPlan {
    pub ph_index: u16,
    pub _pad0: u16,
    /// Filtered protection (`PROT_*`) the segment must be mapped with.
    pub prot: i32,
    pub p_flags: u32,
    pub va: u64,
    pub memsz: u64,
    pub filesz: u64,
    pub offset: u64,
    pub align: u64,
    /// 1 when `p_memsz > p_filesz` (the segment carries a zero-filled tail).
    pub anon_tail: u8,
    pub _pad1: [u8; 7],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct TlsPlan {
    pub present: u8,
    pub _pad0: [u8; 7],
    pub offset: u64,
    pub filesz: u64,
    pub memsz: u64,
    pub align: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct InterpPlan {
    pub present: u8,
    pub _pad0: [u8; 7],
    pub offset: u64,
    pub filesz: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct DynamicPlan {
    pub present: u8,
    pub _pad0: [u8; 7],
    pub vaddr: u64,
    pub memsz: u64,
}

/// The complete validated image plan the C adapter executes segment by segment.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ImagePlan {
    pub ehdr: Ehdr,
    pub is_dyn: u8,
    pub is_native_abi: u8,
    pub nseg: u32,
    pub segments: [SegmentPlan; MAX_PHDRS],
    pub tls: TlsPlan,
    pub interp: InterpPlan,
    pub dynamic: DynamicPlan,
}

impl Default for ImagePlan {
    fn default() -> Self {
        ImagePlan {
            ehdr: Ehdr::default(),
            is_dyn: 0,
            is_native_abi: 0,
            nseg: 0,
            segments: [SegmentPlan::default(); MAX_PHDRS],
            tls: TlsPlan::default(),
            interp: InterpPlan::default(),
            dynamic: DynamicPlan::default(),
        }
    }
}

/// `p_flags` → `PROT_*`. Mirrors `seg_prot()`.
#[inline]
pub fn segment_prot(p_flags: u32) -> i32 {
    let mut prot = 0;
    if p_flags & PF_R != 0 {
        prot |= PROT_READ;
    }
    if p_flags & PF_W != 0 {
        prot |= PROT_WRITE;
    }
    if p_flags & PF_X != 0 {
        prot |= PROT_EXEC;
    }
    prot
}

/// W^X decision. `xlator_host` is the one adapter-supplied fact (the task's
/// per-process translator exemption). `off` honours the request, `strip`
/// drops the writable bit, `deny` refuses W|X with `-EACCES`.
pub fn wx_filter(prot: i32, policy: i32, xlator_host: bool) -> i32 {
    if prot & (PROT_WRITE | PROT_EXEC) != (PROT_WRITE | PROT_EXEC) {
        return prot;
    }
    if xlator_host {
        return prot;
    }
    match policy {
        MM_WX_STRIP => prot & !PROT_WRITE,
        MM_WX_DENY => EACCES.checked_neg().unwrap_or(-EACCES),
        _ => prot,
    }
}

/// Per-segment validation and layout check, independent of a concrete address
/// space. Returns `-ENOEXEC` on impossible sizes, `-EINVAL` on an
/// offset/vaddr alignment mismatch.
fn validate_segment(ph: &Phdr) -> Result<(), i32> {
    if ph.p_filesz > ph.p_memsz {
        return Err(-ENOEXEC);
    }
    // vaddr + memsz must not wrap.
    ph.p_vaddr.checked_add(ph.p_memsz).ok_or(-ENOEXEC)?;
    // Where the file bytes live must not wrap either.
    ph.p_offset.checked_add(ph.p_filesz).ok_or(-ENOEXEC)?;
    // An image with bytes in the file must keep p_offset and p_vaddr
    // congruent modulo the page size; the eager loader tolerates the
    // mismatching case, the lazy path does not, so the plan refuses it.
    if ph.p_filesz != 0 && (ph.p_offset & 0xfff) != (ph.p_vaddr & 0xfff) {
        return Err(-EINVAL);
    }
    Ok(())
}

/// Plan the whole image: validate the header, walk the program-header table
/// with wrap-safe bounds checks, validate and collect every `PT_LOAD`, and
/// record `PT_TLS`/`PT_INTERP`/`PT_DYNAMIC` and the native-ABI marker.
///
/// `phdr_bytes` must hold at least `e_phnum * e_phentsize` readable bytes and
/// is read relative to the start of the table (`e_phoff` is not re-applied).
pub fn plan_image(
    ehdr_bytes: &[u8],
    phdr_bytes: &[u8],
    expect_class: u8,
    expect_machine: u16,
    wx_policy: i32,
    wx_xlator_host: bool,
) -> Result<ImagePlan, i32> {
    let eh = parse_ehdr(ehdr_bytes).ok_or(-ENOEXEC)?;
    validate_ehdr(&eh, expect_class, expect_machine)?;

    let entsize = eh.e_phentsize as usize;
    let elf32 = eh.class == ELFCLASS32;
    let nph = eh.e_phnum as usize;

    // The table must fit: nph * entsize with no overflow, and physically
    // present in the buffer the adapter supplied.
    let table_len = nph.checked_mul(entsize).ok_or(-ENOEXEC)?;
    if table_len > phdr_bytes.len() {
        return Err(-ENOEXEC);
    }

    let mut plan = ImagePlan {
        ehdr: eh,
        is_dyn: u8::from(eh.e_type == ET_DYN),
        ..Default::default()
    };

    let mut native_marker = false;
    for i in 0..nph {
        let ph = parse_phdr(phdr_bytes, i, entsize, elf32).ok_or(-ENOEXEC)?;
        match ph.p_type {
            PT_LOAD => {
                validate_segment(&ph)?;
                let prot = wx_filter(segment_prot(ph.p_flags), wx_policy, wx_xlator_host);
                if prot < 0 {
                    return Err(prot);
                }
                // Distinct PT_LOAD memory ranges may share a physical page
                // (for example .data followed by BSS). The C mapper preserves
                // bytes already present there and lets the later segment set
                // the page permissions. Reject only overlapping byte ranges.
                let start = ph.p_vaddr;
                let end = ph.p_vaddr.checked_add(ph.p_memsz).ok_or(-ENOEXEC)?;
                for seg in plan.segments.iter().take(plan.nseg as usize) {
                    let s2 = seg.va;
                    let e2 = seg.va.checked_add(seg.memsz).ok_or(-ENOEXEC)?;
                    if start < e2 && s2 < end {
                        return Err(-EINVAL);
                    }
                }
                let slot = plan.segments.get_mut(plan.nseg as usize)
                    .ok_or(-ENOEXEC)?;
                *slot = SegmentPlan {
                    ph_index: i as u16,
                    prot,
                    p_flags: ph.p_flags,
                    va: ph.p_vaddr,
                    memsz: ph.p_memsz,
                    filesz: ph.p_filesz,
                    offset: ph.p_offset,
                    align: ph.p_align,
                    anon_tail: u8::from(ph.p_memsz > ph.p_filesz),
                    ..Default::default()
                };
                plan.nseg += 1;
            }
            PT_TLS => {
                if ph.p_filesz > ph.p_memsz {
                    return Err(-ENOEXEC);
                }
                plan.tls = TlsPlan {
                    present: 1,
                    _pad0: [0; 7],
                    offset: ph.p_offset,
                    filesz: ph.p_filesz,
                    memsz: ph.p_memsz,
                    align: if ph.p_align < 1 { 1 } else { ph.p_align },
                };
            }
            PT_INTERP => {
                plan.interp = InterpPlan {
                    present: 1,
                    _pad0: [0; 7],
                    offset: ph.p_offset,
                    filesz: ph.p_filesz,
                };
            }
            PT_DYNAMIC => {
                plan.dynamic = DynamicPlan {
                    present: 1,
                    _pad0: [0; 7],
                    vaddr: ph.p_vaddr,
                    memsz: ph.p_memsz,
                };
            }
            PT_A20_START_INFO => native_marker = true,
            _ => {}
        }
    }

    if plan.nseg == 0 {
        return Err(-ENOEXEC);
    }
    plan.is_native_abi = u8::from(native_marker);
    Ok(plan)
}

/// Load-bias policy: a PIE (`ET_DYN`) is based at `dyn_base + aslr_bias`; an
/// `ET_EXEC` keeps its link-time addresses (`bias 0`).
#[inline]
pub fn plan_load_bias(is_dyn: bool, dyn_base: u64, aslr_bias: u64) -> u64 {
    if is_dyn {
        dyn_base.wrapping_add(aslr_bias)
    } else {
        0
    }
}

/// Final layout facts derived from the planned segments plus the bias.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Layout {
    pub base: u64,
    pub max_va: u64,
    pub brk_va: u64,
    pub head_va: u64,
    pub hdr_map_va: u64,
    pub phdr_va: u64,
    pub entry: u64,
    pub exec_entry: u64,
    pub load_size: u64,
}

/// Mechanism callbacks for the executable image transaction. Rust owns the
/// phase order and the single abort/commit decision; C supplies kernel-only
/// operations that cannot be expressed in the host-testable core.
#[repr(C)]
pub struct LoadResources {
    pub pgdir: u64,
    pub mm: u64,
    pub tls_buffer: u64,
    pub tls_filesz: u64,
    pub tls_memsz: u64,
    pub tls_align: u64,
    pub tls_offset: u64,
    pub tls_tcb_offset: u64,
    pub tls_total_size: u64,
    pub tls_pages: u64,
    pub tls_tcb_size: u64,
    pub interp_fd: i32,
    pub _pad: u32,
    pub interp_entry: u64,
    pub interp_base: u64,
    pub mmap: u64,
    pub stack_top: u64,
    pub tls_va: u64,
    pub tls_tp: u64,
}
impl Default for LoadResources {
    fn default() -> Self { Self { pgdir: 0, mm: 0, tls_buffer: 0, tls_filesz: 0, tls_memsz: 0,
        tls_align: 1, tls_offset: 0, tls_tcb_offset: 0, tls_total_size: 0,
        tls_pages: 0, tls_tcb_size: 0, interp_fd: -1, _pad: 0, interp_entry: 0, interp_base: 0,
        stack_top: 0, tls_va: 0, tls_tp: 0, mmap: 0 } }
}

#[repr(C)]
pub struct LoadTransactionOps {
    pub context: *mut core::ffi::c_void,
    pub resources: *mut LoadResources,
    pub begin: Option<unsafe extern "C" fn(*mut core::ffi::c_void, u64, u64, *mut u64) -> i32>,
    pub map_segments: Option<unsafe extern "C" fn(*mut core::ffi::c_void, u64) -> i32>,
    pub stage_tls: Option<unsafe extern "C" fn(*mut core::ffi::c_void) -> i32>,
    pub relocate: Option<unsafe extern "C" fn(*mut core::ffi::c_void, u64, u64) -> i32>,
    pub finish_layout: Option<unsafe extern "C" fn(*mut core::ffi::c_void, *const Layout) -> i32>,
    pub read_interpreter: Option<unsafe extern "C" fn(*mut core::ffi::c_void, u64, *mut u8, usize) -> i32>,
    pub open_interpreter: Option<unsafe extern "C" fn(*mut core::ffi::c_void, *const u8, usize, *mut i32) -> i32>,
    pub path_capacity: usize,
    pub nommu: u8,
    pub load_interpreter: Option<unsafe extern "C" fn(*mut core::ffi::c_void, u64) -> i32>,
    pub close_interpreter: Option<unsafe extern "C" fn(*mut core::ffi::c_void)>,
    pub map_stack: Option<unsafe extern "C" fn(*mut core::ffi::c_void) -> i32>,
    pub install_tls: Option<unsafe extern "C" fn(*mut core::ffi::c_void) -> i32>,
    pub build_interpreter_path: Option<unsafe extern "C" fn(*mut core::ffi::c_void, u32, *const u8, *mut u8, usize) -> i32>,
    pub arch_interpreter_path: Option<unsafe extern "C" fn(*mut core::ffi::c_void, *const u8, *mut u8, usize) -> i32>,
    pub brk_offset: Option<unsafe extern "C" fn(*mut core::ffi::c_void, u64, *mut u64) -> i32>,
    pub commit: Option<unsafe extern "C" fn(*mut core::ffi::c_void, *const LoadResult) -> i32>,
    pub free_tls: Option<unsafe extern "C" fn(*mut core::ffi::c_void)>,
    pub discard_vmas: Option<unsafe extern "C" fn(*mut core::ffi::c_void)>,
    pub discard_nommu: Option<unsafe extern "C" fn(*mut core::ffi::c_void)>,
    pub destroy_pgdir: Option<unsafe extern "C" fn(*mut core::ffi::c_void)>,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct LoadResult {
    pub entry: u64, pub exec_entry: u64, pub base: u64, pub end_va: u64,
    pub brk: u64, pub phdr_va: u64, pub phnum: u32, pub phentsize: u32,
    pub load_addr: u64, pub load_size: u64, pub pgdir: u64, pub stack_top: u64,
    pub tls_va: u64, pub tls_size: u64, pub tls_tp: u64, pub interp_base: u64,
    pub mmap: u64, pub is_native_abi: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct TlsLayout { pub tcb_offset: u64, pub total_size: u64, pub pages: u64, pub tcb_size: u64 }

pub fn plan_tls_layout(memsz: u64, align: u64, page_size: u64, tcb_size: u64) -> Option<TlsLayout> {
    if page_size == 0 || !page_size.is_power_of_two() || tcb_size == 0 { return None; }
    let align = align.max(1);
    let tcb_offset = memsz.checked_add(align - 1)?.checked_div(align)?.checked_mul(align)?;
    let total_size = tcb_offset.checked_add(tcb_size)?;
    let pages = total_size.checked_add(page_size - 1)? / page_size;
    if pages == 0 { return None; }
    Some(TlsLayout { tcb_offset, total_size, pages, tcb_size })
}

#[repr(C)]
pub struct InterpreterOps {
    pub context: *mut core::ffi::c_void,
    pub read: Option<unsafe extern "C" fn(*mut core::ffi::c_void, u64, *mut u8, usize, *mut usize) -> i32>,
    pub choose_bias: Option<unsafe extern "C" fn(*mut core::ffi::c_void, u64, u64, *mut u64) -> i32>,
    pub map_segment: Option<unsafe extern "C" fn(*mut core::ffi::c_void, *const SegmentPlan, u64) -> i32>,
    pub finish: Option<unsafe extern "C" fn(*mut core::ffi::c_void, *const Layout) -> i32>,
    pub expect_class: u8,
    pub wx_xlator_host: u8,
    pub _pad: u16,
    pub expect_machine: u16,
    pub wx_policy: i32,
    pub nommu: u8,
    pub _pad1: [u8; 7],
}

#[no_mangle]
pub unsafe extern "C" fn a20_elf_load_interpreter(
    ops: *const InterpreterOps, entry_out: *mut u64, base_out: *mut u64,
) -> i32 {
    if ops.is_null() || entry_out.is_null() || base_out.is_null() { return -EFAULT; }
    let ops = &*ops;
    let (Some(read), Some(choose_bias), Some(map_segment), Some(finish)) =
        (ops.read, ops.choose_bias, ops.map_segment, ops.finish) else { return -EINVAL; };
    let mut ehdr_bytes = [0u8; 64];
    let mut got = 0usize;
    let rc = read(ops.context, 0, ehdr_bytes.as_mut_ptr(), ehdr_bytes.len(), &mut got);
    if rc < 0 { return rc; }
    if got > ehdr_bytes.len() { return -EIO; }
    if got < 52 { return -ENOEXEC; }
    let ehdr_slice = core::slice::from_raw_parts(ehdr_bytes.as_ptr(), got);
    let eh = match parse_ehdr(ehdr_slice) { Some(eh) => eh, None => return -ENOEXEC };
    let table_len = (eh.e_phnum as usize).checked_mul(eh.e_phentsize as usize).unwrap_or(usize::MAX);
    if table_len > MAX_PHDRS * 64 { return -ENOEXEC; }
    let mut phdr_bytes = [0u8; MAX_PHDRS * 64];
    let mut phgot = 0usize;
    let rc = read(ops.context, eh.e_phoff, phdr_bytes.as_mut_ptr(), table_len, &mut phgot);
    if rc < 0 { return rc; }
    if phgot > phdr_bytes.len() { return -EIO; }
    if phgot < table_len { return -ENOEXEC; }
    let phdr_slice = core::slice::from_raw_parts(phdr_bytes.as_ptr(), phgot);
    let plan = match plan_image(ehdr_slice, phdr_slice, ops.expect_class,
                                ops.expect_machine, ops.wx_policy, ops.wx_xlator_host != 0) {
        Ok(plan) => plan, Err(e) => return e,
    };
    let (mut min, mut max) = (u64::MAX, 0u64);
    for seg in plan.segments.iter().take(plan.nseg as usize) {
        let start = seg.va & !0xfff;
        let end = match seg.va.checked_add(seg.memsz).and_then(|v| v.checked_add(0xfff)) {
            Some(v) => v & !0xfff, None => return -ENOEXEC,
        };
        min = min.min(start); max = max.max(end);
    }
    if min == u64::MAX || max <= min { return -ENOEXEC; }
    let mut bias = 0;
    let rc = choose_bias(ops.context, min, max, &mut bias);
    if rc < 0 { return rc; }
    for seg in plan.segments.iter().take(plan.nseg as usize) {
        let rc = map_segment(ops.context, seg, bias);
        if rc < 0 { return rc; }
    }
    let mut layout = finish_layout(&plan, bias);
    if ops.nommu != 0 { layout.base = bias.wrapping_add(min); }
    let rc = finish(ops.context, &layout);
    if rc < 0 { return rc; }
    *entry_out = layout.entry;
    *base_out = layout.base;
    0
}

/// Execute the validated load in its only legal order. Rust retains the
/// resource handles, unwinds them in reverse acquisition order on failure,
/// and clears the page-table/mm handles only after commit transfers them.
#[no_mangle]
pub unsafe extern "C" fn a20_elf_load_transaction(
    plan: *const ImagePlan,
    ops: *const LoadTransactionOps,
    bias_out: *mut u64,
) -> i32 {
    if plan.is_null() || ops.is_null() || bias_out.is_null() { return -EFAULT; }
    let plan = &*plan;
    let ops = &*ops;
    if ops.resources.is_null() { return -EFAULT; }
    let resources = ops.resources;
    let (Some(begin), Some(map_segments), Some(stage_tls), Some(relocate),
         Some(finish_layout_cb), Some(read_interpreter), Some(open_interpreter), Some(load_interpreter),
         Some(close_interpreter), Some(map_stack), Some(install_tls),
         Some(brk_offset), Some(commit), Some(free_tls), Some(discard_vmas),
         Some(discard_nommu), Some(destroy_pgdir)) =
        (ops.begin, ops.map_segments, ops.stage_tls, ops.relocate,
         ops.finish_layout, ops.read_interpreter, ops.open_interpreter, ops.load_interpreter,
         ops.close_interpreter, ops.map_stack, ops.install_tls, ops.brk_offset, ops.commit,
         ops.free_tls, ops.discard_vmas, ops.discard_nommu, ops.destroy_pgdir)
    else { return -EINVAL; };
    let (mut min, mut max) = (u64::MAX, 0u64);
    for seg in plan.segments.iter().take(plan.nseg as usize) {
        let start = seg.va & !0xfff;
        let end = match seg.va.checked_add(seg.memsz).and_then(|v| v.checked_add(0xfff)) {
            Some(v) => v & !0xfff,
            None => return -ENOEXEC,
        };
        min = min.min(start);
        max = max.max(end);
    }
    if min == u64::MAX || max <= min { return -ENOEXEC; }
    let mut bias = 0;
    let mut rc = begin(ops.context, min, max, &mut bias);
    let began = rc == 0;
    if rc == 0 { rc = map_segments(ops.context, bias); }
    if rc == 0 {
        (*resources).tls_filesz = if plan.tls.present != 0 { plan.tls.filesz } else { 0 };
        (*resources).tls_memsz = if plan.tls.present != 0 { plan.tls.memsz } else { 0 };
        (*resources).tls_align = if plan.tls.present != 0 { plan.tls.align.max(1) } else { 1 };
        (*resources).tls_offset = if plan.tls.present != 0 { plan.tls.offset } else { 0 };
        if (*resources).tls_filesz != 0 { rc = stage_tls(ops.context); }
    }
    if rc == 0 && plan.dynamic.present != 0 { rc = relocate(ops.context, bias, max - min); }
    let mut layout = Layout::default();
    if rc == 0 {
        layout = finish_layout(plan, bias);
        if ops.nommu != 0 { layout.base = bias.wrapping_add(min); }
        rc = finish_layout_cb(ops.context, &layout);
    }
    if rc == 0 && plan.interp.present != 0 {
        let cap = ops.path_capacity;
        if cap == 0 || cap > 512 { rc = -EINVAL; }
        let mut path = [0u8; 512];
        let path_limit = cap.min(512).saturating_sub(1);
        let n = core::cmp::min(plan.interp.filesz.min(path_limit as u64) as usize, path_limit);
        if rc == 0 && n != 0 { rc = read_interpreter(ops.context, plan.interp.offset, path.as_mut_ptr(), n); }
        path.as_mut_ptr().add(n).write(0);
        if rc == 0 {
            rc = open_interpreter(ops.context, path.as_ptr(), n, &mut (*resources).interp_fd);
            if rc < 0 {
                let mut candidate = [0u8; 512];
                let mut found = false;
                if let Some(build) = ops.build_interpreter_path {
                    for kind in 1..=3 {
                        candidate.fill(0);
                        if build(ops.context, kind, path.as_ptr(), candidate.as_mut_ptr(), cap) < 0 { continue; }
                        let clen = candidate.iter().position(|b| *b == 0).unwrap_or(cap);
                        if open_interpreter(ops.context, candidate.as_ptr(), clen, &mut (*resources).interp_fd) == 0 { found = true; break; }
                    }
                }
                if !found {
                    if let Some(arch_path) = ops.arch_interpreter_path {
                        candidate.fill(0);
                        if arch_path(ops.context, path.as_ptr(), candidate.as_mut_ptr(), cap) == 0 {
                            let clen = candidate.iter().position(|b| *b == 0).unwrap_or(cap);
                            found = open_interpreter(ops.context, candidate.as_ptr(), clen, &mut (*resources).interp_fd) == 0;
                        }
                    }
                }
                rc = if found { 0 } else { -ENOENT };
            }
        }
        if rc == 0 {
            rc = load_interpreter(ops.context, bias);
            if (*resources).interp_fd >= 0 {
                close_interpreter(ops.context);
                (*resources).interp_fd = -1;
            }
        }
    }
    if rc == 0 { rc = map_stack(ops.context); }
    if rc == 0 {
        if let Some(tls) = plan_tls_layout((*resources).tls_memsz, (*resources).tls_align, 4096, 512) {
            (*resources).tls_tcb_offset = tls.tcb_offset;
            (*resources).tls_total_size = tls.total_size;
            (*resources).tls_pages = tls.pages;
            (*resources).tls_tcb_size = tls.tcb_size;
            rc = install_tls(ops.context);
        } else { rc = -ENOEXEC; }
    }
    if rc == 0 {
        if plan.interp.present != 0 { (*resources).tls_tp = 0; }
        let brk_base = (if layout.brk_va != 0 { layout.brk_va } else { layout.max_va }).wrapping_add(0xfff) & !0xfff;
        let mut offset = 0;
        rc = brk_offset(ops.context, brk_base, &mut offset);
        if rc == 0 {
            let result = LoadResult {
                entry: if plan.interp.present != 0 { (*resources).interp_entry } else { layout.entry },
                exec_entry: layout.exec_entry, base: layout.base, end_va: layout.max_va,
                brk: brk_base.wrapping_add(offset), phdr_va: layout.phdr_va,
                phnum: plan.ehdr.e_phnum as u32, phentsize: plan.ehdr.e_phentsize as u32,
                load_addr: layout.base, load_size: layout.load_size, pgdir: (*resources).pgdir,
                stack_top: (*resources).stack_top, tls_va: (*resources).tls_va,
                tls_size: (*resources).tls_memsz, tls_tp: (*resources).tls_tp,
                interp_base: (*resources).interp_base, mmap: (*resources).mmap,
                is_native_abi: plan.is_native_abi as u32,
            };
            rc = commit(ops.context, &result);
        }
    }
    if rc < 0 {
        if began {
        if (*resources).interp_fd >= 0 { close_interpreter(ops.context); (*resources).interp_fd = -1; }
        if (*resources).tls_buffer != 0 {
            free_tls(ops.context);
            (*resources).tls_buffer = 0;
        }
        if (*resources).pgdir != 0 {
            discard_vmas(ops.context);
            discard_nommu(ops.context);
            destroy_pgdir(ops.context);
            (*resources).pgdir = 0; (*resources).mm = 0;
        }
        }
        return rc;
    }
    if (*resources).tls_buffer != 0 {
        free_tls(ops.context);
        (*resources).tls_buffer = 0;
    }
    (*resources).pgdir = 0;
    (*resources).mm = 0;
    *bias_out = bias;
    0
}

/// Reproduce `elf_load64/32`'s post-loop arithmetic: first mapped page as
/// `base`, highest rounded segment end as `max_va`, highest writable end as
/// `brk_va`, and the header/`AT_PHDR` base derived from the first `PT_LOAD`.
pub fn finish_layout(plan: &ImagePlan, load_bias: u64) -> Layout {
    let mut l = Layout::default();
    for seg in plan.segments.iter().take(plan.nseg as usize) {
        let seg_va = seg.va.wrapping_add(load_bias);
        if l.hdr_map_va == 0 {
            l.hdr_map_va = seg.va.wrapping_add(load_bias).wrapping_sub(seg.offset);
        }
        let seg_start = seg_va & !0xfff;
        if l.base == 0 {
            l.base = seg_start;
        }
        let seg_end = seg_va.wrapping_add(seg.memsz).wrapping_add(0xfff) & !0xfff;
        if seg_end > l.max_va {
            l.max_va = seg_end;
        }
        if seg.p_flags & PF_W != 0 && seg_end > l.brk_va {
            l.brk_va = seg_end;
        }
        if seg.offset == 0 {
            l.head_va = seg_va;
        }
    }
    l.entry = plan.ehdr.e_entry.wrapping_add(load_bias);
    l.exec_entry = l.entry;
    l.phdr_va = if l.hdr_map_va != 0 {
        l.hdr_map_va.wrapping_add(plan.ehdr.e_phoff)
    } else if l.head_va != 0 {
        l.head_va.wrapping_add(plan.ehdr.e_phoff)
    } else {
        l.base.wrapping_add(plan.ehdr.e_phoff)
    };
    l.load_size = l.max_va.saturating_sub(l.base);
    l
}

// ================================================================== dynamic

/// Subset of `DT_*` the static-pie relocator needs, with the validation the C
/// loader used to do inline.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct DynInfo {
    pub rela: u64,
    pub relasz: u64,
    pub relaent: u64,
    pub symtab: u64,
    pub syment: u64,
    pub hash_sysv: u64,
    pub hash_gnu: u64,
    pub has_rela: u8,
    pub _pad0: [u8; 7],
}

pub const DT_NULL: i64 = 0;
pub const DT_HASH: i64 = 4;
pub const DT_SYMTAB: i64 = 6;
pub const DT_RELA: i64 = 7;
pub const DT_RELASZ: i64 = 8;
pub const DT_RELAENT: i64 = 9;
pub const DT_SYMENT: i64 = 11;
pub const DT_GNU_HASH: i64 = 0x6fff_fef5;

/// Walk the `.dynamic` array, stopping at `DT_NULL`, and validate the reloc
/// stride. `elf64` selects the entry width. This is the tag-scan decision the C
/// `elf_apply_relocs()` used to make.
pub fn scan_dynamic(dyn_bytes: &[u8], elf64: bool) -> Result<DynInfo, i32> {
    let dyn_ent = if elf64 { 16usize } else { 8usize };
    let min_rela = if elf64 { 24usize } else { 12usize };
    let mut info = DynInfo::default();
    let mut off = 0usize;
    while off + dyn_ent <= dyn_bytes.len() {
        let (tag, val) = if elf64 {
            (
                rd_u64(dyn_bytes, off).ok_or(-ENOEXEC)? as i64,
                rd_u64(dyn_bytes, off + 8).ok_or(-ENOEXEC)?,
            )
        } else {
            (
                rd_u32(dyn_bytes, off).ok_or(-ENOEXEC)? as i32 as i64,
                rd_u32(dyn_bytes, off + 4).ok_or(-ENOEXEC)? as u64,
            )
        };
        if tag == DT_NULL {
            break;
        }
        match tag {
            DT_RELA => info.rela = val,
            DT_RELASZ => info.relasz = val,
            DT_RELAENT => info.relaent = val,
            DT_SYMTAB => info.symtab = val,
            DT_SYMENT => info.syment = val,
            DT_HASH => info.hash_sysv = val,
            DT_GNU_HASH => info.hash_gnu = val,
            _ => {}
        }
        off += dyn_ent;
    }
    if info.rela == 0 || info.relasz == 0 {
        return Ok(info);
    }
    if info.relaent != 0 && (info.relaent as usize) < min_rela {
        return Err(-ENOEXEC);
    }
    if info.syment == 0 {
        info.syment = if elf64 { 24 } else { 16 };
    }
    info.has_rela = 1;
    Ok(info)
}

/// The RELATIVE / ABSOLUTE reloc type for a supported machine, or
/// `u32::MAX` for "not applicable". Mirrors `arch_elf_reloc.c`.
pub fn reloc_types(machine: u16, elf64: bool) -> (u32, u32) {
    const NONE: u32 = u32::MAX;
    match machine {
        EM_ARM => (23, if elf64 { NONE } else { 2 }),
        EM_X86_64 => (8, if elf64 { 1 } else { NONE }),
        EM_AARCH64 => (1027, if elf64 { 257 } else { NONE }),
        EM_RISCV => (3, if elf64 { 2 } else { NONE }),
        EM_PPC64 => (1, if elf64 { 38 } else { NONE }),
        EM_LOONGARCH => (2, if elf64 { 3 } else { NONE }),
        _ => (NONE, NONE),
    }
}

/// What the relocator must do with one entry.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum RelocAction {
    /// `RELATIVE`: store `bias + addend`.
    Relative,
    /// `ABSOLUTE`: resolve the symbol, then store `bias + sym + addend`.
    Absolute,
    /// Not one of the two supported families: skip.
    Skip,
}

/// Classify a relocation entry and enforce the symbol bound the C loader
/// checked. Weak undefined symbols resolve to zero; strong undefined symbols
/// make the image non-self-contained and are refused.
pub fn reloc_action(
    rel_type: u32,
    symidx: u32,
    nsyms: u64,
    st_info: u8,
    st_shndx: u16,
    want_relative: u32,
    want_absolute: u32,
) -> Result<RelocAction, i32> {
    if want_relative == u32::MAX && want_absolute == u32::MAX {
        return Ok(RelocAction::Skip);
    }
    if rel_type == want_relative {
        return Ok(RelocAction::Relative);
    }
    if rel_type != want_absolute {
        return Ok(RelocAction::Skip);
    }
    if (symidx as u64) >= nsyms {
        return Err(-ENOEXEC);
    }
    if st_shndx == 0 {
        // Undefined: weak resolves to zero, strong is fatal.
        const STB_WEAK: u8 = 2;
        if st_info >> 4 != STB_WEAK {
            return Err(-ENOEXEC);
        }
    }
    Ok(RelocAction::Absolute)
}

/// Reconstruct the `.dynsym` symbol count from whichever hash table exists.
/// Mirrors `elf_dynsym_count()`.
///
/// The header fields are attacker-controlled, so every derived offset is
/// checked: a wrapped `bloom_words`/`nbuckets` product or a `symoffset` past
/// the highest bucket yields 0 symbols (which then makes any ABSOLUTE
/// relocation fail its bound) instead of walking the image out of range. The
/// slice length is the caller's mapping-vetted window, so an in-image walk
/// past the table simply stops when `rd_u32` leaves the window.
pub fn dynsym_count(sysv: Option<&[u8]>, gnu: Option<&[u8]>) -> u64 {
    if let Some(s) = sysv {
        if s.len() >= 8 {
            return rd_u32(s, 4).unwrap_or(0) as u64;
        }
        return 0;
    }
    let gnu = match gnu {
        Some(g) => g,
        None => return 0,
    };
    let nbuckets = match rd_u32(gnu, 0) {
        Some(v) => v as u64,
        None => return 0,
    };
    let symoffset = rd_u32(gnu, 4).unwrap_or(0) as u64;
    let bloom_words = rd_u32(gnu, 8).unwrap_or(0) as u64;
    // 64-bit host: bloom words are 64 bits wide, so 8 bytes each.
    let buckets_off = match bloom_words.checked_mul(8).and_then(|b| 16u64.checked_add(b)) {
        Some(v) => v,
        None => return 0,
    };
    let mut nsym: u64 = 0;
    for i in 0..nbuckets {
        let off = match i.checked_mul(4).and_then(|o| buckets_off.checked_add(o)) {
            Some(v) => v,
            None => return 0,
        };
        let off = match usize::try_from(off) {
            Ok(v) => v,
            Err(_) => return 0,
        };
        match rd_u32(gnu, off) {
            Some(v) if (v as u64) > nsym => nsym = v as u64,
            Some(_) => {}
            None => return 0,
        }
    }
    if nsym == 0 {
        return 0;
    }
    // The chain array starts after the buckets, at the entry the lowest bucket
    // points to. `nsym` is the highest bucket value, so this is the highest
    // possible chain start; an underflow means the table is inconsistent.
    let chain_off = match nsym
        .checked_sub(symoffset)
        .and_then(|adj| adj.checked_mul(4))
        .and_then(|adj| nbuckets.checked_mul(4).and_then(|b| buckets_off.checked_add(b)).and_then(|b| b.checked_add(adj)))
    {
        Some(v) => v,
        None => return 0,
    };
    let mut guard = 0u64;
    loop {
        let off = match guard.checked_mul(4).and_then(|o| chain_off.checked_add(o)) {
            Some(v) => v,
            None => break,
        };
        let off = match usize::try_from(off) {
            Ok(v) => v,
            Err(_) => break,
        };
        match rd_u32(gnu, off) {
            Some(w) if w & 1 != 0 && nsym < 0x0100_0000 => nsym += 1,
            Some(_) => {
                nsym += 1;
                break;
            }
            None => break,
        }
        guard += 1;
    }
    nsym
}

// ================================================================== reloc

// Static-PIE relocation is the one place the loader dereferences the loaded
// image through values that come *from* the image itself (`DT_RELA`,
// `DT_HASH`, `DT_GNU_HASH`, `DT_SYMTAB`, and every `r_offset`). Each of those
// is attacker-controlled and must be checked against the `PT_LOAD` mapping the
// adapter actually installed before a single byte is read or written. The
// whole apply loop lives here over one bounds-vetted image slice, so there is
// no C pointer arithmetic left to walk off the mapping.

/// The `PT_LOAD` page extent that contains `va` and grants *all* bits of
/// `need`, or `None`. `va` must fall inside the segment's declared
/// `[va, va+memsz)` byte range — a `va` in the page padding below a segment, in
/// an inter-segment hole, or past the image is not mapped even when the flat
/// NOMMU allocation happens to cover it.
fn seg_window(plan: &ImagePlan, load_bias: u64, va: u64, need: i32) -> Option<(u64, u64)> {
    for i in 0..plan.nseg as usize {
        let s = plan.segments.get(i)?;
        if s.prot & need != need {
            continue;
        }
        let start = s.va.wrapping_add(load_bias);
        let end = start.checked_add(s.memsz)?;
        if start <= va && va < end {
            return Some((start, end));
        }
    }
    None
}

/// Validate that `[va, va+len)` is fully inside one mapped segment with
/// `need`. Returns the containing window so the caller can print/derive facts.
fn seg_span(plan: &ImagePlan, load_bias: u64, va: u64, len: u64, need: i32) -> Result<(u64, u64), i32> {
    let (start, end) = seg_window(plan, load_bias, va, need).ok_or(-ENOEXEC)?;
    let vend = va.checked_add(len).ok_or(-ENOEXEC)?;
    if vend > end {
        return Err(-ENOEXEC);
    }
    Ok((start, end))
}

/// Window `[va, end_of_containing_segment)` as an `(offset, len)` pair into the
/// image slice. `None` when `va` is not in a segment with `need`, or the
/// adapter's slice does not actually cover it (belt and braces: the slice is
/// derived from the same plan, but a mismatch must never become a raw read).
fn read_window(
    plan: &ImagePlan,
    load_bias: u64,
    image_base: u64,
    image_len: usize,
    va: u64,
    need: i32,
) -> Option<(usize, usize)> {
    let (_, end) = seg_window(plan, load_bias, va, need)?;
    let off = usize::try_from(va.checked_sub(image_base)?).ok()?;
    if off > image_len {
        return None;
    }
    let len = usize::try_from(end - va).ok()?;
    if len > image_len - off {
        return None;
    }
    Some((off, len))
}

#[inline]
fn rd_mut<'a>(image: &'a mut [u8], off: usize, width: usize) -> Option<&'a mut [u8]> {
    image.get_mut(off..off.checked_add(width)?)
}

#[inline]
fn wr_le(image: &mut [u8], off: usize, value: u64, width: usize) -> Result<(), i32> {
    let dst = rd_mut(image, off, width).ok_or(-ENOEXEC)?;
    let bytes = value.to_le_bytes();
    dst.copy_from_slice(&bytes[..width]);
    Ok(())
}

/// Apply the supported relocations of a loaded static PIE to `image`, which
/// must be the flat loaded image the adapter mapped (`[load_bias + min_page,
/// load_bias + max_page)`), and return how many entries were stored.
///
/// Every access is derived from `plan`:
///   * `.dynamic` must be fully contained in one readable `PT_LOAD`;
///   * `DT_RELA`/`DT_RELASZ` must name a readable table inside one `PT_LOAD`;
///   * `DT_HASH`/`DT_GNU_HASH`, when used to bound the symbol index, must lie
///     in a readable `PT_LOAD`, and the symbol walk is clamped to the end of
///     that segment rather than a fixed guess;
///   * `DT_SYMTAB` entries are checked against the mapping before they are
///     read;
///   * every `r_offset` destination must land, with room for the full store
///     width, inside a writable `PT_LOAD`.
///
/// Padding, holes, cross-segment spans and arithmetic overflow are refused with
/// `-ENOEXEC`; a target segment that exists but lacks the required permission
/// is refused the same way. A partially applied image is never used: the
/// caller discards it on any error.
pub fn apply_relocs(
    plan: &ImagePlan,
    load_bias: u64,
    image: &mut [u8],
    elf64: bool,
) -> Result<u32, i32> {
    // The image the adapter mapped spans the page-rounded extent of the
    // planned segments. Re-derive it and prove the slice covers it.
    let mut min_page = u64::MAX;
    let mut max_page = 0u64;
    for i in 0..plan.nseg as usize {
        let s = plan.segments.get(i).ok_or(-ENOEXEC)?;
        let start = s.va & !0xfff;
        let end = s
            .va
            .checked_add(s.memsz)
            .ok_or(-ENOEXEC)?
            .checked_add(0xfff)
            .ok_or(-ENOEXEC)?
            & !0xfff;
        if start < min_page {
            min_page = start;
        }
        if end > max_page {
            max_page = end;
        }
    }
    if plan.nseg == 0 || min_page == u64::MAX || max_page <= min_page {
        return Ok(0);
    }
    let image_base = load_bias.checked_add(min_page).ok_or(-ENOEXEC)?;
    if max_page - min_page > image.len() as u64 {
        return Err(-ENOEXEC);
    }

    if plan.dynamic.present == 0 || plan.dynamic.memsz == 0 {
        return Ok(0);
    }

    let (want, want_abs) = reloc_types(plan.ehdr.e_machine, elf64);
    if want == u32::MAX && want_abs == u32::MAX {
        return Ok(0);
    }

    // 1. `.dynamic`: the declared table must be wholly readable.
    let dyn_va = load_bias.checked_add(plan.dynamic.vaddr).ok_or(-ENOEXEC)?;
    seg_span(plan, load_bias, dyn_va, plan.dynamic.memsz, PROT_READ)?;
    let (dyn_off, dyn_len) =
        read_window(plan, load_bias, image_base, image.len(), dyn_va, PROT_READ)
            .ok_or(-ENOEXEC)?;
    let dyn_bytes = image
        .get(dyn_off..dyn_off.checked_add(dyn_len).ok_or(-ENOEXEC)?)
        .ok_or(-ENOEXEC)?;
    let dyninfo = scan_dynamic(dyn_bytes, elf64)?;
    if dyninfo.has_rela == 0 {
        return Ok(0);
    }

    // 2. Relocation table extent, bounded and readable.
    let min_rela: u64 = if elf64 { 24 } else { 12 };
    let ent = if dyninfo.relaent != 0 {
        dyninfo.relaent
    } else {
        min_rela
    };
    if ent < min_rela {
        return Err(-ENOEXEC);
    }
    let count = dyninfo.relasz / ent;
    if count == 0 {
        return Ok(0);
    }
    let span = count.checked_mul(ent).ok_or(-ENOEXEC)?;
    let rela_va = load_bias.checked_add(dyninfo.rela).ok_or(-ENOEXEC)?;
    seg_span(plan, load_bias, rela_va, span, PROT_READ)?;
    let rela_off = usize::try_from(rela_va.checked_sub(image_base).ok_or(-ENOEXEC)?).map_err(|_| -ENOEXEC)?;

    // 3. Symbols and the hash table that bounds them. Only an ABSOLUTE entry
    //    touches symbols, but if the image advertises a hash table it must
    //    still be inside the mapping; an address that is not is a malformed
    //    image, not a table to walk off the end of.
    let min_sym: u64 = if elf64 { 24 } else { 16 };
    let mut syment = dyninfo.syment;
    if syment == 0 {
        syment = min_sym;
    }
    if syment < min_sym {
        return Err(-ENOEXEC);
    }
    let mut nsyms = 0u64;
    if want_abs != u32::MAX {
        let mut sysv_view: Option<&[u8]> = None;
        let mut gnu_view: Option<&[u8]> = None;
        if dyninfo.hash_sysv != 0 {
            let hva = load_bias.checked_add(dyninfo.hash_sysv).ok_or(-ENOEXEC)?;
            let (off, len) = read_window(plan, load_bias, image_base, image.len(), hva, PROT_READ)
                .ok_or(-ENOEXEC)?;
            sysv_view = image.get(off..off.checked_add(len).ok_or(-ENOEXEC)?);
        } else if dyninfo.hash_gnu != 0 {
            let hva = load_bias.checked_add(dyninfo.hash_gnu).ok_or(-ENOEXEC)?;
            let (off, len) = read_window(plan, load_bias, image_base, image.len(), hva, PROT_READ)
                .ok_or(-ENOEXEC)?;
            gnu_view = image.get(off..off.checked_add(len).ok_or(-ENOEXEC)?);
        }
        nsyms = dynsym_count(sysv_view, gnu_view);
    }
    let sym_va = if dyninfo.symtab != 0 {
        Some(load_bias.checked_add(dyninfo.symtab).ok_or(-ENOEXEC)?)
    } else {
        None
    };

    let width: usize = if elf64 { 8 } else { 4 };
    let mut applied = 0u32;
    let mut eoff = 0usize;
    let mut remaining = count;
    while remaining > 0 {
        let eoff_end = eoff.checked_add(ent as usize).ok_or(-ENOEXEC)?;
        if eoff_end > (span as usize) {
            break;
        }
        let (r_offset, rel_type, symidx, addend) = if elf64 {
            (
                rd_u64(image, rela_off + eoff).ok_or(-ENOEXEC)?,
                (rd_u64(image, rela_off + eoff + 8).ok_or(-ENOEXEC)? & 0xffff_ffff) as u32,
                (rd_u64(image, rela_off + eoff + 8).ok_or(-ENOEXEC)? >> 32) as u32,
                rd_u64(image, rela_off + eoff + 16).ok_or(-ENOEXEC)?,
            )
        } else {
            (
                rd_u32(image, rela_off + eoff).ok_or(-ENOEXEC)? as u64,
                rd_u32(image, rela_off + eoff + 4).ok_or(-ENOEXEC)? & 0xff,
                rd_u32(image, rela_off + eoff + 4).ok_or(-ENOEXEC)? >> 8,
                rd_u32(image, rela_off + eoff + 8).ok_or(-ENOEXEC)? as u64,
            )
        };

        let maybe_abs = want_abs != u32::MAX && rel_type == want_abs;
        let mut st_info = 0u8;
        let mut st_shndx = 0u16;
        if maybe_abs {
            // Bound the index and the byte range before reading a single
            // symbol byte.
            if symidx as u64 >= nsyms {
                return Err(-ENOEXEC);
            }
            let sym = sym_va.ok_or(-ENOEXEC)?;
            let slot = (symidx as u64).checked_mul(syment).ok_or(-ENOEXEC)?;
            let sym_at = sym.checked_add(slot).ok_or(-ENOEXEC)?;
            let (soff, slen) =
                read_window(plan, load_bias, image_base, image.len(), sym_at, PROT_READ)
                    .ok_or(-ENOEXEC)?;
            if (syment as usize) > slen {
                // A symbol entry may not straddle the end of its segment.
                return Err(-ENOEXEC);
            }
            let s = image
                .get(soff..soff.checked_add(syment as usize).ok_or(-ENOEXEC)?)
                .ok_or(-ENOEXEC)?;
            if elf64 {
                st_info = *s.get(4).ok_or(-ENOEXEC)?;
                st_shndx = rd_u16(s, 6).ok_or(-ENOEXEC)?;
            } else {
                st_info = *s.get(12).ok_or(-ENOEXEC)?;
                st_shndx = rd_u16(s, 14).ok_or(-ENOEXEC)?;
            }
        }

        let act = reloc_action(rel_type, symidx, nsyms, st_info, st_shndx, want, want_abs)?;
        if act != RelocAction::Skip {
            // The destination must be a writable mapped range with room for
            // the whole store.
            let dst_va = load_bias.checked_add(r_offset).ok_or(-ENOEXEC)?;
            seg_span(plan, load_bias, dst_va, width as u64, PROT_WRITE)?;
            let dst_off = usize::try_from(dst_va.checked_sub(image_base).ok_or(-ENOEXEC)?)
                .map_err(|_| -ENOEXEC)?;

            let value = match act {
                RelocAction::Relative => load_bias.wrapping_add(addend),
                RelocAction::Absolute => {
                    let sym = sym_va.ok_or(-ENOEXEC)?;
                    let slot = (symidx as u64).checked_mul(syment).ok_or(-ENOEXEC)?;
                    let sym_at = sym.checked_add(slot).ok_or(-ENOEXEC)?;
                    let (soff, _) =
                        read_window(plan, load_bias, image_base, image.len(), sym_at, PROT_READ)
                            .ok_or(-ENOEXEC)?;
                    let s = image
                        .get(soff..soff.checked_add(syment as usize).ok_or(-ENOEXEC)?)
                        .ok_or(-ENOEXEC)?;
                    let (shndx, st_value) = if elf64 {
                        (rd_u16(s, 6).ok_or(-ENOEXEC)?, rd_u64(s, 8).ok_or(-ENOEXEC)?)
                    } else {
                        (rd_u16(s, 14).ok_or(-ENOEXEC)?, rd_u32(s, 4).ok_or(-ENOEXEC)? as u64)
                    };
                    // Undefined weak resolves to zero; the stronger policy
                    // verdicts were already enforced by `reloc_action`.
                    let symv = if shndx == 0 { 0 } else { st_value };
                    load_bias.wrapping_add(symv).wrapping_add(addend)
                }
                RelocAction::Skip => unreachable!(),
            };
            let value = if elf64 { value } else { value & 0xffff_ffff };
            wr_le(image, dst_off, value, width)?;
            applied = applied.wrapping_add(1);
        }

        eoff = eoff_end;
        remaining -= 1;
    }

    Ok(applied)
}

// ================================================================== stack

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct AuxvPair {
    pub key: u64,
    pub val: u64,
}

/// Everything the C writer needs to lay down the initial stack: the final
/// program SP plus the address of each string, the platform string, the random
/// window and the auxiliary vector.
#[repr(C)]
#[derive(Debug, PartialEq, Eq)]
pub struct StackPlan {
    pub sp: u64,
    pub platform_va: u64,
    pub random_va: u64,
    pub execfn_va: u64,
    pub env_count: u32,
    pub arg_count: u32,
    pub auxv_count: u32,
    pub _pad0: u32,
    pub env_addrs: [u64; MAX_ARG_STRINGS],
    pub arg_addrs: [u64; MAX_ARG_STRINGS],
    pub auxv: [AuxvPair; AUXV_MAX_PAIRS],
}

impl Default for StackPlan {
    fn default() -> Self {
        StackPlan {
            sp: 0,
            platform_va: 0,
            random_va: 0,
            execfn_va: 0,
            env_count: 0,
            arg_count: 0,
            auxv_count: 0,
            _pad0: 0,
            env_addrs: [0; MAX_ARG_STRINGS],
            arg_addrs: [0; MAX_ARG_STRINGS],
            auxv: [AuxvPair::default(); AUXV_MAX_PAIRS],
        }
    }
}

/// Aux-vector inputs derived from the load plan and the run-time facts.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct AuxvInput {
    pub phdr_va: u64,
    pub phentsize: u64,
    pub phnum: u64,
    pub page_size: u64,
    pub interp_base: u64,
    pub exec_entry: u64,
    pub hwcap: u64,
    pub hwcap2: u64,
    pub vdso_ehdr: u64,
    pub clktck: u64,
    pub secure: u64,
}

/// Build the fixed Linux auxiliary vector, substituting the addresses the
/// planner computed. Returns the pair count.
pub fn build_auxv(
    aux: &AuxvInput,
    platform_va: u64,
    random_va: u64,
    execfn_va: u64,
    out: &mut [AuxvPair; AUXV_MAX_PAIRS],
) -> usize {
    const AT_NULL: u64 = 0;
    const AT_PHDR: u64 = 3;
    const AT_PHENT: u64 = 4;
    const AT_PHNUM: u64 = 5;
    const AT_PAGESZ: u64 = 6;
    const AT_BASE: u64 = 7;
    const AT_FLAGS: u64 = 8;
    const AT_ENTRY: u64 = 9;
    const AT_UID: u64 = 11;
    const AT_EUID: u64 = 12;
    const AT_GID: u64 = 13;
    const AT_EGID: u64 = 14;
    const AT_PLATFORM: u64 = 15;
    const AT_HWCAP: u64 = 16;
    const AT_CLKTCK: u64 = 17;
    const AT_SECURE: u64 = 23;
    const AT_RANDOM: u64 = 25;
    const AT_HWCAP2: u64 = 26;
    const AT_SYSINFO_EHDR: u64 = 33;
    const AT_EXECFN: u64 = 31;

    let mut n = 0usize;
    let mut push = |key: u64, val: u64| {
        if let Some(slot) = out.get_mut(n) {
            *slot = AuxvPair { key, val };
            n += 1;
        }
    };
    push(AT_PHDR, aux.phdr_va);
    push(AT_PHENT, aux.phentsize);
    push(AT_PHNUM, aux.phnum);
    push(AT_PAGESZ, aux.page_size);
    push(AT_BASE, aux.interp_base);
    push(AT_FLAGS, 0);
    push(AT_ENTRY, aux.exec_entry);
    push(AT_UID, 0);
    push(AT_EUID, 0);
    push(AT_GID, 0);
    push(AT_EGID, 0);
    push(AT_PLATFORM, platform_va);
    push(AT_HWCAP, aux.hwcap);
    push(AT_CLKTCK, aux.clktck);
    push(AT_SECURE, aux.secure);
    push(AT_RANDOM, random_va);
    push(AT_HWCAP2, aux.hwcap2);
    push(AT_SYSINFO_EHDR, aux.vdso_ehdr);
    // A buffer-loaded image has no pathname; omit AT_EXECFN rather than
    // publishing a null string pointer.
    if execfn_va != 0 {
        push(AT_EXECFN, execfn_va);
    }
    push(AT_NULL, 0);
    n
}

/// Plan the conventional Linux initial stack. `argv`/`envp` are the string
/// bytes without their terminating NUL (the planner adds one byte each).
/// `ptr_size` is the target `sizeof(uintptr_t)` so the same maths serves the
/// 32-bit kernels. Returns the plan or a negative errno.
pub fn plan_stack(
    stack_top: u64,
    ptr_size: usize,
    argv: &[&[u8]],
    envp: &[&[u8]],
    platform: &[u8],
    execfn: &[u8],
    aux: &AuxvInput,
) -> Result<StackPlan, i32> {
    let ptr = ptr_size as u64;
    if argv.len() > MAX_ARG_STRINGS || envp.len() > MAX_ARG_STRINGS {
        return Err(-E2BIG);
    }
    let mut plan = StackPlan::default();
    let mut sp = stack_top;

    // envp strings, in order.
    let mut env_slots = plan.env_addrs.iter_mut();
    for e in envp.iter() {
        let len = e.len() as u64 + 1;
        sp = sp.checked_sub(len).ok_or(-ENOMEM)?;
        *env_slots.next().ok_or(-E2BIG)? = sp;
        plan.env_count += 1;
    }
    // argv strings, in reverse so argv[0] ends up lowest.
    for (i, a) in argv.iter().enumerate().rev() {
        let len = a.len() as u64 + 1;
        sp = sp.checked_sub(len).ok_or(-ENOMEM)?;
        *plan.arg_addrs.get_mut(i).ok_or(-E2BIG)? = sp;
        plan.arg_count += 1;
    }

    sp &= !15u64;

    // execfn/platform strings, then the 16-byte random window.
    let plat_len = platform.len() as u64 + 1;
    sp = sp.checked_sub(plat_len).ok_or(-ENOMEM)?;
    plan.platform_va = sp;
    if !execfn.is_empty() {
        let execfn_len = execfn.len() as u64 + 1;
        sp = sp.checked_sub(execfn_len).ok_or(-ENOMEM)?;
        plan.execfn_va = sp;
    }
    sp = sp.checked_sub(16).ok_or(-ENOMEM)?;
    plan.random_va = sp;

    let naux = build_auxv(aux, plan.platform_va, plan.random_va, plan.execfn_va,
                          &mut plan.auxv);
    plan.auxv_count = naux as u32;
    if naux > AUXV_MAX_PAIRS {
        return Err(-E2BIG);
    }

    let envc = plan.env_count as u64;
    let argc = plan.arg_count as u64;
    let vector_bytes =
        (naux as u64) * 2 * ptr + (envc + 1) * ptr + (argc + 1) * ptr + ptr;

    // Align the bottom of the vector area to 16 bytes.
    sp -= (sp.wrapping_sub(vector_bytes)) & 15;
    // auxv, envp pointer array, argv pointer array, then argc.
    sp -= (naux as u64) * 2 * ptr;
    sp -= (envc + 1) * ptr;
    sp -= (argc + 1) * ptr;
    sp -= ptr;
    plan.sp = sp;
    debug_assert_eq!(plan.sp & 15, 0);
    Ok(plan)
}

/// Native `a20_start_info_t` bootstrap fields plus the per-string addresses;
/// the Rust planner owns the ordering and the pointer arithmetic, the C
/// writer copies the bytes.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct StartInfoPlan {
    pub sp: u64,
    pub start_info_va: u64,
    pub argv_va: u64,
    pub envp_va: u64,
    pub argc: u32,
    pub envc: u32,
    pub _pad0: u32,
    pub size: u32,
    pub env_addrs: [u64; MAX_ARG_STRINGS],
    pub arg_addrs: [u64; MAX_ARG_STRINGS],
}

impl Default for StartInfoPlan {
    fn default() -> Self {
        StartInfoPlan {
            sp: 0,
            start_info_va: 0,
            argv_va: 0,
            envp_va: 0,
            argc: 0,
            envc: 0,
            _pad0: 0,
            size: 0,
            env_addrs: [0; MAX_ARG_STRINGS],
            arg_addrs: [0; MAX_ARG_STRINGS],
        }
    }
}

/// Plan the native-ABI initial stack: the env strings (in order), then the
/// argv strings (reversed so argv[0] ends up lowest), the envp pointer array,
/// the argv pointer array, and finally the `a20_start_info_t` struct placed at
/// the (16-byte aligned) bottom. Mirrors `elf_setup_stack_a20()`'s layout
/// exactly; the C writer copies strings/arrays to the planned addresses and
/// fills the descriptor.
pub fn plan_stack_a20(
    stack_top: u64,
    ptr_size: usize,
    argv: &[&[u8]],
    envp: &[&[u8]],
    start_info_size: u64,
) -> Result<StartInfoPlan, i32> {
    if argv.len() > MAX_ARG_STRINGS || envp.len() > MAX_ARG_STRINGS {
        return Err(-E2BIG);
    }
    let ptr = ptr_size as u64;
    let mut sp = stack_top;

    let mut plan = StartInfoPlan {
        argc: argv.len() as u32,
        envc: envp.len() as u32,
        size: start_info_size as u32,
        ..Default::default()
    };
    let mut env_slots = plan.env_addrs.iter_mut();
    for e in envp.iter() {
        sp = sp.checked_sub(e.len() as u64 + 1).ok_or(-ENOMEM)?;
        *env_slots.next().ok_or(-E2BIG)? = sp;
    }
    for (i, a) in argv.iter().enumerate().rev() {
        sp = sp.checked_sub(a.len() as u64 + 1).ok_or(-ENOMEM)?;
        *plan.arg_addrs.get_mut(i).ok_or(-E2BIG)? = sp;
    }
    sp &= !15u64;

    sp = sp.checked_sub((envp.len() as u64 + 1) * ptr).ok_or(-ENOMEM)?;
    plan.envp_va = sp;
    sp = sp.checked_sub((argv.len() as u64 + 1) * ptr).ok_or(-ENOMEM)?;
    plan.argv_va = sp;
    sp = sp.checked_sub(start_info_size).ok_or(-ENOMEM)?;
    sp &= !15u64;

    plan.sp = sp;
    plan.start_info_va = sp;
    Ok(plan)
}

/// A dynamically linked native program keeps its `a20_start_info_t` in the
/// reserved top of the stack while the ELF interpreter below gets the
/// conventional Linux layout. Returns the start-info address, which is also
/// the stack top the conventional planner must build under. Mirrors
/// `elf_setup_stack_a20_dynamic()`'s placement (no extra alignment, matching
/// the C reference).
pub fn plan_stack_a20_dynamic(stack_top: u64, start_info_size: u64) -> u64 {
    stack_top.wrapping_sub(start_info_size)
}

// ================================================================== creds

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct CredInput {
    pub no_new_privs: u8,
    pub _pad0: [u8; 7],
    pub uid: i32,
    pub euid: i32,
    pub suid: i32,
    pub egid: i32,
    pub fsuid: i32,
    pub fsgid: i32,
    pub sgid: i32,
    pub st_uid: i32,
    pub st_gid: i32,
    pub st_mode: u32,
    pub _pad1: u32,
    pub cap_bounding: u64,
    pub cap_permitted: u64,
    pub cap_effective: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct CredPlan {
    pub uid: i32,
    pub euid: i32,
    pub suid: i32,
    pub egid: i32,
    pub fsuid: i32,
    pub fsgid: i32,
    pub sgid: i32,
    pub cap_permitted: u64,
    pub cap_effective: u64,
    pub make_undumpable: u8,
    pub _pad0: [u8; 7],
}

/// Decision form of `proc_apply_exec_creds()`: setuid/setgid elevation,
/// `no_new_privs` stickiness, capability reconstruction, and the dumpable
/// transition. The caller applies the returned values to the task credential.
pub fn plan_exec_creds(input: &CredInput) -> CredPlan {
    let mut p = CredPlan {
        uid: input.uid,
        euid: input.euid,
        suid: input.suid,
        egid: input.egid,
        fsuid: input.fsuid,
        fsgid: input.fsgid,
        sgid: input.sgid,
        cap_permitted: input.cap_permitted,
        cap_effective: input.cap_effective,
        make_undumpable: 0,
        _pad0: [0; 7],
    };

    if input.no_new_privs != 0 {
        p.suid = p.euid;
        p.sgid = p.egid;
        return p;
    }

    let old_uid = input.uid;
    let old_euid = input.euid;
    let old_suid = input.suid;
    let old_egid = input.egid;

    if input.st_mode & S_ISUID != 0 {
        p.euid = input.st_uid;
        p.suid = p.euid;
        p.fsuid = p.euid;
    }
    if input.st_mode & S_ISGID != 0 {
        p.egid = input.st_gid;
        p.sgid = p.egid;
        p.fsgid = p.egid;
    }

    if old_euid != p.euid || old_egid != p.egid {
        p.make_undumpable = 1;
    }

    let old_had_root = old_uid == 0 || old_euid == 0 || old_suid == 0;
    let new_has_root = p.uid == 0 || p.euid == 0 || p.suid == 0;
    if old_had_root && !new_has_root {
        p.cap_effective = 0;
        p.cap_permitted = 0;
    } else if p.euid == 0 {
        p.cap_permitted = input.cap_bounding;
        p.cap_effective = p.cap_permitted;
    } else if old_euid == 0 {
        p.cap_effective = 0;
    }
    p
}

// ================================================================== commit

/// Ordered image replacement phases. Failable preparation completes before
/// sibling termination; from that phase onward process-wide changes cannot be
/// rolled back. The Rust runner owns sequencing, resource unwind and boundary.
#[repr(u32)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum CommitPhase {
    StackBuild = 0,
    VdsoMap,
    AllocNewMm,
    PublishAuxv,
    FdtableUnshare,
    TerminateSiblings,
    CloseOnExec,
    TrapContext,
    Creds,
    CpuEnter,
}

/// The full commit sequence, in execution order.
pub const COMMIT_PHASES: [CommitPhase; 10] = [
    CommitPhase::StackBuild,
    CommitPhase::VdsoMap,
    CommitPhase::AllocNewMm,
    CommitPhase::PublishAuxv,
    CommitPhase::FdtableUnshare,
    CommitPhase::TerminateSiblings,
    CommitPhase::CloseOnExec,
    CommitPhase::TrapContext,
    CommitPhase::Creds,
    CommitPhase::CpuEnter,
];

/// Look up a commit phase by its discriminant.
pub fn phase_from_index(v: u32) -> Option<CommitPhase> {
    COMMIT_PHASES.get(v as usize).copied()
}

/// Whether aborting at `phase` is reachable: the phase performs a fallible
/// operation. The test profile arms its fault injection only at these stages,
/// so the set of genuinely failable phases stays Rust-owned rather than being
/// re-listed by the probe.
pub fn phase_is_failable(phase: CommitPhase) -> bool {
    matches!(
        phase,
        CommitPhase::StackBuild
            | CommitPhase::VdsoMap
            | CommitPhase::AllocNewMm
            | CommitPhase::FdtableUnshare
    )
}

/// Kernel mechanisms used by the exec transaction. Rust owns the phase
/// sequence, ABI transition, resource registration, commit point and unwind;
/// each callback performs one kernel operation which cannot be expressed
/// safely without the C task/mm/page-table layout.
pub trait ExecOps {
    fn build_linux_stack(&mut self) -> i32;
    fn build_native_stack(&mut self) -> i32;
    fn map_vdso(&mut self) -> i32;
    fn get_abi_mode(&mut self) -> i32;
    fn set_abi_mode(&mut self, mode: i32);
    fn allocate_new_mm(&mut self) -> Result<u64, i32>;
    fn publish_auxv(&mut self);
    fn unshare_fdtable(&mut self) -> i32;
    fn terminate_siblings(&mut self);
    fn close_on_exec(&mut self);
    fn install_trap_context(&mut self);
    fn apply_credentials(&mut self);
    fn enter_new_mm(&mut self);
    fn undo_resource(&mut self, kind: u32, handle: u64);
    fn inject_failure(&mut self, _phase: CommitPhase, _after: bool) -> bool {
        false
    }
}

/// Run the kernel exec replacement protocol. The image is already loaded and
/// is the first resource. Fallible mechanisms run before the trap-context
/// swap; after that swap the remaining callbacks are contractually infallible.
/// A test injection may fail after a fallible operation has completed, which
/// exercises cleanup of resources that phase-entry injection never reaches.
pub fn run_exec_transaction(
    txn: &mut ExecTxn,
    image: u64,
    native_abi: bool,
    ops: &mut impl ExecOps,
) -> i32 {
    *txn = txn_init();
    if txn_acquire(txn, RES_IMAGE, image) < 0 {
        return -ENOMEM;
    }

    loop {
        let phase_raw = txn_begin(txn);
        if phase_raw == TXN_DONE {
            return txn_commit(txn);
        }
        if phase_raw < 0 {
            return rollback_exec_transaction(txn, ops, -EINVAL);
        }
        let phase = match phase_from_index(phase_raw as u32) {
            Some(phase) => phase,
            None => return rollback_exec_transaction(txn, ops, -EINVAL),
        };

        if phase_is_failable(phase) && ops.inject_failure(phase, false) {
            return rollback_exec_transaction(txn, ops, -ENOMEM);
        }

        let rc = match phase {
            CommitPhase::StackBuild => {
                if native_abi { ops.build_native_stack() } else { ops.build_linux_stack() }
            }
            CommitPhase::VdsoMap => ops.map_vdso(),
            CommitPhase::AllocNewMm => {
                let old_mode = ops.get_abi_mode();
                if txn_acquire(txn, RES_ABI_MODE, old_mode as u32 as u64) < 0 {
                    -ENOMEM
                } else {
                    ops.set_abi_mode(if native_abi { 1 } else { 0 });
                    match ops.allocate_new_mm() {
                        Ok(handle) => {
                            if txn_acquire(txn, RES_NEW_MM, handle) < 0 {
                                ops.undo_resource(RES_NEW_MM, handle);
                                -ENOMEM
                            } else {
                                0
                            }
                        }
                        Err(error) => error,
                    }
                }
            }
            CommitPhase::PublishAuxv => { ops.publish_auxv(); 0 }
            CommitPhase::FdtableUnshare => ops.unshare_fdtable(),
            CommitPhase::TerminateSiblings => { ops.terminate_siblings(); 0 }
            CommitPhase::CloseOnExec => { ops.close_on_exec(); 0 }
            CommitPhase::TrapContext => { ops.install_trap_context(); 0 }
            CommitPhase::Creds => { ops.apply_credentials(); 0 }
            CommitPhase::CpuEnter => { ops.enter_new_mm(); 0 }
        };

        if rc != 0 {
            if !phase_is_failable(phase) {
                /* A callback after the irreversible boundary must never fail. */
                return -EINVAL;
            }
            return rollback_exec_transaction(txn, ops, rc);
        }

        // Only these phases can be tested after success without leaving an
        // irreversible process-wide side effect behind. In particular an
        // after-effect failure after fdtable unshare would change sharing.
        let rollback_safe_after = matches!(phase,
            CommitPhase::StackBuild | CommitPhase::VdsoMap | CommitPhase::AllocNewMm);
        if rollback_safe_after && ops.inject_failure(phase, true) {
            return rollback_exec_transaction(txn, ops, -ENOMEM);
        }

    }
}

fn rollback_exec_transaction(txn: &mut ExecTxn, ops: &mut impl ExecOps, error: i32) -> i32 {
    while let Some((kind, handle)) = txn_undo_next(txn) {
        ops.undo_resource(kind, handle);
    }
    let _ = txn_abort(txn);
    error
}

/// Maximum live resources one exec transaction can track. The image, the
/// changed ABI mode and the new `mm_struct` are the three the current
/// sequence holds; four slots of headroom keep a future phase from silently
/// dropping a handle on the floor.
pub const TXN_MAX_RESOURCES: usize = 8;

/// What a live ledger entry is, so the C adapter knows how to release it.
/// These are protocol discriminants, not decisions.
pub const RES_IMAGE: u32 = 1;
pub const RES_NEW_MM: u32 = 2;
pub const RES_ABI_MODE: u32 = 3;

/* Transaction states (mirrored in elf_rust.h). */
pub const TXN_RUNNING: u32 = 0;
pub const TXN_COMMITTED: u32 = 1;
pub const TXN_ABORTED: u32 = 2;

/// `a20_elf_txn_begin` sentinel: every phase has run.
pub const TXN_DONE: i32 = -ENOENT;

/// One held resource: a protocol kind plus an opaque adapter handle.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct TxnResource {
    pub kind: u32,
    pub _pad0: u32,
    pub handle: u64,
}

/// A real exec-replacement transaction. The C executor allocates one on its
/// stack, drives the phases through `begin`, and registers each resource it
/// acquires with `acquire`. The live set — not a static phase table — is what
/// `undo_*` hands back after a failure, so a phase that acquires a new
/// resource is rolled back by construction, and `commit` is refused until the
/// whole sequence has run.
///
/// The reversible/irreversible boundary is Rust-owned too: `irreversible`
/// becomes 1 as soon as `TerminateSiblings` begins, after which neither
/// `abort` nor `undo_next` will release anything.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ExecTxn {
    pub phase: u32,
    pub state: u32,
    pub live: u32,
    pub irreversible: u32,
    pub res: [TxnResource; TXN_MAX_RESOURCES],
}

impl Default for ExecTxn {
    fn default() -> Self {
        ExecTxn {
            phase: 0,
            state: TXN_RUNNING,
            live: 0,
            irreversible: 0,
            res: [TxnResource::default(); TXN_MAX_RESOURCES],
        }
    }
}

/// Begin a fresh transaction over the fixed commit sequence.
pub fn txn_init() -> ExecTxn {
    ExecTxn::default()
}

/// Return the next phase to execute and advance the cursor. `TXN_DONE` when
/// the sequence is exhausted; `-EINVAL` when the transaction is not running.
///
/// Starting `TerminateSiblings` is the commit point: sibling termination and
/// CLOEXEC are irreversible process-wide effects. All fallible preparation
/// must have completed before this phase.
pub fn txn_begin(txn: &mut ExecTxn) -> i32 {
    if txn.state != TXN_RUNNING {
        return -EINVAL;
    }
    match COMMIT_PHASES.get(txn.phase as usize) {
        Some(p) => {
            if txn.phase >= CommitPhase::TerminateSiblings as u32 {
                txn.irreversible = 1;
            }
            txn.phase += 1;
            *p as i32
        }
        None => TXN_DONE,
    }
}

/// Register a resource the current phase just acquired. Returns `-ENOMEM`
/// when the ledger is full and `-EINVAL` after the transaction has ended.
pub fn txn_acquire(txn: &mut ExecTxn, kind: u32, handle: u64) -> i32 {
    if txn.state != TXN_RUNNING {
        return -EINVAL;
    }
    let i = txn.live as usize;
    if i >= TXN_MAX_RESOURCES {
        return -ENOMEM;
    }
    match txn.res.get_mut(i) {
        Some(slot) => {
            *slot = TxnResource {
                kind,
                _pad0: 0,
                handle,
            };
            txn.live += 1;
            0
        }
        None => -ENOMEM,
    }
}

/// Hand a resource to its permanent owner, dropping it from the live set. The
/// commit point transfers the new image and mm; anything transferred is not
/// rolled back.
pub fn txn_transfer(txn: &mut ExecTxn, kind: u32, handle: u64) -> i32 {
    if txn.state != TXN_RUNNING {
        return -EINVAL;
    }
    let live = txn.live as usize;
    let mut found = usize::MAX;
    for i in 0..live {
        match txn.res.get(i) {
            Some(r) if r.kind == kind && r.handle == handle => {
                found = i;
                break;
            }
            _ => {}
        }
    }
    if found == usize::MAX {
        return -EINVAL;
    }
    // Compact the tail left by one slot. All accesses go through `get`/
    // `get_mut` so the core links without the Rust panic runtime.
    for j in found..live.saturating_sub(1) {
        let next = match txn.res.get(j + 1) {
            Some(r) => *r,
            None => return -EINVAL,
        };
        match txn.res.get_mut(j) {
            Some(dst) => *dst = next,
            None => return -EINVAL,
        }
    }
    if let Some(last) = txn.res.get_mut(live - 1) {
        *last = TxnResource::default();
    }
    txn.live -= 1;
    0
}

/// The replacement completed: the whole sequence ran and every live resource
/// is now owned by the new image. Refused (`-EINVAL`) if a phase has not run,
/// which is the invariant that keeps `commit` from hiding a failed setup.
pub fn txn_commit(txn: &mut ExecTxn) -> i32 {
    if txn.state != TXN_RUNNING || (txn.phase as usize) != COMMIT_PHASES.len() {
        return -EINVAL;
    }
    txn.state = TXN_COMMITTED;
    txn.live = 0;
    0
}

/// The replacement failed before the commit point. The live set is what the
/// adapter must release. Refused (`-EPERM`) once sibling termination starts:
/// the old process state cannot be restored by then.
pub fn txn_abort(txn: &mut ExecTxn) -> i32 {
    if txn.state != TXN_RUNNING {
        return -EINVAL;
    }
    if txn.irreversible != 0 {
        return -EPERM;
    }
    txn.state = TXN_ABORTED;
    0
}

/// Hand back the newest live resource and drop it from the ledger, so a
/// partial setup unwinds in reverse acquisition order and every resource is
/// released exactly once. `None` when the transaction is not running, holds
/// nothing, or has already crossed the irreversible boundary (process teardown
/// has started, so returning to the old image is unsafe). Driving rollback through the ledger
/// (rather than index accessors) keeps the C side from deciding the order or
/// the set.
pub fn txn_undo_next(txn: &mut ExecTxn) -> Option<(u32, u64)> {
    if txn.state != TXN_RUNNING || txn.irreversible != 0 || txn.live == 0 {
        return None;
    }
    let i = (txn.live - 1) as usize;
    let slot = match txn.res.get(i) {
        Some(r) => *r,
        None => return None,
    };
    if let Some(dst) = txn.res.get_mut(i) {
        *dst = TxnResource::default();
    }
    txn.live -= 1;
    Some((slot.kind, slot.handle))
}

// ================================================================== interp

/// Interpreter search-result classification. The adapter performs the opens
/// and supplies which candidate succeeded; the ordering itself is Rust-owned.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum InterpResolution {
    /// 1. the exact `PT_INTERP` path opened
    Exact,
    /// 2. resolved relative to the executable's mount root
    MountRelative,
    /// 3. sibling of the executable
    Sibling,
    /// 4. sibling `libc.so` (musl doubles as the loader)
    SiblingLibc,
    /// 5. architecture-specific fallback table
    ArchFallback,
    /// none of the candidates opened
    NotFound,
}

impl InterpResolution {
    pub fn from_attempt(index: usize, opened: bool) -> InterpResolution {
        if !opened {
            return InterpResolution::NotFound;
        }
        match index {
            0 => InterpResolution::Exact,
            1 => InterpResolution::MountRelative,
            2 => InterpResolution::Sibling,
            3 => InterpResolution::SiblingLibc,
            _ => InterpResolution::ArchFallback,
        }
    }
}

/// How many candidate shapes the interpreter resolution ladder has. The C
/// adapter performs the opens; the count and the classification keep the
/// ordering itself Rust-owned.
pub const INTERP_ATTEMPTS: usize = 5;

/// Classify one ladder attempt. Returns the `InterpResolution` discriminant,
/// or -1 for "nothing opened" (`NotFound`).
pub fn interp_resolution(index: usize, opened: bool) -> i32 {
    match InterpResolution::from_attempt(index, opened) {
        InterpResolution::Exact => 0,
        InterpResolution::MountRelative => 1,
        InterpResolution::Sibling => 2,
        InterpResolution::SiblingLibc => 3,
        InterpResolution::ArchFallback => 4,
        InterpResolution::NotFound => -1,
    }
}

// ================================================================== foreign

/// Result of the foreign-architecture probe.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ForeignKey {
    pub machine: u16,
    pub abi: u8,
    pub is_foreign: u8,
    pub _pad0: u8,
}

/// Decide whether a file the loader rejected is a structurally valid
/// *foreign* ELF. Mirrors `elf_probe_foreign()`: a native machine is not
/// foreign, and a corrupt header is not foreign either. The ABI is derived
/// from the native marker in the program headers.
pub fn probe_foreign(
    ehdr_bytes: &[u8],
    phdr_bytes: &[u8],
    expect_class: u8,
    expect_machine: u16,
) -> Result<ForeignKey, i32> {
    let eh = parse_ehdr(ehdr_bytes).ok_or(-ENOEXEC)?;
    if eh.class != expect_class
        || eh.data != ELFDATA2LSB
        || (eh.e_type != ET_EXEC && eh.e_type != ET_DYN)
    {
        return Err(-ENOEXEC);
    }
    let min_phdr = if eh.class == ELFCLASS32 {
        PHDR32_SIZE
    } else {
        PHDR64_SIZE
    };
    if (eh.e_phentsize as usize) < min_phdr || eh.e_phnum == 0 || eh.e_phnum as usize > MAX_PHDRS
    {
        return Err(-ENOEXEC);
    }
    if eh.e_machine == expect_machine {
        return Err(-ENOEXEC);
    }
    let entsize = eh.e_phentsize as usize;
    let elf32 = eh.class == ELFCLASS32;
    let mut native = false;
    if entsize.checked_mul(eh.e_phnum as usize).ok_or(-ENOEXEC)? <= phdr_bytes.len() {
        for i in 0..eh.e_phnum as usize {
            if let Some(ph) = parse_phdr(phdr_bytes, i, entsize, elf32) {
                if ph.p_type == PT_A20_START_INFO {
                    native = true;
                    break;
                }
            }
        }
    }
    Ok(ForeignKey {
        machine: eh.e_machine,
        abi: u8::from(native),
        is_foreign: 1,
        _pad0: 0,
    })
}

// ================================================================== shebang

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ShebangPlan {
    pub is_script: u8,
    pub _pad0: [u8; 7],
    pub interp_off: u32,
    pub interp_len: u32,
    pub arg_off: u32,
    pub arg_len: u32,
}

/// Parse a `#!` line. Returns `is_script=0` for a non-script so the caller
/// proceeds to ELF. Mirrors the field extraction in `exec_try_script()`.
pub fn plan_shebang(line: &[u8]) -> Result<ShebangPlan, i32> {
    if line.len() < 2 || line[0] != b'#' || line[1] != b'!' {
        return Ok(ShebangPlan::default());
    }
    let mut i = 2usize;
    while i < line.len() && (line[i] == b' ' || line[i] == b'\t') {
        i += 1;
    }
    if i >= line.len() || line[i] == b'\n' || line[i] == b'\r' {
        return Err(-ENOEXEC);
    }
    let start = i;
    while i < line.len() && line[i] != b'\n' && line[i] != b'\r' && line[i] != b' ' && line[i] != b'\t'
    {
        i += 1;
    }
    let interp_len = i - start;
    if interp_len == 0 {
        return Err(-ENOEXEC);
    }
    while i < line.len() && (line[i] == b' ' || line[i] == b'\t') {
        i += 1;
    }
    let mut arg_off = 0u32;
    let mut arg_len = 0u32;
    if i < line.len() && line[i] != b'\n' && line[i] != b'\r' {
        let astart = i;
        while i < line.len() && line[i] != b'\n' && line[i] != b'\r' {
            i += 1;
        }
        arg_off = astart as u32;
        arg_len = (i - astart) as u32;
    }
    Ok(ShebangPlan {
        is_script: 1,
        _pad0: [0; 7],
        interp_off: start as u32,
        interp_len: interp_len as u32,
        arg_off,
        arg_len,
    })
}

// ================================================================== bprm

/// `MAX_ARG_STRLEN`-style per-string cap and the aggregate budget the C
/// `exec_copy_args()` enforced. Rust owns the limit arithmetic; the adapter
/// `MAX_ARG_STRLEN`-style per-string cap and the aggregate budget the C
/// `exec_copy_args()`/`xlator_inject_env()` enforced. Rust owns the limit
/// arithmetic; the adapter performs the `copy_from_user`.
///
/// A `max_bytes` or `stack_quarter` of zero disables that cap. The C call
/// sites always pass `MAX_ARG_BYTES` (a real cap) for `max_bytes`; the
/// per-task `limits.stack / 4` may be absent, and the C reference skips that
/// check when the budget is zero.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct BprmLimits {
    pub max_strlen: u64,
    pub max_bytes: u64,
    pub stack_quarter: u64,
}

/// Decide whether one more `len`-byte string (NUL included) fits. Returns the
/// new total or a negative errno. Mirrors `exec_copy_args()`'s `E2BIG` paths.
pub fn check_arg_bytes(total: u64, len: u64, limits: &BprmLimits) -> Result<u64, i32> {
    if len >= limits.max_strlen {
        return Err(-E2BIG);
    }
    let next = total.checked_add(len).ok_or(-E2BIG)?;
    if limits.max_bytes != 0 && next > limits.max_bytes {
        return Err(-E2BIG);
    }
    if limits.stack_quarter != 0 && next > limits.stack_quarter {
        return Err(-E2BIG);
    }
    Ok(next)
}

/// Outcome classification of an `elf_load()` return code.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum ElfOutcome {
    Loaded,
    /// `-ENOEXEC`: try shebang, then the foreign translator.
    FormatMismatch,
    Failed(i32),
}

pub fn classify_elf(rc: i32) -> ElfOutcome {
    if rc == 0 {
        ElfOutcome::Loaded
    } else if rc == -ENOEXEC {
        ElfOutcome::FormatMismatch
    } else {
        ElfOutcome::Failed(rc)
    }
}

/// What the format-resolution loop should do next. Mirrors `proc_exec()`'s
/// `EXEC_DONE`/`EXEC_RETRY` control flow. The `STEP_*` constants are the FFI
/// spelling the C executor switches on.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Step {
    Install,
    Retry,
    /// Continue to the next probe in the same iteration (script -> xlator).
    Continue,
    Fail(i32),
}

pub const STEP_INSTALL: i32 = 0;
pub const STEP_RETRY: i32 = 1;
pub const STEP_CONTINUE: i32 = 2;
pub const STEP_FAIL: i32 = 3;

impl Step {
    pub fn as_ffi(self) -> i32 {
        match self {
            Step::Install => STEP_INSTALL,
            Step::Retry => STEP_RETRY,
            Step::Continue => STEP_CONTINUE,
            Step::Fail(_) => STEP_FAIL,
        }
    }
}

pub fn step_after_elf(o: ElfOutcome) -> Step {
    match o {
        ElfOutcome::Loaded => Step::Install,
        ElfOutcome::Failed(e) => Step::Fail(e),
        ElfOutcome::FormatMismatch => Step::Continue,
    }
}

/// `exec_try_script()` result: `EXEC_RETRY`(1) rewrites and retries, `0` means
/// "not a script", negative is fatal.
pub fn step_after_script(rc: i32) -> Step {
    if rc == 1 {
        Step::Retry
    } else if rc < 0 {
        Step::Fail(rc)
    } else {
        Step::Continue
    }
}

/// `exec_try_translator()` result, gated on `CONFIG_XLATOR`.
pub fn step_after_xlator(enabled: bool, rc: i32) -> Step {
    if !enabled {
        return Step::Fail(-ENOEXEC);
    }
    if rc == 1 {
        Step::Retry
    } else if rc < 0 {
        Step::Fail(rc)
    } else {
        Step::Fail(-ENOEXEC)
    }
}

pub fn depth_exhausted(depth: u32, max_depth: u32) -> bool {
    depth >= max_depth
}

/// Shebang argv rewrite ordering: `[interp, (interp_arg), script_path, argv[1..]]`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ShebangArgvPlan {
    pub new_argc: u32,
    pub has_arg: u8,
    pub _pad0: [u8; 3],
    pub script_pos: u32,
    pub first_orig_src_index: u32,
}

/// Plan the new argv length and slot positions for a `#!` rewrite, capped at
/// `max_args`. Mirrors the ownership dance in `exec_try_script()`.
pub fn plan_shebang_argv(has_arg: bool, orig_argc: usize, max_args: usize) -> ShebangArgvPlan {
    let script_pos = 1 + u32::from(has_arg);
    let orig_tail = orig_argc.saturating_sub(1);
    let mut new_argc = (2 + u32::from(has_arg)) as usize + orig_tail;
    if new_argc > max_args {
        new_argc = max_args;
    }
    ShebangArgvPlan {
        new_argc: new_argc as u32,
        has_arg: u8::from(has_arg),
        _pad0: [0; 3],
        script_pos,
        first_orig_src_index: 1,
    }
}

// ================================================================== kexec

/// Mirrors `KEXEC_MAX_SEGMENTS`: the wire protocol allows at most this many
/// segments, so the staged-validation arrays cannot overflow.
pub const KEXEC_MAX_SEGMENTS: usize = 16;

/// One kexec segment as it arrives from user space. Only the layout fields
/// are decisions; `buf` is a user pointer the adapter copies from and is
/// never dereferenced here.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct KexecSegmentWire {
    pub buf: u64,
    pub bufsz: u64,
    pub mem: u64,
    pub memsz: u64,
}

/// The wire-count gate `kexec_load_segments()` applies before copying
/// anything: at least one segment, at most `KEXEC_MAX_SEGMENTS`.
pub fn kexec_wire_ok(nr_segments: u64, max_segments: u64) -> bool {
    nr_segments >= 1 && nr_segments <= max_segments
}

/// Validate one kexec segment against the running kernel's physical extent
/// and every segment staged so far. Mirrors `kexec_load_segments()`'s
/// checks with wrap-safe arithmetic: `-EINVAL` for an impossible segment,
/// `-EADDRNOTAVAIL` for an overlap (with the kernel image or a predecessor).
/// The C adapter copies the bytes and stages the pages; it feeds each
/// accepted segment back in via `staged` so the pairwise overlap decision
/// stays here.
pub fn kexec_segment_ok(
    seg: &KexecSegmentWire,
    kstart: u64,
    kend: u64,
    staged: &[KexecSegmentWire],
) -> Result<(), i32> {
    if seg.bufsz > seg.memsz {
        return Err(-EINVAL);
    }
    let end = seg.mem.checked_add(seg.memsz).ok_or(-EINVAL)?;
    if seg.mem < kend && kstart < end {
        return Err(-EADDRNOTAVAIL);
    }
    for prev in staged {
        // Predecessors were accepted, so their mem+memsz addition is sound.
        let pstart = prev.mem;
        let pend = prev.mem + prev.memsz;
        if seg.mem < pend && pstart < end {
            return Err(-EADDRNOTAVAIL);
        }
    }
    Ok(())
}

/// The kexec_file_load magic gate: the C backend owns relocation, so all the
/// loader promises here is the four-byte ELF signature (matching the C
/// reference, which reads exactly four bytes).
pub fn is_elf_image(b: &[u8]) -> bool {
    b.get(0..4) == Some(&[0x7f, b'E', b'L', b'F'])
}

// -------------------------------------------------------------- kexec ledger
//
// Rust owns the kexec image lifecycle end to end: a staging transaction is
// opened, each validated segment reserves a slot, the adapter's page chain is
// attached to it, and only a fully-staged transaction can be published as the
// loaded image. Release (rollback of a failed load, discard, or replacement)
// hands the still-held chains back newest-first, so every chain is freed
// exactly once. The C adapter keeps only the primitives: frame allocation and
// free, usercopy, and the lock around the published ledger.

/// Ledger states. `Staging` is a local, unpublished transaction; `Loaded` is
/// the image `reboot(LINUX_REBOOT_CMD_KEXEC)` would enter.
pub const KX_EMPTY: u32 = 0;
pub const KX_STAGING: u32 = 1;
pub const KX_LOADED: u32 = 2;

/// One staged segment: the opaque page-chain handle the adapter allocated (0
/// for a zero-length segment) plus the wire facts kept for the commit.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct KexecSlot {
    pub pages: u64,
    pub pages_bytes: u64,
    pub mem: u64,
    pub bufsz: u64,
    pub memsz: u64,
}

/// The kexec image/staging ledger.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct KexecLedger {
    pub state: u32,
    pub file_backed: u32,
    pub nr: u32,
    pub nstaged: u32,
    pub entry: u64,
    pub seg: [KexecSlot; KEXEC_MAX_SEGMENTS],
}

impl Default for KexecLedger {
    fn default() -> Self {
        KexecLedger {
            state: KX_EMPTY,
            file_backed: 0,
            nr: 0,
            nstaged: 0,
            entry: 0,
            seg: [KexecSlot::default(); KEXEC_MAX_SEGMENTS],
        }
    }
}

/// A fresh, empty ledger.
pub fn kx_init() -> KexecLedger {
    KexecLedger::default()
}

/// Begin a staging transaction on an empty ledger. The wire count is checked
/// here (at least one, at most `max_segments`), so no caller can stage an
/// unbounded number of segments.
pub fn kx_stage_open(
    l: &mut KexecLedger,
    entry: u64,
    nr_segments: u64,
    max_segments: u64,
    file_backed: bool,
) -> Result<(), i32> {
    if l.state != KX_EMPTY || !kexec_wire_ok(nr_segments, max_segments) {
        return Err(-EINVAL);
    }
    l.state = KX_STAGING;
    l.file_backed = file_backed as u32;
    l.nr = nr_segments as u32;
    l.nstaged = 0;
    l.entry = entry;
    Ok(())
}

/// Whether `[mem, end)` overlaps any already-staged predecessor. Predecessors
/// were accepted, so their `mem + memsz` is sound; a wrapped predecessor is
/// treated as an overlap so it can never be skipped.
fn kx_overlaps(l: &KexecLedger, mem: u64, end: u64) -> bool {
    let n = l.nstaged as usize;
    let mut i = 0;
    while i < n {
        match l.seg.get(i) {
            Some(prev) => {
                let pend = match prev.mem.checked_add(prev.memsz) {
                    Some(e) => e,
                    None => return true,
                };
                if mem < pend && prev.mem < end {
                    return true;
                }
            }
            None => return true,
        }
        i += 1;
    }
    false
}

/// Validate one user segment against the running-kernel extent and every
/// already-staged predecessor, then reserve the next slot. Returns the slot
/// index. `buf` is a user pointer the adapter copies from; it is carried, not
/// dereferenced.
pub fn kx_stage_check(
    l: &mut KexecLedger,
    buf: u64,
    bufsz: u64,
    mem: u64,
    memsz: u64,
    kstart: u64,
    kend: u64,
) -> Result<u32, i32> {
    if l.state != KX_STAGING {
        return Err(-EINVAL);
    }
    let seg = KexecSegmentWire {
        buf,
        bufsz,
        mem,
        memsz,
    };
    // Wire rules (bufsz <= memsz) and the running-kernel overlap.
    kexec_segment_ok(&seg, kstart, kend, &[])?;
    let end = mem.checked_add(memsz).ok_or(-EINVAL)?;
    if kx_overlaps(l, mem, end) {
        return Err(-EADDRNOTAVAIL);
    }
    kx_reserve(l, mem, bufsz, memsz)
}

/// Reserve a slot for a file-backed segment: no destination/overlap policy
/// applies because the architecture backend relocates the image, matching the
/// reviewed C reference.
pub fn kx_stage_file(l: &mut KexecLedger, size: u64) -> Result<u32, i32> {
    if l.state != KX_STAGING {
        return Err(-EINVAL);
    }
    kx_reserve(l, 0, size, size)
}

fn kx_reserve(l: &mut KexecLedger, mem: u64, bufsz: u64, memsz: u64) -> Result<u32, i32> {
    let i = l.nstaged as usize;
    if i >= KEXEC_MAX_SEGMENTS {
        return Err(-EINVAL);
    }
    match l.seg.get_mut(i) {
        Some(slot) => {
            *slot = KexecSlot {
                pages: 0,
                pages_bytes: 0,
                mem,
                bufsz,
                memsz,
            };
            l.nstaged += 1;
            Ok(i as u32)
        }
        None => Err(-EINVAL),
    }
}

/// Attach the adapter-allocated page chain to an already-reserved slot.
pub fn kx_stage_pages(
    l: &mut KexecLedger,
    index: u32,
    pages: u64,
    pages_bytes: u64,
) -> Result<(), i32> {
    if l.state != KX_STAGING || index >= l.nstaged {
        return Err(-EINVAL);
    }
    match l.seg.get_mut(index as usize) {
        Some(slot) => {
            slot.pages = pages;
            slot.pages_bytes = pages_bytes;
            Ok(())
        }
        None => Err(-EINVAL),
    }
}

/// Publish a fully-staged transaction as the loaded image. `dst` must be empty
/// (the caller releases the previous image first) and `src` must hold every
/// segment it promised; anything else is refused with both ledgers untouched.
pub fn kx_publish(dst: &mut KexecLedger, src: &mut KexecLedger) -> Result<(), i32> {
    if dst.state != KX_EMPTY || src.state != KX_STAGING || src.nstaged != src.nr {
        return Err(-EINVAL);
    }
    dst.state = KX_LOADED;
    dst.file_backed = src.file_backed;
    dst.nr = src.nr;
    dst.nstaged = src.nstaged;
    dst.entry = src.entry;
    let n = src.nstaged as usize;
    let mut i = 0;
    while i < n {
        let s = match src.seg.get(i) {
            Some(s) => *s,
            None => return Err(-EINVAL),
        };
        match dst.seg.get_mut(i) {
            Some(d) => *d = s,
            None => return Err(-EINVAL),
        }
        i += 1;
    }
    *src = KexecLedger::default();
    Ok(())
}

/// Hand back the newest still-held chain and clear its slot. Drives both the
/// rollback of a failed load and the release of a loaded/replaced image, so a
/// chain is released exactly once, newest first. `None` when nothing is held.
pub fn kx_release_next(l: &mut KexecLedger) -> Option<(u64, u64)> {
    if l.nstaged == 0 {
        return None;
    }
    let i = (l.nstaged - 1) as usize;
    let slot = match l.seg.get(i) {
        Some(s) => *s,
        None => return None,
    };
    if let Some(d) = l.seg.get_mut(i) {
        *d = KexecSlot::default();
    }
    l.nstaged -= 1;
    if l.nstaged == 0 {
        l.state = KX_EMPTY;
    }
    Some((slot.pages, slot.pages_bytes))
}

/// Whether an image is currently staged and published.
pub fn kx_loaded(l: &KexecLedger) -> bool {
    l.state == KX_LOADED
}

// ================================================================== FFI

/// Validate a header from raw bytes. Returns 0 or a negative errno.
#[no_mangle]
pub extern "C" fn a20_elf_validate_header(
    buf: *const u8,
    len: usize,
    expect_class: u8,
    expect_machine: u16,
) -> i32 {
    if buf.is_null() {
        return -EFAULT;
    }
    let bytes = unsafe { core::slice::from_raw_parts(buf, len) };
    match parse_ehdr(bytes) {
        None => -ENOEXEC,
        Some(eh) => match validate_ehdr(&eh, expect_class, expect_machine) {
            Ok(()) => 0,
            Err(e) => e,
        },
    }
}

/// Build the full image plan into `*out`.
#[no_mangle]
pub unsafe extern "C" fn a20_elf_plan_image(
    ehdr: *const u8,
    ehdr_len: usize,
    phdr: *const u8,
    phdr_len: usize,
    expect_class: u8,
    expect_machine: u16,
    wx_policy: i32,
    wx_xlator_host: bool,
    out: *mut ImagePlan,
) -> i32 {
    if ehdr.is_null() || out.is_null() {
        return -EFAULT;
    }
    let eh = core::slice::from_raw_parts(ehdr, ehdr_len);
    let ph = if phdr.is_null() {
        &[][..]
    } else {
        core::slice::from_raw_parts(phdr, phdr_len)
    };
    match plan_image(eh, ph, expect_class, expect_machine, wx_policy, wx_xlator_host) {
        Ok(p) => {
            /* Copy only the fields the adapter reads. The plan arrays are
             * large fixed-capacity buffers (64 segments), so blitting the
             * whole struct per exec is pure overhead: the adapter walks
             * `nseg` entries and the TLS/INTERP/DYNAMIC singletons. */
            (*out).ehdr = p.ehdr;
            (*out).is_dyn = p.is_dyn;
            (*out).is_native_abi = p.is_native_abi;
            (*out).nseg = p.nseg;
            for i in 0..p.nseg as usize {
                match p.segments.get(i) {
                    Some(src) => (*out).segments[i] = *src,
                    None => break,
                }
            }
            (*out).tls = p.tls;
            (*out).interp = p.interp;
            (*out).dynamic = p.dynamic;
            0
        }
        Err(e) => e,
    }
}

type MapElfSegment = unsafe extern "C" fn(
    *mut core::ffi::c_void, *const SegmentPlan, u64,
) -> i32;

/// Execute the validated PT_LOAD plan in program-header order. The C bridge
/// supplies only the page/frame and file-copy mechanism for one segment.
#[no_mangle]
pub unsafe extern "C" fn a20_elf_map_planned_segments(
    plan: *const ImagePlan,
    load_bias: u64,
    context: *mut core::ffi::c_void,
    map_segment: Option<MapElfSegment>,
) -> i32 {
    if plan.is_null() || map_segment.is_none() {
        return -EFAULT;
    }
    let plan = &*plan;
    let count = plan.nseg as usize;
    if count > MAX_PHDRS {
        return -ENOEXEC;
    }
    let map_segment = map_segment.unwrap();
    for segment in plan.segments.iter().take(count) {
        let rc = map_segment(context, segment, load_bias);
        if rc < 0 {
            return rc;
        }
    }
    0
}

/// Return the page-rounded virtual span shared by image allocation and
/// relocation bounds. PT_LOAD arithmetic is checked again at this FFI edge.
#[no_mangle]
pub unsafe extern "C" fn a20_elf_image_span(
    plan: *const ImagePlan,
    min_out: *mut u64,
    max_out: *mut u64,
) -> i32 {
    if plan.is_null() || min_out.is_null() || max_out.is_null() {
        return -EFAULT;
    }
    let plan = &*plan;
    let count = plan.nseg as usize;
    if count == 0 || count > MAX_PHDRS {
        return -ENOEXEC;
    }
    let mut min_va = u64::MAX;
    let mut max_va = 0u64;
    for segment in plan.segments.iter().take(count) {
        let end = match segment.va.checked_add(segment.memsz) {
            Some(value) => value,
            None => return -ENOEXEC,
        };
        let start_page = segment.va & !0xfffu64;
        let end_page = match end.checked_add(0xfff) {
            Some(value) => value & !0xfffu64,
            None => return -ENOEXEC,
        };
        min_va = min_va.min(start_page);
        max_va = max_va.max(end_page);
    }
    *min_out = min_va;
    *max_out = max_va;
    0
}

/// Plan the initial Linux stack into `*out`.
#[no_mangle]
pub unsafe extern "C" fn a20_elf_plan_stack(
    stack_top: u64,
    ptr_size: usize,
    argv_ptrs: *const *const u8,
    argv_lens: *const usize,
    argc: usize,
    envp_ptrs: *const *const u8,
    envp_lens: *const usize,
    envc: usize,
    platform: *const u8,
    platform_len: usize,
    execfn: *const u8,
    execfn_len: usize,
    aux: *const AuxvInput,
    out: *mut StackPlan,
) -> i32 {
    if out.is_null() || aux.is_null() {
        return -EFAULT;
    }
    let mut argv_buf: [&[u8]; MAX_ARG_STRINGS] = [&[]; MAX_ARG_STRINGS];
    let mut envp_buf: [&[u8]; MAX_ARG_STRINGS] = [&[]; MAX_ARG_STRINGS];
    for i in 0..argc.min(MAX_ARG_STRINGS) {
        let p = *argv_ptrs.add(i);
        let l = *argv_lens.add(i);
        argv_buf[i] = if p.is_null() {
            &[]
        } else {
            core::slice::from_raw_parts(p, l)
        };
    }
    for i in 0..envc.min(MAX_ARG_STRINGS) {
        let p = *envp_ptrs.add(i);
        let l = *envp_lens.add(i);
        envp_buf[i] = if p.is_null() {
            &[]
        } else {
            core::slice::from_raw_parts(p, l)
        };
    }
    let platform_bytes = if platform.is_null() {
        &[][..]
    } else {
        core::slice::from_raw_parts(platform, platform_len)
    };
    let execfn_bytes = if execfn.is_null() || execfn_len == 0 {
        &[][..]
    } else {
        core::slice::from_raw_parts(execfn, execfn_len)
    };
    let argv_n = argc.min(MAX_ARG_STRINGS);
    let envp_n = envc.min(MAX_ARG_STRINGS);
    let argv_slice = argv_buf.get(..argv_n).map_or(&[][..], |s| s);
    let envp_slice = envp_buf.get(..envp_n).map_or(&[][..], |s| s);
    match plan_stack(
        stack_top,
        ptr_size,
        argv_slice,
        envp_slice,
        platform_bytes,
        execfn_bytes,
        &*aux,
    ) {
        Ok(p) => {
            /* Copy only the entries the adapter consumes: it walks
             * env_count/arg_count/auxv_count, not the 256-entry arrays. */
            (*out).sp = p.sp;
            (*out).platform_va = p.platform_va;
            (*out).random_va = p.random_va;
            (*out).execfn_va = p.execfn_va;
            (*out).env_count = p.env_count;
            (*out).arg_count = p.arg_count;
            (*out).auxv_count = p.auxv_count;
            for i in 0..p.env_count as usize {
                match (p.env_addrs.get(i), (0..MAX_ARG_STRINGS).contains(&i)) {
                    (Some(v), true) => (*out).env_addrs[i] = *v,
                    _ => break,
                }
            }
            for i in 0..p.arg_count as usize {
                match (p.arg_addrs.get(i), (0..MAX_ARG_STRINGS).contains(&i)) {
                    (Some(v), true) => (*out).arg_addrs[i] = *v,
                    _ => break,
                }
            }
            for i in 0..p.auxv_count as usize {
                match (p.auxv.get(i), (0..AUXV_MAX_PAIRS).contains(&i)) {
                    (Some(v), true) => (*out).auxv[i] = *v,
                    _ => break,
                }
            }
            0
        }
        Err(e) => e,
    }
}

type StackCopy = unsafe extern "C" fn(*mut core::ffi::c_void, u64, *const u8, usize) -> i32;
type StackRandom = unsafe extern "C" fn(*mut core::ffi::c_void, *mut u8, usize) -> i32;

unsafe fn stack_input_slices<'a>(
    ptrs: *const *const u8,
    lens: *const usize,
    count: usize,
) -> Result<[&'a [u8]; MAX_ARG_STRINGS], i32> {
    if count > MAX_ARG_STRINGS || (count > 0 && (ptrs.is_null() || lens.is_null())) {
        return Err(-E2BIG);
    }
    let mut out = [&[][..]; MAX_ARG_STRINGS];
    for (i, slot) in out.iter_mut().enumerate().take(count) {
        let ptr = *ptrs.add(i);
        if ptr.is_null() {
            return Err(-EFAULT);
        }
        *slot = core::slice::from_raw_parts(ptr, *lens.add(i));
    }
    Ok(out)
}

/// Rust owns the conventional Linux stack write sequence. C supplies only
/// the mapped-image copy and random-byte mechanisms; the plan, vector order,
/// pointer-width narrowing, and auxv publication stay in this module.
#[no_mangle]
pub unsafe extern "C" fn a20_elf_write_linux_stack(
    stack_top: u64,
    ptr_size: usize,
    argv_ptrs: *const *const u8,
    argv_lens: *const usize,
    argc: usize,
    envp_ptrs: *const *const u8,
    envp_lens: *const usize,
    envc: usize,
    platform: *const u8,
    platform_len: usize,
    execfn: *const u8,
    execfn_len: usize,
    aux: *const AuxvInput,
    context: *mut core::ffi::c_void,
    copy: Option<StackCopy>,
    random: Option<StackRandom>,
    auxv_out: *mut usize,
    auxv_count_out: *mut u32,
    sp_out: *mut u64,
) -> i32 {
    if aux.is_null() || platform.is_null() || sp_out.is_null() ||
        copy.is_none() || random.is_none() || ptr_size != size_of::<usize>() ||
        (execfn_len != 0 && execfn.is_null()) {
        return -EFAULT;
    }
    let argv_buf = match stack_input_slices(argv_ptrs, argv_lens, argc) {
        Ok(v) => v,
        Err(e) => return e,
    };
    let envp_buf = match stack_input_slices(envp_ptrs, envp_lens, envc) {
        Ok(v) => v,
        Err(e) => return e,
    };
    let argv = &argv_buf[..argc];
    let envp = &envp_buf[..envc];
    let platform_bytes = core::slice::from_raw_parts(platform, platform_len);
    let execfn_bytes = if execfn_len == 0 {
        &[][..]
    } else {
        core::slice::from_raw_parts(execfn, execfn_len)
    };
    let plan = match plan_stack(stack_top, ptr_size, argv, envp, platform_bytes,
                                execfn_bytes, &*aux) {
        Ok(p) => p,
        Err(e) => return e,
    };
    let copy = copy.unwrap();
    for (bytes, addr) in envp.iter().zip(plan.env_addrs.iter()) {
        if copy(context, *addr, bytes.as_ptr(), bytes.len() + 1) < 0 {
            return -EFAULT;
        }
    }
    for (bytes, addr) in argv.iter().zip(plan.arg_addrs.iter()) {
        if copy(context, *addr, bytes.as_ptr(), bytes.len() + 1) < 0 {
            return -EFAULT;
        }
    }
    if copy(context, plan.platform_va, platform, platform_len + 1) < 0 {
        return -EFAULT;
    }
    if plan.execfn_va != 0 && execfn_len != 0 &&
        copy(context, plan.execfn_va, execfn, execfn_len + 1) < 0 {
        return -EFAULT;
    }
    let mut random_bytes = [0u8; 16];
    if random.unwrap()(context, random_bytes.as_mut_ptr(), random_bytes.len()) < 0 ||
        copy(context, plan.random_va, random_bytes.as_ptr(), random_bytes.len()) < 0 {
        return -EFAULT;
    }

    let ptr = size_of::<usize>();
    let mut argc_word = argc;
    let mut argv_words = [0usize; MAX_ARG_STRINGS + 1];
    let mut envp_words = [0usize; MAX_ARG_STRINGS + 1];
    for (slot, addr) in argv_words.iter_mut().zip(plan.arg_addrs.iter()).take(argc) {
        *slot = *addr as usize;
    }
    for (slot, addr) in envp_words.iter_mut().zip(plan.env_addrs.iter()).take(envc) {
        *slot = *addr as usize;
    }
    let argc_va = plan.sp;
    let argv_va = argc_va + ptr as u64;
    let envp_va = argv_va + (argc as u64 + 1) * ptr as u64;
    let auxv_va = envp_va + (envc as u64 + 1) * ptr as u64;
    if copy(context, argc_va, &mut argc_word as *mut usize as *const u8, ptr) < 0 ||
        copy(context, argv_va, argv_words.as_ptr().cast(), (argc + 1) * ptr) < 0 ||
        copy(context, envp_va, envp_words.as_ptr().cast(), (envc + 1) * ptr) < 0 {
        return -EFAULT;
    }
    let mut auxv_words = [0usize; AUXV_MAX_PAIRS * 2];
    for (i, pair) in plan.auxv.iter().enumerate().take(plan.auxv_count as usize) {
        let key = pair.key as usize;
        let val = pair.val as usize;
        let pair_start = 2 * i;
        if let Some(pair_slots) = auxv_words.get_mut(pair_start..pair_start + 2) {
            if let [key_slot, val_slot] = pair_slots {
                *key_slot = key;
                *val_slot = val;
            } else {
                return -E2BIG;
            }
            if !auxv_out.is_null() {
                *auxv_out.add(pair_start) = key;
                *auxv_out.add(pair_start + 1) = val;
            }
        } else {
            return -E2BIG;
        }
    }
    if !auxv_count_out.is_null() {
        *auxv_count_out = plan.auxv_count;
    }
    if copy(context, auxv_va, auxv_words.as_ptr().cast(),
            plan.auxv_count as usize * 2 * ptr) < 0 {
        return -EFAULT;
    }
    *sp_out = plan.sp;
    0
}

/// Rust-owned native-ABI initial-stack writer. The descriptor arrives with
/// ABI handles filled by the exec adapter; Rust derives its string pointers,
/// counts, descriptor placement, and pointer-array order from the plan.
#[no_mangle]
pub unsafe extern "C" fn a20_elf_write_native_stack(
    stack_top: u64,
    ptr_size: usize,
    argv_ptrs: *const *const u8,
    argv_lens: *const usize,
    argc: usize,
    envp_ptrs: *const *const u8,
    envp_lens: *const usize,
    envc: usize,
    start_info: *const u8,
    start_info_size: usize,
    context: *mut core::ffi::c_void,
    copy: Option<StackCopy>,
    sp_out: *mut u64,
) -> i32 {
    if start_info.is_null() || sp_out.is_null() || copy.is_none() ||
        ptr_size != size_of::<usize>() || start_info_size == 0 {
        return -EFAULT;
    }
    let argv_buf = match stack_input_slices(argv_ptrs, argv_lens, argc) {
        Ok(v) => v,
        Err(e) => return e,
    };
    let envp_buf = match stack_input_slices(envp_ptrs, envp_lens, envc) {
        Ok(v) => v,
        Err(e) => return e,
    };
    let plan = match plan_stack_a20(
        stack_top, ptr_size, &argv_buf[..argc], &envp_buf[..envc],
        start_info_size as u64,
    ) {
        Ok(p) => p,
        Err(e) => return e,
    };
    let copy = copy.unwrap();
    for (bytes, addr) in envp_buf.iter().take(envc).zip(plan.env_addrs.iter()) {
        if copy(context, *addr, bytes.as_ptr(), bytes.len() + 1) < 0 {
            return -EFAULT;
        }
    }
    for (bytes, addr) in argv_buf.iter().take(argc).zip(plan.arg_addrs.iter()) {
        if copy(context, *addr, bytes.as_ptr(), bytes.len() + 1) < 0 {
            return -EFAULT;
        }
    }
    let mut env_words = [0usize; MAX_ARG_STRINGS + 1];
    let mut arg_words = [0usize; MAX_ARG_STRINGS + 1];
    for (slot, addr) in env_words.iter_mut().zip(plan.env_addrs.iter()).take(envc) {
        *slot = *addr as usize;
    }
    for (slot, addr) in arg_words.iter_mut().zip(plan.arg_addrs.iter()).take(argc) {
        *slot = *addr as usize;
    }
    let ptr_bytes = ptr_size;
    if copy(context, plan.envp_va, env_words.as_ptr().cast(), (envc + 1) * ptr_bytes) < 0 ||
        copy(context, plan.argv_va, arg_words.as_ptr().cast(), (argc + 1) * ptr_bytes) < 0 {
        return -EFAULT;
    }
    let mut descriptor = [0u64; 32];
    if start_info_size > 32 * size_of::<u64>() || start_info_size < 48 {
        return -E2BIG;
    }
    let descriptor_bytes = descriptor.as_mut_ptr().cast::<u8>();
    core::ptr::copy_nonoverlapping(start_info, descriptor_bytes, start_info_size);
    if start_info_size >= 48 {
        let words = descriptor_bytes.cast::<u32>();
        *words.add(0) = start_info_size as u32;
        *words.add(2) = argc as u32;
        *words.add(3) = envc as u32;
        let addresses = descriptor_bytes.add(24).cast::<u64>();
        *addresses.add(0) = plan.argv_va;
        *addresses.add(1) = plan.envp_va;
        *addresses.add(2) = 0;
    }
    if copy(context, plan.start_info_va, descriptor_bytes, start_info_size) < 0 {
        return -EFAULT;
    }
    *sp_out = plan.sp;
    0
}

/// Compute the credential transition into `*out`.
#[no_mangle]
pub unsafe extern "C" fn a20_elf_plan_creds(input: *const CredInput, out: *mut CredPlan) -> i32 {
    if input.is_null() || out.is_null() {
        return -EFAULT;
    }
    core::ptr::write(out, plan_exec_creds(&*input));
    0
}

/// Scan a `.dynamic` array into `*out`.
#[no_mangle]
pub unsafe extern "C" fn a20_elf_scan_dynamic(
    dyn_bytes: *const u8,
    len: usize,
    elf64: bool,
    out: *mut DynInfo,
) -> i32 {
    if out.is_null() {
        return -EFAULT;
    }
    let bytes = if dyn_bytes.is_null() {
        &[][..]
    } else {
        core::slice::from_raw_parts(dyn_bytes, len)
    };
    match scan_dynamic(bytes, elf64) {
        Ok(d) => {
            core::ptr::write(out, d);
            0
        }
        Err(e) => e,
    }
}

/// Probe a file for a foreign architecture, writing `*out` on a hit.
#[no_mangle]
pub unsafe extern "C" fn a20_elf_probe_foreign(
    ehdr: *const u8,
    ehdr_len: usize,
    phdr: *const u8,
    phdr_len: usize,
    expect_class: u8,
    expect_machine: u16,
    out: *mut ForeignKey,
) -> i32 {
    if ehdr.is_null() || out.is_null() {
        return -EFAULT;
    }
    let eh = core::slice::from_raw_parts(ehdr, ehdr_len);
    let ph = if phdr.is_null() {
        &[][..]
    } else {
        core::slice::from_raw_parts(phdr, phdr_len)
    };
    match probe_foreign(eh, ph, expect_class, expect_machine) {
        Ok(k) => {
            core::ptr::write(out, k);
            0
        }
        Err(e) => e,
    }
}

/// Parse just the ELF identification and fixed header fields. The adapter
/// uses this to learn the program-header table location before reading it.
#[no_mangle]
pub unsafe extern "C" fn a20_elf_parse_header(
    buf: *const u8,
    len: usize,
    out: *mut Ehdr,
) -> i32 {
    if buf.is_null() || out.is_null() {
        return -EFAULT;
    }
    match parse_ehdr(core::slice::from_raw_parts(buf, len)) {
        Some(eh) => {
            core::ptr::write(out, eh);
            0
        }
        None => -ENOEXEC,
    }
}

/// Load-bias decision: `ET_DYN` slides by `dyn_base + aslr`, `ET_EXEC` keeps
/// its link-time addresses.
#[no_mangle]
pub extern "C" fn a20_elf_plan_load_bias(is_dyn: u8, dyn_base: u64, aslr_bias: u64) -> u64 {
    plan_load_bias(is_dyn != 0, dyn_base, aslr_bias)
}

/// Post-loop image layout derived from the plan and the bias.
#[no_mangle]
pub unsafe extern "C" fn a20_elf_finish_layout(
    plan: *const ImagePlan,
    load_bias: u64,
    out: *mut Layout,
) -> i32 {
    if plan.is_null() || out.is_null() {
        return -EFAULT;
    }
    core::ptr::write(out, finish_layout(&*plan, load_bias));
    0
}

/// Relocation action verdicts returned by `a20_elf_reloc_action`.
pub const RELOC_ACT_SKIP: i32 = 0;
pub const RELOC_ACT_RELATIVE: i32 = 1;
pub const RELOC_ACT_ABSOLUTE: i32 = 2;

/// The RELATIVE/ABSOLUTE relocation type pair for a machine, `u32::MAX` when
/// a width has none. Writes two u32s through `out`.
#[no_mangle]
pub unsafe extern "C" fn a20_elf_reloc_types(
    machine: u16,
    elf64: bool,
    out: *mut u32,
) -> i32 {
    if out.is_null() {
        return -EFAULT;
    }
    let (rel, abs) = reloc_types(machine, elf64);
    *out.add(0) = rel;
    *out.add(1) = abs;
    0
}

/// Classify one relocation entry; enforces the symbol bound and the
/// weak/undefined policy the C loader used to hold inline.
#[allow(clippy::too_many_arguments)]
#[no_mangle]
pub extern "C" fn a20_elf_reloc_action(
    rel_type: u32,
    symidx: u32,
    nsyms: u64,
    st_info: u8,
    st_shndx: u16,
    want_relative: u32,
    want_absolute: u32,
) -> i32 {
    match reloc_action(
        rel_type,
        symidx,
        nsyms,
        st_info,
        st_shndx,
        want_relative,
        want_absolute,
    ) {
        Ok(RelocAction::Skip) => RELOC_ACT_SKIP,
        Ok(RelocAction::Relative) => RELOC_ACT_RELATIVE,
        Ok(RelocAction::Absolute) => RELOC_ACT_ABSOLUTE,
        Err(e) => e,
    }
}

/// Reconstruct the .dynsym count from whichever hash table the image
/// carries. `sysv`/`gnu` may be NULL; the lengths cap how far the walk reads.
///
/// This is a primitive, not a production entry point: the caller must supply a
/// length that is already known to be readable. The loader's own relocation
/// pass (`a20_elf_apply_relocs`) derives that length from the `PT_LOAD` map and
/// never calls this with an unvetted range.
#[no_mangle]
pub unsafe extern "C" fn a20_elf_dynsym_count(
    sysv: *const u8,
    sysv_len: usize,
    gnu: *const u8,
    gnu_len: usize,
) -> u64 {
    let sysv_slice = if sysv.is_null() {
        None
    } else {
        Some(core::slice::from_raw_parts(sysv, sysv_len))
    };
    let gnu_slice = if gnu.is_null() {
        None
    } else {
        Some(core::slice::from_raw_parts(gnu, gnu_len))
    };
    dynsym_count(sysv_slice, gnu_slice)
}

/// Apply the supported relocations of a loaded image. `plan` is the image plan
/// the adapter executed, `image`/`image_len` the flat loaded-image buffer the
/// adapter mapped (NOMMU), `load_bias` the bias that turns plan addresses into
/// the image's addresses. Returns the number of stored entries, or a negative
/// errno; a failed call may have written some entries, and the caller discards
/// the image in that case.
#[no_mangle]
pub unsafe extern "C" fn a20_elf_apply_relocs(
    plan: *const ImagePlan,
    load_bias: u64,
    image: *mut u8,
    image_len: usize,
    elf64: bool,
) -> i64 {
    if plan.is_null() || image.is_null() {
        return -EFAULT as i64;
    }
    let bytes = core::slice::from_raw_parts_mut(image, image_len);
    match apply_relocs(&*plan, load_bias, bytes, elf64) {
        Ok(n) => n as i64,
        Err(e) => e as i64,
    }
}

/// Parse a `#!` first line into `*out` (`is_script=0` for a non-script).
#[no_mangle]
pub unsafe extern "C" fn a20_elf_plan_shebang(
    line: *const u8,
    len: usize,
    out: *mut ShebangPlan,
) -> i32 {
    if line.is_null() || out.is_null() {
        return -EFAULT;
    }
    match plan_shebang(core::slice::from_raw_parts(line, len)) {
        Ok(p) => {
            core::ptr::write(out, p);
            0
        }
        Err(e) => e,
    }
}

/// Plan the shebang argv rewrite (`[interp, (arg), script, argv[1..]]`).
#[no_mangle]
pub extern "C" fn a20_elf_plan_shebang_argv(
    has_arg: bool,
    orig_argc: usize,
    max_args: usize,
    out: *mut ShebangArgvPlan,
) -> i32 {
    if out.is_null() {
        return -EFAULT;
    }
    unsafe { core::ptr::write(out, plan_shebang_argv(has_arg, orig_argc, max_args)) };
    0
}

/// argv/envp byte-budget decision. Returns the new running total, or a
/// negative errno (as i64 so the total never aliases one).
#[no_mangle]
pub extern "C" fn a20_elf_check_arg_bytes(
    total: u64,
    len: u64,
    max_strlen: u64,
    max_bytes: u64,
    stack_quarter: u64,
) -> i64 {
    let limits = BprmLimits {
        max_strlen,
        max_bytes,
        stack_quarter,
    };
    match check_arg_bytes(total, len, &limits) {
        Ok(next) => next as i64,
        Err(e) => e as i64,
    }
}

/// Format-resolution classification of an `elf_load()` return code:
/// 0 = install, 1 = format mismatch (try shebang/xlator), 2 = real failure
/// (the original rc is the errno).
#[no_mangle]
pub extern "C" fn a20_elf_classify(rc: i32) -> i32 {
    match classify_elf(rc) {
        ElfOutcome::Loaded => STEP_INSTALL,
        ElfOutcome::FormatMismatch => 1,
        ElfOutcome::Failed(_) => STEP_FAIL,
    }
}

/// `exec_try_script()`/`exec_try_translator()` result classification:
/// `STEP_RETRY` rewrites and retries, `STEP_CONTINUE` means "not mine",
/// `STEP_FAIL` keeps rc as the errno.
#[no_mangle]
pub extern "C" fn a20_elf_step_after_script(rc: i32) -> i32 {
    step_after_script(rc).as_ffi()
}

#[no_mangle]
pub extern "C" fn a20_elf_step_after_xlator(enabled: bool, rc: i32) -> i32 {
    step_after_xlator(enabled, rc).as_ffi()
}

/// Interpreter ladder facts for the C opener.
#[no_mangle]
pub extern "C" fn a20_elf_interp_attempts() -> u32 {
    INTERP_ATTEMPTS as u32
}

#[no_mangle]
pub extern "C" fn a20_elf_interp_resolution(index: u32, opened: bool) -> i32 {
    interp_resolution(index as usize, opened)
}

/// Plan the native-ABI initial stack into `*out`.
#[no_mangle]
pub unsafe extern "C" fn a20_elf_plan_stack_a20(
    stack_top: u64,
    ptr_size: usize,
    argv_ptrs: *const *const u8,
    argv_lens: *const usize,
    argc: usize,
    envp_ptrs: *const *const u8,
    envp_lens: *const usize,
    envc: usize,
    start_info_size: u64,
    out: *mut StartInfoPlan,
) -> i32 {
    if out.is_null() {
        return -EFAULT;
    }
    let mut argv_buf: [&[u8]; MAX_ARG_STRINGS] = [&[]; MAX_ARG_STRINGS];
    let mut envp_buf: [&[u8]; MAX_ARG_STRINGS] = [&[]; MAX_ARG_STRINGS];
    for i in 0..argc.min(MAX_ARG_STRINGS) {
        let p = *argv_ptrs.add(i);
        let l = *argv_lens.add(i);
        argv_buf[i] = if p.is_null() {
            &[]
        } else {
            core::slice::from_raw_parts(p, l)
        };
    }
    for i in 0..envc.min(MAX_ARG_STRINGS) {
        let p = *envp_ptrs.add(i);
        let l = *envp_lens.add(i);
        envp_buf[i] = if p.is_null() {
            &[]
        } else {
            core::slice::from_raw_parts(p, l)
        };
    }
    let argv_n = argc.min(MAX_ARG_STRINGS);
    let envp_n = envc.min(MAX_ARG_STRINGS);
    let argv_slice = argv_buf.get(..argv_n).map_or(&[][..], |s| s);
    let envp_slice = envp_buf.get(..envp_n).map_or(&[][..], |s| s);
    match plan_stack_a20(
        stack_top,
        ptr_size,
        argv_slice,
        envp_slice,
        start_info_size,
    ) {
        Ok(p) => {
            core::ptr::write(out, p);
            0
        }
        Err(e) => e,
    }
}

/// Reserved-stack-top start-info address for a dynamic-native image; also the
/// stack top the conventional planner builds under.
#[no_mangle]
pub extern "C" fn a20_elf_plan_stack_a20_dynamic(stack_top: u64, start_info_size: u64) -> u64 {
    plan_stack_a20_dynamic(stack_top, start_info_size)
}

/// kexec ledger FFI. The Rust transaction creates and owns its private ledger,
/// validates/stages every segment, atomically swaps a complete replacement,
/// and drains discarded ledgers through narrow C frame-free callbacks.
#[no_mangle]
pub unsafe extern "C" fn a20_elf_kexec_init(l: *mut KexecLedger) -> i32 {
    if l.is_null() {
        return -EINVAL;
    }
    core::ptr::write(l, kx_init());
    0
}

#[no_mangle]
pub unsafe extern "C" fn a20_elf_kexec_stage_open(
    l: *mut KexecLedger,
    entry: u64,
    nr_segments: u64,
    max_segments: u64,
    file_backed: bool,
) -> i32 {
    if l.is_null() {
        return -EINVAL;
    }
    match kx_stage_open(&mut *l, entry, nr_segments, max_segments, file_backed) {
        Ok(()) => 0,
        Err(e) => e,
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_elf_kexec_stage_check(
    l: *mut KexecLedger,
    buf: u64,
    bufsz: u64,
    mem: u64,
    memsz: u64,
    kstart: u64,
    kend: u64,
    out_index: *mut u32,
) -> i32 {
    if l.is_null() || out_index.is_null() {
        return -EINVAL;
    }
    match kx_stage_check(&mut *l, buf, bufsz, mem, memsz, kstart, kend) {
        Ok(i) => {
            *out_index = i;
            0
        }
        Err(e) => e,
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_elf_kexec_stage_file(
    l: *mut KexecLedger,
    size: u64,
    out_index: *mut u32,
) -> i32 {
    if l.is_null() || out_index.is_null() {
        return -EINVAL;
    }
    match kx_stage_file(&mut *l, size) {
        Ok(i) => {
            *out_index = i;
            0
        }
        Err(e) => e,
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_elf_kexec_stage_pages(
    l: *mut KexecLedger,
    index: u32,
    pages: u64,
    pages_bytes: u64,
) -> i32 {
    if l.is_null() {
        return -EINVAL;
    }
    match kx_stage_pages(&mut *l, index, pages, pages_bytes) {
        Ok(()) => 0,
        Err(e) => e,
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_elf_kexec_publish(
    dst: *mut KexecLedger,
    src: *mut KexecLedger,
) -> i32 {
    if dst.is_null() || src.is_null() {
        return -EINVAL;
    }
    match kx_publish(&mut *dst, &mut *src) {
        Ok(()) => 0,
        Err(e) => e,
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_elf_kexec_release_next(
    l: *mut KexecLedger,
    pages_out: *mut u64,
    bytes_out: *mut u64,
) -> bool {
    if l.is_null() {
        return false;
    }
    match kx_release_next(&mut *l) {
        Some((pages, bytes)) => {
            if !pages_out.is_null() {
                *pages_out = pages;
            }
            if !bytes_out.is_null() {
                *bytes_out = bytes;
            }
            true
        }
        None => false,
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_elf_kexec_loaded(l: *const KexecLedger) -> bool {
    !l.is_null() && kx_loaded(&*l)
}

#[repr(C)]
pub struct KexecLoadOps {
    pub context: *mut core::ffi::c_void,
    pub read_segment: Option<unsafe extern "C" fn(*mut core::ffi::c_void, u32, *mut KexecSegmentWire) -> i32>,
    pub stage_segment: Option<unsafe extern "C" fn(*mut core::ffi::c_void, *const KexecSegmentWire, *mut u64, *mut u64) -> i32>,
    pub lock: Option<unsafe extern "C" fn(*mut core::ffi::c_void) -> u64>,
    pub unlock: Option<unsafe extern "C" fn(*mut core::ffi::c_void, u64)>,
    pub free_pages: Option<unsafe extern "C" fn(*mut core::ffi::c_void, u64)>,
}

#[repr(C)]
pub struct KexecFileOps {
    pub context: *mut core::ffi::c_void,
    pub read_file: Option<unsafe extern "C" fn(*mut core::ffi::c_void, *mut *const u8, *mut u64) -> i32>,
    pub stage_file: Option<unsafe extern "C" fn(*mut core::ffi::c_void, *const u8, u64, *mut u64, *mut u64) -> i32>,
    pub free_buffer: Option<unsafe extern "C" fn(*mut core::ffi::c_void, *const u8)>,
    pub lock: Option<unsafe extern "C" fn(*mut core::ffi::c_void) -> u64>,
    pub unlock: Option<unsafe extern "C" fn(*mut core::ffi::c_void, u64)>,
    pub free_pages: Option<unsafe extern "C" fn(*mut core::ffi::c_void, u64)>,
}

unsafe fn kx_drain_with(l: &mut KexecLedger, free_pages: unsafe extern "C" fn(*mut core::ffi::c_void, u64), context: *mut core::ffi::c_void) {
    while let Some((pages, _)) = kx_release_next(l) {
        if pages != 0 { free_pages(context, pages); }
    }
}

/// Own segment validation, private staging, atomic replacement, and rollback.
/// C callbacks only fetch user bytes, allocate/copy frame chains, synchronize,
/// and free an opaque chain.
#[no_mangle]
pub unsafe extern "C" fn a20_elf_kexec_load_transaction(
    dst: *mut KexecLedger, entry: u64, nr: u64, kstart: u64, kend: u64,
    ops: *const KexecLoadOps,
) -> i32 {
    if dst.is_null() || ops.is_null() { return -EFAULT; }
    let o = &*ops;
    let (Some(read_segment), Some(stage_segment), Some(lock), Some(unlock), Some(free_pages)) =
        (o.read_segment, o.stage_segment, o.lock, o.unlock, o.free_pages) else { return -EINVAL; };
    let mut staged = kx_init();
    if let Err(e) = kx_stage_open(&mut staged, entry, nr, KEXEC_MAX_SEGMENTS as u64, false) { return e; }
    let mut i = 0;
    while i < nr {
        let mut wire = KexecSegmentWire::default();
        let mut rc = read_segment(o.context, i as u32, &mut wire);
        let mut index = 0;
        if rc == 0 {
            match kx_stage_check(&mut staged, wire.buf, wire.bufsz, wire.mem, wire.memsz, kstart, kend) {
                Ok(v) => index = v,
                Err(e) => rc = e,
            }
        }
        let mut pages = 0u64;
        let mut pages_bytes = 0u64;
        if rc == 0 { rc = stage_segment(o.context, &wire, &mut pages, &mut pages_bytes); }
        if rc == 0 {
            if let Err(e) = kx_stage_pages(&mut staged, index, pages, pages_bytes) { rc = e; }
        }
        if rc != 0 {
            if pages != 0 { free_pages(o.context, pages); }
            kx_drain_with(&mut staged, free_pages, o.context);
            return rc;
        }
        i += 1;
    }
    let mut replacement = kx_init();
    if let Err(e) = kx_publish(&mut replacement, &mut staged) {
        kx_drain_with(&mut staged, free_pages, o.context);
        return e;
    }
    let flags = lock(o.context);
    core::ptr::swap(dst, &mut replacement);
    unlock(o.context, flags);
    kx_drain_with(&mut replacement, free_pages, o.context);
    0
}

#[no_mangle]
pub unsafe extern "C" fn a20_elf_kexec_discard_transaction(
    dst: *mut KexecLedger, ops: *const KexecLoadOps,
) -> i32 {
    if dst.is_null() || ops.is_null() { return -EFAULT; }
    let o = &*ops;
    let (Some(lock), Some(unlock), Some(free_pages)) = (o.lock, o.unlock, o.free_pages) else { return -EINVAL; };
    let mut empty = kx_init();
    let flags = lock(o.context);
    core::ptr::swap(dst, &mut empty);
    unlock(o.context, flags);
    kx_drain_with(&mut empty, free_pages, o.context);
    0
}

#[no_mangle]
pub unsafe extern "C" fn a20_elf_kexec_file_transaction(
    dst: *mut KexecLedger, ops: *const KexecFileOps,
) -> i32 {
    if dst.is_null() || ops.is_null() { return -EFAULT; }
    let o = &*ops;
    let (Some(read_file), Some(stage_file), Some(free_buffer), Some(lock), Some(unlock), Some(free_pages)) =
        (o.read_file, o.stage_file, o.free_buffer, o.lock, o.unlock, o.free_pages) else { return -EINVAL; };
    let mut buffer: *const u8 = core::ptr::null();
    let mut size = 0u64;
    let mut rc = read_file(o.context, &mut buffer, &mut size);
    if rc == 0 && (buffer.is_null() || size < 4 || size > 64 * 1024 * 1024) { rc = -EINVAL; }
    if rc == 0 && !is_elf_image(core::slice::from_raw_parts(buffer, 4)) { rc = -ENOEXEC; }
    let mut staged = kx_init();
    if rc == 0 { rc = kx_stage_open(&mut staged, 0, 1, KEXEC_MAX_SEGMENTS as u64, true).err().unwrap_or(0); }
    let mut index = 0;
    if rc == 0 { match kx_stage_file(&mut staged, size) { Ok(v) => index = v, Err(e) => rc = e } }
    let mut pages = 0u64;
    let mut pages_bytes = 0u64;
    if rc == 0 { rc = stage_file(o.context, buffer, size, &mut pages, &mut pages_bytes); }
    if !buffer.is_null() { free_buffer(o.context, buffer); }
    if rc == 0 { if let Err(e) = kx_stage_pages(&mut staged, index, pages, pages_bytes) { rc = e; } }
    if rc != 0 {
        if pages != 0 { free_pages(o.context, pages); }
        kx_drain_with(&mut staged, free_pages, o.context);
        return rc;
    }
    let mut replacement = kx_init();
    if let Err(e) = kx_publish(&mut replacement, &mut staged) {
        kx_drain_with(&mut staged, free_pages, o.context);
        return e;
    }
    let flags = lock(o.context);
    core::ptr::swap(dst, &mut replacement);
    unlock(o.context, flags);
    kx_drain_with(&mut replacement, free_pages, o.context);
    0
}

/// The kexec_file_load four-byte ELF signature gate.
#[no_mangle]
pub unsafe extern "C" fn a20_elf_is_elf_image(buf: *const u8, len: usize) -> bool {
    !buf.is_null() && is_elf_image(core::slice::from_raw_parts(buf, len))
}

/// Exec-transaction FFI. The C executor allocates an `a20_elf_exec_txn_t` on
/// its stack, walks the phases with `a20_elf_txn_begin`, registers each
/// resource it acquires through `a20_elf_txn_acquire`, and either commits (all
/// phases ran) or walks `a20_elf_txn_undo_*` and releases what is still live.
#[no_mangle]
pub unsafe extern "C" fn a20_elf_txn_init(txn: *mut ExecTxn) -> i32 {
    if txn.is_null() {
        return -EFAULT;
    }
    core::ptr::write(txn, txn_init());
    0
}

#[no_mangle]
pub unsafe extern "C" fn a20_elf_txn_begin(txn: *mut ExecTxn) -> i32 {
    if txn.is_null() {
        return -EFAULT;
    }
    txn_begin(&mut *txn)
}

#[no_mangle]
pub unsafe extern "C" fn a20_elf_txn_acquire(txn: *mut ExecTxn, kind: u32, handle: u64) -> i32 {
    if txn.is_null() {
        return -EFAULT;
    }
    txn_acquire(&mut *txn, kind, handle)
}

#[no_mangle]
pub unsafe extern "C" fn a20_elf_txn_transfer(txn: *mut ExecTxn, kind: u32, handle: u64) -> i32 {
    if txn.is_null() {
        return -EFAULT;
    }
    txn_transfer(&mut *txn, kind, handle)
}

#[no_mangle]
pub unsafe extern "C" fn a20_elf_txn_commit(txn: *mut ExecTxn) -> i32 {
    if txn.is_null() {
        return -EFAULT;
    }
    txn_commit(&mut *txn)
}

#[no_mangle]
pub unsafe extern "C" fn a20_elf_txn_abort(txn: *mut ExecTxn) -> i32 {
    if txn.is_null() {
        return -EFAULT;
    }
    txn_abort(&mut *txn)
}

#[no_mangle]
pub unsafe extern "C" fn a20_elf_txn_undo_next(
    txn: *mut ExecTxn,
    kind_out: *mut u32,
    handle_out: *mut u64,
) -> bool {
    if txn.is_null() || kind_out.is_null() || handle_out.is_null() {
        return false;
    }
    match txn_undo_next(&mut *txn) {
        Some((kind, handle)) => {
            *kind_out = kind;
            *handle_out = handle;
            true
        }
        None => false,
    }
}

/// Whether `phase` is one the exec sequence can really fail at. The test
/// profile's fault injection refuses to arm anywhere else.
#[no_mangle]
pub extern "C" fn a20_elf_phase_failable(phase: u32) -> bool {
    let after = (phase & 0x100) != 0;
    match phase_from_index(phase & !0x100) {
        Some(p) if after => matches!(p,
            CommitPhase::StackBuild | CommitPhase::VdsoMap | CommitPhase::AllocNewMm),
        Some(p) => phase_is_failable(p),
        None => false,
    }
}

type ExecAction = unsafe extern "C" fn(*mut core::ffi::c_void) -> i32;
type ExecVoidAction = unsafe extern "C" fn(*mut core::ffi::c_void);
type ExecGetAbi = unsafe extern "C" fn(*mut core::ffi::c_void) -> i32;
type ExecSetAbi = unsafe extern "C" fn(*mut core::ffi::c_void, i32);
type ExecAllocMm = unsafe extern "C" fn(*mut core::ffi::c_void, *mut u64) -> i32;
type ExecUndo = unsafe extern "C" fn(*mut core::ffi::c_void, u32, u64);
type ExecInject = unsafe extern "C" fn(*mut core::ffi::c_void, u32, bool) -> bool;

#[repr(C)]
pub struct ExecOpsFfi {
    pub build_linux_stack: Option<ExecAction>,
    pub build_native_stack: Option<ExecAction>,
    pub map_vdso: Option<ExecAction>,
    pub get_abi_mode: Option<ExecGetAbi>,
    pub set_abi_mode: Option<ExecSetAbi>,
    pub allocate_new_mm: Option<ExecAllocMm>,
    pub publish_auxv: Option<ExecVoidAction>,
    pub unshare_fdtable: Option<ExecAction>,
    pub terminate_siblings: Option<ExecVoidAction>,
    pub close_on_exec: Option<ExecVoidAction>,
    pub install_trap_context: Option<ExecVoidAction>,
    pub apply_credentials: Option<ExecVoidAction>,
    pub enter_new_mm: Option<ExecVoidAction>,
    pub undo_resource: Option<ExecUndo>,
    pub inject_failure: Option<ExecInject>,
}

struct ExecFfiAdapter {
    context: *mut core::ffi::c_void,
    ops: *const ExecOpsFfi,
}

impl ExecOps for ExecFfiAdapter {
    fn build_linux_stack(&mut self) -> i32 {
        unsafe { match (*self.ops).build_linux_stack { Some(f) => f(self.context), None => -EFAULT } }
    }
    fn build_native_stack(&mut self) -> i32 {
        unsafe { match (*self.ops).build_native_stack { Some(f) => f(self.context), None => -EFAULT } }
    }
    fn map_vdso(&mut self) -> i32 {
        unsafe { match (*self.ops).map_vdso { Some(f) => f(self.context), None => -EFAULT } }
    }
    fn get_abi_mode(&mut self) -> i32 {
        unsafe { match (*self.ops).get_abi_mode { Some(f) => f(self.context), None => -EFAULT } }
    }
    fn set_abi_mode(&mut self, mode: i32) {
        unsafe { if let Some(f) = (*self.ops).set_abi_mode { f(self.context, mode); } }
    }
    fn allocate_new_mm(&mut self) -> Result<u64, i32> {
        let mut handle = 0;
        let rc = unsafe {
            match (*self.ops).allocate_new_mm {
                Some(f) => f(self.context, &mut handle),
                None => -EFAULT,
            }
        };
        if rc == 0 { Ok(handle) } else { Err(rc) }
    }
    fn publish_auxv(&mut self) {
        unsafe { if let Some(f) = (*self.ops).publish_auxv { f(self.context); } }
    }
    fn unshare_fdtable(&mut self) -> i32 {
        unsafe { match (*self.ops).unshare_fdtable { Some(f) => f(self.context), None => -EFAULT } }
    }
    fn terminate_siblings(&mut self) {
        unsafe { if let Some(f) = (*self.ops).terminate_siblings { f(self.context); } }
    }
    fn close_on_exec(&mut self) {
        unsafe { if let Some(f) = (*self.ops).close_on_exec { f(self.context); } }
    }
    fn install_trap_context(&mut self) {
        unsafe { if let Some(f) = (*self.ops).install_trap_context { f(self.context); } }
    }
    fn apply_credentials(&mut self) {
        unsafe { if let Some(f) = (*self.ops).apply_credentials { f(self.context); } }
    }
    fn enter_new_mm(&mut self) {
        unsafe { if let Some(f) = (*self.ops).enter_new_mm { f(self.context); } }
    }
    fn undo_resource(&mut self, kind: u32, handle: u64) {
        unsafe { if let Some(f) = (*self.ops).undo_resource { f(self.context, kind, handle); } }
    }
    fn inject_failure(&mut self, phase: CommitPhase, after: bool) -> bool {
        unsafe {
            match (*self.ops).inject_failure {
                Some(f) => f(self.context, phase as u32, after),
                None => false,
            }
        }
    }
}

/// Kernel entry: Rust drives every exec phase and resource transition. The
/// callback table contains only task/mm/FD/architecture mechanisms.
#[no_mangle]
pub unsafe extern "C" fn a20_elf_exec_run(
    image: u64,
    native_abi: bool,
    context: *mut core::ffi::c_void,
    ops: *const ExecOpsFfi,
) -> i32 {
    if context.is_null() || ops.is_null() {
        return -EFAULT;
    }
    let required = [
        (*ops).build_linux_stack.is_some(),
        (*ops).build_native_stack.is_some(),
        (*ops).map_vdso.is_some(),
        (*ops).get_abi_mode.is_some(),
        (*ops).set_abi_mode.is_some(),
        (*ops).allocate_new_mm.is_some(),
        (*ops).publish_auxv.is_some(),
        (*ops).unshare_fdtable.is_some(),
        (*ops).terminate_siblings.is_some(),
        (*ops).close_on_exec.is_some(),
        (*ops).install_trap_context.is_some(),
        (*ops).apply_credentials.is_some(),
        (*ops).enter_new_mm.is_some(),
        (*ops).undo_resource.is_some(),
    ];
    if required.iter().any(|present| !present) {
        return -EFAULT;
    }
    let mut txn = txn_init();
    let mut adapter = ExecFfiAdapter { context, ops };
    run_exec_transaction(&mut txn, image, native_abi, &mut adapter)
}

// ------------------------------------------------------------------ layout

// Pin the FFI layout so a C bridge and the kernels cannot silently disagree.
const _: () = {
    assert!(size_of::<Ehdr>() == 32);
    assert!(size_of::<SegmentPlan>() == 64);
    assert!(size_of::<AuxvPair>() == 16);
    assert!(size_of::<CredInput>() == 80);
    assert!(size_of::<CredPlan>() == 56);
    assert!(size_of::<DynInfo>() == 64);
    assert!(size_of::<ForeignKey>() == 6);
    assert!(size_of::<Layout>() == 72);
    assert!(size_of::<LoadResources>() == 144);
    assert!(size_of::<LoadResult>() == 136);
    // InterpreterOps is the one struct here whose size follows the pointer
    // width: five function pointers followed by a u8/u16/i32 tail whose
    // padding depends on where that tail starts.  Every other struct above is
    // built from u64 members with explicit padding and is width-independent.
    #[cfg(target_pointer_width = "64")]
    assert!(size_of::<InterpreterOps>() == 64);
    #[cfg(target_pointer_width = "32")]
    assert!(size_of::<InterpreterOps>() == 40);
    assert!(size_of::<AuxvInput>() == 88);
    assert!(size_of::<StartInfoPlan>() == 48 + 2 * MAX_ARG_STRINGS * 8);
    assert!(size_of::<KexecSegmentWire>() == 32);
    assert!(size_of::<BprmLimits>() == 24);
    assert!(size_of::<TxnResource>() == 16);
    assert!(size_of::<ExecTxn>() == 16 + TXN_MAX_RESOURCES * 16);
    assert!(size_of::<ExecOpsFfi>() == 15 * size_of::<usize>());
    assert!(size_of::<KexecSlot>() == 40);
    assert!(size_of::<KexecLedger>() == 24 + KEXEC_MAX_SEGMENTS * 40);
};

// ================================================================== tests
#[cfg(test)]
mod tests {
    use super::*;

    #[derive(Default)]
    struct LoadTrace { stages: [u8; 32], len: usize, fail: u8, resources: LoadResources }
    unsafe fn trace(trace: *mut core::ffi::c_void, stage: u8) -> i32 {
        let t = &mut *(trace as *mut LoadTrace);
        t.stages[t.len] = stage;
        t.len += 1;
        if t.fail == stage { -ENOMEM } else { 0 }
    }
    unsafe extern "C" fn tx_begin(p: *mut core::ffi::c_void, _: u64, _: u64, bias: *mut u64) -> i32 {
        let t = &mut *(p as *mut LoadTrace); t.resources.pgdir = 1; t.resources.mm = 2;
        *bias = 0x1000; trace(p, 1)
    }
    macro_rules! tx_stage { ($name:ident, $n:expr) => {
        unsafe extern "C" fn $name(p: *mut core::ffi::c_void) -> i32 { trace(p, $n) }
    }; }
    unsafe extern "C" fn tx_map(p: *mut core::ffi::c_void, _: u64) -> i32 { trace(p, 2) }
    unsafe extern "C" fn tx_reloc(p: *mut core::ffi::c_void, _: u64, _: u64) -> i32 { trace(p, 4) }
    unsafe extern "C" fn tx_finish(p: *mut core::ffi::c_void, _: *const Layout) -> i32 { trace(p, 5) }
    unsafe extern "C" fn tx_read(p: *mut core::ffi::c_void, _: u64, _: *mut u8, _: usize) -> i32 { trace(p, 6) }
    unsafe extern "C" fn tx_open(p: *mut core::ffi::c_void, _: *const u8, _: usize, fd: *mut i32) -> i32 {
        *fd = 9; (*(p as *mut LoadTrace)).resources.interp_fd = 9; trace(p, 6)
    }
    unsafe extern "C" fn tx_interp(p: *mut core::ffi::c_void, _: u64) -> i32 { trace(p, 7) }
    unsafe extern "C" fn tx_close(p: *mut core::ffi::c_void) { (*(p as *mut LoadTrace)).resources.interp_fd = -1; let _ = trace(p, 11); }
    unsafe extern "C" fn tx_brk(_: *mut core::ffi::c_void, _: u64, out: *mut u64) -> i32 { *out = 0; 0 }
    unsafe extern "C" fn tx_commit(p: *mut core::ffi::c_void, _: *const LoadResult) -> i32 { trace(p, 9) }
    unsafe extern "C" fn tx_tls(p: *mut core::ffi::c_void) -> i32 {
        (*(p as *mut LoadTrace)).resources.tls_buffer = 0x55; trace(p, 3)
    }
    tx_stage!(tx_stack, 7);
    tx_stage!(tx_install_tls, 8);
    unsafe extern "C" fn tx_free_tls(p: *mut core::ffi::c_void) { (*(p as *mut LoadTrace)).resources.tls_buffer = 0; let _ = trace(p, 12); }
    unsafe extern "C" fn tx_discard_vmas(p: *mut core::ffi::c_void) { let _ = trace(p, 13); }
    unsafe extern "C" fn tx_discard_nommu(p: *mut core::ffi::c_void) { let _ = trace(p, 14); }
    unsafe extern "C" fn tx_destroy_pgdir(p: *mut core::ffi::c_void) { let t = &mut *(p as *mut LoadTrace); t.resources.pgdir = 0; t.resources.mm = 0; let _ = trace(p, 15); }

    fn tx_ops(t: &mut LoadTrace) -> LoadTransactionOps {
        LoadTransactionOps { context: t as *mut _ as *mut _, resources: &mut t.resources,
            begin: Some(tx_begin), map_segments: Some(tx_map), stage_tls: Some(tx_tls),
            relocate: Some(tx_reloc), finish_layout: Some(tx_finish),
            read_interpreter: Some(tx_read), open_interpreter: Some(tx_open), path_capacity: 512, nommu: 0,
            load_interpreter: Some(tx_interp),
            close_interpreter: Some(tx_close), map_stack: Some(tx_stack),
            install_tls: Some(tx_install_tls), brk_offset: Some(tx_brk), commit: Some(tx_commit),
            build_interpreter_path: None, arch_interpreter_path: None,
            free_tls: Some(tx_free_tls), discard_vmas: Some(tx_discard_vmas),
            discard_nommu: Some(tx_discard_nommu), destroy_pgdir: Some(tx_destroy_pgdir) }
    }

    fn tx_plan() -> ImagePlan {
        let mut p = ImagePlan::default();
        p.nseg = 1;
        p.segments[0] = SegmentPlan { va: 0x1000, memsz: 0x1000, ..SegmentPlan::default() };
        p.dynamic.present = 1;
        p.interp.present = 1;
        p.tls.present = 1;
        p.tls.filesz = 1;
        p.tls.memsz = 1;
        p
    }

    #[test]
    fn tls_layout_aligns_tcb_and_checks_overflow() {
        let layout = plan_tls_layout(513, 64, 4096, 512).unwrap();
        assert_eq!(layout.tcb_offset, 576);
        assert_eq!(layout.total_size, 1088);
        assert_eq!(layout.pages, 1);
        assert!(plan_tls_layout(u64::MAX, 16, 4096, 512).is_none());
    }

    #[test]
    fn load_transaction_orders_stages_and_commits_once() {
        let p = tx_plan();
        let mut t = LoadTrace::default();
        let ops = tx_ops(&mut t);
        let mut bias = 0;
        assert_eq!(unsafe { a20_elf_load_transaction(&p, &ops, &mut bias) }, 0);
        assert_eq!(bias, 0x1000);
        assert_eq!(&t.stages[..t.len], &[1,2,3,4,5,6,7,11,7,8,9,12]);
        assert_eq!(t.resources.pgdir, 0);
    }

    #[test]
    fn load_transaction_aborts_after_successful_begin_and_never_commits() {
        let p = tx_plan();
        let mut t = LoadTrace { fail: 7, ..LoadTrace::default() };
        let ops = tx_ops(&mut t);
        let mut bias = 0;
        assert_eq!(unsafe { a20_elf_load_transaction(&p, &ops, &mut bias) }, -ENOMEM);
        assert_eq!(&t.stages[..t.len], &[1,2,3,4,5,6,7,11,12,13,14,15]);
        assert_eq!(t.resources.pgdir, 0);
    }

    struct KxTrace { wire: KexecSegmentWire, fail_stage: bool, freed: [u64; 4], nfree: usize, locks: u32 }
    unsafe extern "C" fn kx_test_read(p: *mut core::ffi::c_void, _: u32, out: *mut KexecSegmentWire) -> i32 {
        *out = (*(p as *mut KxTrace)).wire; 0
    }
    unsafe extern "C" fn kx_test_stage(p: *mut core::ffi::c_void, _: *const KexecSegmentWire, pages: *mut u64, bytes: *mut u64) -> i32 {
        let t = &mut *(p as *mut KxTrace);
        if t.fail_stage { return -ENOMEM; }
        *pages = 0x2220; *bytes = 0x1000; 0
    }
    unsafe extern "C" fn kx_test_lock(p: *mut core::ffi::c_void) -> u64 { (*(p as *mut KxTrace)).locks += 1; 0 }
    unsafe extern "C" fn kx_test_unlock(_: *mut core::ffi::c_void, _: u64) {}
    unsafe extern "C" fn kx_test_free(p: *mut core::ffi::c_void, pages: u64) {
        let t = &mut *(p as *mut KxTrace); t.freed[t.nfree] = pages; t.nfree += 1;
    }
    fn kx_test_ops(t: &mut KxTrace) -> KexecLoadOps {
        KexecLoadOps { context: t as *mut _ as *mut _, read_segment: Some(kx_test_read),
            stage_segment: Some(kx_test_stage), lock: Some(kx_test_lock),
            unlock: Some(kx_test_unlock), free_pages: Some(kx_test_free) }
    }
    fn kx_test_loaded(pages: u64) -> KexecLedger {
        let mut stage = kx_init(); let mut dst = kx_init();
        kx_stage_open(&mut stage, 0x1000, 1, KEXEC_MAX_SEGMENTS as u64, false).unwrap();
        let index = kx_stage_check(&mut stage, 0x2000, 0x1000, 0x3000, 0x1000, 0, 0x1000).unwrap();
        kx_stage_pages(&mut stage, index, pages, 0x1000).unwrap();
        kx_publish(&mut dst, &mut stage).unwrap(); dst
    }

    #[test]
    fn kexec_transaction_keeps_old_image_on_stage_failure() {
        let mut old = kx_test_loaded(0x1110);
        let mut trace = KxTrace { wire: KexecSegmentWire { buf: 0x5000, bufsz: 0x1000, mem: 0x6000, memsz: 0x1000 },
            fail_stage: true, freed: [0;4], nfree: 0, locks: 0 };
        let ops = kx_test_ops(&mut trace);
        assert_eq!(unsafe { a20_elf_kexec_load_transaction(&mut old, 0x7000, 1, 0, 0x1000, &ops) }, -ENOMEM);
        assert!(kx_loaded(&old)); assert_eq!(old.seg[0].pages, 0x1110);
        assert_eq!(trace.nfree, 0); assert_eq!(trace.locks, 0);
    }

    #[test]
    fn kexec_transaction_replaces_only_after_staging_and_releases_old_once() {
        let mut old = kx_test_loaded(0x1110);
        let mut trace = KxTrace { wire: KexecSegmentWire { buf: 0x5000, bufsz: 0x1000, mem: 0x6000, memsz: 0x1000 },
            fail_stage: false, freed: [0;4], nfree: 0, locks: 0 };
        let ops = kx_test_ops(&mut trace);
        assert_eq!(unsafe { a20_elf_kexec_load_transaction(&mut old, 0x7000, 1, 0, 0x1000, &ops) }, 0);
        assert!(kx_loaded(&old)); assert_eq!(old.seg[0].pages, 0x2220);
        assert_eq!(&trace.freed[..trace.nfree], &[0x1110]); assert_eq!(trace.locks, 1);
    }

    struct KxFileTrace { bytes: [u8; 4], fail_stage: bool, freed: [u64; 4], nfree: usize, buffers_freed: usize, locks: u32 }
    unsafe extern "C" fn kxf_read(p: *mut core::ffi::c_void, buf: *mut *const u8, size: *mut u64) -> i32 {
        let t = &mut *(p as *mut KxFileTrace); *buf = t.bytes.as_ptr(); *size = 4; 0
    }
    unsafe extern "C" fn kxf_stage(p: *mut core::ffi::c_void, _: *const u8, _: u64, pages: *mut u64, bytes: *mut u64) -> i32 {
        let t = &mut *(p as *mut KxFileTrace); if t.fail_stage { return -ENOMEM; }
        *pages = 0x3330; *bytes = 0x1000; 0
    }
    unsafe extern "C" fn kxf_free_buffer(p: *mut core::ffi::c_void, _: *const u8) { (*(p as *mut KxFileTrace)).buffers_freed += 1; }
    unsafe extern "C" fn kxf_lock(p: *mut core::ffi::c_void) -> u64 { (*(p as *mut KxFileTrace)).locks += 1; 0 }
    unsafe extern "C" fn kxf_unlock(_: *mut core::ffi::c_void, _: u64) {}
    unsafe extern "C" fn kxf_free(p: *mut core::ffi::c_void, pages: u64) { let t=&mut *(p as *mut KxFileTrace); t.freed[t.nfree]=pages; t.nfree+=1; }
    fn kxf_ops(t: &mut KxFileTrace) -> KexecFileOps { KexecFileOps { context:t as *mut _ as *mut _, read_file:Some(kxf_read),
        stage_file:Some(kxf_stage), free_buffer:Some(kxf_free_buffer), lock:Some(kxf_lock),
        unlock:Some(kxf_unlock), free_pages:Some(kxf_free) } }

    #[test]
    fn kexec_file_transaction_pins_old_until_replacement_and_frees_buffer() {
        let mut old = kx_test_loaded(0x1110);
        let mut t = KxFileTrace { bytes:[0x7f,b'E',b'L',b'F'], fail_stage:false, freed:[0;4], nfree:0, buffers_freed:0, locks:0 };
        let ops = kxf_ops(&mut t);
        assert_eq!(unsafe { a20_elf_kexec_file_transaction(&mut old,&ops) },0);
        assert_eq!(old.seg[0].pages,0x3330); assert_eq!(t.buffers_freed,1);
        assert_eq!(&t.freed[..t.nfree],&[0x1110]); assert_eq!(t.locks,1);
    }

    #[test]
    fn kexec_file_transaction_refusal_preserves_old_image() {
        let mut old = kx_test_loaded(0x1110);
        let mut t = KxFileTrace { bytes:[0x7f,b'E',b'L',b'F'], fail_stage:true, freed:[0;4], nfree:0, buffers_freed:0, locks:0 };
        let ops = kxf_ops(&mut t);
        assert_eq!(unsafe { a20_elf_kexec_file_transaction(&mut old,&ops) },-ENOMEM);
        assert_eq!(old.seg[0].pages,0x1110); assert_eq!(t.buffers_freed,1);
        assert_eq!(t.nfree,0); assert_eq!(t.locks,0);
    }

    const CLASS: u8 = ELFCLASS64;
    const MACH: u16 = EM_RISCV;

    /// Minimal 64-bit little-endian ELF header with the given fields.
    fn ehdr64(e_type: u16, machine: u16, entry: u64, phoff: u64, phnum: u16, phentsize: u16) -> Vec<u8> {
        let mut b = vec![0u8; EHDR64_SIZE];
        b[0..4].copy_from_slice(&[0x7f, b'E', b'L', b'F']);
        b[4] = ELFCLASS64;
        b[5] = ELFDATA2LSB;
        b[6] = 1;
        b[16..18].copy_from_slice(&e_type.to_le_bytes());
        b[18..20].copy_from_slice(&machine.to_le_bytes());
        b[20..24].copy_from_slice(&1u32.to_le_bytes());
        b[24..32].copy_from_slice(&entry.to_le_bytes());
        b[32..40].copy_from_slice(&phoff.to_le_bytes());
        b[52..54].copy_from_slice(&(EHDR64_SIZE as u16).to_le_bytes());
        b[54..56].copy_from_slice(&phentsize.to_le_bytes());
        b[56..58].copy_from_slice(&phnum.to_le_bytes());
        b
    }

    fn phdr64(p_type: u32, flags: u32, off: u64, vaddr: u64, filesz: u64, memsz: u64, align: u64) -> Vec<u8> {
        let mut b = vec![0u8; PHDR64_SIZE];
        b[0..4].copy_from_slice(&p_type.to_le_bytes());
        b[4..8].copy_from_slice(&flags.to_le_bytes());
        b[8..16].copy_from_slice(&off.to_le_bytes());
        b[16..24].copy_from_slice(&vaddr.to_le_bytes());
        b[32..40].copy_from_slice(&filesz.to_le_bytes());
        b[40..48].copy_from_slice(&memsz.to_le_bytes());
        b[48..56].copy_from_slice(&align.to_le_bytes());
        b
    }

    fn concat(parts: &[Vec<u8>]) -> Vec<u8> {
        let mut v = Vec::new();
        for p in parts {
            v.extend_from_slice(p);
        }
        v
    }

    #[test]
    fn header_accepts_minimal_exec() {
        let eh = ehdr64(ET_EXEC, MACH, 0x1000, 64, 1, 56);
        let e = parse_ehdr(&eh).unwrap();
        assert_eq!(e.e_type, ET_EXEC);
        assert!(validate_ehdr(&e, CLASS, MACH).is_ok());
    }

    #[test]
    fn header_rejects_bad_magic() {
        let mut eh = ehdr64(ET_EXEC, MACH, 0, 64, 1, 56);
        eh[1] = b'X';
        assert!(parse_ehdr(&eh).is_none());
    }

    #[test]
    fn header_rejects_wrong_class_endian_type_machine() {
        let e = parse_ehdr(&ehdr64(ET_EXEC, MACH, 0, 64, 1, 56)).unwrap();
        assert_eq!(validate_ehdr(&e, ELFCLASS32, MACH), Err(-ENOEXEC));
        let mut be = ehdr64(ET_EXEC, MACH, 0, 64, 1, 56);
        be[5] = 2;
        assert_eq!(validate_ehdr(&parse_ehdr(&be).unwrap(), CLASS, MACH), Err(-ENOEXEC));
        let dyn_wrong = parse_ehdr(&ehdr64(1, MACH, 0, 64, 1, 56)).unwrap();
        assert_eq!(validate_ehdr(&dyn_wrong, CLASS, MACH), Err(-ENOEXEC));
        let wrongmach = parse_ehdr(&ehdr64(ET_EXEC, EM_X86_64, 0, 64, 1, 56)).unwrap();
        assert_eq!(validate_ehdr(&wrongmach, CLASS, MACH), Err(-ENOEXEC));
    }

    #[test]
    fn header_rejects_truncated() {
        let eh = ehdr64(ET_EXEC, MACH, 0, 64, 1, 56);
        assert!(parse_ehdr(&eh[..EHDR64_SIZE - 1]).is_none());
        assert!(parse_ehdr(&eh[..10]).is_none());
    }

    #[test]
    fn header_rejects_bad_phentsize_and_phnum() {
        let e = parse_ehdr(&ehdr64(ET_EXEC, MACH, 0, 64, 1, 40)).unwrap();
        assert_eq!(validate_ehdr(&e, CLASS, MACH), Err(-ENOEXEC));
        let e0 = parse_ehdr(&ehdr64(ET_EXEC, MACH, 0, 64, 0, 56)).unwrap();
        assert_eq!(validate_ehdr(&e0, CLASS, MACH), Err(-ENOEXEC));
        let ebig = parse_ehdr(&ehdr64(ET_EXEC, MACH, 0, 64, (MAX_PHDRS + 1) as u16, 56)).unwrap();
        assert_eq!(validate_ehdr(&ebig, CLASS, MACH), Err(-ENOEXEC));
    }

    #[test]
    fn plan_loads_one_segment() {
        let eh = ehdr64(ET_EXEC, MACH, 0x1000, 64, 1, 56);
        let ph = phdr64(PT_LOAD, PF_R | PF_X, 0, 0x1000, 0x100, 0x100, 0x1000);
        let plan = plan_image(&eh, &ph, CLASS, MACH, MM_WX_OFF, false).unwrap();
        assert_eq!(plan.nseg, 1);
        assert_eq!(plan.segments[0].prot, PROT_READ | PROT_EXEC);
        assert_eq!(plan.segments[0].anon_tail, 0);
    }

    #[test]
    fn ffi_image_span_and_segment_execution_follow_plan_order() {
        unsafe extern "C" fn record_segment(
            context: *mut core::ffi::c_void,
            segment: *const SegmentPlan,
            bias: u64,
        ) -> i32 {
            let calls = &mut *(context as *mut Vec<(u16, u64)>);
            calls.push(((*segment).ph_index, bias));
            0
        }

        let mut plan = ImagePlan::default();
        plan.nseg = 2;
        plan.segments[0] = SegmentPlan { ph_index: 4, va: 0x5008, memsz: 0x20, ..Default::default() };
        plan.segments[1] = SegmentPlan { ph_index: 7, va: 0x1004, memsz: 0x10, ..Default::default() };
        let mut min = 0;
        let mut max = 0;
        assert_eq!(unsafe { a20_elf_image_span(&plan, &mut min, &mut max) }, 0);
        assert_eq!((min, max), (0x1000, 0x6000));
        let mut calls: Vec<(u16, u64)> = Vec::new();
        assert_eq!(unsafe {
            a20_elf_map_planned_segments(
                &plan, 0x2000, &mut calls as *mut _ as *mut core::ffi::c_void,
                Some(record_segment),
            )
        }, 0);
        assert_eq!(calls, vec![(4, 0x2000), (7, 0x2000)]);
    }

    #[test]
    fn plan_detects_filesz_gt_memsz() {
        let eh = ehdr64(ET_EXEC, MACH, 0, 64, 1, 56);
        let ph = phdr64(PT_LOAD, PF_R, 0, 0x1000, 0x200, 0x100, 0x1000);
        assert_eq!(plan_image(&eh, &ph, CLASS, MACH, MM_WX_OFF, false), Err(-ENOEXEC));
    }

    #[test]
    fn plan_detects_vaddr_overflow() {
        let eh = ehdr64(ET_EXEC, MACH, 0, 64, 1, 56);
        let ph = phdr64(PT_LOAD, PF_R, 0, u64::MAX - 8, 0, 0x1000, 0x1000);
        assert_eq!(plan_image(&eh, &ph, CLASS, MACH, MM_WX_OFF, false), Err(-ENOEXEC));
    }

    #[test]
    fn plan_detects_phoff_wrap() {
        // phnum * phentsize overflows usize is not reachable with u16 counts,
        // but a table larger than the supplied buffer must be refused.
        let eh = ehdr64(ET_EXEC, MACH, 0, 64, 4, 56);
        let ph = phdr64(PT_LOAD, PF_R, 0, 0x1000, 0, 0, 0x1000);
        assert_eq!(plan_image(&eh, &ph, CLASS, MACH, MM_WX_OFF, false), Err(-ENOEXEC));
    }

    #[test]
    fn plan_rejects_bad_offset_vaddr_congruence() {
        let eh = ehdr64(ET_EXEC, MACH, 0, 64, 1, 56);
        // filesz != 0, offset low bits 0x10, vaddr low bits 0 -> mismatch
        let ph = phdr64(PT_LOAD, PF_R, 0x10, 0x1000, 0x100, 0x100, 0x10);
        assert_eq!(plan_image(&eh, &ph, CLASS, MACH, MM_WX_OFF, false), Err(-EINVAL));
    }

    #[test]
    fn plan_rejects_overlapping_segments() {
        let eh = ehdr64(ET_EXEC, MACH, 0, 64, 2, 56);
        let a = phdr64(PT_LOAD, PF_R, 0x0, 0x1000, 0x2000, 0x2000, 0x1000);
        let b = phdr64(PT_LOAD, PF_R, 0x1000, 0x2000, 0x2000, 0x2000, 0x1000);
        assert_eq!(plan_image(&eh, &concat(&[a, b]), CLASS, MACH, MM_WX_OFF, false), Err(-EINVAL));
    }

    #[test]
    fn plan_allows_adjacent_segments() {
        let eh = ehdr64(ET_EXEC, MACH, 0, 64, 2, 56);
        let a = phdr64(PT_LOAD, PF_R, 0x0, 0x1000, 0x1000, 0x1000, 0x1000);
        let b = phdr64(PT_LOAD, PF_R | PF_W, 0x1000, 0x2000, 0x1000, 0x1000, 0x1000);
        let plan = plan_image(&eh, &concat(&[a, b]), CLASS, MACH, MM_WX_OFF, false).unwrap();
        assert_eq!(plan.nseg, 2);
    }

    #[test]
    fn plan_allows_adjacent_segments_sharing_page_with_bss() {
        let eh = ehdr64(ET_EXEC, MACH, 0x1000, 64, 2, 56);
        let data = phdr64(PT_LOAD, PF_R | PF_W, 0x1000, 0x5000, 4, 4, 0x1000);
        let bss = phdr64(PT_LOAD, PF_R | PF_W, 8, 0x5008, 0, 20, 0x1000);
        let plan = plan_image(&eh, &concat(&[data, bss]), CLASS, MACH, MM_WX_OFF, false)
            .expect("byte-disjoint segments may share a page");
        assert_eq!(plan.nseg, 2);
        assert_eq!(plan.segments[1].filesz, 0);
        assert_eq!(plan.segments[1].anon_tail, 1);
    }

    #[test]
    fn plan_allows_shared_page_with_different_protection() {
        let eh = ehdr64(ET_EXEC, MACH, 0x1000, 64, 2, 56);
        let ro = phdr64(PT_LOAD, PF_R, 0x1000, 0x5000, 4, 4, 0x1000);
        let rw = phdr64(PT_LOAD, PF_R | PF_W, 8, 0x5008, 0, 20, 0x1000);
        let plan = plan_image(&eh, &concat(&[ro, rw]), CLASS, MACH, MM_WX_DENY, false)
            .expect("individual non-WX segments retain C shared-page behavior");
        assert_eq!(plan.nseg, 2);
        assert_eq!(plan.segments[0].prot, PROT_READ);
        assert_eq!(plan.segments[1].prot, PROT_READ | PROT_WRITE);
    }

    #[test]
    fn wx_policy_strip_and_deny() {
        let rwx = PROT_READ | PROT_WRITE | PROT_EXEC;
        assert_eq!(wx_filter(rwx, MM_WX_OFF, false), rwx);
        assert_eq!(wx_filter(rwx, MM_WX_STRIP, false), PROT_READ | PROT_EXEC);
        assert_eq!(wx_filter(rwx, MM_WX_DENY, false), -EACCES);
        assert_eq!(wx_filter(rwx, MM_WX_DENY, true), rwx);
        let rw = PROT_READ | PROT_WRITE;
        assert_eq!(wx_filter(rw, MM_WX_DENY, false), rw);
    }

    #[test]
    fn wx_deny_is_decided_inside_plan() {
        let eh = ehdr64(ET_EXEC, MACH, 0, 64, 1, 56);
        let ph = phdr64(PT_LOAD, PF_R | PF_W | PF_X, 0, 0x1000, 0x100, 0x100, 0x1000);
        assert_eq!(plan_image(&eh, &ph, CLASS, MACH, MM_WX_DENY, false), Err(-EACCES));
        assert!(plan_image(&eh, &ph, CLASS, MACH, MM_WX_DENY, true).is_ok());
    }

    #[test]
    fn plan_records_interp_tls_dynamic() {
        let eh = ehdr64(ET_DYN, MACH, 0x2000, 64, 4, 56);
        let load = phdr64(PT_LOAD, PF_R, 0x0, 0x0, 0x1000, 0x1000, 0x1000);
        let interp = phdr64(PT_INTERP, PF_R, 0x1000, 0, 0x20, 0x20, 1);
        let tls = phdr64(PT_TLS, PF_R, 0x2000, 0x2000, 0x40, 0x80, 16);
        let dynseg = phdr64(PT_DYNAMIC, PF_R | PF_W, 0x3000, 0x3000, 0x100, 0x100, 8);
        let plan = plan_image(&eh, &concat(&[load, interp, tls, dynseg]), CLASS, MACH, MM_WX_OFF, false)
            .unwrap();
        assert_eq!(plan.is_dyn, 1);
        assert_eq!(plan.interp.present, 1);
        assert_eq!(plan.tls.present, 1);
        assert_eq!(plan.tls.align, 16);
        assert_eq!(plan.dynamic.present, 1);
    }

    #[test]
    fn plan_tls_present_without_load_is_rejected() {
        let eh = ehdr64(ET_EXEC, MACH, 0, 64, 1, 56);
        let tls = phdr64(PT_TLS, PF_R, 0x1000, 0x1000, 0x40, 0x80, 16);
        assert_eq!(plan_image(&eh, &tls, CLASS, MACH, MM_WX_OFF, false), Err(-ENOEXEC));
    }

    #[test]
    fn native_marker_sets_abi() {
        let eh = ehdr64(ET_EXEC, MACH, 0, 64, 2, 56);
        let load = phdr64(PT_LOAD, PF_R, 0, 0x1000, 0x100, 0x100, 0x1000);
        let marker = phdr64(PT_A20_START_INFO, 0, 0, 0, 0, 0, 0);
        let plan = plan_image(&eh, &concat(&[load, marker]), CLASS, MACH, MM_WX_OFF, false).unwrap();
        assert_eq!(plan.is_native_abi, 1);
    }

    #[test]
    fn load_bias_policy() {
        assert_eq!(plan_load_bias(true, 0x10000, 0x220000), 0x230000);
        assert_eq!(plan_load_bias(false, 0x10000, 0x220000), 0);
    }

    #[test]
    fn finish_layout_derives_base_max_and_phdr() {
        let eh = ehdr64(ET_DYN, MACH, 0x1100, 64, 2, 56);
        let a = phdr64(PT_LOAD, PF_R, 0x0, 0x0, 0x1000, 0x1000, 0x1000);
        let b = phdr64(PT_LOAD, PF_R | PF_W, 0x1000, 0x2000, 0x800, 0x1000, 0x1000);
        let plan = plan_image(&eh, &concat(&[a, b]), CLASS, MACH, MM_WX_OFF, false).unwrap();
        let l = finish_layout(&plan, 0x10000);
        assert_eq!(l.base, 0x10000);
        assert_eq!(l.max_va, 0x13000);
        assert_eq!(l.brk_va, 0x13000);
        assert_eq!(l.hdr_map_va, 0x10000);
        assert_eq!(l.phdr_va, 0x10000 + 64);
        assert_eq!(l.entry, 0x11100);
    }

    #[test]
    fn dynamic_scan_reads_tags() {
        // DT_RELA, DT_RELASZ, DT_SYMTAB, DT_NULL
        let mut b = Vec::new();
        for (tag, val) in [
            (DT_RELA, 0x1000u64),
            (DT_RELASZ, 0x100u64),
            (DT_SYMTAB, 0x2000u64),
            (DT_NULL, 0u64),
        ] {
            b.extend_from_slice(&(tag as i64).to_le_bytes());
            b.extend_from_slice(&val.to_le_bytes());
        }
        let d = scan_dynamic(&b, true).unwrap();
        assert_eq!(d.rela, 0x1000);
        assert_eq!(d.relasz, 0x100);
        assert_eq!(d.symtab, 0x2000);
        assert_eq!(d.has_rela, 1);
        assert_eq!(d.syment, 24);
    }

    #[test]
    fn dynamic_scan_rejects_tiny_relaent() {
        let mut b = Vec::new();
        for (tag, val) in [
            (DT_RELA, 0x1000u64),
            (DT_RELASZ, 0x100u64),
            (DT_RELAENT, 8u64),
            (DT_NULL, 0u64),
        ] {
            b.extend_from_slice(&(tag as i64).to_le_bytes());
            b.extend_from_slice(&val.to_le_bytes());
        }
        assert_eq!(scan_dynamic(&b, true), Err(-ENOEXEC));
    }

    #[test]
    fn reloc_types_supported_machines() {
        assert_eq!(reloc_types(EM_RISCV, true), (3, 2));
        assert_eq!(reloc_types(EM_AARCH64, true), (1027, 257));
        assert_eq!(reloc_types(EM_X86_64, true), (8, 1));
        assert_eq!(reloc_types(EM_ARM, false), (23, 2));
        assert_eq!(reloc_types(EM_ARM, true).1, u32::MAX);
        assert_eq!(reloc_types(0xffff, true), (u32::MAX, u32::MAX));
    }

    #[test]
    fn reloc_action_classification_and_bounds() {
        let (rel, abs) = reloc_types(EM_RISCV, true);
        assert_eq!(reloc_action(3, 0, 1, 0x12, 1, rel, abs), Ok(RelocAction::Relative));
        // ABS64 with valid in-range defined symbol
        assert_eq!(reloc_action(2, 0, 4, 0x10, 1, rel, abs), Ok(RelocAction::Absolute));
        // ABS64 with symidx out of range
        assert_eq!(reloc_action(2, 9, 4, 0x10, 1, rel, abs), Err(-ENOEXEC));
        // ABS64 undefined weak resolves
        assert_eq!(reloc_action(2, 0, 4, 0x20, 0, rel, abs), Ok(RelocAction::Absolute));
        // ABS64 undefined strong is fatal
        assert_eq!(reloc_action(2, 0, 4, 0x10, 0, rel, abs), Err(-ENOEXEC));
        // Unrelated type is skipped
        assert_eq!(reloc_action(99, 0, 4, 0, 1, rel, abs), Ok(RelocAction::Skip));
    }

    #[test]
    fn dynsym_count_from_sysv() {
        let mut sysv = vec![0u8; 16];
        sysv[4..8].copy_from_slice(&7u32.to_le_bytes());
        assert_eq!(dynsym_count(Some(&sysv), None), 7);
    }

    #[test]
    fn stack_plan_basic_layout() {
        let argv: [&[u8]; 2] = [b"/bin/sh", b"-c"];
        let envp: [&[u8]; 1] = [b"PATH=/"];
        let aux = AuxvInput {
            phdr_va: 0x400040,
            phentsize: 56,
            phnum: 4,
            page_size: 4096,
            interp_base: 0x40000000,
            exec_entry: 0x401000,
            hwcap: 0,
            hwcap2: 0,
            vdso_ehdr: 0,
            clktck: 100,
            secure: 0,
        };
        let plan = plan_stack(0x3FFFF000, 8, &argv, &envp, b"riscv64", b"/bin/app", &aux).unwrap();
        assert_eq!(plan.sp & 15, 0);
        assert_eq!(plan.arg_count, 2);
        assert_eq!(plan.env_count, 1);
        assert_eq!(plan.auxv_count, 20);
        // argv[0] sits below argv[1] (reverse placement).
        assert!(plan.arg_addrs[0] < plan.arg_addrs[1]);
        // The final SP must be below every string.
        assert!(plan.sp < plan.arg_addrs[0]);
        assert!(plan.sp < plan.env_addrs[0]);
        assert!(plan.platform_va < plan.arg_addrs[0]);
        assert!(plan.execfn_va < plan.platform_va);
        assert!(plan.random_va + 16 == plan.execfn_va);
        // AT_NULL terminator present; AT_EXECFN precedes it.
        assert_eq!(plan.auxv[19].key, 0);
        assert_eq!(plan.auxv[18].key, 31);
        assert_eq!(plan.auxv[18].val, plan.execfn_va);
        assert_eq!(plan.auxv[3].key, 6); // AT_PAGESZ
        assert_eq!(plan.auxv[3].val, 4096);
    }

    #[test]
    fn stack_plan_rejects_overlong_arrays() {
        let big: Vec<&[u8]> = vec![b"x"; MAX_ARG_STRINGS + 1];
        let aux = AuxvInput::default();
        assert_eq!(plan_stack(0x3FFFF000, 8, &big, &[], b"p", b"", &aux), Err(-E2BIG));
    }

    #[test]
    fn stack_plan_32bit_pointer_sizes() {
        let argv: [&[u8]; 1] = [b"a"];
        let aux = AuxvInput { page_size: 4096, clktck: 100, ..Default::default() };
        let plan = plan_stack(0x3FFFF000, 4, &argv, &[], b"arm32", b"/bin/app", &aux).unwrap();
        assert_eq!(plan.sp & 15, 0);
    }

    #[test]
    fn creds_nnp_is_sticky() {
        let input = CredInput {
            no_new_privs: 1,
            uid: 1000,
            euid: 1000,
            suid: 0,
            egid: 1000,
            ..Default::default()
        };
        let p = plan_exec_creds(&input);
        assert_eq!(p.suid, 1000);
        assert_eq!(p.sgid, 1000);
    }

    #[test]
    fn creds_setuid_elevates_and_clears_dumpable() {
        let input = CredInput {
            uid: 1000,
            euid: 1000,
            suid: 1000,
            egid: 1000,
            st_mode: S_ISUID,
            st_uid: 0,
            cap_bounding: 0xff,
            ..Default::default()
        };
        let p = plan_exec_creds(&input);
        assert_eq!(p.euid, 0);
        assert_eq!(p.suid, 0);
        assert_eq!(p.fsuid, 0);
        assert_eq!(p.make_undumpable, 1);
        assert_eq!(p.cap_permitted, 0xff);
        assert_eq!(p.cap_effective, 0xff);
    }

    #[test]
    fn creds_root_loss_clears_caps() {
        // euid 0 -> 1000 via a setuid-to-unprivileged transition: the new
        // credential is no longer root, so capability sets are dropped.
        let input = CredInput {
            uid: 1000,
            euid: 0,
            suid: 0,
            egid: 1000,
            st_mode: S_ISUID,
            st_uid: 1000,
            cap_bounding: 0xff,
            cap_permitted: 0xff,
            cap_effective: 0xff,
            ..Default::default()
        };
        let p = plan_exec_creds(&input);
        assert_eq!(p.euid, 1000);
        assert_eq!(p.cap_effective, 0);
        assert_eq!(p.cap_permitted, 0);
    }

    #[test]
    fn exec_txn_ledger_and_ordering() {
        // The ledger walks the fixed sequence and refuses to fabricate a
        // commit before every phase has run.
        let mut txn = txn_init();
        assert_eq!(txn_commit(&mut txn), -EINVAL);
        let mut seen: Vec<i32> = Vec::new();
        loop {
            let p = txn_begin(&mut txn);
            if p == TXN_DONE {
                break;
            }
            assert!(p >= 0, "unexpected begin rc {p}");
            seen.push(p);
        }
        assert_eq!(seen.len(), COMMIT_PHASES.len());
        for (i, &p) in COMMIT_PHASES.iter().enumerate() {
            assert_eq!(seen[i], p as i32);
        }
        assert_eq!(txn_commit(&mut txn), 0);
        // A committed transaction has nothing to release.
        assert!(txn_undo_next(&mut txn).is_none());
        assert_eq!(txn_begin(&mut txn), -EINVAL);
    }

    #[test]
    fn exec_txn_undo_is_newest_first() {
        let mut txn = txn_init();
        assert_eq!(txn_begin(&mut txn), CommitPhase::StackBuild as i32);
        assert_eq!(txn_acquire(&mut txn, RES_IMAGE, 0xAAAA), 0);
        assert_eq!(txn_begin(&mut txn), CommitPhase::VdsoMap as i32);
        assert_eq!(txn_begin(&mut txn), CommitPhase::AllocNewMm as i32);
        assert_eq!(txn_acquire(&mut txn, RES_ABI_MODE, 1), 0);
        assert_eq!(txn_acquire(&mut txn, RES_NEW_MM, 0xBBBB), 0);
        // Transfer drops a live entry (e.g. the image handed to the new mm).
        assert_eq!(txn_transfer(&mut txn, RES_IMAGE, 0xAAAA), 0);
        // Newest first and consuming: mm, then abi mode, then empty.
        assert_eq!(txn_undo_next(&mut txn), Some((RES_NEW_MM, 0xBBBB)));
        assert_eq!(txn_undo_next(&mut txn), Some((RES_ABI_MODE, 1)));
        assert!(txn_undo_next(&mut txn).is_none());
        assert_eq!(txn_abort(&mut txn), 0);
        assert!(txn_undo_next(&mut txn).is_none());
        assert_eq!(txn_acquire(&mut txn, RES_NEW_MM, 1), -EINVAL);
    }

    #[test]
    fn exec_txn_irreversible_boundary_refuses_rollback() {
        // Starting sibling termination marks the boundary before its first
        // process-wide irreversible effect.
        let mut txn = txn_init();
        loop {
            let p = txn_begin(&mut txn);
            if p == CommitPhase::TerminateSiblings as i32 {
                break;
            }
            assert!(p >= 0, "unexpected begin rc {p}");
        }
        assert_eq!(txn.irreversible, 1);
        assert_eq!(txn_acquire(&mut txn, RES_NEW_MM, 0x1234), 0);
        // Past the boundary the rollback protocol hands back nothing.
        assert!(txn_undo_next(&mut txn).is_none());
        assert_eq!(txn_abort(&mut txn), -EPERM);
        // Refusing the abort leaves the transaction running, so the walk can
        // still finish and commit the new image.
        while txn_begin(&mut txn) >= 0 {}
        assert_eq!(txn_commit(&mut txn), 0);
        assert!(txn_undo_next(&mut txn).is_none());
    }

    #[test]
    fn exec_txn_capacity_is_refused_not_dropped() {
        let mut txn = txn_init();
        assert_eq!(txn_begin(&mut txn), 0);
        for i in 0..TXN_MAX_RESOURCES as u64 {
            assert_eq!(txn_acquire(&mut txn, RES_NEW_MM, i), 0);
        }
        assert_eq!(txn_acquire(&mut txn, RES_NEW_MM, 0xFFFF), -ENOMEM);
        let mut n = 0;
        while txn_undo_next(&mut txn).is_some() {
            n += 1;
        }
        assert_eq!(n, TXN_MAX_RESOURCES);
    }

    #[test]
    fn failable_phases_are_the_fallible_ones() {
        for (i, &p) in COMMIT_PHASES.iter().enumerate() {
            let want = matches!(
                p,
                CommitPhase::StackBuild
                    | CommitPhase::VdsoMap
                    | CommitPhase::AllocNewMm
                    | CommitPhase::FdtableUnshare
            );
            assert_eq!(phase_is_failable(p), want);
            assert_eq!(a20_elf_phase_failable(i as u32), want);
        }
        assert!(!a20_elf_phase_failable(99));
        assert!(a20_elf_phase_failable(0x100 | CommitPhase::StackBuild as u32));
        assert!(a20_elf_phase_failable(0x100 | CommitPhase::AllocNewMm as u32));
        assert!(!a20_elf_phase_failable(0x100 | CommitPhase::FdtableUnshare as u32));
    }

    #[test]
    fn stack_plan_a20_layout() {
        let argv: [&[u8]; 2] = [b"/bin/app", b"--flag"];
        let envp: [&[u8]; 1] = [b"K=V"];
        let si_size = 128u64;
        let plan = plan_stack_a20(0x3FFFF000, 8, &argv, &envp, si_size).unwrap();
        assert_eq!(plan.sp & 15, 0);
        assert_eq!(plan.argc, 2);
        assert_eq!(plan.envc, 1);
        assert_eq!(plan.size as u64, si_size);
        assert_eq!(plan.start_info_va, plan.sp);
        // The start-info struct sits directly below the argv array; the C
        // reference aligns the descriptor bottom down, so when the argv array
        // is not 16-aligned there is a small padding gap above the struct.
        assert!(plan.argv_va >= plan.sp + si_size);
        assert!(plan.argv_va - (plan.sp + si_size) < 16);
        // envp array sits above the argv array.
        assert_eq!(plan.envp_va, plan.argv_va + (2 + 1) * 8);
        // The final SP is below every string placement would allow.
        assert!(plan.envp_va + (1 + 1) * 8 <= 0x3FFFF000);
        // Dynamic-native placement keeps the descriptor at the reserved top.
        assert_eq!(plan_stack_a20_dynamic(0x3FFFF000, si_size), 0x3FFFF000 - si_size);
        let empty: [&[u8]; 0] = [];
        let p2 = plan_stack_a20(0x10000, 8, &empty, &empty, si_size).unwrap();
        assert_eq!(p2.sp & 15, 0);
        let big: Vec<&[u8]> = vec![b"x"; MAX_ARG_STRINGS + 1];
        assert_eq!(plan_stack_a20(0x10000, 8, &big, &empty, si_size), Err(-E2BIG));
    }

    #[test]
    fn kexec_segment_validation() {
        assert!(kexec_wire_ok(1, KEXEC_MAX_SEGMENTS as u64));
        assert!(kexec_wire_ok(KEXEC_MAX_SEGMENTS as u64, KEXEC_MAX_SEGMENTS as u64));
        assert!(!kexec_wire_ok(0, KEXEC_MAX_SEGMENTS as u64));
        assert!(!kexec_wire_ok(KEXEC_MAX_SEGMENTS as u64 + 1, KEXEC_MAX_SEGMENTS as u64));

        let kernel = (0x10_0000u64, 0x20_0000u64);
        // Valid segment away from the kernel and predecessors.
        let s0 = KexecSegmentWire { buf: 0x1000, bufsz: 0x100, mem: 0x100_0000, memsz: 0x1000 };
        assert!(kexec_segment_ok(&s0, kernel.0, kernel.1, &[]).is_ok());
        // bufsz > memsz.
        let bad = KexecSegmentWire { bufsz: 0x2000, ..s0 };
        assert_eq!(kexec_segment_ok(&bad, kernel.0, kernel.1, &[]), Err(-EINVAL));
        // mem + memsz wraps.
        let wrap = KexecSegmentWire { mem: u64::MAX - 0x800, memsz: 0x1000, bufsz: 0, buf: 0 };
        assert_eq!(kexec_segment_ok(&wrap, kernel.0, kernel.1, &[]), Err(-EINVAL));
        // Overlap with the running kernel.
        let over_k = KexecSegmentWire { buf: 0, bufsz: 0, mem: 0x1F_0000, memsz: 0x40_000 };
        assert_eq!(kexec_segment_ok(&over_k, kernel.0, kernel.1, &[]), Err(-EADDRNOTAVAIL));
        // Overlap with a staged predecessor.
        let s1 = KexecSegmentWire { buf: 0, bufsz: 0, mem: 0x100_0000 + 0x800, memsz: 0x1000 };
        assert_eq!(kexec_segment_ok(&s1, kernel.0, kernel.1, &[s0]), Err(-EADDRNOTAVAIL));
        // Adjacent to the predecessor is fine.
        let s1b = KexecSegmentWire { mem: 0x100_0000 + 0x1000, ..s1 };
        assert!(kexec_segment_ok(&s1b, kernel.0, kernel.1, &[s0]).is_ok());
        assert!(!unsafe { a20_elf_is_elf_image(core::ptr::null(), 0) });
        assert!(is_elf_image(b"\x7fELF"));
        assert!(!is_elf_image(b"\x7fELG"));
        assert!(!is_elf_image(b"\x7f"));
    }

    #[test]
    fn kexec_ledger_staging_and_publish() {
        let kernel = (0x10_0000u64, 0x20_0000u64);
        let mut txn = kx_init();
        // Opening is refused until the ledger is empty.
        assert!(kx_stage_open(&mut txn, 0x40_0000, 2, KEXEC_MAX_SEGMENTS as u64, false).is_ok());
        assert_eq!(
            kx_stage_open(&mut txn, 0x40_0000, 2, KEXEC_MAX_SEGMENTS as u64, false),
            Err(-EINVAL)
        );
        let i0 = kx_stage_check(&mut txn, 0x1000, 0x100, 0x100_0000, 0x1000,
                                kernel.0, kernel.1).unwrap();
        assert_eq!(i0, 0);
        // Overlap with the first staged segment is refused and reserves nothing.
        assert_eq!(
            kx_stage_check(&mut txn, 0, 0, 0x100_0000 + 0x800, 0x1000, kernel.0, kernel.1),
            Err(-EADDRNOTAVAIL)
        );
        assert_eq!(txn.nstaged, 1);
        let i1 = kx_stage_check(&mut txn, 0, 0, 0x200_0000, 0x40, kernel.0, kernel.1).unwrap();
        assert_eq!(i1, 1);
        assert!(kx_stage_pages(&mut txn, i0, 0xAAAA_0000, 0x1000).is_ok());
        assert!(kx_stage_pages(&mut txn, i1, 0xBBBB_0000, 0x1000).is_ok());
        // Publishing before every promised segment is staged is refused.
        let mut txn2 = kx_init();
        assert!(kx_stage_open(&mut txn2, 0, 2, KEXEC_MAX_SEGMENTS as u64, false).is_ok());
        assert!(kx_stage_check(&mut txn2, 0, 0, 0x300_0000, 0x10, kernel.0, kernel.1).is_ok());
        let mut img = kx_init();
        assert_eq!(kx_publish(&mut img, &mut txn2), Err(-EINVAL));
        assert!(!kx_loaded(&img));

        let mut img2 = kx_init();
        assert!(kx_publish(&mut img2, &mut txn).is_ok());
        assert!(kx_loaded(&img2));
        // Publishing consumes the staging ledger, so it cannot be reused.
        assert!(!kx_loaded(&txn) && txn.state == KX_EMPTY && txn.nstaged == 0);
        assert_eq!(kx_publish(&mut img, &mut txn), Err(-EINVAL));
    }

    #[test]
    fn kexec_ledger_release_is_exactly_once() {
        // A failed multi-segment load must hand back exactly the chains it
        // attached, newest first, and nothing twice.
        let kernel = (0x10_0000u64, 0x20_0000u64);
        let mut txn = kx_init();
        assert!(kx_stage_open(&mut txn, 0, 3, KEXEC_MAX_SEGMENTS as u64, false).is_ok());
        let a = kx_stage_check(&mut txn, 0, 0, 0x100_0000, 0x1000, kernel.0, kernel.1).unwrap();
        kx_stage_pages(&mut txn, a, 0x1_0000, 0x1000).unwrap();
        // A zero-length segment reserves a slot but attaches no chain.
        let z = kx_stage_check(&mut txn, 0, 0, 0x200_0000, 0, kernel.0, kernel.1).unwrap();
        kx_stage_pages(&mut txn, z, 0, 0).unwrap();
        let c = kx_stage_check(&mut txn, 0, 0, 0x300_0000, 0x2000, kernel.0, kernel.1).unwrap();
        kx_stage_pages(&mut txn, c, 0x3_0000, 0x2000).unwrap();

        let mut got = Vec::new();
        while let Some((pages, bytes)) = kx_release_next(&mut txn) {
            got.push((pages, bytes));
        }
        assert_eq!(got, vec![(0x3_0000, 0x2000), (0, 0), (0x1_0000, 0x1000)]);
        assert_eq!(txn.state, KX_EMPTY);
        assert!(kx_release_next(&mut txn).is_none());

        // The same drain releases a published image exactly once.
        let mut img = kx_init();
        let mut src = kx_init();
        kx_stage_open(&mut src, 0x40_0000, 1, KEXEC_MAX_SEGMENTS as u64, true).unwrap();
        let i = kx_stage_file(&mut src, 0x1000).unwrap();
        kx_stage_pages(&mut src, i, 0x9_0000, 0x1000).unwrap();
        assert!(kx_publish(&mut img, &mut src).is_ok());
        assert!(kx_loaded(&img));
        assert_eq!(img.file_backed, 1);
        assert_eq!(img.entry, 0x40_0000);
        assert_eq!(kx_release_next(&mut img), Some((0x9_0000, 0x1000)));
        assert!(kx_release_next(&mut img).is_none());
        assert!(!kx_loaded(&img) && img.state == KX_EMPTY);
    }

    #[test]
    fn ffi_step_and_interp_classification() {
        assert_eq!(a20_elf_classify(0), STEP_INSTALL);
        assert_eq!(a20_elf_classify(-ENOEXEC), 1);
        assert_eq!(a20_elf_classify(-EACCES), STEP_FAIL);
        assert_eq!(a20_elf_step_after_script(1), STEP_RETRY);
        assert_eq!(a20_elf_step_after_script(0), STEP_CONTINUE);
        assert_eq!(a20_elf_step_after_script(-ENOMEM), STEP_FAIL);
        assert_eq!(a20_elf_step_after_xlator(false, 1), STEP_FAIL);
        assert_eq!(a20_elf_step_after_xlator(true, 1), STEP_RETRY);
        assert_eq!(a20_elf_step_after_xlator(true, 0), STEP_FAIL);
        assert_eq!(a20_elf_interp_attempts() as usize, INTERP_ATTEMPTS);
        assert_eq!(a20_elf_interp_resolution(0, true), 0);
        assert_eq!(a20_elf_interp_resolution(4, true), 4);
        assert_eq!(a20_elf_interp_resolution(2, false), -1);
    }

    #[test]
    fn ffi_reloc_and_header_helpers() {
        let eh = ehdr64(ET_EXEC, MACH, 0x1000, 64, 1, 56);
        let mut facts = Ehdr::default();
        assert_eq!(
            unsafe { a20_elf_parse_header(eh.as_ptr(), eh.len(), &mut facts) },
            0
        );
        assert_eq!(facts.e_machine, MACH);
        assert_eq!(facts.e_phoff, 64);
        assert_eq!(
            unsafe { a20_elf_parse_header(eh[..10].as_ptr(), 10, &mut facts) },
            -ENOEXEC
        );

        let mut types = [0u32; 2];
        assert_eq!(
            unsafe { a20_elf_reloc_types(EM_RISCV, true, types.as_mut_ptr()) },
            0
        );
        assert_eq!(types, [3, 2]);

        assert_eq!(
            a20_elf_reloc_action(3, 0, 1, 0, 1, types[0], types[1]),
            RELOC_ACT_RELATIVE
        );
        assert_eq!(
            a20_elf_reloc_action(2, 9, 4, 0x10, 1, types[0], types[1]),
            -ENOEXEC
        );

        let mut sysv = vec![0u8; 16];
        sysv[4..8].copy_from_slice(&9u32.to_le_bytes());
        assert_eq!(
            unsafe { a20_elf_dynsym_count(sysv.as_ptr(), sysv.len(), core::ptr::null(), 0) },
            9
        );

        let mut argv_plan = ShebangArgvPlan::default();
        assert_eq!(a20_elf_plan_shebang_argv(true, 3, MAX_ARG_STRINGS, &mut argv_plan), 0);
        assert_eq!(argv_plan.new_argc, 5);
        assert_eq!(argv_plan.script_pos, 2);

        // check_arg_bytes FFI: totals pass through, errors are negative.
        assert_eq!(a20_elf_check_arg_bytes(0, 99, 100, 1000, 1000), 99);
        assert_eq!(a20_elf_check_arg_bytes(0, 100, 100, 1000, 1000), -E2BIG as i64);
        assert_eq!(a20_elf_check_arg_bytes(950, 60, 4096, 1000, 0), -E2BIG as i64);
        assert_eq!(a20_elf_check_arg_bytes(950, 60, 4096, 2000, 0), 1010);
    }

    #[test]
    fn shebang_parse() {
        let p = plan_shebang(b"#!/bin/sh -e\n").unwrap();
        assert_eq!(p.is_script, 1);
        assert_eq!(&b"#!/bin/sh -e\n"[p.interp_off as usize..(p.interp_off + p.interp_len) as usize], b"/bin/sh");
        assert_eq!(p.arg_len, 2);
        assert_eq!(plan_shebang(b"not a script").unwrap().is_script, 0);
        assert_eq!(plan_shebang(b"#!\n").unwrap_err(), -ENOEXEC);
    }

    #[test]
    fn foreign_probe() {
        let eh = ehdr64(ET_EXEC, EM_X86_64, 0, 64, 1, 56);
        let ph = phdr64(PT_LOAD, PF_R, 0, 0x1000, 0x100, 0x100, 0x1000);
        let k = probe_foreign(&eh, &ph, CLASS, MACH).unwrap();
        assert_eq!(k.machine, EM_X86_64);
        assert_eq!(k.is_foreign, 1);
        // Native machine is not foreign.
        let native = ehdr64(ET_EXEC, MACH, 0, 64, 1, 56);
        assert_eq!(probe_foreign(&native, &ph, CLASS, MACH), Err(-ENOEXEC));
    }

    #[test]
    fn elf32_header_and_segment() {
        let mut b = vec![0u8; EHDR32_SIZE];
        b[0..4].copy_from_slice(&[0x7f, b'E', b'L', b'F']);
        b[4] = ELFCLASS32;
        b[5] = ELFDATA2LSB;
        b[16..18].copy_from_slice(&ET_EXEC.to_le_bytes());
        b[18..20].copy_from_slice(&EM_ARM.to_le_bytes());
        b[24..28].copy_from_slice(&0x8000u32.to_le_bytes());
        b[28..32].copy_from_slice(&52u32.to_le_bytes());
        b[42..44].copy_from_slice(&32u16.to_le_bytes());
        b[44..46].copy_from_slice(&1u16.to_le_bytes());
        let mut p = vec![0u8; PHDR32_SIZE];
        p[0..4].copy_from_slice(&PT_LOAD.to_le_bytes());
        p[8..12].copy_from_slice(&0x8000u32.to_le_bytes());
        p[16..20].copy_from_slice(&0x100u32.to_le_bytes());
        p[20..24].copy_from_slice(&0x200u32.to_le_bytes());
        p[24..28].copy_from_slice(&(PF_R | PF_X).to_le_bytes());
        p[28..32].copy_from_slice(&0x1000u32.to_le_bytes());
        let plan = plan_image(&b, &p, ELFCLASS32, EM_ARM, MM_WX_OFF, false).unwrap();
        assert_eq!(plan.nseg, 1);
        assert_eq!(plan.segments[0].prot, PROT_READ | PROT_EXEC);
        assert_eq!(plan.segments[0].anon_tail, 1);
    }

    #[test]
    fn bprm_limit_math() {
        let l = BprmLimits { max_strlen: 100, max_bytes: 1000, stack_quarter: 1000 };
        assert_eq!(check_arg_bytes(0, 99, &l), Ok(99));
        assert_eq!(check_arg_bytes(0, 100, &l), Err(-E2BIG));
        assert_eq!(check_arg_bytes(950, 60, &l), Err(-E2BIG));
        let l2 = BprmLimits { max_strlen: 100, max_bytes: 0, stack_quarter: 40 };
        assert_eq!(check_arg_bytes(31, 10, &l2), Err(-E2BIG));
        assert_eq!(check_arg_bytes(30, 10, &l2), Ok(40));
    }

    #[test]
    fn exec_loop_decision() {
        assert_eq!(classify_elf(0), ElfOutcome::Loaded);
        assert_eq!(classify_elf(-ENOEXEC), ElfOutcome::FormatMismatch);
        assert_eq!(classify_elf(-EACCES), ElfOutcome::Failed(-EACCES));
        assert_eq!(step_after_elf(ElfOutcome::Loaded), Step::Install);
        assert_eq!(step_after_elf(ElfOutcome::FormatMismatch), Step::Continue);
        assert_eq!(step_after_elf(ElfOutcome::Failed(-EIO)), Step::Fail(-EIO));
        assert_eq!(step_after_script(1), Step::Retry);
        assert_eq!(step_after_script(0), Step::Continue);
        assert_eq!(step_after_script(-ENOMEM), Step::Fail(-ENOMEM));
        assert_eq!(step_after_xlator(true, 1), Step::Retry);
        assert_eq!(step_after_xlator(false, 1), Step::Fail(-ENOEXEC));
        assert_eq!(step_after_xlator(true, 0), Step::Fail(-ENOEXEC));
        assert!(depth_exhausted(4, 4));
        assert!(!depth_exhausted(3, 4));
    }

    #[test]
    fn shebang_argv_plan() {
        let p = plan_shebang_argv(false, 3, MAX_ARG_STRINGS);
        assert_eq!(p.new_argc, 4);
        assert_eq!(p.script_pos, 1);
        let p2 = plan_shebang_argv(true, 1, MAX_ARG_STRINGS);
        assert_eq!(p2.new_argc, 3);
        assert_eq!(p2.script_pos, 2);
        let p3 = plan_shebang_argv(false, 10, 4);
        assert_eq!(p3.new_argc, 4);
    }

    // ---- relocation apply: bounds against the PT_LOAD map -----------

    fn dyn_arr(pairs: &[(i64, u64)]) -> Vec<u8> {
        let mut v = Vec::new();
        for (t, val) in pairs {
            v.extend_from_slice(&t.to_le_bytes());
            v.extend_from_slice(&val.to_le_bytes());
        }
        v
    }

    fn dyn_arr32(pairs: &[(i32, u32)]) -> Vec<u8> {
        let mut v = Vec::new();
        for (t, val) in pairs {
            v.extend_from_slice(&t.to_le_bytes());
            v.extend_from_slice(&val.to_le_bytes());
        }
        v
    }

    fn rela64(r_offset: u64, r_type: u32, sym: u32, addend: i64) -> Vec<u8> {
        let mut v = Vec::new();
        v.extend_from_slice(&r_offset.to_le_bytes());
        v.extend_from_slice(&(((sym as u64) << 32) | r_type as u64).to_le_bytes());
        v.extend_from_slice(&addend.to_le_bytes());
        v
    }

    fn rela32(r_offset: u32, r_type: u32, sym: u32, addend: i32) -> Vec<u8> {
        let mut v = Vec::new();
        v.extend_from_slice(&r_offset.to_le_bytes());
        v.extend_from_slice(&((sym << 8) | (r_type & 0xff)).to_le_bytes());
        v.extend_from_slice(&addend.to_le_bytes());
        v
    }

    fn sym64(info: u8, shndx: u16, value: u64) -> Vec<u8> {
        let mut v = vec![0u8; 24];
        v[4] = info;
        v[6..8].copy_from_slice(&shndx.to_le_bytes());
        v[8..16].copy_from_slice(&value.to_le_bytes());
        v
    }

    fn sym32(info: u8, shndx: u16, value: u32) -> Vec<u8> {
        let mut v = vec![0u8; 16];
        v[4..8].copy_from_slice(&value.to_le_bytes());
        v[12] = info;
        v[14..16].copy_from_slice(&shndx.to_le_bytes());
        v
    }

    fn put(img: &mut [u8], at: usize, bytes: &[u8]) {
        img[at..at + bytes.len()].copy_from_slice(bytes);
    }

    fn sysv_hash(nchain: u32) -> Vec<u8> {
        let mut v = vec![0u8; 8];
        v[4..8].copy_from_slice(&nchain.to_le_bytes());
        v
    }

    /// Two PT_LOADs (RO at 0, RW at 0x1000) and a PT_DYNAMIC at 0x100.
    fn reloc_image_plan64() -> ImagePlan {
        let eh = ehdr64(ET_DYN, MACH, 0, 64, 3, 56);
        let ro = phdr64(PT_LOAD, PF_R, 0, 0x0, 0x1000, 0x1000, 0x1000);
        let rw = phdr64(PT_LOAD, PF_R | PF_W, 0, 0x1000, 0x1000, 0x1000, 0x1000);
        let dynseg = phdr64(PT_DYNAMIC, PF_R | PF_W, 0, 0x100, 0x100, 0x100, 8);
        plan_image(&eh, &concat(&[ro, rw, dynseg]), CLASS, MACH, MM_WX_OFF, false).unwrap()
    }

    fn ehdr32(e_type: u16, machine: u16, entry: u32, phoff: u32, phnum: u16, phentsize: u16) -> Vec<u8> {
        let mut b = vec![0u8; EHDR32_SIZE];
        b[0..4].copy_from_slice(&[0x7f, b'E', b'L', b'F']);
        b[4] = ELFCLASS32;
        b[5] = ELFDATA2LSB;
        b[6] = 1;
        b[16..18].copy_from_slice(&e_type.to_le_bytes());
        b[18..20].copy_from_slice(&machine.to_le_bytes());
        b[20..24].copy_from_slice(&1u32.to_le_bytes());
        b[24..28].copy_from_slice(&entry.to_le_bytes());
        b[28..32].copy_from_slice(&phoff.to_le_bytes());
        b[40..42].copy_from_slice(&(EHDR32_SIZE as u16).to_le_bytes());
        b[42..44].copy_from_slice(&phentsize.to_le_bytes());
        b[44..46].copy_from_slice(&phnum.to_le_bytes());
        b
    }

    fn phdr32(p_type: u32, flags: u32, off: u32, vaddr: u32, filesz: u32, memsz: u32, align: u32) -> Vec<u8> {
        let mut b = vec![0u8; PHDR32_SIZE];
        b[0..4].copy_from_slice(&p_type.to_le_bytes());
        b[4..8].copy_from_slice(&off.to_le_bytes());
        b[8..12].copy_from_slice(&vaddr.to_le_bytes());
        b[16..20].copy_from_slice(&filesz.to_le_bytes());
        b[20..24].copy_from_slice(&memsz.to_le_bytes());
        b[24..28].copy_from_slice(&flags.to_le_bytes());
        b[28..32].copy_from_slice(&align.to_le_bytes());
        b
    }

    fn reloc_image_plan32() -> ImagePlan {
        let eh = ehdr32(ET_DYN, EM_ARM, 0, 52, 3, 32);
        let ro = phdr32(PT_LOAD, PF_R, 0, 0x0, 0x1000, 0x1000, 0x1000);
        let rw = phdr32(PT_LOAD, PF_R | PF_W, 0, 0x1000, 0x1000, 0x1000, 0x1000);
        let dynseg = phdr32(PT_DYNAMIC, PF_R | PF_W, 0, 0x100, 0x100, 0x100, 4);
        plan_image(&eh, &concat(&[ro, rw, dynseg]), ELFCLASS32, EM_ARM, MM_WX_OFF, false).unwrap()
    }

    #[test]
    fn reloc_apply_relative_and_absolute() {
        let plan = reloc_image_plan64();
        let mut img = vec![0u8; 0x2000];
        put(&mut img, 0x100, &dyn_arr(&[
            (DT_RELA, 0x200),
            (DT_RELASZ, 48),
            (DT_RELAENT, 24),
            (DT_SYMTAB, 0x400),
            (DT_SYMENT, 24),
            (DT_HASH, 0x600),
            (DT_NULL, 0),
        ]));
        put(&mut img, 0x200, &rela64(0x1000, 3, 0, 0x1234));
        put(&mut img, 0x218, &rela64(0x1008, 2, 0, 8));
        put(&mut img, 0x400, &sym64(0x10, 1, 0x40));
        put(&mut img, 0x600, &sysv_hash(1));

        let n = apply_relocs(&plan, 0, &mut img, true).unwrap();
        assert_eq!(n, 2);
        assert_eq!(u64::from_le_bytes(img[0x1000..0x1008].try_into().unwrap()), 0x1234);
        assert_eq!(u64::from_le_bytes(img[0x1008..0x1010].try_into().unwrap()), 0x48);
    }

    #[test]
    fn reloc_apply_32bit_relative_and_absolute() {
        let plan = reloc_image_plan32();
        let mut img = vec![0u8; 0x2000];
        put(&mut img, 0x100, &dyn_arr32(&[
            (DT_RELA as i32, 0x200),
            (DT_RELASZ as i32, 24),
            (DT_RELAENT as i32, 12),
            (DT_SYMTAB as i32, 0x400),
            (DT_SYMENT as i32, 16),
            (DT_HASH as i32, 0x600),
            (DT_NULL as i32, 0),
        ]));
        put(&mut img, 0x200, &rela32(0x1000, 23, 0, 0x55));
        put(&mut img, 0x20c, &rela32(0x1004, 2, 0, 4));
        put(&mut img, 0x400, &sym32(0x10, 1, 0x30));
        put(&mut img, 0x600, &sysv_hash(1));

        let n = apply_relocs(&plan, 0, &mut img, false).unwrap();
        assert_eq!(n, 2);
        assert_eq!(u32::from_le_bytes(img[0x1000..0x1004].try_into().unwrap()), 0x55);
        assert_eq!(u32::from_le_bytes(img[0x1004..0x1008].try_into().unwrap()), 0x34);
    }

    #[test]
    fn reloc_rejects_dynamic_outside_mapping() {
        let mut plan = reloc_image_plan64();
        plan.dynamic.vaddr = 0x8000;
        let mut img = vec![0u8; 0x2000];
        assert_eq!(apply_relocs(&plan, 0, &mut img, true), Err(-ENOEXEC));
    }

    #[test]
    fn reloc_rejects_dynamic_crossing_segment_end() {
        let mut plan = reloc_image_plan64();
        plan.dynamic.vaddr = 0xf00;
        plan.dynamic.memsz = 0x200; // would run past the 0x1000 end of PT_LOAD 0
        let mut img = vec![0u8; 0x2000];
        assert_eq!(apply_relocs(&plan, 0, &mut img, true), Err(-ENOEXEC));
    }

    #[test]
    fn reloc_rejects_readonly_destination() {
        let plan = reloc_image_plan64();
        let mut img = vec![0u8; 0x2000];
        put(&mut img, 0x100, &dyn_arr(&[
            (DT_RELA, 0x200),
            (DT_RELASZ, 24),
            (DT_SYMTAB, 0x400),
            (DT_SYMENT, 24),
            (DT_NULL, 0),
        ]));
        put(&mut img, 0x200, &rela64(0x800, 3, 0, 1)); // 0x800 is read-only
        assert_eq!(apply_relocs(&plan, 0, &mut img, true), Err(-ENOEXEC));
    }

    #[test]
    fn reloc_rejects_destination_outside_image() {
        let plan = reloc_image_plan64();
        let mut img = vec![0u8; 0x2000];
        put(&mut img, 0x100, &dyn_arr(&[
            (DT_RELA, 0x200),
            (DT_RELASZ, 24),
            (DT_SYMTAB, 0x400),
            (DT_SYMENT, 24),
            (DT_NULL, 0),
        ]));
        put(&mut img, 0x200, &rela64(0x5000, 3, 0, 1));
        assert_eq!(apply_relocs(&plan, 0, &mut img, true), Err(-ENOEXEC));
    }

    #[test]
    fn reloc_rejects_offset_overflow() {
        let plan = reloc_image_plan64();
        let mut img = vec![0u8; 0x2000];
        put(&mut img, 0x100, &dyn_arr(&[
            (DT_RELA, 0x200),
            (DT_RELASZ, 24),
            (DT_SYMTAB, 0x400),
            (DT_SYMENT, 24),
            (DT_NULL, 0),
        ]));
        put(&mut img, 0x200, &rela64(u64::MAX, 3, 0, 1));
        assert_eq!(apply_relocs(&plan, 0, &mut img, true), Err(-ENOEXEC));
    }

    #[test]
    fn reloc_rejects_rela_table_crossing_segment() {
        let plan = reloc_image_plan64();
        let mut img = vec![0u8; 0x2000];
        put(&mut img, 0x100, &dyn_arr(&[
            (DT_RELA, 0xfe0),
            (DT_RELASZ, 0x100), // 0xfe0 + 0xf0 passes the 0x1000 segment end
            (DT_SYMTAB, 0x400),
            (DT_SYMENT, 24),
            (DT_NULL, 0),
        ]));
        assert_eq!(apply_relocs(&plan, 0, &mut img, true), Err(-ENOEXEC));
    }

    #[test]
    fn reloc_rejects_hash_outside_mapping() {
        let plan = reloc_image_plan64();
        let mut img = vec![0u8; 0x2000];
        put(&mut img, 0x100, &dyn_arr(&[
            (DT_RELA, 0x200),
            (DT_RELASZ, 24),
            (DT_SYMTAB, 0x400),
            (DT_SYMENT, 24),
            (DT_HASH, 0x8000),
            (DT_NULL, 0),
        ]));
        put(&mut img, 0x200, &rela64(0x1000, 3, 0, 1));
        assert_eq!(apply_relocs(&plan, 0, &mut img, true), Err(-ENOEXEC));
    }

    #[test]
    fn reloc_rejects_symidx_beyond_hash_bound() {
        let plan = reloc_image_plan64();
        let mut img = vec![0u8; 0x2000];
        put(&mut img, 0x100, &dyn_arr(&[
            (DT_RELA, 0x200),
            (DT_RELASZ, 24),
            (DT_SYMTAB, 0x400),
            (DT_SYMENT, 24),
            (DT_HASH, 0x600),
            (DT_NULL, 0),
        ]));
        put(&mut img, 0x200, &rela64(0x1000, 2, 3, 0)); // symidx 3, nchain 1
        put(&mut img, 0x400, &sym64(0x10, 1, 0x40));
        put(&mut img, 0x600, &sysv_hash(1));
        assert_eq!(apply_relocs(&plan, 0, &mut img, true), Err(-ENOEXEC));
    }

    #[test]
    fn reloc_rejects_undefined_strong_symbol() {
        let plan = reloc_image_plan64();
        let mut img = vec![0u8; 0x2000];
        put(&mut img, 0x100, &dyn_arr(&[
            (DT_RELA, 0x200),
            (DT_RELASZ, 24),
            (DT_SYMTAB, 0x400),
            (DT_SYMENT, 24),
            (DT_HASH, 0x600),
            (DT_NULL, 0),
        ]));
        put(&mut img, 0x200, &rela64(0x1000, 2, 0, 0));
        put(&mut img, 0x400, &sym64(0x10, 0, 0x40)); // undefined, GLOBAL
        put(&mut img, 0x600, &sysv_hash(1));
        assert_eq!(apply_relocs(&plan, 0, &mut img, true), Err(-ENOEXEC));
    }

    #[test]
    fn reloc_skips_unrelated_types_and_accepts_weak_undefined() {
        let plan = reloc_image_plan64();
        let mut img = vec![0u8; 0x2000];
        put(&mut img, 0x100, &dyn_arr(&[
            (DT_RELA, 0x200),
            (DT_RELASZ, 48),
            (DT_SYMTAB, 0x400),
            (DT_SYMENT, 24),
            (DT_HASH, 0x600),
            (DT_NULL, 0),
        ]));
        put(&mut img, 0x200, &rela64(0x1000, 99, 0, 0x77)); // unknown type: skip
        put(&mut img, 0x218, &rela64(0x1008, 2, 0, 4));      // ABS, undefined weak
        put(&mut img, 0x400, &sym64(0x20, 0, 0xdead));       // WEAK, undefined
        put(&mut img, 0x600, &sysv_hash(1));
        let n = apply_relocs(&plan, 0, &mut img, true).unwrap();
        assert_eq!(n, 1);
        assert_eq!(u64::from_le_bytes(img[0x1000..0x1008].try_into().unwrap()), 0);
        assert_eq!(u64::from_le_bytes(img[0x1008..0x1010].try_into().unwrap()), 4);
    }

    #[test]
    fn reloc_gnu_hash_walk_is_clamped_to_segment() {
        // DT_GNU_HASH near the end of the readable segment with absurd
        // bucket/bloom counts: the walk must stay inside the segment window
        // and must not read past the image.
        let plan = reloc_image_plan64();
        let mut img = vec![0u8; 0x2000];
        put(&mut img, 0x100, &dyn_arr(&[
            (DT_RELA, 0x200),
            (DT_RELASZ, 24),
            (DT_SYMTAB, 0x400),
            (DT_SYMENT, 24),
            (DT_GNU_HASH, 0xfe0),
            (DT_NULL, 0),
        ]));
        put(&mut img, 0x200, &rela64(0x1000, 3, 0, 1));
        // nbuckets = 0xffffffff, symoffset = 0, bloom_words = 0xffffffff
        let mut gnu = vec![0u8; 8];
        gnu[0..4].copy_from_slice(&0xffff_ffffu32.to_le_bytes());
        gnu[4..8].copy_from_slice(&0u32.to_le_bytes());
        put(&mut img, 0xfe0, &gnu);
        assert_eq!(apply_relocs(&plan, 0, &mut img, true), Ok(1));
    }
}
