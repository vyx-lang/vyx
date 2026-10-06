"""Prepare user-selected C++ exports through the Adapter and Converter.

Author-declared exports and native build inputs belong in Vyx.toml. Defaults remain on the original
native declarations. Qt connection templates require producer instantiation;
the regular Adapter measures those separately from direct native symbols.
"""
from __future__ import annotations

from collections import defaultdict
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

try:
    import tomllib
except ImportError:
    import tomli as tomllib

try:
    from . import dci_adapter_msvc as adapter, cpp_toolchains, dcib, converter, dci_validate
except ImportError:
    import dci_adapter_msvc as adapter
    import cpp_toolchains
    import dcib
    import converter
    import dci_validate


class ImportError(ValueError):
    pass




def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def expand(value):
    """Match the manifest's nested ${NAME:-fallback} spelling."""
    if not isinstance(value, str):
        raise ImportError("DCI manifest paths and flags must be strings")
    pattern = re.compile(r"\$\{([A-Za-z_]\w*)(?::-([^{}]*))?\}")
    for _ in range(16):
        if not pattern.search(value):
            if "${" in value:
                raise ImportError(f"invalid environment expression: {value}")
            return value
        def replace(match):
            name, fallback = match.groups()
            result = os.environ.get(name) or fallback
            if result is None:
                raise ImportError(f"environment variable {name} is required by DCI import")
            return result
        value = pattern.sub(replace, value)
    raise ImportError("DCI manifest environment expansion is recursive")


def run(argv, **kwargs):
    proc = subprocess.run(list(map(str, argv)), capture_output=True, text=True,
                          errors="replace", **kwargs)
    if proc.returncode:
        raise ImportError(f"producer command failed ({proc.returncode}): {' '.join(map(str, argv))}\n"
                          + proc.stdout + proc.stderr)
    return proc.stdout


def write_changed(path, data):
    path = Path(path)
    data = data.encode("utf-8") if isinstance(data, str) else data
    if not path.exists() or path.read_bytes() != data:
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = path.with_suffix(path.suffix + ".tmp")
        temporary.write_bytes(data)
        temporary.replace(path)


def token_at(location, fallback_file=""):
    location = location.get("expansionLoc", location)
    filename = location.get("file", fallback_file)
    if not filename or filename.startswith("<") or "offset" not in location:
        return ""
    data = Path(filename).read_bytes()
    offset = location["offset"]
    return data[offset:offset + location.get("tokLen", 0)].decode("utf-8")


def harvest_methods(nodes, owners, profile):
    """Use exact AST owner resolution and macro expansion locations."""
    methods = {}
    for record in adapter.walk(nodes):
        if record.get("kind") != "CXXRecordDecl" or not record.get("completeDefinition"):
            continue
        owner = adapter.resolve_record_owner_from_node(record, owners)
        if not owner:
            continue
        file = record.get("loc", {}).get("file", "")
        access = "public" if record.get("tagUsed") == "struct" else "private"
        signal_section = False
        scoped_types = {n.get("name") for n in record.get("inner", [])
                        if n.get("kind") in {"EnumDecl", "TypeAliasDecl", "TypedefDecl", "CXXRecordDecl"}
                        and n.get("name") and not n.get("isImplicit") and n.get("name") != record.get("name")}
        for node in record.get("inner", []):
            if node.get("kind") == "AccessSpecDecl":
                access = node.get("access", access)
                signal_section = profile == "qt" and token_at(node.get("loc", {}), file) in {"Q_SIGNALS", "signals"}
                continue
            if node.get("kind") not in {"CXXMethodDecl", "CXXConstructorDecl", "CXXDestructorDecl"}:
                continue
            symbol = adapter.symbol_from_method(owner, node, access)
            if symbol is None:
                continue
            if symbol.kind == "constructor" and record.get("definitionData", {}).get("isAbstract"):
                continue
            adapter.qualify_symbol_record_types(symbol, owners)
            for ty in [p["type"] for p in symbol.params] + ([symbol.ret["type"]] if symbol.ret else []):
                raw = ty.get("cpp_type", "")
                # Only identities declared in this exact AST class scope are
                # qualified; a same-named type elsewhere is never a candidate.
                ty["cpp_type"] = re.sub(r"(?<![\w:])[A-Za-z_]\w*(?![\w:])",
                    lambda m: owner + "::" + m.group() if m.group() in scoped_types else m.group(), raw)
            params = [n for n in node.get("inner", []) if n.get("kind") == "ParmVarDecl"]
            if len(params) != len(symbol.params):
                continue
            defaults = ["init" in p for p in params]
            method_signal = profile == "qt" and token_at(node.get("range", {}).get("begin", {}), file) == "Q_SIGNAL"
            methods[adapter.symbol_identity(symbol)] = (symbol, defaults, signal_section or method_signal)
    return list(methods.values())


