#![no_std]

//! Foreign-architecture translator configuration.
//!
//! Rust owns the configuration of the translation channel: the cmdline key
//! grammar (`a20.xlator=0|1`, `a20.xlator.<guest>[.argv|.env]=<value>`), the
//! per-guest slot table, the boot pass that resolves registry defaults and
//! normalizes templates, the runtime lookup the exec hook performs, the
//! `/proc/a20/xlator` rendering, the enable switch and the forward counter.
//!
//! C keeps the guest registry (the X-macro table, reached through accessors),
//! the boot args source, the kernel log call and the `/proc` file plumbing.

#[cfg(test)]
extern crate std;

include!("xlator_tmpl_types.rs");

pub const MAX_PATH_LEN: usize = 512;
pub const MAX_GUESTS: usize = 8;

const ELF_ABI_LINUX: u8 = 0;
const ELF_ABI_NATIVE: u8 = 1;

const EINVAL: i32 = -22;
const ENOENT: i32 = -2;

const ENABLE_KEY: &[u8] = b"a20.xlator";
const GUEST_KEY: &[u8] = b"a20.xlator.";

const FIELD_PATH: i32 = 0;
const FIELD_ARGV: i32 = 1;
const FIELD_ENV: i32 = 2;

const LOG_INFO: i32 = 0;
const LOG_WARN: i32 = 1;

extern "C" {
    fn a20_xr_count() -> i32;
    fn a20_xr_name(index: i32) -> *const u8;
    fn a20_xr_machine(index: i32) -> u16;
    fn a20_xr_abi(index: i32) -> u8;
    fn a20_xr_argv(index: i32) -> *const u8;
    fn a20_xlator_log(level: i32, msg: *const u8);
    fn a20_xlator_parse_template(tmpl: *const u8, out: *mut XlatorTmpl) -> i32;
    fn a20_xlator_split_env(scratch: *mut u8, entries: *mut *mut u8, max: i32) -> i32;
}

/// The C `xlator_desc_t`: the three strings the exec hook borrows.
#[repr(C)]
pub struct XlatorDesc {
    pub path: *const u8,
    pub argv: *const u8,
    pub env: *const u8,
}

struct Slot {
    path: [u8; MAX_PATH_LEN],
    argv: [u8; XLATOR_TMPL_LEN],
    env: [u8; XLATOR_ENV_LEN],
    path_set: bool,
    argv_valid: bool,
    env_valid: bool,
    argv_overridden: bool,
    env_overridden: bool,
}

impl Slot {
    const fn new() -> Self {
        Slot {
            path: [0; MAX_PATH_LEN],
            argv: [0; XLATOR_TMPL_LEN],
            env: [0; XLATOR_ENV_LEN],
            path_set: false,
            argv_valid: false,
            env_valid: false,
            argv_overridden: false,
            env_overridden: false,
        }
    }

    fn usable(&self) -> bool {
        self.path_set && self.argv_valid && self.env_valid
    }
}

static mut SLOTS: [Slot; MAX_GUESTS] = [Slot::new(), Slot::new(), Slot::new(), Slot::new(),
                                        Slot::new(), Slot::new(), Slot::new(), Slot::new()];
static ENABLED: core::sync::atomic::AtomicI32 = core::sync::atomic::AtomicI32::new(0);
// unsigned long in the C reference: 32 bits on ILP32, and a forward count
// never approaches 2^32.
static FORWARDS: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(0);

fn slots() -> &'static mut [Slot; MAX_GUESTS] {
    unsafe { &mut *core::ptr::addr_of_mut!(SLOTS) }
}

fn guest_count() -> usize {
    let n = unsafe { a20_xr_count() };
    if n <= 0 { 0 } else { n as usize }
}

fn guest_name(i: usize) -> &'static [u8] {
    c_str(unsafe { a20_xr_name(i as i32) })
}

fn registry_argv(i: usize) -> &'static [u8] {
    c_str(unsafe { a20_xr_argv(i as i32) })
}

/// Borrow a NUL-terminated kernel string.  Every string here is a registry
/// literal or a slot buffer that is NUL-terminated by construction.
fn c_str(p: *const u8) -> &'static [u8] {
    if p.is_null() {
        return &[];
    }
    let mut n = 0usize;
    while unsafe { *p.add(n) } != 0 {
        n += 1;
    }
    unsafe { core::slice::from_raw_parts(p, n) }
}

fn log(level: i32, msg: &[u8]) {
    let mut buf = [0u8; 512];
    let n = if msg.len() < buf.len() { msg.len() } else { buf.len() - 1 };
    buf[..n].copy_from_slice(&msg[..n]);
    buf[n] = 0;
    unsafe { a20_xlator_log(level, buf.as_ptr()) };
}

struct Out {
    buf: [u8; 512],
    len: usize,
}

impl Out {
    fn new() -> Self {
        Out { buf: [0; 512], len: 0 }
    }

