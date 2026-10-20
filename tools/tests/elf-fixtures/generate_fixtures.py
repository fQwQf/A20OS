#!/usr/bin/env python3
"""
A20OS ELF parser fixture generator — malformed, truncated, and overflowing
ELF images for kernel/mm/elf.c and kernel/mm/elf_core.rs.

Design doc: README.md in this directory. This script is deterministic: the same
script produces byte-identical outputs, which is why fixture-manifest.tsv can
pin every fixture image by SHA-256.

Usage:
    python3 generate_fixtures.py OUTDIR

It writes:
    OUTDIR/fixtures/<id>.elf          one raw image file per fixture
    OUTDIR/fixture-manifest.tsv       id, family(class of the bytes), machine,
                                      target, size, sha256
    OUTDIR/expectations.tsv           id, category, C-baseline verdict,
                                      rust-plan verdict, plan_visible flag
    OUTDIR/oracle-verdicts.tsv        id + verdict from the model oracle below

The oracle mirrors only the *pure header/segment decision logic* of
kernel/mm/elf.c (elf_check_header, map_fd_segment_lazy pre-checks, the eager
loop's bounds) and kernel/mm/elf_core.rs (parse_ehdr / validate_ehdr /
validate_segment / plan_image table bounds and page-overlap check).  It exists
to catch byte-layout mistakes in this generator and in README.md expectations;
it is a model check, NOT evidence about the real kernel.  The kernel host test replays these cases against the current C/Rust parser adapters.

No ELF library is used; every byte is written with explicit struct.pack against
kernel/include/mm/elf.h (sha256 926ef8ca..., frozen inventory).
"""

import hashlib
import os
import struct
import sys

PAGE = 0x1000
MAX_PHDRS = 64
MAGIC = b"\x7fELF"

# --------------------------------------------------------------- layout

EHDR64_SIZE, EHDR32_SIZE = 64, 52
PHDR64_SIZE, PHDR32_SIZE = 56, 32

PT_NULL, PT_LOAD, PT_DYNAMIC, PT_INTERP, PT_NOTE, PT_PHDR, PT_TLS = 0, 1, 2, 3, 4, 6, 7
PT_GNU_STACK = 0x6474E551
PT_A20_START_INFO = 0x6A20A200
PF_X, PF_W, PF_R = 1, 2, 4

# e_machine values of the purpose targets (Makefile ELF_MACHINE_*/ELF_CLASS_*)
TARGETS = {
    "riscv64": (2, 243),     # ARCH=riscv64 default
    "arm32":   (1, 40),
    "armv7m":  (1, 40),
}
DEFAULT_TARGET = "riscv64"

LOAD_VA = 0x10000
PHOFF64, PHOFF32 = 64, 52
# End of a two-entry 64-bit program-header table: table spans [64, 176),
# so the second entry sits at PHOFF64+PHDR64_SIZE and BLOB data lands
# right at the table end. Use these for every fixture whose e_phnum==2.
PHDR64_SLOT1 = PHOFF64 + PHDR64_SIZE   # 120 — second table entry
BLOCK3_OFF = PHOFF64 + 2 * PHDR64_SIZE # 176 — first byte after the table

def ehdr64(etype, machine, entry, phoff, phnum, phentsize=56,
           shoff=0, shentsize=64, shnum=0, shstrndx=0, eflags=0, ehsize=64):
    ident = MAGIC + bytes([2, 1, 1, 0]) + b"\0" * 8      # class64 LSB EV_CURRENT
    return (ident
            + struct.pack("<HH", etype, machine)
            + struct.pack("<I", 1)
            + struct.pack("<QQ", entry, phoff)           # e_entry@24, e_phoff@32
            + struct.pack("<Q", shoff)                   # e_shoff@40
            + struct.pack("<IHHHHHH", eflags, ehsize, phentsize,
                          phnum, shentsize, shnum, shstrndx))
    # @48 e_flags, @52 e_ehsize, @54 e_phentsize, @56 e_phnum, @58 e_shentsize,
    # @60 e_shnum, @62 e_shstrndx

def ehdr32(etype, machine, entry, phoff, phnum, phentsize=32,
           shoff=0, shentsize=40, shnum=0, shstrndx=0, eflags=0, ehsize=52):
    ident = MAGIC + bytes([1, 1, 1, 0]) + b"\0" * 8
    return (ident
            + struct.pack("<HH", etype, machine)
            + struct.pack("<I", 1)
            + struct.pack("<II", entry, phoff)           # @24, @28
            + struct.pack("<I", shoff)                   # @32
            + struct.pack("<IHHHHHH", eflags, ehsize, phentsize,
                          phnum, shentsize, shnum, shstrndx))
    # @36 e_flags, @40 e_ehsize, @42 e_phentsize, @44 e_phnum

