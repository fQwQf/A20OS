.PHONY: check-manifests check-instances check-instance-matrix check-component-registry check-trim-registry check-flash-backend-registry regen-driver-fragment regen-trim-fragment
check-instances:
	@tools/a20 check

check-instance-matrix:
	@tools/a20 check --require-arch $(SUPPORTED_HOSTED_ARCHES)

check-component-registry:
	@tools/a20 check-registry

# components/drivers.mk is generated from components/drivers.toml.  Run this
# after editing the TOML; check-component-registry fails while it is stale, so
# the generated copy can never rot the way a second hand-maintained list could.
# Every smoke case must be reachable as a make target, and every target that
# calls tools/smoke.py must name a case that exists.  One make spawn.

regen-driver-fragment:
	@tools/a20 regen-drivers

# components/trim.mk is generated from components/trim.toml -- the policy half
# of kernel trimming (profile source lists, capability arch matrices).  Same
# two-layer stance as the driver registry: the TOML is self-validated, the
# generated fragment must not go stale, and every emitted variable must still
# have a makefile consumer so no entry can die silently.
check-trim-registry:
	@tools/a20 check-trim

regen-trim-fragment:
	@tools/a20 regen-trim

# Flash-programmer registry (components/flash-backends.toml).  Two layers, like
# the driver registry: the toml is self-validated (unique names, boards that
# exist, targets that exist as make rules), then every make target is
# cross-checked against the set a20 can actually dispatch to.  Neither side can
# drift without FAIL.
check-flash-backend-registry:
	@tools/a20 check-flash-backends

# Aggregate for the manifest-layer gates.  These are host-side, pure-Python and
# arch-independent, so they are cheap enough to always run: instances/,
# components/ and the Makefile must never drift apart.  Wired into CI as the
# `manifest-gates` job.
check-manifests: check-instances check-instance-matrix check-component-registry \
                 check-trim-registry check-flash-backend-registry
	@echo "check-manifests: PASS"