available_type = converter.available_type

def bridge_name(symbol, mode, count):
    key = [adapter.symbol_selector(symbol), mode, count, symbol.is_static]
    return "__vyx_dci_" + hashlib.sha256(json.dumps(key).encode()).hexdigest()[:24]


def bridge_plan(methods, owners, rules, profile, connections=()):
    candidates, rejected = [], []
    requested, found = set(connections), set()
    if requested and profile != "qt":
        raise ImportError("qt_connections requires an explicitly selected Qt protocol")
    for symbol, defaults, signal in methods:
        if symbol.visibility != "public" or symbol.kind == "destructor":
            continue
        selector = adapter.symbol_selector(symbol)
        if selector not in requested:
            continue
        found.add(selector)
        if signal and symbol.kind == "method" and not symbol.is_static:
            scalar = all(p["type"].get("kind") == "primitive" and
                         p["type"].get("reference") == "value" for p in symbol.params)
            if scalar and (symbol.ret or {}).get("type", {}).get("name") == "void":
                candidates.append({"symbol": symbol, "mode": "signal", "count": len(symbol.params),
                                   "name": bridge_name(symbol, "signal", len(symbol.params)),
                                   "member": "on_" + symbol.member_name, "profile": profile})
            else:
                rejected.append({"selector": selector, "reason": "signal payload needs a supported lifetime/result protocol"})
        else:
            rejected.append({"selector": selector, "reason": "requested Qt connection is not a declared native signal"})
    if requested - found:
        raise ImportError(f"requested Qt signals are outside the selected exports or undeclared: {sorted(requested - found)}")
    groups = defaultdict(list)
    for candidate in candidates:
        s = candidate["symbol"]
        key = candidate.get("identity", (s.owner, candidate["member"], candidate["count"]))
        groups[key].append(candidate)
    result = []
    for group in groups.values():
        if len(group) != 1:
            rejected.extend({"selector": adapter.symbol_selector(c["symbol"]),
                             "reason": "consumer overload is ambiguous; explicit producer overload required"} for c in group)
        else:
            result.extend(group)
    return sorted(result, key=lambda c: c["name"]), rejected


def emit_producer(headers, plan):
    out = ["// Qt connection template instantiations; native API definitions use original headers.", "#pragma once"]
    out += [f'#include "{Path(h).as_posix()}"' for h in headers]
    out += ["#if defined(__clang__) || defined(__GNUC__)",
            "#define VYX_DCI_IMPORT_USED __attribute__((used))", "#elif defined(_MSC_VER)",
            "#define VYX_DCI_IMPORT_USED __declspec(dllexport)", "#else",
            '#error "DCI auto imports require retained producer symbols"', "#endif"]
    ownership = {}
    for candidate in plan:
        if candidate["mode"] != "signal":
            raise ImportError("native default arguments must not generate producer wrappers")
        symbol, name = candidate["symbol"], candidate["name"]
        params = symbol.params[:candidate["count"]]
        spellings = [p["type"].get("cpp_type", p["type"].get("canonical_cpp_type")) for p in params]
        if not all(spellings):
            raise ImportError(f"producer spellings missing for {symbol.name}")
        selector, facts = adapter.import_operation_ownership(
            symbol, name, candidate["mode"], candidate["count"],
            candidate.get("ownership_rule", {}), candidate["profile"])
        ownership[selector] = facts
        callback = ",".join(["void*"] + spellings)
        declaration = f'{symbol.owner}* self, QObject* context, void (*callback)({callback}), void* user'
        out += [f'bool {name}({symbol.owner}*,QObject*,void (*)({callback}),void*);',
                f'VYX_DCI_IMPORT_USED inline bool {name}({declaration}) {{',
                "    if (!self || !context || !callback) return false;",
                f'    using Member = void ({symbol.owner}::*)({",".join(spellings)});',
                f'    const auto signal = static_cast<Member>(&{symbol.owner}::{symbol.member_name});',
                '    return static_cast<bool>(QObject::connect(self, signal, context,',
                '        [callback, user](' + ",".join(f"{t} p{i}" for i, t in enumerate(spellings)) + ') { callback(user'
                + ''.join(f", p{i}" for i in range(len(params))) + '); }));', "}"]
    out += ["#undef VYX_DCI_IMPORT_USED", "/* dci-ownership", json.dumps(ownership, indent=2), "dci-ownership-end */", ""]
    return "\n".join(out)


