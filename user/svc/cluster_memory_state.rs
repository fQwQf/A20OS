//! Bounded remote page owner. No allocator, network, C locks or callbacks.
//! All mutable bytes and transaction metadata live behind one nonblocking
//! acquire/release gate. A write is visible only at COMMIT; expired staging
//! is discarded, so fencing never depends on revoking a remote CPU mapping.
#![cfg_attr(not(test), no_std)]

use core::cell::UnsafeCell;
use core::sync::atomic::{AtomicBool, Ordering};

const fn page_cap() -> usize {
    let s = match option_env!("A20_MEMORY_MAX_PAGES") {
        Some(s) => s,
        None => "8",
    };
    let b = s.as_bytes();
    let mut n = 0;
    let mut i = 0;
    while i < b.len() {
        assert!(b[i] >= b'0' && b[i] <= b'9');
        n = n * 10 + (b[i] - b'0') as usize;
        assert!(n <= 64);
        i += 1;
    }
    assert!(n > 0);
    n
}
const PAGES: usize = page_cap();
const PAGE: usize = 4096;
const CHUNK: usize = 512;
const HEADER: usize = 192;
const MAX: usize = HEADER + CHUNK;
const OK: u32 = 0;
const INVALID: u32 = 1;
const DISABLED: u32 = 2;
const DENIED: u32 = 3;
const STALE: u32 = 4;
const CONFLICT: u32 = 5;
const BUSY: u32 = 6;
const THROTTLED: u32 = 7;
const INCOMPLETE: u32 = 8;
const EXHAUSTED: u32 = 9;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Config {
    mode: u32,
    pages: u32,
    transaction_ms: u32,
    bytes_per_second: u32,
}
#[derive(Clone, Copy)]
struct Page {
    bytes: [u8; PAGE],
    version: u64,
}
#[cfg(test)]
const EMPTY: Page = Page {
    bytes: [0; PAGE],
    version: 1,
};
#[derive(Clone, Copy)]
struct Transaction {
    token: u64,
    nonce: u64,
    version: u64,
    deadline: u64,
    page: u32,
    client: [u8; 64],
    staged: u8,
    bytes: [u8; PAGE],
}
#[cfg(test)]
const NO_TX: Transaction = Transaction {
    token: 0,
    nonce: 0,
    version: 0,
    deadline: 0,
    page: 0,
    client: [0; 64],
    staged: 0,
    bytes: [0; PAGE],
};
struct State {
    config: Config,
    pages: [Page; PAGES],
    tx: Transaction,
    // A fixed last-completion record permits replay without a second write.
    last: TransactionKey,
    next_token: u64,
    tokens: u64,
    refill_ms: u64,
}
#[derive(Clone, Copy)]
struct TransactionKey {
    token: u64,
    nonce: u64,
    version: u64,
    page: u32,
    client: [u8; 64],
}
#[cfg(test)]
const NO_KEY: TransactionKey = TransactionKey {
    token: 0,
    nonce: 0,
    version: 0,
    page: 0,
    client: [0; 64],
};

