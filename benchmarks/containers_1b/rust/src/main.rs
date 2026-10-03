use std::collections::HashMap;
use std::env;
use std::hash::{BuildHasher, BuildHasherDefault, Hasher};
use std::time::Instant;

const MOD: u64 = 1_000_000_007;
const MASK31: u64 = 0x7fff_ffff;

struct Rng { state: u64 }
impl Rng {
    fn new(seed: u64) -> Self { let state = seed & MASK31; Self { state: if state == 0 { 1 } else { state } } }
    fn next(&mut self) -> u64 { self.state = self.state.wrapping_mul(1_103_515_245).wrapping_add(12_345) & MASK31; self.state }
}

fn splitmix(value: i64) -> u64 {
    let mut h = value as u64;
    h ^= ((h as i64) >> 30) as u64; h = h.wrapping_mul(0xbf58476d1ce4e5b9);
    h ^= ((h as i64) >> 27) as u64; h = h.wrapping_mul(0x94d049bb133111eb);
    h ^= ((h as i64) >> 31) as u64; h
}
#[derive(Default)] struct VyxHasher { hash: u64 }
impl Hasher for VyxHasher { fn finish(&self) -> u64 { self.hash } fn write(&mut self, bytes: &[u8]) { let mut raw = [0; 8]; raw[..bytes.len().min(8)].copy_from_slice(&bytes[..bytes.len().min(8)]); self.hash = splitmix(i64::from_ne_bytes(raw)); } fn write_i64(&mut self, value: i64) { self.hash = splitmix(value); } }
type DeterministicState = BuildHasherDefault<VyxHasher>;

