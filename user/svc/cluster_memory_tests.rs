use super::*;
use std::sync::{Arc, Barrier};

fn id(serial: u64) -> [u8; 64] {
    let mut p = [0; 64];
    p[0] = 1;
    p[16] = 2;
    put64(&mut p, 32, 3);
    put64(&mut p, 40, serial);
    p[48] = 1;
    p
}
fn owner(mode: u32, rate: u32) -> Owner {
    Owner::new(
        Config {
            mode,
            pages: PAGES as u32,
            transaction_ms: 100,
            bytes_per_second: rate,
        },
        id(1),
    )
}
fn request(op: u8, client: u64) -> [u8; MAX] {
    let mut q = [0; MAX];
    q[..4].copy_from_slice(&[b'M', b'P', 1, op]);
    put64(&mut q, 8, 1);
    q[64..128].copy_from_slice(&id(1));
    q[128..192].copy_from_slice(&id(client));
    if op == 1 {
        q[64..128].fill(0);
    }
    if op == 2 {
        put32(&mut q, 24, CHUNK as u32);
    }
    if op >= 3 {
        put64(&mut q, 32, 1);
        put64(&mut q, 48, 7);
    }
    q
}
fn call(o: &Owner, q: &[u8], now: u64) -> Vec<u8> {
    let mut r = vec![0; MAX];
    let n = o.dispatch(q, &mut r, now);
    assert!(n >= HEADER);
    r.truncate(n);
    r
}
fn status(o: &Owner, q: &[u8], now: u64) -> u32 {
    u32_at(&call(o, q, now), 4)
}
fn begin(o: &Owner, client: u64, now: u64) -> ([u8; MAX], u64) {
    let mut q = request(3, client);
    let r = call(o, &q[..HEADER], now);
    assert_eq!(u32_at(&r, 4), OK);
    let token = u64_at(&r, 40);
    assert!(token != 0);
    put64(&mut q, 40, token);
    (q, token)
}
fn stage(o: &Owner, q: &mut [u8; MAX], now: u64, value: u8) {
    q[3] = 4;
    put32(q, 24, CHUNK as u32);
    for chunk in 0..8 {
        put32(q, 20, (chunk * CHUNK) as u32);
        q[HEADER..].fill(value);
        assert_eq!(status(o, q, now), OK);
    }
    q[3] = 5;
    put32(q, 20, 0);
    put32(q, 24, 0);
}

#[test]
fn configuration_and_service_restart_are_enforced() {
    let q = request(2, 2);
    assert_eq!(status(&owner(0, 0), &q[..HEADER], 1), DISABLED);
    let ro = owner(1, 0);
    assert_eq!(status(&ro, &q[..HEADER], 1), OK);
    assert_eq!(status(&ro, &request(3, 2)[..HEADER], 1), DENIED);
    let mut stale = q;
    stale[64] = 9;
    assert_eq!(status(&ro, &stale[..HEADER], 1), STALE);
    let mut foreign = q;
    foreign[128] = 9;
    assert_eq!(status(&ro, &foreign[..HEADER], 1), INVALID);
    let r = call(&ro, &request(1, 2)[..HEADER], 1);
    assert_eq!(u32_at(&r, 16), 1);
    assert_eq!(u32_at(&r, 20), PAGES as u32);
}

#[test]
fn stage_is_invisible_until_complete_commit_and_replays_do_not_write_twice() {
    let o = owner(2, 0);
    let (mut q, token) = begin(&o, 2, 1);
    let duplicate = call(&o, &request(3, 2)[..HEADER], 2);
    assert_eq!(u64_at(&duplicate, 40), token);
    q[3] = 5;
    assert_eq!(status(&o, &q[..HEADER], 2), INCOMPLETE);
    stage(&o, &mut q, 3, 42);
    let before = call(&o, &request(2, 3)[..HEADER], 3);
    assert!(before[HEADER..].iter().all(|&b| b == 0));
    assert_eq!(u64_at(&before, 32), 1);
    let r = call(&o, &q[..HEADER], 4);
    assert_eq!(u32_at(&r, 4), OK);
    assert_eq!(u64_at(&r, 32), 2);
    let replay = call(&o, &q[..HEADER], 5);
    assert_eq!(u32_at(&replay, 4), OK);
    assert_eq!(u64_at(&replay, 32), 2);
    let after = call(&o, &request(2, 3)[..HEADER], 5);
    assert!(after[HEADER..].iter().all(|&b| b == 42));
    assert_eq!(u64_at(&after, 32), 2);
    assert_eq!(status(&o, &request(3, 2)[..HEADER], 6), STALE);
    let mut old_read = request(2, 3);
    put64(&mut old_read, 32, 1);
    assert_eq!(status(&o, &old_read[..HEADER], 6), CONFLICT);
}