def adopt_bridges(document, plan):
    """Reclassify the measured first argument as the semantic receiver.

    No machine type, passing convention, layout or symbol is fabricated.
    The ABI and mangled link identity remain those measured from native C++.
    """
    symbols = document["exports"]["symbols"]
    by_name = defaultdict(list)
    for symbol in symbols:
        if symbol.get("kind") == "function" and not symbol.get("owner"):
            by_name[symbol["name"]].append(symbol)
    adopted, failed = [], []
    for candidate in plan:
        matches = by_name[candidate["name"]]
        if len(matches) != 1:
            reasons = [s["reason"] for s in document["exports"].get("rejected_symbols", []) if s.get("name") == candidate["name"]]
            failed.append({"selector": adapter.symbol_selector(candidate["symbol"]), "reason": "; ".join(reasons) or f"expected one measured native C++ bridge, found {len(matches)}"})
            continue
        measured = matches[0]
        s = copy.deepcopy(measured)
        original = candidate["symbol"]
        s.update(owner=original.owner, member_name=candidate["member"],
                 name=original.owner + "::" + candidate["member"],
                 kind="constructor" if original.kind == "constructor" else "method",
                 is_static=original.is_static, is_virtual=False,
                 is_const=original.is_const, is_override=False, is_final=False)
        if not original.is_static:
            parameter = s["params"].pop(0)
            physical = s["abi"]["params"].pop(0)
            s["abi"]["receiver"] = {k: v for k, v in physical.items() if k not in {"index", "name"}}
            s["abi"]["receiver"].update(ownership=parameter["ownership"], this_adjust=0)
            s["abi"]["parameters"] = s["abi"]["params"]
            for i, p in enumerate(s["abi"]["params"]):
                p.update(index=i, name=f"p{i}")
            for i, p in enumerate(s["params"]):
                p["name"] = f"p{i}"
        if original.kind == "constructor":
            s["return"] = None
        s["producer_bridge"] = {"kind": candidate["mode"], "origin": adapter.symbol_selector(original),
                                "link_name": measured["link_name"]}
        adopted.append(s)
    internal = {c["name"] for c in plan}
    document["exports"]["symbols"] = [s for s in symbols if s["name"] not in internal] + adopted
    return failed


def emit_import(document, module, contract, header):
    # Cache-owned output is optional; standalone Converter output is user-owned.
    return converter.emit_import(document, module, contract,
                                 [header] if isinstance(header, str) else header, editable=False)


def dependency_files(compiler, std, target, probe, flags, directory):
    depfile = directory / "headers.d"
    run([compiler, f"-std={std}", "-target", target, "-M", "-MT", "vyx_dci", "-MF", depfile, probe, *flags])
    content = depfile.read_text().replace("\\\n", " ").split(":", 1)[1]
    # Clang uses forward-slash paths and Make escaping for spaces, # and $.
    items = re.findall(r"(?:\\.|[^\s])+", content)
    paths = {str(Path(re.sub(r"\\([ #\\])", r"\1", p).replace("$$", "$")).resolve()) for p in items}
    return sorted(paths - {str(probe.resolve())})