#[derive(Clone)] struct Config { workload: u64, live_set: usize, string_bytes: usize, seed: u64, selected: String, profile: String, hash_policy: String, clock_rate: u64, reserve: bool }
impl Default for Config { fn default() -> Self { Self { workload: 1_000_000_000, live_set: 1_000_000, string_bytes: 1 << 20, seed: 0x5eed1234, selected: "all".into(), profile: "matched-o2".into(), hash_policy: "deterministic-splitmix64".into(), clock_rate: 1000, reserve: true } } }
fn parse() -> Config {
    let mut c = Config::default();
    for arg in env::args().skip(1) {
        if let Some(v) = arg.strip_prefix("--case=") { c.selected = v.into(); }
        else if let Some(v) = arg.strip_prefix("--profile=") { c.profile = v.into(); }
        else if let Some(v) = arg.strip_prefix("--hash-policy=") { c.hash_policy = v.into(); }
        else if let Some(v) = arg.strip_prefix("--clock-rate=") { c.clock_rate = v.parse().unwrap_or(c.clock_rate); }
        else if let Some(v) = arg.strip_prefix("--workload=") { c.workload = v.parse().unwrap_or(c.workload); }
        else if let Some(v) = arg.strip_prefix("--live-set=") { c.live_set = v.parse().unwrap_or(c.live_set); }
        else if let Some(v) = arg.strip_prefix("--string-bytes=") { c.string_bytes = v.parse().unwrap_or(c.string_bytes); }
        else if let Some(v) = arg.strip_prefix("--seed=") { c.seed = v.parse().unwrap_or(c.seed); }
        else if arg == "--no-reserve" { c.reserve = false; }
    }
    c.live_set = c.live_set.max(1); c.string_bytes = c.string_bytes.max(1); c
}
fn emit(bench: &str, c: &Config, setup: Duration, op: Duration, checksum: u64, size: usize) {
    let setup_ns = setup.as_nanos(); let op_ns = op.as_nanos(); let total_ns = setup_ns + op_ns;
    let setup_ticks = setup_ns * c.clock_rate as u128 / 1_000_000_000; let op_ticks = op_ns * c.clock_rate as u128 / 1_000_000_000;
    println!("language=rust bench={} workload={} live_set={} string_bytes={} seed={} reserve={} profile={} api_mode=safe hash_policy={} timing_mode=c_clock_cpu clock_rate={} setup_ticks={} op_ticks={} total_ticks={} setup_ns={} op_ns={} total_ns={} op_ops_per_sec={:.3} final_size={} checksum={} peak_rss_bytes=0",
        bench, c.workload, c.live_set, c.string_bytes, c.seed, if c.reserve { 1 } else { 0 }, c.profile, c.hash_policy, c.clock_rate, setup_ticks, op_ticks, setup_ticks + op_ticks, setup_ns, op_ns, total_ns, c.workload as f64 / op.as_secs_f64(), size, checksum);
}
use std::time::Duration;
fn vec_bench(c: &Config) { let mut r = Rng::new(c.seed); let t = Instant::now(); let mut v = if c.reserve { Vec::with_capacity(c.live_set) } else { Vec::new() }; for _ in 0..c.live_set.min(c.workload.max(1) as usize) { v.push(r.next() as i64); } let setup = t.elapsed(); let t = Instant::now(); let mut sum = 0; for _ in 0..c.workload { let a = r.next() % 100; let i = r.next(); let x = r.next(); if a < 35 && v.len() < c.live_set { v.push(x as i64); } else if a < 55 && v.len() > 1 { sum = (sum + v.pop().unwrap() as u64) % MOD; } else if a < 80 && !v.is_empty() { let j = i as usize % v.len(); sum = (sum + v[j] as u64 + j as u64) % MOD; } else if !v.is_empty() { let j = i as usize % v.len(); v[j] = x as i64; sum = (sum ^ x) % MOD; } } let op = t.elapsed(); if let (Some(a), Some(b)) = (v.first(), v.last()) { sum = (sum + *a as u64 + *b as u64) % MOD; } emit("vec", c, setup, op, sum, v.len()); }
fn dict_bench<S: BuildHasher>(c: &Config, state: S) { let mut r = Rng::new(c.seed); let t = Instant::now(); let mut m: HashMap<i64, i64, S> = HashMap::with_hasher(state); if c.reserve { m.reserve(c.live_set); } let key_space = c.live_set as u64; for _ in 0..c.live_set.min(c.workload.max(1) as usize) { m.insert((r.next() % key_space) as i64, r.next() as i64); } let setup = t.elapsed(); let t = Instant::now(); let mut sum = 0; for _ in 0..c.workload { let a = r.next() % 100; let k = (r.next() % key_space) as i64; let x = r.next() as i64; if a < 45 { m.insert(k, x); } else if a < 75 { if let Some(v) = m.get(&k) { sum = (sum + *v as u64) % MOD; } } else if a < 92 { sum = (sum + m.remove(&k).is_some() as u64) % MOD; } else { sum = (sum ^ m.contains_key(&k) as u64) % MOD; } } emit("dict", c, setup, t.elapsed(), sum, m.len()); }
fn string_bench(c: &Config) { let mut r = Rng::new(c.seed); let t = Instant::now(); let mut s = if c.reserve { String::with_capacity(c.string_bytes) } else { String::new() }; let initial = (c.string_bytes / 2 + usize::from(c.string_bytes == 1)).min(c.workload.max(1) as usize); for _ in 0..initial { s.push((32 + r.next() % 95) as u8 as char); } let setup = t.elapsed(); let t = Instant::now(); let mut sum = 0; for _ in 0..c.workload { let a = r.next() % 100; let i = r.next(); let x = r.next(); if a < 60 { if s.len() < c.string_bytes { s.push((32 + x % 95) as u8 as char); } else { s.clear(); } } else if a < 85 && !s.is_empty() { let j = i as usize % s.len(); sum = (sum + s.as_bytes()[j] as u64 + j as u64) % MOD; } else { s.clear(); } } if !s.is_empty() { sum = (sum + s.as_bytes()[0] as u64 + *s.as_bytes().last().unwrap() as u64) % MOD; } emit("string", c, setup, t.elapsed(), sum, s.len()); }
fn main() { let c = parse(); if c.selected == "all" || c.selected == "vec" { vec_bench(&c); } if c.selected == "all" || c.selected == "dict" { dict_bench(&c, DeterministicState::default()); } if c.selected == "all" || c.selected == "string" { string_bench(&c); } }