    fn s(&mut self, bytes: &[u8]) {
        let room = self.buf.len() - 1 - self.len;
        let n = if bytes.len() < room { bytes.len() } else { room };
        self.buf[self.len..self.len + n].copy_from_slice(&bytes[..n]);
        self.len += n;
    }

    fn i32(&mut self, v: i32) {
        let mut digits = [0u8; 11];
        let mut n = 0;
        let mut mag: u32;
        if v < 0 {
            self.s(b"-");
            mag = (-(v as i64)) as u32;
        } else {
            mag = v as u32;
        }
        loop {
            digits[n] = b'0' + (mag % 10) as u8;
            n += 1;
            mag /= 10;
            if mag == 0 {
                break;
            }
        }
        while n > 0 {
            n -= 1;
            self.buf[self.len] = digits[n];
            self.len += 1;
        }
    }

    fn u32(&mut self, v: u32) {
        self.i32(v as i32);
    }

    fn as_bytes(&self) -> &[u8] {
        &self.buf[..self.len]
    }
}

fn abi_name(abi: u8) -> &'static [u8] {
    if abi == ELF_ABI_NATIVE { b"native" } else { b"linux" }
}

fn abi_parse(bytes: &[u8]) -> Option<u8> {
    if bytes == b"linux" {
        Some(ELF_ABI_LINUX)
    } else if bytes == b"native" {
        Some(ELF_ABI_NATIVE)
    } else {
        None
    }
}

fn valid_guest_name(name: &[u8]) -> bool {
    if name.is_empty() {
        return false;
    }
    name.iter().all(|&c| {
        (c >= b'a' && c <= b'z') || (c >= b'A' && c <= b'Z') || (c >= b'0' && c <= b'9')
            || c == b'_'
    })
}

/// Find the registry row for a (name, abi) pair.
fn slot_for(name: &[u8], abi: u8) -> Option<usize> {
    for i in 0..guest_count() {
        if guest_name(i) == name && unsafe { a20_xr_abi(i as i32) } == abi {
            return Some(i);
        }
    }
    None
}

/// Split a guest selector into a name and an ABI.  The bare form means Linux
/// ABI; `.<abi>` names the other one.
fn selector_of(sel: &[u8]) -> Option<(&[u8], u8)> {
    match sel.iter().rposition(|&c| c == b'.') {
        Some(dot) => {
            let name = &sel[..dot];
            let abi_bytes = &sel[dot + 1..];
            if abi_bytes.is_empty() || name.is_empty() {
                return None;
            }
            let abi = abi_parse(abi_bytes)?;
            Some((name, abi))
        }
        None => {
            if !valid_guest_name(sel) {
                return None;
            }
            Some((sel, ELF_ABI_LINUX))
        }
    }
}

/// Which of the three per-guest keys a selector names.
fn field_of(key: &[u8]) -> (i32, &[u8]) {
    if let Some(dot) = key.iter().rposition(|&c| c == b'.') {
        if dot != 0 {
            let tail = &key[dot + 1..];
            if tail == b"argv" {
                return (FIELD_ARGV, &key[..dot]);
            }
            if tail == b"env" {
                return (FIELD_ENV, &key[..dot]);
            }
        }
    }
    (FIELD_PATH, key)
}

/// Split `<prefix><key>=<value>`: returns (key, value) when the token carries
/// the prefix and an '='.
fn value_of<'a>(tok: &'a [u8], prefix: &[u8]) -> Option<(&'a [u8], &'a [u8])> {
    if tok.len() <= prefix.len() || !tok.starts_with(prefix) {
        return None;
    }
    let rest = &tok[prefix.len()..];
    let eq = rest.iter().position(|&c| c == b'=')?;
    Some((&rest[..eq], &rest[eq + 1..]))
}

fn parse_enable(val: &[u8]) {
    if val == b"1" {
        set_enabled(1);
    } else if val == b"0" {
        set_enabled(0);
    } else {
        let mut o = Out::new();
        o.s("[XLATOR] 未知 ".as_bytes());
        o.s(ENABLE_KEY);
        o.s(b"='");
        o.s(val);
        o.s(b"'");
        o.s("，保持禁用\n".as_bytes());
        log(LOG_WARN, o.as_bytes());
    }
}