def phdr64(ptype, flags, offset, vaddr, filesz, memsz, align=0x1000):
    return struct.pack("<IIQQQQQQ", ptype, flags, offset, vaddr, 0,
                       filesz, memsz, align)

def phdr32(ptype, offset, vaddr, filesz, memsz, flags, align=0x1000):
    return struct.pack("<IIIIIIII", ptype, offset, vaddr, 0, filesz, memsz,
                       flags, align)

def u64wrap(x):
    return x & ((1 << 64) - 1)

def pad_to(buf, size):
    if len(buf) > size:
        raise ValueError(f"pad_to: {len(buf)} > {size}")
    return buf + b"\0" * (size - len(buf))

def image(*parts, size=0x200, tracer=None):
    """Empty flat image; parts are (offset, bytes) placements."""
    buf = bytearray(size)
    if callable(tracer):
        tracer(buf)
    for off, data in parts:
        buf[off:off + len(data)] = data
    return bytes(buf)

# The standard positive service shape: header at 0, one phdr at 64/52,
# payload region afterwards.

def pos_load64(file_len, vaddr=LOAD_VA, flags=PF_R | PF_X, offset=0,
               memsz=None):
    return phdr64(PT_LOAD, flags, offset, vaddr, file_len,
                  memsz if memsz is not None else file_len)

# --------------------------------------------------------------- fixtures

FIXTURES = []

def add(fid, category, builder, family="elf64", machine=None, target=None,
        c="?", rust="?", plan_visible="yes", note=""):
    if target is None:
        target = DEFAULT_TARGET
    FIXTURES.append(dict(id=fid, category=category, build=builder,
                         family=family, machine=machine, target=target,
                         c=c, rust=rust, plan_visible=plan_visible, note=note))

# --- group 0: positive anchor ------------------------------------------------

def _f00():
    head = ehdr64(2, 243, LOAD_VA, PHOFF64, 1)
    payload = b"A20ELF-POSITIVE-CONTROL"
    return image((0, head), (PHOFF64, pos_load64(len(head) + PHDR64_SIZE + len(payload))),
                 (140, payload))

add("f00-pos-exec-rv64", "3.x positive", _f00, c="ok", rust="ok",
    note="single RX PT_LOAD covering the header; base=LOAD_VA; entry=LOAD_VA")

def _f01():
    vaddr = 0x1000
    head = ehdr64(3, 243, vaddr, PHOFF64, 1)
    payload = b"A20ELF-PIE-POSITIVE"
    return image((0, head), (PHOFF64, pos_load64(len(head) + PHDR64_SIZE + len(payload), vaddr=vaddr)),
                 (140, payload))

add("f01-pos-dyn-rv64", "3.x positive", _f01, c="ok", rust="ok",
    note="ET_DYN: load_bias = USER_DYN_BASE(0x10000) + aslr(biseco<<16)")

def _f13b():
    head = ehdr32(2, 40, LOAD_VA, PHOFF32, 1)
    payload = b"A20ELF-ARM32-POSITIVE"
    return image((0, head),
                 (PHOFF32, phdr32(PT_LOAD, 0, LOAD_VA,
                                  len(head) + PHDR32_SIZE + len(payload),
                                  len(head) + PHDR32_SIZE + len(payload),
                                  PF_R | PF_X)),
                 (84, payload))

add("f13b-pos-arm32-exec", "3.x positive", _f13b, family="elf32", machine=40,
    target="arm32", c="ok (ARCH=arm32)", rust="ok (ARCH=arm32)",
    note="arm32 twin of the positive control: runs on the arm32 target only")

# --- group 2.x: header gates -------------------------------------------------

def _f05():
    return image((0, ehdr64(1, 243, LOAD_VA, PHOFF64, 1)),
                 (PHOFF64, pos_load64(PHOFF64 + PHDR64_SIZE)))

add("f05-type-rel", "2.x header", _f05, c="-ENOEXEC", rust="-ENOEXEC",
    note="e_type=ET_REL with an otherwise valid single LOAD: only the type "
         "gate fires (elf.c:1047)")

def _f10():
    return ehdr64(2, 243, LOAD_VA, PHOFF64, 1)[:63]

