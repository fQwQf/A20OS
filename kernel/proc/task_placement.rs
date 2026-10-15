#![no_std]

//! Deterministic task-to-CPU placement policy.
//!
//! C snapshots affinity, cgroup/online eligibility and scheduler load while
//! retaining ownership of task/runqueue structures. This module owns the
//! decision: UP/empty-mask fallback, current-task preference, least load, and
//! ascending-CPU tie breaking.

pub const MAX_CPUS: usize = 32;

#[repr(C)]
pub struct PlacementSnapshot {
    pub current_cpu: u32,
    pub nr_cpus: u32,
    pub eligible_mask: u32,
    pub prefer_current: u32,
    pub cpu_load: [u32; MAX_CPUS],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct PreemptionSnapshot {
    pub candidate_policy: u32,
    pub candidate_priority: i32,
    pub candidate_deadline: u64,
    pub current_valid: u32,
    pub current_idle: u32,
    pub current_running: u32,
    pub same_task: u32,
    pub current_policy: u32,
    pub current_priority: i32,
    pub current_deadline: u64,
}

const _: () = {
    assert!(core::mem::size_of::<PlacementSnapshot>() == 144);
    assert!(core::mem::align_of::<PlacementSnapshot>() == 4);
    assert!(core::mem::offset_of!(PlacementSnapshot, current_cpu) == 0);
    assert!(core::mem::offset_of!(PlacementSnapshot, nr_cpus) == 4);
    assert!(core::mem::offset_of!(PlacementSnapshot, eligible_mask) == 8);
    assert!(core::mem::offset_of!(PlacementSnapshot, prefer_current) == 12);
    assert!(core::mem::offset_of!(PlacementSnapshot, cpu_load) == 16);
    assert!(core::mem::size_of::<PreemptionSnapshot>() == 48);
    assert!(core::mem::align_of::<PreemptionSnapshot>() == 8);
    assert!(core::mem::offset_of!(PreemptionSnapshot, candidate_policy) == 0);
    assert!(core::mem::offset_of!(PreemptionSnapshot, candidate_priority) == 4);
    assert!(core::mem::offset_of!(PreemptionSnapshot, candidate_deadline) == 8);
    assert!(core::mem::offset_of!(PreemptionSnapshot, current_valid) == 16);
    assert!(core::mem::offset_of!(PreemptionSnapshot, current_idle) == 20);
    assert!(core::mem::offset_of!(PreemptionSnapshot, current_running) == 24);
    assert!(core::mem::offset_of!(PreemptionSnapshot, same_task) == 28);
    assert!(core::mem::offset_of!(PreemptionSnapshot, current_policy) == 32);
    assert!(core::mem::offset_of!(PreemptionSnapshot, current_priority) == 36);
    assert!(core::mem::offset_of!(PreemptionSnapshot, current_deadline) == 40);
};

const POLICY_FIFO: u32 = 1;
const POLICY_RR: u32 = 2;

fn is_rt(policy: u32) -> bool {
    policy == POLICY_FIFO || policy == POLICY_RR
}

// Compare two already-selected tasks.  The caller decides whether a snapshot
// represents a running task or a READY candidate; in particular, the local
// keep-current path compares a RUNNING task with a READY next task.
fn strictly_outranks(snapshot: &PreemptionSnapshot) -> bool {
    if snapshot.current_valid == 0 || snapshot.current_idle != 0 {
        return true;
    }
    if snapshot.same_task != 0 {
        return false;
    }
    let candidate_rt = is_rt(snapshot.candidate_policy);
    let current_rt = is_rt(snapshot.current_policy);
    if candidate_rt != current_rt {
        return candidate_rt;
    }
    if candidate_rt {
        snapshot.candidate_priority > snapshot.current_priority
    } else {
        snapshot.candidate_deadline < snapshot.current_deadline
    }
}

fn remote_should_preempt(snapshot: &PreemptionSnapshot) -> bool {
    snapshot.current_valid == 0
        || snapshot.current_idle != 0
        || (snapshot.current_running != 0 && strictly_outranks(snapshot))
}

fn eligible_mask(affinity: u32, cpuset: u32, online: u32, nr_cpus: u32) -> u32 {
    let configured = if nr_cpus >= 32 {
        u32::MAX
    } else if nr_cpus == 0 {
        0
    } else {
        (1u32 << nr_cpus) - 1
    };
    affinity & cpuset & online & configured
}

fn select_cpu(snapshot: &PlacementSnapshot) -> u32 {
    let current = snapshot.current_cpu;
    let limit = core::cmp::min(snapshot.nr_cpus as usize, MAX_CPUS);
    if snapshot.nr_cpus <= 1 || snapshot.eligible_mask == 0 {
        return current;
    }

    if snapshot.prefer_current != 0
        && current < MAX_CPUS as u32
        && snapshot.eligible_mask & (1u32 << current) != 0
    {
        return current;
    }

    let mut best = current;
    let mut best_load = u32::MAX;
    let mut cpu = 0usize;
    while cpu < limit {
        if snapshot.eligible_mask & (1u32 << cpu) != 0 {
            // `cpu` is bounded by min(nr_cpus, MAX_CPUS), so this entry lies
            // inside the fixed ABI array. Avoid a bounds-panic dependency in
            // the freestanding kernel object.
            let load = unsafe { *snapshot.cpu_load.get_unchecked(cpu) };
            if load < best_load {
                best = cpu as u32;
                best_load = load;
            }
        }
        cpu += 1;
    }
    best
}

/// # Safety
/// `snapshot` must be non-null, aligned, and point to a live `PlacementSnapshot`
/// whose entire object, including all 32 `cpu_load` entries, is initialized.
/// The C adapter zero-initializes the whole snapshot before filling eligible
/// CPU loads from current-task/runqueue state.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_task_placement_select(
    snapshot: *const PlacementSnapshot,
) -> u32 {
    if snapshot.is_null()
        || (snapshot as usize) % core::mem::align_of::<PlacementSnapshot>() != 0
    {
        return 0;
    }
    select_cpu(unsafe { &*snapshot })
}