#[test]
fn timeout_abort_and_late_packets_cannot_corrupt_next_transaction() {
    let o = owner(2, 0);
    let (mut old, _) = begin(&o, 2, 1);
    stage(&o, &mut old, 2, 42);
    assert_eq!(status(&o, &request(3, 3)[..HEADER], 100), BUSY);
    let (mut next, next_token) = begin(&o, 3, 101);
    assert!(next_token > u64_at(&old, 40));
    assert_eq!(status(&o, &old[..HEADER], 102), STALE);
    stage(&o, &mut next, 103, 7);
    next[3] = 6;
    assert_eq!(status(&o, &next[..HEADER], 104), OK);
    next[3] = 5;
    assert_eq!(status(&o, &next[..HEADER], 105), STALE);
    let r = call(&o, &request(2, 2)[..HEADER], 106);
    assert_eq!(u64_at(&r, 32), 1);
    assert!(r[HEADER..].iter().all(|&b| b == 0));
}

#[test]
fn duplicate_chunks_cannot_change_acknowledged_content_or_hijack_a_writer() {
    let o = owner(2, 0);
    let (mut q, _) = begin(&o, 2, 1);
    q[3] = 4;
    put32(&mut q, 24, CHUNK as u32);
    q[HEADER..].fill(1);
    assert_eq!(status(&o, &q, 2), OK);
    assert_eq!(status(&o, &q, 3), OK);
    q[HEADER] = 2;
    assert_eq!(status(&o, &q, 4), CONFLICT);
    q[HEADER] = 1;
    q[128..192].copy_from_slice(&id(3));
    assert_eq!(status(&o, &q, 5), STALE);
}

#[test]
fn quota_is_aggregate_bounded_and_backwards_time_does_not_refill() {
    let o = owner(2, 512);
    let q = request(2, 2);
    assert_eq!(status(&o, &q[..HEADER], 1000), OK);
    assert_eq!(status(&o, &q[..HEADER], 1001), THROTTLED);
    assert_eq!(status(&o, &q[..HEADER], 999), THROTTLED);
    assert_eq!(status(&o, &q[..HEADER], 2000), OK);
    assert_eq!(status(&o, &q[..HEADER], 2000), THROTTLED);
    assert_eq!(status(&o, &q[..HEADER], u64::MAX), OK);
    assert_eq!(status(&o, &q[..HEADER], u64::MAX), THROTTLED);
}

#[test]
fn concurrent_writers_have_one_owner_and_whole_snapshots() {
    let o = Arc::new(owner(2, 0));
    let barrier = Arc::new(Barrier::new(17));
    let threads: Vec<_> = (2..18)
        .map(|client| {
            let o = o.clone();
            let b = barrier.clone();
            std::thread::spawn(move || {
                b.wait();
                let r = call(&o, &request(3, client)[..HEADER], 1);
                let status = u32_at(&r, 4);
                assert!(status == OK || status == BUSY);
                if status == OK {
                    Some((client, u64_at(&r, 40)))
                } else {
                    None
                }
            })
        })
        .collect();
    barrier.wait();
    let winners: Vec<_> = threads
        .into_iter()
        .filter_map(|t| t.join().unwrap())
        .collect();
    assert_eq!(winners.len(), 1);
    let (client, token) = winners[0];
    let mut q = request(3, client);
    put64(&mut q, 40, token);
    stage(&o, &mut q, 2, client as u8);
    assert_eq!(status(&o, &q[..HEADER], 3), OK);
    for offset in (0..PAGE).step_by(CHUNK) {
        let mut q = request(2, 20);
        put32(&mut q, 20, offset as u32);
        let r = call(&o, &q[..HEADER], 4);
        assert!(r[HEADER..].iter().all(|&b| b == client as u8));
    }
}

#[test]
fn malformed_wire_never_panics_and_does_not_publish_or_mutate() {
    let o = owner(2, 0);
    let q = request(2, 2);
    let mut seed = 0x12345678u32;
    for _ in 0..100000 {
        let mut bytes = q;
        for _ in 0..4 {
            seed = seed.wrapping_mul(1664525).wrapping_add(1013904223);
            bytes[seed as usize % MAX] = (seed >> 24) as u8;
        }
        let len = (seed as usize) % (MAX + 1);
        let mut reply = [0; MAX];
        let n = o.dispatch(&bytes[..len], &mut reply, 5);
        assert!(n == 0 || (HEADER..=MAX).contains(&n));
    }
    for op in 1..=6 {
        let mut q = request(op, 2);
        put64(&mut q, 32, u64::MAX);
        put64(&mut q, 40, u64::MAX);
        let _ = call(&o, &q[..HEADER], 6);
    }
    let r = call(&o, &request(2, 2)[..HEADER], 7);
    assert_eq!(u64_at(&r, 32), 1);
    assert!(r[HEADER..].iter().all(|&b| b == 0));
}

#[test]
fn exhaustion_never_wraps_and_ffi_storage_size_is_sufficient() {
    assert!(core::mem::size_of::<Owner>() <= PAGES * (PAGE + 32) + 8192);
    assert!(core::mem::align_of::<Owner>() <= 8);
    let o = owner(2, 0);
    {
        let mut s = o.lock().unwrap();
        s.next_token = u64::MAX;
    }
    assert_eq!(status(&o, &request(3, 2)[..HEADER], 1), EXHAUSTED);
    let o = owner(2, 0);
    assert_eq!(status(&o, &request(3, 2)[..HEADER], u64::MAX), EXHAUSTED);
}
