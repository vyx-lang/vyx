// extern "C" / bindgen binding path runner.  Binds the hand-authored C facade of
// the shared C++ fixture library, verifies each entity by calling into it, and
// writes a per-entity JSON result document to $BENCH_OUT (or stdout).
//
// This path shows both what extern "C" recovers automatically (bindgen respects
// packing) and what it structurally cannot preserve (non-trivial value
// semantics, C++ exceptions, virtual dispatch), plus the classic silent-wrong
// layout drift of a hand-written natural-alignment mirror.
#![allow(non_upper_case_globals, non_camel_case_types, non_snake_case, dead_code)]

include!(concat!(env!("OUT_DIR"), "/bindings.rs"));

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

// A naive, hand-written mirror of the packed C++ record using natural
// alignment.  This is the common extern "C" mistake: without bindgen the
// developer transcribes the fields and silently gets the wrong layout.
#[repr(C)]
struct NaivePackedHeader {
    tag: u8,
    length: u32,
}

fn run_callbench(iters: u64) {
    use std::time::Instant;

    let warmup = iters.min(20_000);
    unsafe {
        let p = AbiCPoint { x: 3, y: 4 };
        for _ in 0..warmup {
            let _ = abi_c_point_sum(p);
        }
        let mut sink = 0u64;
        let start = Instant::now();
        for _ in 0..iters {
            sink = sink.wrapping_add(abi_c_point_sum(p) as u32 as u64);
        }
        let ns = start.elapsed().as_nanos() as u64;
        println!(
            "bindgen,point_sum,{iters},{ns},{:.3},{sink}",
            ns as f64 / iters as f64
        );

        let buf = abi_c_buffer_create(1);
        for _ in 0..warmup {
            abi_c_buffer_fill(buf, 0xAB);
        }
        sink = 0;
        let start = Instant::now();
        for _ in 0..iters {
            abi_c_buffer_fill(buf, 0xAB);
            sink = sink.wrapping_add(1);
        }
        let ns = start.elapsed().as_nanos() as u64;
        println!(
            "bindgen,buffer_fill,{iters},{ns},{:.3},{sink}",
            ns as f64 / iters as f64
        );

        abi_c_buffer_fill(buf, 0xAB);
        for _ in 0..warmup {
            let _ = abi_c_buffer_checksum(buf);
        }
        sink = 0;
        let start = Instant::now();
        for _ in 0..iters {
            sink ^= abi_c_buffer_checksum(buf);
        }
        let ns = start.elapsed().as_nanos() as u64;
        println!(
            "bindgen,buffer_checksum,{iters},{ns},{:.3},{sink}",
            ns as f64 / iters as f64
        );

        for _ in 0..warmup {
            let _ = abi_c_buffer_size(buf);
        }
        sink = 0;
        let start = Instant::now();
        for _ in 0..iters {
            sink = sink.wrapping_add(abi_c_buffer_size(buf) as u64);
        }
        let ns = start.elapsed().as_nanos() as u64;
        println!(
            "bindgen,buffer_size,{iters},{ns},{:.3},{sink}",
            ns as f64 / iters as f64
        );
        abi_c_buffer_destroy(buf);

        let shape = abi_c_make_circle(2.0);
        for _ in 0..warmup {
            let _ = abi_c_shape_area(shape);
        }
        sink = 0;
        let start = Instant::now();
        for _ in 0..iters {
            sink = sink.wrapping_add((abi_c_shape_area(shape) * 1000.0) as i64 as u64);
        }
        let ns = start.elapsed().as_nanos() as u64;
        println!(
            "bindgen,shape_area,{iters},{ns},{:.3},{sink}",
            ns as f64 / iters as f64
        );

        for _ in 0..warmup {
            let _ = abi_c_shape_sides(shape);
        }
        sink = 0;
        let start = Instant::now();
        for _ in 0..iters {
            sink = sink.wrapping_add(abi_c_shape_sides(shape) as u32 as u64);
        }
        let ns = start.elapsed().as_nanos() as u64;
        println!(
            "bindgen,shape_sides,{iters},{ns},{:.3},{sink}",
            ns as f64 / iters as f64
        );
        abi_c_shape_destroy(shape);
    }
}