#[unsafe(no_mangle)]
pub extern "C" fn a20_task_placement_eligible_mask(
    affinity: u32,
    cpuset: u32,
    online: u32,
    nr_cpus: u32,
) -> u32 {
    eligible_mask(affinity, cpuset, online, nr_cpus)
}

/// # Safety
/// `snapshot` must be non-null, aligned, and point to a fully initialized
/// `PreemptionSnapshot`. C copies the current task's policy scalars while its
/// current-slot reference is protected; no task pointer crosses this ABI.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_task_placement_should_preempt(
    snapshot: *const PreemptionSnapshot,
) -> u32 {
    if snapshot.is_null()
        || (snapshot as usize) % core::mem::align_of::<PreemptionSnapshot>() != 0
    {
        return 0;
    }
    remote_should_preempt(unsafe { &*snapshot }) as u32
}

/// # Safety
/// `snapshot` must be non-null, aligned, initialized, and valid for a read.
/// The caller supplies two selected task snapshots and preserves the local
/// scheduler's lock/lifetime contract.
#[no_mangle]
pub unsafe extern "C" fn a20_task_placement_strictly_outranks(
    snapshot: *const PreemptionSnapshot,
) -> u32 {
    if snapshot.is_null() {
        return 0;
    }
    strictly_outranks(unsafe { &*snapshot }) as u32
}

#[cfg(test)]
mod tests {
    use super::*;

    fn input(current: u32, nr: u32, mask: u32, prefer: u32, loads: &[u32]) -> PlacementSnapshot {
        let mut cpu_load = [0; MAX_CPUS];
        cpu_load[..loads.len()].copy_from_slice(loads);
        PlacementSnapshot {
            current_cpu: current,
            nr_cpus: nr,
            eligible_mask: mask,
            prefer_current: prefer,
            cpu_load,
        }
    }

    // Kept as an executable transcription of the legacy C policy so boundary
    // cases compare the migrated decision against its pre-Rust behavior.
    fn c_reference(s: &PlacementSnapshot) -> u32 {
        let current = s.current_cpu;
        if s.nr_cpus <= 1 {
            return current;
        }
        let mask = s.eligible_mask;
        if mask == 0 {
            return current;
        }
        if s.prefer_current != 0 && current < 32 && mask & (1u32 << current) != 0 {
            return current;
        }
        let mut best = current;
        let mut best_load = u32::MAX;
        let mut cpu = 0usize;
        while cpu < core::cmp::min(s.nr_cpus as usize, 32) {
            if mask & (1u32 << cpu) != 0 {
                let load = s.cpu_load[cpu];
                if load < best_load {
                    best = cpu as u32;
                    best_load = load;
                }
            }
            cpu += 1;
        }
        best
    }

    fn c_eligible_reference(affinity: u32, cpuset: u32, online: u32, nr: u32) -> u32 {
        let configured = if nr >= 32 { u32::MAX } else { (1u32 << nr) - 1 };
        affinity & cpuset & configured & online
    }

