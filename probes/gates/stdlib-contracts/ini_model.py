"""INI lookup oracle using RawConfigParser and Python's checked integers."""
import configparser
import hashlib
import json
from pathlib import Path
import random
import re


def reference(text):
    parser = configparser.RawConfigParser(delimiters=("=",), strict=True, interpolation=None,
        comment_prefixes=("#", ";"), inline_comment_prefixes=None, default_section="\0")
    parser.optionxform = str
    # A synthetic section maps the portable root-key subset without depending
    # on Python 3.13's optional unnamed-section support.
    normalized = "\n".join(text.removeprefix("\ufeff").splitlines())
    parser.read_string("[__vyx_root__]\n" + normalized)
    return {(section if section != "__vyx_root__" else "", key): value
            for section in parser.sections() for key, value in parser.items(section, raw=True)}


def write_project(directory: Path, seed: int = 617283) -> dict:
    directory.mkdir(parents=True, exist_ok=True)
    rng = random.Random(seed)
    pool = ("", "plain", "中文 值", "https://example.org/a?x=1;tag=#literal", "100% %(raw)s", '"quoted value"',
            "a=b=c", "-9223372036854775808", "9223372036854775807", "+2147483647", "-2147483648", "00042", "-0",
            "9223372036854775808", "-9223372036854775809", "12oops34", "+", "1_000", "0x10", "YES", "oFf", "true", "0", "1")
    documents = []
    for _ in range(24):
        lines = ["# generated INI", "root = value", "root.empty =", ""]
        for section in ("a", "a.b", "A"):
            lines += ["; full-line comment", "[" + section + "]"]
            for index in range(6):
                lines.append("  key_" + str(index) + " = " + rng.choice(pool) + "\t ")
        newline = rng.choice(("\n", "\r\n", "\r"))
        text = newline.join(lines) + (newline if rng.randrange(2) else "")
        if rng.randrange(4) == 0:
            text = "\ufeff" + text
        documents.append(text)
    invalid = ["[s]\na=1\na=2", "[s]\n[s]", "[s\nx=1", "[s]\nnot-an-assignment"]

    def literal(text):
        return json.dumps(text, ensure_ascii=False).replace("\\u0000", "\\0")

    def integer_literal(value):
        return "(-9223372036854775807 - 1)" if value == -(1 << 63) else str(value)

    lines = ["use std.core;", "use std.string;", "use std.serde.ini;", '''
fn check(document: &IniDocument, section: string, key: string, expected: string, int_kind: i32, number: i64, bool_kind: i32) {
    let value = document.get(section, key).unwrap();
    assert(value == expected, "INI model raw value");
    match (document.try_get_i64(section, key)) {
        Ok(result) => { assert(int_kind > 0 && result.unwrap() == number, "INI model i64 value"); }
        Err(_) => { assert(int_kind == 0, "INI model rejected valid i64"); }
    }
    match (document.try_get_i32(section, key)) {
        Ok(result) => { assert(int_kind == 2 && result.unwrap() == number as i32, "INI model i32 value"); }
        Err(_) => { assert(int_kind != 2, "INI model rejected valid i32"); }
    }
    match (document.try_get_bool(section, key)) {
        Ok(result) => { assert(bool_kind >= 0 && result.unwrap() == (bool_kind == 1), "INI model boolean"); }
        Err(_) => { assert(bool_kind < 0, "INI model rejected valid boolean"); }
    }
}
fn large_document() {
    var header = StringBuilder.create();
    var h: i64 = 0;
    while (h < 4096) { header.appendChar(120); h += 1; }
    let header_owned = header.intoString();
    let section = header_owned.as_str();
    var source = StringBuilder.create();
    source.append("["); source.append(section); source.append("]\\n");
    var i: i64 = 0;
    while (i < 1024) {
        source.append("key_"); source.append(i.toString()); source.append("=");
        source.append((i - 512).toString()); source.append("\\n");
        i += 1;
    }
    let owned = source.intoString();
    let document = IniDocument.parse(owned.as_str());
    assert(document.len() == 1024, "INI model growth count");
    i = 0;
    while (i < 1024) {
        assert(document.try_get_i64(section, "key_" + i.toString()).unwrap().unwrap() == i - 512, "INI model growing index values");
        i += 1;
    }
}
''']
    count = 0
    for index, text in enumerate(documents):
        values = reference(text)
        lines += [f"fn document_{index}() {{", "    let document = IniDocument.parse(" + literal(text) + ");",
                  f'    assert(document.len() == {len(values)}, "INI model entry count");']
        for (section, key), value in values.items():
            number, int_kind = 0, 0
            if re.fullmatch(r"[+-]?[0-9]+", value):
                parsed = int(value, 10)
                if -(1 << 63) <= parsed < (1 << 63):
                    number = parsed
                    int_kind = 2 if -(1 << 31) <= parsed < (1 << 31) else 1
            bool_kind = int(parser_bool) if (parser_bool := configparser.ConfigParser.BOOLEAN_STATES.get(value.lower())) is not None else -1
            lines.append("    check(&document, " + ", ".join(literal(v) for v in (section, key, value)) +
                         f", {int_kind}, {integer_literal(number)}, {bool_kind});")
            count += 1
        lines += ['    assert(document.get("a", "absent").is_none() && document.try_get_i64("a", "absent").unwrap().is_none(), "INI model missing values");', "}"]
    lines += ["fn main() -> i32 {", *(f"    document_{index}();" for index in range(len(documents))), "    large_document();"]
    for text in invalid:
        try:
            reference(text)
        except configparser.Error:
            pass
        else:
            raise AssertionError("Invalid INI oracle input was accepted")
        lines.append("    assert(IniDocument.try_parse(" + literal(text) + ').is_err(), "INI model syntax rejection");')
    lines += ['    print("std ini model ok");', "    return 0;", "}"]
    source = directory / "main.vyx"
    source.write_text("\n".join(lines) + "\n", encoding="utf8")
    (directory / "Vyx.toml").write_text('''[package]
name = "std_ini_model"
entry = "main.vyx"
[build]
cache_dir = ".cache"
output_dir = "target"
[dependencies]
ini = { path = "std:serde", target = "serde_ini" }
[target.std_ini_model]
type = "executable"
entry = "main.vyx"
auto_sources = false
''', encoding="utf8")
    return {"seed": seed, "documents": len(documents), "lookups": count, "invalid": len(invalid), "growing_index_entries": 1024,
            "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
            "generator_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