fn parse_guest_key(key: &[u8], val: &[u8]) {
    if key.is_empty() {
        let mut o = Out::new();
        o.s("[XLATOR] ".as_bytes());
        o.s(GUEST_KEY);
        o.s("=... 缺少 guest 名，忽略\n".as_bytes());
        log(LOG_WARN, o.as_bytes());
        return;
    }

    let (field, sel) = field_of(key);
    let (name, abi) = match selector_of(sel) {
        Some(pair) => pair,
        None => {
            let mut o = Out::new();
            match sel.iter().position(|&c| c == b'.') {
                None => {
                    o.s("[XLATOR] 未知外来架构 '".as_bytes());
                    o.s(sel);
                    o.s("'，忽略（见 proc/xlator_guests.def）\n".as_bytes());
                }
                Some(dot) => {
                    let head = &sel[..dot];
                    let tail = &sel[dot + 1..];
                    if tail.is_empty() {
                        o.s("[XLATOR] ".as_bytes());
                        o.s(GUEST_KEY);
                        o.s(head);
                        o.s(".<abi> 缺少 ABI 名（linux 或 native），忽略\n".as_bytes());
                    } else if !valid_guest_name(head) {
                        o.s("[XLATOR] 未知外来架构 '".as_bytes());
                        o.s(head);
                        o.s("'，忽略（见 proc/xlator_guests.def）\n".as_bytes());
                    } else {
                        o.s("[XLATOR] 未知 ABI '".as_bytes());
                        o.s(tail);
                        o.s("'，".as_bytes());
                        o.s(GUEST_KEY);
                        o.s(head);
                        o.s(".<abi> 忽略（当前可用 linux / native）\n".as_bytes());
                    }
                }
            }
            log(LOG_WARN, o.as_bytes());
            return;
        }
    };

    let slot = match slot_for(name, abi) {
        Some(s) => s,
        None => {
            let mut o = Out::new();
            let other = if abi == ELF_ABI_LINUX { ELF_ABI_NATIVE } else { ELF_ABI_LINUX };
            if slot_for(name, other).is_some() {
                o.s("[XLATOR] '".as_bytes());
                o.s(name);
                o.s("' 未注册 ".as_bytes());
                o.s(abi_name(abi));
                o.s(" ABI 的翻译器，忽略（见 proc/xlator_guests.def）\n".as_bytes());
            } else {
                o.s("[XLATOR] 未知外来架构 '".as_bytes());
                o.s(name);
                o.s("'，忽略（见 proc/xlator_guests.def）\n".as_bytes());
            }
            log(LOG_WARN, o.as_bytes());
            return;
        }
    };

    let table = slots();
    if field == FIELD_PATH {
        if val.is_empty() || val[0] != b'/' {
            let mut o = Out::new();
            o.s("[XLATOR] ".as_bytes());
            o.s(GUEST_KEY);
            o.s(guest_name(slot));
            o.s(b".");
            o.s(abi_name(unsafe { a20_xr_abi(slot as i32) }));
            o.s(" 不是绝对路径，忽略\n".as_bytes());
            log(LOG_WARN, o.as_bytes());
            return;
        }
        let n = copy_into(&mut table[slot].path, val);
        let _ = n;
        table[slot].path_set = true;
        return;
    }

    if field == FIELD_ARGV {
        let mut tmpl: XlatorTmpl = unsafe { core::mem::zeroed() };
        let mut c = [0u8; XLATOR_TMPL_LEN];
        let n = if val.len() < c.len() - 1 { val.len() } else { c.len() - 1 };
        c[..n].copy_from_slice(&val[..n]);
        let rc = unsafe { a20_xlator_parse_template(c.as_ptr(), &mut tmpl) };
        if rc < 0 {
            let mut o = Out::new();
            o.s("[XLATOR] ".as_bytes());
            o.s(GUEST_KEY);
            o.s(guest_name(slot));
            o.s(b".");
            o.s(abi_name(unsafe { a20_xr_abi(slot as i32) }));
            o.s(".argv='".as_bytes());
            o.s(val);
            o.s("' 非法（".as_bytes());
            o.s(if rc == -7 { "过长或 token 过多" } else { "未知 @ 记号或缺少 @P" }.as_bytes());
            o.s("），该 guest 不会被转发\n".as_bytes());
            log(LOG_WARN, o.as_bytes());
            table[slot].argv_valid = false;
            table[slot].argv_overridden = true;
            return;
        }
        copy_into(&mut table[slot].argv, val);
        table[slot].argv_valid = true;
        table[slot].argv_overridden = true;
        return;
    }

    // FIELD_ENV
    if val.len() >= XLATOR_ENV_LEN {
        let mut o = Out::new();
        o.s("[XLATOR] ".as_bytes());
        o.s(GUEST_KEY);
        o.s(guest_name(slot));
        o.s(b".");
        o.s(abi_name(unsafe { a20_xr_abi(slot as i32) }));
        o.s(".env 过长（上限 ".as_bytes());
        o.i32((XLATOR_ENV_LEN - 1) as i32);
        o.s(" 字节），该 guest 不会被转发\n".as_bytes());
        log(LOG_WARN, o.as_bytes());
        table[slot].env_valid = false;
        return;
    }
    let mut scratch = [0u8; XLATOR_ENV_LEN];
    let n = copy_into(&mut scratch, val);
    let mut entries = [core::ptr::null_mut::<u8>(); XLATOR_ENV_MAX];
    let rc = unsafe {
        a20_xlator_split_env(scratch.as_mut_ptr(), entries.as_mut_ptr(), XLATOR_ENV_MAX as i32)
    };
    let _ = n;
    if rc < 0 {
        let mut o = Out::new();
        o.s("[XLATOR] ".as_bytes());
        o.s(GUEST_KEY);
        o.s(guest_name(slot));
        o.s(b".");
        o.s(abi_name(unsafe { a20_xr_abi(slot as i32) }));
        o.s(".env='".as_bytes());
        o.s(val);
        o.s("' 非法（".as_bytes());
        o.s(if rc == -7 { "条目过多" } else { "缺少 '=' 或名称为空" }.as_bytes());
        o.s("），该 guest 不会被转发\n".as_bytes());
        log(LOG_WARN, o.as_bytes());
        table[slot].env_valid = false;
        table[slot].env_overridden = true;
        return;
    }
    // Normalise the separators to commas so /proc renders one stable form and
    // the exec path sees the same text the administrator wrote.
    let mut off = 0usize;
    for i in 0..rc as usize {
        let entry = c_str(entries[i]);
        if i != 0 {
            table[slot].env[off] = b',';
            off += 1;
        }
        off += entry.len();
        table[slot].env[off - entry.len()..off].copy_from_slice(entry);
    }
    table[slot].env[off] = 0;
    table[slot].env_valid = true;
    table[slot].env_overridden = true;
}

