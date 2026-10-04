# ================================================================
# Lock-contention benchmark (roadmap docs/roadmap/lock-serialization-split.md)
# ================================================================
# Owned by the baseline agent per that roadmap's §4 file-ownership table:
# tools/lock_bench.py, this file, the single include line in the Makefile,
# and docs/measured/.  It touches no kernel/ file.
#
# The benchmark boots one workload per guest on a real multi-vCPU QEMU, with
# /proc/a20/lock_contention and /proc/a20/perf reset immediately before the
# workload and read immediately after, so the measured window belongs to the
# workload alone.  See docs/roadmap/lock-serialization-split.md §5 for the
# protocol and the noise discipline; the raw data lands in docs/measured/.
#
# Everything is an environment variable so the post-modification re-run is a
# zero-edit re-invocation:
#
#   make bench-locks                                  # baseline, 5 runs x 4 loads
#   make bench-locks RUNS=9 SMP=4                     # more runs, smaller guest
#   make bench-locks WORKLOADS=mm_stress,net_stress   # subset
#   make bench-locks PHASE=after SKIP_BUILD=1 \
#        KERNEL_ELF=... FAT32_IMG=...                 # reuse a built image
#
# BUILD_CMD defaults to this file's own kernel build.  Override it to point
# the benchmark at an image built some other way.
LOCK_BENCH ?= $(PYTHON) tools/lock_bench.py
LOCK_BENCH_RUNS ?= 5
LOCK_BENCH_SMP ?= 8
LOCK_BENCH_TIMEOUT ?= 600

bench-locks:
	RUNS=$(LOCK_BENCH_RUNS) SMP=$(LOCK_BENCH_SMP) \
	TIMEOUT=$(LOCK_BENCH_TIMEOUT) \
	$(LOCK_BENCH) --runs $(LOCK_BENCH_RUNS) --smp $(LOCK_BENCH_SMP) \
		--timeout $(LOCK_BENCH_TIMEOUT) \
		$(if $(PHASE),--phase $(PHASE),--phase baseline) \
		$(if $(WORKLOADS),--workloads $(WORKLOADS),) \
		$(if $(OUT_DIR),--out-dir $(OUT_DIR),)