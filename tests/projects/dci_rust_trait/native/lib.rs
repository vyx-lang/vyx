#[allow(dead_code)]
pub struct NativePair {
    pub flag: u8,
    pub wide: u64,
    pub mid: u32,
}

impl NativePair {
    pub fn flag_of(&self) -> u8 {
        self.flag
    }
}

pub trait Sink {
    fn consume(&self, x: i32) -> i32;
}

/// A *generic* trait.  The Vyx side inherits it as a **closed instance**
/// (`class VyxHostG : native.SinkG<i32>`), because that is the only form the
/// contract can carry: the open `SinkG` has no vtable, so the producer has
/// nothing to hand the reverse-override stub.  Closing the instance is what
/// `--export-instance 'SinkG<i32>'` asks the adapter for.
pub trait SinkG<T> {
    fn consume(&self, x: T) -> T;
}

/// The Vyx side declares this record itself (`@[repr(C)] struct VyxBox`); the
/// producer never names it in the trait declaration.  Only the *layout* has to
/// agree, which is what `SinkG<VyxBox>` is instantiated over.  12 bytes /
/// align 4: an *indirect* parameter and *sret* return under the Win64 C rules,
/// i.e. the aggregate-passing paths that scalars never exercise.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct VyxBox {
    pub lo: u32,
    pub mid: u32,
    pub hi: u32,
}

const _: () = assert!(core::mem::size_of::<VyxBox>() == 12);
const _: () = assert!(core::mem::align_of::<VyxBox>() == 4);

pub struct NativeDriver {
    pub bias: i32,
}

impl NativeDriver {
    pub fn dispatch(&self, sink: &dyn Sink, x: i32) -> i32 {
        sink.consume(x) + self.bias
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn native_pair_heap() -> *mut NativePair {
    Box::into_raw(Box::new(NativePair { flag: 9, wide: 1, mid: 2 }))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn native_pair_free(p: *mut NativePair) {
    unsafe {
        if !p.is_null() {
            drop(Box::from_raw(p));
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn native_driver_new(bias: i32) -> *mut NativeDriver {
    Box::into_raw(Box::new(NativeDriver { bias }))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn native_driver_free(p: *mut NativeDriver) {
    unsafe {
        if !p.is_null() {
            drop(Box::from_raw(p));
        }
    }
}

/// Rebuild `&dyn Sink` from a reverse-override object whose first word is the
/// rustc dyn vtable pointer stored by the generated factory.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn native_driver_dispatch(
    drv: *const NativeDriver,
    sink_obj: *const (),
    x: i32,
) -> i32 {
    unsafe {
        let vtable = *(sink_obj as *const *const ());
        let fat = [sink_obj as *const (), vtable];
        let sink: &dyn Sink = core::mem::transmute_copy(&fat);
        (*drv).dispatch(sink, x)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn native_dispatch(sink_obj: *const (), x: i32) -> i32 {
    unsafe {
        let vtable = *(sink_obj as *const *const ());
        let fat = [sink_obj as *const (), vtable];
        let sink: &dyn Sink = core::mem::transmute_copy(&fat);
        sink.consume(x) + 30
    }
}

/// Same boundary as `native_dispatch`, but through the generic trait's closed
/// instance `SinkG<i32>`.  A Vyx subclass of `native.SinkG<i32>` must land in
/// the Vyx override through this dispatch.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn native_dispatch_generic(sink_obj: *const (), x: i32) -> i32 {
    unsafe {
        let vtable = *(sink_obj as *const *const ());
        let fat = [sink_obj as *const (), vtable];
        let sink: &dyn SinkG<i32> = core::mem::transmute_copy(&fat);
        sink.consume(x) + 30
    }
}

/// Rebuild a `&dyn SinkG<T>` and call through it, adding the same `+30` bias
/// the non-generic dispatch uses.  One instance per type argument: proving the
/// Vyx side really materializes per closed instance means a *different* `T`
/// must reach a *different* vtable slot, not the `i32` one again.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn native_dispatch_generic_i64(sink_obj: *const (), x: i64) -> i64 {
    unsafe {
        let vtable = *(sink_obj as *const *const ());
        let fat = [sink_obj as *const (), vtable];
        let sink: &dyn SinkG<i64> = core::mem::transmute_copy(&fat);
        sink.consume(x) + 30
    }
}

/// `char` is a 4-byte Unicode scalar on both sides of the boundary (Vyx spells
/// it `char32_t` when lowering to C), so the instance closes at the same width.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn native_dispatch_generic_char(sink_obj: *const (), x: char) -> char {
    unsafe {
        let vtable = *(sink_obj as *const *const ());
        let fat = [sink_obj as *const (), vtable];
        let sink: &dyn SinkG<char> = core::mem::transmute_copy(&fat);
        let biased = (sink.consume(x) as u32).wrapping_add(30);
        char::from_u32(biased).unwrap_or('\u{0}')
    }
}

/// Closed over a consumer-declared record: the same rebuild-the-fat-pointer
/// trick, but the instance's type argument is a struct neither the trait
/// declaration nor the import ever mentions.  `mid` must survive the round
/// trip untouched and `hi` must come back biased -- a swapped field offset
/// reads as a wrong value here, not as a type error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn native_dispatch_generic_box(sink_obj: *const (), x: VyxBox) -> VyxBox {
    unsafe {
        let vtable = *(sink_obj as *const *const ());
        let fat = [sink_obj as *const (), vtable];
        let sink: &dyn SinkG<VyxBox> = core::mem::transmute_copy(&fat);
        let out = sink.consume(x);
        VyxBox { lo: out.lo, mid: out.mid, hi: out.hi.wrapping_add(30) }
    }
}