def export_scope(spec):
    """Only the author's export declarations select the producer surface."""
    all_exports = spec.get("export_all", False)
    if type(all_exports) is not bool:
        raise ImportError("export_all must be a boolean")
    def names(key):
        values = spec.get(key, [])
        if not isinstance(values, list) or any(not isinstance(v, str) or not v.strip() or any(c in v for c in '\n\r\0') for v in values):
            raise ImportError(f"{key} must be an array of nonempty exact producer identities")
        if len(set(values)) != len(values):
            raise ImportError(f"{key} contains duplicate identities")
        return values
    types, functions = names("export_types"), names("export_functions")
    connections = names("qt_connections")
    if all_exports and (types or functions):
        raise ImportError("choose export_all or explicit export_types/export_functions")
    if not all_exports and not types and not functions:
        raise ImportError("declare export_types/export_functions, or explicitly set export_all=true; consumer usage never selects exports")
    return {"all": all_exports, "types": types, "functions": functions, "qt_connections": connections}


def prepare(manifest_path, target_name, jobs=4, force=False, build_context=None, imports=None):
    if jobs < 1:
        raise ImportError("DCI import jobs must be positive")
    manifest_path = Path(manifest_path).resolve()
    project = manifest_path.parent
    manifest = tomllib.loads(manifest_path.read_text(encoding="utf-8"))
    target = manifest.get("target", {}).get(target_name)
    if target is None:
        raise ImportError(f"unknown target {target_name!r}")
    settings = dict(manifest.get("build", {}), **target)
    if build_context:
        context = json.loads(Path(build_context).read_text(encoding="utf-8"))
        allowed = {"dci_imports", "entry", "sources", "cxx", "cxxflags", "include_paths"}
        if not isinstance(context, dict) or set(context) != allowed:
            raise ImportError("invalid effective DCI build context")
        settings = context
    names = imports if imports is not None else settings.get("dci_imports", [])
    if not isinstance(names, list) or not names:
        raise ImportError(f"target {target_name!r} needs --import names or an optional dci_imports list")
    specs = manifest.get("dci", {}).get("import", {})
    for name in names:
        if name not in specs:
            raise ImportError(f"missing [dci.import.{name}]")
        prepare_one(project, name, specs[name], settings, jobs, force)


def definition_source(project, name, spec):
    """Resolve an optional authored definition; preparation never writes it."""
    if "definitions" not in spec:
        return None
    value = spec["definitions"]
    if not isinstance(value, str) or not value.strip():
        raise ImportError(f"dci.import.{name}: definitions must name a Vyx source file")
    source = (project / expand(value)).resolve()
    cache = (project / ".cache" / "dci" / name).resolve()
    if source == cache or cache in source.parents:
        raise ImportError(f"dci.import.{name}: authored definitions must be outside {cache}")
    if source.suffix != ".vyx" or not source.is_file():
        raise ImportError(f"dci.import.{name}: authored definitions not found: {source}; "
                          "use dci convert to create them explicitly")
    return source


def contract_destination(project, name, spec):
    value = spec.get("contract", f"contracts/{name}.dcib")
    if not isinstance(value, str) or not value.strip():
        raise ImportError(f"dci.import.{name}: contract must name a .dcib output file")
    contract = (project / expand(value)).resolve()
    cache = (project / ".cache").resolve()
    if contract == cache or cache in contract.parents:
        raise ImportError(f"dci.import.{name}: project contract must be outside {cache}")
    if contract.suffix != ".dcib":
        raise ImportError(f"dci.import.{name}: contract must name a .dcib output file")
    return contract


def contract_symbol(value):
    return adapter.Symbol(name=value["name"], owner=value.get("owner", ""),
                          member_name=value.get("member_name", value["name"]),
                          mangled=value["link_name"], kind=value["kind"],
                          calling_convention=value["calling_convention"], params=value["params"],
                          ret={"type": value["return"] or value["abi"]["return"]["type"]},
                          is_static=value.get("is_static", False),
                          is_const=value.get("is_const", False))


