// Shared DciFailure capture for Rust panic and Result::Err.
// Layout must match dci_failure_abi.h (64 bytes on LP64/LLP64).

use std::any::Any;
use std::ffi::c_char;
use std::mem::{align_of, size_of};
use std::ptr;
use std::slice;

pub const DCI_FAILURE_TAG_MSVC_CXX: u32 = 1;
pub const DCI_FAILURE_TAG_ITANIUM_CXX: u32 = 2;
pub const DCI_FAILURE_TAG_RUST_PANIC: u32 = 3;
pub const DCI_FAILURE_TAG_RUST_RESULT: u32 = 4;
pub const DCI_FAILURE_TAG_ZIG_ERROR: u32 = 5;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct DciFailure {
    pub type_identity: *const c_char,
    pub type_identity_len: u64,
    pub message: *const c_char,
    pub message_len: u64,
    pub payload: *const u8,
    pub payload_size: u64,
    pub payload_align: u64,
    pub producer_tag: u32,
}

impl DciFailure {
    pub fn empty() -> Self {
        Self {
            type_identity: ptr::null(),
            type_identity_len: 0,
            message: ptr::null(),
            message_len: 0,
            payload: ptr::null(),
            payload_size: 0,
            payload_align: 0,
            producer_tag: 0,
        }
    }
}

fn dup_bytes(src: &[u8]) -> *mut u8 {
    if src.is_empty() {
        return ptr::null_mut();
    }
    let mut owned = Vec::with_capacity(src.len() + 1);
    owned.extend_from_slice(src);
    owned.push(0);
    Box::into_raw(owned.into_boxed_slice()) as *mut u8
}

fn dup_str(text: &str) -> (*const c_char, u64) {
    let pointer = dup_bytes(text.as_bytes()) as *const c_char;
    (pointer, text.len() as u64)
}

unsafe fn free_cstr(pointer: *const c_char, len: u64) {
    if pointer.is_null() {
        return;
    }
    let n = len as usize + 1;
    let _ = Box::from_raw(slice::from_raw_parts_mut(pointer as *mut u8, n));
}

unsafe fn free_payload(pointer: *const u8, size: u64) {
    if pointer.is_null() || size == 0 {
        return;
    }
    let _ = Box::from_raw(slice::from_raw_parts_mut(pointer as *mut u8, size as usize));
}

pub fn dci_failure_clear(failure: &mut DciFailure) {
    *failure = DciFailure::empty();
}

pub fn dci_failure_destroy(failure: &mut DciFailure) {
    unsafe {
        free_cstr(failure.type_identity, failure.type_identity_len);
        free_cstr(failure.message, failure.message_len);
        free_payload(failure.payload, failure.payload_size);
    }
    dci_failure_clear(failure);
}

pub fn dci_failure_copy(src: &DciFailure, dst: &mut DciFailure) -> bool {
    dci_failure_clear(dst);
    dst.producer_tag = src.producer_tag;
    dst.payload_size = src.payload_size;
    dst.payload_align = src.payload_align;
    dst.type_identity_len = src.type_identity_len;
    dst.message_len = src.message_len;
    if !src.type_identity.is_null() && src.type_identity_len > 0 {
        let bytes = unsafe { slice::from_raw_parts(src.type_identity as *const u8, src.type_identity_len as usize) };
        dst.type_identity = dup_bytes(bytes) as *const c_char;
    }
    if !src.message.is_null() && src.message_len > 0 {
        let bytes = unsafe { slice::from_raw_parts(src.message as *const u8, src.message_len as usize) };
        dst.message = dup_bytes(bytes) as *const c_char;
    }
    if !src.payload.is_null() && src.payload_size > 0 {
        let bytes = unsafe { slice::from_raw_parts(src.payload, src.payload_size as usize) };
        let owned = bytes.to_vec();
        dst.payload = Box::into_raw(owned.into_boxed_slice()) as *const u8;
    }
    true
}

pub fn capture_panic_payload(payload: Box<dyn Any + Send>) -> DciFailure {
    let mut failure = DciFailure::empty();
    failure.producer_tag = DCI_FAILURE_TAG_RUST_PANIC;
    if let Some(text) = payload.downcast_ref::<&str>() {
        let (identity, identity_len) = dup_str("&str");
        failure.type_identity = identity;
        failure.type_identity_len = identity_len;
        let (message, message_len) = dup_str(text);
        failure.message = message;
        failure.message_len = message_len;
        return failure;
    }
    if let Some(text) = payload.downcast_ref::<String>() {
        let (identity, identity_len) = dup_str("alloc::string::String");
        failure.type_identity = identity;
        failure.type_identity_len = identity_len;
        let (message, message_len) = dup_str(text);
        failure.message = message;
        failure.message_len = message_len;
        return failure;
    }
    let (identity, identity_len) = dup_str("dyn core::any::Any");
    failure.type_identity = identity;
    failure.type_identity_len = identity_len;
    failure
}

pub fn capture_result_err<E: Copy + 'static>(err: E) -> DciFailure {
    let mut failure = DciFailure::empty();
    failure.producer_tag = DCI_FAILURE_TAG_RUST_RESULT;
    let (identity, identity_len) = dup_str(std::any::type_name::<E>());
    failure.type_identity = identity;
    failure.type_identity_len = identity_len;
    let size = size_of::<E>();
    let align = align_of::<E>();
    failure.payload_size = size as u64;
    failure.payload_align = align as u64;
    if size > 0 {
        let mut bytes = vec![0u8; size];
        unsafe {
            ptr::copy_nonoverlapping(&err as *const E as *const u8, bytes.as_mut_ptr(), size);
        }
        failure.payload = Box::into_raw(bytes.into_boxed_slice()) as *const u8;
    }
    failure
}

#[repr(C)]
pub struct DciTranslated<T> {
    pub tag: i8,
    pub ok: T,
    pub err: DciFailure,
}

pub fn translate_catch_unwind<T: Default, F: FnOnce() -> T>(body: F) -> DciTranslated<T> {
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(body)) {
        Ok(value) => DciTranslated {
            tag: 0,
            ok: value,
            err: DciFailure::empty(),
        },
        Err(payload) => DciTranslated {
            tag: 1,
            ok: T::default(),
            err: capture_panic_payload(payload),
        },
    }
}

pub fn translate_result<T: Default, E: Copy + 'static, F: FnOnce() -> Result<T, E>>(
    body: F,
) -> DciTranslated<T> {
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(body)) {
        Err(payload) => DciTranslated {
            tag: 1,
            ok: T::default(),
            err: capture_panic_payload(payload),
        },
        Ok(Ok(value)) => DciTranslated {
            tag: 0,
            ok: value,
            err: DciFailure::empty(),
        },
        Ok(Err(err)) => DciTranslated {
            tag: 1,
            ok: T::default(),
            err: capture_result_err(err),
        },
    }
}

#[cfg(test)]
mod layout_test {
    use super::*;
    #[test]
    fn failure_is_64_bytes() {
        assert_eq!(size_of::<DciFailure>(), 64);
    }
}
