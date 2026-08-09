# ----------------------------------------------------------------
# Final-round submission build.  The evaluation platform invokes `make all`
# and then consumes kernel-rv, kernel-la, disk.img, and disk-la.img.  Keep the
# preliminary-round image builder available under an explicit target.
# ----------------------------------------------------------------
all:
	$(MAKE) final-submit-rv
	$(MAKE) final-submit-la
	@echo "=== 2026 final submission build complete ==="
	@echo "  kernel-rv  kernel-la  disk.img  disk-la.img"

final-all: all

preliminary-all:
	@set -e; for target in $(DEFAULT_CONTEST_TARGETS); do $(MAKE) $$target; done
	@echo "=== Preliminary-round competition build complete ==="
	@echo "  built: $(DEFAULT_CONTEST_TARGETS)"

all-architectures: all

check-kernel-build: $(DEFAULT_KERNEL_CHECK_TARGETS)

check-kernel-build-all: check-riscv64-bringup check-loongarch64-bringup check-aarch64-bringup check-x86_64-bringup check-arm32-bringup check-riscv32-bringup check-ppc64le-bringup

check-riscv64-bringup:
	$(MAKE) ARCH=riscv64 ABI=$(ABI) BRINGUP=1 kernel-only

check-loongarch64-bringup:
	$(MAKE) ARCH=loongarch64 ABI=$(ABI) BRINGUP=1 kernel-only

check-aarch64-bringup:
	$(MAKE) ARCH=aarch64 ABI=$(ABI) BRINGUP=1 kernel-only

check-x86_64-bringup:
	$(MAKE) ARCH=x86_64 ABI=$(ABI) BRINGUP=1 kernel-only

check-arm32-bringup:
	$(MAKE) ARCH=arm32 ABI=$(ABI) BRINGUP=1 kernel-only

check-riscv32-bringup:
	$(MAKE) ARCH=riscv32 ABI=$(ABI) BRINGUP=1 kernel-only

check-ppc64le-bringup:
	$(MAKE) ARCH=ppc64le ABI=$(ABI) BRINGUP=1 kernel-only

check-user-build: $(DEFAULT_USER_CHECK_TARGETS)

check-user-build-all: check-riscv64-user check-loongarch64-user check-aarch64-user check-x86_64-user check-arm32-user check-riscv32-user check-ppc64le-user

check-build-matrix: check-kernel-build check-user-build
	@rg -q "BUILD_MATRIX_GATE_CONTRACT" docs/testing/testing-gates.md
	@echo "check-build-matrix: PASS"

check-build-matrix-all: check-kernel-build-all check-user-build-all
	@rg -q "BUILD_MATRIX_GATE_CONTRACT" docs/testing/testing-gates.md
	@echo "check-build-matrix-all: PASS"

check-arch-boundary:
	@! rg -n '#if(n?def)?[[:space:]]+(CONFIG_|__)(AARCH64|ARM|RISCV|LOONG|X86|PPC)|CONFIG_ARM32|CONFIG_AARCH64|__aarch64__|__arm__' \
		kernel --glob '!kernel/arch/**' --glob '!kernel/platform/**' \
		--glob '!kernel/external/**' --glob '!kernel/include/core/arch.h' \
		--glob '!kernel/mm/vdso.c' --glob '!kernel/include/mm/vdso.h' \
		--glob '!kernel/include/mm/vdso_blob.h'
	@rg -q "ARCH_MMU_RUNTIME_MATRIX_CONTRACT" docs/testing/testing-gates.md
	@rg -q "smoke-arch-mmu-matrix" tools/targets-smoke.mk docs/OS-Design.md
	@for arch in loongarch64 x86_64 ppc64le; do \
		if $(MAKE) -s ARCH=$$arch NOMMU=1 kernel-only >/dev/null 2>&1; then \
			echo "check-arch-boundary: unsupported NOMMU build accepted for $$arch"; \
			exit 1; \
		fi; \
	done
	@echo "check-arch-boundary: PASS"

