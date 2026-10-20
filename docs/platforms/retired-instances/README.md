# Retired PPC64LE instance snapshots

These TOML manifests were active under `instances/` at the `v0.18` baseline. They are kept as historical configuration evidence, but the current `tools/a20` registry does not scan or execute this directory. `make ARCH=ppc64le ...` is explicitly rejected.

For reproduction, use a clean checkout of the annotated `v0.18` tag, where these files are at their original paths under `instances/`. The current checkout may contain local edits; preserve them before switching revisions.
