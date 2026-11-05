# Release Verification & Version Compliance Tools (`tools/release/`)

This directory contains version auditing, release baseline comparison, and security acceptance harnesses used across milestone releases.

## Files

| File | Type | Description |
|------|------|-------------|
| `check-release-version.py` | Python | CI gate verifying version string alignment between kernel headers and git tags |
| `security_release.py` | Python | Automated acceptance runner executing security and Landlock regression tests |
| `v1_compare.py` | Python | Differential comparison runner asserting output parity between C and Rust subsystem implementations |
| `v1_nat_host.py` | Python | C0 baseline host oracle generator for netfilter NAT rewrite |
| `snapshot_build_inputs.py` | Python | Audits and hashes reproducible build input snapshots |