    fn c_preempt_reference(s: &PreemptionSnapshot) -> bool {
        if s.current_valid == 0 || s.current_idle != 0 {
            return true;
        }
        if s.same_task != 0 {
            return false;
        }
        let candidate_rt = s.candidate_policy == POLICY_FIFO || s.candidate_policy == POLICY_RR;
        let current_rt = s.current_policy == POLICY_FIFO || s.current_policy == POLICY_RR;
        if candidate_rt != current_rt {
            return candidate_rt;
        }
        if candidate_rt {
            s.candidate_priority > s.current_priority
        } else {
            s.candidate_deadline < s.current_deadline
        }
    }

    fn c_remote_preempt_reference(s: &PreemptionSnapshot) -> bool {
        s.current_valid == 0 || s.current_idle != 0 ||
            (s.current_running != 0 && c_preempt_reference(s))
    }

    #[test]
    fn up_and_empty_mask_preserve_current_cpu_fallback() {
        for s in [input(1, 1, 3, 0, &[0, 0]), input(7, 4, 0, 0, &[0; 4])] {
            assert_eq!(select_cpu(&s), c_reference(&s));
        }
    }

    #[test]
    fn eligible_current_task_keeps_current_cpu_even_when_another_is_quieter() {
        let s = input(1, 3, 0b111, 1, &[0, 8, 1]);
        assert_eq!(select_cpu(&s), 1);
        assert_eq!(select_cpu(&s), c_reference(&s));
    }

    #[test]
    fn current_preference_uses_the_legacy_mask_bound_not_nr_cpus() {
        let s = input(7, 2, 1 << 7, 1, &[0; 8]);
        assert_eq!(select_cpu(&s), 7);
        assert_eq!(select_cpu(&s), c_reference(&s));
    }

    #[test]
    fn least_load_respects_mask_and_ties_choose_lowest_cpu() {
        let s = input(2, 4, 0b1101, 0, &[0, 0, 0, 1]);
        assert_eq!(select_cpu(&s), 0);
        assert_eq!(select_cpu(&s), c_reference(&s));
        let only_cpu_three = input(1, 4, 0b1000, 0, &[0, 0, 0, 9]);
        assert_eq!(select_cpu(&only_cpu_three), 3);
    }

    #[test]
    fn out_of_range_mask_bits_are_ignored_and_all_max_loads_keep_legacy_fallback() {
        let outside = input(1, 2, 1 << 7, 0, &[0, 0]);
        assert_eq!(select_cpu(&outside), 1);
        assert_eq!(select_cpu(&outside), c_reference(&outside));
        let maxed = input(7, 4, 0b1111, 0, &[u32::MAX; 4]);
        assert_eq!(select_cpu(&maxed), 7);
        assert_eq!(select_cpu(&maxed), c_reference(&maxed));
    }

    #[test]
    fn ffi_rejects_null_and_accepts_well_formed_snapshot() {
        let s = input(0, 2, 0b11, 0, &[4, 2]);
        unsafe {
            assert_eq!(a20_task_placement_select(core::ptr::null()), 0);
            assert_eq!(a20_task_placement_select(&s), 1);
        }
    }

    #[test]
    fn eligibility_intersects_affinity_cpuset_online_and_configured_mask() {
        for (affinity, cpuset, online, nr) in [
            (0b1111, 0b1101, 0b1011, 4),
            (u32::MAX, u32::MAX, u32::MAX, 32),
            (u32::MAX, u32::MAX, u32::MAX, 1),
            (u32::MAX, u32::MAX, u32::MAX, 0),
        ] {
            assert_eq!(eligible_mask(affinity, cpuset, online, nr),
                       c_eligible_reference(affinity, cpuset, online, nr));
        }
    }

