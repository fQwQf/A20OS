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

## Ground truth from the library, and where the kernel side stands

Disassembled the real `libluajit-5.1.so.2.1.1723681758` out of the rootfs image
(`debugfs -R "dump ..."`), no boot needed.  The faulting site is confirmed exactly:

    53300: mov  0x130(%rbx),%r8     ; head
    53307: lea  0x118(%rbx),%r9     ; sentinel, embedded in the same object
    53318: cmp  %r9,%r8
    5331b: je   53368               ; empty list
    53320: testb $0x7,0x8(%r8)      ; <-- fault, r8 = 0
    53350: mov  0x18(%r8),%r8       ; node->next
    53354: cmp  %r9,%r8 / jne 53320 ; walk

So `rbx+0x130` is a circular-list head and `rbx+0x118` is its embedded sentinel.  Both are
zero in the dump while the object around them is live.  The register dump independently
agrees on identity: `a5 = rbx+0x118` is exactly the sentinel the code computed.

Kernel paths checked so far:

  * `handle_cow_fault` / `handle_cow_fault_locked` (`kernel/mm/fault.c`) hold `mm->lock`
    across the whole break and `pfa.lock` across the refcount decision, and they install the
    new PTE before releasing the old frame.  It is also **not reachable for thread-private
    memory**: threads share one mm and one page table, so a private anon page cannot take a
    COW fault in the first place.  That makes COW an unlikely owner of a lost write here.
  * `mm_tlb_shootdown_page` (`kernel/mm/vm.c`) deliberately skips the faulting CPU because
    the fault path already did `arch_tlb_flush_page_local(stval)`.  This image is `smp = 1`
    (`instances/xfce-x86_64.toml`), so there is no remote CPU to be stale either.
  * `sys_clone` / `sys_clone3` (`kernel/abi/linux/sys_proc.c`) only validate flags and
    forward `tls` to `proc_clone`.  The real work is in `kernel/proc/fork.c`
    (`proc_clone_impl:115`, `proc_clone:398`) -- **that is the next file to read.**

Why the thread path is now the leading suspect: both victims die on a corrupted **link
pointer inside a list a threaded program keeps per thread** -- LuaJIT's GC list inside
`global_State`, and musl's `dlist` inside `struct pthread`, which lives in TLS.  If two
threads ever observed the same TLS/pthread state, both lists corrupt in exactly the observed
way, and "one region of an object is zero while its neighbours are live" is what doubled-up
thread state does to a freshly initialised structure.

Next step, concretely: read `proc_clone_impl` for how the child's trap frame, FS base
(`TRAP_CTX_TP`) and stack are seeded; then write a freestanding guest probe that spawns N
threads, has each record its own FS base and pthread address, and cross-checks that no two
threads ever report the same value.  That probe either reproduces the share (and gives a
kernel fix to make) or eliminates the thread path outright.

## Eliminated: TLS / thread-state sharing

Read `proc_clone_impl` (`kernel/proc/fork.c:272-305`): the child gets a trap frame copied
onto its own freshly allocated kernel stack (`ks_top - sizeof(trap_context_t)`), `SP` is
replaced by the caller's `stack` and `TP` by `tls` only under `CLONE_SETTLS`, then mirrored
into the task context via `arch_task_context_set_user_tp`.  Nothing is shared between the
parent's and the child's frames.

Then tested it empirically rather than trusting the read.  Wrote a freestanding probe
(`/tmp/opencode/tlstest.c`, raw syscalls only) that spawns 16 threads with
`CLONE_VM|FS|FILES|SIGHAND|THREAD|SETTLS`, gives each a unique TLS block, and has each
thread, for 200 rounds:

  * read its own FS base back with `arch_prctl(ARCH_GET_FS)` and compare to the value it
    was cloned with;
  * write a unique 8-word pattern into its own slice of a shared anonymous arena, yield,
    then read the pattern back.

Result on the instrumented kernel:

    tlstest threads_done=0x0000000000000010   (all 16 ran to completion)
    bad_fs=0x0000000000000000
    bad_magic=0x0000000000000000