add("f10-trunc-ehdr-63b", "2.x truncated", _f10, c="OBSERVE", rust="-ENOEXEC",
    note="63 valid bytes: C fills the rest of the read from an UNINITIALIZED "
         "stack buffer after the >= sizeof(Elf32_Ehdr)=52 gate (elf.c:1656-1671); "
         "rust parse_ehdr requires the full 64-byte buffer and rejects")

def _f11():
    return ehdr64(2, 243, LOAD_VA, PHOFF64, 1)[:40]

add("f11-trunc-ehdr-40b", "2.x truncated", _f11, c="-ENOEXEC", rust="-ENOEXEC",
    note="40 < sizeof(Elf32_Ehdr) — short header read fails outside the "
         "short-read gate (elf.c:1658)")

def _f12():
    head = bytearray(ehdr64(2, 243, LOAD_VA, PHOFF64, 1))
    head[3] = ord('G')                                   # \x7fELG
    return image((0, bytes(head)), (PHOFF64, pos_load64(PHOFF64 + PHDR64_SIZE)))

add("f12-magic-corrupt", "2.x header", _f12, c="-ENOEXEC", rust="-ENOEXEC",
    note="magic mismatch keeps elf_probe_foreign from forwarding (elf.c:1138) — "
         "no translator retry, final execve -ENOEXEC (exec.c:1262)")

def _f13():
    head = ehdr32(2, 243, LOAD_VA, PHOFF32, 1)           # class32 bytes offered
    payload = b"A20ELF-RV64-TG-CLASS"
    return image((0, head), (PHOFF32, phdr32(PT_LOAD, 0, LOAD_VA, 0x100, 0x200,
                                             PF_R | PF_X)), (84, payload))

add("f13-class32-on-rv64", "2.x header", _f13, family="elf32", machine=243,
    target=DEFAULT_TARGET, c="-ENOEXEC", rust="-ENOEXEC",
    note="ELFCLASS32 bytes offered to the riscv64 (ELFCLASS64) target: -EINVAL at "
         "the class gate in elf_check_header (elf.c:1045)")

def _f14():
    return image((0, ehdr64(2, 258, LOAD_VA, PHOFF64, 1)),
                 (PHOFF64, pos_load64(PHOFF64 + PHDR64_SIZE)))

add("f14-foreign-loong64", "2.x probe", _f14, machine=258,
    c="-ENOEXEC (or EXEC_RETRY with xlator row)", rust="-ENOEXEC",
    note="structurally valid EM_LOONGARCH64: -ENOEXEC from the machine gate; "
         "with CONFIG_XLATOR=y and no configured row: exec finishes -ENOEXEC; "
         "a configured row flips exec into EXEC_RETRY (observe only)")

def _f15():
    head = ehdr64(2, 243, LOAD_VA, PHOFF64, 65)
    nulls = phdr64(PT_NULL, 0, 0, 0, 0, 0) * 64
    return image((0, head), (PHOFF64, pos_load64(64 + 65 * PHDR64_SIZE)),
                 (120, nulls), size=0x2100)

add("f15-phnum-65", "2.x header", _f15, c="-ENOEXEC", rust="-ENOEXEC",
    note="e_phnum=65 > MAX_PHDRS(64) rejected by the count gate (elf.c:1050)")

def _f16():
    return image((0, ehdr64(2, 243, LOAD_VA, PHOFF64, 0)),
                 (PHOFF64, pos_load64(PHOFF64 + PHDR64_SIZE)))

add("f16-phnum-0", "2.x header", _f16, c="-ENOEXEC", rust="-ENOEXEC",
    note="e_phnum=0 rejected (elf.c:1050)")

def _f17():
    return image((0, ehdr64(2, 243, LOAD_VA, 0x10000, 1)),
                 (PHOFF64, pos_load64(PHDR64_SIZE)))

add("f17-phoff-past-eof", "2.x truncated", _f17, c="-ENOEXEC", rust="-ENOEXEC",
    note="program-header table beyond EOF: short vfs_read in elf_load64, and "
         "table_len > supplied bytes in the rust plan; both -ENOEXEC")

def _f18():
    head = ehdr64(2, 243, 0, PHOFF64, 64)
    nulls = phdr64(PT_NULL, 0, 0, 0, 0, 0) * 64
    return image((0, head), (PHOFF64, nulls), size=0x1000)

add("f18-phnum-64-no-load", "2.x header", _f18, c="OBSERVE", rust="-ENOEXEC",
    note="64 valid-but-empty phdrs: C produces an empty address space and "
         "enters e_entry=0 (deferred SIGSEGV); plan_image refuses nseg==0 — "
         "divergence D5 to reconcile")

