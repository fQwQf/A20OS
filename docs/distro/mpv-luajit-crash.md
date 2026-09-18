# mpv / LuaJIT crash — deterministically reproduced

`comm=lua/console`, `path=/extra/usr/bin/mpv`, faulting library
`/extra/usr/lib/libluajit-5.1.so.2`.

## Reproducer

    mpv --no-config --audio-device=help

The main process exits 0; the crash is in mpv's `lua/console` thread.  Standalone LuaJIT
is *not* affected — `luajit -v`, `luajit -e 'print(1)'`, a 300 000-entry table with GC,
and a 3 000 000-iteration JIT loop all complete normally.  So this is LuaJIT as embedded
in an mpv thread, not LuaJIT by itself.

Fault signature, identical on every run:

    SIGSEGV: pid=210 code=13 sepc=0x638a6320 stval=0x8 abi=0
    vma_file=/extra/usr/lib/libluajit-5.1.so.2
    vma=[0x63858000,0x638cb000) flags=0x2005 file_fd=1018 off=0x5000   (lib vaddr 0x53320)
    insn@sepc=0x0840f641

## What the faulting instruction is doing

Disassembling `libluajit-5.1.so.2` at the library vaddr `0x53320`:

    53300: mov    0x130(%rbx),%r8        ; r8 = list head
    53307: lea    0x118(%rbx),%r9        ; r9 = sentinel address (inside the same object)
    5330e: movabs $0x7fffffffffff,%r10
    53318: cmp    %r9,%r8
    5331b: je     53368                  ; head == sentinel -> list empty, done
    53320: testb  $0x7,0x8(%r8)          ; <-- FAULT: reads [r8+8] with r8 == 0
    53325: jne    53350
    53327: mov    0x20(%r8),%rax         ; follow object field
    5332b: mov    (%rax),%rsi
    53331: sar    $0x2f,%rax             ; LuaJIT tag extraction
    53338: cmp    $0xfffffff6,%eax
    53350: mov    0x18(%r8),%r8          ; r8 = r8->next
    53354: cmp    %r9,%r8
    53357: jne    53320                  ; walk

This is a circular-list walk whose only "empty" test is `head == sentinel`.  A head of
**NULL** is therefore an invariant violation: the field at `rbx+0x130` holds 0 where it
should hold either the sentinel address or a valid node.  In other words a pointer that
was written is not there — a lost write, or a second writer zeroing the same page.

`rbx` itself is fine: it points into an anonymous RW mapping (`flags=0x13`), so the
object is real and mapped; only that one field is wrong.

## Ruled out

* **More raw-frame writes bypassing COW** (the class fixed three times already).  In the
  current tree `user_resolve_leaf()` is `static` in `kernel/mm/mm.c` and its only callers
  are that file's own copy helpers, which pass the `write` flag through
  `user_prepare_write()`.  No other kernel file resolves user leaves directly any more.
* **Standalone LuaJIT / the JIT itself**: every standalone `luajit` invocation succeeded,
  including GC stress and a 3M-iteration JIT loop.
* **Blaming mpv's audio**: the crash happens with `--no-config` and with no audio device
  open, in the scripting thread.

## Hypotheses to test next

1. **A lost write on a thread path.** The crash is in a thread created by mpv, and the
   earlier three fixes were all about writes that a process expected to see.  Audit the
   thread/`clone` path and anything that writes into a child's address space (CLONE_VM
   sharing, `set_tid_address`, TLS/FS setup, signal frames) for the same COW hazard.
2. **Overlapping or stale mappings.** If two threads' mappings can alias (mmap hint /
   MAP_FIXED / mremap edge cases), one thread initialising its `lua_State` could zero
   another's GC list head.  Worth testing with a mmap/mremap stress that shares pages
   between threads.
3. **Thread-entry mismatch.** LuaJIT keeps per-thread state; if the kernel's thread entry
   delivers a wrong user SP/FS or a wrong argument, the embedded state would be the wrong
   object — but the object address here is valid, so this is the weakest of the three.