So every thread always saw its own FS base, and none of the 25,600 verified writes was lost
or clobbered by another thread.  **The thread-creation/TLS path is not the owner of the lost
write**, and a plain "kernel drops stores on a shared anon page" bug does not reproduce
under ordinary threaded churn either.

The mpv reproducer still crashes in the very same boot (`--load-scripts=no` -> rc=139), so
the trigger is narrower than "threads + anonymous memory".  Remaining leads, in order:

  1. The anonymous VMA holding the object had `file_fd=-1` but `off=0x2000` -- check
     VMA merge/split for anonymous mappings, since a bad merge can alias two ranges.
  2. Reproduce LuaJIT's actual access pattern instead of generic churn: mmap an arena, then
     high-frequency alloc/free plus list insert/remove, and verify the links.
  3. Keep `FAULT-BX`/`PAGE-ZEROMAP` in the dump -- they are what produced all of the above.

## Also checked: MADV_DONTNEED (contents discard)

`mm_madvise_dontneed` (`kernel/mm/madvise.c:42`) is the other path that discards page
contents.  It uses the transaction machinery (`mm_tlb_invalidate_begin/finish`), defers each
frame with `mm_tlb_hold_frame` so the free happens only after the shootdown, and keeps VMO
frames with their VMO instead of `frame_put`-ing them.  That ordering is the same discipline
the COW path uses, so this path does not obviously lose a live page either.

## Still unread: the fresh-frame install path

`handle_demand_fault` (`kernel/mm/fault.c:815`) is the path that installs a **fresh zero
frame** for a first write to an anonymous page -- i.e. exactly what a freshly `mmap`ed
LuaJIT arena gets, and the one remaining place where a page can legitimately become zeros.
It has 7 callers (`sys_mmap`, `sys_madvise`, `sys_mlock`, `sys_mlockall`,
`sys_map_shadow_stack`, `user_prepare_write`, `user_resolve_leaf`) and has **not** been read
yet.  That is the next file to inspect.

## Also checked: the fresh-frame install path (cleared)

`handle_demand_fault` / `handle_demand_fault_access` (`kernel/mm/fault.c:815-952`) refuses to
touch an already-present leaf -- `if (pte && (*pte & PTE_V)) return -1;` -- and that check
runs under `mm->lock`, which is the same lock the anonymous install path
(`handle_demand_fault_locked`, line 939) is called under.  So a live page cannot have a fresh
zero frame installed over it, which was the last mechanism that could zero a live object.

All four page-management paths are now cleared by reading: COW break, clone/TLS, MADV_DONTNEED,
demand fault.  The lost write is not in the obvious page-management code, so the next step is
behavioural, not more reading: reproduce LuaJIT's own pattern (mmap an arena, then churn
alloc/free plus list insert/remove and verify the links) instead of generic threaded churn,
which already passed cleanly.

## Also checked: VMA merge/offset (aliasing) -- cleared

The merge logic lives in `kernel/mm/vma.c`, not `vm.c`.  `vma_can_merge` (line 22) requires
offset continuity only where an offset exists: `file_offset + (end - start) == b->file_offset`
for file VMAs and the `vmo_offset` equivalent for VMO VMAs.  A plain anonymous VMA has no
backing offset, so merging two of them cannot alias anything -- the `off=0x2000` seen on the
faulting VMA is a carried value, not an aliasing hazard.

That closes the last structural lead as well.  Five hypotheses are now eliminated with
evidence: COW break, clone/TLS, MADV_DONTNEED, demand-fault install, and VMA merge aliasing.
The remaining work is behavioural: reproduce LuaJIT's own access pattern (mmap an arena,
churn alloc/free, insert/remove list nodes, verify the links) rather than generic threaded
churn -- which already passed cleanly -- and see what the kernel does differently there.

## New lead: mremap frame refcount accounting

