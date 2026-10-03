#![no_std]

use core::hint::spin_loop;
use core::panic::PanicInfo;
use core::ptr;
use core::sync::atomic::{AtomicI32, Ordering};

#[panic_handler]
fn panic(_info: &PanicInfo<'_>) -> ! {
    loop {
        spin_loop();
    }
}

#[repr(C)]
pub struct Packet {
    pub total: i64,
    pub tag: i64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Flags {
    pub storage: u32,
}

#[repr(C, align(16))]
#[derive(Clone, Copy)]
pub struct AlignedPair {
    pub low: u64,
    pub high: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct BitfieldProbe {
    pub prefix: u32,
    pub storage: u32,
}

#[repr(C)]
pub struct RuntimeBase {
    pub kind: i32,
    pub value: i32,
}

#[repr(C)]
pub struct RuntimeDerived {
    pub prefix: i64,
    pub base: RuntimeBase,
    pub extra: i32,
    pub padding: i32,
}

const EMPTY_PACKET: Packet = Packet { total: 0, tag: 0 };
const PACKET_TYPE_ID: u64 = 0x4443_4952_5553_5401;

static CREATES: AtomicI32 = AtomicI32::new(0);
static COPIES: AtomicI32 = AtomicI32::new(0);
static MOVES: AtomicI32 = AtomicI32::new(0);
static DESTROY_CALLS: AtomicI32 = AtomicI32::new(0);
static DESTROYS: AtomicI32 = AtomicI32::new(0);
static mut RUNTIME_BASE: RuntimeBase = RuntimeBase { kind: 0, value: 0 };
static mut RUNTIME_DERIVED: RuntimeDerived = RuntimeDerived {
    prefix: 0,
    base: RuntimeBase { kind: 1, value: 0 },
    extra: 0,
    padding: 0,
};

#[no_mangle]
pub extern "system" fn dci_rust_packet_create(seed: i64) -> Packet {
    CREATES.fetch_add(1, Ordering::Relaxed);
    Packet {
        total: seed,
        tag: seed + 1000,
    }
}

#[no_mangle]
pub unsafe extern "system" fn dci_rust_packet_copy(value: *const Packet) -> Packet {
    let Some(value) = (unsafe { value.as_ref() }) else {
        return EMPTY_PACKET;
    };
    COPIES.fetch_add(1, Ordering::Relaxed);
    Packet {
        total: value.total,
        tag: value.tag,
    }
}

#[no_mangle]
pub unsafe extern "system" fn dci_rust_packet_move(value: *mut Packet) -> Packet {
    if value.is_null() {
        return EMPTY_PACKET;
    }
    let result = unsafe { ptr::read(value) };
    unsafe { ptr::write(value, EMPTY_PACKET) };
    MOVES.fetch_add(1, Ordering::Relaxed);
    result
}

#[no_mangle]
pub unsafe extern "system" fn dci_rust_packet_destroy(value: *mut Packet) {
    if value.is_null() {
        return;
    }
    DESTROY_CALLS.fetch_add(1, Ordering::Relaxed);
    let packet = unsafe { &mut *value };
    if packet.total != 0 || packet.tag != 0 {
        *packet = EMPTY_PACKET;
        DESTROYS.fetch_add(1, Ordering::Relaxed);
    }
}

#[no_mangle]
pub unsafe extern "system" fn dci_rust_packet_fold(value: *const Packet) -> i64 {
    let Some(value) = (unsafe { value.as_ref() }) else {
        return 0;
    };
    value.total + value.tag
}

#[no_mangle]
pub extern "system" fn dci_rust_creates() -> i32 {
    CREATES.load(Ordering::Relaxed)
}

#[no_mangle]
pub extern "system" fn dci_rust_copies() -> i32 {
    COPIES.load(Ordering::Relaxed)
}

#[no_mangle]
pub extern "system" fn dci_rust_moves() -> i32 {
    MOVES.load(Ordering::Relaxed)
}

#[no_mangle]
pub extern "system" fn dci_rust_destroys() -> i32 {
    DESTROYS.load(Ordering::Relaxed)
}

#[no_mangle]
pub extern "system" fn dci_rust_destroy_calls() -> i32 {
    DESTROY_CALLS.load(Ordering::Relaxed)
}

#[no_mangle]
pub extern "system" fn dci_rust_flags_make(priority: u32, enabled: i32) -> Flags {
    Flags {
        storage: (priority & 0x7) | (if enabled != 0 { 1 << 3 } else { 0 }),
    }
}

#[no_mangle]
pub extern "system" fn dci_rust_flags_priority(value: Flags) -> u32 {
    value.storage & 0x7
}

#[no_mangle]
pub extern "system" fn dci_rust_flags_enabled(value: Flags) -> i32 {
    if value.storage & (1 << 3) != 0 { 1 } else { 0 }
}

#[no_mangle]
pub unsafe extern "system" fn dci_rust_mixed_params(
    flags: Flags,
    packet: *const Packet,
    bias: i64,
) -> i64 {
    let Some(packet) = (unsafe { packet.as_ref() }) else {
        return 0;
    };
    flags.storage as i64 + packet.total + packet.tag + bias
}

#[no_mangle]
pub extern "system" fn dci_rust_aligned_make(low: u64, high: u64) -> AlignedPair {
    AlignedPair { low, high }
}

#[no_mangle]
pub extern "system" fn dci_rust_aligned_fold(value: AlignedPair) -> u64 {
    value.low.wrapping_mul(10).wrapping_add(value.high)
}

#[no_mangle]
pub extern "system" fn dci_rust_aligned_byval_fold(value: AlignedPair) -> u64 {
    value.low.wrapping_mul(17).wrapping_add(value.high)
}

#[no_mangle]
pub extern "system" fn dci_rust_bitfield_probe_make(value: i32) -> BitfieldProbe {
    BitfieldProbe {
        prefix: 0x1234_5678,
        storage: ((value as u32) & 0x0f) << 28,
    }
}

#[no_mangle]
pub extern "system" fn dci_rust_packet_type_id() -> u64 {
    PACKET_TYPE_ID
}

#[no_mangle]
pub extern "system" fn dci_rust_packet_is_type(type_id: u64) -> i32 {
    if type_id == PACKET_TYPE_ID { 1 } else { 0 }
}

#[no_mangle]
pub unsafe extern "system" fn dci_rust_runtime_make(
    kind: i32,
    value: i32,
) -> *mut RuntimeBase {
    if kind < 0 {
        return ptr::null_mut();
    }
    if kind == 1 {
        unsafe {
            RUNTIME_DERIVED.prefix = 5000;
            RUNTIME_DERIVED.base.kind = 1;
            RUNTIME_DERIVED.base.value = value;
            RUNTIME_DERIVED.extra = 700;
        }
        return ptr::addr_of_mut!(RUNTIME_DERIVED.base);
    }
    unsafe {
        RUNTIME_BASE.kind = 0;
        RUNTIME_BASE.value = value;
    }
    ptr::addr_of_mut!(RUNTIME_BASE)
}

#[no_mangle]
pub unsafe extern "system" fn dci_rust_runtime_checked_downcast(
    value: *mut RuntimeBase,
) -> *mut RuntimeDerived {
    if value.is_null() || unsafe { (*value).kind } != 1 {
        return ptr::null_mut();
    }
    let derived = unsafe { value.cast::<u8>().sub(8).cast::<RuntimeDerived>() };
    if unsafe { (*derived).prefix } != 5000 {
        return ptr::null_mut();
    }
    derived
}

#[no_mangle]
pub unsafe extern "system" fn dci_rust_runtime_inspect(value: *mut RuntimeDerived) -> i64 {
    if value.is_null() {
        return -1;
    }
    unsafe { (*value).prefix + (*value).base.value as i64 + (*value).extra as i64 }
}