check-task-state-boundary:
	@! rg -n --pcre2 --glob '*.c' --glob '!kernel/external/**' \
		--glob '!kernel/proc/park.c' --glob '!kernel/proc/sched.c' \
		--glob '!kernel/proc/exit.c' --glob '!kernel/proc/task.c' \
		-- '->state[[:space:]]*=[[:space:]]*PROC_' kernel
	@! rg -n --pcre2 --glob '*.c' --glob '!kernel/external/**' \
		--glob '!kernel/proc/sched.c' --glob '!kernel/proc/current.c' \
		--glob '!kernel/proc/task.c' --glob '!kernel/proc/park.c' \
		-- '->(on_rq|dispatching|on_cpu|owner_cpu|rq_next|rq_prev)[[:space:]]*=' kernel
	@! rg -n 'proc_runq_(enqueue|remove)_locked[[:space:]]*\(' kernel \
		--glob '*.c' --glob '!kernel/proc/park.c' \
		--glob '!kernel/proc/current.c' \
		--glob '!kernel/proc/sched.c' --glob '!kernel/proc/exit.c' \
		--glob '!kernel/proc/task.c'
	@! rg -n --pcre2 'task_t[[:space:]]*\*[[:space:]]*(waiter|rx_waiter)\b' \
		kernel --glob '*.[ch]' --glob '!kernel/external/**'
	@! rg -n --pcre2 '\bproc_find[[:space:]]*\(' kernel \
		--glob '*.[ch]' --glob '!kernel/external/**'
	@rg -q 'A20_PARK_WAKE_PROTOCOL' kernel/include/proc/park.h
	@rg -q 'WAIT_QUEUE_PARK_PROTOCOL' kernel/include/core/sync.h
	@rg -q 'TASK_REFERENCE_LIFETIME' kernel/include/proc/proc.h
	@rg -q 'proc_get\(token\.task\)' kernel/core/sync.c
	@rg -q '_Static_assert\(offsetof\(task_t, kstack\) == 0' kernel/include/proc/proc.h
	@rg -q '_Static_assert\(offsetof\(task_t, kstack_base\) == sizeof\(uintptr_t\)' kernel/include/proc/proc.h
	@echo "check-task-state-boundary: PASS"

check-smp-platform-boundary:
	@rg -q "typedef struct smp_platform_ops" kernel/include/core/smp.h
	@rg -q "const smp_platform_ops_t \*smp" kernel/drivers/core/driver_core.h
	@files="kernel/arch/riscv64/platform/smp.c kernel/arch/aarch64/platform/smp.c kernel/arch/x86_64/platform/smp.c kernel/arch/loongarch64/platform/smp.c"; \
		test -r kernel/arch/riscv64/platform/smp.c && \
		test -r kernel/arch/aarch64/platform/smp.c && \
		test -r kernel/arch/x86_64/platform/smp.c && \
		test -r kernel/arch/loongarch64/platform/smp.c && \
		if rg -n "CONFIG_BOARD|firmware_cpu_on|sbi_hart_start|firmware_acpi_apic_ids|IOCSR_MBUF" $$files; then exit 1; fi
	@! rg -n "void smp_(init|boot_secondaries|send_reschedule|secondary_init)\\(" kernel/arch
	@for board in qemu-virt-riscv64 qemu-virt-aarch64 qemu-virt-loongarch64 qemu-virt-x86_64; do \
		rg -q "\\.smp[[:space:]]*=" "kernel/platform/$$board/board.c" || exit 1; \
	done
	@echo "check-smp-platform-boundary: PASS"

check-abi-smoke-gate:
	@rg -q "ABI_SMOKE_GATE_CONTRACT" docs/testing/testing-gates.md
	@rg -q "syscall_smoke" tools/targets-smoke.mk
	@rg -q "smoke-abi-linux" tools/targets-smoke.mk
	@rg -q "native-minimal" tools/targets-native.mk
	@rg -q "native-test" tools/targets-native.mk
	@rg -q "test_liba20c" user/tests/test_liba20c.c tools/targets-native.mk
	@echo "check-abi-smoke-gate: PASS"

check-doc-drift:
	@rg -q "DOC_DRIFT_KEYWORD_GATE" docs/testing/testing-gates.md
	@$(PYTHON) tools/gen_linux_syscall_coverage.py
	@rg -q "stub" kernel/abi/linux/syscall_coverage.md kernel/abi/linux/compat_notes.md docs/testing/testing-gates.md
	@rg -q "partial" kernel/abi/linux/syscall_coverage.md kernel/abi/linux/compat_notes.md docs/testing/testing-gates.md
	@rg -q "Future" docs/testing/testing-gates.md kernel/abi/native/sys_core.c
	@rg -q "not yet" docs/testing/testing-gates.md kernel/abi/native/sys_phase2.c kernel/mm/fault.c
	@! rg -q "for simplicity" docs kernel --glob '!docs/research/**' --glob '!docs/testing/testing-gates.md' --glob '!kernel/external/**'
	@echo "check-doc-drift: PASS"

