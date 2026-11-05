# A20OS Unified CLI Implementation (`tools/cli/`)

This directory contains the Python modules backing the unified `tools/a20` declarative instance runner.

The front-facing command-line entrypoint remains at `tools/a20`.

## Modules Overview

| Module | Role | Key Functions / Types |
|--------|------|------------------------|
| `a20_instance.py` | Instance manifest parser and model | `parse_instance`, `Instance`, `applicable_actions` |
| `a20_derive.py` | Derives Makefile variables from instance models | `derive_make_vars` |
| `a20_make.py` | Makefile execution engine and query interface | `exec_make`, `build_instance`, `query_make` |
| `a20_registry.py` | Driver and trim registry validators | `load_registry`, `validate_registry`, `plan_trim` |
| `a20_resource.py` | Host resource preflight and QEMU gating | `gate`, `gate_qemu`, `preflight`, `Policy` |
| `a20_validate.py` | Semantic validator for instances | `validate_instance` |
| `a20_runtime.py` | Background instance lifecycle and process management | `start`, `control`, `list_runs`, `attach` |
| `a20_test.py` | Automated guest smoke testing runner | `run_test`, `_qemu_cmdline` |
| `a20_console.py` | Interactive serial, PTY, and QMP console sessions | `run_console_session`, `open_transport` |
| `a20_board.py` | Hardware flashing and packaging dispatch | `run_flash`, `run_package` |
| `a20_boards.py` | Architecture-owned board discovery | `board_directories`, `board_directory` |
| `a20_complete.py` | Shell auto-completion candidate generation | `candidates`, `BASH_COMPLETION` |
| `a20_manifest.py` | Machine ledger and provenance generator | `collect`, `render_json`, `render_markdown` |
| `a20_net_vars.py` | Standalone helper emitting network make variables | `derive_make_vars` |
| `a20_tftp.py` | Read-only TFTP server for kernel handoffs | `serve_tftp`, `verified_kernel` |
| `a20_vm.py` | RPC client for the `a20-vmd` supervisor daemon | `dispatch`, `_rpc` |
| `a20_handoff.py` | Automated U-Boot protocol handoff verification | `run_image_handoff`, `load_plan` |
| `a20_error.py` | Structured error hierarchy and CLI reporting | `A20Error`, `ToolError`, `report` |
