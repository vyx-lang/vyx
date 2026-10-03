#![allow(dead_code, improper_ctypes_definitions, invalid_value)]

// rustc-measured ABI dump. Rebuild when rustc or target changes.
// Fields may be reordered; fat pointers are two words; niches are rustc-private.

use core::ptr::NonNull;

pub struct NativePair {
    pub flag: u8,
    pub wide: u64,
    pub mid: u32,
}

pub struct Word1 {
    pub a: u64,
}

pub struct Word2 {
    pub a: u64,
    pub b: u64,
}

pub struct Word3 {
    pub a: u64,
    pub b: u64,
    pub c: u64,
}

pub struct DropGuard {
    pub value: i32,
}

impl Drop for DropGuard {
    fn drop(&mut self) {}
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

#[repr(C)]
pub enum ColorC {
    Red = 0,
    Green = 1,
    Blue = 2,
}

pub enum ColorNative {
    Red,
    Green,
    Blue,
}

pub enum Payload {
    Empty,
    One(u32),
    Two(u64, u8),
}

macro_rules! dump_ty {
    ($name:expr, $ty:ty) => {
        println!(
            "layout {} size={} align={}",
            $name,
            core::mem::size_of::<$ty>(),
            core::mem::align_of::<$ty>(),
        );
    };
}

macro_rules! dump_off {
    ($name:expr, $ty:ty, $field:ident) => {
        println!(
            "offset {}.{} {}",
            $name,
            stringify!($field),
            core::mem::offset_of!($ty, $field),
        );
    };
}

#[unsafe(no_mangle)]
pub extern "C" fn abi_c_word1(value: Word1) {
    core::mem::forget(value);
}

#[unsafe(no_mangle)]
pub extern "Rust" fn abi_rust_word1(value: Word1) {
    core::mem::forget(value);
}

#[unsafe(no_mangle)]
pub extern "C" fn abi_c_word2(value: Word2) {
    core::mem::forget(value);
}

#[unsafe(no_mangle)]
pub extern "Rust" fn abi_rust_word2(value: Word2) {
    core::mem::forget(value);
}

#[unsafe(no_mangle)]
pub extern "C" fn abi_c_word3(value: Word3) {
    core::mem::forget(value);
}

#[unsafe(no_mangle)]
pub extern "Rust" fn abi_rust_word3(value: Word3) {
    core::mem::forget(value);
}

#[unsafe(no_mangle)]
pub extern "C" fn abi_c_thin(value: &NativePair) -> usize {
    value as *const NativePair as usize
}

#[unsafe(no_mangle)]
pub extern "Rust" fn abi_rust_thin(value: &NativePair) -> usize {
    value as *const NativePair as usize
}

#[unsafe(no_mangle)]
pub extern "C" fn abi_c_slice(value: &[u8]) -> usize {
    value.len()
}

#[unsafe(no_mangle)]
pub extern "Rust" fn abi_rust_slice(value: &[u8]) -> usize {
    value.len()
}

#[unsafe(no_mangle)]
pub extern "C" fn abi_c_str(value: &str) -> usize {
    value.len()
}

#[unsafe(no_mangle)]
pub extern "Rust" fn abi_rust_str(value: &str) -> usize {
    value.len()
}

#[unsafe(no_mangle)]
pub extern "C" fn abi_c_dyn(value: &dyn Sink) -> usize {
    let fat: [*const (); 2] = unsafe { core::mem::transmute_copy(&value) };
    fat[1] as usize
}

#[unsafe(no_mangle)]
pub extern "Rust" fn abi_rust_dyn(value: &dyn Sink) -> usize {
    let fat: [*const (); 2] = unsafe { core::mem::transmute_copy(&value) };
    fat[1] as usize
}

#[unsafe(no_mangle)]
pub extern "C" fn abi_c_box(value: Box<i32>) -> i32 {
    *value
}

#[unsafe(no_mangle)]
pub extern "Rust" fn abi_rust_box(value: Box<i32>) -> i32 {
    *value
}

#[unsafe(no_mangle)]
pub extern "C" fn abi_c_box_dyn(value: Box<dyn Sink>) -> i32 {
    value.consume(0)
}

#[unsafe(no_mangle)]
pub extern "Rust" fn abi_rust_box_dyn(value: Box<dyn Sink>) -> i32 {
    value.consume(0)
}

#[unsafe(no_mangle)]
pub extern "C" fn abi_c_opt_box(value: Option<Box<i32>>) -> i32 {
    value.map(|b| *b).unwrap_or(0)
}

#[unsafe(no_mangle)]
pub extern "Rust" fn abi_rust_opt_box(value: Option<Box<i32>>) -> i32 {
    value.map(|b| *b).unwrap_or(0)
}

fn main() {
    println!("rustc-depth-probe");
    dump_ty!("NativePair", NativePair);
    dump_off!("NativePair", NativePair, flag);
    dump_off!("NativePair", NativePair, wide);
    dump_off!("NativePair", NativePair, mid);
    dump_ty!("Word1", Word1);
    dump_ty!("Word2", Word2);
    dump_ty!("Word3", Word3);
    dump_ty!("DropGuard", DropGuard);
    dump_ty!("&i32", &i32);
    dump_ty!("&mut i32", &mut i32);
    dump_ty!("*const i32", *const i32);
    dump_ty!("*mut i32", *mut i32);
    dump_ty!("NonNull<i32>", NonNull<i32>);
    dump_ty!("&[u8]", &[u8]);
    dump_ty!("&mut [u8]", &mut [u8]);
    dump_ty!("*const [u8]", *const [u8]);
    dump_ty!("&str", &str);
    dump_ty!("&dyn Sink", &dyn Sink);
    dump_ty!("*const dyn Sink", *const dyn Sink);
    dump_ty!("Box<i32>", Box<i32>);
    dump_ty!("Box<[u8]>", Box<[u8]>);
    dump_ty!("Box<dyn Sink>", Box<dyn Sink>);
    dump_ty!("Option<&i32>", Option<&i32>);
    dump_ty!("Option<NonNull<i32>>", Option<NonNull<i32>>);
    dump_ty!("Option<Box<i32>>", Option<Box<i32>>);
    dump_ty!("Option<bool>", Option<bool>);
    dump_ty!("Option<u32>", Option<u32>);
    dump_ty!("ColorC", ColorC);
    dump_ty!("ColorNative", ColorNative);
    dump_ty!("Payload", Payload);
    dump_ty!("Vec<u8>", Vec<u8>);
    dump_ty!("String", String);
    dump_ty!("[u8; 3]", [u8; 3]);
    dump_ty!("[u64; 2]", [u64; 2]);

    let sink = VyxSink { bias: 7 };
    let obj: &dyn Sink = &sink;
    let fat: [*const (); 2] = unsafe { core::mem::transmute_copy(&obj) };
    println!("fat data={:p} vtable={:p}", fat[0], fat[1]);
    unsafe {
        let vtbl = fat[1] as *const usize;
        println!(
            "vtable drop={:#x} size={} align={} method0={:#x}",
            *vtbl.add(0),
            *vtbl.add(1),
            *vtbl.add(2),
            *vtbl.add(3),
        );
    }

    let slice: &[u8] = b"abc";
    let slice_fat: [*const (); 2] = unsafe { core::mem::transmute_copy(&slice) };
    println!("slice data={:p} len={}", slice_fat[0], slice_fat[1] as usize);

    let text: &str = "abc";
    let str_fat: [*const (); 2] = unsafe { core::mem::transmute_copy(&text) };
    println!("str data={:p} len={}", str_fat[0], str_fat[1] as usize);

    let boxed = Box::new(42i32);
    println!("box ptr={:p} opt_box_none_size={}", &*boxed, core::mem::size_of::<Option<Box<i32>>>());
    let none_box: Option<Box<i32>> = None;
    let none_bits: usize = unsafe { core::mem::transmute_copy(&none_box) };
    println!("opt_box_none_bits={:#x}", none_bits);
}
