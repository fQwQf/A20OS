# Binary Translator & IDL Interop Tools (`tools/xlator/`)

This directory contains executable translators (qemu-user / binfmt), foreign architecture ELF loaders, and IDL wire-format verification fixtures.

## Files

| File | Type | Description |
|------|------|-------------|
| `xlator_fetch.py` | Python | Fetches, unpacks, and validates static host translation ELFs from Alpine packages |
| `xlator_guests_audit.py` | Python | CI gate (`check-xlator-guests`) verifying guest table declarations against translator binaries |
| `gen_linux_syscall_coverage.py` | Python | Cross-references Linux ABI coverage matrices |
| `rust_idl_peer_wire_check.c` | Host C | C-side wire representation assertions against Rust IDL peer channels |
| `run-idl-ack-guest.py` | Python | Standalone IDL ACK transaction runner in guest environments |
| `run-idl-ack-matrix.sh` | Shell | Cross-architecture matrix test driver for IDL ACK verification |
