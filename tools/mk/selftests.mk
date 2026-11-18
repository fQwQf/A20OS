# Bounded, host-only tests. No QEMU, Rust, downloads or cross compiler.
HOST_CC ?= cc
HOST_CFLAGS ?= -std=gnu99 -O2 -Wall -Wextra
SELFTEST_NAMES := drm_geom_test ntfs_format_test x86_syscall_args_test test_clx_frame test_clx_uart
SELFTEST_BINS := $(addprefix $(TMPDIR)/selftests/,$(SELFTEST_NAMES))

include tools/mk/manifests.mk

.PHONY: test host-tests check test-doc-tools
test host-tests: $(SELFTEST_BINS)
	@set -e; for binary in $(SELFTEST_BINS); do "$$binary"; done
	@$(PYTHON) -m unittest selftests.test_elf_load_span
	@echo 'selftests: PASS'

# Header dependencies matter: a changed production codec must rebuild its test.
$(TMPDIR)/selftests/%: selftests/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) -MMD -MP -Ikernel/include -Ikernel $< -o $@

-include $(SELFTEST_BINS:=.d)

check: check-manifests test

test-doc-tools:
	$(PYTHON) -m unittest selftests.test_doc_translator selftests.test_check_doc_citations selftests.test_mkdocs_repo_links selftests.test_docs_layout