def _f19():
    return image((0, ehdr64(2, 243, LOAD_VA, PHOFF64, 1, phentsize=55)),
                 (PHOFF64, pos_load64(PHOFF64 + PHDR64_SIZE)))

add("f19-phentsize-small", "2.x header", _f19, c="-ENOEXEC", rust="-ENOEXEC",
    note="e_phentsize=55 < 56 rejected by the phentsize gate (elf.c:1049)")

# --- group 3.x: segment size/layout --------------------------------------

def _f20():
    head = ehdr64(2, 243, LOAD_VA, PHOFF64, 1)
    return image((0, head),
                 (PHOFF64, phdr64(PT_LOAD, PF_R | PF_X, 0, LOAD_VA, 0x2000, 0x1000)),
                 (120, b"A20ELF-OVERSIZE-FILESZ"))

add("f20-filesz-gt-memsz", "3.x overflow", _f20,
    c="-ENOEXEC (lazy) | OBSERVE (eager: silent)", rust="-ENOEXEC",
    note="lazy rejects before mapping (elf.c:323-324); an eager build "
         "(arm32, overlap fallback) copies p_filesz bytes into a "
         "p_memsz-sized slot; see README for the baseline limitation")

def _f21():
    head = ehdr64(2, 243, 0x10180, PHOFF64, 1)
    ph = phdr64(PT_LOAD, PF_R | PF_X, 0x2000, 0x10080, 0x100, 0x200)
    return image((0, head), (PHOFF64, ph), (0x2000, b"\xAA" * 0x100),
                 size=0x2200)

add("f21-congruence-mismatch", "3.x layout", _f21,
    c="OBSERVE (eager fallback)", rust="-EINVAL",
    note="(0x2000&0xfff)=0 != (0x10080&0xfff)=0x80: lazy returns -EINVAL and "
         "the C loader keeps the eager fallback with shifted file content "
         "(elf.c:325-327, 688-692); the plan refuses -EINVAL")

def _f22():
    head = ehdr64(2, 243, 0x100000, PHOFF64, 1)
    return image((0, head), (PHOFF64,
        phdr64(PT_LOAD, PF_R | PF_W, 0, u64wrap(-PAGE * 1), 0x2000, 0x2000)))

add("f22-vaddr-memsz-wrap", "3.x overflow", _f22, c="OBSERVE (ghost VMA)",
    rust="-ENOEXEC",
    note="p_vaddr+p_memsz wraps to 0x1000: the eager loop maps zero pages and "
         "still calls elf_add_vma(start=0xFFFF...F000, end=0x1000) — corrupt "
         "VMA (elf.c:682-683, 778); plan_image checked_add refuses")

def _f23():
    head = ehdr64(2, 243, LOAD_VA, PHOFF64, 1)
    off = u64wrap(-PAGE)
    # filesz/memsz must exceed the 0x1000 gap so off+filesz crosses 2^64.
    return image((0, head),
                 (PHOFF64, phdr64(PT_LOAD, PF_R | PF_X, off, LOAD_VA,
                                  0x2000, 0x2000)),
                 (120, b"\xCC" * 0x100))

add("f23-offset-filesz-wrap", "3.x overflow", _f23, c="OBSERVE (past-EOF mmap)",
    rust="-ENOEXEC",
    note="p_offset=0xFFFF...F000 is page-congruent with vaddr, so only "
         "p_offset+p_filesz (0x2000 -> 2^64) reveals the wrapped file range; "
         "the lazy path passes its local checks and mm_mmap_file receives a "
         "past-EOF file_page_offset (elf.c:320-347); the plan refuses: "
         "p_offset+p_filesz checked_add wraps")

def _f24():
    head = ehdr64(2, 243, 0x100000, PHOFF64, 2)
    a = phdr64(PT_LOAD, PF_R | PF_X, 0, 0x100000, 0x1000, 0x1000)
    b = phdr64(PT_LOAD, PF_R | PF_X, 0x800, 0x100800, 0x1000, 0x1000)
    buf = bytearray(0x1800)
    buf[0:64] = head
    buf[PHOFF64:PHOFF64 + 56] = a
    buf[PHOFF64 + 56:PHOFF64 + 112] = b
    buf[0x800:0x820] = b"B" * 0x20                   # overlapping segment payload
    buf[0x1000:0x1030] = b"A" * 0x30
    return bytes(buf)