fn copy_into(dst: &mut [u8], src: &[u8]) -> usize {
    let n = if src.len() < dst.len() - 1 { src.len() } else { dst.len() - 1 };
    dst[..n].copy_from_slice(&src[..n]);
    dst[n] = 0;
    n
}

/// Parse the whole cmdline and finalize the configuration.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_xlator_config_init(cmdline: *const u8) {
    let line = c_str(cmdline);
    let mut p = 0usize;
    while p < line.len() {
        while p < line.len() && (line[p] == b' ' || line[p] == b'\t') {
            p += 1;
        }
        if p >= line.len() {
            break;
        }
        let start = p;
        while p < line.len() && line[p] != b' ' && line[p] != b'\t' {
            p += 1;
        }
        let tok = &line[start..p];
        if let Some(val) = enable_value(tok) {
            parse_enable(val);
        } else if let Some((key, val)) = value_of(tok, GUEST_KEY) {
            parse_guest_key(key, val);
        }
    }

    finalize();

    if !enabled() {
        log(LOG_INFO, "[XLATOR] 外来架构翻译通道: 禁用\n".as_bytes());
        return;
    }

    let table = slots();
    let mut nconfigured = 0;
    for i in 0..guest_count() {
        if !table[i].usable() {
            continue;
        }
        nconfigured += 1;
        let mut o = Out::new();
        o.s("[XLATOR] ".as_bytes());
        o.s(guest_name(i));
        o.s(" (e_machine=".as_bytes());
        o.u32(unsafe { a20_xr_machine(i as i32) } as u32);
        o.s(" abi=".as_bytes());
        o.s(abi_name(unsafe { a20_xr_abi(i as i32) }));
        o.s(") → ".as_bytes());
        o.s(c_str(table[i].path.as_ptr()));
        o.s("  argv=\"".as_bytes());
        o.s(c_str(table[i].argv.as_ptr()));
        o.s(b"\"");
        if table[i].argv_overridden {
            o.s(" (cmdline 覆盖)".as_bytes());
        }
        if table[i].env[0] != 0 {
            o.s("  env=\"".as_bytes());
            o.s(c_str(table[i].env.as_ptr()));
            o.s(b"\"");
        }
        o.s(b"\n");
        log(LOG_INFO, o.as_bytes());
    }

    if nconfigured == 0 {
        log(LOG_WARN,
            "[XLATOR] 已启用但未配置任何翻译器路径，execve 外来二进制仍将返回 -ENOEXEC\n"
                .as_bytes());
        return;
    }

    for i in 0..guest_count() {
        let mut o = Out::new();
        if !table[i].path_set {
            o.s("[XLATOR] ".as_bytes());
            o.s(guest_name(i));
            o.s(" (e_machine=".as_bytes());
            o.u32(unsafe { a20_xr_machine(i as i32) } as u32);
            o.s(" abi=".as_bytes());
            o.s(abi_name(unsafe { a20_xr_abi(i as i32) }));
            o.s(") 未配置翻译器，execve 将返回 -ENOEXEC\n".as_bytes());
            log(LOG_WARN, o.as_bytes());
            continue;
        }
        if !table[i].argv_valid {
            o.s("[XLATOR] ".as_bytes());
            o.s(guest_name(i));
            o.s(b".");
            o.s(abi_name(unsafe { a20_xr_abi(i as i32) }));
            o.s(" argv 模板非法，execve 将返回 -ENOEXEC\n".as_bytes());
            log(LOG_WARN, o.as_bytes());
        }
        if !table[i].env_valid {
            let mut o = Out::new();
            o.s("[XLATOR] ".as_bytes());
            o.s(guest_name(i));
            o.s(b".");
            o.s(abi_name(unsafe { a20_xr_abi(i as i32) }));
            o.s(" env 规格非法，execve 将返回 -ENOEXEC\n".as_bytes());
            log(LOG_WARN, o.as_bytes());
        }
    }
}

fn enable_value(tok: &[u8]) -> Option<&[u8]> {
    if tok.len() <= ENABLE_KEY.len() || !tok.starts_with(ENABLE_KEY) {
        return None;
    }
    if tok[ENABLE_KEY.len()] != b'=' {
        return None;
    }
    Some(&tok[ENABLE_KEY.len() + 1..])
}