def validate_contract_document(document):
    schema = json.loads(Path(__file__).with_name("schema").joinpath("dci-1.0.schema.json").read_text())
    errors = dci_validate.schema_errors(schema, document)
    if isinstance(document, dict):
        errors += dci_validate.semantic_errors(document, strict=True)
    if errors:
        raise ImportError("invalid prepared DCI contract:\n" + "\n".join(errors))


def validate_prepared_contract(document, target, spec, scope):
    if document["source"]["language"] != "cpp":
        raise ImportError("prepared DCI contract is not a C++ contract")
    expected = cpp_toolchains.target_info(target).descriptor()
    actual = document["target"]
    for key in ("architecture", "pointer_width", "endianness", "object_format"):
        if actual.get(key) != expected[key]:
            raise ImportError(f"prepared DCI contract target mismatch: {key}={actual.get(key)!r}, expected {expected[key]!r}")
    if actual.get("abi_family", actual.get("abi")) != expected["abi_family"]:
        raise ImportError("prepared DCI contract ABI family does not match the build target")
    boundary = document["control_flow"].get("default_boundary", document["source"].get("boundary"))
    if boundary != spec.get("boundary", "no_unwind"):
        raise ImportError("prepared DCI contract exception boundary does not match the declared import")
    layouts = {entry["type_name"] for entry in document["exports"].get("layouts", [])}
    missing_types = set(scope["types"]) - layouts
    functions = [s for s in document["exports"].get("symbols", []) if not s.get("owner")]
    available_functions = {s["name"] for s in functions}
    available_functions.update(adapter.symbol_selector(contract_symbol(s)) for s in functions)
    missing_functions = set(scope["functions"]) - available_functions
    if missing_types or missing_functions:
        raise ImportError(f"prepared DCI contract is missing selected exports: {sorted(missing_types | missing_functions)}; "
                          "regenerate explicitly with cpp-import --force")


def prepared_connection_plan(document, profile, connections):
    """Re-materialize only operations whose ABI is already measured in the contract."""
    if connections and profile != "qt":
        raise ImportError("qt_connections requires an explicitly selected Qt protocol")
    symbols = document["exports"].get("symbols", [])
    native = defaultdict(list)
    for symbol in symbols:
        if not symbol.get("producer_bridge"):
            native[adapter.symbol_selector(contract_symbol(symbol))].append(symbol)
    result = []
    for origin in connections:
        bridges = [s for s in symbols if s.get("producer_bridge", {}).get("origin") == origin]
        if len(bridges) != 1 or len(native[origin]) != 1:
            raise ImportError(f"prepared DCI contract has no unique measured connection for {origin}; "
                              "regenerate explicitly with cpp-import --force")
        bridge = bridges[0]
        symbol = contract_symbol(native[origin][0])
        scalar = all(p["type"].get("kind") == "primitive" and
                     p["type"].get("reference") == "value" for p in symbol.params)
        if (bridge["producer_bridge"].get("kind") != "signal" or symbol.is_static or
                symbol.kind != "method" or not scalar or symbol.ret["type"].get("name") != "void"):
            raise ImportError(f"prepared DCI connection has an unsupported protocol: {origin}")
        name = bridge_name(symbol, "signal", len(symbol.params))
        member = "on_" + symbol.member_name
        callback = adapter.dci_type("void (*)(void*," + ",".join(
            p["type"]["cpp_type"] for p in symbol.params) + ")") if symbol.params else adapter.dci_type("void (*)(void*)")
        link = bridge["link_name"]
        native_name_matches = (link.startswith("?" + name + "@@") or
                               link.startswith("_Z" + str(len(name)) + name))
        params = bridge["params"]
        receiver = bridge.get("abi", {}).get("receiver", {})
        if (bridge.get("owner") != symbol.owner or bridge.get("member_name") != member or
                bridge["producer_bridge"].get("link_name") != bridge["link_name"] or
                not native_name_matches or len(params) != 3 or bridge.get("is_static", False) or
                converter.type_shape(receiver.get("type", {})) != "*" + symbol.owner or
                receiver.get("ownership") != "borrow" or receiver.get("this_adjust") != 0 or
                converter.type_shape(params[0]["type"]) != "*QObject" or params[0]["ownership"] != "borrow" or
                converter.type_shape(params[1]["type"]) != converter.type_shape(callback) or params[1]["ownership"] != "copy" or
                converter.type_shape(params[2]["type"]) != "rawptr" or params[2]["ownership"] != "borrow" or
                converter.type_shape(bridge["return"]) != "bool"):
            raise ImportError(f"prepared DCI connection identity/signature mismatch: {origin}")
        result.append({"symbol": symbol, "mode": "signal", "count": len(symbol.params),
                       "name": name, "member": member, "profile": profile})
    return sorted(result, key=lambda c: c["name"])