    #[test]
    fn preemption_preserves_rt_priority_and_normal_deadline_rules() {
        let normal_over_rt = PreemptionSnapshot {
            candidate_policy: 0, candidate_priority: 99, candidate_deadline: 1,
            current_valid: 1, current_idle: 0, current_running: 1, same_task: 0,
            current_policy: POLICY_FIFO, current_priority: 1, current_deadline: 0,
        };
        assert_eq!(strictly_outranks(&normal_over_rt), c_preempt_reference(&normal_over_rt));
        assert!(!strictly_outranks(&normal_over_rt));
        let rt_over_normal = PreemptionSnapshot {
            candidate_policy: POLICY_RR, candidate_priority: -10, candidate_deadline: 99,
            current_valid: 1, current_idle: 0, current_running: 1, same_task: 0,
            current_policy: 0, current_priority: 0, current_deadline: 0,
        };
        assert_eq!(strictly_outranks(&rt_over_normal), c_preempt_reference(&rt_over_normal));
        assert!(strictly_outranks(&rt_over_normal));
        let lower_rt_priority = PreemptionSnapshot {
            candidate_policy: POLICY_FIFO, candidate_priority: 2, candidate_deadline: 0,
            current_valid: 1, current_idle: 0, current_running: 1, same_task: 0,
            current_policy: POLICY_RR, current_priority: 3, current_deadline: 0,
        };
        assert_eq!(strictly_outranks(&lower_rt_priority), c_preempt_reference(&lower_rt_priority));
        assert!(!strictly_outranks(&lower_rt_priority));
        let earlier_deadline = PreemptionSnapshot {
            candidate_policy: 0, candidate_priority: 0, candidate_deadline: 8,
            current_valid: 1, current_idle: 0, current_running: 1, same_task: 0,
            current_policy: 0, current_priority: 0, current_deadline: 9,
        };
        assert_eq!(strictly_outranks(&earlier_deadline), c_preempt_reference(&earlier_deadline));
        assert!(strictly_outranks(&earlier_deadline));
    }

    #[test]
    fn ready_candidate_uses_strict_priority_comparison_but_remote_requires_running() {
        let ready_lower_rt = PreemptionSnapshot {
            candidate_policy: POLICY_FIFO, candidate_priority: 2, candidate_deadline: 0,
            current_valid: 1, current_idle: 0, current_running: 1, same_task: 0,
            current_policy: POLICY_RR, current_priority: 3, current_deadline: 0,
        };
        assert!(!strictly_outranks(&ready_lower_rt));
        let mut ready_higher_rt = PreemptionSnapshot {
            candidate_policy: POLICY_FIFO, candidate_priority: 4, candidate_deadline: 0,
            current_valid: 1, current_idle: 0, current_running: 0, same_task: 0,
            current_policy: POLICY_RR, current_priority: 3, current_deadline: 0,
        };
        assert!(strictly_outranks(&ready_higher_rt));
        assert!(!remote_should_preempt(&ready_higher_rt));
        assert_eq!(remote_should_preempt(&ready_higher_rt),
                   c_remote_preempt_reference(&ready_higher_rt));
        ready_higher_rt.current_running = 1;
        assert!(remote_should_preempt(&ready_higher_rt));
        assert_eq!(remote_should_preempt(&ready_higher_rt),
                   c_remote_preempt_reference(&ready_higher_rt));
        let mut ready_later_deadline = PreemptionSnapshot {
            candidate_policy: 0, candidate_priority: 0, candidate_deadline: 10,
            current_valid: 1, current_idle: 0, current_running: 1, same_task: 0,
            current_policy: 0, current_priority: 0, current_deadline: 9,
        };
        assert!(!strictly_outranks(&ready_later_deadline));
        let mut ready_earlier_deadline = ready_later_deadline;
        ready_earlier_deadline.candidate_deadline = 8;
        ready_earlier_deadline.current_deadline = 9;
        ready_earlier_deadline.current_running = 0;
        assert!(strictly_outranks(&ready_earlier_deadline));
        assert!(!remote_should_preempt(&ready_earlier_deadline));
        assert_eq!(remote_should_preempt(&ready_earlier_deadline),
                   c_remote_preempt_reference(&ready_earlier_deadline));
        ready_later_deadline.current_running = 0;
        assert!(!remote_should_preempt(&ready_later_deadline));
        assert!(!strictly_outranks(&ready_later_deadline));
    }

    #[test]
    fn preemption_noops_for_same_or_nonrunning_and_prefers_idle_slot() {
        let mut s = PreemptionSnapshot {
            candidate_policy: POLICY_FIFO, candidate_priority: 9, candidate_deadline: 0,
            current_valid: 1, current_idle: 0, current_running: 1, same_task: 1,
            current_policy: 0, current_priority: 0, current_deadline: 9,
        };
        assert!(!remote_should_preempt(&s));
        s.same_task = 0;
        s.current_running = 0;
        assert!(!remote_should_preempt(&s));
        s.current_idle = 1;
        assert!(remote_should_preempt(&s));
        s.current_valid = 0;
        s.current_idle = 0;
        assert!(remote_should_preempt(&s));
    }
}
