// DCI Active Adapter phase 1 fixture: in-crate generics for the Rust endpoint.
pub fn pair<T>(a: T, b: T) -> (T, T) {
    (a, b)
}

// Closes for i32; rejected for Marker (no Add impl).
pub fn twice<T: std::ops::Add<Output = T> + Copy>(value: T) -> T {
    value + value
}

// Per-operation admission on the same producer entity type: Marker can be
// passed through size_of_val, but display_of refuses it (no Display impl).
pub fn display_of<T: std::fmt::Display>(value: T) -> usize {
    value.to_string().len()
}

pub fn size_of_val<T>(value: T) -> usize {
    std::mem::size_of_val(&value)
}

pub struct Marker;
