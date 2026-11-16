#![no_std]

//! ELF core-dump layout.
//!
//! Rust owns what the core file looks like: the `core_pattern` expansion, the
//! VM-flag to `p_flags` mapping, the note blob (`NT_PRSTATUS`, `NT_FPREGSET`,
//! `NT_PRPSINFO` with their four-byte framing), the program-header table and
//! the `RLIMIT_CORE` clamping that decides how much of each segment is written.
//! C keeps the mechanical I/O it already owned: the `core_pattern` storage and
//! its lock, the VMA snapshot under `mm->lock`, `kmalloc`/`kfree`, the VFS
//! open/write/lseek/fsync path and `copy_from_user` of the dumped pages.
//!
//! The wire structures are `repr(C)` mirrors of the ELF64 and Linux
//! `elf_prstatus`/`elf_prpsinfo` layouts; `kernel/proc/coredump_layout.h` holds
//! the matching C definitions and the size assertions on both sides.

#[cfg(test)]
extern crate std;

pub const COREDUMP_EHDR_SIZE: usize = 64;
pub const COREDUMP_PHDR_SIZE: usize = 56;

pub const CORE_ET_CORE: u16 = 4;
pub const CORE_PT_LOAD: u32 = 1;
pub const CORE_PT_NOTE: u32 = 4;
pub const CORE_PF_X: u32 = 1;
pub const CORE_PF_W: u32 = 2;
pub const CORE_PF_R: u32 = 4;

pub const CORE_NT_PRSTATUS: u32 = 1;
pub const CORE_NT_FPREGSET: u32 = 2;
pub const CORE_NT_PRPSINFO: u32 = 3;

pub const SI_KERNEL: i32 = 0x80;

pub const VM_READ: u64 = 0x0000_0001;
pub const VM_WRITE: u64 = 0x0000_0002;
pub const VM_EXEC: u64 = 0x0000_0004;

const ENOMEM: i32 = -12;

