# The pinned suite is optional. Merely initializing it must not alter normal
# build/test dependencies or parse thousands of integration make rules.
A20_TESTS_DIR := tests
A20_TEST_GOALS := $(filter smoke-% bench-% check-% test-% step% full-host-tests tests-% _step%,$(MAKECMDGOALS))
A20_CORE_CHECKS := check check-manifests check-instances check-instance-matrix check-component-registry check-trim-registry check-flash-backend-registry check-docs test-doc-tools check-kernel-build% check-user-build% check-build-matrix% check-dev-build check-release-build check-%-bringup check-%-user check-%-build check-lao64-board-builds check-arm-board-builds check-rootfs-overlay-generator check-x86-drvmod-isa
A20_TEST_GOALS := $(filter-out $(A20_CORE_CHECKS),$(A20_TEST_GOALS))

ifneq ($(strip $(A20_TEST_GOALS)),)
ifneq ($(wildcard $(A20_TESTS_DIR)/suite.mk),)
include $(A20_TESTS_DIR)/suite.mk
else
# Fail explicitly for missing suite targets, without intercepting build checks.
.PHONY: tests-help tests-nightly tests-release full-host-tests check-a20-tests check-smoke-cases check-integration-host
tests-help tests-nightly tests-release full-host-tests check-a20-tests check-smoke-cases check-integration-host:
	@echo "Optional integration suite missing. Run: git submodule update --init tests" >&2
	@exit 2
smoke-% bench-% step%:
	@echo "Optional integration suite missing. Run: git submodule update --init tests" >&2
	@exit 2
endif
endif
