"""URL byte and component oracles using urllib.parse and ipaddress."""
import hashlib
import ipaddress
import json
from pathlib import Path
import random
import re
from urllib.parse import quote_from_bytes, urlencode, urlsplit


def encode(data: bytes, form: bool = False) -> str:
    # urllib always leaves '~' unescaped and escapes '*'. Adapt those two
    # characters to the form alphabet; no percent scanner is shared with Vyx.
    encoded = quote_from_bytes(data, safe="-._~")
    return encoded.replace("~", "%7E").replace("%2A", "*").replace("%20", "+") if form else encoded


def write_project(directory: Path, seed: int = 914527) -> dict:
    directory.mkdir(parents=True, exist_ok=True)
    rng = random.Random(seed)
    data = [bytes(range(256)), b"", b"\0", b" +*~", b"%252F", "中文 +".encode(), b"\xff\xfe\x80"]
    data += [bytes(rng.randrange(256) for _ in range(rng.randrange(97))) for _ in range(73)]
    urls = ["http://example.org", "https://example.org/?#", "http://@example.org", "ws://[::1]:0/", "wss://[::ffff:192.0.2.128]:65535/a?x=+#z"]
    for _ in range(91):
        scheme = rng.choice(("http", "https", "ws", "wss"))
        host = rng.choice(("example.org", "localhost", str(ipaddress.IPv4Address(rng.getrandbits(32))),
                           "[" + str(ipaddress.IPv6Address(rng.getrandbits(128))) + "]", "[2001:db8::1]"))
        credentials = rng.choice((None, "", "user:p%40ss", "a%2Bb"))
        port = rng.choice((None, 0, 80, 443, 65535, rng.randrange(65536)))
        path = rng.choice(("", "/", "/a%2Fb", "/%E4%B8%AD%E6%96%87", "/a:b@c/"))
        query = rng.choice((None, "", "a=+&b=%00", "a=1&a=2", "q=foo?bar/baz"))
        fragment = rng.choice((None, "", "frag", "a?b/c"))
        raw = scheme + "://" + (credentials + "@" if credentials is not None else "") + host
        raw += (":" + str(port) if port is not None else "") + path
        raw += "?" + query if query is not None else ""
        raw += "#" + fragment if fragment is not None else ""
        urls.append(raw)
    invalid = ["%", "%0", "%xz", "a%%20", "ok%2+", "%GG"]
    invalid += ["a" * rng.randrange(24) + rng.choice(("%", "%A", "%ZZ", "%x0", "%0x")) for _ in range(58)]
    query_sets = []
    for _ in range(32):
        pairs = [(b"tag", b"a+b"), (b"tag", b" "), (b"", b"")]
        pairs += [(rng.choice(data[:7]), rng.choice(data[:7])) for _ in range(rng.randrange(4))]
        # urllib independently handles first-equals delimiters and repeated
        # byte-valued pairs; we require the same explicit order in the consumer.
        percent = urlencode(pairs, quote_via=lambda value, safe, encoding=None, errors=None: quote_from_bytes(value, safe))
        form = urlencode(pairs).replace("~", "%7E").replace("%2A", "*")
        query_sets.append((percent, form, len(pairs)))

    def literal(text):
        return json.dumps(text, ensure_ascii=False)

    calls = [f"check_codec({literal(value.hex())}, {literal(encode(value))}, {literal(encode(value, True))});" for value in data]
    for raw in urls:
        parts = urlsplit(raw)
        flags = int("?" in raw.split("#", 1)[0]) | (int("#" in raw) << 1) | (int("@" in parts.netloc) << 2)
        userinfo = parts.netloc.rsplit("@", 1)[0] if "@" in parts.netloc else ""
        port = str(parts.port) if parts.port is not None else ""
        arguments = [literal(value) for value in (raw, parts.hostname, port, parts.path, parts.query, parts.fragment, userinfo)]
        calls.append("check_url(" + ", ".join(arguments) + ", " + str(flags) + ");")
    for raw in invalid:
        bad = re.search(r"%(?![0-9A-Fa-f]{2})", raw)
        if bad is None:
            raise AssertionError("Invalid escape oracle has a valid input")
        calls.append(f"check_invalid({literal(raw)}, {bad.start()});")
    calls += [f"check_query({literal(percent)}, {literal(form)}, {count});" for percent, form, count in query_sets]
    lines = ["use std.core;", "use std.string;", "use std.encoding;", "use std.collections;", "use std.url;", '''
fn check_codec(raw: string, percent: string, form: string) {
    let input = hex_decode(raw);
    assert(url_encode(input) == percent && form_encode(input) == form, "URL model encode");
    assert(hex_encode(url_decode(percent)) == raw && hex_encode(form_decode(form)) == raw, "URL model decode");
}
fn check_url(raw: string, host: string, port: string, path: string, query: string, fragment: string, userinfo: string, flags: i32) {
    let parsed = Url.parse(raw);
    assert(parsed.host == host && parsed.port == port && parsed.path == path, "URL model authority/path");
    assert(parsed.query == query && parsed.fragment == fragment && parsed.userinfo == userinfo, "URL model components");
    assert(parsed.has_query == ((flags & 1) != 0) && parsed.has_fragment == ((flags & 2) != 0) && parsed.has_userinfo == ((flags & 4) != 0), "URL model delimiters");
    assert(parsed.toString() == raw, "URL model serialization");
}
fn check_invalid(raw: string, offset: i64) {
    match (try_url_decode(raw)) {
        Ok(_) => { panic("URL model malformed escape accepted"); }
        Err(failure) => { assert(failure.offset == offset && failure.kind == UrlErrorKind.InvalidEscape, "URL model escape error"); }
    }
    assert(try_form_decode(raw).is_err(), "URL model form escape error");
}
fn check_query(percent: string, form: string, count: i64) {
    let query = try_query_decode(percent).unwrap();
    let fields = try_form_query_decode(form).unwrap();
    assert(query.len == count && fields.len == count, "URL model repeated/empty query fields");
    assert(query_encode_pairs(&query) == percent && query_encode_pairs(&fields) == percent, "URL model query values");
    assert(form_query_encode(&query) == form && form_query_encode(&fields) == form, "URL model form values");
}
''']
    names = []
    for start in range(0, len(calls), 16):
        name = "trace_" + str(start // 16)
        names.append(name)
        lines += ["fn " + name + "() {", *("    " + call for call in calls[start:start + 16]), "}"]
    lines += ["fn main() -> i32 {", *("    " + name + "();" for name in names), '    print("std url model ok");', "    return 0;", "}"]
    source = directory / "main.vyx"
    source.write_text("\n".join(lines) + "\n", encoding="utf8")
    (directory / "Vyx.toml").write_text('''[package]
name = "std_url_model"
entry = "main.vyx"
[build]
cache_dir = ".cache"
output_dir = "target"
[dependencies]
std = { path = "std:std", target = "std" }
[target.std_url_model]
type = "executable"
entry = "main.vyx"
auto_sources = false
''', encoding="utf8")
    return {"seed": seed, "bytes": len(data), "urls": len(urls), "invalid": len(invalid), "queries": len(query_sets),
            "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
            "generator_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
