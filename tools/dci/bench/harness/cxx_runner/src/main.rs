// cxx binding path runner.  Binds the cxx-expressible subset of the shared C++
// fixture library and verifies each entity by actually calling into C++, then
// writes a per-entity JSON result document to $BENCH_OUT (or stdout).
//
// Every entity that cxx can express is exercised here; the entities cxx cannot
// express (opaque-by-value, packed/bit-field layouts) are covered by the
// separate reject probes the harness generates.

#[cxx::bridge(namespace = "bench")]
mod ffi {
    #[derive(Clone, Copy)]
    struct Point {
        x: i32,
        y: i32,
    }
    struct Rect {
        origin: Point,
        w: i32,
        h: i32,
    }
    struct Vec2 {
        x: f64,
        y: f64,
    }
    struct Span {
        data: *const u8,
        len: u64,
    }
    struct Quad {
        a: i64,
        b: i64,
        c: i64,
        d: i64,
    }

    // A cxx shared-struct mirror of the packed C++ record.  cxx shared structs
    // have no packing syntax, so this necessarily takes the natural layout; the
    // runner records its size to show cxx cannot faithfully represent the packed
    // library type.
    struct PackedHeaderMirror {
        tag: u8,
        length: u32,
    }

    unsafe extern "C++" {
        include!("cxx_runner/src/bridge.h");

        #[namespace = "abi"]
        type Buffer;
        #[namespace = "abi"]
        type Shape;

        fn point_sum(p: Point) -> i32;
        fn point_translate(p: Point, dx: i32, dy: i32) -> Point;
        fn rect_area(r: Rect) -> i32;
        fn vec2_dot(a: Vec2, b: Vec2) -> f64;
        unsafe fn span_checksum(s: Span) -> u64;
        fn quad_sum(q: Quad) -> i64;
        fn quad_scale(q: Quad, k: i64) -> Quad;
        fn color_next(c: u8) -> u8;
        fn status_step(s: i32) -> i32;

        fn make_buffer(n: usize) -> UniquePtr<Buffer>;
        fn fill(self: Pin<&mut Buffer>, value: u8);
        fn checksum(self: &Buffer) -> u64;
        fn size(self: &Buffer) -> usize;

        fn make_circle(radius: f64) -> UniquePtr<Shape>;
        fn make_square(side: f64) -> UniquePtr<Shape>;
        fn area(self: &Shape) -> f64;
        fn sides(self: &Shape) -> i32;

        fn checked_divide(a: i32, b: i32) -> Result<i32>;
    }
}

use std::fmt::Write as _;

struct Rec {
    id: &'static str,
    outcome: &'static str,
    detail: String,
}

fn rec(id: &'static str, outcome: &'static str, detail: String) -> Rec {
    Rec { id, outcome, detail }
}

fn fnv1a(bytes: &[u8]) -> u64 {
    let mut acc: u64 = 1469598103934665603;
    for b in bytes {
        acc ^= *b as u64;
        acc = acc.wrapping_mul(1099511628211);
    }
    acc
}

