#![allow(dead_code, improper_ctypes_definitions)]

pub struct NativePair {
    pub flag: u8,
    pub wide: u64,
    pub mid: u32,
}

pub enum Color {
    Red,
    Green,
    Blue,
}

pub trait Sink {
    fn consume(&self, x: i32) -> i32;
}

pub struct VyxSink {
    pub bias: i32,
}

impl Sink for VyxSink {
    fn consume(&self, x: i32) -> i32 {
        x + self.bias
    }
}

impl NativePair {
    pub fn flag_of(&self) -> u8 {
        self.flag
    }

    pub fn take(self) -> NativePair {
        self
    }

    fn hidden(&self) -> u8 {
        self.flag
    }
}

pub fn take_pair(value: NativePair) {
    core::mem::forget(value);
}

#[unsafe(no_mangle)]
pub extern "C" fn vyx_c_arg(value: NativePair) {
    core::mem::forget(value);
}

#[unsafe(no_mangle)]
pub extern "Rust" fn vyx_rust_arg(value: NativePair) {
    core::mem::forget(value);
}

#[unsafe(no_mangle)]
pub extern "C" fn vyx_c_thin(value: &NativePair) -> usize {
    value as *const NativePair as usize
}

#[unsafe(no_mangle)]
pub extern "Rust" fn vyx_rust_thin(value: &NativePair) -> usize {
    value as *const NativePair as usize
}

#[unsafe(no_mangle)]
pub extern "C" fn vyx_c_slice(value: &[u8]) -> usize {
    value.len()
}

#[unsafe(no_mangle)]
pub extern "Rust" fn vyx_rust_slice(value: &[u8]) -> usize {
    value.len()
}

#[unsafe(no_mangle)]
pub extern "C" fn vyx_c_dyn(value: &dyn Sink) -> usize {
    let fat: [*const (); 2] = unsafe { core::mem::transmute_copy(&value) };
    fat[1] as usize
}

#[unsafe(no_mangle)]
pub extern "Rust" fn vyx_rust_dyn(value: &dyn Sink) -> usize {
    let fat: [*const (); 2] = unsafe { core::mem::transmute_copy(&value) };
    fat[1] as usize
}

#[unsafe(no_mangle)]
pub extern "C" fn vyx_c_color(value: Color) -> i32 {
    match value {
        Color::Red => 0,
        Color::Green => 1,
        Color::Blue => 2,
    }
}