/// Resolve registry defaults, then validate and normalize every slot's
/// template.  Runs after the whole cmdline so key order cannot matter.
fn finalize() {
    let table = slots();
    for i in 0..guest_count() {
        if !table[i].argv_overridden {
            copy_into(&mut table[i].argv, registry_argv(i));
            table[i].argv_valid = true;
        }
        // An unset env means "no environment to add", which is valid; it is
        // not the same as an .env key that failed to parse.
        if !table[i].env_overridden {
            table[i].env_valid = true;
        }
        if table[i].argv_valid {
            let mut tmpl: XlatorTmpl = unsafe { core::mem::zeroed() };
            if unsafe { a20_xlator_parse_template(table[i].argv.as_ptr(), &mut tmpl) } < 0 {
                table[i].argv_valid = false;
            } else {
                let mut norm = [0u8; XLATOR_TMPL_LEN];
                let mut off = 0usize;
                for t in 0..tmpl.n as usize {
                    let (piece, plen): (&[u8], usize) = match tmpl.kind[t] {
                        XLATOR_TOK_LITERAL => {
                            let s = tmpl.start[t] as usize;
                            let l = tmpl.len[t] as usize;
                            (&tmpl.text[s..s + l], l)
                        }
                        XLATOR_TOK_ARGV0 => (b"@A", 2),
                        XLATOR_TOK_PATH => (b"@P", 2),
                        _ => (b"@*", 2),
                    };
                    if off + plen + 2 >= XLATOR_TMPL_LEN {
                        break;
                    }
                    if off != 0 {
                        norm[off] = b' ';
                        off += 1;
                    }
                    norm[off..off + plen].copy_from_slice(piece);
                    off += plen;
                }
                norm[off] = 0;
                table[i].argv = norm;
            }
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_xlator_enabled() -> i32 {
    ENABLED.load(core::sync::atomic::Ordering::Acquire)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_xlator_set_enabled(on: i32) {
    ENABLED.store(if on != 0 { 1 } else { 0 }, core::sync::atomic::Ordering::Release);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_xlator_note_forward() {
    FORWARDS.fetch_add(1, core::sync::atomic::Ordering::Relaxed);
}

fn set_enabled(v: i32) {
    unsafe { a20_xlator_set_enabled(v) };
}

fn enabled() -> bool {
    let v = unsafe { a20_xlator_enabled() };
    v != 0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_xlator_abi_name(abi: u8) -> *const u8 {
    if abi == ELF_ABI_NATIVE { b"native\0".as_ptr() } else { b"linux\0".as_ptr() }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_xlator_abi_parse(s: *const u8, len: usize, out: *mut u8) -> i32 {
    if s.is_null() || out.is_null() {
        return EINVAL;
    }
    let bytes = unsafe { core::slice::from_raw_parts(s, len) };
    match abi_parse(bytes) {
        Some(abi) => {
            unsafe { *out = abi };
            0
        }
        None => EINVAL,
    }
}

/// Resolve the translator for a guest (e_machine, ABI) pair.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_xlator_lookup(machine: u16, abi: u8,
                                           out: *mut XlatorDesc) -> i32 {
    if out.is_null() || !enabled() {
        return ENOENT;
    }
    let table = slots();
    for i in 0..guest_count() {
        if unsafe { a20_xr_machine(i as i32) } != machine
            || unsafe { a20_xr_abi(i as i32) } != abi
        {
            continue;
        }
        if !table[i].usable() {
            return ENOENT;
        }
        unsafe {
            (*out).path = table[i].path.as_ptr();
            (*out).argv = table[i].argv.as_ptr();
            (*out).env = table[i].env.as_ptr();
        }
        return 0;
    }
    ENOENT
}

/// Render `/proc/a20/xlator`, mirroring the C format exactly.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_xlator_render(buf: *mut u8, bufsz: usize) -> i32 {
    if buf.is_null() || bufsz == 0 {
        return EINVAL;
    }
    let out = unsafe { core::slice::from_raw_parts_mut(buf, bufsz) };
    let mut pos = 0usize;
    let trunc = bufsz as i32 - 1;

    if !put(out, &mut pos, b"enabled: ") {
        return trunc;
    }
    if !put_i32(out, &mut pos, if enabled() { 1 } else { 0 }) {
        return trunc;
    }
    if !put(out, &mut pos, b"\nforwards: ") {
        return trunc;
    }
    if !put_u32(out, &mut pos, FORWARDS.load(core::sync::atomic::Ordering::Relaxed)) {
        return trunc;
    }
    if !put(out, &mut pos, b"\nregistered: ") {
        return trunc;
    }
    if !put_i32(out, &mut pos, guest_count() as i32) {
        return trunc;
    }
    if !put(out, &mut pos, b"\n") {
        return trunc;
    }

    let table = slots();
    for i in 0..guest_count() {
        let usable = table[i].usable();
        if !put(out, &mut pos, b"guest ") { return trunc; }
        if !put(out, &mut pos, guest_name(i)) { return trunc; }
        if !put(out, &mut pos, b": machine=") { return trunc; }
        if !put_u32(out, &mut pos, unsafe { a20_xr_machine(i as i32) } as u32) { return trunc; }
        if !put(out, &mut pos, b" abi=") { return trunc; }
        if !put(out, &mut pos, abi_name(unsafe { a20_xr_abi(i as i32) })) { return trunc; }
        if !put(out, &mut pos, b" path=") { return trunc; }
        if !put(out, &mut pos, if usable { c_str(table[i].path.as_ptr()) } else { b"(none)" }) {
            return trunc;
        }
        if !put(out, &mut pos, b" argv=\"") { return trunc; }
        if !put(out, &mut pos, if usable { c_str(table[i].argv.as_ptr()) } else { b"(unset)" }) {
            return trunc;
        }
        if !put(out, &mut pos, b"\"") { return trunc; }
        if table[i].argv_overridden && !put(out, &mut pos, b" (override)") {
            return trunc;
        }
        if !put(out, &mut pos, b" env=") { return trunc; }
        if !put(out, &mut pos, if usable && table[i].env[0] != 0 {
            c_str(table[i].env.as_ptr())
        } else {
            b"(none)"
        }) {
            return trunc;
        }
        if !put(out, &mut pos, b"\n") { return trunc; }
    }
    pos as i32
}

fn put(out: &mut [u8], pos: &mut usize, bytes: &[u8]) -> bool {
    let room = out.len() - 1 - *pos;
    let n = bytes.len();
    if n > room {
        return false;
    }
    out[*pos..*pos + n].copy_from_slice(bytes);
    *pos += n;
    true
}

fn put_i32(out: &mut [u8], pos: &mut usize, v: i32) -> bool {
    let mut digits = [0u8; 11];
    let mut n = 0;
    let mut mag: u32;
    if v < 0 {
        if !put(out, pos, b"-") {
            return false;
        }
        mag = (-(v as i64)) as u32;
    } else {
        mag = v as u32;
    }
    loop {
        digits[n] = b'0' + (mag % 10) as u8;
        n += 1;
        mag /= 10;
        if mag == 0 {
            break;
        }
    }
    while n > 0 {
        n -= 1;
        if !put(out, pos, &digits[n..n + 1]) {
            return false;
        }
    }
    true
}

fn put_u32(out: &mut [u8], pos: &mut usize, v: u32) -> bool {
    put_i32(out, pos, v as i32)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::format;
    use std::string::String;
    use std::sync::Mutex;
    use std::vec::Vec;

    static TEST_LOCK: Mutex<()> = Mutex::new(());

    struct Guest {
        name: &'static [u8],
        machine: u16,
        abi: u8,
        argv: &'static [u8],
    }

    static GUESTS: [Guest; 2] = [
        Guest { name: b"x86_64\0", machine: 62, abi: ELF_ABI_LINUX, argv: b"-0 @A @P @*\0" },
        Guest { name: b"aarch64\0", machine: 183, abi: ELF_ABI_LINUX, argv: b"-0 @A @P @*\0" },
    ];

    static mut LOGS: Vec<(i32, String)> = Vec::new();

    fn reset() -> std::sync::MutexGuard<'static, ()> {
        let guard = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        unsafe {
            LOGS.clear();
            SLOTS = [Slot::new(), Slot::new(), Slot::new(), Slot::new(),
                     Slot::new(), Slot::new(), Slot::new(), Slot::new()];
        }
        ENABLED.store(0, core::sync::atomic::Ordering::Release);
        FORWARDS.store(0, core::sync::atomic::Ordering::Relaxed);
        guard
    }

    fn config(line: &str) {
        let mut c = line.as_bytes().to_vec();
        c.push(0);
        unsafe { a20_xlator_config_init(c.as_ptr()) };
    }

    fn logs() -> &'static [(i32, String)] {
        unsafe { &*(core::ptr::addr_of!(LOGS)) }
    }

    fn warnings() -> Vec<String> {
        logs().iter().filter(|(l, _)| *l != 0).map(|(_, m)| m.clone()).collect()
    }

    fn infos() -> Vec<String> {
        logs().iter().filter(|(l, _)| *l == 0).map(|(_, m)| m.clone()).collect()
    }

    fn render() -> String {
        let mut buf = [0u8; 1024];
        let rc = unsafe { a20_xlator_render(buf.as_mut_ptr(), buf.len()) };
        assert!(rc >= 0);
        String::from_utf8_lossy(&buf[..rc as usize]).into_owned()
    }

    #[no_mangle]
    unsafe extern "C" fn a20_xr_count() -> i32 { GUESTS.len() as i32 }
    #[no_mangle]
    unsafe extern "C" fn a20_xr_name(i: i32) -> *const u8 { GUESTS[i as usize].name.as_ptr() }
    #[no_mangle]
    unsafe extern "C" fn a20_xr_machine(i: i32) -> u16 { GUESTS[i as usize].machine }
    #[no_mangle]
    unsafe extern "C" fn a20_xr_abi(i: i32) -> u8 { GUESTS[i as usize].abi }
    #[no_mangle]
    unsafe extern "C" fn a20_xr_argv(i: i32) -> *const u8 { GUESTS[i as usize].argv.as_ptr() }
    #[no_mangle]
    unsafe extern "C" fn a20_xlator_log(level: i32, msg: *const u8) {
        let mut n = 0usize;
        while unsafe { *msg.add(n) } != 0 { n += 1; }
        let bytes = unsafe { core::slice::from_raw_parts(msg, n) };
        unsafe { (*core::ptr::addr_of_mut!(LOGS)).push((level, String::from_utf8_lossy(bytes).into_owned())) };
    }

    unsafe fn lookup(machine: u16, abi: u8) -> (i32, XlatorDesc) {
        let mut out = XlatorDesc { path: core::ptr::null(), argv: core::ptr::null(), env: core::ptr::null() };
        let rc = unsafe { a20_xlator_lookup(machine, abi, &mut out) };
        (rc, out)
    }

    #[test]
    fn disabled_channel_logs_the_switch_and_renders_no_usable_guest() {
        let _g = reset();
        config("a20.xlator=1");
        ENABLED.store(0, core::sync::atomic::Ordering::Release);
        config("a20.xlator=0 a20.xlator.x86_64=/bin/qemu-x86_64");
        assert_eq!(infos().last().unwrap(), "[XLATOR] 外来架构翻译通道: 禁用\n");
        let text = render();
        assert!(text.starts_with("enabled: 0\nforwards: 0\nregistered: 2\n"));
        assert!(text.contains("guest x86_64: machine=62 abi=linux path=/bin/qemu-x86_64"));
        assert!(text.contains("argv=\"-0 @A @P @*\" env=(none)"));
    }

    #[test]
    fn an_invalid_enable_value_is_reported_and_keeps_the_switch_off() {
        let _g = reset();
        config("a20.xlator=maybe");
        assert_eq!(warnings()[0], "[XLATOR] 未知 a20.xlator='maybe'，保持禁用\n");
        assert_eq!(unsafe { a20_xlator_enabled() }, 0);
    }

    #[test]
    fn a_configured_guest_is_logged_and_a_missing_one_is_named() {
        let _g = reset();
        config("a20.xlator=1 a20.xlator.x86_64=/bin/qemu-x86_64");
        let infos = infos();
        assert!(infos.iter().any(|m| m == "[XLATOR] x86_64 (e_machine=62 abi=linux) → /bin/qemu-x86_64  argv=\"-0 @A @P @*\"\n"));
        assert!(warnings().iter().any(|m| m == "[XLATOR] aarch64 (e_machine=183 abi=linux) 未配置翻译器，execve 将返回 -ENOEXEC\n"));
    }

    #[test]
    fn enabled_with_no_path_warns_loudly() {
        let _g = reset();
        config("a20.xlator=1");
        assert!(warnings().iter().any(|m| m.contains("已启用但未配置任何翻译器路径")));
        // Nothing is configured, so the per-guest list is not printed.
        assert!(!warnings().iter().any(|m| m.contains("未配置翻译器，execve")));
    }

    #[test]
    fn a_partly_configured_registry_names_the_rest() {
        let _g = reset();
        config("a20.xlator=1 a20.xlator.x86_64=/bin/qemu-x86_64");
        assert!(warnings().iter().any(|m| m.contains("x86_64 (e_machine=62 abi=linux) 未配置翻译器")) == false);
        assert!(warnings().iter().any(|m| m.contains("aarch64 (e_machine=183 abi=linux) 未配置翻译器")));
    }

    #[test]
    fn a_relative_path_is_refused() {
        let _g = reset();
        config("a20.xlator=1 a20.xlator.x86_64=qemu-x86_64");
        assert!(warnings().iter().any(|m| m.contains("不是绝对路径，忽略")));
        assert_eq!(unsafe { lookup(62, ELF_ABI_LINUX) }.0, ENOENT);
    }

    #[test]
    fn an_argv_override_is_validated_and_normalized() {
        let _g = reset();
        config("a20.xlator=1 a20.xlator.x86_64=/bin/qemu-x86_64 a20.xlator.x86_64.argv=@P,@*");
        let text = render();
        assert!(text.contains("argv=\"@P @*\" (override)"), "{text}");
        let (rc, desc) = unsafe { lookup(62, ELF_ABI_LINUX) };
        assert_eq!(rc, 0);
        assert_eq!(c_str(desc.argv), b"@P @*");
    }

    #[test]
    fn an_invalid_argv_override_disables_the_guest() {
        let _g = reset();
        config("a20.xlator=1 a20.xlator.x86_64=/bin/qemu-x86_64 a20.xlator.x86_64.argv=@A");
        assert!(warnings().iter().any(|m| m.contains(".argv='@A' 非法（未知 @ 记号或缺少 @P）")));
        assert_eq!(unsafe { lookup(62, ELF_ABI_LINUX) }.0, ENOENT);
        assert!(render().contains("argv=\"(unset)\""));
    }

    #[test]
    fn an_env_override_is_split_and_rejoined() {
        let _g = reset();
        config("a20.xlator=1 a20.xlator.x86_64=/bin/qemu-x86_64 a20.xlator.x86_64.env=A=1,B=2");
        let (_, desc) = unsafe { lookup(62, ELF_ABI_LINUX) };
        assert_eq!(c_str(desc.env), b"A=1,B=2");
        assert!(render().contains("env=A=1,B=2"));
    }

    #[test]
    fn an_invalid_env_override_disables_the_guest() {
        let _g = reset();
        config("a20.xlator=1 a20.xlator.x86_64=/bin/qemu-x86_64 a20.xlator.x86_64.env=NOVALUE");
        assert!(warnings().iter().any(|m| m.contains("缺少 '=' 或名称为空")));
        assert_eq!(unsafe { lookup(62, ELF_ABI_LINUX) }.0, ENOENT);
    }

    #[test]
    fn an_over_long_env_is_refused_before_splitting() {
        let _g = reset();
        let long = format!("a20.xlator=1 a20.xlator.x86_64=/bin/qemu-x86_64 a20.xlator.x86_64.env=A={}",
                           "v".repeat(XLATOR_ENV_LEN));
        config(&long);
        assert!(warnings().iter().any(|m| m.contains(".env 过长")));
    }

    #[test]
    fn unknown_names_and_abis_are_reported_distinctly() {
        let _g = reset();
        config("a20.xlator=1 a20.xlator.sparc=/bin/x a20.xlator.x86_64.foo=/bin/y a20.xlator.x86_64.=/bin/z");
        let w = warnings();
        assert!(w.iter().any(|m| m == "[XLATOR] 未知外来架构 'sparc'，忽略（见 proc/xlator_guests.def）\n"));
        assert!(w.iter().any(|m| m == "[XLATOR] 未知 ABI 'foo'，a20.xlator.x86_64.<abi> 忽略（当前可用 linux / native）\n"));
        assert!(w.iter().any(|m| m.contains(".<abi> 缺少 ABI 名")));
    }

    #[test]
    fn a_missing_abi_for_the_other_half_is_named() {
        let _g = reset();
        config("a20.xlator=1 a20.xlator.x86_64.native=/bin/x");
        assert!(warnings().iter().any(|m| m
            == "[XLATOR] 'x86_64' 未注册 native ABI 的翻译器，忽略（见 proc/xlator_guests.def）\n"));
    }

    #[test]
    fn an_empty_guest_name_is_reported() {
        let _g = reset();
        config("a20.xlator=1 a20.xlator.=/bin/x");
        assert!(warnings().iter().any(|m| m == "[XLATOR] a20.xlator.=... 缺少 guest 名，忽略\n"));
    }

    #[test]
    fn key_order_does_not_change_the_outcome() {
        let _g = reset();
        config("a20.xlator.x86_64=/bin/qemu-x86_64 a20.xlator.x86_64.env=A=1 a20.xlator=1 a20.xlator.x86_64.argv=@P,@*");
        let text = render();
        assert!(text.contains("path=/bin/qemu-x86_64 argv=\"@P @*\" (override) env=A=1"));
    }

    #[test]
    fn lookup_is_keyed_on_the_machine_and_abi_pair() {
        let _g = reset();
        config("a20.xlator=1 a20.xlator.x86_64=/bin/qemu-x86_64");
        assert_eq!(unsafe { lookup(62, ELF_ABI_LINUX) }.0, 0);
        assert_eq!(unsafe { lookup(62, ELF_ABI_NATIVE) }.0, ENOENT);
        assert_eq!(unsafe { lookup(183, ELF_ABI_LINUX) }.0, ENOENT);
        ENABLED.store(0, core::sync::atomic::Ordering::Release);
        assert_eq!(unsafe { lookup(62, ELF_ABI_LINUX) }.0, ENOENT);
    }

    #[test]
    fn a_later_key_overwrites_the_earlier_one() {
        let _g = reset();
        config("a20.xlator=1 a20.xlator.x86_64=/bin/one a20.xlator.x86_64=/bin/two");
        let (_, desc) = unsafe { lookup(62, ELF_ABI_LINUX) };
        assert_eq!(c_str(desc.path), b"/bin/two");
    }

    #[test]
    fn render_truncates_and_reports_the_bound() {
        let _g = reset();
        config("a20.xlator=1 a20.xlator.x86_64=/bin/qemu-x86_64");
        let mut small = [0u8; 8];
        let rc = unsafe { a20_xlator_render(small.as_mut_ptr(), small.len()) };
        assert_eq!(rc, 7);
    }

    #[test]
    fn the_forward_counter_is_reported() {
        let _g = reset();
        unsafe { a20_xlator_note_forward() };
        unsafe { a20_xlator_note_forward() };
        assert!(render().contains("forwards: 2\n"));
    }
}