add("f24-seg-overlap", "3.x overlap", _f24,
    c="-EINVAL (lazy) | OBSERVE (eager fallback: overlay + double VMA)", rust="-EINVAL",
    note="[0x100000,0x101000) vs [0x100800,0x101800): lazy refuses overlap and "
         "the eager fallback overlays page-shared contents plus inserts a "
         "second overlapping VMA (elf.c:339-342, 696-705); the plan refuses -EINVAL")

def _f25():
    head = ehdr64(2, 243, LOAD_VA, PHOFF64, 1)
    return image((0, head),
                 (PHOFF64, phdr64(PT_LOAD, PF_R | PF_W, 0, LOAD_VA, 0x800, 0x1800)),
                 (120, b"E" * 0x800), size=0xC00)

add("f25-bss-tail-zeros", "3.x positive", _f25, c="ok", rust="ok",
    note="memsz[0x1800] > filesz[0x800]: tail [LOAD_VA+0x800, LOAD_VA+0x1800) "
         "must be zeroes, not file content (elf.c:306-311, 706-713)")

# --- group 3.x: interpreter / rollback -----------------------------------

def _f26():
    interp = b"/lib/ld-a20-fixture-absent.so\0"
    head = ehdr64(2, 243, LOAD_VA, PHOFF64, 2)
    return image((0, head), (PHOFF64, pos_load64(BLOCK3_OFF)),
                 (PHDR64_SLOT1,
                  phdr64(PT_INTERP, PF_R, BLOCK3_OFF, 0x20000,
                         len(interp), len(interp))),
                 (BLOCK3_OFF, interp), size=0x210)

add("f26-interp-missing", "3.x interp", _f26, c="-ENOENT", rust="-ENOENT",
    note="unique interpreter name absent from every resolve_interp fallback; "
         "a rootfs holding /lib/libc.so re-resolves it (elf.c:919-964) + attest: "
         "run on a rootfs without /lib/libc.so, or expect the load to succeed")

def _f27():
    interp = b"a" * 4095          # no NUL either within filesz
    head = ehdr64(2, 243, LOAD_VA, PHOFF64, 2)
    return image((0, head), (PHOFF64, pos_load64(BLOCK3_OFF)),
                 (PHDR64_SLOT1,
                  phdr64(PT_INTERP, PF_R, BLOCK3_OFF, 0x20000,
                         len(interp), len(interp))),
                 (BLOCK3_OFF, interp), size=0x1400)

add("f27-interp-no-nul", "3.x interp", _f27, c="-ENOENT|-ENAMETOOLONG",
    rust="-ENOENT|-ENAMETOOLONG",
    note="interp fills MAX_PATH_LEN-1 with no NUL; C appends the terminator "
         "(elf.c:1355-1360) and vfs_open decides; record the observed errno")

# --- group 3.x: permission / W^X sweet ----------------------------------

def _f30():
    head = ehdr64(2, 243, LOAD_VA, PHOFF64, 1)
    return image((0, head),
                 (PHOFF64, phdr64(PT_LOAD, PF_R | PF_W | PF_X, 0, LOAD_VA,
                                  0x100, 0x100)),
                 (120, b"\x90" * 0x10))

add("f30-rwx-load", "3.x permission", _f30,
    c="off: ok | strip: ok (W stripped) | deny: -EACCES",
    rust="off: ok | strip: ok (W stripped) | deny: -EACCES",
    note="one RWX PT_LOAD against a20.wx=off|strip|deny (elf.c:141-153, "
         "wx.c:120-151; xlator_host exemption applies only to translator "
         "hosts and is set by exec, not by this path)")

def _f32():
    head = ehdr64(2, 243, LOAD_VA, PHOFF64, 1)
    return image((0, head), (PHOFF64, phdr64(PT_LOAD, 0, 0, LOAD_VA, 0x100, 0x100)),
                 (120, b"\x00" * 0x10))

add("f32-flags-zero", "3.x permission", _f32, c="ok (silent)", rust="ok",
    note="p_flags=0 makes PROT_NONE pages; exec succeeds and the first user "
         "access takes SIGSEGV — documents the silent degenerate case")

# --- TLS / rollback each owe an observable --------------------------------

def _f31():
    head = ehdr64(2, 243, LOAD_VA, PHOFF64, 2)
    return image((0, head), (PHOFF64, pos_load64(64 + 2 * PHDR64_SIZE)),
                 (PHOFF64 + PHDR64_SIZE,
                  phdr64(PT_TLS, PF_R, 0xB0, 0x20000, 0x20000000, 0x20000000, 8)),
                 (BLOCK3_OFF, b"T" * 0x20))