#[repr(C)]
#[derive(Clone, Copy)]
struct Ehdr {
    ident: [u8; 16],
    e_type: u16,
    e_machine: u16,
    e_version: u32,
    e_entry: u64,
    e_phoff: u64,
    e_shoff: u64,
    e_flags: u32,
    e_ehsize: u16,
    e_phentsize: u16,
    e_phnum: u16,
    e_shentsize: u16,
    e_shnum: u16,
    e_shstrndx: u16,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct Phdr {
    p_type: u32,
    p_flags: u32,
    p_offset: u64,
    p_vaddr: u64,
    p_paddr: u64,
    p_filesz: u64,
    p_memsz: u64,
    p_align: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct Prstatus<const N: usize> {
    si_signo: i32,
    si_code: i32,
    si_errno: i32,
    pr_cursig: i16,
    _pad0: u16,
    pr_sigpend: u64,
    pr_sighold: u64,
    pr_pid: i32,
    pr_ppid: i32,
    pr_pgrp: i32,
    pr_sid: i32,
    pr_utime: [u64; 2],
    pr_stime: [u64; 2],
    pr_cutime: [u64; 2],
    pr_cstime: [u64; 2],
    pr_reg: [u64; N],
    pr_fpvalid: i32,
    _pad1: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct Fpregset {
    f: [u64; 32],
    fcsr: u32,
    _pad: u32,
}

/// Everything the note blob needs, read from the crashing task by the adapter.
#[repr(C)]
pub struct NoteInput {
    pub sig: i32,
    pub pid: i32,
    pub ppid: i32,
    pub pgid: i32,
    pub sid: i32,
    pub uid: u32,
    pub gid: u32,
    pub regs: [u64; 32],
    pub fp: [u64; 32],
    pub fcsr: u64,
    pub name: [u8; 64],
    pub pc: u64,
    pub sp: u64,
    pub status: u64,
    pub orig_syscall: u64,
}

/// One dumped VMA, snapshotted by the adapter under `mm->lock`.
#[repr(C)]
pub struct VmaInput {
    pub start: u64,
    pub end: u64,
    pub flags: u64,
}

/// Layout results the adapter needs for the write phase.
#[repr(C)]
pub struct LayoutResult {
    pub notes_len: u64,
    pub notes_off: u64,
    pub data_off: u64,
    pub phnum: u16,
}

/// Where one dumped segment's bytes go, so the adapter does not have to parse
/// the program headers it just asked Rust to lay out.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct SegmentOut {
    pub offset: u64,
    pub filesz: u64,
}

const _: () = {
    assert!(core::mem::size_of::<Ehdr>() == COREDUMP_EHDR_SIZE);
    assert!(core::mem::size_of::<Phdr>() == COREDUMP_PHDR_SIZE);
    assert!(core::mem::size_of::<NoteInput>() == 648);
};

struct Blob {
    buf: *mut u8,
    cap: usize,
    len: usize,
    overflow: bool,
}

impl Blob {
    fn put(&mut self, data: &[u8]) {
        if self.len + data.len() > self.cap {
            self.overflow = true;
            return;
        }
        unsafe {
            core::ptr::copy_nonoverlapping(data.as_ptr(), self.buf.add(self.len), data.len());
        }
        self.len += data.len();
    }

    fn pad4(&mut self) {
        let pad = (4 - (self.len & 3)) & 3;
        if pad != 0 {
            self.put(&[0u8; 3][..pad]);
        }
    }

    /// One ELF note: header, NUL-terminated name, description, four-byte pads.
    fn note(&mut self, name: &[u8], ntype: u32, desc: &[u8]) {
        let namesz = (name.len() + 1) as u32;
        let mut hdr = [0u8; 12];
        hdr[0..4].copy_from_slice(&namesz.to_ne_bytes());
        hdr[4..8].copy_from_slice(&(desc.len() as u32).to_ne_bytes());
        hdr[8..12].copy_from_slice(&ntype.to_ne_bytes());
        self.put(&hdr);
        self.put(name);
        self.put(&[0u8]); // the name's NUL is part of namesz
        self.pad4();
        self.put(desc);
        self.pad4();
    }
}

fn vma_pflags(vm_flags: u64) -> u32 {
    let mut f = 0;
    if vm_flags & VM_READ != 0 {
        f |= CORE_PF_R;
    }
    if vm_flags & VM_WRITE != 0 {
        f |= CORE_PF_W;
    }
    if vm_flags & VM_EXEC != 0 {
        f |= CORE_PF_X;
    }
    f
}

fn align_up(v: u64, a: u64) -> u64 {
    (v + a - 1) & !(a - 1)
}

fn align_down(v: u64, a: u64) -> u64 {
    v & !(a - 1)
}

/// Decimal `i32` without `core::fmt`, appended to `out[..limit]`.  `limit`
/// leaves room for the caller's terminating NUL.
fn put_i32(out: &mut [u8], len: usize, value: i32, limit: usize) -> usize {
    let mut n = len;
    let mut mag: u32;
    if value < 0 {
        if n < limit {
            out[n] = b'-';
            n += 1;
        }
        mag = (-(value as i64)) as u32;
    } else {
        mag = value as u32;
    }
    let mut digits = [0u8; 10];
    let mut count = 0;
    loop {
        digits[count] = b'0' + (mag % 10) as u8;
        count += 1;
        mag /= 10;
        if mag == 0 {
            break;
        }
    }
    while count > 0 && n < limit {
        count -= 1;
        out[n] = digits[count];
        n += 1;
    }
    n
}

fn put_u64(out: &mut [u8], len: usize, value: u64, limit: usize) -> usize {
    let mut n = len;
    let mut digits = [0u8; 20];
    let mut count = 0;
    let mut mag = value;
    loop {
        digits[count] = b'0' + (mag % 10) as u8;
        count += 1;
        mag /= 10;
        if mag == 0 {
            break;
        }
    }
    while count > 0 && n < limit {
        count -= 1;
        out[n] = digits[count];
        n += 1;
    }
    n
}

/// Copy a NUL-terminated name into `out`, stopping at `max` bytes.
fn put_name(out: &mut [u8], len: usize, name: &[u8], max: usize, limit: usize) -> usize {
    let mut n = len;
    let mut written = 0;
    for &byte in name {
        if byte == 0 || written >= max || n >= limit {
            break;
        }
        out[n] = byte;
        n += 1;
        written += 1;
    }
    n
}

/// Expand the `%p %e %s %u %g %t %%` subset.  An unsupported placeholder is
/// dropped, matching the documented subset boundary.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_coredump_expand_pattern(
    pattern: *const u8,
    name: *const u8,
    pid: i32,
    tgid: i32,
    uid: i32,
    gid: i32,
    sig: i32,
    seconds: u64,
    out: *mut u8,
    outsz: usize,
) -> i32 {
    if pattern.is_null() || out.is_null() || outsz == 0 {
        return -22;
    }
    let mut buf = unsafe { core::slice::from_raw_parts_mut(out, outsz) };
    let name_slice = if name.is_null() {
        &[][..]
    } else {
        unsafe { core::slice::from_raw_parts(name, 64) }
    };
    let limit = buf.len() - 1; // keep room for the terminating NUL
    let mut o = 0usize;
    let mut i = 0usize;
    loop {
        let c = unsafe { *pattern.add(i) };
        if c == 0 || o >= limit {
            break;
        }
        if c != b'%' {
            buf[o] = c;
            o += 1;
            i += 1;
            continue;
        }
        let next = unsafe { *pattern.add(i + 1) };
        if next == 0 {
            buf[o] = c;
            o += 1;
            i += 1;
            continue;
        }
        i += 2;
        match next {
            b'%' => {
                buf[o] = b'%';
                o += 1;
            }
            b'p' => o = put_i32(&mut buf, o, if tgid > 0 { tgid } else { pid }, limit),
            // The C reference formats %e through a 32-byte scratch buffer, so
            // a longer name is truncated to 31 bytes; keep that behaviour.
            b'e' => o = put_name(&mut buf, o, name_slice, 31, limit),
            b's' => o = put_i32(&mut buf, o, sig, limit),
            b'u' => o = put_i32(&mut buf, o, uid, limit),
            b'g' => o = put_i32(&mut buf, o, gid, limit),
            b't' => o = put_u64(&mut buf, o, seconds, limit),
            _ => {}
        }
    }
    buf[o] = 0;
    0
}

fn emit_prstatus<const N: usize>(blob: &mut Blob, input: &NoteInput,
                                regs: [u64; N], fpvalid: i32) {
    let mut prs: Prstatus<N> = unsafe { core::mem::zeroed() };
    prs.si_signo = input.sig;
    prs.si_code = SI_KERNEL;
    prs.pr_cursig = input.sig as i16;
    prs.pr_pid = input.pid;
    prs.pr_ppid = input.ppid;
    prs.pr_pgrp = input.pgid;
    prs.pr_sid = input.sid;
    prs.pr_reg = regs;
    prs.pr_fpvalid = fpvalid;
    let prs_bytes = unsafe {
        core::slice::from_raw_parts(&prs as *const Prstatus<N> as *const u8,
                                    core::mem::size_of::<Prstatus<N>>())
    };
    blob.note(b"CORE", CORE_NT_PRSTATUS, prs_bytes);
}

fn emit_prpsinfo(blob: &mut Blob, input: &NoteInput, wide_flag: bool) {
    // Write bytes explicitly: the 64-bit pr_flag has four preceding padding
    // bytes, which must never expose uninitialized kernel stack storage.
    let mut psi = [0u8; 136];
    let uid = if wide_flag { 16 } else { 8 };
    let pid = uid + 8;
    let name = pid + 16;
    let args = name + 16;
    psi[1] = b'R';
    psi[uid..uid + 4].copy_from_slice(&input.uid.to_ne_bytes());
    psi[uid + 4..uid + 8].copy_from_slice(&input.gid.to_ne_bytes());
    psi[pid..pid + 4].copy_from_slice(&input.pid.to_ne_bytes());
    psi[pid + 4..pid + 8].copy_from_slice(&input.ppid.to_ne_bytes());
    psi[pid + 8..pid + 12].copy_from_slice(&input.pgid.to_ne_bytes());
    psi[pid + 12..pid + 16].copy_from_slice(&input.sid.to_ne_bytes());
    let mut n = 0;
    while n < 15 && n < input.name.len() && input.name[n] != 0 {
        psi[name + n] = input.name[n];
        n += 1;
    }
    n = 0;
    while n < 79 && n < input.name.len() && input.name[n] != 0 {
        psi[args + n] = input.name[n];
        n += 1;
    }
    blob.note(b"CORE", CORE_NT_PRPSINFO, &psi[..if wide_flag { 136 } else { 128 }]);
}

/// Serialize the machine's note format. This wire-format choice is independent
/// of the ISA compiling the host tests. Unsupported ports retain legacy notes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_coredump_notes(
    input: *const NoteInput,
    e_machine: u16,
    buf: *mut u8,
    cap: usize,
    out_len: *mut u64,
) -> i32 {
    if input.is_null() || buf.is_null() || out_len.is_null() {
        return -22;
    }
    let input = unsafe { &*input };
    let mut blob = Blob { buf, cap, len: 0, overflow: false };
    if e_machine == 62 { // EM_X86_64: Linux struct user_regs_struct, 27 u64.
        let r = &input.regs;
        let regs = [r[15], r[14], r[13], r[12], r[6], r[1], r[11], r[10],
                    r[9], r[8], r[0], r[2], r[3], r[4], r[5], input.orig_syscall,
                    input.pc, r[16], input.status, input.sp, r[17], r[18], r[19],
                    0, 0, 0, 0];
        // The core-specific export supplies saved CS/SS and TLS bases. The
        // other selectors and x87/SSE are unavailable; do not invent FP data.
        emit_prstatus(&mut blob, input, regs, 0);
        emit_prpsinfo(&mut blob, input, true);
    } else {
        // Preserve the old RISC-V note and documented best-effort other ports.
        let mut regs = input.regs;
        regs[0] = input.pc;
        emit_prstatus(&mut blob, input, regs, 1);
        let mut fprs: Fpregset = unsafe { core::mem::zeroed() };
        fprs.f.copy_from_slice(&input.fp);
        fprs.fcsr = input.fcsr as u32;
        let fprs_bytes = unsafe {
            core::slice::from_raw_parts(&fprs as *const Fpregset as *const u8,
                                        core::mem::size_of::<Fpregset>())
        };
        blob.note(b"CORE", CORE_NT_FPREGSET, fprs_bytes);
        emit_prpsinfo(&mut blob, input, false);
    }

    if blob.overflow {
        return ENOMEM;
    }
    unsafe { *out_len = blob.len as u64 };
    0
}