fn run_callbench(iters: u64) {
    use std::time::Instant;

    let warmup = iters.min(20_000);
    let p = ffi::Point { x: 3, y: 4 };
    for _ in 0..warmup {
        let _ = ffi::point_sum(p);
    }
    let mut sink = 0u64;
    let start = Instant::now();
    for _ in 0..iters {
        sink = sink.wrapping_add(ffi::point_sum(p) as u32 as u64);
    }
    let ns = start.elapsed().as_nanos() as u64;
    println!(
        "cxx,point_sum,{iters},{ns},{:.3},{sink}",
        ns as f64 / iters as f64
    );

    let mut buf = ffi::make_buffer(1);
    for _ in 0..warmup {
        buf.pin_mut().fill(0xAB);
    }
    sink = 0;
    let start = Instant::now();
    for _ in 0..iters {
        buf.pin_mut().fill(0xAB);
        sink = sink.wrapping_add(1);
    }
    let ns = start.elapsed().as_nanos() as u64;
    println!(
        "cxx,buffer_fill,{iters},{ns},{:.3},{sink}",
        ns as f64 / iters as f64
    );

    buf.pin_mut().fill(0xAB);
    for _ in 0..warmup {
        let _ = buf.checksum();
    }
    sink = 0;
    let start = Instant::now();
    for _ in 0..iters {
        sink ^= buf.checksum();
    }
    let ns = start.elapsed().as_nanos() as u64;
    println!(
        "cxx,buffer_checksum,{iters},{ns},{:.3},{sink}",
        ns as f64 / iters as f64
    );

    for _ in 0..warmup {
        let _ = buf.size();
    }
    sink = 0;
    let start = Instant::now();
    for _ in 0..iters {
        sink = sink.wrapping_add(buf.size() as u64);
    }
    let ns = start.elapsed().as_nanos() as u64;
    println!(
        "cxx,buffer_size,{iters},{ns},{:.3},{sink}",
        ns as f64 / iters as f64
    );

    let shape = ffi::make_circle(2.0);
    for _ in 0..warmup {
        let _ = shape.area();
    }
    sink = 0;
    let start = Instant::now();
    for _ in 0..iters {
        sink = sink.wrapping_add((shape.area() * 1000.0) as i64 as u64);
    }
    let ns = start.elapsed().as_nanos() as u64;
    println!(
        "cxx,shape_area,{iters},{ns},{:.3},{sink}",
        ns as f64 / iters as f64
    );

    for _ in 0..warmup {
        let _ = shape.sides();
    }
    sink = 0;
    let start = Instant::now();
    for _ in 0..iters {
        sink = sink.wrapping_add(shape.sides() as u32 as u64);
    }
    let ns = start.elapsed().as_nanos() as u64;
    println!(
        "cxx,shape_sides,{iters},{ns},{:.3},{sink}",
        ns as f64 / iters as f64
    );
}