pub struct Owner {
    busy: AtomicBool,
    identity: [u8; 64],
    state: UnsafeCell<State>,
}
// The identity is immutable; all access to UnsafeCell is protected by busy.
unsafe impl Sync for Owner {}
struct Guard<'a>(&'a Owner);
impl Drop for Guard<'_> {
    fn drop(&mut self) {
        self.0.busy.store(false, Ordering::Release);
    }
}
impl core::ops::Deref for Guard<'_> {
    type Target = State;
    fn deref(&self) -> &State {
        unsafe { &*self.0.state.get() }
    }
}
impl core::ops::DerefMut for Guard<'_> {
    fn deref_mut(&mut self) -> &mut State {
        unsafe { &mut *self.0.state.get() }
    }
}
fn u32_at(p: &[u8], at: usize) -> u32 {
    u32::from_le_bytes(p[at..at + 4].try_into().unwrap())
}
fn u64_at(p: &[u8], at: usize) -> u64 {
    u64::from_le_bytes(p[at..at + 8].try_into().unwrap())
}
fn put32(p: &mut [u8], at: usize, v: u32) {
    p[at..at + 4].copy_from_slice(&v.to_le_bytes());
}
fn put64(p: &mut [u8], at: usize, v: u64) {
    p[at..at + 8].copy_from_slice(&v.to_le_bytes());
}
fn valid_id(p: &[u8]) -> bool {
    p.len() == 64
        && p[..16].iter().any(|&b| b != 0)
        && p[..16].iter().any(|&b| b != 255)
        && p[16..32].iter().any(|&b| b != 0)
        && p[16..32].iter().any(|&b| b != 255)
        && u64_at(p, 32) != 0
        && u64_at(p, 40) != 0
        && p[48] == 1
        && p[49..].iter().all(|&b| b == 0)
}
impl Owner {
    #[cfg(test)]
    fn new(config: Config, identity: [u8; 64]) -> Self {
        Self {
            busy: AtomicBool::new(false),
            identity,
            state: UnsafeCell::new(State {
                config,
                pages: [EMPTY; PAGES],
                tx: NO_TX,
                last: NO_KEY,
                next_token: 0,
                tokens: config.bytes_per_second as u64,
                refill_ms: 0,
            }),
        }
    }
    fn lock(&self) -> Option<Guard<'_>> {
        self.busy
            .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
            .ok()
            .map(|_| Guard(self))
    }
    fn dispatch(&self, q: &[u8], r: &mut [u8], now: u64) -> usize {
        if q.len() < HEADER || q.len() > MAX || r.len() < MAX {
            return 0;
        }
        r[..HEADER].copy_from_slice(&q[..HEADER]);
        put32(r, 4, INVALID);
        put64(r, 56, 0);
        r[64..128].copy_from_slice(&self.identity);
        let op = q[3];
        let page = u32_at(q, 16) as usize;
        let offset = u32_at(q, 20) as usize;
        let len = u32_at(q, 24) as usize;
        let version = u64_at(q, 32);
        let token = u64_at(q, 40);
        let nonce = u64_at(q, 48);
        let client: [u8; 64] = q[128..192].try_into().unwrap();
        let hello = op == 1;
        let valid = q[..3] == [b'M', b'P', 1]
            && (1..=6).contains(&op)
            && u32_at(q, 4) == 0
            && u64_at(q, 8) != 0
            && u32_at(q, 28) == 0
            && u64_at(q, 56) == 0
            && valid_id(&client)
            && client[..16] == self.identity[..16];
        let shape = match op {
            1 => q[16..64].iter().all(|&b| b == 0) && q[64..128].iter().all(|&b| b == 0),
            2 => len > 0 && len <= CHUNK && offset <= PAGE - len && token == 0 && nonce == 0,
            3 => offset == 0 && len == 0 && version != 0 && token == 0 && nonce != 0,
            4 => {
                len == CHUNK
                    && offset % CHUNK == 0
                    && offset <= PAGE - CHUNK
                    && version != 0
                    && token != 0
                    && nonce != 0
            }
            5 | 6 => offset == 0 && len == 0 && version != 0 && token != 0 && nonce != 0,
            _ => false,
        };
        if !valid || !shape || q.len() != HEADER + if op == 4 { len } else { 0 } {
            return HEADER;
        }
        if !hello && q[64..128] != self.identity {
            put32(r, 4, STALE);
            return HEADER;
        }
        let Some(mut s) = self.lock() else {
            put32(r, 4, BUSY);
            return HEADER;
        };
        if s.config.mode == 0 {
            put32(r, 4, DISABLED);
            return HEADER;
        }
        if hello {
            put32(r, 4, OK);
            put32(r, 16, s.config.mode);
            put32(r, 20, s.config.pages);
            put32(r, 24, s.config.transaction_ms);
            put32(r, 28, s.config.bytes_per_second);
            return HEADER;
        }
        if page >= s.config.pages as usize {
            return HEADER;
        }
        // Expiry never publishes pending bytes, and tokens never wrap/reuse.
        if s.tx.token != 0 && now >= s.tx.deadline {
            s.tx.token = 0;
        }
        if op == 2 {
            let current = s.pages[page].version;
            if version != 0 && version != current {
                put32(r, 4, CONFLICT);
                return HEADER;
            }
            if !s.charge(len, now) {
                put32(r, 4, THROTTLED);
                return HEADER;
            }
            put64(r, 32, current);
            put32(r, 4, OK);
            r[HEADER..HEADER + len].copy_from_slice(&s.pages[page].bytes[offset..offset + len]);
            return HEADER + len;
        }
        if s.config.mode != 2 {
            put32(r, 4, DENIED);
            return HEADER;
        }
        if op == 3 {
            if s.last.token != 0
                && s.last.page as usize == page
                && s.last.nonce == nonce
                && s.last.client == client
            {
                put32(r, 4, STALE);
                return HEADER;
            }
            if s.tx.token != 0 {
                let duplicate = s.tx.page as usize == page
                    && s.tx.client == client
                    && s.tx.nonce == nonce
                    && s.tx.version == version;
                if duplicate {
                    put32(r, 4, OK);
                    put64(r, 40, s.tx.token);
                } else {
                    put32(r, 4, BUSY);
                }
                return HEADER;
            }
            if version != s.pages[page].version {
                put32(r, 4, CONFLICT);
                return HEADER;
            }
            if s.next_token == u64::MAX
                || version == u64::MAX
                || now > u64::MAX - s.config.transaction_ms as u64
            {
                put32(r, 4, EXHAUSTED);
                return HEADER;
            }
            s.next_token += 1;
            s.tx.token = s.next_token;
            s.tx.page = page as u32;
            s.tx.client = client;
            s.tx.nonce = nonce;
            s.tx.version = version;
            s.tx.deadline = now + s.config.transaction_ms as u64;
            s.tx.staged = 0;
            put32(r, 4, OK);
            put64(r, 40, s.tx.token);
            return HEADER;
        }
        if op == 5
            && s.last.token == token
            && s.last.page as usize == page
            && s.last.client == client
            && s.last.nonce == nonce
            && Some(s.last.version) == version.checked_add(1)
        {
            put32(r, 4, OK);
            put64(r, 32, s.last.version);
            return HEADER;
        }
        if s.tx.token == 0
            || s.tx.token != token
            || s.tx.page as usize != page
            || s.tx.client != client
            || s.tx.nonce != nonce
            || s.tx.version != version
        {
            put32(r, 4, STALE);
            return HEADER;
        }
        match op {
            4 => {
                if !s.charge(len, now) {
                    put32(r, 4, THROTTLED);
                    return HEADER;
                }
                let bit = 1 << (offset / CHUNK);
                // A retried stage must carry identical bytes. Never silently
                // change already acknowledged data under the same token.
                if s.tx.staged & bit != 0 && s.tx.bytes[offset..offset + len] != q[HEADER..] {
                    put32(r, 4, CONFLICT);
                    return HEADER;
                }
                s.tx.bytes[offset..offset + len].copy_from_slice(&q[HEADER..]);
                s.tx.staged |= bit;
            }
            5 => {
                if s.tx.staged != 255 {
                    put32(r, 4, INCOMPLETE);
                    return HEADER;
                }
                let bytes = s.tx.bytes;
                s.pages[page].bytes = bytes;
                s.pages[page].version += 1;
                s.last = TransactionKey {
                    token,
                    nonce,
                    version: version + 1,
                    page: page as u32,
                    client,
                };
                s.tx.token = 0;
                put64(r, 32, version + 1);
            }
            6 => {
                s.tx.token = 0;
            }
            _ => return HEADER,
        }
        put32(r, 4, OK);
        HEADER
    }
}
impl State {
    fn charge(&mut self, bytes: usize, now: u64) -> bool {
        let rate = self.config.bytes_per_second as u64;
        if rate == 0 {
            return true;
        }
        // Preserve fractional refill time: advance by whole milliseconds
        // only when at least one byte can be credited. Backwards time never
        // creates quota. One-second burst is bounded by the configured rate.
        if self.refill_ms == 0 {
            self.refill_ms = now;
        }
        if now >= self.refill_ms {
            let delta = now - self.refill_ms;
            let credit = delta.saturating_mul(rate) / 1000;
            if credit != 0 {
                self.tokens = self.tokens.saturating_add(credit).min(rate);
                self.refill_ms = now;
            }
        }
        if self.tokens < bytes as u64 {
            return false;
        }
        self.tokens -= bytes as u64;
        true
    }
}