add("f31-tls-oversize", "3.x overflow/rollback", _f31,
    c="expect -ENOMEM; OBSERVE if the allocator satisfies it", rust="same",
    note="PT_TLS p_filesz=p_memsz=512 MiB — the load passes "
         "kmalloc(tls_filesz) (elf.c:1366-1372); asserts failed-load rollback "
         "keeps the old image intact after -ENOMEM")

def _f41():
    head = ehdr64(2, 243, LOAD_VA, PHOFF64, 2)
    return image((0, head), (PHOFF64, pos_load64(64 + 2 * PHDR64_SIZE)),
                 (PHOFF64 + PHDR64_SIZE,
                  phdr64(PT_TLS, PF_R, 0xB0, 0x20000, 0x1000, 0x800, 8)),
                 (BLOCK3_OFF, b"T" * 0x20))

add("f41-tls-filesz-gt-memsz", "3.x overflow/divergence", _f41,
    c="ok (no TLS size check)", rust="-ENOEXEC",
    note="PT_TLS p_filesz=0x1000 > p_memsz=0x800: the C loader has no TLS "
         "sanity check (elf.c:1362-1374) while plan_image rejects "
         "(elf_core.rs:458-459) — divergence D7 to reconcile")

# --- native marker ------------------------------------------------------

def _f40():
    head = ehdr64(2, 243, LOAD_VA, PHOFF64, 2)
    marker_off = len(head) + 2 * PHDR64_SIZE          # 0xB0
    return image((0, head), (PHOFF64, pos_load64(64 + 2 * PHDR64_SIZE)),
                 (PHOFF64 + PHDR64_SIZE,
                  phdr64(PT_A20_START_INFO, PF_R | PF_W, marker_off, 0x20000,
                         0x10, 0x10)),
                 (marker_off, b"S" * 0x10))

add("f40-native-marker", "6.x native", _f40,
    c="ABI=both: is_native_abi=1 | ABI=linux: -ENOEXEC",
    rust="ABI=both: is_native_abi=1 | ABI=linux: -ENOEXEC",
    note="PT_A20_START_INFO(0x6a20a200) flips the native-ABI verdict in "
         "elf_phdrs_native (elf.c:1070-1100); the default ABI=both kernel "
         "loads the image with the a20 stack layout")

# --- fixed vs honored phdr stride (D6) ----------------------------------

def _f33():
    head = ehdr64(2, 243, LOAD_VA, PHOFF64, 2, phentsize=57)
    a = pos_load64(64 + 56 + 0x10)
    b = phdr64(PT_LOAD, PF_R | PF_X, 0, LOAD_VA + 0x1000, 0x8000, 0x1000)
    buf = bytearray(0x140)
    buf[0:64] = head
    buf[PHOFF64:PHOFF64 + 56] = a
    buf[PHOFF64 + 56] = 0x00                          # byte 120: C's stride pad
    buf[PHOFF64 + 57:PHOFF64 + 57 + 56] = b           # spec position: 121
    return bytes(buf)

add("f33-phentsize-57", "2.x header", _f33, c="OBSERVE (drops entry B)",
    rust="-ENOEXEC",
    note="e_phentsize=57 (>56 accepted). C strides the table at a fixed 56 "
         "bytes, so entry B is misread as p_type=0x100 (skipped) and the "
         "image runs with only segment A; the rust plan honors entsize and "
         "reads the real filesz>memsz entry — divergence D6")

# --- .a20drv probes (path-gated; validated by the driver adapter) -------

def _f28():
    head = ehdr64(2, 243, LOAD_VA, PHOFF64, 1, shentsize=63)
    return image((0, head), (PHOFF64, pos_load64(64 + PHDR64_SIZE)),
                 (120, b"D" * 0x40))

add("f28-a20drv-badshentsize", "2.x driver", _f28, c="-ENOEXEC",
    rust="n/a (driver-registry adapter)", plan_visible="no",
    note="take effect only when exec'd under a *.a20drv filename "
         "(elf.c:85-95); user-driver section validation is out of the rust "
         "core's scope (function map 2.6)")

def _f29():
    head = ehdr64(2, 243, LOAD_VA, PHOFF64, 1, shnum=0)
    return image((0, head), (PHOFF64, pos_load64(64 + PHDR64_SIZE)),
                 (120, b"D" * 0x40))

