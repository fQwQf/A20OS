# OpenCode local helpers

The isolated connectivity helpers keep their configuration and run artifacts separate from the user's normal OpenCode profile. They do not read credentials, expose tools to the connectivity probe, or commit/push changes. The run directory is ignored by Git and created with owner-only permissions. Normal interactive use runs the official main TUI with the existing user profile; the helpers below are historical diagnostics and are not the recommended route for routine model use.

## Step 5 Preview Free on OpenCode v2

The older OpenCode v1.19 catalog did not list `step-5-preview-free`; that observation did not establish that the model was discontinued. OpenCode v2.0.28 lists it as `opencode/step-5-preview-free`, and its current official model page marks input, output, and cache-read as Free, cache-write as `-`, and says this is a limited-time offer. The runner pins CLI v2.0.28 and checks the live official catalog and pricing page before each request. It refuses to send a request if the model disappears, the provider does not match, or any price cannot be confirmed as zero.

The checked-in configuration adds a model under the built-in `opencode` provider. It does not replace that provider with a generic OpenAI-compatible package. The v2 CLI parsed the explicit model and exposed it in `models --standalone`. A short probe through `opencode run` reached the provider, but returned HTTP 403 with `provider.auth` / `FreeTierError`: “OpenCode's free tier can only be used from within OpenCode.” No completion was produced. I then tested one additional request through the documented OpenCode v2 `mini` TUI path, using `--standalone`, an empty workspace, isolated HOME/XDG/SQLite paths, and the all-deny `free-probe` agent. It returned the same HTTP 403 and no assistant content. This confirms the rejection also occurs through that supported CLI/TUI entry point; it does not establish why, and does not show that signing in again would resolve it. The TUI process was closed with Ctrl-C after the error and exited 0, so process exit alone is not a success signal. The noninteractive runner parses structured error events and refuses to count an error as success even if its process exits zero.

The `opencode run` result and official source snapshots are in `runs/20261013T112137Z-step5-v2.0.28/`; the isolated mini TUI result and pricing snapshot are in `runs/20261013T1154Z-step5-v2-tui-01/` (both ignored by Git). Those results describe only the isolated `run` and `mini --standalone` invocations above. A later ordinary main-TUI check in the existing user profile succeeded for both free models; the earlier 403s do not describe that client path. Relevant official references:

- [OpenCode v2 providers](https://opencode.ai/v2/docs/providers/) documents built-in providers, adding model entries, and provider package selection.
- [OpenCode v2 CLI](https://opencode.ai/v2/docs/cli/) documents the mini interactive TUI; [the CLI commands reference](https://dev.opencode.ai/v2/docs/cli/commands/) documents `mini --model`, `--agent`, and `--prompt`.
- [OpenCode v2 model catalog and pricing](https://opencode.ai/v2/docs/console/models/) lists Step 5 Preview Free and its limited-time zero-price terms.
- [OpenCode changelog](https://opencode.ai/changelog) records when Step 5 Preview Free joined the free model lineup.

### Ordinary main TUI verification

On 2026-10-13, the installed official v2.0.28 executable was started in the repository with its normal interactive entry point (full path: `~/.opencode/versions/v2.0.28/node_modules/.bin/opencode2 .`; no `run`, `mini`, standalone mode, custom agent, or isolated HOME/XDG). The TUI's `/models` picker selected the built-in `opencode` provider entries, and the assistant returned exact short markers for both `space-bunny-free` (`SPACE_BUNNY_TUI_OK`) and `step-5-preview-free` (`STEP5_TUI_OK`). The sanitized session identifier is `ses_eca7ccddcffeYhVGkSbMTJxvud`; read-only export confirmed each final model ID and matching assistant response. The Step 5 response followed a separate valid model selection; an earlier prompt issued before that selection was correctly excluded from Step 5 evidence. No credentials or full session/environment dump are stored here. These are point-in-time successful completions; re-check the official limited-time pricing before any later request.

The ordinary TUI command can also be invoked as `opencode .` when the installed executable is on `PATH`. In the tested environment it was not, so the exact installed v2.0.28 path above was used. The model is chosen interactively with `/models`; the CLI's `--model` option is not supported by this v2 entry point.

The isolated v2 runner is retained only as a diagnostic for the earlier noninteractive `run` path, which returned HTTP 403. It expects the separately installed official binary at `~/.opencode/versions/v2.0.28/node_modules/.bin/opencode2` (or an exact-version binary selected with `OPENCODE_V2_BIN`) and uses only its project config and isolated XDG/config directories:

```sh
python3 tools/agents/run_opencode_v2.py
```

Do not use this as the ordinary model path, an automatic fallback, or a retry loop. The script stores its command, version, provider/model, session ID, stdout, stderr, live price sources, and exit status beneath `tools/agents/runs/`.

## Legacy OpenCode v1 helper

`run_opencode.py` remains a separate v1.19 helper for `opencode/space-bunny-free`. Its one recorded isolated request was also rejected by the service with HTTP 403. It does not imply the model is unavailable in the ordinary main TUI, and is retained only as a historical diagnostic. For current interactive use, launch the ordinary main TUI described below.

The legacy helper supports a short smoke request, but its recorded service request returned HTTP 403. This command is retained only to reproduce that isolated diagnostic; do not use it as the ordinary model path:

```sh
python3 tools/agents/run_opencode.py --mode smoke
```

Its read-only audit mode was also an isolated diagnostic and is not the ordinary model path:

```sh
python3 tools/agents/run_opencode.py --mode readonly --message 'Read docs/zh/platforms/loongarch32.md and docs/zh/platforms/ppc64le.md. Identify only claims that conflict with current active build configuration; cite path and line. Do not modify files.'
```

Model output is untrusted and must be independently checked. The repository helper never grants shell access or permission to write project files through OpenCode.

## Ordinary TUI task workflow

Launch the installed official OpenCode v2.0.28 main TUI from the repository root with `~/.opencode/versions/v2.0.28/node_modules/.bin/opencode2 .` (or `opencode .` when the executable is on `PATH`). For long-lived delegated tasks, use `python3 tools/agents/opencode_tui.py`: it launches the same ordinary main TUI with the existing HOME/XDG/profile, continuously drains its PTY into a 64 KiB in-memory ring, and prints an ANSI-parsed screen snapshot when Ctrl+] is sent. It does not persist terminal output or environment data. The wrapper tests are `python3 -m unittest tools.tests.test_opencode_tui`. Keep the user's existing HOME/XDG and configuration. Create one independent session per task with the ordinary TUI `/new` command; this was verified to create separate session IDs while earlier sessions continued running. `/sessions` returns to an existing session. Verify the new session ID before sending a prompt, and confirm previous sessions remain available. In each session use `/models` to select the intended free model and confirm the model and variant in a sanitized export. Do not substitute `run`, `mini`, standalone mode, generic API calls, a CLI-created `--session`, or a separate profile for a requested ordinary TUI task. See the official [v2 TUI reference](https://dev.opencode.ai/v2/docs/cli/tui/).

Before starting parallel tasks, register a unique task ID and record: parent, model, OpenCode session ID, PTY/process ID when available, state, exact source paths and their hashes, allowed output paths, acceptance criteria, build/QEMU lease, and cleanup owner. Each task must have distinct owned files or write only a patch to its assigned output directory. The model must not create subagents, edit shared repository files, commit, push, touch global config/credentials, or start builds/QEMU unless its parent explicitly grants that exact scope. Keep output directories on the designated SSD, not in the repository or build tree. A local coordination ledger belongs under `.git/` and is not committed.

For a task result, preserve only a filtered, read-only session export and the final answer/patch needed for review. Review every claim against source and tests; apply patches yourself after checking ownership and current hashes. A terminal process exit by itself does not establish that a model answered successfully.