def prepare_one(project, name, spec, settings, jobs, force):
    if not isinstance(name, str) or not re.fullmatch(r"[A-Za-z_]\w*", name):
        raise ImportError(f"invalid DCI import name: {name!r}")
    unknown = set(spec) - {"module", "headers", "profile", "project_roots", "triplet", "boundary", "std", "toolchain",
                           "export_all", "export_types", "export_functions", "qt_connections", "ownership_headers", "definitions", "contract"}
    if unknown:
        raise ImportError(f"dci.import.{name}: unknown keys {sorted(unknown)}")
    profile = spec.get("profile", "cpp")
    if profile not in {"cpp", "qt"}:
        raise ImportError(f"unknown C++ import profile {profile!r}")
    scope = export_scope(spec)
    module = spec["module"]
    authored_definitions = definition_source(project, name, spec)
    contract = contract_destination(project, name, spec)
    if not spec.get("headers"):
        raise ImportError(f"dci.import.{name}: headers are required")
    def path(value):
        return (project / expand(value)).resolve()
    headers = [path(h) for h in spec["headers"]]
    public_headers = list(headers)
    # Library ownership tables are author-selected declarations, not guesses
    # enabled by a library's name. They can also live in the public headers.
    headers += [path(h) for h in spec.get("ownership_headers", [])]
    if any(not h.is_file() for h in headers):
        raise ImportError(f"DCI public header missing: {headers}")
    flags = [expand(f) for f in settings.get("cxxflags", [])]
    includes = [path(p) for p in settings.get("include_paths", [])]
    flags += ["-I" + str(p) for p in includes]
    target = cpp_toolchains.canonical_target(spec.get("triplet", "windows_x64" if os.name == "nt" else "linux_x64"))
    compiler = expand(settings.get("cxx", "")) or None
    if compiler and ("/" in compiler or "\\" in compiler):
        compiler = str((project / compiler).resolve())
    toolchain, extractor = adapter.select_cpp_frontends(spec.get("toolchain", "clang"), compiler, compiler, target)
    std = spec.get("std", next((f.split("=", 1)[1] for f in flags if f.startswith("-std=")), "c++20"))
    output = project / ".cache" / "dci" / name
    output.mkdir(parents=True, exist_ok=True)
    stamp_path = output / "inputs.json"
    product_names = ["contract.dcib", "producer.hpp", "report.json", "producer.cpp"]
    if authored_definitions is None:
        product_names.append("import.vyx")
    products = {n: output / n for n in product_names}
    products["contract.dcib"] = contract
    probe = output / "input.cpp"
    write_changed(probe, "".join(f'#include "{h.as_posix()}"\n' for h in headers))
    dependencies = dependency_files(extractor.executable, std, target, probe, flags, output)
    roots = sorted(set([path(r) for r in spec.get("project_roots", [])] + [h.parent for h in headers]))
    inputs = {"config": [name, spec, settings, target, std, flags],
              "files": {p: digest(p) for p in dependencies},
              "exports": scope,
              "tools": {str(p.resolve()): digest(p) for p in Path(__file__).parent.glob("*.py")},
              "compilers": {str(p): digest(p) for p in {toolchain.executable, extractor.executable}}}
    schema_path = Path(__file__).with_name("schema") / "dci-1.0.schema.json"
    inputs["tools"][str(schema_path.resolve())] = digest(schema_path)
    fingerprint = hashlib.sha256(json.dumps(inputs, sort_keys=True).encode()).hexdigest()
    # Contract freshness is independent of cache state and local tool versions.
    # Plain offline Adapter contracts are authoritative inputs; generated ones
    # carry content hashes so a known producer-input change triggers measurement.
    producer_inputs = {"version": 1, "headers": [h.as_posix() for h in headers],
                       "files": {p: digest(p) for p in dependencies if Path(p).resolve() != probe.resolve()},
                       "target": target, "std": std, "flags": flags,
                       "boundary": spec.get("boundary", "no_unwind"), "exports": scope}
    if not force and stamp_path.exists():
        try:
            old = json.loads(stamp_path.read_text())
        except json.JSONDecodeError:
            old = None
        if not isinstance(old, dict) or not isinstance(old.get("outputs"), dict):
            print(f"[dci] {name}: invalid cache metadata; rebuilding from the project contract", flush=True)
            old = {"outputs": {}}
        if old.get("fingerprint") == fingerprint and all(p.is_file() and digest(p) == old["outputs"].get(p.name) for p in products.values()):
            if authored_definitions is not None:
                (output / "import.vyx").unlink(missing_ok=True)
            print(f"[dci] {name}: cached verified import ({module})")
            return
    # An unsuccessful regeneration cannot leave a valid success stamp.
    stamp_path.unlink(missing_ok=True)
    document = None
    if contract.is_file() and not force:
        document = dcib.decode(contract.read_bytes())
        validate_contract_document(document)
        previous_inputs = document.get("source", {}).get("preparation_inputs")
        if previous_inputs is not None and previous_inputs != producer_inputs:
            print(f"[dci] {name}: producer inputs changed; preparing a new contract", flush=True)
            document = None
        else:
            validate_prepared_contract(document, target, spec, scope)
    reused_contract = document is not None
    if reused_contract:
        print(f"[dci] {name}: using prepared contract {contract}", flush=True)
        adapter.CPP_LONG_WIDTH = cpp_toolchains.target_info(target).long_width
        adapter.CPP_POINTER_WIDTH = cpp_toolchains.target_info(target).pointer_width
        plan = prepared_connection_plan(document, profile, scope["qt_connections"])
        rejected = []
        producer = emit_producer(headers, plan)
    else:
        document, plan, rejected, producer = measure_selected_exports(
            name, module, scope, spec, headers, roots, probe, flags, output,
            target, std, toolchain, extractor, profile, jobs)
        document["source"]["preparation_inputs"] = producer_inputs
    if authored_definitions is None:
        relative_contract = Path(os.path.relpath(contract, output)).as_posix()
        text, binding_rejected = emit_import(document, module, relative_contract, [h.as_posix() for h in public_headers])
        rejected += binding_rejected
    report = {"module": module, "profile": profile, "headers": list(map(str, public_headers)),
              "export_scope": scope, "contract": str(contract), "reused_contract": reused_contract,
              "definitions": str(authored_definitions) if authored_definitions else None,
              "ownership_headers": list(map(str, headers[len(public_headers):])),
              "bridges": [{"symbol": c["name"], "owner": c["symbol"].owner, "member": c["member"],
                           "kind": c["mode"], "origin": adapter.symbol_selector(c["symbol"])} for c in plan],
              "rejected": rejected + document["exports"].get("rejected_symbols", [])}
    if not reused_contract:
        write_changed(products["contract.dcib"], dcib.encode(document))
    if authored_definitions is None:
        write_changed(products["import.vyx"], text)
    else:
        (output / "import.vyx").unlink(missing_ok=True)
    write_changed(products["producer.hpp"], producer)
    write_changed(products["report.json"], json.dumps(report, indent=2) + "\n")
    write_changed(products["producer.cpp"], '#include "producer.hpp"\n' + f'// DCI build input identity: {fingerprint}\n')
    write_changed(stamp_path, json.dumps({"fingerprint": fingerprint, "inputs": inputs,
                                        "outputs": {p.name: digest(p) for p in products.values()}}, indent=2) + "\n")
    print(f"[dci] {name}: prepared build artifacts, {len(plan)} measured connection operations; diagnostics: {products['report.json']}")


