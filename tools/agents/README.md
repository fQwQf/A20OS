# OpenCode free-model helper

This repository-scoped helper runs OpenCode without changing the user's global configuration. It pins `opencode/space-bunny-free`, checks the installed live catalog immediately before each request, and refuses to send a request unless the provider reports the model active with zero input, output, and cache prices. There is no paid fallback. On 2026-10-13, `opencode/step-5-preview-free` was absent from the active catalog. The one connectivity request to the active free model was rejected by the service with HTTP 403 / `FreeTierError` (“OpenCode's free tier can only be used from within OpenCode”); the helper records that failure and will not retry or bypass the service restriction. A zero catalog price does not guarantee that a particular client invocation is eligible for the free tier.

The CLI is installed at `~/.opencode/bin/opencode` (version 1.19.0) from the official OpenCode installer. The helper uses only the account already authenticated in OpenCode; it never reads or prints credential files. It sets a temporary per-run `OPENCODE_CONFIG_DIR` and `XDG_CONFIG_HOME`, disables default plugins, and points `OPENCODE_CONFIG` at one of the checked-in project configs. Per-run metadata, request, response events, stderr, and exported session are stored under `tools/agents/runs/` with owner-only directory permissions. That directory is ignored by Git.

Run a short connectivity check:

```sh
python3 tools/agents/run_opencode.py --mode smoke
```

For a read-only audit, provide a narrow task that names the exact repository files to inspect:

```sh
python3 tools/agents/run_opencode.py --mode readonly --message 'Read docs/platforms/loongarch32.md and docs/platforms/ppc64le.md. Identify only claims that conflict with current active build configuration; cite path and line. Do not modify files.'
```

The smoke agent has all tools disabled. The read-only agent exposes only repository reads from allowlisted source/documentation paths and explicitly denies all other tools and external directories. Model/provider/cost are pinned in both config and the runner; model output is untrusted and must be independently checked. The helper does not commit, push, or modify repository files through OpenCode. The recorded smoke result is under `runs/20261013T105316Z-63ba399d/`; it contains the command, live catalog metadata, server error, session export, and exit status 1. No completion text was returned.
