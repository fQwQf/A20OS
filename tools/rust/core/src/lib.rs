#![no_std]

// This crate exists only to make Cargo build the matching `core` for a custom
// freestanding target through `-Z build-std=core`. The kernel links its own
// Rust objects directly and does not link this wrapper crate.