`kernel/mm/mremap.c` is the one page-moving path not yet checked, and it is used by musl's
malloc and LuaJIT's allocator for growing large blocks -- exactly the churn mpv does.  The
move logic maps the destination to the **same physical frame** as the source and then drops
the source mapping, so correctness depends entirely on whether `pt_map`/`pt_map_huge` take a
frame reference of their own.  Both candidate functions do `frame_get(pfn)` before `pt_map`,
which only balances if `pt_map` takes a reference too:

    pt_map takes a ref:  1 -> frame_get 2 -> pt_map 3 -> unmap src 2 -> frame_put 1  OK
    pt_map takes none:   1 -> frame_get 2 -> pt_map 2 -> unmap src 1 -> frame_put 0  FREED
                                                                         while dst maps it

The second case frees a frame that a live PTE still points at, which is precisely the shape of
the symptom: the buddy hands the frame to something else, and a live object's fields read back
as zeros.

Also note `mm_move_mapping_pages` (line 128) is marked `__attribute__((unused))`, so it is
dead code; the live path is the earlier function (lines ~60-126), and the visible tail of that
one maps the destination but does not unmap the source before returning.

Next step: establish whether `pt_map`/`pt_map_huge` increment the frame refcount, then check
the live move function's accounting end to end.  This is now the most specific lead found so
far and is directly testable by reading two functions.

## mremap lead resolved: accounting is correct

`pt_map` (`kernel/mm/mm.c:258`) does **not** take a reference on the new frame -- it only drops
the replaced one (`frame_put(phys_to_pfn(old_pa))` when the old leaf had a different pa).  With
those semantics the move path balances:

    rc=1 (src mapping)
    frame_get           -> 2   ref for the destination
    pt_map(dst, pa)     -> 2   src and dst both map it, two refs   OK
    pt_unmap_leaf(src)  -> 2   pa passed as NULL, so no change
    frame_put           -> 1   one mapping left, one ref           OK

So the frame is never freed while a PTE still points at it.  The `frame_get`/`frame_put` pair
exists to keep the count above zero during the window where both mappings are live, and the
source mapping's reference is consumed by the trailing `frame_put`.  The lead is closed.

Six hypotheses are now eliminated with evidence: COW break, clone/TLS, MADV_DONTNEED,
demand-fault install, VMA merge aliasing, and mremap frame accounting.  None of the obvious
page-management code loses a write, so further reading is no longer the way to converge --
the remaining work is behavioural, reproducing LuaJIT's own pattern rather than generic
threaded churn (which already passed cleanly at 25,600 verified writes).

## COW exercised for real (fork), and it holds

The COW path had only been *reasoned* about before, and the reasoning had a gap: threads share
one page table, so a threaded program never takes a COW fault -- COW only happens after
`fork`.  The threaded probe therefore never touched it.

Wrote a second freestanding probe (`/tmp/opencode/forktest.c`) that loops 60 times: fill an
8-page mapping with the parent's pattern, `fork`, have the child rewrite every page with its
own pattern and verify it sees its own data, then in the parent verify its pattern is intact.
Every page is dirtied in the child, so each one takes a real COW break.

    forktest forks=0x000000000000003c   (60)
    bad_parent=0x0000000000000000
    bad_child=0x0000000000000000

No corruption in either direction across 60 fork/COW cycles.  COW is now cleared empirically
as well as by reading.

## Status: seven hypotheses eliminated, none by guessing

  | hypothesis | how it was eliminated |
  |---|---|
  | COW break loses data | read (locking + ordering) **and** 60 fork/COW cycles clean |
  | clone/TLS shares thread state | read **and** 16 threads x 200 rounds clean |
  | MADV_DONTNEED discards a live page | read (hold -> shootdown -> free ordering) |
  | demand fault installs a zero frame over a live page | read (present-leaf guard under mm->lock) |
  | VMA merge aliases anonymous ranges | read (offsets only checked for file/VMO VMAs) |
  | mremap frees a still-mapped frame | read (refcount derivation balances) |
  | generic lost stores on shared anon memory | 25,600 verified writes clean |

The mpv reproducer still crashes in the same boot every time (`sepc=0x638a6320`, rc=139), so
the trigger is narrower than any of the above.  Every remaining candidate is a *narrower*
mechanism, which means the next probe has to be closer to mpv's own behaviour -- not more
reading of the page-management code, which is now exhausted as a source of leads.

