mod dci_rust_failure {
    include!("../runtime/dci_rust_failure.rs");
}

use dci_rust_failure::*;
use std::mem::size_of;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::slice;
use std::str;

fn expect(ok: bool, msg: &str) -> i32 {
    if ok {
        0
    } else {
        eprintln!("FAIL {msg}");
        1
    }
}

fn cstr<'a>(pointer: *const std::ffi::c_char, len: u64) -> Option<&'a str> {
    if pointer.is_null() || len == 0 {
        return None;
    }
    let bytes = unsafe { slice::from_raw_parts(pointer as *const u8, len as usize) };
    str::from_utf8(bytes).ok()
}

fn main() {
    let mut failed = 0;
    failed |= expect(size_of::<DciFailure>() == 64, "sizeof DciFailure is 64");

    let panic_result = catch_unwind(AssertUnwindSafe(|| panic!("boom")));
    match panic_result {
        Ok(_) => failed |= expect(false, "panic should unwind"),
        Err(payload) => {
            let mut failure = capture_panic_payload(payload);
            failed |= expect(failure.producer_tag == DCI_FAILURE_TAG_RUST_PANIC, "panic tag");
            let message = cstr(failure.message, failure.message_len).unwrap_or("");
            failed |= expect(message.contains("boom"), "panic message boom");
            dci_failure_destroy(&mut failure);
        }
    }

    let mut failure = capture_result_err::<i32>(7);
    failed |= expect(failure.producer_tag == DCI_FAILURE_TAG_RUST_RESULT, "result tag");
    failed |= expect(failure.payload_size == 4, "result payload size");
    failed |= expect(!failure.payload.is_null(), "result payload pointer");
    if !failure.payload.is_null() && failure.payload_size == 4 {
        let value = unsafe { *(failure.payload as *const i32) };
        failed |= expect(value == 7, "result payload 7");
    }
    dci_failure_destroy(&mut failure);

    let translated = translate_result(|| -> Result<i32, i32> { Err(7) });
    failed |= expect(translated.tag == 1, "translate_result err tag");
    failed |= expect(translated.err.producer_tag == DCI_FAILURE_TAG_RUST_RESULT, "nested result tag");
    let mut err = translated.err;
    dci_failure_destroy(&mut err);

    let translated_ok = translate_catch_unwind(|| 9i32);
    failed |= expect(translated_ok.tag == 0 && translated_ok.ok == 9, "translate_catch_unwind ok");

    std::process::exit(failed);
}
