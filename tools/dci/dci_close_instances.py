#!/usr/bin/env python3
"""Close open-generic DCI instances the consumer discovered while lowering.

This is the build-internal counterpart of the compiler's `.dci_open` request
sidecar.  The Vyx consumer declares its base classes *open* (`class X<T> :
native.SinkG<T>`), lowering records every instance it actually materializes as
an `instance <type-text>` request line, and this tool asks the *producer*
adapter to close exactly those instances -- nothing hand-listed, nothing
pre-exported.  The output is a supplementary .dcib carrying the closed facts
(vtable layouts, method ABIs) that the next lowering pass consumes; the
original contract is never rewritten.

The producer invocation is derived from the contract's own provenance when
possible (the rust adapter records `crate_root` / `crate_name` in the dcib
`source` section) and can be overridden explicitly for producers whose adapter
needs flags the dcib does not carry (C++ headers / clang / includes):

    dci_close_instances.py --requests r.txt --out closed.dcib --contract c.dcib
    dci_close_instances.py ... --adapter-cmd python dci.py adapter --language cpp \
        overlay.hpp qwidget.h --triplet windows_x64 --clang clang++ ...

    (`--adapter-cmd` consumes every following argument; the tool appends
    `--export-instance <spec>` per requested instance and the adapter's output
    flag (`--out` for cpp, `-o` for rust) pointing at a temp dcib.)
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dcib  # noqa: E402


def _decode(path: Path) -> dict:
    return dcib.decode(path.read_bytes())


def _dispatch_class_names(document: dict) -> set[str]:
    """Every instance name any descriptor of `document` already carries."""
    names: set[str] = set()
    exports = document.get("exports") or {}
    for table in exports.get("dispatch_tables") or []:
        for key in ("class_name", "base_class"):
            value = table.get(key)
            if value:
                names.add(str(value))
    for table in exports.get("vtables") or []:
        for key in ("class_name", "base_class"):
            value = table.get(key)
            if value:
                names.add(str(value))
    return names


def _method_owner_names(document: dict) -> set[str]:
    """Every instance a closed *method* symbol is attached to.

    A generic record's instance (`Pair2<f64,i32>`) has no vtable: its closure
    product is the set of `kind=method` symbols whose `owner` names the
    instance.  Progress detection and already-described filtering must count
    these, or closing instance methods loops on a false "closed none of".
    """
    names: set[str] = set()
    for symbol in (document.get("exports") or {}).get("symbols") or []:
        if str(symbol.get("kind")) == "method" and symbol.get("owner"):
            names.add(str(symbol.get("owner")))
    return names


_UNVERIFIED_RETURN_RE = re.compile(
    r"value return type '([^']+)' has no verified nonzero layout"
)


def _unverified_return_instances(document: dict, requested_keys: set[str]) -> list[str]:
    """Instance names the adapter refused to export a method *returning*.

    The cpp adapter adjudicates a method symbol only when the producer's own
    toolchain verified the layout of every type it names -- including a
    returned instance (`swapped() -> Pair2<B, A>`) the consumer never called
    a method on, so no sidecar line asked for it.  Its rejection reason
    spells the instance it still needs; feeding that name back as another
    `--export-instance` lets the next round measure it and export the
    method.  Names come back in the provider's own spelling, ready to be
    sent straight back to the same adapter.
    """
    names: list[str] = []
    seen: set[str] = set()
    for rejected in (document.get("exports") or {}).get("rejected_symbols") or []:
        owner = str(rejected.get("name") or "").rpartition("::")[0]
        if owner and _canon_instance(owner) not in requested_keys:
            continue
        match = _UNVERIFIED_RETURN_RE.search(str(rejected.get("reason") or ""))
        if not match:
            continue
        name = match.group(1).strip()
        if "<" not in name:
            continue
        key = _canon_instance(name)
        if key in requested_keys or key in seen:
            continue
        seen.add(key)
        names.append(name)
    return names


def _canon_instance(name: str) -> str:
    """Normalize separators and primitive spelling, preserving namespaces."""
    text = name.replace(" ", "").replace("::", ".")
    return _respell_cpp_primitives(text)


#: C++ builtin spellings -> DCI spelling, longest phrase first.  Two-word
#: names (`unsigned int`) must be matched as phrases: an identifier-level
#: token sub would leave `unsigned i32` behind.  Bare `long`/`unsigned` are
#: deliberately absent -- their width is platform-dependent and the adapters
#: never spell a DCI width that way.
_CPP_PRIMITIVE_PHRASES: list[tuple[re.Pattern[str], str]] = [
    (re.compile(r"\bunsignedlonglong\b"), "u64"),
    (re.compile(r"\bunsignedlong\b"), "u32"),
    (re.compile(r"\bunsignedint\b"), "u32"),
    (re.compile(r"\bunsignedshort\b"), "u16"),
    (re.compile(r"\bunsignedchar\b"), "u8"),
    (re.compile(r"\blonglong\b"), "i64"),
    (re.compile(r"\bsignedchar\b"), "i8"),
    (re.compile(r"\bint\b"), "i32"),
    (re.compile(r"\bshort\b"), "i16"),
    (re.compile(r"\bfloat\b"), "f32"),
    (re.compile(r"\bdouble\b"), "f64"),
]


def _respell_cpp_primitives(text: str) -> str:
    """Fold C++ builtin spellings inside an instance name to DCI spelling.

    Runs on the space-stripped form (`unsignedint`), which is why the
    phrases above carry no spaces.
    """
    for pattern, replacement in _CPP_PRIMITIVE_PHRASES:
        text = pattern.sub(replacement, text)
    return text


def _requested_instances(requests_path: Path, known: set[str], *, language: str = "rust") -> list[str]:
    """Fold the sidecar's `instance` lines into producer-spelling specs.

    The sidecar spells instances the consumer way (`native.SinkG<i32>`, or
    `native.SinkG::<i32>` when the request came from the diagnostic site);
    the adapter wants the producer's own spelling (`SinkG<i32>`).  Instances
    without a type argument are not generic and are always already described
    by the producer contract; asking for them again is noise.
    """
    specs: list[str] = []
    consumer_texts: list[str] = []
    seen: set[str] = set()
    for lineno, raw in enumerate(
        requests_path.read_text(encoding="utf-8").splitlines(), 1
    ):
        line = raw.strip()
        if not line or "\t" not in line:
            continue
        kind, _, name = line.partition("\t")
        if kind != "instance":
            continue
        name = name.strip()
        if not name or "<" not in name:
            continue
        consumer_text = name.replace("::<", "<")
        if any(
            _canon_instance(consumer_text) == _canon_instance(k)
            or _canon_instance(consumer_text).startswith(_canon_instance(k) + "<")
            for k in known
        ):
            continue
        if language == "cpp":
            name = name.replace(".", "::")
        else:
            dot = name.find(".")
            if dot >= 0:
                name = name[dot + 1 :]
        name = name.replace("::<", "<")
        if name not in seen:
            seen.add(name)
            specs.append(name)
            consumer_texts.append(consumer_text)
    return specs, consumer_texts


def _rust_adapter_cmd(contract: dict, rustc: str | None) -> list[str]:
    source = contract.get("source") or {}
    crate_root = source.get("crate_root")
    if not crate_root:
        raise SystemExit(
            "close-instances: contract carries no crate_root provenance; "
            "pass --adapter-cmd explicitly"
        )
    cmd = [sys.executable, str(Path(__file__).with_name("dci_adapter_rust.py")), crate_root]
    crate_name = source.get("crate_name")
    if crate_name:
        cmd += ["--crate-name", str(crate_name)]
    cargo = source.get("cargo") or {}
    if cargo.get("manifest_path"):
        cmd += ["--manifest-path", str(cargo["manifest_path"])]
        if cargo.get("package_id"):
            cmd += ["--package", str(cargo["package_id"])]
        if cargo.get("requested_features"):
            cmd += ["--features", ",".join(cargo["requested_features"])]
        for flag in ("no_default_features", "offline", "locked"):
            if cargo.get(flag):
                cmd += ["--" + flag.replace("_", "-")]
    for item in source.get("selected_items") or []:
        cmd += ["--item", str(item)]
    if rustc:
        cmd += ["--rustc", rustc]
    if source.get("emit_views"):
        cmd += ["--emit-views"]
    for opaque in source.get("opaque_types") or []:
        cmd += ["--opaque-type", str(opaque)]
    return cmd


def _cpp_adapter_cmd(contract: dict) -> list[str]:
    source = contract.get("source") or {}
    headers = source.get("headers") or []
    if not headers:
        raise SystemExit(
            "close-instances: cpp contract carries no headers provenance; "
            "pass --adapter-cmd explicitly"
        )
    # The msvc-shape adapter is the one that owns instance closure
    # (`--export-instance`); it derives clang from the repo toolchain the same
    # way the passive contract generation does.
    cmd = [sys.executable, str(Path(__file__).with_name("dci_adapter_msvc.py"))]
    settings = source.get("closure_settings") or {}
    for option in ("toolchain", "cxx", "clang"):
        if settings.get(option):
            cmd += ["--" + option, str(settings[option])]
    for flag in settings.get("clang_flags") or (source.get("compiler") or {}).get("flags") or []:
        cmd += ["--compile-flags=" + flag]
    for flag in settings.get("cxx_flags") or []:
        cmd += ["--cxx-arg=" + flag]
    for header in headers:
        cmd += ["--include", str(header)]
    if source.get("boundary"):
        cmd += ["--boundary", str(source["boundary"])]
    for member in source.get("borrowed_template_returns", []):
        cmd += ["--borrow-template-return", str(member)]
    if source.get("cxx_standard"):
        cmd += ["--std", str(source["cxx_standard"])]
    if (source.get("target") or {}).get("triple"):
        cmd += ["--target", str(source["target"]["triple"])]
    return cmd


def _derived_adapter_cmd(contract: dict, rustc: str | None) -> list[str]:
    source = contract.get("source") or {}
    language = source.get("language") or (
        "rust" if source.get("crate_root") else None)
    if language == "cpp":
        return _cpp_adapter_cmd(contract)
    return _rust_adapter_cmd(contract, rustc)


def _requested_list_initializers(path: Path, documents: list[dict]) -> list[tuple[str, list[str], str]]:
    """Producer brace requests not already measured by a previous round."""
    known = set()
    for document in documents:
        for symbol in (document.get("exports") or {}).get("symbols", []):
            request = symbol.get("cpp_materialization") or {}
            if request.get("kind") == "list_constructor":
                known.add((_canon_instance(request["owner"]), tuple(request["arguments"])))
    requests = []
    for raw in path.read_text(encoding="utf-8").splitlines():
        fields = raw.split("\t")
        if fields[0] != "list-init":
            continue
        if len(fields) != 3:
            raise SystemExit("close-instances: malformed list initializer request")
        owner = fields[1].replace("::<", "<")
        # Nested type arguments contain commas, so split only at depth zero.
        arguments, start, depth = [], 0, 0
        for index, ch in enumerate(fields[2]):
            if ch == "<":
                depth += 1
            elif ch == ">":
                depth -= 1
            elif ch == "," and depth == 0:
                arguments.append(fields[2][start:index])
                start = index + 1
            if depth < 0:
                raise SystemExit("close-instances: unbalanced initializer argument type")
        if depth:
            raise SystemExit("close-instances: unbalanced initializer argument type")
        if fields[2]:
            arguments.append(fields[2][start:])
        provider_owner = _respell(owner.replace(".", "::"), _CPP_PRIMITIVES)
        provider_arguments = [_respell(arg.replace(".", "::"), _CPP_PRIMITIVES) for arg in arguments]
        key = (_canon_instance(provider_owner), tuple(provider_arguments))
        if key not in known:
            known.add(key)
            requests.append((provider_owner, provider_arguments, owner))
    return requests


#: Vyx primitive -> C++ spelling for the instance specs the cpp adapter parses
#: (clang adjudicates).  Basic builtin spellings on purpose: the producer
#: header may not include <cstdint>, and `long long` / `unsigned int` name the
#: same types `int64_t` / `uint32_t` would.  The consumer's DCI `char` closes
#: against the C++ `char` the same way the hand-listed flow did.
_CPP_PRIMITIVES = {
    "i8": "signed char", "u8": "unsigned char",
    "i16": "short", "u16": "unsigned short",
    "i32": "int", "u32": "unsigned int",
    "i64": "long long", "u64": "unsigned long long",
    "f32": "float", "f64": "double",
    "bool": "bool", "char": "char",
}

_IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


def _respell(text: str, mapping: dict[str, str]) -> str:
    return _IDENT_RE.sub(lambda m: mapping.get(m.group(0), m.group(0)), text)


def _respell_document(node: object, mapping: dict[str, str]) -> object:
    """Rewrite spelled-out primitive type arguments inside instance names.

    The cpp adapter names what it closed in provider spelling
    (``SinkG<int32_t>``); the consumer and its lowering speak Vyx spelling
    (``SinkG<i32>``).  Only identifier-shaped tokens are touched, so mangled
    link names (which encode primitives as single-letter codes) pass through
    untouched.  Rust needs no mapping -- its primitive spelling already is the
    consumer's -- so an identity map is a no-op there.
    """
    if isinstance(node, dict):
        return {k: _respell_document(v, mapping) for k, v in node.items()}
    if isinstance(node, list):
        return [_respell_document(v, mapping) for v in node]
    if isinstance(node, str) and ("<" in node or "::" in node):
        return _respell(text=node, mapping=mapping)
    return node


def _focus_closure(
    document: dict, spec_keys: set[str], rename: dict[str, str]
) -> tuple[dict, set[str]]:
    """Keep only the requested instances' closure products, consumer-spelled.

    The adapter's closure answer describes the whole producer surface (its
    own public functions, unrelated records) under the adapter's internal
    namespace spellings (`lib.Pair2<f64, i32>`).  A supplement must carry
    only what the consumer asked for, spelled the way the consumer's request
    spelled it, for two reasons: the consumer's layout/owner lookups match on
    its own spelling, and duplicate provider-spelled method symbols would
    shadow the base contract's entries during descriptor scans.  Layouts
    pass through unfiltered (a requested method may reference a nested
    instance the consumer never called a method on); symbols and dispatch
    tables are kept only for the requested instances.

    Returns the focused document and the canonical names it closes.
    """
    exports = document.get("exports") or {}

    def fix(node: object) -> object:
        if isinstance(node, dict):
            return {k: v if k == "cpp_materialization" else fix(v) for k, v in node.items()}
        if isinstance(node, list):
            return [fix(v) for v in node]
        if isinstance(node, str):
            canon = _canon_instance(node)
            return rename.get(canon, node)
        return node

    closed_names: set[str] = set()
    out_exports: dict[str, object] = {}
    kept_layouts = [
        fix(layout) for layout in exports.get("layouts") or []
    ]
    if kept_layouts:
        out_exports["layouts"] = kept_layouts
    kept_symbols = [
        fix(symbol) for symbol in exports.get("symbols") or []
        if str(symbol.get("kind")) != "function"
        and (
            str(symbol.get("kind")) not in ("method", "constructor", "destructor")
            or _canon_instance(str(symbol.get("owner") or "")) in spec_keys
        )
    ]
    if kept_symbols:
        out_exports["symbols"] = kept_symbols
        closed_names |= {
            _canon_instance(str(symbol.get("owner")))
            for symbol in kept_symbols
            if symbol.get("owner")
            and str(symbol.get("kind")) in ("method", "constructor", "destructor")
        }
    for key in ("dispatch_tables", "vtables"):
        kept = [
            fix(table) for table in exports.get(key) or []
            if any(
                _canon_instance(str(table.get(field) or "")) in spec_keys
                for field in ("class_name", "base_class")
            )
        ]
        if kept:
            out_exports[key] = kept
            closed_names |= {
                _canon_instance(str(table.get(field)))
                for table in kept
                for field in ("class_name", "base_class")
                if table.get(field)
            }
    focused = dict(document)
    focused["exports"] = out_exports
    return focused, closed_names


def _merge_documents(base: dict, extra: dict) -> dict:
    """Fold previously closed facts (an existing supplement) under new ones.

    The build re-runs this tool per closure round with only the instances the
    latest lowering still misses; overwriting the supplement would discard the
    instances closed by earlier rounds (the compiler consumes ONE supplement
    file).  New facts win on identity conflicts; everything else carries
    forward.
    """
    if not isinstance(base, dict) or not base:
        return extra
    merged = dict(base)
    base_exports = dict(merged.get("exports") or {})
    extra_exports = dict((extra.get("exports") or {}))

    def merge_list(key: str, identity) -> None:
        # New facts win: an identity present in `extra` replaces the stale
        # one from the previous supplement (earlier rounds may have closed
        # the same instance with fewer facts).
        extras = list(extra_exports.get(key) or [])
        seen = set()
        for item in extras:
            seen.add(identity(item))
        items = list(extras)
        for item in base_exports.get(key) or []:
            item_id = identity(item)
            if item_id in seen:
                continue
            seen.add(item_id)
            items.append(item)
        base_exports[key] = items

    def _identity(item):
        return json.dumps(item, sort_keys=True, default=str)

    merge_list("dispatch_tables", lambda t: str(t.get("class_name") or _identity(t)))
    merge_list("vtables", lambda t: str(t.get("class_name") or _identity(t)))
    merge_list("layouts", lambda r: str(r.get("type_name") or _identity(r)))
    merge_list("symbols", lambda s: (str(s.get("kind")), str(s.get("owner")), str(s.get("link_name") or s.get("name"))))
    merge_list("aliases", _identity)
    for key, value in extra_exports.items():
        if key not in base_exports:
            base_exports[key] = value
    merged["exports"] = base_exports
    return merged


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--requests", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--contract", action="append", default=[])
    parser.add_argument(
        "--adapter-cmd",
        nargs=argparse.REMAINDER,
        default=None,
        metavar="CMD",
        help="producer adapter invocation; instances and -o are appended",
    )
    parser.add_argument(
        "--rustc",
        default=None,
        help="rustc executable forwarded to the derived rust adapter",
    )
    args = parser.parse_args()

    requests_path = Path(args.requests)
    if not requests_path.is_file():
        # No sidecar (or nothing discoverable in it): nothing to close, and a
        # retry cannot help.  Report "no progress" rather than an error.
        print(f"close-instances: no requests sidecar at {requests_path}")
        return 3
    if not args.contract:
        raise SystemExit("close-instances: at least one --contract is required")
    contracts = [Path(p) for p in args.contract]
    for path in contracts:
        if not path.is_file():
            raise SystemExit(f"close-instances: contract not found: {path}")

    documents = [_decode(path) for path in contracts]
    known: set[str] = set()
    for document in documents:
        known |= {
            _canon_instance(name)
            for name in _dispatch_class_names(document) | _method_owner_names(document)
        }
    source = documents[0].get("source") or {}
    language = source.get("language") or ("rust" if source.get("crate_root") else "rust")
    specs, consumer_texts = _requested_instances(requests_path, known, language=language)
    list_initializers = _requested_list_initializers(requests_path, documents) if language == "cpp" else []
    for owner, _, consumer in list_initializers:
        if not any(_canon_instance(owner) == _canon_instance(spec) for spec in specs):
            specs.append(owner)
            consumer_texts.append(consumer)
    if not specs:
        # Every requested instance is already described by the contracts in
        # hand: closing produced nothing new, so a retry would fail the same
        # way.  The caller (build system) treats exit 3 as "no progress" and
        # reports the original compile error instead of retrying forever.
        print("close-instances: nothing to close (all requests already described)")
        return 3

    source = documents[0].get("source") or {}
    language = source.get("language") or (
        "rust" if source.get("crate_root") else "rust")
    # Consumer spelling -> provider spelling for the adapter request.  The
    # facts that come back are NOT respelled field-by-field: the adapters
    # record ABI type names in DCI spelling natively, and owner/layout texts
    # are renamed through the paired rename map below (an identifier-level
    # reverse respell would corrupt two-word spellings like `unsigned int`).
    to_provider: dict[str, str] = {}
    if language == "cpp":
        to_provider = dict(_CPP_PRIMITIVES)
    specs = [_respell(spec, to_provider) for spec in specs]

    if args.adapter_cmd is not None:
        cmd = [c for c in args.adapter_cmd if c != "--"]
    else:
        cmd = _derived_adapter_cmd(documents[0], args.rustc)

    with tempfile.TemporaryDirectory(prefix="dci_close_") as tmp:
        temp_dcib = Path(tmp) / "closed.dcib"
        # The adapter families spell the output flag differently: the cpp
        # (msvc-shape) adapter takes only `--out`; the rust adapter takes
        # `-o`/`--output`.
        out_flag = "--out" if language == "cpp" else "-o"
        requested_keys = {_canon_instance(spec) for spec in specs}
        closed: dict = {}
        # A closed method may RETURN another instance (`swapped() ->
        # Pair2<B, A>`), and the cpp adapter refuses such a symbol until the
        # returned instance's layout is measured by the producer itself.
        # Instead of predicting that shape here, feed the adapter's own
        # rejection reason back in as an additional request and re-run --
        # the answer names exactly the instances it still needs (bounded:
        # a template surface closes over a finite instance set).
        for _round in range(4):
            full_cmd = list(cmd)
            for spec in specs:
                full_cmd += ["--export-instance", spec]
            for owner, arguments, _ in list_initializers:
                full_cmd += ["--list-initializer", json.dumps([owner, arguments])]
            full_cmd += [out_flag, str(temp_dcib)]
            print("close-instances: " + " ".join(full_cmd))
            result = subprocess.run(full_cmd)
            if result.returncode != 0:
                raise SystemExit(f"close-instances: producer adapter failed ({result.returncode})")
            closed = _decode(temp_dcib)
            missing = _unverified_return_instances(closed, requested_keys)
            if not missing:
                break
            print(
                "close-instances: the producer needs the return instance(s) "
                + ", ".join(missing)
                + " measured before it can export the requested methods"
            )
            specs = specs + [name.replace(".", "::") if language == "cpp" else name for name in missing]
            requested_keys |= {_canon_instance(name) for name in missing}

    # Focus the answer down to the requested instances and rename every match
    # to the consumer's own request text.  The rename map keys on canonical
    # shapes (space-, namespace- and primitive-insensitive) of BOTH spellings:
    # what we sent the provider (`Pair2<long long, char>`) and what the
    # consumer asked for (`Pair2<i64,char>`), so `lib.Pair2<f64, i32>` and
    # `Pair2<double,int>` both land on what the sidecar asked for.
    #
    # The base contracts' layouts extend the map to *derived* instances: a
    # method's return type (`Pair2<B, A>`) is an instance the consumer never
    # called a method on, so no sidecar line names it -- but the merged
    # contract carries its layout under the consumer's spelling, and that is
    # the identity the compiler's return-type match looks up.
    rename: dict[str, str] = {}
    for document in documents:
        for layout in (document.get("exports") or {}).get("layouts") or []:
            type_name = str(layout.get("type_name") or "")
            if type_name:
                rename.setdefault(_canon_instance(type_name), type_name)
    for spec, consumer_text in zip(specs, consumer_texts):
        rename[_canon_instance(spec)] = consumer_text
        rename[_canon_instance(consumer_text)] = consumer_text
    spec_keys = {
        _canon_instance(spec) for spec in specs
    } | {
        _canon_instance(text) for text in consumer_texts
    }
    closed, closed_names = _focus_closure(closed, spec_keys, rename)
    newly_closed = {name for name in closed_names if name not in known}
    if not newly_closed and not list_initializers:
        raise SystemExit(
            "close-instances: the adapter closed none of "
            + ", ".join(specs)
            + " -- the producer source cannot materialize the requested instances"
        )
    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    if out_path.is_file():
        # A previous closure round (or warm build) wrote facts here; the
        # compiler consumes one supplement file, so accumulate instead of
        # overwriting.  The existing file is already in consumer spelling.
        try:
            closed = _merge_documents(_decode(out_path), closed)
        except dcib.DcibError:
            pass
    out_path.write_bytes(dcib.encode(closed))
    print(
        "close-instances: closed "
        + ", ".join(sorted(newly_closed) or [owner for owner, _, _ in list_initializers])
        + f" -> {out_path}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
