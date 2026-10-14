#![no_std]

use core::panic::PanicInfo;

// Keep the Rust runtime's panic policy in Rust while routing the terminal
// report through the kernel's existing panic/console primitive.
unsafe extern "C" {
    fn panic(fmt: *const core::ffi::c_char, ...) -> !;
}

#[panic_handler]
fn rust_panic(_info: &PanicInfo<'_>) -> ! {
    unsafe { panic(b"Rust panic\0".as_ptr().cast()) }
}

// This entry exists only in the dedicated panic-path test profile. It must
// never be present in the normal kernel runtime archive.
#[cfg(a20_rust_runtime_panic_test)]
#[no_mangle]
pub extern "C" fn a20_rust_runtime_panic_test() -> ! {
    unsafe {
        extern "C" {
            fn printf(fmt: *const core::ffi::c_char, ...);
        }
        printf(b"RUST_RUNTIME_PANIC_PATH: PASS\n\0".as_ptr().cast());
    }
    panic!("A20 Rust panic runtime test")
}
