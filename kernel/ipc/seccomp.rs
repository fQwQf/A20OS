#![no_std]

use core::ffi::c_void;
use core::ptr;

const EINVAL: i32 = 22;
const EFAULT: i32 = 14;
const ENOMEM: i32 = 12;
const EPERM: u32 = 1;
const RET_ERRNO: u32 = 0x0005_0000;
const RET_ALLOW: u32 = 0x7fff_0000;
const RET_ACTION: u32 = 0xffff_0000;
const MAX_INSNS: usize = 4096;
const MEMWORDS: usize = 16;
const SD_SIZE: usize = 64;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SockFilter { code: u16, jt: u8, jf: u8, k: u32 }
#[repr(C)]
#[derive(Clone, Copy)]
pub struct SeccompData { nr: u32, arch: u32, ip: u64, args: [u64; 6] }
#[repr(C)]
struct SockFprog { len: u16, filter: *const SockFilter }
#[repr(C)]
struct Filter { refs: i32, prev: *mut Filter, len: u16, insns: *mut SockFilter }

unsafe extern "C" {
    fn a20_seccomp_copy_from_user(dst: *mut c_void, src: *const c_void, len: usize) -> i32;
    fn a20_seccomp_alloc(size: usize) -> *mut c_void;
    fn a20_seccomp_free(ptr: *mut c_void);
}

#[inline] fn class(c: u16) -> u16 { c & 7 }
#[inline] fn size(c: u16) -> u16 { c & 0x18 }
#[inline] fn mode(c: u16) -> u16 { c & 0xe0 }
#[inline] fn op(c: u16) -> u16 { c & 0xf0 }
#[inline] fn src(c: u16) -> u16 { c & 8 }
#[inline] fn rval(c: u16) -> u16 { c & 0x18 }

fn verify(p: &[SockFilter]) -> bool {
    let len = p.len();
    if len == 0 || len > MAX_INSNS || class(p[len - 1].code) != 6 { return false; }
    for (i, f) in p.iter().enumerate() {
        let c = f.code;
        match class(c) {
            0 | 1 => match mode(c) {
                0 => (),
                0x20 | 0x40 => if size(c) != 0 || (mode(c) == 0x20 && (f.k as usize > SD_SIZE || SD_SIZE - (f.k as usize) < 4)) { return false; },
                0x60 | 0x80 => if size(c) != 0 || (mode(c) == 0x60 && f.k >= MEMWORDS as u32) { return false; },
                _ => return false,
            },
            2 | 3 => if c != 2 && c != 3 || f.k >= MEMWORDS as u32 { return false; },
            4 => match op(c) {
                0|0x10|0x20|0x30|0x40|0x50|0x60|0x70|0x90|0xa0 => (),
                0x80 => (), _ => return false,
            },
            5 => match op(c) {
                0 => if f.k >= (len - i - 1) as u32 { return false; },
                0x10|0x20|0x30|0x40 => if i + 1 + f.jt as usize >= len || i + 1 + f.jf as usize >= len { return false; },
                _ => return false,
            },
            6 => if rval(c) != 0 && rval(c) != 0x10 && rval(c) != 0x18 { return false; },
            7 => if c & 0xf0 != 0 && c & 0xf0 != 0x80 { return false; },
            _ => return false,
        }
    }
    true
}

fn run(p: &[SockFilter], sd: &SeccompData) -> u32 {
    let data = sd as *const _ as *const u8;
    let (mut a, mut x) = (0u32, 0u32);
    let mut m = [0u32; MEMWORDS];
    let mut pc = 0usize;
    while pc < p.len() {
        let f = p[pc]; pc += 1;
        let k = f.k;
        match class(f.code) {
            0 | 1 => {
                let saved = a;
                match mode(f.code) {
                    0 => a = k,
                    0x20 => { if k as usize > SD_SIZE || SD_SIZE - (k as usize) < 4 { return RET_ERRNO | EPERM; } unsafe { a = ptr::read_unaligned(data.add(k as usize) as *const u32); } },
                    0x40 => { let n = x.wrapping_add(k); if n < x || n as usize > SD_SIZE || SD_SIZE - (n as usize) < 4 { return RET_ERRNO | EPERM; } unsafe { a = ptr::read_unaligned(data.add(n as usize) as *const u32); } },
                    0x60 => a = m[(k as usize) & (MEMWORDS-1)],
                    0x80 => a = SD_SIZE as u32,
                    _ => return RET_ERRNO | EPERM,
                }
                if class(f.code) == 1 { x = a; a = saved; }
            }
            2 => m[(k as usize)&(MEMWORDS-1)] = a,
            3 => m[(k as usize)&(MEMWORDS-1)] = x,
            4 => { let v = if src(f.code) == 8 { x } else { k }; a = match op(f.code) { 0=>a.wrapping_add(v),0x10=>a.wrapping_sub(v),0x20=>a.wrapping_mul(v),0x30=>if v==0 {0}else{a/v},0x40=>a|v,0x50=>a&v,0x60=>if v<32 {a<<v}else{0},0x70=>if v<32 {a>>v}else{0},0x80=>(-(a as i32)) as u32,0x90=>if v==0 {0}else{a%v},0xa0=>a^v,_=>return RET_ERRNO|EPERM}; },
            5 => { let v=if src(f.code)==8{x}else{k}; match op(f.code) {0=>{pc+=k as usize;continue},0x10=>pc+=if a==v{f.jt as usize}else{f.jf as usize},0x20=>pc+=if a>v{f.jt as usize}else{f.jf as usize},0x30=>pc+=if a>=v{f.jt as usize}else{f.jf as usize},0x40=>pc+=if a&v!=0{f.jt as usize}else{f.jf as usize},_=>return RET_ERRNO|EPERM} },
            6 => return match rval(f.code) {0x10=>a,0x18=>x,_=>k},
            7 => if f.code & 0xf0 == 0 { x=a; } else { a=x; },
            _ => return RET_ERRNO|EPERM,
        }
    }
    RET_ERRNO | EPERM
}