## BREAKTHROUGH: it is the first file-backed page read

Two controlled experiments, each one boot, same instrumented kernel, same image.

**1. It is not the flags -- it is being first.**  Flipped the harness order so the previously
passing default invocation ran first:

    mpv --no-config --audio-device=help                    -> rc=139  CRASH   (1st)
    mpv --no-config --load-scripts=no --audio-device=help  -> rc=0            (2nd)
    mpv --no-config --script=/tmp/t.lua --audio-device=help-> rc=0            (3rd)

`--load-scripts=no` had looked like the trigger only because it was always run first.

**2. It is not a race with the startup storm.**  Added `sleep 150` so the desktop was fully
settled before the first mpv run.  The first run still crashed:

    mpv --no-config --audio-device=help                    -> rc=139  CRASH   (1st, quiet system)

So the trigger is genuinely *the first mpv/LuaJIT run in the system* -- something is cold only
on the first run.

**3. Warming the page cache removes the crash.**  Added, before the first mpv run:

    for f in /usr/bin/mpv /usr/lib/*.so* /usr/lib/lua/*; do cat "$f" > /dev/null 2>&1; done

Result:

    mpv --no-config --audio-device=help                    -> rc=0   (1st!)
    mpv --no-config --load-scripts=no --audio-device=help  -> rc=0
    mpv --no-config --script=/tmp/t.lua --audio-device=help-> rc=0

No mpv SIGSEGV anywhere in the boot.  (tumblerd still crashed at `stval=0x10`, the musl dlist
signature -- it runs before the warming, which is consistent.)

**Conclusion: the bug is in the first-time read of a file page / the file-backed mapping path,
not in anonymous memory at all.**  The warm runs find the pages already in the page cache and
never exercise it, which is why every later run succeeds.

That also explains the shape of the symptom for the first time: if the frame being filled by
the disk read is simultaneously in use as an anonymous heap frame, the read overwrites the
heap object's bytes with file contents -- and a zero-filled stretch of the file lands as a
**zero hole inside a live object**, exactly the `rbx+0x118..+0x148` pattern measured earlier.

Next step: read `handle_file_fault` and the page-cache refill path in `kernel/mm/fault.c` for
a frame that is used both as a page-cache page and as an anonymous frame (or a read that writes
into the wrong frame).  That is now the only open lead, and it is a specific one.

## The concrete suspect: a short read is treated as EOF

`page_cache_fill_vfile_page` (`kernel/fs/page_cache.c:624`) is the first-time fill, and its
short-read handling is the one place that manufactures **zeros** in a file-backed page:

    int r = vf->ops->read(vf, (char *)data, PAGE_SIZE);
    ...
    if ((size_t)r < PAGE_SIZE)
        memset((char *)data + r, 0, PAGE_SIZE - (size_t)r);

That is correct at EOF (the tail of a file's last page is genuinely zeros) but wrong for a
**short read in the middle of a file**: the page is then marked uptodate with its tail zeroed,
and because it is uptodate from then on, every later reader gets the zeros without re-reading.
That matches the breakthrough exactly -- a zero region inside otherwise-live content, on the
first access only.

This is the lead to confirm and fix: check whether `vf->ops->read` can return a count below
PAGE_SIZE mid-file (and whether the `vnode->ops->readpage` branch has the same assumption),
then make the fill loop until the page is full or the true EOF is reached, rather than
zero-filling on any short count.

## Tried and failed: looping the page-cache fill past a short read

Hypothesis: `page_cache_fill_vfile_page` zero-fills the rest of the page whenever
`vf->ops->read` returns below PAGE_SIZE, so a short read mid-file would leave a zero tail that
later readers hit without re-reading.  The fill also does `lseek`/`read`/`lseek` on the shared
`vf->offset` while holding only the *per-page* `fill_lock`, so concurrent first-time fills of
different pages of one file can interleave on that offset.

Applied the change -- read in a loop until the page is full or a true EOF (read returns 0),
and zero-fill only what is genuinely past EOF -- then rebuilt and booted **without** any
page-cache warming.

Result: **the first mpv run still crashed**, same `sepc=0x638a6320`, rc=139.  The change was
reverted (`git checkout -- kernel/fs/page_cache.c`) rather than left in the tree, because it
did not fix anything and is not verified.

So the trigger really is the first file-backed read -- that part is proven by the warming
experiment -- but this is not how it corrupts memory.  The remaining candidates in that path
are the ones the fill does *around* the read: the page-cache frame's identity/refcount when a
file page is installed into a user PTE, and whether that frame can simultaneously be in use as
an anonymous frame.  That is the next thing to instrument, not more reading.

## Cold file fills alone do not corrupt anon memory -- concurrency is the missing piece

Wrote a third freestanding probe (`/tmp/opencode/filefill.c`): hold four patterned anonymous
regions, then `mmap` mpv / libluajit / libc / ffmpeg and touch one byte per page so every page
takes a **real first-time page-cache fill**, re-verifying all four anonymous regions after every
single touch.

    filefill files=0x3          (3 of the 4 mapped)
    pages_touched=0x386         (902 cold pages)
    bad_anon=0x0                (no anonymous corruption at all)

And the three mpv commands all returned `rc=0` in that same boot -- not because of any fix, but
because the probe incidentally warmed the page cache.  That is now the third independent
confirmation that warming removes the crash.

So: cold file fills by themselves do not disturb live anonymous memory.  The two things the
probe does **not** do that mpv does are

  1. fault those pages from **several threads at once**, and
  2. **execute** from them (the probe only reads; mpv runs code out of the mappings).

(1) is the stronger candidate and lines up with the code: `page_cache_fill_vfile_page` does
`lseek`/`read`/`lseek` on the shared `vf->offset` while holding only the **per-page**
`fill_lock`, so two threads filling *different pages of the same file* interleave on that
offset and can fill a page from the wrong file position.

Next probe: the same test but with N threads faulting disjoint page ranges of one file
concurrently, while the main thread verifies the anonymous patterns -- and also verify the
**file** contents read back against a known-good copy, since a wrong-offset fill corrupts the
file page rather than the anonymous one.

## Confirmed at code level: the fill path never takes the offset lock

`grep offset_lock` shows the lock is taken **only in the syscall layer**
(`kernel/abi/linux/sys_fs.c` -- read/pread/lseek) and never inside the vfile
`ops->read`/`ops->lseek` implementations.  `page_cache_fill_vfile_page` calls those ops
**directly**, so the shared `vf->offset` is manipulated with no lock at all:

    A: lseek(page1)   B: lseek(page2)   A: read -> gets page2's bytes   B: read -> page2 again

Two threads filling *different pages of the same file* concurrently therefore fill a page from
the wrong file position.  This fits every measured fact:

  * first access only -- once a page is uptodate it is never refilled, so warm runs never race;
  * threaded programs only -- a single-threaded fill cannot interleave;
  * the corrupted thing is the **file** page, not the anonymous one -- which is why the
    filefill probe reported `bad_anon=0`.

Fix direction: serialise the fill's offset manipulation per vfile.  It must **not** simply take
`vf->offset_lock`, because a page fault taken inside a `read()` syscall would re-enter and
deadlock on that same non-recursive mutex (the comment at `sys_fs.c:164` warns about exactly
this re-entry).  Either add a dedicated per-vfile fill mutex, or give the fill a positional read
so it never touches the shared offset.

## Fix recipe (next change to make)

The codebase already solves this exact problem one layer down: `kernel/fs/block_cache.c`
serialises its own fills with a **striped lock array**

    mutex_t *fill_lock = &bc->fill_locks[pcache_hash_key(page_no) & (PCACHE_FILL_LOCKS - 1)];
    mutex_init(&bc->fill_locks[i]);      /* at block-cache init */