def measure_selected_exports(name, module, scope, spec, headers, roots, probe, flags, output,
                             target, std, toolchain, extractor, profile, jobs):
    project_headers = adapter.discover_project_headers(list(map(str, headers)), list(map(str, roots)))
    argv = [sys.executable, str(Path(__file__).with_name("dci.py")), "adapter", "--language", "cpp",
            "--toolchain", toolchain.family, "--compiler", toolchain.executable,
            "--extractor", extractor.executable, "--triplet", target, "--std", std,
            "--boundary", spec.get("boundary", "no_unwind"), "-j", str(jobs)]
    argv += [piece for r in roots for piece in ("--project-root", str(r))]
    argv += ["--extractor-arg=" + f for f in flags]
    argv += [piece for t in scope["types"] for piece in ("--export-type", t)]
    argv += [piece for f in scope["functions"] for piece in ("--export-function", f)]
    print(f"[dci] {name}: extracting producer API ({module})", flush=True)
    base = output / "base.dcib"
    run(argv + list(headers) + ["-o", base])
    document = dcib.decode(base.read_bytes())
    owners = {l["type_name"] for l in document["exports"]["layouts"] if re.fullmatch(r"[A-Za-z_]\w*", l["type_name"])}
    adapter.ADAPTER_JOBS = jobs
    adapter.CPP_LONG_WIDTH = cpp_toolchains.target_info(target).long_width
    adapter.CPP_POINTER_WIDTH = cpp_toolchains.target_info(target).pointer_width
    adapter.CPP_ENUM_UNDERLYING = adapter.discover_enum_underlying(project_headers)
    adapter.CPP_TYPE_ALIAS_TARGETS = {adapter.normalize_cpp_type(a.name): a.target for a in adapter.discover_type_aliases(project_headers)}
    methods = []
    if scope["qt_connections"]:
        for text in adapter.clang_ast_dump_filter_texts(extractor.executable, std, target, probe, flags, sorted(owners)):
            nodes = adapter.json_stream(text)
            adapter.harvest_ast_type_aliases(nodes)
            methods.extend(harvest_methods(nodes, owners, profile))
    unique = {adapter.symbol_identity(m[0]): m for m in methods}
    rules = adapter.discover_ownership_annotations(project_headers)
    plan, rejected = bridge_plan(list(unique.values()), owners, rules, profile, scope["qt_connections"])
    producer = emit_producer(headers, plan)
    # Measure in a temporary sibling directory, then publish a complete tuple.
    # Old successful outputs are retained if the producer rejects this build.
    with tempfile.TemporaryDirectory(prefix="measure-", dir=output) as temporary:
        temp = Path(temporary)
        bridge = temp / "producer.hpp"
        bridge.write_text(producer, encoding="utf-8")
        measured = temp / "measured.dcib"
        bridge_functions = [piece for c in plan for piece in ("--export-function", c["name"])]
        run(argv + list(headers) + [str(bridge), "--project-root", str(temp), *bridge_functions, "-o", measured])
        document = dcib.decode(measured.read_bytes())
        def relocate(value):
            if isinstance(value, dict):
                return {k: relocate(v) for k, v in value.items()}
            if isinstance(value, list):
                return [relocate(v) for v in value]
            if isinstance(value, str):
                for prefix in (str(temp), temp.as_posix()):
                    if value.startswith(prefix + "/") or value.startswith(prefix + "\\"):
                        return output.as_posix() + "/" + value[len(prefix) + 1:].replace("\\", "/")
            return value
        document = relocate(document)
    rejected += adopt_bridges(document, plan)
    # Public source inputs never point at the deleted measurement directory.
    final_header = (output / "producer.hpp").as_posix()
    source = document.get("source", {})
    if isinstance(source, dict):
        source["headers"] = [h.as_posix() for h in headers] + [final_header]
    return document, plan, rejected, producer
