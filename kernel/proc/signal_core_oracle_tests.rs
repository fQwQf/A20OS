// Independent oracle tests for the signal decision core ABI
// (kernel/proc/signal_core.rs).
//
// Authored by the Signal role from the frozen C reference in
// kernel/proc/signal.c at 8a85da5b, as a THIRD encoding independent of both
// the Rust tables and the in-file `tests` oracle. Cross-checked against the
// guest-validated semantics on 2026-10-16.
//
// Correction vs the free-signal-oracle-tests request: the C reference does
// NOT keep SIGKILL/SIGSTOP always blocked. sys_sigprocmask_impl strips those
// bits from the syscall OPERAND before the transition; SIG_SETMASK with a
// full mask therefore legitimately CLEARS them from sig_blocked, and a guest
// (ptrace SIGSTOP delivery) depends on that. The transition ABI itself is
// operand-agnostic: bits present in `set` pass through like any other bit.

use super::*;

#[test]
fn mask_apply_catalog_sequences() {
    let (m10, m12, m14) = (bit(10), bit(12), bit(14));
    // M01: BLOCK 10; BLOCK 12; UNBLOCK 10; SETMASK 14
    assert_eq!(a20_signal_mask_apply(SIG_BLOCK, 0, m10), m10);
    assert_eq!(a20_signal_mask_apply(SIG_BLOCK, m10, m12), m10 | m12);
    assert_eq!(a20_signal_mask_apply(SIG_UNBLOCK, m10 | m12, m10), m12);
    assert_eq!(a20_signal_mask_apply(SIG_SETMASK, m10 | m12, m14), m14);
    // M02: KILL/STOP bits are operand policy (adapter strip), not transition
    // policy: through the pure ABI they behave like any other bit.
    assert_eq!(a20_signal_mask_apply(SIG_BLOCK, m14, bit(9) | bit(19) | m10),
               m10 | m14 | bit(9) | bit(19));
    assert_eq!(a20_signal_mask_apply(SIG_SETMASK, m10 | m14, bit(9) | bit(19)),
               bit(9) | bit(19));
    // M03: invalid how with non-null set -> identity (syscall rejects)
    assert_eq!(a20_signal_mask_apply(7, m10, m12), m10);
    assert_eq!(a20_signal_mask_apply(-1, m10, m12), m10);
    // Bit 0 is outside the signal domain; bit 63 is in the u64 domain.
    assert_eq!(a20_signal_mask_apply(SIG_BLOCK, 0, 1u64), 1u64);
    assert_eq!(a20_signal_mask_apply(SIG_BLOCK, 0, 1u64 << 63), 1u64 << 63);
}

#[test]
fn deliverable_fatal_and_generation_catalog() {
    // Union of both scopes minus blocked.
    assert_eq!(a20_signal_deliverable_mask(bit(10), bit(12), 0),
               bit(10) | bit(12));
    assert_eq!(a20_signal_deliverable_mask(bit(10), bit(12), bit(10)),
               bit(12));
    assert_eq!(a20_signal_deliverable_mask(0, 0, u64::MAX), 0);
    // Fatality family: TERM fatal by default; blocked / handled / CONT / STOP
    // are not.
    assert_eq!(a20_signal_action_fatal(0, SIG_DFL, 15), 1);
    assert_eq!(a20_signal_action_fatal(bit(15), SIG_DFL, 15), 0);
    assert_eq!(a20_signal_action_fatal(0, 0x4000, 15), 0);
    assert_eq!(a20_signal_action_fatal(0, SIG_DFL, 18), 0);
    assert_eq!(a20_signal_action_fatal(0, SIG_DFL, 19), 0);
    // Deliverability family: default-ignored CHLD, explicit IGN, real handler.
    assert_eq!(a20_signal_action_deliverable(0, SIG_DFL, 17), 0);
    assert_eq!(a20_signal_action_deliverable(0, SIG_IGN, 10), 0);
    assert_eq!(a20_signal_action_deliverable(0, 0x4000, 10), 1);
    // Generation-cancel masks.
    let stops = bit(19) | bit(20) | bit(21) | bit(22);
    assert_eq!(a20_signal_generation_cancel_mask(18), stops);
    assert_eq!(a20_signal_generation_cancel_mask(19), bit(18));
    assert_eq!(a20_signal_generation_cancel_mask(10), 0);
    // Wait-status encodings.
    assert_eq!(a20_signal_wait_status_dumped(15, 0), 15);
    assert_eq!(a20_signal_wait_status_dumped(11, 1), 11 | 0x80);
    assert_eq!(a20_signal_fatal_exit_code_dumped(11, 1), -(11 | 0x80));
}
