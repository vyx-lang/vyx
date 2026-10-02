#![allow(dead_code, improper_ctypes_definitions)]

// Default Rust layout: rustc may reorder fields. Probe, do not guess.
pub struct NativePair {
    pub flag: u8,
    pub wide: u64,
    pub mid: u32,
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

#[unsafe(no_mangle)]
pub extern "C" fn vyx_c_size() -> usize {
    core::mem::size_of::<NativePair>()
}

#[unsafe(no_mangle)]
pub extern "C" fn vyx_c_align() -> usize {
    core::mem::align_of::<NativePair>()
}

#[unsafe(no_mangle)]
pub extern "C" fn vyx_c_off_flag() -> usize {
    core::mem::offset_of!(NativePair, flag)
}

#[unsafe(no_mangle)]
pub extern "C" fn vyx_c_off_wide() -> usize {
    core::mem::offset_of!(NativePair, wide)
}

#[unsafe(no_mangle)]
pub extern "C" fn vyx_c_off_mid() -> usize {
    core::mem::offset_of!(NativePair, mid)
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
pub extern "C" fn vyx_dyn_size() -> usize {
    core::mem::size_of::<&dyn Sink>()
}

#[unsafe(no_mangle)]
pub extern "C" fn vyx_dyn_align() -> usize {
    core::mem::align_of::<&dyn Sink>()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn vyx_vtable_slot(obj: *const dyn Sink, index: usize) -> usize {
    let fat = obj as *const *const ();
    let vtbl = unsafe { *fat.add(1) as *const usize };
    unsafe { *vtbl.add(index) }
}

#[unsafe(no_mangle)]
pub extern "C" fn vyx_vtable_len() -> usize {
    // drop, size, align, then consume
    4
}

fn main() {
    let sink = VyxSink { bias: 7000 };
    let obj: &dyn Sink = &sink;
    let fat: [*const (); 2] = unsafe { core::mem::transmute_copy(&obj) };
    println!(
        "pair size={} align={} flag={} wide={} mid={}",
        core::mem::size_of::<NativePair>(),
        core::mem::align_of::<NativePair>(),
        core::mem::offset_of!(NativePair, flag),
        core::mem::offset_of!(NativePair, wide),
        core::mem::offset_of!(NativePair, mid),
    );
    println!("dyn_ref size={} align={}", core::mem::size_of::<&dyn Sink>(), core::mem::align_of::<&dyn Sink>());
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
    println!("consume={}", obj.consume(1));
}
