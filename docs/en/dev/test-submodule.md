# Optional test submodule

The OS repository owns implementation, build rules, images and operational tools. Integration tests live in [a20-tests](https://github.com/fQwQf/a20-tests), pinned as the `tests/` submodule.

## Build and fast tests

```sh
make ARCH=riscv64          # 等同 kernel-only，不运行测试
make test                 # 5 个宿主 C 单测和 ELF 解析测试
make host-tests           # make test 的兼容入口
make check                # 清单检查和轻量测试
make check-docs           # 文档一致性
make release-rv           # 显式的 RISC-V 发布构建
make release-la           # 显式的 LoongArch 发布构建
```

These commands do not need the test submodule. Fast tests use a host C compiler and Python's standard library, without QEMU, Rust or cross compilers. OS and image builds still need the relevant build dependencies.

## Import integration tests

```sh
git submodule update --init tests
make tests-help
make check-smoke-cases
make smoke-quickstart
make -C tests smoke-quickstart
```

No script copies or compatibility symlinks are needed. Test makefiles load only for explicit integration goals; initializing the suite does not change the dependencies of `make test` or `make check`. Install the QEMU, cross compiler, Rust targets and image tools required by the selected case.

The original broad host aggregate is `make full-host-tests`, and the broad host gate aggregate is `make check-integration-host`. After initialization, `make check-a20-tests` and `make smoke-*` remain available.

## CI tiers

PR checks build the kernel architecture matrix and run fast OS checks. Scheduled or manual integration CI imports the pinned suite and runs `make tests-nightly`. Release acceptance uses `make tests-release`; select the actual required tests with `RELEASE_TEST_TARGETS`. The default covers representative boot and ABI tests, not every stress, cluster, GPU or hardware acceptance case.

This migration preserves existing case semantics. Legacy cases may still inspect source, build special images, match serial output and require kernel test knobs. An image-only artifact interface and further removal of test hooks remain future work; moving files alone does not implement them.

## History and tags

```sh
python3 tools/repo/prepare-test-history.py \
  --output /absolute/path/outside-the-checkout
```

This requires `git-filter-repo` and a new output directory outside the checkout. It backs up referenced history, removes migrated and recognized historical paths in new mirrors, and prepares separate OS and test histories. It does not change the source checkout, index or refs and does not push. Mirrors initially contain committed history only; import and review the current migration changes before publishing.

All OS branches and tags are rewritten together. Empty commits are retained so that tagged test-only commits are not collapsed. Validation checks tag names, lightweight/annotated types, commit mappings and unchanged tree entries outside removed paths, then scans all historical paths and runs `git fsck`. Signed tags must be signed again after rewriting; the report lists them.

Publication must coordinate all remote branches and tags so old refs do not keep removed objects reachable. Do not publish local backup branches or internal tool refs through `push --mirror`. GitHub PR refs, caches and existing clones may still retain old objects; a force push does not guarantee physical deletion there. Existing reflogs, backup refs and submodule object stores do not shrink merely because new history exists. Keep historical backups outside the OS repository.
