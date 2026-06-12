# Testing and Documentation Gates

`DOCS_AS_FACT_CONTRACT`: architecture documents in `docs/`, `kernel/abi/*/*.md`,
and top-level status files describe the current implementation. Future designs
belong in planning material, not in fact documents.

`TEST_FIRST_ARCHITECTURE_MATRIX`: each architecture-debt area has a repeatable
gate before its TODO entry can be checked off.

| Area | Gate |
| --- | --- |
| concurrency foundation | `make check-concurrency-foundation` |
| MM/VMA/page tables | `make check-mm-lock-model` |
| I/O progress | `make check-io-progress-model` |
| VFS abstraction | `make check-vfs-abstraction` |
| ABI boundary | `make check-abi-boundary` |
| driver core | `make check-driver-core-model` |
| external dependencies | `make check-external-dependency-boundary` |

`BUILD_MATRIX_GATE_CONTRACT`: bringup builds cover `riscv64`, `loongarch64`,
`aarch64`, and `x86_64`; user builds cover the same architecture set through
`make check-user-build`.

`ABI_SMOKE_GATE_CONTRACT`: Linux ABI smoke runs `syscall_smoke` and userland
commands through `smoke-abi-linux`; Native ABI coverage includes
`native-minimal`, `native-test`, and `user/tests/test_liba20c.c` build coverage.

`DOC_DRIFT_KEYWORD_GATE`: drift keywords such as `stub`, `partial`, `TODO`,
`Future`, `not yet`, and `for simplicity` are allowed only when they are tied to
an explicit coverage table, TODO entry, or gate contract. Imported third-party
trees under `kernel/external/` and `user/external/` are excluded from this gate.