fn main() {
    if let Ok(raw) = std::env::var("DCI_CALLBENCH_ITERS") {
        run_callbench(raw.parse().unwrap_or(2_000_000_000));
        return;
    }

    let mut records: Vec<Rec> = Vec::new();

    // POD register-class aggregate.
    let v = ffi::point_sum(ffi::Point { x: 3, y: 4 });
    records.push(rec(
        "point_sum",
        if v == 7 { "bound" } else { "silent_wrong" },
        format!("point_sum(3,4)={v} expected=7"),
    ));

    // Aggregate return by value.
    let t = ffi::point_translate(ffi::Point { x: 3, y: 4 }, 10, 20);
    records.push(rec(
        "point_translate",
        if t.x == 13 && t.y == 24 { "bound" } else { "silent_wrong" },
        format!("point_translate=({},{}) expected=(13,24)", t.x, t.y),
    ));

    // Nested aggregate.
    let a = ffi::rect_area(ffi::Rect { origin: ffi::Point { x: 1, y: 2 }, w: 5, h: 6 });
    records.push(rec(
        "rect_area",
        if a == 30 { "bound" } else { "silent_wrong" },
        format!("rect_area=30? got {a}"),
    ));

    // SSE aggregate by value (DCI fail-closes on this; cxx handles it via glue).
    let d = ffi::vec2_dot(ffi::Vec2 { x: 1.5, y: 2.0 }, ffi::Vec2 { x: 3.0, y: 4.0 });
    records.push(rec(
        "vec2_dot",
        if (d - 12.5).abs() < 1e-9 { "bound" } else { "silent_wrong" },
        format!("vec2_dot=12.5? got {d}"),
    ));

    // Pointer+integer aggregate.
    let bytes: [u8; 4] = [1, 2, 3, 4];
    let cs = unsafe {
        ffi::span_checksum(ffi::Span {
            data: bytes.as_ptr(),
            len: bytes.len() as u64,
        })
    };
    records.push(rec(
        "span_checksum",
        if cs == fnv1a(&bytes) { "bound" } else { "silent_wrong" },
        format!("span_checksum matches FNV-1a? got {cs}"),
    ));

    // Memory-class aggregate by value (DCI fail-closes; cxx handles via glue).
    let qs = ffi::quad_sum(ffi::Quad { a: 1, b: 2, c: 3, d: 4 });
    records.push(rec(
        "quad_sum",
        if qs == 10 { "bound" } else { "silent_wrong" },
        format!("quad_sum=10? got {qs}"),
    ));

    // Memory-class aggregate returned by value.
    let qsc = ffi::quad_scale(ffi::Quad { a: 1, b: 2, c: 3, d: 4 }, 3);
    records.push(rec(
        "quad_scale",
        if qsc.a == 3 && qsc.d == 12 { "bound" } else { "silent_wrong" },
        format!("quad_scale=(3..12)? got ({},{})", qsc.a, qsc.d),
    ));

    // Enums.
    let c = ffi::color_next(1);
    records.push(rec(
        "color_next",
        if c == 2 { "bound" } else { "silent_wrong" },
        format!("color_next(Green)=Blue? got {c}"),
    ));
    let st = ffi::status_step(0);
    records.push(rec(
        "status_step",
        if st == 1 { "bound" } else { "silent_wrong" },
        format!("status_step(Ok)=Retry? got {st}"),
    ));

    // Non-trivial value type via opaque handle + methods.
    let mut buf = ffi::make_buffer(8);
    buf.pin_mut().fill(0xAB);
    let bcs = buf.checksum();
    let bsz = buf.size();
    let mut reference = [0u8; 8];
    for b in reference.iter_mut() {
        *b = 0xAB;
    }
    records.push(rec(
        "buffer_handle",
        if bcs == fnv1a(&reference) && bsz == 8 { "bound" } else { "silent_wrong" },
        format!("buffer(8) filled 0xAB checksum matches? got {bcs} size={bsz}"),
    ));

    // Polymorphic hierarchy via opaque handle + virtual methods.
    let circle = ffi::make_circle(2.0);
    let square = ffi::make_square(3.0);
    let ok_virtual = (circle.area() - 12.566370614359172).abs() < 1e-9
        && circle.sides() == 0
        && (square.area() - 9.0).abs() < 1e-9
        && square.sides() == 4;
    records.push(rec(
        "shape_virtual",
        if ok_virtual { "bound" } else { "silent_wrong" },
        format!(
            "circle.area={:.4} circle.sides={} square.area={:.4} square.sides={}",
            circle.area(),
            circle.sides(),
            square.area(),
            square.sides()
        ),
    ));

    // Throwing function translated into a Rust Result.
    let ok = ffi::checked_divide(10, 2);
    let err = ffi::checked_divide(1, 0);
    let ok_value = matches!(ok, Ok(5));
    records.push(rec(
        "checked_divide",
        if ok_value && err.is_err() { "bound" } else { "silent_wrong" },
        format!(
            "divide(10,2) ok_is_5={ok_value} divide(1,0) is_err={}",
            err.is_err()
        ),
    ));

    // Packed layout: cxx shared struct cannot express packing.
    let mirror_size = std::mem::size_of::<ffi::PackedHeaderMirror>();
    records.push(rec(
        "packed_layout",
        if mirror_size == 5 { "bound" } else { "mismatch_layout" },
        format!("cxx shared-struct PackedHeaderMirror size={mirror_size} true=5"),
    ));

    let mut json = String::new();
    json.push_str("{\n  \"path\": \"cxx\",\n  \"entities\": [\n");
    for (i, r) in records.iter().enumerate() {
        let comma = if i + 1 < records.len() { "," } else { "" };
        let _ = write!(
            json,
            "    {{\"id\": \"{}\", \"outcome\": \"{}\", \"detail\": {}}}{}\n",
            r.id,
            r.outcome,
            json_string(&r.detail),
            comma
        );
    }
    json.push_str("  ]\n}\n");

    match std::env::var("BENCH_OUT") {
        Ok(path) => std::fs::write(&path, json).expect("write BENCH_OUT"),
        Err(_) => print!("{json}"),
    }
    // Non-bound outcomes are expected for the layout gap; the process still
    // exits 0 so the harness reads the structured result rather than a status.
}

fn json_string(s: &str) -> String {
    let mut out = String::from("\"");
    for ch in s.chars() {
        match ch {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            _ => out.push(ch),
        }
    }
    out.push('"');
    out
}
