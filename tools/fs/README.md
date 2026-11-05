# Filesystem & Storage Test Tools (`tools/fs/`)

This directory contains standalone filesystem formatting tools and crash-recovery verification gates used by the smoke testing and image generation pipelines.

## Files

| File | Type | Description | Make Target / Usage |
|------|------|-------------|---------------------|
| `ext4_journal_gate.py` | Python | Multi-architecture ext4 journal recovery gate (asserts clean journal commit) | `smoke-ext4-journal` |
| `ext4_legacy_journal_gate.py` | Python | Ext4 journal recovery regression test on legacy images | `smoke-ext4-legacy-journal` |
| `ext4_unjournaled_recovery_gate.py` | Python | Crash recovery verification on unjournaled ext4 filesystems | `smoke-ext4-unjournaled-recovery` |
| `ext4_xattr_gate.py` | Python | Ext4 extended attributes (`xattr`) persistence and inline/block gate | `smoke-ext4-xattr` |
| `mkfs_lfs.c` | Host C | Formatter compiling against vendored LittleFS to create LittleFS test disk images | Host build helper in `tools/mk/targets-images.mk` |
| `mkisofs_test.c` | Host C | Synthetic ISO9660 image generator creating complex multi-extent test trees | Host build helper in `tools/mk/targets-gates.mk` |