check-task-lifetime-boundary:
	@rg -q "STEP35_TASK_LIFETIME_DIAGNOSTICS" kernel/include/proc/lifetime.h
	@rg -q "proc_lifetime_note_task_init" kernel/proc/task.c
	@rg -q "proc_lifetime_note_pid_add" kernel/proc/pid.c
	@rg -q "proc_lifetime_note_wait_to_wake" kernel/core/sync.c
	@rg -q "proc_lifetime_note_wake_remove" kernel/proc/park.c
	@rg -q "proc_wait_timer_count_locked" kernel/proc/timer_heap.c
	@rg -q "proc_current_lifetime_violations_locked" kernel/proc/current.c
	@rg -q "PF_A20_TASK_LIFETIME" kernel/fs/procfs/procfs.c
	@rg -q "TASK_LIFETIME_OWNERSHIP_AUDIT" docs/testing/task-lifetime-audit.md
	@! rg -n --pcre2 '\bproc_find[[:space:]]*\(' kernel \
		--glob '*.[ch]' --glob '!kernel/external/**'
	@echo "check-task-lifetime-boundary: PASS"

check-blocking-point-boundary:
	@rg -q "BLOCKING_POINT_PROTOCOL_AUDIT" docs/testing/blocking-point-audit.md
	@rg -q "PROC_BLOCKED_ALLOCATION_WHITELIST" kernel/proc/task.c
	@rg -q "FUTEX_WAIT_RECHECK_PROTOCOL" kernel/abi/linux/sys_futex.c
	@rg -q "proc_try_wake_locked\(t, t->wait_seq, PROC_WAKE_EVENT\)" kernel/proc/sched.c
	@! rg -n --pcre2 '\bproc_block_until[[:space:]]*\(' kernel \
		--glob '*.[ch]' --glob '!kernel/external/**'
	@bad=$$(rg -n --pcre2 '(?:->|\.)state[[:space:]]*=[[:space:]]*PROC_BLOCKED' \
		kernel --glob '*.[ch]' --glob '!kernel/external/**' | \
		rg -v '^kernel/proc/(task|park)\.c:' || true); \
		test -z "$$bad" || { echo "$$bad"; exit 1; }
	@bad=$$(rg -n --pcre2 '(?:->|\.)(?:on_rq|cpu_id|rq_next|rq_prev)[[:space:]]*=' \
		kernel --glob '*.[ch]' --glob '!kernel/external/**' | \
		rg -v '^kernel/proc/(task|sched|park)\.c:' || true); \
		test -z "$$bad" || { echo "$$bad"; exit 1; }
	@bad=$$(rg -n --pcre2 '\bproc_make_ready[[:space:]]*\(' kernel \
		--glob '*.c' --glob '!kernel/external/**' | \
		rg -v '^kernel/(proc/(fork|proc|sched|cg_cpu)\.c|abi/native/sys_core\.c):' || true); \
		test -z "$$bad" || { echo "$$bad"; exit 1; }
	@rg -Uq 'typedef struct wait_queue_entry[^{]*\{[^}]*task[^}]*wait_seq' kernel/include/core/sync.h
	@rg -q "wait_queue_entry_t entry" kernel/abi/linux/sys_futex.c
	@! rg -n "typedef struct futex_waiter" kernel/abi/linux/sys_futex.c
	@rg -Uq 'typedef struct wait_timer[^{]*\{[^}]*task[^}]*wait_seq' kernel/proc/timer_heap.c
	@rg -Uq 'typedef struct proc_wake_q_item[^{]*\{[^}]*task[^}]*seq' kernel/include/proc/park.h
	@rg -q "FUTEX_STRESS: unrelated-wake-isolation PASS" user/cmds/stress/futex_stress.c
	@rg -q "PROC_STRESS: vfork-auto-reap PASS" user/cmds/stress/proc_stress.c
	@echo "check-blocking-point-boundary: PASS"