/// Fill the ELF header and the program-header table, and report the layout the
/// write phase needs.  `ehdr_out` is `COREDUMP_EHDR_SIZE` bytes and
/// `phdrs_out` holds at least `(1 + nvma) * COREDUMP_PHDR_SIZE` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_coredump_layout(
    vmas: *const VmaInput,
    nvma: u32,
    notes_len: u64,
    rlim: u64,
    page_size: u64,
    e_machine: u16,
    ehdr_out: *mut u8,
    phdrs_out: *mut u8,
    phdrs_cap: usize,
    segs_out: *mut SegmentOut,
    segs_cap: usize,
    result: *mut LayoutResult,
) -> i32 {
    if ehdr_out.is_null() || phdrs_out.is_null() || result.is_null() {
        return -22;
    }
    if nvma != 0 && vmas.is_null() {
        return -22;
    }
    if page_size == 0 || page_size & (page_size - 1) != 0 {
        return -22;
    }
    if nvma != 0 && (segs_out.is_null() || segs_cap < nvma as usize) {
        return -22;
    }
    let phnum = 1 + nvma;
    let need = phnum as usize * COREDUMP_PHDR_SIZE;
    if phdrs_cap < need {
        return -22;
    }

    let notes_off = COREDUMP_EHDR_SIZE as u64 + phnum as u64 * COREDUMP_PHDR_SIZE as u64;
    let data_off = align_up(notes_off + notes_len, page_size);
    let limit = rlim;

    let phdrs = unsafe { core::slice::from_raw_parts_mut(phdrs_out as *mut Phdr, phnum as usize) };
    let mut note_phdr: Phdr = unsafe { core::mem::zeroed() };
    note_phdr.p_type = CORE_PT_NOTE;
    note_phdr.p_offset = notes_off;
    note_phdr.p_filesz = notes_len;
    note_phdr.p_memsz = notes_len;
    note_phdr.p_align = 4;
    phdrs[0] = note_phdr;

    let mut off = data_off;
    for i in 0..nvma as usize {
        let vma = unsafe { &*vmas.add(i) };
        let size = vma.end.wrapping_sub(vma.start);
        let mut filesz = size;
        if limit != u64::MAX && off + filesz > limit {
            filesz = if off < limit { limit - off } else { 0 };
        }
        // p_offset must stay congruent to p_vaddr modulo p_align, so a
        // limit-clamped segment rounds down to a page multiple.
        filesz = align_down(filesz, page_size);
        let mut ph: Phdr = unsafe { core::mem::zeroed() };
        ph.p_type = CORE_PT_LOAD;
        ph.p_flags = vma_pflags(vma.flags);
        ph.p_offset = off;
        ph.p_vaddr = vma.start;
        ph.p_filesz = filesz;
        ph.p_memsz = size;
        ph.p_align = page_size;
        phdrs[1 + i] = ph;
        if !segs_out.is_null() {
            unsafe {
                *segs_out.add(i) = SegmentOut { offset: off, filesz };
            }
        }
        off += filesz;
    }

    let mut eh: Ehdr = unsafe { core::mem::zeroed() };
    eh.ident[0] = 0x7f;
    eh.ident[1] = b'E';
    eh.ident[2] = b'L';
    eh.ident[3] = b'F';
    eh.ident[4] = 2; // ELFCLASS64
    eh.ident[5] = 1; // little-endian
    eh.ident[6] = 1; // EV_CURRENT
    eh.e_type = CORE_ET_CORE;
    eh.e_machine = e_machine;
    eh.e_version = 1;
    eh.e_phoff = COREDUMP_EHDR_SIZE as u64;
    eh.e_ehsize = COREDUMP_EHDR_SIZE as u16;
    eh.e_phentsize = COREDUMP_PHDR_SIZE as u16;
    eh.e_phnum = phnum as u16;
    unsafe {
        core::ptr::copy_nonoverlapping(&eh as *const Ehdr as *const u8, ehdr_out,
                                       COREDUMP_EHDR_SIZE);
    }

    let res = unsafe { &mut *result };
    res.notes_len = notes_len;
    res.notes_off = notes_off;
    res.data_off = data_off;
    res.phnum = phnum as u16;
    0
}


