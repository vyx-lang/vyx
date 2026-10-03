#![allow(improper_ctypes_definitions)]

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Point {
    pub x: i64,
    pub y: i64,
}

#[repr(transparent)]
#[derive(Clone, Copy)]
pub struct Status(pub u32);

#[repr(u8)]
#[derive(Clone, Copy)]
pub enum Axis {
    Horizontal = 0,
    Vertical = 1,
}

/* dci-ownership
{
  "dci_point_translate(*mut Point, i64, i64)": {
    "parameters": { "0": "borrow_mut" },
    "return": "copy"
  }
}
dci-ownership-end */

#[unsafe(no_mangle)]
pub extern "C" fn dci_point_make(x: i64, y: i64) -> Point {
    Point { x, y }
}

#[unsafe(no_mangle)]
pub extern "system" fn dci_point_component(point: Point, axis: Axis) -> i64 {
    match axis {
        Axis::Horizontal => point.x,
        Axis::Vertical => point.y,
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dci_point_translate(point: *mut Point, dx: i64, dy: i64) -> Status {
    let Some(point) = (unsafe { point.as_mut() }) else {
        return Status(1);
    };
    point.x += dx;
    point.y += dy;
    Status(0)
}

// This declaration must be visible in rejected_symbols and must never be
// emitted as an executable DCI symbol: data-carrying enums have no union contract.
pub enum Payload {
    Empty,
    One(u32),
}

#[unsafe(no_mangle)]
pub extern "C" fn dci_payload_not_exported(value: Payload) -> u32 {
    match value {
        Payload::Empty => 0,
        Payload::One(x) => x,
    }
}