with `mutex_init` / `mutex_lock` / `mutex_unlock` / `mutex_trylock` as the API (there is no
static-initialiser macro, so the array must be initialised at module init).

Apply the same shape to the vfile page cache so the fill's offset manipulation is serialised
per vfile:

  1. `kernel/fs/page_cache.c`: add a small striped `mutex_t` array (or one mutex per vfile if a
     field on `vfile_t` is preferred), initialised wherever the page cache is initialised.
  2. In `page_cache_fill_vfile_page`, take that lock around the whole
     `lseek(page_base)` -> `read` -> `lseek(saved)` sequence, and release it before returning.
  3. Do **not** use `vf->offset_lock` for this: a page fault taken inside a `read()` syscall
     re-enters and would deadlock on that non-recursive mutex (`sys_fs.c:164` documents the
     re-entry).
  4. Keep `page->fill_lock` as-is; it serialises fills of the *same* page, while the new lock
     serialises the shared offset across *different* pages of the same file.

Acceptance test (no page-cache warming anywhere in the harness):

    mpv --no-config --audio-device=help                     -> rc=0
    mpv --no-config --load-scripts=no --audio-device=help   -> rc=0
    mpv --no-config --script=/tmp/t.lua --audio-device=help -> rc=0
    and no lua-thread SIGSEGV in the kernel log

