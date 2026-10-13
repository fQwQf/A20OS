# Online CPU reporting

`/proc/cpuinfo` used to return one fixed `rv64gc` / `sv39` record on every
architecture. Minecraft therefore displayed `CPU: 1x` on a four-CPU x86 guest,
even though its JVM GC and Mesa llvmpipe workers already used all four CPUs.

The shared renderer now snapshots the online mask and emits one processor
record per online logical CPU. Architecture-owned fields come through
`arch_cpuinfo_fields()`; x86 obtains vendor and model strings with CPUID in
`kernel/arch/x86_64/platform/arch_hooks.c`. The fallback reports the architecture
name without inventing an ISA, MMU mode or hardware topology. The open-file
buffer scales with the configured CPU capacity; procfs reports size zero, as
these are generated records.

`cpuinfo_test` checks distinct processor IDs, coverage of the calling thread's
affinity, and an optional expected CPU count. `make smoke-cpuinfo` exercises a
one-CPU RISC-V guest. Additional four-CPU, preemptible x86 and RISC-V runs passed
alongside `sigchld_group_test`:

- `build/browser-followup/cpuinfo-x86-guest.log`
- `build/browser-followup/cpuinfo-rv-guest.log`

The same x86 test with the preceding kernel failed with
`CPUINFO: FAIL processors=1` (`cpuinfo-negative-guest.log`). The new x86 output
contains four `AuthenticAMD` / `QEMU Virtual CPU version 2.5+` records. This
corrects processor reporting; it does not establish the cause of low game FPS.