check-signal-exit-boundary:
	@rg -q "SIGNAL_EXIT_PROTOCOL_AUDIT" docs/testing/signal-exit-audit.md
	@rg -q "SIGNAL_STATE_LOCK_CONTRACT" kernel/include/proc/signal.h
	@rg -q "PARK_SIGNAL_MODE_PROTOCOL" kernel/proc/park.c
	@rg -q "SIGNAL_MASK_PARK_PROTOCOL" kernel/abi/linux/sys_signal.c
	@rg -q "REMOTE_EXIT_SAFE_BOUNDARY" kernel/proc/exit.c
	@rg -q "PROC_WAKE_FATAL_SIGNAL" kernel/include/proc/park.h kernel/proc/park.c
	@rg -q "PROC_WAKE_TASK_EXIT" kernel/include/proc/park.h kernel/proc/exit.c
	@! rg -n --pcre2 '\bproc_make_ready[[:space:]]*\(' \
		kernel/proc/signal.c kernel/proc/exit.c
	@! rg -n --pcre2 'reason[[:space:]]*==[[:space:]]*PROC_WAKE_SIGNAL' \
		kernel --glob '*.c' --glob '!kernel/external/**' \
		--glob '!kernel/proc/park.c'
	@bad=$$(rg -n --pcre2 \
		'(?:->|\.)(?:sig_blocked|thread_pending|sigsuspend_old_blocked|sigsuspend_active|sigwait_mask|sigwait_active)\b|(?:ss|signal_state)->(?:pending|pending_has_info|pending_info|actions)\b' \
		kernel --glob '*.c' --glob '!kernel/external/**' | \
		rg -v '^kernel/(proc/(signal|task)\.c|abi/linux/sys_signal\.c|abi/native/handle_table\.c|ipc/signalfd\.c):' || true); \
		test -z "$$bad" || { echo "$$bad"; exit 1; }
	@rg -Uq 'eventfd_read[\s\S]*proc_park_prepare\(PROC_WAIT_INTERRUPTIBLE' \
		kernel/ipc/eventfd.c
	@rg -Uq 'timerfd_read[\s\S]*proc_park_prepare\(PROC_WAIT_INTERRUPTIBLE' \
		kernel/ipc/timerfd.c
	@rg -q "PROC_STRESS: signal-stop-exit PASS" user/cmds/stress/proc_stress.c
	@rg -q "PROC_STRESS: signal-mask-park PASS" user/cmds/stress/proc_stress.c
	@echo "check-signal-exit-boundary: PASS"

check-timeout-ownership-boundary:
	@rg -q "TIMEOUT_OWNERSHIP_AUDIT" docs/testing/timeout-ownership-audit.md
	@rg -Uq 'typedef struct wait_timer[^{]*\{[^}]*deadline[^}]*task[^}]*wait_seq' \
		kernel/proc/timer_heap.c
	@rg -q "PROC_PARK_PREPARE_TIMEOUT_CAPACITY" \
		kernel/include/proc/park.h kernel/proc/park.c kernel/proc/timer_heap.c
	@rg -q "wait_timer_duplicate_rejections" kernel/proc/timer_heap.c
	@rg -Uq 'proc_try_wake_locked\(timer\.task,[[:space:]]*timer\.wait_seq' \
		kernel/proc/timer_heap.c
	@rg -q "timeout_heap_violations" \
		kernel/include/proc/lifetime.h kernel/proc/lifetime.c
	@rg -Fq "timeout-capacity+1 PASS" user/cmds/stress/lifetime_stress.c
	@rg -q "FUTEX_STRESS: stale-timeout-isolation PASS" \
		user/cmds/stress/futex_stress.c
	@! rg -U --pcre2 \
		'wait_timer_count[\s\S]{0,500}PROC_READY' kernel/proc/sched.c
	@echo "check-timeout-ownership-boundary: PASS"