add("f29-a20drv-shnum0", "2.x driver", _f29, c="-ENOEXEC",
    rust="n/a (driver-registry adapter)", plan_visible="no",
    note="*.a20drv filename with e_shnum=0: elf_validate_user_driver gate")


# ---------------------------------------------------------------- oracle

def oracle(f, content):
    """Model the pure decision logic for the fixture's target.
    Returns a model verdict a string for cross-checking README expectations."""
    target_cls, target_mach = TARGETS.get(f["target"], TARGETS[DEFAULT_TARGET])

    if len(content) < 16 or content[0:4] != MAGIC:
        return "reject:magic"

    if f["family"] == "elf64":
        if len(content) < EHDR64_SIZE:
            return "reject:short-ehdr"
        cls, data = content[4], content[5]
        etype, machine = struct.unpack_from("<HH", content, 16)
        entry, phoff = struct.unpack_from("<QQ", content, 24)
        ehsize_v, phentsize, phnum = struct.unpack_from("<HHH", content, 52)
        shentsize = struct.unpack_from("<H", content, 58)[0]
        shoff = struct.unpack_from("<Q", content, 40)[0]
        shnum = struct.unpack_from("<H", content, 60)[0]
    else:
        if len(content) < EHDR32_SIZE:
            return "reject:short-ehdr"
        cls, data = content[4], content[5]
        etype, machine = struct.unpack_from("<HH", content, 16)
        entry, phoff = struct.unpack_from("<II", content, 24)
        ehsize_v, phentsize, phnum = struct.unpack_from("<HHH", content, 40)
        shentsize = struct.unpack_from("<H", content, 46)[0]
        shoff = struct.unpack_from("<I", content, 32)[0]
        shnum = struct.unpack_from("<H", content, 48)[0]
    u32 = 4

    # --- C-side gates (arch=target arch) --------------------------------
    bad = []
    if cls != target_cls:
        bad.append("class")
    if data != 1:
        bad.append("data")
    if etype not in (2, 3):
        bad.append("type")
    bad.append("machine") if machine != target_mach else None
    if phentsize < (PHDR32_SIZE if f["family"] == "elf32" else PHDR64_SIZE):
        bad.append("phentsize")
    if phnum == 0 or phnum > MAX_PHDRS:
        bad.append("phnum")
    if bad:
        return "reject:" + ','.join(bad)

    if f["id"] == "f28-a20drv-badshentsize":
        # shentsize is compared against sizeof(Elf64_Shdr)=64 (elf.c:93)
        if (64 if f["family"] == "elf64" else 40) != shentsize:
            return "reject:driver-shentsize"
    if f["id"] == "f29-a20drv-shnum0" and shnum == 0 and shoff == 0:
        return "reject:driver-shnum"

    # --- plan-side (rust) ----------------------------------------------
    ent = phentsize
    if phoff + phnum * ent > len(content):
        return "reject:phdr-table-bounds"
    if phoff + phnum * ent >= (1 << 64):
        return "reject:plan-wrap"

    nseg, native = 0, False
    segments = []
    for i in range(phnum):
        at = phoff + i * ent
        phsize = PHDR32_SIZE if f["family"] == "elf32" else PHDR64_SIZE
        if at + phsize > len(content):
            return "reject:short-phdr"
        if f["family"] == "elf64":
            ptype, pflags, off, vaddr = struct.unpack_from("<IIQQ", content, at)
            filesz, memsz = struct.unpack_from("<QQ", content, at + 32)
        else:
            ptype, off, vaddr = struct.unpack_from("<III", content, at)
            filesz, memsz = struct.unpack_from("<II", content, at + 16)
            pflags = struct.unpack_from("<I", content, at + 24)[0]
        if ptype == PT_A20_START_INFO:
            native = True
        if ptype != PT_LOAD:
            continue
        if filesz > memsz:
            return "reject:plan-filesz-gt-memsz"
        if vaddr + memsz >= (1 << 64) or off + filesz >= (1 << 64):
            return "reject:plan-wrap"
        if filesz != 0 and (off & 0xFFF) != (vaddr & 0xFFF):
            return "reject:plan-congruence"
        start = vaddr & ~0xFFF
        end = u64wrap((vaddr + memsz + 0xFFF) & ~0xFFF)
        for (s2, e2) in segments:
            if start < e2 and s2 < end:
                return "reject:plan-overlap"
        segments.append((start, end))
        nseg += 1
    if nseg == 0:
        return "reject:plan-empty"
    return "ok" + ("+native" if native else "")


