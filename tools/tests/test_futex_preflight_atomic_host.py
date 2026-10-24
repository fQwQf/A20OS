"""Exercise atomic usercopy and futex wait preflight from the production kernel tree.

The fixtures extract the Rust wait path and C adapter functions from ROOT/kernel;
only page access, mapping, wait-queue, and scheduler services are mocked.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
TREE = ROOT


def function(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[start:pos + 1]
    raise AssertionError(f"unterminated function: {signature}")


def item(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                end = pos + 1
                if source[end:end + 1] == ";":
                    end += 1
                return source[start:end]
    raise AssertionError(f"unterminated item: {signature}")


def rustc_test(source: str) -> None:
    with tempfile.TemporaryDirectory(prefix="a20-futex-preflight-") as tmp:
        rs = Path(tmp) / "candidate.rs"
        exe = Path(tmp) / "candidate-test"
        rs.write_text(source)
        subprocess.run(["rustc", "--edition=2021", "--test", "-C", "opt-level=1",
                        str(rs), "-o", str(exe)], check=True)
        subprocess.run([str(exe), "--test-threads=1"], check=True)


class FutexPreflightCandidate(unittest.TestCase):
    def test_real_atomic_usercopy_helper(self):
        source = (TREE / "kernel/mm/usercopy.rs").read_text()
        extern_start = source.index("unsafe extern \"C\" {")
        externs = item(source, "unsafe extern \"C\"")
        context_start = source.index("struct CopyContext {")
        context_end = source.index("unsafe fn copy_loop(", context_start)
        context = source[context_start:context_end]
        helper = function(source, "pub unsafe extern \"C\" fn a20_usercopy_read_u32_atomic_rust(")
        prefix = r'''#![allow(dead_code, improper_ctypes)]
use core::ffi::c_void;
use core::marker::PhantomData;
use core::ptr;
use core::sync::atomic::{AtomicU32, Ordering};
const EFAULT: isize = -14;
const NEED_FAULT: i32 = 1;
'''
        mocks = r'''
mod mock {
    use super::*;
    use core::sync::atomic::{AtomicI32, AtomicUsize};
    pub static DATA: AtomicU32 = AtomicU32::new(0);
    pub static MODE: AtomicI32 = AtomicI32::new(0);
    pub static RESOLVE_RC: AtomicI32 = AtomicI32::new(0);
    pub static CHUNK: AtomicUsize = AtomicUsize::new(4);
    pub static ALIAS_OFFSET: AtomicUsize = AtomicUsize::new(0);
    pub static ACCESS: AtomicUsize = AtomicUsize::new(0);
    pub static RESOLVE: AtomicUsize = AtomicUsize::new(0);
    pub static UNLOCK: AtomicUsize = AtomicUsize::new(0);
    pub fn reset() {
        DATA.store(0x8123_4567, Ordering::Relaxed);
        MODE.store(0, Ordering::Relaxed);
        RESOLVE_RC.store(0, Ordering::Relaxed);
        CHUNK.store(4, Ordering::Relaxed);
        ALIAS_OFFSET.store(0, Ordering::Relaxed);
        ACCESS.store(0, Ordering::Relaxed);
        RESOLVE.store(0, Ordering::Relaxed);
        UNLOCK.store(0, Ordering::Relaxed);
    }
    #[no_mangle]
    unsafe extern "C" fn a20_usercopy_page_access(
        _task: *mut c_void, _mm: *mut c_void, _address: usize, _write: i32,
        kernel_address: *mut *mut u8, chunk: *mut usize, irq_flags: *mut u64,
        preempt_before: *mut u32, shared_write_guard: *mut u32,
    ) -> i32 {
        let n = ACCESS.fetch_add(1, Ordering::Relaxed);
        let mode = MODE.load(Ordering::Relaxed);
        if mode < 0 { return mode; }
        if mode == 1 && n == 0 { return 1; }
        unsafe {
            *kernel_address = DATA.as_ptr().cast::<u8>().add(ALIAS_OFFSET.load(Ordering::Relaxed));
            *chunk = CHUNK.load(Ordering::Relaxed);
            *irq_flags = 0x33;
            *preempt_before = 7;
            *shared_write_guard = 0;
        }
        0
    }
    #[no_mangle]
    unsafe extern "C" fn a20_usercopy_resolve_fault(
        _task: *mut c_void, _mm: *mut c_void, _address: usize, _write: i32,
        _irq_before: u64, _preempt_before: u32,
    ) -> i32 {
        RESOLVE.fetch_add(1, Ordering::Relaxed);
        RESOLVE_RC.load(Ordering::Relaxed)
    }
    #[no_mangle]
    unsafe extern "C" fn a20_usercopy_page_unlock(
        _mm: *mut c_void, _irq_flags: u64, _preempt_before: u32,
        _shared_write_guard: u32,
    ) { UNLOCK.fetch_add(1, Ordering::Relaxed); }
}
'''
        tests = r'''
#[test]
fn aligned_word_is_atomic_and_guard_is_released() {
    mock::reset();
    let mut out = 0u32;
    let rc = unsafe { a20_usercopy_read_u32_atomic_rust(1usize as *mut c_void,
        2usize as *mut c_void, 0x4000, &mut out) };
    assert_eq!(rc, 0);
    assert_eq!(out, 0x8123_4567);
    assert_eq!(mock::ACCESS.load(Ordering::Relaxed), 1);
    assert_eq!(mock::UNLOCK.load(Ordering::Relaxed), 1);
}
#[test]
fn need_fault_resolves_unlocked_then_retries() {
    mock::reset(); mock::MODE.store(1, Ordering::Relaxed);
    let mut out = 0u32;
    let rc = unsafe { a20_usercopy_read_u32_atomic_rust(1usize as *mut c_void,
        2usize as *mut c_void, 0x4000, &mut out) };
    assert_eq!(rc, 0);
    assert_eq!(mock::ACCESS.load(Ordering::Relaxed), 2);
    assert_eq!(mock::RESOLVE.load(Ordering::Relaxed), 1);
    assert_eq!(mock::UNLOCK.load(Ordering::Relaxed), 1);
}
#[test]
fn faults_alignment_and_short_or_unaligned_alias_fail_cleanly() {
    mock::reset(); let mut out = 0u32;
    assert_eq!(unsafe { a20_usercopy_read_u32_atomic_rust(1usize as *mut c_void,
        2usize as *mut c_void, 0x4001, &mut out) }, -14);
    assert_eq!(mock::ACCESS.load(Ordering::Relaxed), 0);
    assert_eq!(unsafe { a20_usercopy_read_u32_atomic_rust(1usize as *mut c_void,
        2usize as *mut c_void, 0x4000, 1usize as *mut u32) }, -14);
    assert_eq!(mock::ACCESS.load(Ordering::Relaxed), 0);
    mock::reset(); mock::MODE.store(-14, Ordering::Relaxed);
    assert_eq!(unsafe { a20_usercopy_read_u32_atomic_rust(1usize as *mut c_void,
        2usize as *mut c_void, 0x4000, &mut out) }, -14);
    assert_eq!(mock::UNLOCK.load(Ordering::Relaxed), 0);
    mock::reset(); mock::CHUNK.store(3, Ordering::Relaxed);
    assert_eq!(unsafe { a20_usercopy_read_u32_atomic_rust(1usize as *mut c_void,
        2usize as *mut c_void, 0x4000, &mut out) }, -14);
    assert_eq!(mock::UNLOCK.load(Ordering::Relaxed), 1);
    mock::reset(); mock::ALIAS_OFFSET.store(1, Ordering::Relaxed);
    assert_eq!(unsafe { a20_usercopy_read_u32_atomic_rust(1usize as *mut c_void,
        2usize as *mut c_void, 0x4000, &mut out) }, -14);
    assert_eq!(mock::UNLOCK.load(Ordering::Relaxed), 1);
    mock::reset(); mock::MODE.store(1, Ordering::Relaxed);
    mock::RESOLVE_RC.store(-14, Ordering::Relaxed);
    assert_eq!(unsafe { a20_usercopy_read_u32_atomic_rust(1usize as *mut c_void,
        2usize as *mut c_void, 0x4000, &mut out) }, -14);
    assert_eq!(mock::ACCESS.load(Ordering::Relaxed), 1);
    assert_eq!(mock::RESOLVE.load(Ordering::Relaxed), 1);
    assert_eq!(mock::UNLOCK.load(Ordering::Relaxed), 0);
}
'''
        rustc_test(prefix + externs + context + helper + mocks + tests)

    def test_real_futex_wait_preflight_admission(self):
        source = (TREE / "kernel/ipc/futex_wait.rs").read_text()
        names = [
            "struct ProcWaitToken", "pub struct WaitQueueEntry", "struct WaitQueue {",
            "struct FutexWaitKey", "struct FutexWaiter", "struct FutexWakeArg",
            "struct FutexRequeueArg", "struct LinkedWaiter",
            "struct MappingPin", "struct BucketLock", "struct ParkToken",
            "impl Drop for LinkedWaiter", "impl Drop for MappingPin",
            "impl BucketLock", "impl Drop for BucketLock", "impl ParkToken",
            "impl Drop for ParkToken", "fn bucket_index(",
            "pub unsafe extern \"C\" fn a20_futex_wait_ticks_rust(",
        ]
        pieces = []
        for signature in names:
            extracted = (function(source, signature) if signature.startswith("pub unsafe") or
                         signature.startswith("fn bucket_index") else item(source, signature))
            if signature == "struct ProcWaitToken":
                extracted = "#[repr(C)]\n#[derive(Clone, Copy)]\n" + extracted
            elif signature in ("pub struct WaitQueueEntry", "struct WaitQueue",
                               "struct FutexWaitKey", "struct FutexWaiter",
                               "struct FutexWakeArg", "struct FutexRequeueArg"):
                extracted = "#[repr(C)]\n" + extracted
            pieces.append(extracted)
        externs = item(source, "unsafe extern \"C\"")
        prefix = r'''#![allow(dead_code, improper_ctypes)]
use core::ffi::c_void;
use core::ptr;
use core::sync::atomic::{AtomicI32, Ordering};
const EFAULT: i32 = 14; const EINVAL: i32 = 22; const ESRCH: i32 = 3;
const EAGAIN: i32 = 11; const ERESTARTSYS: i32 = 512;
const PROC_WAIT_INTERRUPTIBLE: i32 = 1; const PROC_WAKE_TIMEOUT: i32 = 2;
const ETIMEDOUT: i32 = 110; const BUCKETS: usize = 64;
'''
        mocks = r'''
mod mock {
    use super::*;
    use core::sync::atomic::{AtomicI32, AtomicU64, AtomicUsize};
    pub static PRE_RC: AtomicI32 = AtomicI32::new(0);
    pub static PRE_VALUE: AtomicI32 = AtomicI32::new(7);
    pub static WORD: AtomicI32 = AtomicI32::new(7);
    pub static MAP_MUTATE: AtomicI32 = AtomicI32::new(0);
    pub static BUCKET_MUTATE: AtomicI32 = AtomicI32::new(0);
    pub static PARK_REASON: AtomicI32 = AtomicI32::new(0);
    pub static NOW: AtomicU64 = AtomicU64::new(100);
    pub static UNTIL: AtomicU64 = AtomicU64::new(0);
    pub static PREFLIGHT: AtomicUsize = AtomicUsize::new(0);
    pub static TIMER: AtomicUsize = AtomicUsize::new(0);
    pub static PARK_PREPARE: AtomicUsize = AtomicUsize::new(0);
    pub static PARK_CANCEL: AtomicUsize = AtomicUsize::new(0);
    pub static PARK_COMMIT: AtomicUsize = AtomicUsize::new(0);
    pub static PARK_FINISH: AtomicUsize = AtomicUsize::new(0);
    pub static MAP_LOCK: AtomicUsize = AtomicUsize::new(0);
    pub static MAP_UNLOCK: AtomicUsize = AtomicUsize::new(0);
    pub static BUCKET_LOCK: AtomicUsize = AtomicUsize::new(0);
    pub static BUCKET_UNLOCK: AtomicUsize = AtomicUsize::new(0);
    pub static LINK: AtomicUsize = AtomicUsize::new(0);
    pub static UNLINK: AtomicUsize = AtomicUsize::new(0);
    pub fn reset() {
        PRE_RC.store(0, Ordering::Relaxed); PRE_VALUE.store(7, Ordering::Relaxed);
        WORD.store(7, Ordering::Relaxed); MAP_MUTATE.store(0, Ordering::Relaxed);
        BUCKET_MUTATE.store(0, Ordering::Relaxed); PARK_REASON.store(0, Ordering::Relaxed);
        NOW.store(100, Ordering::Relaxed); UNTIL.store(0, Ordering::Relaxed);
        for c in [&PREFLIGHT,&TIMER,&PARK_PREPARE,&PARK_CANCEL,&PARK_COMMIT,&PARK_FINISH,
                  &MAP_LOCK,&MAP_UNLOCK,&BUCKET_LOCK,&BUCKET_UNLOCK,&LINK,&UNLINK] {
            c.store(0, Ordering::Relaxed);
        }
    }
    #[no_mangle] unsafe extern "C" fn a20_futex_current_task() -> *mut c_void { 1usize as *mut c_void }
    #[no_mangle] unsafe extern "C" fn a20_futex_task_mm(_task:*mut c_void)->*mut c_void { 2usize as *mut c_void }
    #[no_mangle] unsafe extern "C" fn a20_futex_preflight_word(_addr:*const i32,out:*mut i32)->i32 {
        PREFLIGHT.fetch_add(1,Ordering::Relaxed);
        let rc=PRE_RC.load(Ordering::Relaxed);
        if rc<0 { return rc; }
        unsafe { ptr::write(out,PRE_VALUE.load(Ordering::Relaxed)); } 0
    }
    #[no_mangle] unsafe extern "C" fn a20_futex_mapping_lock(_task:*mut c_void,_addr:*mut i32,
        word:*mut *mut i32,pkey:*mut usize,flags:*mut u64)->i32 {
        MAP_LOCK.fetch_add(1,Ordering::Relaxed);
        let change=MAP_MUTATE.swap(0,Ordering::Relaxed); if change!=0 { WORD.store(change,Ordering::Relaxed); }
        unsafe { *word=WORD.as_ptr() as *mut i32; *pkey=0x2000; *flags=0x44; } 0
    }
    #[no_mangle] unsafe extern "C" fn a20_futex_mapping_unlock(_task:*mut c_void,_flags:u64) { MAP_UNLOCK.fetch_add(1,Ordering::Relaxed); }
    #[no_mangle] unsafe extern "C" fn a20_futex_bucket(_index:u32)->*mut WaitQueue { 3usize as *mut WaitQueue }
    #[no_mangle] unsafe extern "C" fn a20_futex_bucket_lock(_q:*mut WaitQueue)->u64 {
        BUCKET_LOCK.fetch_add(1,Ordering::Relaxed);
        let change=BUCKET_MUTATE.swap(0,Ordering::Relaxed); if change!=0 { WORD.store(change,Ordering::Relaxed); } 0
    }
    #[no_mangle] unsafe extern "C" fn a20_futex_bucket_unlock(_q:*mut WaitQueue,_f:u64) { BUCKET_UNLOCK.fetch_add(1,Ordering::Relaxed); }
    #[no_mangle] unsafe extern "C" fn a20_futex_wait_link_locked(_q:*mut WaitQueue,e:*mut WaitQueueEntry,_t:ProcWaitToken)->bool {
        LINK.fetch_add(1,Ordering::Relaxed); unsafe { (*e).linked=true; } true
    }
    #[no_mangle] unsafe extern "C" fn a20_futex_wait_unlink(_q:*mut WaitQueue,_e:*mut WaitQueueEntry) { UNLINK.fetch_add(1,Ordering::Relaxed); }
    #[no_mangle] unsafe extern "C" fn proc_park_prepare(_mode:i32,until:u64)->ProcWaitToken {
        PARK_PREPARE.fetch_add(1,Ordering::Relaxed); UNTIL.store(until,Ordering::Relaxed);
        ProcWaitToken { task:4usize as *mut c_void, seq:9, prepare_error:0 }
    }
    #[no_mangle] unsafe extern "C" fn proc_park_cancel(_t:ProcWaitToken)->i32 { PARK_CANCEL.fetch_add(1,Ordering::Relaxed); 0 }
    #[no_mangle] unsafe extern "C" fn proc_park_commit(_t:ProcWaitToken)->i32 { PARK_COMMIT.fetch_add(1,Ordering::Relaxed); PARK_REASON.load(Ordering::Relaxed) }
    #[no_mangle] unsafe extern "C" fn proc_park_finish(_t:ProcWaitToken) { PARK_FINISH.fetch_add(1,Ordering::Relaxed); }
    #[no_mangle] unsafe extern "C" fn a20_futex_signal_unblocked(_t:*mut c_void)->bool { false }
    #[no_mangle] unsafe extern "C" fn a20_futex_wake_is_interrupt(_r:i32)->bool { false }
    #[no_mangle] unsafe extern "C" fn a20_futex_entry_task_dead(_e:*const WaitQueueEntry)->bool { false }
    #[no_mangle] unsafe extern "C" fn a20_futex_requeue_matching(
        _s:*mut WaitQueue,_t:*mut WaitQueue,_m:unsafe extern "C" fn(*const WaitQueueEntry,*mut c_void)->bool,
        _a:*mut c_void,_l:u32,_r:unsafe extern "C" fn(*mut WaitQueueEntry,*mut c_void),_ra:*mut c_void)->u32 { 0 }
    #[no_mangle] unsafe extern "C" fn timer_get_ticks()->u64 { TIMER.fetch_add(1,Ordering::Relaxed); NOW.load(Ordering::Relaxed) }
}
'''
        tests = r'''
#[test]
fn stale_preflight_and_fault_return_before_timer_park_or_mapping() {
    mock::reset(); mock::PRE_VALUE.store(99,Ordering::Relaxed);
    assert_eq!(unsafe { a20_futex_wait_ticks_rust(0x1000 as *mut i32,7,9,1,0) },-EAGAIN);
    assert_eq!(mock::PREFLIGHT.load(Ordering::Relaxed),1);
    assert_eq!(mock::TIMER.load(Ordering::Relaxed),0);
    assert_eq!(mock::PARK_PREPARE.load(Ordering::Relaxed),0);
    assert_eq!(mock::MAP_LOCK.load(Ordering::Relaxed),0);
    mock::reset(); mock::PRE_RC.store(-EFAULT,Ordering::Relaxed);
    assert_eq!(unsafe { a20_futex_wait_ticks_rust(0x1000 as *mut i32,7,9,1,0) },-EFAULT);
    assert_eq!(mock::TIMER.load(Ordering::Relaxed),0);
    assert_eq!(mock::PARK_PREPARE.load(Ordering::Relaxed),0);
    assert_eq!(mock::MAP_LOCK.load(Ordering::Relaxed),0);
}
#[test]
fn mapping_and_bucket_rechecks_still_cancel_prepared_park() {
    mock::reset(); mock::MAP_MUTATE.store(8,Ordering::Relaxed);
    assert_eq!(unsafe { a20_futex_wait_ticks_rust(0x1000 as *mut i32,7,0,1,0) },-EAGAIN);
    assert_eq!(mock::TIMER.load(Ordering::Relaxed),0);
    assert_eq!(mock::MAP_UNLOCK.load(Ordering::Relaxed),1);
    assert_eq!(mock::BUCKET_LOCK.load(Ordering::Relaxed),0);
    assert_eq!(mock::PARK_CANCEL.load(Ordering::Relaxed),1);
    assert_eq!(mock::PARK_FINISH.load(Ordering::Relaxed),1);
    mock::reset(); mock::BUCKET_MUTATE.store(8,Ordering::Relaxed);
    assert_eq!(unsafe { a20_futex_wait_ticks_rust(0x1000 as *mut i32,7,0,1,0) },-EAGAIN);
    assert_eq!(mock::BUCKET_LOCK.load(Ordering::Relaxed),1);
    assert_eq!(mock::BUCKET_UNLOCK.load(Ordering::Relaxed),1);
    assert_eq!(mock::MAP_UNLOCK.load(Ordering::Relaxed),1);
    assert_eq!(mock::LINK.load(Ordering::Relaxed),0);
    assert_eq!(mock::PARK_CANCEL.load(Ordering::Relaxed),1);
}
#[test]
fn matching_finite_wait_keeps_deadline_and_infinite_wait_skips_clock() {
    mock::reset(); mock::PARK_REASON.store(PROC_WAKE_TIMEOUT,Ordering::Relaxed);
    assert_eq!(unsafe { a20_futex_wait_ticks_rust(0x1000 as *mut i32,7,9,1,0) },-ETIMEDOUT);
    assert_eq!(mock::TIMER.load(Ordering::Relaxed),1);
    assert_eq!(mock::UNTIL.load(Ordering::Relaxed),109);
    assert_eq!(mock::LINK.load(Ordering::Relaxed),1);
    assert_eq!(mock::UNLINK.load(Ordering::Relaxed),1);
    mock::reset();
    assert_eq!(unsafe { a20_futex_wait_ticks_rust(0x1000 as *mut i32,7,0,1,0) },0);
    assert_eq!(mock::TIMER.load(Ordering::Relaxed),0);
    assert_eq!(mock::UNTIL.load(Ordering::Relaxed),0);
    assert_eq!(mock::LINK.load(Ordering::Relaxed),1);
    assert_eq!(mock::UNLINK.load(Ordering::Relaxed),1);
}
'''
        rustc_test(prefix + "\n".join(pieces[:0]) + externs + "\n".join(pieces) + mocks + tests)

    def test_real_c_usercopy_wrapper_and_futex_bit_preservation(self):
        mm_source = (TREE / "kernel/mm/mm.c").read_text()
        futex_source = (TREE / "kernel/ipc/futex.c").read_text()
        body = "\n\n".join([
            function(mm_source, "static inline int user_range_ok("),
            function(mm_source, "static mm_struct_t *usercopy_pin_mm("),
            function(mm_source, "int user_read_u32_atomic("),
            function(futex_source, "int a20_futex_preflight_word("),
        ])
        harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define EFAULT 14
#define USER_VA_LIMIT UINT64_C(0xfffffffffffff000)
typedef uintptr_t vaddr_t;
typedef struct mm_struct { unsigned refs; } mm_struct_t;
typedef struct task_t { mm_struct_t *mm; int park_lock; } task_t;
static mm_struct_t g_mm;
static task_t g_task={&g_mm,0};
static task_t *g_current=&g_task;
static int pins,unpins,gets,put_count,rust_calls,rust_rc;
static uint32_t rust_bits;
task_t *proc_current(void) { return g_current; }
uint64_t spin_lock_irqsave(int *lock) { (void)lock; pins++; return 0x55; }
void spin_unlock_irqrestore(int *lock,uint64_t flags) { (void)lock; assert(flags==0x55); unpins++; }
mm_struct_t *mm_get(mm_struct_t *mm) { gets++; mm->refs++; return mm; }
void mm_destroy(mm_struct_t *mm) { put_count++; assert(mm->refs); mm->refs--; }
int a20_usercopy_read_u32_atomic_rust(task_t *t,mm_struct_t *mm,
                                      uintptr_t src,uint32_t *out) {
    rust_calls++; assert(t==g_current && mm==&g_mm);
    assert((src&3)==0);
    if(rust_rc<0) return rust_rc;
    *out=rust_bits; return 0;
}
/* REAL_C_BODIES */
int main(void) {
    int out=0; uint32_t expected=UINT32_C(0xdeadbeef);
    rust_bits=expected;
    assert(a20_futex_preflight_word((const int *)(uintptr_t)0x4000,&out)==0);
    assert(memcmp(&out,&expected,sizeof(out))==0);
    assert(rust_calls==1 && gets==1 && put_count==1 && pins==1 && unpins==1 && g_mm.refs==0);
    rust_rc=-EFAULT;
    assert(a20_futex_preflight_word((const int *)(uintptr_t)0x4000,&out)==-EFAULT);
    assert(rust_calls==2 && gets==2 && put_count==2 && g_mm.refs==0);
    rust_rc=0;
    assert(a20_futex_preflight_word((const int *)(uintptr_t)0x4001,&out)==-EFAULT);
    assert(rust_calls==2 && gets==2);
    assert(a20_futex_preflight_word((const int *)(uintptr_t)0x4000,NULL)==-EFAULT);
    assert(rust_calls==2 && gets==2);
    assert(user_read_u32_atomic((const uint32_t *)(uintptr_t)USER_VA_LIMIT,&expected)==-EFAULT);
    assert(rust_calls==2 && gets==2);
    g_current=NULL;
    assert(user_read_u32_atomic((const uint32_t *)(uintptr_t)0x4000,&expected)==-EFAULT);
    assert(rust_calls==2 && gets==2);
    puts("FUTEX_PREFLIGHT_C_ADAPTER: PASS bit pattern, range/alignment, pin lifetime");
    return 0;
}
'''.replace("/* REAL_C_BODIES */", body)
        with tempfile.TemporaryDirectory(prefix="a20-futex-preflight-c-") as tmp:
            cfile, exe = Path(tmp) / "candidate.c", Path(tmp) / "candidate"
            cfile.write_text(harness)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(cfile), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    unittest.main()
