use std::env;

const C0: u64 = 6_364_136_223_846_793_005;
const C1: u64 = 1_442_695_040_888_963_407;
const C2: u64 = 7_046_029_254_386_353_131;
const C3: u64 = 3_202_034_522_624_059_733;
const C4: u64 = 3_935_559_000_370_003_845;
const C5: u64 = 2_691_343_689_449_507_681;
const C6: u64 = 4_768_777_513_237_032_717;
const C7: u64 = 2_405_875_930_906_139_467;
const ACCEPT_DIGEST: u64 = 6_317_825_462_749_680_818;

#[inline(always)]
fn hex_nibble(byte: u8) -> Option<u8> {
    match byte {
        b'0'..=b'9' => Some(byte - b'0'),
        b'a'..=b'f' => Some(byte - b'a' + 10),
        b'A'..=b'F' => Some(byte - b'A' + 10),
        _ => None,
    }
}

#[inline(never)]
fn parse_hex(input: &[u8], output: &mut [u8; 32]) -> bool {
    if input.len() != output.len() * 2 {
        return false;
    }
    for (index, slot) in output.iter_mut().enumerate() {
        let Some(high) = hex_nibble(input[index * 2]) else {
            return false;
        };
        let Some(low) = hex_nibble(input[index * 2 + 1]) else {
            return false;
        };
        *slot = (high << 4) | low;
    }
    true
}

#[inline(never)]
fn arx_mix(data: &[u8], seed: u64) -> u64 {
    let mut state = seed ^ C2;
    for (index, &byte) in data.iter().enumerate() {
        let keyed = (byte as u64).wrapping_add(((index + 1) * 257) as u64);
        state ^= keyed.wrapping_mul(C0);
        state = state.rotate_left(((index * 7 + 13) % 63 + 1) as u32);
        state = state.wrapping_mul(C3).wrapping_add(C1);
    }
    state ^ state.rotate_left(17)
}

#[inline(never)]
fn state_walk(data: &[u8], seed: u64) -> u64 {
    let mut state = seed ^ C4;
    let mut score = C5;
    for (index, &input) in data.iter().enumerate() {
        let byte = input as u64;
        state = match (state ^ byte ^ index as u64) & 3 {
            0 => state.wrapping_add(byte).wrapping_add(C1).rotate_left(5),
            1 => (state ^ byte.wrapping_mul(C2))
                .rotate_left(11)
                .wrapping_add(C3),
            2 => state
                .wrapping_mul(C0)
                ^ byte.wrapping_add(C4).rotate_left(17),
            _ => state
                .wrapping_add(byte << ((index & 7) + 1))
                .rotate_left(23)
                ^ C6,
        };
        score = (score ^ state ^ byte.wrapping_mul((index + 1) as u64)).rotate_left(9);
        score = score.wrapping_mul(C7).wrapping_add(C2);
    }
    score ^ state.rotate_left(29)
}

#[inline(always)]
fn read_u64_le(data: &[u8]) -> u64 {
    let mut word = 0_u64;
    for (index, &byte) in data.iter().enumerate() {
        word |= (byte as u64) << (index * 8);
    }
    word
}

#[inline(never)]
fn verify_token(bytes: &[u8; 32]) -> u64 {
    let left = arx_mix(bytes, C1);
    let right = state_walk(bytes, left ^ C6);
    let mut fold = C5;
    for block in 0..4 {
        let offset = block * 8;
        let word = read_u64_le(&bytes[offset..offset + 8]);
        fold ^= word.wrapping_add(left).rotate_left((block * 11 + 3) as u32);
        fold = fold.wrapping_mul(C0).wrapping_add(C3);
    }
    (fold ^ right)
        .rotate_left(27)
        .wrapping_add(left ^ C4)
}

fn main() {
    let mut args = env::args();
    let _program = args.next();
    let token = match (args.next(), args.next()) {
        (Some(value), None) => value,
        _ => {
            println!("INVALID");
            std::process::exit(2);
        }
    };

    let mut bytes = [0_u8; 32];
    if !parse_hex(token.as_bytes(), &mut bytes) {
        println!("INVALID");
        std::process::exit(2);
    }

    let digest = verify_token(&bytes);
    if digest == ACCEPT_DIGEST {
        println!("ACCEPT {digest}");
        return;
    }
    println!("REJECT {digest}");
    std::process::exit(1);
}
