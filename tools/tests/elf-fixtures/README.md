# ELF parser test fixtures

This directory contains a deterministic generator, oracle driver, TSV expectations, and SHA-256 manifest for ELF32/ELF64 parser and planning cases. Generated ELF binaries go in a caller-selected temporary directory; none are stored in the repository.

Run the full host check from the repository root:

```sh
sh tools/test-elf-exec-host.sh
```

Or generate inputs for inspection:

```sh
python3 tools/tests/elf-fixtures/generate_fixtures.py /tmp/a20-elf-fixtures
```

## Boundary contract

The Rust oracle checks malformed/truncated headers and program-header tables, checked file/virtual-address bounds, segment extent and overlap rules, alignment, permissions, TLS, interpreter, and Native ABI cases. `rust-expectations.tsv` is the oracle input; `fixture-manifest.tsv` records generated input hashes and ELF metadata. The recorded C-baseline verdicts are historical comparison observations, not expected Rust results. The parser requires `off <= len` and `size <= len - off` before reading; segment file extents must fit memory extents and load ranges must not overlap. Host replay checks the parser/planner and does not replace guest loader validation.
