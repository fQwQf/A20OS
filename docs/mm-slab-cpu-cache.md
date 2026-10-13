# Slab CPU-array ownership across refill and drain

The small-object allocator's fast paths access their CPU-local arrays with
interrupts disabled and without the slab-cache lock. Its slow paths must retain
that CPU ownership through the entire refill or drain as well.

Previously `kmalloc_flags` selected an empty local array, re-enabled interrupts,
and then took the slab-cache lock to refill the previously selected array. A
migration in that window leaves the task accessing another CPU's array while
that CPU's fast path can modify it concurrently. `kfree` had the same window
between observing a full local array and draining eight entries. The cache
lock does not protect against a fast path that deliberately avoids that lock.

Both paths now retain the initial IRQ exclusion through the bounded cache
transaction and restore the caller's original IRQ state after unlocking. No
architecture-specific policy is needed.

```sh
python3 tools/tests/test_slab_cpu_migration.py
```

The host probe extracts the real refill/drain code and injects migration when
interrupts are enabled. Both paths fail the CPU-ownership assertion with the
preceding source; the fixed source passes, including callers already entering
with interrupts disabled. Local negative evidence is in
`build/browser-followup/slab-migration-negative2.log`.

The x86_64 full VM smoke and RISC-V 4-CPU preemptible
`mtcorrupt_test`, `vm_fault_test`, descriptor passing and network-option probes
pass (`slab-fixed-full-guest.log`, `slab-fixed-rv-guest.log`).

A browser run before this fix captured a cycle joining dirty-page chains from
different vnodes. A CPU held the page-cache lock while traversing that cycle,
other CPUs spun with IRQs masked, and a COW shootdown then timed out. The slab
migration race is a demonstrated allocator defect that can corrupt unrelated
objects; the chain corruption's exact first write was not captured. Browser
cold-boot validation must therefore be recorded separately rather than treating
this host probe as proof of that particular failure's cause.
