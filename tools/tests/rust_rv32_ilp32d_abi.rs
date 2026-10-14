#![no_std]

const _: [(); 4] = [(); core::mem::size_of::<usize>()];
const _: [(); 8] = [(); core::mem::size_of::<u64>()];
const _: [(); 8] = [(); core::mem::size_of::<f64>()];
const _: [(); 8] = [(); core::mem::align_of::<f64>()];

#[no_mangle]
pub extern "C" fn a20_rv32_ilp32d_probe(a: f64, b: u32) -> f64 {
    a + b as f64
}