## Packaging note

Any userspace part of the fix (a patched mpv, LuaJIT, or an added helper package) is to
be delivered as an apk via `packages/recipes/`, not by copying files into an image.

## Update: it is not an mpv bug

Isolation run:

    mpv --no-config --load-scripts=no --audio-device=help     -> rc=139  (still crashes)
    mpv --no-config --script=/tmp/t.lua --audio-device=help   -> rc=0
    mpv --no-config --audio-device=help                       -> rc=0

Disabling the scripts does not prevent it, so this is not about loading a script — mpv
initialises LuaJIT on its scripting thread regardless.

In that same boot **tumblerd crashed too**, at a different PC:

    FATAL: pid=163 signal=11 pc=0x476a602b  comm=tumblerd           path=/extra/usr/lib/tumbler-1/tumblerd
    FATAL: pid=193 signal=11 pc=0x638a6320  comm=lua/console        path=/extra/usr/bin/mpv

With the earlier python3 crash (null dereference inside `libpython3.12`), that is three
unrelated threaded programs failing with small `stval` values.  The crash class is
therefore **systemic**, and the LuaJIT instruction decoded above is one *symptom* of it,
not the disease.  Any fix has to be found on the platform side.

TLS itself was checked and looks right: `ARCH_SET_FS` stores the value in the task's trap
frame (`TRAP_CTX_TP`) and the clone path sets it from the `tls` argument, so a per-thread
FS base is maintained.  That hypothesis is weakened but not eliminated.

## Next steps for the systemic class

### Both crashes are the same kind of failure: a NULL link pointer

The two dumps side by side:

| | mpv (`lua/console`) | tumblerd |
|---|---|---|
| PC | `0x638a6320` in `libluajit-5.1.so.2` (vaddr `0x53320`) | `0x476a602b` in `ld-musl-x86_64.so.1` (file off `0x4602b`) |
| fault VA | `stval=0x8` | `stval=0x10` |
| what it was doing | list head at `G+0x130` was NULL where it must be the sentinel or a node | musl `dlist` self-pointer check with the node pointer NULL |

Both are **link pointers in a list that threaded programs maintain** — LuaJIT's GC list
inside its `global_State`, and musl's thread list (`dlist`) inside `struct pthread` —
found to be NULL where a valid pointer was written.  That is a lost write, not a bad
computation, and it is why this has to be fixed in the kernel: the userspace side is
stock Alpine and correct.

### The one experiment that separates the two remaining explanations

Whether the *whole* object is zeroed (the page was replaced or the mapping is stale) or
only the link field is zero (a single write was lost) decides between "mapping/COW bug"
and "lost write".  The dump already prints the page behind `stval`, `a0`, `sp` and `fp`;
the object of interest sits in `rbx`, so add one more block in
`kernel/core/trap.c` next to the existing ones (around line 349):

    if (TRAP_CTX_RBX(ctx) && TRAP_CTX_RBX(ctx) != stval) {
        kerr("  [FAULT-BX] rbx=0x%lx\n", (unsigned long)TRAP_CTX_RBX(ctx));
        dump_fault_pte(cur, TRAP_CTX_RBX(ctx));
    }

(`TRAP_CTX_RBX` may need adding alongside `TRAP_CTX_ARG0`/`TRAP_CTX_SP`/`TRAP_CTX_FP`.)
Rebuild, run the mpv reproducer, and compare the words around `rbx+0x130` with the words
around `rbx`: a wall of zeros means the object was never initialised (mapping/COW side);
only the link being zero means one write went missing.

### Then

1. **Trace the kernel writes into a thread's memory** — `clear_child_tid`, robust list,
   rseq, and any TLS install.  That is exactly the family of the three bugs already fixed
   (raw writes that bypassed COW), and a fourth member would explain a systemic
   lost-write class.
2. **Build a minimal threaded reproducer** (spawn a thread, hammer TLS and a small heap)
   so the failure can be bisected without mpv and tumblerd.


