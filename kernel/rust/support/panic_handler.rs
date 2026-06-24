//! Minimal panic handler for all Rust kernel modules.
//!
//! Compiles to a staticlib that provides `#[panic_handler]`.  Linked together
//! with other Rust modules so each module crate does not have to repeat the
//! handler.

#![no_std]
#![feature(lang_items)]

pub mod lock;

use core::panic::PanicInfo;

extern "C" {
    fn panic(fmt: *const core::ffi::c_char, ...) -> !;
}

#[panic_handler]
fn panic_handler(_info: &PanicInfo) -> ! {
    const MSG: *const core::ffi::c_char = b"Rust panic\0".as_ptr() as _;
    unsafe { panic(MSG) }
}

#[lang = "eh_personality"]
fn eh_personality() {}