check-smp-runqueue-boundary:
	@rg -q "SMP_RUNQUEUE_PREEMPT_AUDIT" \
		docs/testing/smp-runqueue-audit.md
	@rg -q "SMP_RUNQUEUE_PREEMPT_PROTOCOL" kernel/include/proc/proc.h
	@rg -q "SMP_RUNQUEUE_MIGRATION_PROTOCOL" kernel/proc/sched.c
	@rg -q "need_resched" kernel/proc/sched.c
	@rg -Uq 'first = src_cpu < dst_cpu[\s\S]*RUNQ_LOCK_IRQ\(first\)[\s\S]*RUNQ_LOCK_IRQ\(second\)' \
		kernel/proc/sched.c
	@rg -q "proc_sched_safe_point" kernel/core/trap.c
	@rg -q "proc_sched_tick" \
		kernel/arch/riscv64/trap/irqchip.c \
		kernel/arch/loongarch64/trap/irqchip.c
	@rg -q "proc_sched_handle_reschedule_ipi" \
		kernel/arch/riscv64/trap/irqchip.c \
		kernel/arch/loongarch64/platform/smp.c
	@! rg -n 'proc_yield' \
		kernel/arch/riscv64/trap/irqchip.c \
		kernel/arch/loongarch64/platform/smp.c \
		kernel/arch/aarch64/trap/irqchip.c \
		kernel/arch/x86_64/trap/irqchip.c
	@rg -q "scheduler_violations" \
		kernel/include/proc/lifetime.h kernel/proc/lifetime.c
	@rg -q "SCHED_STRESS: smp-runqueue PASS" user/cmds/stress/sched_stress.c
	@echo "check-smp-runqueue-boundary: PASS"

check-process-lock-split-boundary:
	@rg -q "PROCESS_LOCK_SPLIT_AUDIT" \
		docs/testing/process-lock-split-audit.md
	@rg -q "SCHED_LOCAL_PICK_LOCK_SPLIT_BEGIN" kernel/proc/sched.c
	@rg -Uq 'task_t \*next = proc_runq_pick_local\(\);[[:space:]]*uint64_t flags = spin_lock_irqsave\(&proc_lock\)' \
		kernel/proc/sched.c
	@! rg -U --pcre2 \
		'SCHED_LOCAL_PICK_LOCK_SPLIT_BEGIN(?:(?!SCHED_LOCAL_PICK_LOCK_SPLIT_END)[\s\S])*spin_lock_irqsave\(&proc_lock\)' \
		kernel/proc/sched.c
	@! rg -n 'proc_runq_pick_locked' kernel/proc kernel/include/proc
	@rg -q "runqueue_parallel_pick_peak" \
		kernel/include/proc/lifetime.h kernel/proc/lifetime.c
	@rg -q "SCHED_STRESS: lock-split PASS" user/cmds/stress/sched_stress.c
	@echo "check-process-lock-split-boundary: PASS"

check-doc-test-gates: check-concurrency-foundation check-smp-platform-boundary check-task-state-boundary check-task-lifetime-boundary check-blocking-point-boundary check-signal-exit-boundary check-timeout-ownership-boundary check-smp-runqueue-boundary check-process-lock-split-boundary check-mm-lock-model check-io-progress-model check-vfs-abstraction check-abi-boundary check-driver-core-model check-external-dependency-boundary check-abi-smoke-gate check-doc-drift
	@rg -q "DOCS_AS_FACT_CONTRACT" docs/testing/testing-gates.md
	@rg -q "TEST_FIRST_ARCHITECTURE_MATRIX" docs/testing/testing-gates.md
	@echo "check-doc-test-gates: PASS"

check-final-definition: check-doc-test-gates
	@rg -q "MM_LOCK_MODEL" kernel/include/mm/vm.h
	@rg -q "TASK_STATE_MUTATION_CONTRACT" kernel/include/proc/proc.h
	@rg -q "TASK_REFERENCE_LIFETIME" kernel/include/proc/proc.h
	@rg -q "VFS_REFCOUNT_HELPER_CONTRACT" kernel/include/fs/vfs.h
	@rg -q "NATIVE_HANDLE_CAPABILITY_CONSISTENCY_MATRIX" kernel/include/ipc/handle_table.h
	@rg -q "KERNEL_PROGRESS_SERVICE_CONTRACT" kernel/include/core/progress.h
	@rg -q "VFS_OPEN_DISPATCH_CONTRACT" kernel/include/fs/vfs.h
	@rg -q "LINUX_ABI_EXPLICIT_STUB_CONTRACT" kernel/abi/linux/syscall_table.def
	@rg -q "NATIVE_DEBUG_LIMITED_CONTRACT" kernel/abi/native/sys_phase2.c
	@rg -q "DRIVER_CORE_CONCURRENCY_MODEL" kernel/drivers/core/driver_core.c
	@rg -q "EXTERNAL_USERLAND_UPGRADE_CHECKLIST" docs/project/external-dependencies.md
	@echo "check-final-definition: PASS (SMP smoke tracked separately by TODO section 10)"