# ---------------------------------------------------- rust expectations rows
# (id, parse, code_off, code_strip, code_deny, nseg, is_native, skip,
#  target_class, target_machine, strip_prot_check)
# Negative codes follow errno sign convention; per-arch target facts come from
# Makefile ELF_MACHINE_riscv64=243 / ELF_CLASS_riscv64=2 (and arm32 40/1).
# plan_visible "no" fixtures (path/bridge context) are records with skip=1.

RUST_TARGET_RV64 = (2, 243)
RUST_TARGET_ARM32 = (1, 40)
OKR = ("1", "0", "0", "0")

RUST_ROWS = {
    # skip=1: verdict belongs to the bridge/path context, not the plan API.
    "f28-a20drv-badshentsize": ("0", "-8", "-8", "-8", "0", "0", "1",
                                "2", "243", "0"),
    "f29-a20drv-shnum0":       ("0", "-8", "-8", "-8", "0", "0", "1",
                                "2", "243", "0"),
}


def rust_row(f):
    fid, target = f["id"], f["target"]
    cls, mach = RUST_TARGET_ARM32 if target == "arm32" else RUST_TARGET_RV64
    if fid in RUST_ROWS:
        return "\t".join((fid,) + RUST_ROWS[fid])
    if fid in ("f10-trunc-ehdr-63b", "f11-trunc-ehdr-40b"):
        return f"{fid}\t0\t-8\t-8\t-8\t0\t0\t0\t{cls}\t{mach}\t0"
    if fid in ("f00-pos-exec-rv64", "f01-pos-dyn-rv64", "f13b-pos-arm32-exec",
               "f25-bss-tail-zeros", "f26-interp-missing", "f27-interp-no-nul",
               "f30-rwx-load", "f31-tls-oversize", "f32-flags-zero",
               "f40-native-marker"):
        native = "1" if fid == "f40-native-marker" else "0"
        strip = "1" if fid == "f30-rwx-load" else "0"
        return "\t".join((fid, "1", "0", "0", "-13" if fid == "f30-rwx-load"
                          else "0", "1", native, "0", str(cls), str(mach),
                          strip))
    if fid == "f21-congruence-mismatch" or fid == "f24-seg-overlap":
        code = "-22"
    else:
        code = "-8"
    return "\t".join((fid, "1", code, code, code, "0", "0", "0", str(cls),
                      str(mach), "0"))

# --------------------------------------------------------------------- main

def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "fixtures"
    fixdir = os.path.join(out, "fixtures")
    os.makedirs(fixdir, exist_ok=True)
    manifest, expectations, verdicts = [], [], []
    for f in FIXTURES:
        content = f["build"]()
        assert content == f["build"](), f"builder not deterministic: {f['id']}"
        path = os.path.join(fixdir, f["id"] + ".elf")
        with open(path, "wb") as fh:
            fh.write(content)
        digest = hashlib.sha256(content).hexdigest()
        manifest.append("\t".join([f["id"], f["family"],
                                   str(f["machine"] if f["machine"] else
                                       TARGETS[f['target']][1]), f["target"],
                                   str(len(content)), digest]))
        expectations.append("\t".join([f["id"], f["category"], f["c"],
                                       f["rust"], f["plan_visible"]]))
        verdicts.append("\t".join([f["id"], oracle(f, content), f["note"]]))
    with open(os.path.join(out, "fixture-manifest.tsv"), "w") as fh:
        fh.write("id\tfamily(class-of-bytes)\tcontent-machine\ttarget\tsize\tsha256\n")
        fh.write("\n".join(manifest) + "\n")
    with open(os.path.join(out, "expectations.tsv"), "w") as fh:
        fh.write("id\tcategory\tc-baseline\trust-plan\tplan_visible\n")
        fh.write("\n".join(expectations) + "\n")
    with open(os.path.join(out, "oracle-verdicts.tsv"), "w") as fh:
        fh.write("id\tmodel-verdict\tnote\n")
        fh.write("\n".join(verdicts) + "\n")
    with open(os.path.join(out, "rust-expectations.tsv"), "w") as fh:
        fh.write("# id\tparse\tcode_off\tcode_strip\tcode_deny\tnseg\tis_native"
                 "\tskip\ttarget_class\ttarget_machine\tstrip_prot\n")
        fh.write("\n".join(rust_row(f) for f in FIXTURES) + "\n")
    print(f"wrote {len(FIXTURES)} fixtures into {out}")

if __name__ == "__main__":
    main()
