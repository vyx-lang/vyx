"""SemVer grammar/precedence oracle using Python regex, integers and tuple keys."""
import hashlib
import json
from pathlib import Path
import random
import re

NUMBER = r"(?:0|[1-9][0-9]*)"
IDENTIFIER = r"[0-9A-Za-z-]+"
VERSION = re.compile(rf"({NUMBER})\.({NUMBER})\.({NUMBER})(?:-({IDENTIFIER}(?:\.{IDENTIFIER})*))?(?:\+({IDENTIFIER}(?:\.{IDENTIFIER})*))?")


def precedence(text):
    match = VERSION.fullmatch(text)
    if match is None:
        raise ValueError(text)
    major, minor, patch, prerelease, _ = match.groups()
    core = tuple(map(int, (major, minor, patch)))
    if max(core) > 2147483647:
        raise ValueError(text)
    identifiers = []
    if prerelease is not None:
        for identifier in prerelease.split("."):
            if identifier.isascii() and identifier.isdigit():
                if len(identifier) > 1 and identifier.startswith("0"):
                    raise ValueError(text)
                identifiers.append((0, int(identifier)))
            else:
                identifiers.append((1, identifier))
    return (*core, prerelease is None, tuple(identifiers))


def write_project(directory: Path, seed: int = 572163, pairs: int = 192) -> dict:
    directory.mkdir(parents=True, exist_ok=True)
    rng = random.Random(seed)
    valid = ["1.0.0" + suffix for suffix in ("-alpha", "-alpha.1", "-alpha.beta", "-beta", "-beta.2", "-beta.11", "-rc.1", "")]
    valid += ["0.0.0", "2147483647.2147483647.2147483647", "1.0.0+linux", "1.0.0+windows"]
    identifiers = ("0", "1", "9", "10", "9999999999999999999999999999999999999999", "alpha", "beta", "rc-2", "A", "a", "--", "01a")
    while len(valid) < 80:
        core = ".".join(str(rng.choice((0, 1, 2, 9, 10, 2147483647))) for _ in range(3))
        pre = "-" + ".".join(rng.choices(identifiers, k=rng.randint(1, 4))) if rng.randrange(4) else ""
        build = "+" + rng.choice(("001", "sha.123", "linux-x64", "windows", "a.0.00")) if rng.randrange(2) else ""
        valid.append(core + pre + build)
    invalid = ["", "1", "1.2", "1.2.3.4", "1..3", ".1.2", "1.2.", "v1.2.3", "V1.2.3", "01.2.3", "1.02.3", "1.2.03",
               "2147483648.0.0", "0.2147483648.0", "0.0.2147483648", "-1.2.3", "1.2.3-", "1.2.3+", "1.2.3-01", "1.2.3-a.00",
               "1.2.3-a..b", "1.2.3+a..b", "1.2.3-a.", "1.2.3+a.", "1.2.3-a_b", "1.2.3+a_b", "1.2.3+a+b", "1.2.3-a\0b", "1.2.3-中"]
    invalid += [text + char for text in rng.choices(valid, k=48) for char in (" ", "\n", "\0", "_")]
    for text in invalid:
        try:
            precedence(text)
        except ValueError:
            continue
        raise AssertionError(f"Invalid oracle input is valid: {text!r}")
    comparisons = [(valid[i], valid[i + 1]) for i in range(7)]
    comparisons += [(valid[10], valid[11]), (valid[11], valid[10])]
    comparisons += [(rng.choice(valid), rng.choice(valid)) for _ in range(pairs - len(comparisons))]

    def literal(text):
        return json.dumps(text, ensure_ascii=False).replace("\\u0000", "\\0")

    calls = [f"check_parse({literal(text)}, true);" for text in valid]
    calls += [f"check_parse({literal(text)}, false);" for text in invalid]
    for left, right in comparisons:
        a, b = precedence(left), precedence(right)
        expected = (a > b) - (a < b)
        calls.append(f"check_pair({literal(left)}, {literal(right)}, {expected});")
    lines = ['use std.core;', 'use std.string;', 'use std.semver;', 'use std.hash;', '''
fn check_parse(text: string, accepted: bool) {
    match (SemVer.try_parse(text)) {
        Ok(version) => { assert(accepted && version.toString() == text, "SemVer model acceptance/roundtrip"); }
        Err(failure) => {
            assert(!accepted, "SemVer model rejected a valid version");
            assert(failure.offset >= 0 && failure.offset <= text.len && failure.message.len > 0, "SemVer model located error");
        }
    }
}
fn check_pair(a: string, b: string, expected: i32) {
    let left = SemVer.parse(a);
    let right = SemVer.parse(b);
    assert(left.compareTo(right) == expected && right.compareTo(left) == -expected, "SemVer model precedence");
    assert(left.equals(right) == (expected == 0), "SemVer model equality");
    if (expected == 0) { assert(left.hash() == right.hash(), "SemVer model hash agreement"); }
    assert(left.toString() == a && right.toString() == b, "SemVer model preserves inputs");
}
''']
    functions = []
    for start in range(0, len(calls), 16):
        name = "model_" + str(start // 16)
        functions.append(name)
        lines += ["fn " + name + "() {", *("    " + call for call in calls[start:start + 16]), "}"]
    lines += ["fn main() -> i32 {", *("    " + name + "();" for name in functions), '    print("std semver model ok");', "    return 0;", "}"]
    source = directory / "main.vyx"
    source.write_text("\n".join(lines) + "\n", encoding="utf-8")
    (directory / "Vyx.toml").write_text('''[package]
name = "std_semver_model"
entry = "main.vyx"
[build]
cache_dir = ".cache"
output_dir = "target"
[dependencies]
std = { path = "std:std", target = "std" }
[target.std_semver_model]
type = "executable"
entry = "main.vyx"
auto_sources = false
''', encoding="utf-8")
    return {"seed": seed, "valid": len(valid), "invalid": len(invalid), "comparisons": len(comparisons),
            "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
            "generator_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
