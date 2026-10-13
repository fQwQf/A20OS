# `MREMAP_DONTUNMAP` validation record

Status: **open; private-anonymous functional and both deterministic rollback
slices pass, while broader MM acceptance remains unverified.** These records are QEMU functional
evidence only. They are not a physical-hardware result, a performance
comparison, or a validated C0 baseline.

## Current functional slice

The focused command is `python3 tools/smoke.py smoke-mm-dontunmap`. It builds a
separate RV64 Linux SMP2 image and runs `mm_stress --dontunmap-only`. The case
checks invalid flag/length/overlap requests, destination data retention, zero
refault at the retained source VMA, independent writes, fork/COW behavior, and
resident-page accounting through cleanup. The latest 2026-10-13 candidate-04
run passed with RSS pages `14→17→17→19→15`; shutdown audit errors were zero
and the `[MM-ASM]` audit reported zero missing metadata, PTE/status/VMA
mismatches, and unsafe entries.

The ignored run artifacts are under
`.kernel-build/evidence/mm-dontunmap/candidate-04/`. The guest-log SHA-256 is
`46e059a9af229c7a3ad1c39c88a2f4b66a69ab663d7a3167d17e54cc52ddcba8`; the
kernel ELF is `ec68aebf8fce896a3a710d8a65179ad60d84d3b9d61b4001a231873d927f6aab`.
The successful runner transcript is `runner-retry-01.log`; the first build
attempt is preserved in `runner.log` and failed before QEMU because
`dontunmap` was unused when the test-only hook was disabled. The fix explicitly
marks the argument unused in that configuration. The exact QEMU argv is
retained by the runner and `tools/smoke_cases.py`. QEMU does not establish physical MMIO, IRQ, DMA, bus
coherency, or timing behavior.

The test source was built from checkout `20193475564536521e4ee6d53081238cd4de6558`
plus the working-tree changes. At the end of the runs, the tracked test inputs
had these SHA-256 values: `kernel/mm/mremap.c`
`9b2b1413c3bb7d887ec0da090a35009fb4b97bd4aa34ef72541e6e62da1fda37`,
`user/cmds/stress/mm_stress.c`
`6ba64f220f7d42aa2cc2d834895f6b73849367920824202fa8b644d1d6cbee1d`,
`tools/smoke_cases.py`
`ccc4ec6e67d4c7db4090e5c158366aedcf7a12ee72efe4b62b6f1869c96ee439`, and
`tools/targets-smoke.mk`
`9feaff5c233dc284ae7e26c93166b760a617bbf0f1eaaacef94da07f6f55a73e`.
The runner transcript hashes are `44b206134d1379da1211a7d25564277f80bdb9e38375d9a35b40e9e87c8e0b35`
(first failed build) and `8585788448a721444a1203bc60b250bb86d0bc8da7c0052d6d318a9e7cae16a0`
(successful retry).

## Controls and failed attempts

`current-cn-control-01/guest.log` is the pre-fix working-tree Cn control, not
the `v0.18` C0 baseline. It recorded RSS `18→21` across the old remap behavior;
that run predates the independent source-zero assertion and therefore does not
claim that assertion was measured. Its log SHA-256 is
`f20aa14851634fe8eb6dbfdf82bb64c01b2a1d8886fee45569fef8059145eba6`.

`candidate-01/guest.log` is a failed early candidate attempt: source refault
was observed to return zero, but the test incorrectly required total RSS to
remain exactly unchanged despite one extra test-stack page (`18→19`). Its log
SHA-256 is `5a00acd84c8257df1648b9905f61ea719964fb685232147e30652b1c9e6c1d58`.
The corrected candidate-03 test warms the stack before taking its baseline and
uses exact RSS deltas; it does not use a tolerance.

`rollback-candidate-01/guest.log` checked source preservation and successful
reoccupation of the destination after an expected `ENOMEM`, but did not check
RSS or same-process retry. Its SHA-256 is
`237249b9adbfc44f41f9ae06e01149609d47c37f637a6bab7368cb2bb7f233f2`.

`rollback-candidate-02/guest.log` is a failed acceptance attempt: after the
source-data check, RSS changed `12→17`, so the assertion stopped the scenario
before destination-release or retry checks. It had no explicit kernel marker
confirming that the intended hook caused `ENOMEM`; retain it as a failed,
unconfirmed-injection run, not a pass. Its SHA-256 is
`d597b985d13c8f72d27a986cda28c63d7250959d61554639e31866cb4f4395ef`.

## Rollback tests

The new rollback cases use a test-build-only, one-shot injection on the
DONTUNMAP path. They emit and require `MM_DONTUNMAP_TEST:
injected-failure leaves=N` after the Nth destination leaf has actually been
mapped. N=1 exercises partial destination rollback; N=2 exercises failure
after the two-page destination is fully populated and before source teardown.
Both cases then check source data and writability, exact RSS preservation,
destination release, and successful retry in the same process. On 2026-10-13,
both ran and passed under RV64 Linux SMP2 QEMU:

| Injection point | Guest result | Guest-log SHA-256 | Kernel ELF SHA-256 |
| --- | --- | --- | --- |
| N=1, after first destination leaf | `injected-failure leaves=1`; `source_preserved=1 destination_cleared=1 retry=1 rss=16` | `7d39f0104ec668f52876f5b597a1321ca9d3dc9f77c61eb1a71cbe10a5c6f9cf` | `df20a4ad7a5ceec8c4a479eeeb4c6373d349dffc2214c2088b3610a46213c78a` |
| N=2, after second destination leaf | `injected-failure leaves=2`; `source_preserved=1 destination_cleared=1 retry=1 rss=16` | `2089f95d17dd68cb7e8fbc878a3a0e7c22e99adb3b461db21f0e16e0f9dbea2d` | `a6d6d7cc7542c4f324c4b23eba0ce50735af87a56c7cdd3c762217d356fb4a5b` |

Full runner and guest logs are under
`.kernel-build/evidence/mm-dontunmap/rollback-leaf{1,2}-candidate-01/`.
The N=1 runner log SHA-256 is
`e457d5ad390356a09d5e13868b8b13dcb9d21ef84a54abed699fd358412294a7`; N=2 is
`02a2c657c620b94bf7f3c51efe7ab607ef42f704f63ce946bbdbc7001c16a119`.
Both runs reported shutdown audit errors=0 and zero MM-ASM metadata, PTE,
status, VMA, and unsafe-entry discrepancies. The explicit per-run kernel marker
confirms each intended failure point was reached. The deterministic fault is
build-only and one-shot; the same guest process reoccupied the fixed destination
and retried successfully after each failure.

The same candidate-04 kernel and FAT32 image then ran the repository's complete
`mm_stress` suite under RV64 Linux SMP2 QEMU without rebuilding. It exercised the
existing file-backed DONTUNMAP coherence/writeback and page-cache pin-count
assertions together with fork, remap, eviction, huge-page, and VMA stress; the
suite printed `MM_STRESS: PASS`, poweroff audit errors were zero, and all MM-ASM
structural mismatch counters were zero. Transcript SHA-256:
`a741ff2fe0b7782e1dcfd7592e3257bb9b0330de4341f59b0de54386a2e84249`; archived
at `mm-stress-full-rv64-smp2-candidate-04.log` beside `manifest.json`.

Complete isolated build trees and a second copy of every MM evidence log are
archived on the larger data volume at
`/media/fqwqf/Windows-SSD/a20os-g1-task-identity-20261013/mm-dontunmap-rv64-20261013/`.
Its `manifest.json` records each command, result, and artifact hash. The archive
was compared with the local build/evidence trees before local
generated objects and images were removed to recover workspace disk space.

## AArch64 aggregate and Cn control

The AArch64 ABI=both SMP2 aggregate was run on 2026-10-13 with QEMU. Its
focused anonymous DONTUNMAP phase passed (`rss_pages=16→20→20→22→18`). The
full `mm_stress` phase then passed the existing file-backed DONTUNMAP,
merge/read/write/eviction, and huge-basic phases, but failed at
`huge-fork-child`. The child repeatedly store-faulted in the huge-page write
loop (`pc=0x126d8`, `va=0x12a400000`, decoded cause 37) until the trap-storm
guard killed it. This aggregate remains a failure; its full test scope has
not been weakened. The standalone focused target is now named
`smoke-mm-dontunmap-aarch64-both-focused`; the original
`smoke-mm-dontunmap-aarch64-both` continues to run the full aggregate.

An isolated Cn control used the same AArch64 build inputs except for
`kernel/mm/mremap.c`, which was replaced with `HEAD:kernel/mm/mremap.c`
(`bb0f72…`). Both current-candidate and Cn kernels ran the same diagnostic
`mm_stress --huge-fork-only` workload with the same FAT image and user ELF.
Both reproduced the huge-fork-child failure: cause 37 is the kernel's
`CAUSE_STORE_PAGE_FAULT` code (`0x25`), the wait status was `raw=0x9`, and the
child died by SIGKILL after 4096 repeated faults. The exact user PC resolves to
the child `strb` in `huge_page_fork_cow()`. This makes the observed AArch64
failure a pre-existing huge-page COW/fault issue under this control; it does
not establish DONTUNMAP correctness on the whole architecture or resolve the
separate COW defect.

The original failed aggregate and isolated control are archived at
`/media/fqwqf/Windows-SSD/a20os-mm-dontunmap-20261013/aarch64-original-candidate-01/manifest.json`.
The manifest contains the original guest/runner logs, kernel and FAT image
hashes, source inputs, and paths/hashes for the isolated candidate/Cn kernel,
same-image huge-fork runs, and build transcript. The successful focused phase
is included in the failed aggregate guest log; the renamed standalone focused
target has not yet been run independently.

## Remaining checks

The separate AArch64 huge-page COW failure needs its own fix and validation.
DONTUNMAP still needs swap-in source behavior, huge-page-boundary behavior,
concurrent mutation, allocation-pressure variants, and broader cross-architecture
coverage. The RV64 full MM stress pass covers shared-file DONTUNMAP
coherence/writeback and existing fork/remap/eviction/huge-page cases, but it
does not close those focused boundaries. No performance calibration or
validated C0 comparison exists for this MM case.
