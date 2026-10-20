# NAT host-oracle comparison

The fixed C0/Cn/Rust candidate host comparison is registered in
[`../../tools/tests/nat-host-comparison.toml`](../../tools/tests/nat-host-comparison.toml). It compiles the shared
host oracle against the C source at commit
`9526af438a73eb834169d002536f8e57e21f24a8`, the current C reference, and the
Rust selector. The C0 adapter extracts only that source file into each fresh
runner directory; it does not checkout a revision or write source into the
worktree. Before compiling, it asks the selected compiler for the actual
project-header dependency closure and rejects any header whose bytes differ
from the pinned commit (or which is absent there). It records the source,
fixture, and every checked header hash, exact dependency/compile commands,
compiler version, built binary hash, and run output hashes in each C0
`manifest.json`. A changed header requires extracting the historical closure
instead of silently compiling C0 against newer definitions.

The oracle builds Ethernet/IPv4/TCP/UDP frames and independently re-sums the
IPv4 and transport checksums after rewrites. Its expected contract pins the
44 named checks with `check_names_fnv1a32=3915109928`. The registered case is
functional-only. It does not treat host elapsed time as kernel performance,
and it does not establish guest packet-path behavior.

Run it after the C/Rust selector adapter is available:

```sh
python3 tools/v1_compare.py tools/tests/nat-host-comparison.toml \
  --out build/evidence/nat-host-$(date +%Y%m%d-%H%M%S)
```

The runner performs two C0 executions, two Cn executions, and one candidate
execution in a seeded order, retaining source/binary hashes and logs under the
new evidence directory. A matching functional contract can report `PASS` for
the case. Since C0 and Cn are explicitly unvalidated, the aggregate remains
`EVIDENCE_INSUFFICIENT`; this is the expected conservative result until the
full baseline matrices are accepted. No performance calibration is registered
or implied here.

## Current evidence

The first paired run is retained at
[`build/evidence/nat-host-20261013-01/report.json`](../../build/evidence/nat-host-20261013-01/report.json)
as preliminary evidence from before automated header-closure enforcement.
The header-closure rerun is retained at
[`build/evidence/nat-host-20261013-02/report.json`](../../build/evidence/nat-host-20261013-02/report.json)
with its C0 manifests. The final rerun, using the current runner with strict
schema validation, is
[`build/evidence/nat-host-20261013-03/report.json`](../../build/evidence/nat-host-20261013-03/report.json).
Its schedule contained two fixed-C0 runs, two current-C runs, and one Rust
candidate run. All five returned zero and matched the expected 44-check
contract with zero failures and digest `3915109928`. Each C0 manifest verified
the same three-header closure and recorded the source, fixture, compile
command, compiler, binary, and output hashes. The functional case is `PASS`;
the aggregate is `EVIDENCE_INSUFFICIENT` because C0 and Cn are not validated.
No guest NAT test, Rust performance claim, or C0 validation is recorded by
this evidence entry.