with the harness still running tlstest / forktest / mmapchurn / filefill first, since a
regression in any of those counters (bad_fs, bad_magic, bad_parent, bad_child, bad_slot,
overlaps, bad_anon) means the change is wrong.

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

## Measured: the object is live, the link fields are a hole

Booted the instrumented kernel three times; the crash reproduces every time.  The dump
now prints the object behind `rbx` and a per-64-byte-block all-zero map of its page.

Register facts re-confirmed on a fresh dump (`comm=lua/stats`):

    a0 (rdi) = 0x77dbe380          lua_State
    a0 + 0x10 = 0x77dbe3f0         == rbx  ->  rbx is the global_State
    insn@sepc = 41 f6 40 08 07     testb $0x7, 0x8(%r8) with r8 = 0   (stval = 0x8)

So the decoded instruction and the identity of `rbx` hold.  What the object dump adds:

    rbx+0x100  ffffffffffffffff
    rbx+0x108  ffffffffffffffff
    rbx+0x110  fffa000077c56400      last live word
    rbx+0x118  0000000000000000  <--  sentinel, zero
    rbx+0x130  0000000000000000  <--  the head r8 loaded, zero
    rbx+0x138  0000000000000000
    rbx+0x148  0000000000000000

The rest of the object is healthy: live pointers at `+0x28`, `+0x30`, `+0x40`, `+0x48`,
`+0x70`, `+0x98`, `+0xc0`, `+0xe8`, `+0x110`.

Page state at the fault:

    refs=1  fflags=0x1              private, not COW-shared at fault time
    nonzero_bytes = 1288 / 4096     ~31% live, the rest is untouched space
    all-zero 64B blocks = 0xffcffffe30001c00

That bitmap is a normal heap arena: live chunks separated by never-written (still zero)
free space — blocks 8-12, 28-29, 33-51, 54-63 are free, 52-53 and 30-32 are live.

**What this rules out:** the whole page or the whole object being lost or replaced.  The
object is live; only the link field group (`+0x118` onward) is a hole in it.

**What it leaves:** the write that initialises those link fields never landed, while the
writes to the fields before them did.  That is a lost-write in the strict sense, and it
is why the next step is to audit the kernel write paths rather than read more of LuaJIT:
the userspace side is stock Alpine and its stores are correct.

  Next concrete probes, cheapest first:

  1. Wake the fault dump for a *userspace store* (a `CAUSE_PAGE_*` write fault) that maps
     a fresh zero frame, and check the frame is actually installed before the retry.
  2. Check the `munmap`/`mremap` paths for a region overlapping a live heap page, since a
     partially freed page would show exactly this free-space-with-live-objects layout.
  3. Keep the `FAULT-BX`/`PAGE-ZEROMAP` dump in place -- it costs nothing unless a
     SIGSEGV happens and it is what produced all of the above.


## The systemic crash class

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