/// Initialize unpublished, 8-byte aligned C-owned storage exactly once.
/// Reinitialization while any caller can access it is forbidden.
#[no_mangle]
pub unsafe extern "C" fn a20_memory_owner_init(
    storage: *mut u8,
    size: u32,
    config: *const Config,
    identity: *const u8,
) -> i32 {
    if storage.is_null()
        || storage as usize % core::mem::align_of::<Owner>() != 0
        || (size as usize) < core::mem::size_of::<Owner>()
        || config.is_null()
        || identity.is_null()
    {
        return -1;
    }
    let c = *config;
    let id: [u8; 64] = core::slice::from_raw_parts(identity, 64)
        .try_into()
        .unwrap();
    if c.mode > 2
        || c.pages == 0
        || c.pages as usize > PAGES
        || c.transaction_ms == 0
        || c.transaction_ms > 30000
        || (c.bytes_per_second != 0 && c.bytes_per_second < CHUNK as u32)
        || !valid_id(&id)
    {
        return -1;
    }
    // Initialize in place: a constrained native process must not need a
    // second MAX_PAGES-sized owner on its user stack merely to start.
    core::ptr::write_bytes(storage, 0, core::mem::size_of::<Owner>());
    let owner = storage as *mut Owner;
    core::ptr::addr_of_mut!((*owner).identity).write(id);
    let s = (*owner).state.get();
    core::ptr::addr_of_mut!((*s).config).write(c);
    core::ptr::addr_of_mut!((*s).tokens).write(c.bytes_per_second as u64);
    for i in 0..PAGES {
        core::ptr::addr_of_mut!((*s).pages[i].version).write(1);
    }
    0
}
#[no_mangle]
pub unsafe extern "C" fn a20_memory_owner_seed(
    storage: *const Owner,
    page: u32,
    bytes: *const u8,
    len: u32,
) -> i32 {
    if storage.is_null() || bytes.is_null() || len as usize > PAGE {
        return -1;
    }
    let Some(mut s) = (*storage).lock() else {
        return -1;
    };
    if page >= s.config.pages || s.next_token != 0 {
        return -1;
    }
    let p = &mut s.pages[page as usize];
    p.bytes.fill(0);
    p.bytes[..len as usize].copy_from_slice(core::slice::from_raw_parts(bytes, len as usize));
    0
}
#[no_mangle]
pub unsafe extern "C" fn a20_memory_owner_dispatch(
    storage: *const Owner,
    request: *const u8,
    len: u32,
    reply: *mut u8,
    capacity: u32,
    now_ms: u64,
) -> i32 {
    if storage.is_null()
        || request.is_null()
        || reply.is_null()
        || len as usize > MAX
        || (len as usize) < HEADER
        || (capacity as usize) < MAX
    {
        return -1;
    }
    // Copy first so the wire buffers may alias each other (not the owner).
    let mut q = [0u8; MAX];
    core::ptr::copy_nonoverlapping(request, q.as_mut_ptr(), len as usize);
    (*storage).dispatch(
        &q[..len as usize],
        core::slice::from_raw_parts_mut(reply, MAX),
        now_ms,
    ) as i32
}

#[cfg(not(test))]
#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
    unsafe extern "C" {
        fn a20_memory_panic() -> !;
    }
    unsafe { a20_memory_panic() }
}

#[cfg(test)]
#[path = "cluster_memory_tests.rs"]
mod tests;
