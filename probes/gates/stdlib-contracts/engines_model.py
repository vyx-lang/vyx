"""Differential public-API tests: Python re, uuid and tomllib are the oracles."""
from pathlib import Path
import datetime
import hashlib
import json
import random
import re
import tomllib
import uuid


def literal(text):
    # Rewrite an actual NUL, never the six literal bytes of a TOML \u0000
    # escape. A replacement after JSON encoding would corrupt that TOML input.
    return '"' + ''.join('\\0' if ch == '\0' else json.dumps(ch, ensure_ascii=False)[1:-1]
                         for ch in text) + '"'


def write_project(directory: Path, seed=520193):
    directory.mkdir(parents=True, exist_ok=True)
    rng = random.Random(seed)
    blocks = []
    patterns = [r'a*', r'a+?', r'(a|ab)+', r'^a.$', r'([a-z]+)-(\d+)', r'(x)?()',
                r'\b[a-z]+\b', r'(a)(b)\2\1', r'(?P<word>\w+)', r'a(?=b)',
                r'(?<=a)b', r'[^x]{1,3}', r'(?:ab|c){0,2}', r'\d{2,4}', r'.*', r'']
    texts = ['', 'abc-42', 'abba', 'abc\n', 'aaaa!', '中 文', 'abcc', ' 12 x3456', 'x\0a']
    matches = 0
    for pattern in patterns:
        for text in rng.sample(texts, 6):
            result = re.search(pattern, text)
            a = b = -1
            groups = ['', '', '']
            mask = 0
            count = 0
            if result is None:
                pass
            else:
                a = len(text[:result.start()].encode())
                b = len(text[:result.end()].encode())
                for index, group in enumerate((result.group(), *result.groups())):
                    count += 1
                    if group is not None:
                        groups[index] = group
                        mask |= 1 << index
            blocks.append([f'regex_check({literal(pattern)}, {literal(text)}, {a}, {b}, {literal(groups[0])}, {literal(groups[1])}, {literal(groups[2])}, {mask}, {count});'])
            matches += 1
    for _ in range(64):
        number = rng.getrandbits(128)
        reference = uuid.UUID(int=number)
        blocks.append([f'uuid_check({literal(str(reference).upper())}, {literal(str(reference))}, {number >> 64} as u64, {number & ((1 << 64) - 1)} as u64);'])

    documents = []
    for i in range(8):
        n = rng.randrange(-(1 << 63), 1 << 63)
        document = '\n'.join([
            f'title = {literal(rng.choice(["中文", "value # comma,", "", "a.b"]))}',
            f'number = {n}', f'hex = 0x{rng.getrandbits(32):x}',
            'float = 1_234.5e-1', 'enabled = true',
            'items = [1, "text", false, { deep = [2, 3] }]',
            '"literal.dot" = { "" = "empty key", "zero" = "a\\u0000b" }',
            'date = 2000-02-29', 'clock = 01:02:03.456789',
            'stamp = 2026-11-04T12:34:56+05:30',
            '[nested.table]', "literal = '''hello \\t world'''", 'escaped = "a\\t\\\"b"',
            '[[servers]]', 'port = 8080', '[[servers]]', 'port = 9090',
        ])
        documents.append(document)
        reference = tomllib.loads(document)
        lines = [f'let doc = Toml.parse({literal(document)}).unwrap();']
        def visit(value, expression):
            if isinstance(value, dict):
                lines.append(f'assert({expression}.size().unwrap() == {len(value)}, "TOML table size oracle");')
                for key, child in value.items():
                    visit(child, expression + f'.require({literal(key)}).unwrap()')
            elif isinstance(value, list):
                lines.append(f'assert({expression}.size().unwrap() == {len(value)}, "TOML array size oracle");')
                for index, child in enumerate(value):
                    visit(child, expression + f'.at({index}).unwrap()')
            elif isinstance(value, str):
                lines.append(f'assert({expression}.as_string().unwrap() == {literal(value)}, "TOML text oracle");')
            elif isinstance(value, bool):
                lines.append(f'assert({expression}.as_bool().unwrap() == {str(value).lower()}, "TOML bool oracle");')
            elif isinstance(value, int):
                lines.append(f'assert({expression}.as_i64().unwrap() == {value}, "TOML integer oracle");')
            elif isinstance(value, float):
                lines.append(f'assert({expression}.as_f64().unwrap() == {value!r}, "TOML float oracle");')
            else:
                stamp = expression + '.as_datetime().unwrap()'
                for field in ('year', 'month', 'day', 'hour', 'minute', 'second', 'microsecond'):
                    if hasattr(value, field):
                        lines.append(f'assert({stamp}.{field} == {getattr(value, field)}, "TOML datetime oracle");')
                if isinstance(value, datetime.datetime) and value.tzinfo:
                    lines.append(f'assert({stamp}.offset_minutes == {int(value.utcoffset().total_seconds() // 60)}, "TOML offset oracle");')
        visit(reference, 'doc')
        blocks.append(lines)
    invalid = ['x=01', 'x=1__2', 'x=0x_1', 'x=9223372036854775808', 'x=-9223372036854775809',
               'x=1\nx=2', '[a]\n[a]', 'x={a=1,a=2}', 'x=[1,,2]', 'x="\\uD800"',
               'x="unterminated', 'x=2023-02-29', 'x=24:00:00', 'x=true junk', 'a=1\na.b=2']
    for document in invalid:
        try:
            parsed = tomllib.loads(document)
        except tomllib.TOMLDecodeError:
            blocks.append([f'assert(Toml.parse({literal(document)}).is_err(), "TOML rejection oracle");'])
        else:
            # tomllib deliberately uses Python's arbitrary-precision integers;
            # TOML's required signed-64-bit domain is a separate check.
            if not (isinstance(parsed.get('x'), int) and not -(1 << 63) <= parsed['x'] < (1 << 63)):
                raise AssertionError('Invalid corpus item accepted by independent oracle: ' + document)
            blocks.append([f'assert(Toml.parse({literal(document)}).is_err(), "TOML signed-64-bit bound");'])
    sources = ['use std.core;', 'use std.string;', 'use std.regex;', 'use std.uuid;', 'use std.serde.toml;']
    sources += ['''
fn regex_check(pattern: string, text: string, a: i64, b: i64, g0: string, g1: string, g2: string, mask: i32, count: i64) {
    let compiled = Regex.compile(pattern).unwrap();
    let found = compiled.find(text).unwrap();
    if (a < 0) { assert(found.is_none(), "regex oracle no match"); return; }
    let hit = found.unwrap();
    assert(hit.start() == a && hit.end() == b, "regex oracle byte range");
    assert(hit.group_count() == count, "regex oracle group count");
    var i: i64 = 0;
    while (i < count) {
        let group = hit.group(i).unwrap();
        if ((mask & (1 << i)) == 0) { assert(group.is_none(), "regex oracle unmatched group"); }
        else {
            let expected = if (i == 0) { g0 } elif (i == 1) { g1 } else { g2 };
            assert(group.unwrap() == expected, "regex oracle capture");
        }
        i += 1;
    }
}
fn uuid_check(text: string, canonical: string, high: u64, low: u64) {
    let id = Uuid.try_parse(text).unwrap();
    assert(id.toString() == canonical, "UUID canonical oracle");
    let bytes = id.to_bytes();
    var i: i64 = 0;
    while (i < 16) {
        let word = if (i < 8) { high } else { low };
        assert(bytes.get(i) == ((word >> ((7 - i % 8) * 8)) & 255 as u64) as u8, "UUID network byte oracle");
        i += 1;
    }
    assert(Uuid.try_from_bytes(&bytes).unwrap() == id, "UUID oracle roundtrip");
}
''']
    # Bound function sizes independently of the number of generated cases.
    for i, block in enumerate(blocks):
        sources += [f'fn case_{i}() {{', *('    ' + line for line in block), '}']
    sources += ['fn main() -> i32 {', *(f'    case_{i}();' for i in range(len(blocks))),
                '    print("std engines model ok");', '    return 0;', '}']
    text = '\n'.join(sources) + '\n'
    (directory / 'main.vyx').write_text(text, encoding='utf-8')
    (directory / 'Vyx.toml').write_text('''[package]
name="engines_model"
entry="main.vyx"
[build]
output_dir="target"
[dependencies]
std={path="std:std",target="std"}
toml={path="std:serde",target="serde_toml"}
[target.engines_model]
type="executable"
entry="main.vyx"
sources=[]
auto_sources=false
''', encoding='utf-8')
    return dict(seed=seed, regex_cases=matches, uuid_cases=64, toml_documents=len(documents),
                toml_invalid=len(invalid), source_sha256=hashlib.sha256(text.encode()).hexdigest())
