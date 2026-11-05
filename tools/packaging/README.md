# Package & Rootfs Composition Utilities (`tools/packaging/`)

This directory contains the tools for packaging software recipes into apk v2 format, signing repository metadata, and composing ext4 root filesystems from world definitions.

## Subsystems Covered

1. **Recipe Packaging (`mka20pkg.py`)**: Gathers built binary artifacts defined in TOML recipes (`packages/recipes/*.toml`) into standard `.apk` archives with optional RSA-SHA256 signatures.
2. **Repository Management (`mka20repo.sh`, `apk-sign-stream.py`, `ensure-apk-static.sh`)**: Indexes directories of `.apk` archives, downloads static `apk` binary tools, and prefixes RSA signatures to `APKINDEX.tar.gz`.
3. **Rootfs Generation (`mkrootfs.py`, `gen_rootfs_overlay.py`)**: Resolves world manifests (`packages/world/*.world`) against A20OS and upstream Alpine packages, generates in-kernel rootfs C overlays, and outputs formatted ext4 images.
4. **Toolchain Identity (`musl_build_identity.py`)**: Computes cache invalidation hashes for native musl toolchain components.

## Files

| File | Type | Description |
|------|------|-------------|
| `mka20pkg.py` | Python | Compiles TOML recipes into signed `.apk` packages |
| `mka20repo.sh` | Shell | Indexes `.apk` collections into local apk repositories |
| `apk-sign-stream.py` | Python | Stream-level RSA256 signature generator for apk indexes |
| `ensure-apk-static.sh` | Shell | Fetches and caches standalone static apk binaries |
| `mkrootfs.py` | Python | Composes ext4 rootfs disk images from `.world` manifests |
| `gen_rootfs_overlay.py` | Python | Emits embedded C byte tables from `user/rootfs_overlay/` |
| `musl_build_identity.py` | Python | Computes deterministic build identity hashes for musl |
| `a20-fortify.c` | C | Glibc fortify compatibility shim for JNA / Minecraft |
| `a20-fortify.map` | Version Script | Symbol version map for `liba20-fortify.so` |