fn main() {
    if let Ok(raw) = std::env::var("DCI_CALLBENCH_ITERS") {
        run_callbench(raw.parse().unwrap_or(2_000_000_000));
        return;
    }

    let mut records: Vec<Rec> = Vec::new();

    unsafe {
        let v = abi_c_point_sum(AbiCPoint { x: 3, y: 4 });
        records.push(rec(
            "point_sum",
            if v == 7 { "bound" } else { "silent_wrong" },
            format!("point_sum(3,4)={v} expected=7"),
        ));

        let t = abi_c_point_translate(AbiCPoint { x: 3, y: 4 }, 10, 20);
        records.push(rec(
            "point_translate",
            if t.x == 13 && t.y == 24 { "bound" } else { "silent_wrong" },
            format!("point_translate=({},{}) expected=(13,24)", t.x, t.y),
        ));

        let a = abi_c_rect_area(AbiCRect {
            origin: AbiCPoint { x: 1, y: 2 },
            w: 5,
            h: 6,
        });
        records.push(rec(
            "rect_area",
            if a == 30 { "bound" } else { "silent_wrong" },
            format!("rect_area=30? got {a}"),
        ));

        let d = abi_c_vec2_dot(AbiCVec2 { x: 1.5, y: 2.0 }, AbiCVec2 { x: 3.0, y: 4.0 });
        records.push(rec(
            "vec2_dot",
            if (d - 12.5).abs() < 1e-9 { "bound" } else { "silent_wrong" },
            format!("vec2_dot=12.5? got {d}"),
        ));

        let bytes: [u8; 4] = [1, 2, 3, 4];
        let cs = abi_c_span_checksum(AbiCSpan {
            data: bytes.as_ptr(),
            len: bytes.len() as u64,
        });
        records.push(rec(
            "span_checksum",
            if cs == fnv1a(&bytes) { "bound" } else { "silent_wrong" },
            format!("span_checksum matches FNV-1a? got {cs}"),
        ));

        let qs = abi_c_quad_sum(AbiCQuad {
            a: 1,
            b: 2,
            c: 3,
            d: 4,
        });
        records.push(rec(
            "quad_sum",
            if qs == 10 { "bound" } else { "silent_wrong" },
            format!("quad_sum=10? got {qs}"),
        ));

        let qsc = abi_c_quad_scale(
            AbiCQuad {
                a: 1,
                b: 2,
                c: 3,
                d: 4,
            },
            3,
        );
        records.push(rec(
            "quad_scale",
            if qsc.a == 3 && qsc.d == 12 { "bound" } else { "silent_wrong" },
            format!("quad_scale=(3..12)? got ({},{})", qsc.a, qsc.d),
        ));

        let c = abi_c_color_next(1);
        records.push(rec(
            "color_next",
            if c == 2 { "bound" } else { "silent_wrong" },
            format!("color_next(Green)=Blue? got {c}"),
        ));

        let st = abi_c_status_step(0);
        records.push(rec(
            "status_step",
            if st == 1 { "bound" } else { "silent_wrong" },
            format!("status_step(Ok)=Retry? got {st}"),
        ));

        // Packed layout: bindgen respects #pragma pack, so the generated struct
        // matches the C++ record; the naive hand mirror does not.
        let true_size = abi_c_true_packed_size();
        let bindgen_size = std::mem::size_of::<AbiCPackedHeader>() as u64;
        records.push(rec(
            "packed_layout",
            if bindgen_size == true_size { "bound" } else { "silent_wrong" },
            format!("bindgen packed size={bindgen_size} true={true_size}"),
        ));
        let naive_size = std::mem::size_of::<NaivePackedHeader>() as u64;
        records.push(rec(
            "packed_layout_naive",
            if naive_size == true_size { "bound" } else { "silent_wrong" },
            format!("hand-written natural-layout mirror size={naive_size} true={true_size}"),
        ));

        // Non-trivial value type: only expressible via an opaque handle; there
        // is no by-value C symbol.  The handle path works, but value semantics
        // are lost.
        let buf = abi_c_buffer_create(8);
        abi_c_buffer_fill(buf, 0xAB);
        let bcs = abi_c_buffer_checksum(buf);
        let bsz = abi_c_buffer_size(buf);
        abi_c_buffer_destroy(buf);
        let reference = [0xABu8; 8];
        records.push(rec(
            "buffer_handle",
            if bcs == fnv1a(&reference) && bsz == 8 { "bound" } else { "silent_wrong" },
            format!("opaque buffer(8) filled 0xAB checksum matches? got {bcs} size={bsz}"),
        ));
        records.push(rec(
            "buffer_value",
            "unsupported",
            "no by-value C symbol exists; non-trivial value semantics are not expressible in extern C".to_string(),
        ));

        // Polymorphic hierarchy: expressible only as opaque handle plus one C
        // function per virtual method; the dispatch table and RTTI are lost.
        let circle = abi_c_make_circle(2.0);
        let square = abi_c_make_square(3.0);
        let ok_virtual = (abi_c_shape_area(circle) - 12.566370614359172).abs() < 1e-9
            && abi_c_shape_sides(circle) == 0
            && (abi_c_shape_area(square) - 9.0).abs() < 1e-9
            && abi_c_shape_sides(square) == 4;
        abi_c_shape_destroy(circle);
        abi_c_shape_destroy(square);
        records.push(rec(
            "shape_virtual",
            if ok_virtual { "bound" } else { "silent_wrong" },
            "virtual dispatch via one C function per method".to_string(),
        ));

        // Exception: expressible only as an error-code wrapper; a native
        // exception crossing extern "C" would be undefined.
        let mut out_value: i32 = 0;
        let rc_ok = abi_c_checked_divide(10, 2, &mut out_value as *mut i32);
        let rc_err = abi_c_checked_divide(1, 0, std::ptr::null_mut());
        records.push(rec(
            "checked_divide",
            if rc_ok == 0 && out_value == 5 && rc_err == 1 { "bound" } else { "silent_wrong" },
            format!("divide(10,2) rc={rc_ok} out={out_value} divide(1,0) rc={rc_err}"),
        ));
    }

    let mut json = String::new();
    json.push_str("{\n  \"path\": \"bindgen\",\n  \"entities\": [\n");
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