check-riscv64-user:
	$(MAKE) -C user ARCH=riscv64 OPT="$(USER_OPT)"

check-loongarch64-user:
	$(MAKE) -C user ARCH=loongarch64 OPT="$(USER_OPT)"

check-aarch64-user:
	$(MAKE) -C user ARCH=aarch64 OPT="$(USER_OPT)"

check-x86_64-user:
	$(MAKE) -C user ARCH=x86_64 OPT="$(USER_OPT)"

check-arm32-user:
	$(MAKE) -C user ARCH=arm32 OPT="$(USER_OPT)"

check-riscv32-user:
	$(MAKE) -C user ARCH=riscv32 OPT="$(USER_OPT)"

check-ppc64le-user:
	$(MAKE) -C user ARCH=ppc64le OPT="$(USER_OPT)"

check-dev-build:
	$(MAKE) ARCH=riscv64 ABI=$(ABI) BRINGUP=0 dev-build

check-contest-build:
	$(MAKE) all

check-contest-build-all:
	$(MAKE) all-architectures

check-concurrency-foundation:
	@rg -q "SCHEDULER_CONCURRENCY_PREREQS" kernel/proc/sched.c
	@rg -q "SCHEDULER_CPU_OWNERSHIP" kernel/proc/sched.c
	@rg -q "PER_CPU_CURRENT_VALIDATION" kernel/proc/current.c
	@rg -q "TASK_STATE_MUTATION_CONTRACT" kernel/include/proc/proc.h
	@rg -q "A20_PARK_WAKE_PROTOCOL" kernel/include/proc/park.h
	@rg -q "WAIT_QUEUE_PARK_PROTOCOL" kernel/include/core/sync.h
	@$(MAKE) ARCH=$(ARCH) NR_CPUS=2 ALLOW_UNVERIFIED_SMP=1 BRINGUP=1 kernel-only >/dev/null
	@echo "check-concurrency-foundation: PASS"

check-mm-lock-model: smoke-mm-stress smoke-mm-fork-exec-race
	@rg -q "MM_LOCK_MODEL" kernel/include/mm/vm.h
	@rg -q "MM_VMA_PTE_AUDIT" kernel/mm/vm.c
	@rg -q "COW_FAULT_TLB_CONTRACT" kernel/mm/fault.c
	@rg -q "DEMAND_FAULT_TLB_CONTRACT" kernel/mm/fault.c
	@rg -q "MM_FORK_COW_REGRESSION_GUARD" kernel/mm/vm.c
	@rg -q "MM_FORK_DEFERRED_STATE_REGRESSION_GUARD" kernel/mm/vm.c
	@rg -q "child->deferred_vma = NULL;" kernel/mm/vm.c
	@rg -q "MM_VMA_FORK_EXEC: PASS" user/cmds/stress/mm_stress.c
	@rg -q "FILE_MMAP_PAGE_CACHE_CONTRACT" kernel/include/fs/page_cache.h
	@rg -q "OOM_RECLAIM_LIFETIME_CONTRACT" kernel/include/mm/oom.h
	@rg -q "MM_STRESS: PASS" user/cmds/stress/mm_stress.c
	@echo "check-mm-lock-model: PASS"

check-io-progress-model:
	@rg -q "KERNEL_PROGRESS_SERVICE_CONTRACT" kernel/include/core/progress.h
	@rg -q "IO_PROGRESS_SERVICE" kernel/core/progress.c
	@rg -q "kernel_progress_run_bottom_halves\(\)" kernel/proc/sched.c kernel/proc/proc.c
	@rg -q "kernel_progress_timer_tick\(\)" kernel/arch/riscv64/trap/irqchip.c kernel/arch/loongarch64/trap/irqchip.c kernel/arch/aarch64/trap/irqchip.c kernel/arch/x86_64/trap/irqchip.c
	@rg -q "VIRTIO_BLK_COMPLETION_MODEL" kernel/drivers/block/virtio_blk.c
	@rg -q "LWIP_NO_THREAD_PROGRESS_CONTRACT" kernel/net/lwip_stack.c
	@rg -q "g_lwip_lock -> virtio-net nonblocking" kernel/include/core/lock.h
	@! rg -q "virtio_blk_poll_all" kernel/proc/sched.c kernel/proc/proc.c
	@! rg -q "a20_lwip_poll" kernel/proc/sched.c kernel/proc/proc.c
	@echo "check-io-progress-model: PASS"
