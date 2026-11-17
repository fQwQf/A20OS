# Contributing

See the [release plan](releases/v1.0.md) for 1.0 scope, subsystem completion criteria and Rust migration order, and the [acceptance specification](releases/v1.0-validation.md) for functional comparisons and performance requirements. New implementations must correspond to V1 work packages, retain existing behavioral cases and add failure paths. Do not complete migration by reducing functionality, disabling SMP or relaxing performance thresholds. LoongArch32/PPC64LE have left 1.0; new porting commitments for that release are no longer accepted.

The patch workflow is intended to stay straightforward.

## 1. Fork and branch

```bash
git clone --recurse-submodules <你的 fork>
cd A20OS
git checkout -b fix/<简短描述>
# 或 feature/<子系统>-<描述>
```

Do not commit directly to `main`.

## 2. Build locally

Follow the [first-run guide](dev/build.md#首次运行linux--risc-v-64) to prepare single-architecture dependencies and confirm that you can build and run. For an existing ordinary clone, first run `git submodule update --init --recursive`.

```bash
python3 tools/check_quickstart.py
tools/a20 run qemu-riscv64 -- -j4
# 自动验证 shell、外部命令与正常关机：
make -j4 smoke-quickstart
```

## 3. Pass validation checks

For documentation-only changes, run `make check-docs`, or run `python3 tools/docs/format_markdown.py --check`, `python3 tools/docs/check_doc_citations.py` and `python3 tools/docs/check_doc_nav.py` individually. They check formatting, source line citations, full navigation coverage and links/anchors. Wording changes do not require running the entire OS. For code changes, run the checks and subsystem cases relevant to the affected behavior; release candidates require the full applicable matrix. See [docs/testing-gates.md](testing-gates.md):

```bash
# 构建矩阵
make check-build-matrix

# 基础 bring-up smoke；必须正常关机，timeout 失败，不验证 syscall
make smoke-riscv64

# 广泛聚合门禁；包含构建和 QEMU runtime，可能耗时较长
make check-doc-test-gates
```

Run `make smoke-abi-linux` to validate Linux syscalls. `smoke-riscv64` is only a `BRINGUP=1` boot check. `check-doc-test-gates` is also not a fast documentation-only check; allow time for builds and QEMU smoke tests in its transitive dependencies.

For changes to a specific subsystem, also run its checks:

```bash
make check-mm-lock-model          # 内存管理
make check-vfs-abstraction        # 文件系统
make check-abi-boundary           # ABI 边界
make check-native-abi-coverage    # 新增 A20_NATIVE_SYSCALL 时（登记表/编号表/文档要对齐）
make check-abi-config-guard       # 改动 CONFIG_ABI_* 守卫写法时
make check-driver-core-model      # 驱动
make check-io-progress-model      # I/O 进展
make check-concurrency-foundation # 并发
```

### 3.1 Source line citations in documentation

In `docs/` or another `.md` file, a citation such as line 123 of `file.c` may stop pointing to the intended code after that file changes. Run `tools/docs/check_doc_citations.py` to check that line numbers in references resolving to this repository are within the target file:

```bash
python3 tools/docs/format_markdown.py --check         # 格式与排版换行检查
python3 tools/docs/check_doc_citations.py --verbose   # 引用检查，并列出被跳过的条目
```

This check **deliberately** covers only what can be determined precisely: whether the range is valid. It does not decide whether line 123 still means the same thing; that requires a reader, not a regex. Pretending otherwise would produce noisy checks that people disable with `--no-verify`. Three categories are skipped and counted without failing: references to upstream sources outside this repository (mesa, virgl, wlroots and others); **quoted program output** such as `[foo.c:122] 某条日志` (evidence rather than a source citation); and ambiguous basenames. A bare `trap.c` cannot distinguish `kernel/core/trap.c` from the eight architecture-specific `trap.c` files, and guessing would produce false alarms.

Near the start of `tools/docs/check_doc_citations.py`, `KNOWN_MOVING` lists documents temporarily excluded from verification, each with a reason. The documented example is `feat/mm-complete` rewriting `kernel/mm/`, where line numbers will change again. Remove entries when their branches merge and the line numbers stabilize.

## 4. Code and comment conventions

### 4.1 Language

- Chinese documentation in `docs/zh/` is the source version. English translations use the same relative paths under `docs/en/`; the repository introduction keeps `README.md` and `README.en.md`. Edit Chinese source documents during normal development; incremental translation workflows open English-update PRs. Do not mark unreviewed translations as synchronized. See [documentation internationalization](dev/documentation-i18n.md) for configuration, review and local commands.
- Code comments use English. Existing kernel comments are already about 98% English; follow the existing language instead of introducing new mixtures.
- Commit subject lines use the English imperative, as described in section 5, consistent with the language of code comments.

### 4.2 Formatting

`.clang-format` records the C style: 4-space indentation without tabs, K&R braces, `type *name` pointers and an 80-column limit. It configures editors and formatters; there is no longer a repository-wide formatting gate. Format changed files in your editor using this configuration before submission.

`.editorconfig` adds line-ending and indentation conventions, and disables automatic rewriting for `*.s`/`*.S` and vendored directories. Assembly alignment and preprocessor instructions must not be reformatted.

### 4.3 What deserves a comment

Comment density is not a goal in itself. Ask whether a comment **explains something the code alone cannot express**. Useful comments include:

- Why this design was chosen: tradeoffs, rejected alternatives, hardware or ABI constraints.
- Invariants and preconditions: lock ordering, lock pre/postconditions, memory ordering, and what callers must or must not hold.
- Values hidden in code structure: derivations of magic numbers, capacity/size constants, register-bit meanings and wire-format byte offsets.
- Past failures: actual bug causes and clues for diagnosing unexpected register readback.
- Contract blocks: collect cross-file and cross-subsystem invariants in named `NAME_CONTRACT:` blocks, such as `COW_FAULT_TLB_CONTRACT`, `LWIP_NO_THREAD_PROGRESS_CONTRACT` and `SCHEDULER_CONCURRENCY_PREREQS`. Checks grep for these tokens; do not rename or delete them casually.

Comments that should be removed include:

- Restating identifiers or adjacent code, such as `// 分配一个物理帧` above `frame_alloc()`.
- Narrating code line by line.
- Section headings referring to moved functions; orphaned comments often remain after function extraction.
- Pure decoration: `====` separators, ASCII art and repeated per-file license banners.

### 4.4 File headers

Start each first-party `.c` / `.h` file with a short header describing its responsibility and why it is separate from neighboring files, rather than listing functions. About 79% of current kernel files lack such headers, making this a useful improvement area.

### 4.5 Lock-order comments

The global lock hierarchy is defined in `kernel/include/core/lock.h`; see [drivers/guide/lock-order.md](drivers/guide/lock-order.md) for the complete order. Prefix driver lock-order comments with `LOCK_ORDER:` and describe **only relative ordering or the protected invariant**:

```c
/* LOCK_ORDER: rx_lock is released before acquiring proc_lock in the Ctrl-C
 * path.  The dump/signal helpers acquire proc_lock on their own. */
```

Avoid merely repeating the following `spin_lock_irqsave()` call, as in `/* LOCK_ORDER: acquire rx_lock to push a character. */`. A cleanup reduced such comments from 53 to 18, retaining those that express real constraints. Do not copy the removed boilerplate into new code.

### 4.6 ABIs and registers

- Manually decoded structures in the ABI layer (`kernel/abi/`) require a **byte-offset table** and attribution to the referenced UAPI header. Reference the caller's user-space structure: for Linux compatibility, musl/glibc `shmid_ds`, `semid_ds` and `msqid_ds`, **not** the kernel's internal `shmid64_ds`.
- Manually decoded MMIO bitfields must explain what each bit selects and the observable consequence of an incorrect value. Bitfields often contain **encodings**, not literal values: STM32 `PLLMUL` means `bits + 2`, with 14 as a special case.

## 5. Commit message style

Use `type(scope): description` for subjects, with an English imperative describing the change. Start the description with a lowercase word; preserve `QEMU`, `VMA`, `RISC-V`, function names and paths. Omit a final period. Prefer at most 72 characters, with a maximum of 100. Use a lowercase subsystem for `scope`, such as `mm`, `net`, `hyp`, `build` or `history`.

| type | Purpose |
| --- | --- |
| `feat` | Add usable behavior or an interface |
| `fix` | Correct existing behavior, a bug or a regression |
| `refactor` | Change implementation structure while preserving external behavior |
| `perf` | Improve performance or resource usage |
| `docs` | Change documentation |
| `test` | Add or correct tests, validation checks or benchmark tools |
| `build` | Change builds, dependencies or packaging |
| `ci` | Change continuous integration |
| `chore` | Other repository maintenance |
| `revert` | Revert an existing change |
| `merge` | Describe features or fixes integrated by a multi-parent commit |

Choose the type by the primary change. Adding tests does not turn a fix into `test`; changing a Makefile does not turn a feature into `build`. Use subsystem names for scope, without mixing in types such as `fix` or `feat`.

Separate the body from the subject with a blank line. When a body is useful, explain motivation, behavior changes and significant limitations. Validation should list only commands actually run and their results; explain when tests were not run. Keep complete logs, diagnostic history and long-term design records in documentation, and summarize conclusions with paths in the commit message. Each commit should have an independently reviewable purpose. Avoid using only `update`, `fix bugs`, `WIP` or phase numbers as its description.

Use `Co-authored-by` only for people who actually co-authored the patch; do not automatically add model or tool names. Preserve existing human attribution and do not treat tool usage as co-authorship.

Example:

```text
fix(mm): flush stale COW translations on RISC-V

Invalidate the previous writable translation before exposing the
read-only COW mapping to another hart.

Validation:
- make smoke-mm-stress
- make host-tests
```

The example's validation commands illustrate formatting; replace them with checks actually run for the commit.

## 6. Pull Request

1. Push the branch to your fork.
2. Open a Pull Request on the repository page.
3. Describe:
   - What changed
   - Why it changed
   - Which checks ran
   - Any related issue

Maintainers review the code after CI passes. Address review comments and keep the checks passing.

## 7. Questions

Consult [docs/testing-gates.md](testing-gates.md) and [docs/zh/dev/build.md](dev/build.md) first. If the problem remains, open a GitHub Issue labeled `question`, including relevant commands and logs.

## Notes

- Do not directly change contract strings in `docs/zh/testing-gates.md` or `docs/zh/dev/external-dependencies.md` unless your change actually affects those contracts.
- Do not use `ALLOW_UNVERIFIED_SMP=1` as a default validation parameter. Only the corresponding QEMU virt boards for RISC-V64, AArch64, LoongArch64 and x86_64 are on the `Makefile` SMP allowlist. Other platforms may use this switch only in changes specifically addressing SMP bring-up.
- Before submitting, run `make clean` and rerun a key validation check to avoid misleading results from stale artifacts.