#[no_mangle]
pub unsafe extern "C" fn a20_seccomp_filter_install(chain: *mut *mut c_void, ufprog: *const c_void) -> i32 {
    if chain.is_null() || ufprog.is_null() { return -EFAULT; }
    let mut prog = SockFprog { len: 0, filter: ptr::null() };
    if a20_seccomp_copy_from_user(&mut prog as *mut _ as *mut c_void, ufprog, core::mem::size_of::<SockFprog>()) < 0 { return -EFAULT; }
    let len = prog.len as usize;
    if len == 0 || len > MAX_INSNS || prog.filter.is_null() { return -EINVAL; }
    let bytes = len * core::mem::size_of::<SockFilter>();
    let insns = a20_seccomp_alloc(bytes) as *mut SockFilter;
    if insns.is_null() { return -ENOMEM; }
    if a20_seccomp_copy_from_user(insns as *mut c_void, prog.filter as *const c_void, bytes) < 0 { a20_seccomp_free(insns as *mut c_void); return -EFAULT; }
    if !verify(core::slice::from_raw_parts(insns, len)) { a20_seccomp_free(insns as *mut c_void); return -EINVAL; }
    let f = a20_seccomp_alloc(core::mem::size_of::<Filter>()) as *mut Filter;
    if f.is_null() { a20_seccomp_free(insns as *mut c_void); return -ENOMEM; }
    (*f).refs=1; (*f).prev=*chain as *mut Filter; (*f).len=len as u16; (*f).insns=insns;
    *chain=f as *mut c_void;
    0
}

#[no_mangle]
pub unsafe extern "C" fn a20_seccomp_filter_inherit(dst: *mut *mut c_void, src: *mut c_void) {
    *dst=src;
    if !src.is_null() { core::sync::atomic::AtomicI32::from_ptr(&mut (*(src as *mut Filter)).refs).fetch_add(1, core::sync::atomic::Ordering::AcqRel); }
}
#[no_mangle]
pub unsafe extern "C" fn a20_seccomp_filter_release(chain: *mut *mut c_void) {
    let mut f=*chain as *mut Filter; *chain=ptr::null_mut();
    while !f.is_null() {
        let refs=&mut (*f).refs;
        if core::sync::atomic::AtomicI32::from_ptr(refs).fetch_sub(1,core::sync::atomic::Ordering::AcqRel)!=1 { break; }
        let prev=(*f).prev; a20_seccomp_free((*f).insns as *mut c_void); a20_seccomp_free(f as *mut c_void); f=prev;
    }
}
#[no_mangle]
pub unsafe extern "C" fn a20_seccomp_filter_evaluate(chain: *const c_void, sd: *const SeccompData) -> u32 {
    let mut f=chain as *mut Filter; let mut result=RET_ALLOW;
    while !f.is_null() {
        let act=run(core::slice::from_raw_parts((*f).insns,(*f).len as usize),&*sd);
        if ((act & RET_ACTION) as i32) < ((result & RET_ACTION) as i32) { result=act; }
        f=(*f).prev;
    }
    result
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test] fn verifier_and_vm_handle_returns_and_bad_jumps() {
        let allow=[SockFilter{code:0x06,jt:0,jf:0,k:RET_ALLOW}];
        let sd=SeccompData{nr:1,arch:2,ip:3,args:[0;6]};
        assert!(verify(&allow)); assert_eq!(run(&allow,&sd),RET_ALLOW);
        let bad=[SockFilter{code:0x05,jt:0,jf:0,k:1},SockFilter{code:0x06,jt:0,jf:0,k:RET_ALLOW}];
        assert!(!verify(&bad));
        let load=[SockFilter{code:0x20,jt:0,jf:0,k:SD_SIZE as u32},SockFilter{code:0x06,jt:0,jf:0,k:RET_ALLOW}];
        assert!(!verify(&load));
    }
    #[test] fn conditional_jump_uses_expected_action() {
        let p=[SockFilter{code:0x00,jt:0,jf:0,k:42},SockFilter{code:0x15,jt:0,jf:1,k:42},SockFilter{code:0x06,jt:0,jf:0,k:RET_ALLOW},SockFilter{code:0x06,jt:0,jf:0,k:RET_ERRNO|EPERM}];
        let sd=SeccompData{nr:0,arch:0,ip:0,args:[0;6]};
        assert!(verify(&p)); assert_eq!(run(&p,&sd),RET_ALLOW);
    }
}

#[cfg(test)]
mod chain_tests {
 use super::*;
 #[test] fn most_restrictive_action_wins_across_chain() {
  let allow=[SockFilter{code:0x06,jt:0,jf:0,k:RET_ALLOW}];
  let deny=[SockFilter{code:0x06,jt:0,jf:0,k:RET_ERRNO|EPERM}];
  let mut base=Filter{refs:1,prev:core::ptr::null_mut(),len:1,insns:allow.as_ptr() as *mut _};
  let head=Filter{refs:1,prev:&mut base,len:1,insns:deny.as_ptr() as *mut _};
  let sd=SeccompData{nr:0,arch:0,ip:0,args:[0;6]};
  unsafe { assert_eq!(a20_seccomp_filter_evaluate(&head as *const _ as *const c_void,&sd),RET_ERRNO|EPERM); }
 }
}
