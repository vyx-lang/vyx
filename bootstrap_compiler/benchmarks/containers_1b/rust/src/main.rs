use std::collections::HashMap;
use std::time::Instant;

const N_PUSH: usize = 100_000_000; // 1e8 vec push/pop
const N_GET: usize = 100_000_000; // 1e8 vec get/set
const N_SORT: usize = 10_000_000; // 1e7 elements sorted once
const N_MAP_PUT: usize = 10_000_000; // 1e7 hashmap put (i64 keys)
const N_MAP_GET: usize = 10_000_000; // 1e7 hashmap get hits
const N_STR_APPEND: usize = 2_000_000; // 2e6 string appends (~16B each)
const N_UPPER: usize = 200_000; // 200k to_upper over 64-char strings

fn now_ms(t: Instant) -> f64 {
    t.elapsed().as_secs_f64() * 1000.0
}

fn bench_vec_push_pop() -> f64 {
    let t = Instant::now();
    let mut v: Vec<i64> = Vec::with_capacity(N_PUSH);
    for i in 0..N_PUSH as i64 {
        v.push(i);
    }
    let mut sum: i64 = 0;
    for _ in 0..N_PUSH {
        sum ^= v.pop().unwrap();
    }
    assert_eq!(sum, expected_xor(N_PUSH as i64));
    now_ms(t)
}

fn expected_xor(n: i64) -> i64 {
    // xor of 0..n-1 pattern-dependent but we just need a checksum;
    // recompute by folding the same sequence.
    let mut acc: i64 = 0;
    for i in 0..n {
        acc ^= i;
    }
    acc
}

fn bench_vec_get_set() -> f64 {
    let mut v: Vec<i64> = vec![0i64; N_GET];
    for i in 0..N_GET {
        v[i] = i as i64;
    }
    let t = Instant::now();
    let mut sum: u64 = 0;
    for k in 0..N_GET as u64 {
        // stride walk to defeat trivial loop fusion on get-only
        let idx = ((k * 2654435761) % N_GET as u64) as usize;
        sum = sum.wrapping_add(v[idx] as u64);
    }
    std::hint::black_box(sum);
    now_ms(t)
}

fn bench_vec_sort() -> f64 {
    let mut rng_state: u64 = 0x9E3779B97F4A7C15;
    let mut v: Vec<i64> = Vec::with_capacity(N_SORT);
    for _ in 0..N_SORT {
        rng_state ^= rng_state << 13;
        rng_state ^= rng_state >> 7;
        rng_state ^= rng_state << 17;
        v.push(rng_state as i64);
    }
    let t = Instant::now();
    v.sort_unstable();
    assert!(v[0] <= v[N_SORT - 1]);
    now_ms(t)
}

fn bench_map_put_get() -> (f64, f64) {
    let mut m: HashMap<i64, i64> = HashMap::with_capacity(N_MAP_PUT);
    let t1 = Instant::now();
    for i in 0..N_MAP_PUT as i64 {
        m.insert(i * 2654435761, i);
    }
    let put_ms = now_ms(t1);
    let t2 = Instant::now();
    let mut sink: i64 = 0;
    for i in 0..N_MAP_GET as i64 {
        sink ^= m[&(i * 2654435761)];
    }
    std::hint::black_box(sink);
    let get_ms = now_ms(t2);
    (put_ms, get_ms)
}

fn bench_string_append() -> f64 {
    let frag = "abcdefgh01234567"; // exactly 16 bytes
    let t = Instant::now();
    let mut s = String::with_capacity(1024);
    for _ in 0..N_STR_APPEND {
        s.push_str(frag);
    }
    assert_eq!(s.len(), N_STR_APPEND * 16);
    now_ms(t)
}

fn bench_string_upper() -> f64 {
    let src: String = "The quick brown fox jumps over the lazy dog 0123456789!".to_uppercase();
    let base = "the quick brown fox jumps over the lazy dog 0123456789!".repeat(64 / 56 + 1);
    let base = &base[..64];
    let mut sink: usize = 0;
    let t = Instant::now();
    for _ in 0..N_UPPER {
        let up = base.to_uppercase();
        sink += up.len();
    }
    std::hint::black_box(sink);
    let _ = src.len();
    now_ms(t)
}

fn main() {
    println!("=== Rust container baseline (release) ===");
    let ms = bench_vec_push_pop();
    println!("vec   push+pop x1e8      : {:>10.2} ms", ms);
    let ms = bench_vec_get_set();
    println!("vec   random get  x1e8    : {:>10.2} ms", ms);
    let ms = bench_vec_sort();
    println!("vec   sort_unstable x1e7  : {:>10.2} ms", ms);
    let (put_ms, get_ms) = bench_map_put_get();
    println!("map   put       x1e7      : {:>10.2} ms", put_ms);
    println!("map   get hit   x1e7      : {:>10.2} ms", get_ms);
    let ms = bench_string_append();
    println!("str   push_str x2e6(16B) : {:>10.2} ms", ms);
    let ms = bench_string_upper();
    println!("str   to_upper x2e5(64B) : {:>10.2} ms", ms);
}
