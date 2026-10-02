#!/usr/bin/env python3
"""Validate DCI JSON documents against the language-neutral 1.0 schema."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any, Iterable


DEFAULT_SCHEMA = Path(__file__).resolve().parent / "schema" / "dci-1.0.schema.json"

DIRECT_TABLE_ADJUSTMENT_FIELDS = (
    "table_pointer_offset",
    "table_entry_offset",
    "table_entry_size",
    "table_entry_signed",
    "displacement_base_offset",
    "result",
    "null_preserving",
)
SUPPORTED_TABLE_ENTRY_SIZES = {1, 2, 4, 8}
DIRECT_RUNTIME_CAST_FIELDS = (
    "source_type",
    "target_type",
    "failure",
    "null_behavior",
)


class DciValidationError(Exception):
    pass


def load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except OSError as exc:
        raise DciValidationError(f"cannot read {path}: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise DciValidationError(
            f"invalid JSON in {path}:{exc.lineno}:{exc.colno}: {exc.msg}"
        ) from exc


def json_path(parts: Iterable[Any]) -> str:
    out = "$"
    for part in parts:
        if isinstance(part, int):
            out += f"[{part}]"
        else:
            out += "." + str(part)
    return out


def schema_errors(schema: dict[str, Any], document: Any) -> list[str]:
    try:
        from jsonschema import Draft202012Validator
    except ImportError as exc:
        raise DciValidationError(
            "the 'jsonschema' package is required (install jsonschema>=4.18)"
        ) from exc

    try:
        Draft202012Validator.check_schema(schema)
    except Exception as exc:
        raise DciValidationError(f"invalid DCI schema: {exc}") from exc

    validator = Draft202012Validator(schema)
    errors = sorted(validator.iter_errors(document), key=lambda err: list(err.absolute_path))
    return [f"{json_path(error.absolute_path)}: {error.message}" for error in errors]


def operation_symbol_refs(operation: Any) -> Iterable[str]:
    if isinstance(operation, dict):
        symbol = operation.get("symbol")
        if isinstance(symbol, str) and symbol:
            yield symbol


def operation_has_implementation(operation: Any) -> bool:
    if not isinstance(operation, dict):
        return False
    if isinstance(operation.get("symbol"), str) and operation["symbol"]:
        return True
    if isinstance(operation.get("intrinsic"), str) and operation["intrinsic"]:
        return True
    dispatch_table = operation.get("dispatch_table")
    slot = operation.get("slot")
    return (
        isinstance(dispatch_table, str)
        and bool(dispatch_table)
        and isinstance(slot, int)
        and not isinstance(slot, bool)
        and slot >= 0
    )


def is_positive_power_of_two(value: Any) -> bool:
    return (
        isinstance(value, int)
        and not isinstance(value, bool)
        and value > 0
        and value & (value - 1) == 0
    )


def primitive_byte_size(type_ref: Any, pointer_width: Any) -> int | None:
    if not isinstance(type_ref, dict) or type_ref.get("kind") != "primitive":
        return None
    name = type_ref.get("name")
    if not isinstance(name, str):
        return None
    if name == "bool":
        return 1
    if name in {"isize", "usize"}:
        if (
            isinstance(pointer_width, int)
            and not isinstance(pointer_width, bool)
            and pointer_width > 0
            and pointer_width % 8 == 0
        ):
            return pointer_width // 8
        return None
    width_match = re.fullmatch(r"[iuf](\d+)", name)
    if width_match is None:
        return None
    width = int(width_match.group(1))
    return width // 8 if width > 0 and width % 8 == 0 else None


def type_ref_byte_size(
    type_ref: Any,
    layouts: Any,
    aliases: dict[str, str],
    pointer_width: Any,
) -> int | None:
    if not isinstance(type_ref, dict):
        return None
    reference = type_ref.get("reference", "value")
    if reference != "value":
        # A Rust slice/str/trait-object reference is a wide value at the
        # producer boundary. Its consumer-facing type still uses
        # `reference: pointer`, but the measured `size` and `wide` fields carry
        # the two-word ABI that split lowering must validate.
        declared_size = type_ref.get("size")
        if (
            type_ref.get("wide") is True
            and isinstance(declared_size, int)
            and not isinstance(declared_size, bool)
            and declared_size > 0
        ):
            return declared_size
        if (
            isinstance(pointer_width, int)
            and not isinstance(pointer_width, bool)
            and pointer_width > 0
            and pointer_width % 8 == 0
        ):
            return pointer_width // 8
        return None
    declared_size = type_ref.get("size")
    if (
        isinstance(declared_size, int)
        and not isinstance(declared_size, bool)
        and declared_size >= 0
    ):
        return declared_size
    primitive_size = primitive_byte_size(type_ref, pointer_width)
    if primitive_size is not None:
        return primitive_size
    if not isinstance(layouts, list):
        return None

    candidate_names: set[str] = set()
    for key in ("name", "cpp_type"):
        candidate = type_ref.get(key)
        if isinstance(candidate, str) and candidate:
            candidate_names.add(candidate)
            candidate_names.add(resolve_type_alias(candidate, aliases))
    matched_sizes: set[int] = set()
    for layout in layouts:
        if not isinstance(layout, dict):
            continue
        layout_name = layout.get("type_name")
        layout_size = layout.get("size")
        if (
            not isinstance(layout_name, str)
            or not layout_name
            or not isinstance(layout_size, int)
            or isinstance(layout_size, bool)
            or layout_size < 0
        ):
            continue
        layout_names = {layout_name, resolve_type_alias(layout_name, aliases)}
        if candidate_names & layout_names:
            matched_sizes.add(layout_size)
    if len(matched_sizes) == 1:
        return next(iter(matched_sizes))
    return None


_LLVM_ABI_TOKEN = re.compile(r"^(ptr|half|float|double|fp128|i\d+)$")


def append_abi_register_errors(errors: list[str], path: str, lowering: dict) -> None:
    passing = lowering.get("passing")
    registers = lowering.get("registers")
    offsets = lowering.get("register_offsets")
    if passing == "split":
        if not isinstance(registers, list) or not registers:
            errors.append(f"{path}.registers: split lowering requires compiler LLVM types")
            return
        if not isinstance(offsets, list) or len(offsets) != len(registers):
            errors.append(f"{path}.register_offsets: must match registers")
            return
    elif passing == "coerce" and isinstance(registers, list) and registers:
        if offsets is not None and (
            not isinstance(offsets, list) or len(offsets) != len(registers)
        ):
            errors.append(f"{path}.register_offsets: must match registers")
            return
    else:
        return
    for index, token in enumerate(registers):
        if not isinstance(token, str) or _LLVM_ABI_TOKEN.fullmatch(token) is None:
            errors.append(f"{path}.registers[{index}]: unknown LLVM type token {token!r}")
        if isinstance(offsets, list) and index < len(offsets):
            offset = offsets[index]
            if not isinstance(offset, int) or isinstance(offset, bool) or offset < 0:
                errors.append(f"{path}.register_offsets[{index}]: must be a non-negative integer")


def append_abi_value_lowering_errors(
    errors: list[str],
    path: str,
    lowering: Any,
    strict: bool,
    source_type: Any = None,
    layouts: Any = None,
    aliases: dict[str, str] | None = None,
    pointer_width: Any = None,
) -> None:
    if not isinstance(lowering, dict):
        return
    passing = lowering.get("passing")
    alignment = lowering.get("alignment")
    if alignment is not None and not is_positive_power_of_two(alignment):
        errors.append(f"{path}.alignment: must be a positive power of two")
    if not strict:
        return

    source_size = type_ref_byte_size(
        source_type, layouts, aliases or {}, pointer_width
    )
    if passing in {"indirect", "byval", "sret"}:
        size = lowering.get("size")
        if not isinstance(size, int) or isinstance(size, bool) or size <= 0:
            errors.append(f"{path}.size: {passing} lowering requires a positive byte size")
        elif source_size is not None and size != source_size:
            errors.append(
                f"{path}.size: {passing} lowering size {size} does not match "
                f"the {source_size}-byte source type"
            )
        if not is_positive_power_of_two(alignment):
            errors.append(
                f"{path}.alignment: {passing} lowering requires a positive power-of-two alignment"
            )
    elif passing in {"coerce", "split"}:
        size = lowering.get("size")
        if not isinstance(size, int) or isinstance(size, bool) or size <= 0:
            errors.append(f"{path}.size: {passing} lowering requires a positive byte size")
            append_abi_register_errors(errors, path, lowering)
            return
        if source_size is not None and size != source_size:
            errors.append(
                f"{path}.size: {passing} lowering size {size} does not match "
                f"the {source_size}-byte source type"
            )
        if passing == "coerce":
            coerce_to = lowering.get("coerce_to")
            registers = lowering.get("registers")
            if not isinstance(coerce_to, dict) and not (
                isinstance(registers, list) and registers
            ):
                errors.append(f"{path}.coerce_to: required for coerce lowering")
            if isinstance(coerce_to, dict):
                coerce_size = type_ref_byte_size(
                    coerce_to, layouts, aliases or {}, pointer_width
                )
                if coerce_size is not None and size != coerce_size:
                    errors.append(
                        f"{path}.coerce_to: target width is {coerce_size} bytes but "
                        f"lowering size is {size}"
                    )
        append_abi_register_errors(errors, path, lowering)


def table_adjustment_is_directly_executable(adjustment: Any) -> bool:
    if not isinstance(adjustment, dict) or adjustment.get("kind") != "table":
        return False
    if not all(field in adjustment for field in DIRECT_TABLE_ADJUSTMENT_FIELDS):
        return False
    integer_fields = (
        "table_pointer_offset",
        "table_entry_offset",
        "table_entry_size",
        "displacement_base_offset",
    )
    if not all(
        isinstance(adjustment.get(field), int)
        and not isinstance(adjustment.get(field), bool)
        for field in integer_fields
    ):
        return False
    if adjustment["table_entry_size"] not in SUPPORTED_TABLE_ENTRY_SIZES:
        return False
    if adjustment.get("result") != "object_plus_base_plus_displacement":
        return False
    return (
        isinstance(adjustment.get("table_entry_signed"), bool)
        and isinstance(adjustment.get("null_preserving"), bool)
    )


def canonical_json(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


TYPE_REF_KINDS = {
    "primitive",
    "enum",
    "record",
    "class",
    "interface",
    "opaque",
    "function",
    "vector",
}


def type_alias_targets(exports: dict[str, Any]) -> dict[str, str]:
    targets: dict[str, str] = {}
    aliases = exports.get("aliases")
    if not isinstance(aliases, list):
        return targets
    for alias in aliases:
        if not isinstance(alias, dict):
            continue
        name = alias.get("name")
        target = alias.get("target")
        if isinstance(name, str) and name and isinstance(target, str) and target:
            targets[name] = target
    return targets


def resolve_type_alias(name: str, aliases: dict[str, str]) -> str:
    current = name
    visited: set[str] = set()
    while current in aliases and current not in visited:
        visited.add(current)
        current = aliases[current]
    if current in visited:
        return min(visited)
    return current


def normalize_interop_value(value: Any, aliases: dict[str, str]) -> Any:
    if isinstance(value, list):
        return [normalize_interop_value(item, aliases) for item in value]
    if not isinstance(value, dict):
        return value
    normalized = {
        key: normalize_interop_value(child, aliases)
        for key, child in value.items()
    }
    if normalized.get("kind") in TYPE_REF_KINDS and isinstance(
        normalized.get("name"), str
    ):
        normalized["name"] = resolve_type_alias(normalized["name"], aliases)
        # Source spelling is adapter evidence, not part of the callable ABI.
        normalized.pop("cpp_type", None)
    return normalized


def symbol_binding_identity(symbol: dict[str, Any]) -> tuple[Any, ...]:
    params = symbol.get("params") if isinstance(symbol.get("params"), list) else []
    param_types = [
        parameter.get("type") if isinstance(parameter, dict) else None
        for parameter in params
    ]
    returned = symbol.get("return")
    return_type = returned.get("type") if isinstance(returned, dict) else None
    return (
        symbol.get("kind"),
        symbol.get("owner", ""),
        symbol.get("member_name") or symbol.get("name"),
        bool(symbol.get("is_static")),
        bool(symbol.get("is_const")),
        canonical_json(param_types),
        canonical_json(return_type),
    )


def symbol_interop_contract(
    symbol: dict[str, Any], aliases: dict[str, str] | None = None
) -> dict[str, Any]:
    alias_targets = aliases or {}
    params = normalize_interop_value(symbol.get("params"), alias_targets)
    if isinstance(params, list):
        params = [
            {key: value for key, value in parameter.items() if key != "name"}
            if isinstance(parameter, dict)
            else parameter
            for parameter in params
        ]
    abi = normalize_interop_value(symbol.get("abi"), alias_targets)
    if isinstance(abi, dict):
        for key in ("params", "parameters", "hidden_parameters"):
            lowerings = abi.get(key)
            if isinstance(lowerings, list):
                abi[key] = [
                    {field: value for field, value in lowering.items() if field != "name"}
                    if isinstance(lowering, dict)
                    else lowering
                    for lowering in lowerings
                ]
    return {
        "calling_convention": symbol.get("calling_convention"),
        "params": params,
        "return": normalize_interop_value(symbol.get("return"), alias_targets),
        "abi": abi,
        "control_flow": symbol.get("control_flow"),
    }


def symbol_abi_contract(
    symbol: dict[str, Any], aliases: dict[str, str] | None = None
) -> str:
    contract = symbol_interop_contract(symbol, aliases)
    contract["link_name"] = symbol.get("link_name") or symbol.get("mangled")
    return canonical_json(contract)


def symbol_link_identity_contract(
    symbol: dict[str, Any], aliases: dict[str, str] | None = None
) -> str:
    return canonical_json(symbol_interop_contract(symbol, aliases))


def append_direct_runtime_cast_symbol_errors(
    errors: list[str], path: str, operation: dict[str, Any], symbol: dict[str, Any]
) -> None:
    params = symbol.get("params")
    returned = symbol.get("return")
    if symbol.get("kind") != "function" or not isinstance(params, list) or len(params) != 1:
        errors.append(f"{path}.symbol: Direct runtime cast must reference a one-parameter function")
        return
    parameter = params[0] if isinstance(params[0], dict) else {}
    parameter_type = parameter.get("type") if isinstance(parameter.get("type"), dict) else {}
    return_type = returned.get("type") if isinstance(returned, dict) and isinstance(
        returned.get("type"), dict
    ) else {}
    if (
        parameter_type.get("name") != operation.get("source_type")
        or parameter_type.get("reference") != "pointer"
        or parameter_type.get("nullable") is not True
        or parameter.get("ownership") != "borrow"
    ):
        errors.append(
            f"{path}.symbol: Direct runtime cast source must be a nullable borrowed pointer "
            "matching source_type"
        )
    if (
        not isinstance(returned, dict)
        or return_type.get("name") != operation.get("target_type")
        or return_type.get("reference") != "pointer"
        or return_type.get("nullable") is not True
        or returned.get("ownership") != "borrow"
    ):
        errors.append(
            f"{path}.symbol: Direct runtime cast result must be a nullable borrowed pointer "
            "matching target_type"
        )
    abi = symbol.get("abi")
    abi_params = abi.get("parameters") if isinstance(abi, dict) else None
    abi_return = abi.get("return") if isinstance(abi, dict) else None
    if (
        not isinstance(abi_params, list)
        or len(abi_params) != 1
        or not isinstance(abi_params[0], dict)
        or abi_params[0].get("passing") != "direct"
        or not isinstance(abi_return, dict)
        or abi_return.get("passing") != "direct"
    ):
        errors.append(f"{path}.symbol: Direct runtime cast requires direct pointer ABI lowering")


def iter_objects(
    value: Any, path: tuple[Any, ...] = ()
) -> Iterable[tuple[tuple[Any, ...], dict[str, Any]]]:
    if isinstance(value, dict):
        yield path, value
        for key, child in value.items():
            yield from iter_objects(child, path + (key,))
    elif isinstance(value, list):
        for index, child in enumerate(value):
            yield from iter_objects(child, path + (index,))


def append_control_flow_errors(errors: list[str], path: str, control_flow: Any) -> None:
    if not isinstance(control_flow, dict):
        return
    boundary = control_flow.get("default_boundary")
    propagation = control_flow.get("propagation")
    propagation_mode = propagation.get("mode") if isinstance(propagation, dict) else None

    if boundary in {"translated", "shared_abi"} and propagation_mode != boundary:
        errors.append(
            f"{path}.propagation.mode: {boundary} boundary requires {boundary} propagation"
        )

    effective_mode = boundary if boundary in {"translated", "shared_abi"} else propagation_mode
    if effective_mode == "translated":
        if not isinstance(propagation, dict) or not propagation.get("translator_symbol"):
            errors.append(
                f"{path}.propagation.translator_symbol: required for translated propagation"
            )
    elif effective_mode == "shared_abi":
        if not isinstance(propagation, dict) or not propagation.get("abi"):
            errors.append(f"{path}.propagation.abi: required for shared_abi propagation")


def semantic_errors(document: dict[str, Any], strict: bool) -> list[str]:
    errors: list[str] = []
    exports = document.get("exports")
    if not isinstance(exports, dict):
        return errors

    profile = document.get("profile")
    consumer_modes: set[str] = set()
    if isinstance(profile, dict) and isinstance(profile.get("consumer_modes"), list):
        consumer_modes = {
            mode for mode in profile["consumer_modes"] if isinstance(mode, str)
        }

    if strict:
        for key in ("profile", "target", "control_flow"):
            if key not in document:
                errors.append(f"$.{key}: required by --strict for a portable DCI document")
        target = document.get("target")
        if isinstance(target, dict):
            for key in ("triple", "architecture", "pointer_width", "endianness"):
                if key not in target:
                    errors.append(f"$.target.{key}: required by --strict")
            if not target.get("abi") and not target.get("abi_family"):
                errors.append("$.target: --strict requires abi or abi_family")

    root_target = document.get("target")
    source = document.get("source")
    legacy_target = source.get("target") if isinstance(source, dict) else None
    if isinstance(root_target, dict) and isinstance(legacy_target, dict):
        root_triple = root_target.get("triple")
        legacy_triple = legacy_target.get("triple")
        if root_triple and legacy_triple and root_triple != legacy_triple:
            errors.append("$.target.triple: differs from $.source.target.triple")

    for collection, key in (("aliases", "name"), ("layouts", "type_name")):
        seen: set[str] = set()
        for index, item in enumerate(exports.get(collection, [])):
            if not isinstance(item, dict):
                continue
            value = item.get(key)
            if isinstance(value, str) and value:
                if value in seen:
                    errors.append(f"$.exports.{collection}[{index}].{key}: duplicate {value!r}")
                seen.add(value)

    symbol_refs: set[str] = set()
    symbols_by_ref: dict[str, list[tuple[int, dict[str, Any]]]] = {}
    symbol_bindings: dict[tuple[Any, ...], tuple[int, str]] = {}
    link_identities: dict[str, tuple[int, str, str]] = {}
    alias_targets = type_alias_targets(exports)
    exported_layouts = exports.get("layouts", [])
    pointer_width = root_target.get("pointer_width") if isinstance(root_target, dict) else None
    symbols = exports.get("symbols", [])
    for index, symbol in enumerate(symbols):
        if not isinstance(symbol, dict):
            continue
        binding_identity = symbol_binding_identity(symbol)
        binding_contract = symbol_abi_contract(symbol)
        previous_binding = symbol_bindings.get(binding_identity)
        if previous_binding is not None and previous_binding[1] != binding_contract:
            errors.append(
                f"$.exports.symbols[{index}]: conflicting ABI contract for the same "
                f"binding identity as $.exports.symbols[{previous_binding[0]}]"
            )
        else:
            symbol_bindings.setdefault(binding_identity, (index, binding_contract))
        link_contract = symbol_link_identity_contract(symbol, alias_targets)
        declared_link_identities: dict[str, str] = {}
        for key in ("link_name", "mangled"):
            value = symbol.get(key)
            if isinstance(value, str) and value:
                declared_link_identities.setdefault(value, key)
        for value, key in declared_link_identities.items():
            previous_link = link_identities.get(value)
            if previous_link is not None and previous_link[2] != link_contract:
                errors.append(
                    f"$.exports.symbols[{index}].{key}: conflicting ABI, ownership, or "
                    f"control-flow contract for link identity {value!r} declared by "
                    f"$.exports.symbols[{previous_link[0]}].{previous_link[1]}"
                )
            else:
                link_identities.setdefault(value, (index, key, link_contract))
        for key in ("name", "semantic_id", "link_name", "mangled"):
            value = symbol.get(key)
            if isinstance(value, str) and value:
                symbol_refs.add(value)
                symbols_by_ref.setdefault(value, []).append((index, symbol))
        if strict:
            if not symbol.get("link_name"):
                errors.append(
                    f"$.exports.symbols[{index}].link_name: required by --strict; "
                    "mangled is only a compatibility alias"
                )
            if "abi" not in symbol:
                errors.append(f"$.exports.symbols[{index}].abi: required by --strict")

            if isinstance(symbol.get("params"), list):
                for param_index, parameter in enumerate(symbol["params"]):
                    if isinstance(parameter, dict) and "ownership" not in parameter:
                        errors.append(
                            f"$.exports.symbols[{index}].params[{param_index}].ownership: "
                            "required by --strict"
                        )
            returned = symbol.get("return")
            if isinstance(returned, dict) and "ownership" not in returned:
                errors.append(
                    f"$.exports.symbols[{index}].return.ownership: required by --strict"
                )

        abi = symbol.get("abi")
        params = symbol.get("params")
        if isinstance(abi, dict) and isinstance(params, list):
            lowered = abi.get("parameters", [])
            seen_indices: set[int] = set()
            for lowering_index, lowering in enumerate(lowered):
                if not isinstance(lowering, dict):
                    continue
                param_index = lowering.get("index", lowering_index)
                source_type = None
                if (
                    not isinstance(param_index, int)
                    or isinstance(param_index, bool)
                    or not 0 <= param_index < len(params)
                ):
                    errors.append(
                        f"$.exports.symbols[{index}].abi.parameters[{lowering_index}].index: "
                        f"must select one of {len(params)} source parameters"
                    )
                elif param_index in seen_indices:
                    errors.append(
                        f"$.exports.symbols[{index}].abi.parameters[{lowering_index}].index: "
                        f"duplicate lowering for parameter {param_index}"
                    )
                else:
                    seen_indices.add(param_index)
                    parameter = params[param_index]
                    if isinstance(parameter, dict):
                        source_type = parameter.get("type")
                append_abi_value_lowering_errors(
                    errors,
                    f"$.exports.symbols[{index}].abi.parameters[{lowering_index}]",
                    lowering,
                    strict,
                    source_type,
                    exported_layouts,
                    alias_targets,
                    pointer_width,
                )
            missing_indices = sorted(set(range(len(params))) - seen_indices)
            if missing_indices:
                errors.append(
                    f"$.exports.symbols[{index}].abi.parameters: missing lowering for "
                    f"source parameter indices {missing_indices}"
                )
        if isinstance(abi, dict):
            returned = symbol.get("return")
            return_type = returned.get("type") if isinstance(returned, dict) else None
            append_abi_value_lowering_errors(
                errors,
                f"$.exports.symbols[{index}].abi.return",
                abi.get("return"),
                strict,
                return_type,
                exported_layouts,
                alias_targets,
                pointer_width,
            )
            hidden_parameters = abi.get("hidden_parameters", [])
            if isinstance(hidden_parameters, list):
                for hidden_index, lowering in enumerate(hidden_parameters):
                    hidden_source_type = (
                        return_type
                        if isinstance(lowering, dict) and lowering.get("passing") == "sret"
                        else None
                    )
                    append_abi_value_lowering_errors(
                        errors,
                        f"$.exports.symbols[{index}].abi.hidden_parameters[{hidden_index}]",
                        lowering,
                        strict,
                        hidden_source_type,
                        exported_layouts,
                        alias_targets,
                        pointer_width,
                    )

    dispatch_tables = exports.get("dispatch_tables")
    legacy_vtables = exports.get("vtables")
    if (
        strict
        and isinstance(dispatch_tables, list)
        and dispatch_tables
        and isinstance(legacy_vtables, list)
        and legacy_vtables
        and dispatch_tables != legacy_vtables
    ):
        errors.append(
            "$.exports.dispatch_tables: differs from the compatibility alias $.exports.vtables"
        )
    effective_dispatch_tables = (
        dispatch_tables
        if isinstance(dispatch_tables, list) and dispatch_tables
        else legacy_vtables if isinstance(legacy_vtables, list) else []
    )
    dispatch_by_id: dict[str, tuple[int, dict[str, Any]]] = {}
    for table_index, table in enumerate(effective_dispatch_tables):
        if not isinstance(table, dict):
            continue
        table_id = table.get("id")
        if not isinstance(table_id, str) or not table_id:
            continue
        if table_id in dispatch_by_id:
            errors.append(
                f"$.exports.dispatch_tables[{table_index}].id: duplicate dispatch table {table_id!r}"
            )
        else:
            dispatch_by_id[table_id] = (table_index, table)

    operation_refs: list[tuple[str, str]] = []
    operation_contracts: list[tuple[str, dict[str, Any]]] = []
    for layout_index, layout in enumerate(exports.get("layouts", [])):
        if not isinstance(layout, dict):
            continue
        size = layout.get("size")
        alignment = layout.get("alignment")
        representation = layout.get("representation")
        lifecycle_evidence = layout.get("lifecycle")
        if representation in {"stable", "transparent"} and not isinstance(
            lifecycle_evidence, dict
        ):
            if layout.get("is_pod") is not True or layout.get(
                "is_trivially_destructible"
            ) is not True:
                errors.append(
                    f"$.exports.layouts[{layout_index}]: stable value layout must declare "
                    "lifecycle or explicit trivial POD/destruction evidence"
                )
        if isinstance(alignment, int) and alignment > 0 and alignment & (alignment - 1):
            errors.append(
                f"$.exports.layouts[{layout_index}].alignment: must be a power of two"
            )
        for field_index, field in enumerate(layout.get("fields", [])):
            if not isinstance(field, dict):
                continue
            offset = field.get("offset")
            if isinstance(size, int) and isinstance(offset, int) and size > 0 and offset >= size:
                errors.append(
                    f"$.exports.layouts[{layout_index}].fields[{field_index}].offset: "
                    f"offset {offset} is outside {size}-byte layout"
                )
            bitfield = field.get("bitfield")
            if isinstance(bitfield, dict):
                bitfield_path = (
                    f"$.exports.layouts[{layout_index}].fields[{field_index}].bitfield"
                )
                bit_offset = bitfield.get("bit_offset")
                bit_width = bitfield.get("bit_width")
                storage_size = bitfield.get("storage_size")
                storage_offset = bitfield.get("storage_offset")
                if all(isinstance(v, int) for v in (bit_offset, bit_width, storage_size)):
                    if bit_offset + bit_width > storage_size * 8:
                        errors.append(
                            f"{bitfield_path}: "
                            "bit range exceeds its storage unit"
                        )
                if all(isinstance(v, int) for v in (storage_offset, storage_size, size)):
                    if storage_offset + storage_size > size:
                        errors.append(
                            f"{bitfield_path}: "
                            "storage unit exceeds its containing layout"
                        )
                if strict and bitfield.get("bit_order") == "target":
                    errors.append(
                        f"{bitfield_path}.bit_order: 'target' has no normalized Profile "
                        "interpretation for a portable Direct contract"
                    )
                for mode_key, operation_key in (
                    ("read", "read_operation"),
                    ("write", "write_operation"),
                ):
                    if bitfield.get(mode_key) == "operation":
                        operation = bitfield.get(operation_key)
                        if operation_key not in bitfield:
                            errors.append(
                                f"{bitfield_path}.{operation_key}: "
                                f"required when {mode_key} is 'operation'"
                            )
                        elif not operation_has_implementation(operation):
                            errors.append(
                                f"{bitfield_path}.{operation_key}: operation mode requires "
                                "a symbol, dispatch table/slot, or intrinsic"
                            )
                for op_name in ("read_operation", "write_operation"):
                    operation = bitfield.get(op_name)
                    if isinstance(operation, dict):
                        operation_contracts.append(
                            (f"{bitfield_path}.{op_name}", operation)
                        )
                    for ref in operation_symbol_refs(operation):
                        operation_refs.append(
                            (f"{bitfield_path}.{op_name}", ref)
                        )

        for base_index, base in enumerate(layout.get("bases", [])):
            if not isinstance(base, dict):
                continue
            adjustment = base.get("adjustment")
            path = f"$.exports.layouts[{layout_index}].bases[{base_index}].adjustment"
            if not strict:
                continue
            if not isinstance(adjustment, dict):
                if base.get("is_virtual") is True:
                    errors.append(f"{path}: required for a virtual base")
                continue
            adjustment_kind = adjustment.get("kind")
            if adjustment_kind == "constant":
                if base.get("is_virtual") is True:
                    errors.append(f"{path}: a virtual base cannot use a constant adjustment")
                    continue
                offset = adjustment.get("offset")
                if not isinstance(offset, int) or isinstance(offset, bool):
                    errors.append(f"{path}.offset: required for a constant adjustment")
                continue
            if adjustment_kind == "operation":
                operation = adjustment.get("operation")
                if not operation_has_implementation(operation):
                    errors.append(
                        f"{path}.operation: operation adjustment requires a symbol, "
                        "dispatch table/slot, or intrinsic"
                    )
                if isinstance(operation, dict):
                    operation_contracts.append((f"{path}.operation", operation))
                for ref in operation_symbol_refs(operation):
                    operation_refs.append((f"{path}.operation", ref))
                continue
            if adjustment_kind != "table":
                continue
            if table_adjustment_is_directly_executable(adjustment):
                continue

            fallback = adjustment.get("fallback_complete_object_offset")
            direct_semantic_fields = set(DIRECT_TABLE_ADJUSTMENT_FIELDS) - {
                "null_preserving"
            }
            has_partial_direct_contract = any(
                field in adjustment for field in direct_semantic_fields
            )
            if (
                isinstance(fallback, int)
                and not isinstance(fallback, bool)
                and not has_partial_direct_contract
            ):
                if consumer_modes and "stub" not in consumer_modes:
                    errors.append(
                        f"{path}: fallback_complete_object_offset is descriptive only; "
                        "this table adjustment requires the stub consumer mode"
                    )
                continue

            missing = [
                field for field in DIRECT_TABLE_ADJUSTMENT_FIELDS if field not in adjustment
            ]
            detail = f"; missing {', '.join(missing)}" if missing else ""
            errors.append(
                f"{path}: incomplete or unsupported table adjustment is not directly executable{detail}"
            )

        lifecycle = layout.get("lifecycle")
        if isinstance(lifecycle, dict):
            operations = lifecycle.get("operations", {})
            if isinstance(operations, dict):
                required_operations = {
                    "operation": {
                        "copy_semantics": "copy",
                        "move_semantics": "move",
                        "destruction": "destroy",
                    },
                    "retain": {"copy_semantics": "retain"},
                    "release": {"destruction": "release"},
                }
                for semantic_value, requirements in required_operations.items():
                    for semantic_key, operation_name in requirements.items():
                        if lifecycle.get(semantic_key) == semantic_value:
                            operation_path = (
                                f"$.exports.layouts[{layout_index}].lifecycle.operations.{operation_name}"
                            )
                            operation = operations.get(operation_name)
                            if operation_name not in operations:
                                errors.append(
                                    f"{operation_path}: required when {semantic_key} "
                                    f"is {semantic_value!r}"
                                )
                            elif not operation_has_implementation(operation):
                                errors.append(
                                    f"{operation_path}: required lifecycle operation needs a "
                                    "symbol, dispatch table/slot, or intrinsic"
                                )
                for op_name, operation in operations.items():
                    if isinstance(operation, dict):
                        operation_contracts.append(
                            (f"$.exports.layouts[{layout_index}].lifecycle.operations.{op_name}", operation)
                        )
                    for ref in operation_symbol_refs(operation):
                        operation_refs.append(
                            (f"$.exports.layouts[{layout_index}].lifecycle.operations.{op_name}", ref)
                        )

    runtime_type_names: set[str] = set()
    for rto_index, runtime_ops in enumerate(exports.get("runtime_type_operations", [])):
        if not isinstance(runtime_ops, dict):
            continue
        runtime_type_name = runtime_ops.get("type_name")
        if isinstance(runtime_type_name, str) and runtime_type_name:
            if runtime_type_name in runtime_type_names:
                errors.append(
                    f"$.exports.runtime_type_operations[{rto_index}].type_name: "
                    f"duplicate {runtime_type_name!r}"
                )
            runtime_type_names.add(runtime_type_name)
        operations = runtime_ops.get("operations", {})
        if isinstance(operations, dict):
            for capability in runtime_ops.get("capabilities", []):
                if capability not in operations:
                    errors.append(
                        f"$.exports.runtime_type_operations[{rto_index}].operations.{capability}: "
                        "required by the declared capability"
                    )
            for op_name, operation in operations.items():
                if isinstance(operation, dict):
                    operation_contracts.append(
                        (f"$.exports.runtime_type_operations[{rto_index}].operations.{op_name}", operation)
                    )
                if (
                    isinstance(operation, dict)
                    and operation.get("availability") == "required"
                    and not operation_has_implementation(operation)
                ):
                    errors.append(
                        f"$.exports.runtime_type_operations[{rto_index}].operations.{op_name}: "
                        "availability 'required' needs a symbol, dispatch table/slot, or intrinsic"
                    )
                if (
                    op_name in {"checked_downcast", "downcast", "cast"}
                    and isinstance(operation, dict)
                    and operation_has_implementation(operation)
                    and operation.get("requires_stub") is not True
                ):
                    for field in DIRECT_RUNTIME_CAST_FIELDS:
                        if not isinstance(operation.get(field), str) or not operation[field]:
                            errors.append(
                                f"$.exports.runtime_type_operations[{rto_index}].operations.{op_name}.{field}: "
                                "required for a Direct runtime cast operation"
                            )
                    if operation.get("no_unwind") is not True:
                        errors.append(
                            f"$.exports.runtime_type_operations[{rto_index}].operations.{op_name}.no_unwind: "
                            "must be true for a Direct runtime cast operation"
                        )
                    symbol_ref = operation.get("symbol")
                    if isinstance(symbol_ref, str) and symbol_ref:
                        candidates = symbols_by_ref.get(symbol_ref, [])
                        if candidates:
                            append_direct_runtime_cast_symbol_errors(
                                errors,
                                f"$.exports.runtime_type_operations[{rto_index}].operations.{op_name}",
                                operation,
                                candidates[0][1],
                            )
                for ref in operation_symbol_refs(operation):
                    operation_refs.append(
                        (f"$.exports.runtime_type_operations[{rto_index}].operations.{op_name}", ref)
                    )

    stub_requests = exports.get("stub_requests")
    if isinstance(stub_requests, list) and stub_requests:
        if consumer_modes and "stub" not in consumer_modes:
            errors.append(
                "$.exports.stub_requests: present but the profile does not declare "
                "the stub consumer mode"
            )
        request_ids: set[str] = set()
        wrapper_names: set[str] = set()
        for request_index, request in enumerate(stub_requests):
            if not isinstance(request, dict):
                continue
            request_path = f"$.exports.stub_requests[{request_index}]"
            request_id = request.get("id")
            if isinstance(request_id, str) and request_id:
                if request_id in request_ids:
                    errors.append(f"{request_path}.id: duplicate stub request {request_id!r}")
                request_ids.add(request_id)
            wrapper = request.get("wrapper")
            wrapper_name = wrapper.get("link_name") if isinstance(wrapper, dict) else None
            synthesis = request.get("synthesis")
            strategy = synthesis.get("strategy") if isinstance(synthesis, dict) else None
            if isinstance(wrapper_name, str) and wrapper_name:
                if wrapper_name in wrapper_names:
                    errors.append(
                        f"{request_path}.wrapper.link_name: duplicate wrapper {wrapper_name!r}"
                    )
                wrapper_names.add(wrapper_name)
                # forward_direct wrappers are new Consumer-defined surfaces.
                # translate_unwind wrappers are exported translator symbols.
                if wrapper_name in link_identities and strategy != "translate_unwind":
                    errors.append(
                        f"{request_path}.wrapper.link_name: {wrapper_name!r} collides with "
                        "an existing exported symbol"
                    )
            if "control_flow" in request:
                append_control_flow_errors(
                    errors, f"{request_path}.control_flow", request.get("control_flow")
                )
            if isinstance(synthesis, dict):
                if strategy == "forward_direct" or strategy == "reverse_override":
                    label = "forward_direct" if strategy == "forward_direct" else "reverse_override"
                    target_ref = synthesis.get("target") or request.get("target")
                    if not isinstance(target_ref, str) or not target_ref:
                        errors.append(
                            f"{request_path}.synthesis.target: {label} synthesis "
                            "requires a target symbol reference"
                        )
                    elif target_ref not in symbol_refs:
                        errors.append(
                            f"{request_path}.synthesis.target: unresolved {label} "
                            f"target {target_ref!r}"
                        )
                    elif wrapper_name and target_ref == wrapper_name:
                        errors.append(
                            f"{request_path}.synthesis.target: {label} wrapper "
                            "cannot forward to itself"
                        )
                elif strategy == "translate_unwind":
                    target_ref = synthesis.get("target") or request.get("target")
                    if not isinstance(target_ref, str) or not target_ref:
                        errors.append(
                            f"{request_path}.synthesis.target: translate_unwind synthesis "
                            "requires a target symbol reference"
                        )
                    elif target_ref not in symbol_refs:
                        errors.append(
                            f"{request_path}.synthesis.target: unresolved translate_unwind "
                            f"target {target_ref!r}"
                        )
                    if not isinstance(wrapper_name, str) or not wrapper_name:
                        errors.append(
                            f"{request_path}.wrapper.link_name: translate_unwind requires "
                            "an exported translator link_name"
                        )
                    elif wrapper_name not in symbol_refs:
                        errors.append(
                            f"{request_path}.wrapper.link_name: unresolved translator "
                            f"{wrapper_name!r}"
                        )
                    elif wrapper_name == target_ref:
                        errors.append(
                            f"{request_path}.wrapper.link_name: translator cannot be the "
                            "may_unwind target"
                        )
                elif strategy is not None:
                    errors.append(
                        f"{request_path}.synthesis.strategy: unsupported synthesis "
                        f"strategy {strategy!r}"
                    )
                else:
                    errors.append(
                        f"{request_path}.synthesis.strategy: required for in-Consumer synthesis"
                    )
            request_operations = request.get("operations")
            operation_iterable = list(request_operations) if isinstance(request_operations, list) else []
            if isinstance(request.get("operation"), dict):
                operation_iterable.append(request["operation"])
            for operation in operation_iterable:
                for ref in operation_symbol_refs(operation):
                    operation_refs.append((f"{request_path}.operation", ref))

    for path, ref in operation_refs:
        candidates = symbols_by_ref.get(ref, [])
        if not candidates:
            errors.append(f"{path}.symbol: unresolved symbol reference {ref!r}")
            continue
        first_contract = symbol_abi_contract(candidates[0][1], alias_targets)
        if any(
            symbol_abi_contract(symbol, alias_targets) != first_contract
            for _, symbol in candidates[1:]
        ):
            errors.append(f"{path}.symbol: conflicting symbol contracts for reference {ref!r}")

    for path, operation in operation_contracts:
        if (
            operation.get("availability") == "required"
            and not operation_has_implementation(operation)
        ):
            errors.append(
                f"{path}: availability 'required' needs a symbol, dispatch table/slot, or intrinsic"
            )
        table_ref = operation.get("dispatch_table")
        if not isinstance(table_ref, str) or not table_ref:
            continue
        resolved = dispatch_by_id.get(table_ref)
        if resolved is None:
            errors.append(f"{path}.dispatch_table: unresolved dispatch table {table_ref!r}")
            continue
        _, table = resolved
        slot = operation.get("slot")
        if not isinstance(slot, int) or isinstance(slot, bool) or slot < 0:
            continue
        matching_entries = [
            entry
            for entry in table.get("entries", [])
            if isinstance(entry, dict) and entry.get("index") == slot
        ]
        if len(matching_entries) != 1:
            errors.append(
                f"{path}.slot: dispatch table {table_ref!r} has "
                f"{len(matching_entries)} entries for slot {slot}"
            )
            continue
        entry = matching_entries[0]
        entry_symbol = entry.get("link_name") or entry.get("mangled")
        if not isinstance(entry_symbol, str) or entry_symbol not in symbol_refs:
            errors.append(
                f"{path}.slot: dispatch entry {table_ref!r}[{slot}] has no resolvable ABI symbol"
            )

    append_control_flow_errors(errors, "$.control_flow", document.get("control_flow"))
    for symbol_index, symbol in enumerate(symbols):
        if isinstance(symbol, dict) and "control_flow" in symbol:
            append_control_flow_errors(
                errors,
                f"$.exports.symbols[{symbol_index}].control_flow",
                symbol.get("control_flow"),
            )

    if strict and consumer_modes and "stub" not in consumer_modes:
        for object_path, value in iter_objects(document):
            if value.get("requires_stub") is True:
                errors.append(
                    f"{json_path(object_path)}.requires_stub: document profile does not declare "
                    "the stub consumer mode"
                )

    return errors


def validate_file(path: Path, schema: dict[str, Any], strict: bool) -> list[str]:
    document = load_json(path)
    errors = schema_errors(schema, document)
    if isinstance(document, dict):
        errors.extend(semantic_errors(document, strict))
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("documents", nargs="+", type=Path, help=".dci or .abi.json document")
    parser.add_argument("--schema", type=Path, default=DEFAULT_SCHEMA,
                        help="JSON schema to validate against (default: bundled dci-1.0 schema)")
    parser.add_argument(
        "--strict",
        action="store_true",
        help="require the portable profile/target/control-flow and explicit ABI lowering fields",
    )
    args = parser.parse_args(argv)

    try:
        schema = load_json(args.schema)
        failed = False
        for document in args.documents:
            errors = validate_file(document, schema, args.strict)
            if errors:
                failed = True
                print(f"DCI validation failed: {document}", file=sys.stderr)
                for error in errors:
                    print(f"  {error}", file=sys.stderr)
            else:
                print(f"DCI validation OK: {document}")
        return 1 if failed else 0
    except DciValidationError as exc:
        print(f"DCI validation error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