#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Mutex;

    static TEST_LOCK: Mutex<()> = Mutex::new(());

    fn pattern(input: &str, name: &str, pid: i32, tgid: i32, uid: i32, gid: i32,
               sig: i32, seconds: u64, outsz: usize) -> (i32, std::vec::Vec<u8>) {
        let mut name_buf = [0u8; 64];
        for (i, b) in name.as_bytes().iter().enumerate() {
            if i >= name_buf.len() { break; }
            name_buf[i] = *b;
        }
        let mut out = std::vec![0u8; outsz];
        // The production contract is a NUL-terminated C string.
        let mut c_pattern = input.as_bytes().to_vec();
        c_pattern.push(0);
        let rc = unsafe {
            a20_coredump_expand_pattern(c_pattern.as_ptr(), name_buf.as_ptr(), pid, tgid,
                                        uid, gid, sig, seconds, out.as_mut_ptr(),
                                        out.len())
        };
        (rc, out)
    }

    fn text(out: &[u8]) -> std::string::String {
        let end = out.iter().position(|&b| b == 0).unwrap_or(out.len());
        std::string::String::from_utf8_lossy(&out[..end]).into_owned()
    }

    fn notes(input: &NoteInput) -> std::vec::Vec<u8> {
        let mut buf = std::vec![0u8; CORE_NOTES_CAP];
        let mut len = 0u64;
        let rc = unsafe { a20_coredump_notes(input, 243, buf.as_mut_ptr() as *mut u8, buf.len(), &mut len) };
        assert_eq!(rc, 0);
        buf.truncate(len as usize);
        buf
    }

    const CORE_NOTES_CAP: usize = 2048;

    fn zeroed_input() -> NoteInput {
        unsafe { core::mem::zeroed() }
    }

    #[test]
    fn wire_sizes_match_the_elf64_abi() {
        assert_eq!(core::mem::size_of::<Ehdr>(), 64);
        assert_eq!(core::mem::size_of::<Phdr>(), 56);
        assert_eq!(core::mem::size_of::<Prstatus<32>>(), 376);
        assert_eq!(core::mem::size_of::<Prstatus<27>>(), 336);
        assert_eq!(core::mem::size_of::<Fpregset>(), 264);
    }

    #[test]
    fn pattern_expands_the_documented_placeholders() {
        let _guard = TEST_LOCK.lock().unwrap();
        let (rc, out) = pattern("%p-%e-%s-%u-%g-%t-%%.core", "bash", 42, 0, 1000, 1000,
                                11, 99, 128);
        assert_eq!(rc, 0);
        assert_eq!(text(&out), "42-bash-11-1000-1000-99-%.core");
    }

    #[test]
    fn pattern_prefers_the_thread_group_id_for_percent_p() {
        let _guard = TEST_LOCK.lock().unwrap();
        let (_, out) = pattern("%p", "x", 42, 7, 0, 0, 11, 0, 64);
        assert_eq!(text(&out), "7");
        let (_, out) = pattern("%p", "x", 42, -1, 0, 0, 11, 0, 64);
        assert_eq!(text(&out), "42");
    }

    #[test]
    fn pattern_drops_unsupported_placeholders() {
        let _guard = TEST_LOCK.lock().unwrap();
        let (_, out) = pattern("a%Zb%qc", "x", 1, 1, 0, 0, 1, 0, 64);
        assert_eq!(text(&out), "abc");
    }

    #[test]
    fn pattern_keeps_a_trailing_percent() {
        let _guard = TEST_LOCK.lock().unwrap();
        let (_, out) = pattern("core%", "x", 1, 1, 0, 0, 1, 0, 64);
        assert_eq!(text(&out), "core%");
    }

    #[test]
    fn pattern_truncates_the_name_like_the_c_scratch_buffer() {
        let _guard = TEST_LOCK.lock().unwrap();
        let long = "0123456789012345678901234567890123456789";
        let (_, out) = pattern("%e", long, 1, 1, 0, 0, 1, 0, 64);
        assert_eq!(text(&out), &long[..31]);
    }

    #[test]
    fn pattern_stays_terminated_and_bounded() {
        let _guard = TEST_LOCK.lock().unwrap();
        for outsz in 1..12usize {
            let (rc, out) = pattern("%p%p%p%p%p", "x", 123456, 0, 0, 0, 1, 0, outsz);
            assert_eq!(rc, 0);
            assert_eq!(out.len(), outsz);
            assert_eq!(out[outsz - 1], 0, "outsz {outsz}");
            assert!(out[..outsz - 1].iter().all(|&b| b != 0));
        }
    }

    #[test]
    fn pattern_formats_the_extremes() {
        let _guard = TEST_LOCK.lock().unwrap();
        let (_, out) = pattern("%u", "x", 0, 0, i32::MIN, 0, 0, 0, 64);
        assert_eq!(text(&out), "-2147483648");
        let (_, out) = pattern("%t", "x", 0, 0, 0, 0, 0, u64::MAX, 64);
        assert_eq!(text(&out), "18446744073709551615");
    }

    #[test]
    fn notes_preserve_the_legacy_riscv_descriptors() {
        let _guard = TEST_LOCK.lock().unwrap();
        let mut input = zeroed_input();
        input.sig = 11;
        input.pid = 7;
        input.ppid = 3;
        input.pgid = 7;
        input.sid = 3;
        input.uid = 1000;
        input.gid = 1000;
        input.name[..4].copy_from_slice(b"bash");
        input.pc = 0xdead;
        input.fp[0] = 0xbeef;
        input.fcsr = 5;
        let blob = notes(&input);

        // NT_PRSTATUS
        let (namesz, descsz, ntype) = note_header(&blob, 0);
        assert_eq!(namesz, 5);
        assert_eq!(ntype, CORE_NT_PRSTATUS);
        assert_eq!(descsz, 376);
        let prs = 12 + 8; // header + "CORE\0" (already four-byte aligned)
        assert_eq!(read_i32(&blob, prs), 11); // si_signo
        assert_eq!(read_i32(&blob, prs + 4), SI_KERNEL);
        assert_eq!(read_i32(&blob, prs + 32), 7); // pr_pid
        assert_eq!(read_u64(&blob, prs + 112), 0xdead); // pr_reg[0]

        // NT_FPREGSET, four-byte aligned after the 376-byte descriptor
        let next = prs + 376;
        let (namesz, descsz, ntype) = note_header(&blob, next);
        assert_eq!(namesz, 5);
        assert_eq!(ntype, CORE_NT_FPREGSET);
        assert_eq!(descsz, 264);
        assert_eq!(read_u64(&blob, next + 12 + 8), 0xbeef); // f[0]

        // NT_PRPSINFO
        let next = next + 12 + 8 + 264;
        let (namesz, descsz, ntype) = note_header(&blob, next);
        assert_eq!(namesz, 5);
        assert_eq!(ntype, CORE_NT_PRPSINFO);
        assert_eq!(descsz, 128);
        let psi = next + 12 + 8;
        assert_eq!(blob[psi + 1], b'R');
        assert_eq!(read_u32(&blob, psi + 8), 1000); // pr_uid
        assert_eq!(blob[psi + 32..psi + 36], *b"bash");

        assert_eq!(blob.len(), 12 + 8 + 376 + 12 + 8 + 264 + 12 + 8 + 128);
    }

    #[test]
    fn notes_report_an_undersized_buffer() {
        let input = zeroed_input();
        let mut buf = [0u8; 64];
        let mut len = 0u64;
        let rc = unsafe { a20_coredump_notes(&input, 243, buf.as_mut_ptr() as *mut u8, buf.len(), &mut len) };
        assert_eq!(rc, ENOMEM);
    }

    fn note_header(blob: &[u8], at: usize) -> (u32, u32, u32) {
        (read_u32(blob, at), read_u32(blob, at + 4), read_u32(blob, at + 8))
    }

    fn read_u32(blob: &[u8], at: usize) -> u32 {
        u32::from_ne_bytes(blob[at..at + 4].try_into().unwrap())
    }

    fn read_i32(blob: &[u8], at: usize) -> i32 {
        i32::from_ne_bytes(blob[at..at + 4].try_into().unwrap())
    }

    fn read_u64(blob: &[u8], at: usize) -> u64 {
        u64::from_ne_bytes(blob[at..at + 8].try_into().unwrap())
    }

    fn layout(vmas: &[VmaInput], notes_len: u64, rlim: u64, page: u64)
              -> (i32, std::vec::Vec<u8>, std::vec::Vec<u8>, std::vec::Vec<SegmentOut>,
                  LayoutResult) {
        let mut ehdr = std::vec![0u8; COREDUMP_EHDR_SIZE];
        let mut phdrs = std::vec![0u8; (1 + vmas.len()) * COREDUMP_PHDR_SIZE];
        let mut segs = std::vec![SegmentOut { offset: 0, filesz: 0 }; vmas.len()];
        let mut result: LayoutResult =
            LayoutResult { notes_len: 0, notes_off: 0, data_off: 0, phnum: 0 };
        let rc = unsafe {
            a20_coredump_layout(vmas.as_ptr(), vmas.len() as u32, notes_len, rlim, page,
                                243, ehdr.as_mut_ptr() as *mut u8,
                                phdrs.as_mut_ptr() as *mut u8, phdrs.len(),
                                segs.as_mut_ptr(), segs.len(), &mut result)
        };
        (rc, ehdr, phdrs, segs, result)
    }

    fn phdr_at(phdrs: &[u8], index: usize) -> Phdr {
        let at = index * COREDUMP_PHDR_SIZE;
        unsafe { core::ptr::read_unaligned(phdrs[at..at + COREDUMP_PHDR_SIZE].as_ptr() as *const Phdr) }
    }

    #[test]
    fn layout_places_the_note_then_the_page_aligned_segments() {
        let vmas = [
            VmaInput { start: 0x1000, end: 0x3000, flags: VM_READ | VM_EXEC },
            VmaInput { start: 0x8000, end: 0x9000, flags: VM_READ | VM_WRITE },
        ];
        let (rc, ehdr, phdrs, segs, result) = layout(&vmas, 700, u64::MAX, 4096);
        assert_eq!(rc, 0);
        assert_eq!(result.phnum, 3);
        assert_eq!(result.notes_off, 64 + 3 * 56);
        assert_eq!(result.data_off, align_up(64 + 168 + 700, 4096));
        assert_eq!(&ehdr[..4], &[0x7f, b'E', b'L', b'F']);
        assert_eq!(ehdr[4], 2);
        assert_eq!(ehdr[5], 1);
        assert_eq!(u16::from_ne_bytes(ehdr[16..18].try_into().unwrap()), CORE_ET_CORE);
        assert_eq!(u16::from_ne_bytes(ehdr[18..20].try_into().unwrap()), 243);
        assert_eq!(u64::from_ne_bytes(ehdr[32..40].try_into().unwrap()), 64);
        assert_eq!(u16::from_ne_bytes(ehdr[56..58].try_into().unwrap()), 3);

        let note = phdr_at(&phdrs, 0);
        assert_eq!(note.p_type, CORE_PT_NOTE);
        assert_eq!(note.p_offset, result.notes_off);
        assert_eq!(note.p_filesz, 700);
        assert_eq!(note.p_memsz, 700);
        assert_eq!(note.p_align, 4);

        let first = phdr_at(&phdrs, 1);
        assert_eq!(first.p_type, CORE_PT_LOAD);
        assert_eq!(first.p_flags, CORE_PF_R | CORE_PF_X);
        assert_eq!(first.p_offset, result.data_off);
        assert_eq!(first.p_vaddr, 0x1000);
        assert_eq!(first.p_filesz, 0x2000);
        assert_eq!(first.p_memsz, 0x2000);
        assert_eq!(first.p_align, 4096);
        assert_eq!(segs[0].offset, result.data_off);
        assert_eq!(segs[0].filesz, 0x2000);

        let second = phdr_at(&phdrs, 2);
        assert_eq!(second.p_flags, CORE_PF_R | CORE_PF_W);
        assert_eq!(second.p_offset, result.data_off + 0x2000);
        assert_eq!(segs[1].filesz, 0x1000);
    }

    #[test]
    fn layout_clamps_the_total_size_to_rlimit_core() {
        let vmas = [
            VmaInput { start: 0x1000, end: 0x9000, flags: VM_READ },
            VmaInput { start: 0x9000, end: 0xa000, flags: VM_READ },
        ];
        let data_off = align_up(64 + 3 * 56 + 100, 4096);
        let rlim = data_off + 0x3000;
        let (rc, _, phdrs, segs, result) = layout(&vmas, 100, rlim, 4096);
        assert_eq!(rc, 0);
        assert_eq!(result.data_off, data_off);
        // The first segment is clamped to the page-rounded remainder.
        assert_eq!(segs[0].filesz, 0x3000);
        assert_eq!(phdr_at(&phdrs, 1).p_memsz, 0x8000);
        // Nothing is left for the second one.
        assert_eq!(segs[1].filesz, 0);
        assert_eq!(phdr_at(&phdrs, 2).p_offset, data_off + 0x3000);
    }

    #[test]
    fn layout_keeps_offsets_congruent_when_clamping() {
        let vmas = [VmaInput { start: 0x2000, end: 0x9000, flags: VM_READ }];
        let data_off = align_up(64 + 2 * 56 + 8, 4096);
        // A limit that is not a page multiple must still round the segment
        // down, so p_offset stays congruent to p_vaddr modulo p_align.
        let (rc, _, phdrs, segs, _) = layout(&vmas, 8, data_off + 0x1234, 4096);
        assert_eq!(rc, 0);
        assert_eq!(segs[0].filesz, 0x1000);
        assert_eq!(segs[0].filesz % phdr_at(&phdrs, 1).p_align, 0);
    }

    #[test]
    fn layout_rejects_bad_arguments() {
        let vmas = [VmaInput { start: 0, end: 0x1000, flags: VM_READ }];
        let mut ehdr = std::vec![0u8; COREDUMP_EHDR_SIZE];
        let mut phdrs = std::vec![0u8; 2 * COREDUMP_PHDR_SIZE];
        let mut segs = [SegmentOut { offset: 0, filesz: 0 }; 1];
        let mut result = LayoutResult { notes_len: 0, notes_off: 0, data_off: 0, phnum: 0 };
        // A non-power-of-two page size is refused.
        let rc = unsafe {
            a20_coredump_layout(vmas.as_ptr(), 1, 0, u64::MAX, 3000, 243,
                                ehdr.as_mut_ptr() as *mut u8,
                                phdrs.as_mut_ptr() as *mut u8, phdrs.len(),
                                segs.as_mut_ptr(), segs.len(), &mut result)
        };
        assert_eq!(rc, -22);
        // So is a program-header buffer that cannot hold the table.
        let rc = unsafe {
            a20_coredump_layout(vmas.as_ptr(), 1, 0, u64::MAX, 4096, 243,
                                ehdr.as_mut_ptr() as *mut u8,
                                phdrs.as_mut_ptr() as *mut u8, COREDUMP_PHDR_SIZE,
                                segs.as_mut_ptr(), segs.len(), &mut result)
        };
        assert_eq!(rc, -22);
        // And a segment table that is too small.
        let rc = unsafe {
            a20_coredump_layout(vmas.as_ptr(), 1, 0, u64::MAX, 4096, 243,
                                ehdr.as_mut_ptr() as *mut u8,
                                phdrs.as_mut_ptr() as *mut u8, phdrs.len(),
                                segs.as_mut_ptr(), 0, &mut result)
        };
        assert_eq!(rc, -22);
    }

    #[test]
    fn layout_accepts_an_empty_vma_list() {
        let (rc, ehdr, phdrs, _, result) = layout(&[], 64, u64::MAX, 4096);
        assert_eq!(rc, 0);
        assert_eq!(result.phnum, 1);
        assert_eq!(u16::from_ne_bytes(ehdr[56..58].try_into().unwrap()), 1);
        assert_eq!(phdr_at(&phdrs, 0).p_type, CORE_PT_NOTE);
        assert_eq!(result.data_off, align_up(64 + 56 + 64, 4096));
    }
}
