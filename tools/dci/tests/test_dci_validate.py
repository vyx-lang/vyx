import copy
import importlib.util
import sys
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).parents[1] / "dci_validate.py"
SPEC = importlib.util.spec_from_file_location("dci_validate", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
validator = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = validator
SPEC.loader.exec_module(validator)

SCHEMA = validator.load_json(validator.DEFAULT_SCHEMA)


def portable_document(consumer_modes=None):
    modes = consumer_modes if consumer_modes is not None else ["direct"]
    return {
        "dci": "1.0",
        "kind": "abi",
        "profile": {
            "id": "test.native",
            "version": "1.0",
            "level": "L3",
            "consumer_modes": modes,
        },
        "source": {"language": "fixture"},
        "target": {
            "triple": "x86_64-pc-windows-msvc",
            "architecture": "x86_64",
            "pointer_width": 64,
            "endianness": "little",
            "abi": "fixture-native",
        },
        "control_flow": {
            "default_boundary": "no_unwind",
            "propagation": {"mode": "forbidden"},
        },
        "exports": {
            "aliases": [],
            "layouts": [],
            "symbols": [],
            "calling_conventions": [],
            "dispatch_tables": [],
            "runtime_type_operations": [],
        },
    }


def direct_table_adjustment():
    return {
        "kind": "table",
        "table_pointer_offset": 8,
        "table_entry_offset": 12,
        "table_entry_size": 4,
        "table_entry_signed": True,
        "displacement_base_offset": 8,
        "result": "object_plus_base_plus_displacement",
        "null_preserving": True,
        "fallback_complete_object_offset": 40,
    }


def add_base(document, adjustment):
    document["exports"]["layouts"] = [
        {
            "type_name": "fixture.Derived",
            "size": 64,
            "alignment": 8,
            "fields": [],
            "bases": [
                {
                    "type_name": "fixture.Base",
                    "is_virtual": True,
                    "adjustment": adjustment,
                }
            ],
        }
    ]


def validation_errors(document, strict=True):
    return validator.schema_errors(SCHEMA, document) + validator.semantic_errors(
        document, strict
    )


class DciValidatorTests(unittest.TestCase):
    def test_bitfield_contract_is_explicit_and_operations_are_complete(self):
        document = portable_document()
        bitfield = {
            "storage_offset": 0,
            "storage_size": 4,
            "bit_offset": 3,
            "bit_width": 5,
            "signed": False,
            "bit_order": "lsb0",
            "read": "direct",
            "write": "direct",
        }
        document["exports"]["layouts"] = [
            {
                "type_name": "fixture.Flags",
                "size": 4,
                "alignment": 4,
                "fields": [
                    {
                        "name": "value",
                        "offset": 0,
                        "type": {
                            "name": "u32",
                            "kind": "primitive",
                            "nullable": False,
                            "reference": "value",
                        },
                        "bitfield": bitfield,
                    }
                ],
                "bases": [],
            }
        ]

        self.assertEqual(validation_errors(document), [])
        required = SCHEMA["$defs"]["bitfield"]["required"]
        for field in ("signed", "bit_order", "read", "write"):
            with self.subTest(field=field):
                self.assertIn(field, required)
                incomplete = copy.deepcopy(document)
                del incomplete["exports"]["layouts"][0]["fields"][0]["bitfield"][field]
                errors = validation_errors(incomplete)
                self.assertTrue(any("is a required property" in error for error in errors), errors)

        target_order = copy.deepcopy(document)
        target_order["exports"]["layouts"][0]["fields"][0]["bitfield"][
            "bit_order"
        ] = "target"
        errors = validation_errors(target_order, strict=True)
        self.assertTrue(any("has no normalized Profile interpretation" in error for error in errors), errors)

        operation_read = copy.deepcopy(document)
        operation_bitfield = operation_read["exports"]["layouts"][0]["fields"][0][
            "bitfield"
        ]
        operation_bitfield["read"] = "operation"
        errors = validation_errors(operation_read)
        self.assertTrue(any("read_operation" in error for error in errors), errors)

        operation_bitfield["read_operation"] = {"availability": "required"}
        errors = validation_errors(operation_read)
        self.assertTrue(any("operation mode requires" in error for error in errors), errors)

        operation_bitfield["read_operation"] = {"intrinsic": "fixture.bitfield.read"}
        self.assertEqual(validation_errors(operation_read), [])

    def test_stable_value_layout_requires_trivial_or_lifecycle_evidence(self):
        document = portable_document()
        document["exports"]["layouts"] = [
            {
                "type_name": "fixture.Value",
                "size": 8,
                "alignment": 8,
                "representation": "stable",
                "fields": [],
            }
        ]
        errors = validation_errors(document)
        self.assertTrue(any("must declare lifecycle or explicit trivial" in error for error in errors), errors)

        document["exports"]["layouts"][0].update(
            {"is_pod": True, "is_trivially_destructible": True}
        )
        self.assertEqual(validation_errors(document), [])

    def test_abi_parameter_lowering_covers_every_source_parameter(self):
        document = portable_document()
        primitive = {
            "name": "i32",
            "kind": "primitive",
            "nullable": False,
            "reference": "value",
        }
        document["exports"]["symbols"] = [
            {
                "name": "fixture_mixed_params",
                "link_name": "fixture_mixed_params",
                "kind": "function",
                "params": [
                    {"name": "first", "type": primitive, "ownership": "copy"},
                    {"name": "second", "type": primitive, "ownership": "copy"},
                    {"name": "third", "type": primitive, "ownership": "copy"},
                ],
                "return": None,
                "abi": {
                    "calling_convention": "system",
                    "parameters": [
                        {"index": 2, "passing": "direct"},
                        {"index": 0, "passing": "direct"},
                        {"index": 1, "passing": "direct"},
                    ],
                    "return": {"passing": "ignore"},
                },
            }
        ]

        self.assertEqual(validation_errors(document, strict=True), [])
        self.assertEqual(validation_errors(document, strict=False), [])

        positional = copy.deepcopy(document)
        for lowering in positional["exports"]["symbols"][0]["abi"]["parameters"]:
            del lowering["index"]
        self.assertEqual(validation_errors(positional, strict=True), [])
        self.assertEqual(validation_errors(positional, strict=False), [])

        missing = copy.deepcopy(document)
        del missing["exports"]["symbols"][0]["abi"]["parameters"][1]
        for strict in (True, False):
            with self.subTest(strict=strict):
                errors = validation_errors(missing, strict=strict)
                self.assertTrue(
                    any(
                        "abi.parameters: missing lowering for source parameter indices [0]"
                        in error
                        for error in errors
                    ),
                    errors,
                )

    def test_conflicting_duplicate_binding_identity_is_rejected(self):
        document = portable_document()
        primitive = {
            "name": "i32",
            "kind": "primitive",
            "nullable": False,
            "reference": "value",
        }
        symbol = {
            "name": "fixture_value",
            "link_name": "fixture_value",
            "kind": "function",
            "params": [{"name": "value", "type": primitive, "ownership": "copy"}],
            "return": {"type": primitive, "ownership": "copy"},
            "abi": {
                "calling_convention": "system",
                "parameters": [{"index": 0, "passing": "direct"}],
                "return": {"passing": "direct"},
            },
        }
        document["exports"]["symbols"] = [symbol, copy.deepcopy(symbol)]
        self.assertEqual(validation_errors(document), [])

        document["exports"]["symbols"][1]["abi"]["parameters"][0][
            "passing"
        ] = "indirect"
        errors = validation_errors(document)
        self.assertTrue(any("conflicting ABI contract" in error for error in errors), errors)

    def test_duplicate_link_identities_require_equivalent_interop_contracts(self):
        document = portable_document()
        primitive = {
            "name": "i32",
            "kind": "primitive",
            "nullable": False,
            "reference": "value",
        }
        symbol = {
            "name": "fixture_value",
            "link_name": "fixture_shared_link",
            "kind": "function",
            "params": [{"name": "value", "type": primitive, "ownership": "copy"}],
            "return": {"type": primitive, "ownership": "copy"},
            "abi": {
                "calling_convention": "system",
                "parameters": [{"index": 0, "passing": "direct"}],
                "return": {"passing": "direct"},
            },
            "control_flow": {
                "default_boundary": "no_unwind",
                "propagation": {"mode": "forbidden"},
            },
        }
        alias = copy.deepcopy(symbol)
        alias["name"] = "fixture_value_alias"
        document["exports"]["symbols"] = [symbol, alias]
        self.assertEqual(validation_errors(document), [])

        mutations = {
            "ABI": lambda candidate: candidate["abi"].update(
                {"calling_convention": "fixture_alternate"}
            ),
            "parameter ownership": lambda candidate: candidate["params"][0].update(
                {"ownership": "move"}
            ),
            "return ownership": lambda candidate: candidate["return"].update(
                {"ownership": "owned"}
            ),
            "control flow": lambda candidate: candidate.update(
                {
                    "control_flow": {
                        "default_boundary": "abort",
                        "propagation": {"mode": "abort"},
                    }
                }
            ),
        }
        for contract_part, mutate in mutations.items():
            with self.subTest(contract_part=contract_part):
                conflicting = copy.deepcopy(document)
                mutate(conflicting["exports"]["symbols"][1])
                errors = validation_errors(conflicting)
                self.assertTrue(
                    any(
                        "conflicting ABI, ownership, or control-flow contract for link identity"
                        in error
                        for error in errors
                    ),
                    errors,
                )

    def test_legacy_mangled_identity_is_checked_globally(self):
        document = portable_document()
        primitive = {
            "name": "i32",
            "kind": "primitive",
            "nullable": False,
            "reference": "value",
        }
        symbol = {
            "name": "fixture_first",
            "mangled": "fixture_shared_legacy_link",
            "kind": "function",
            "params": [{"name": "value", "type": primitive, "ownership": "copy"}],
            "return": {"type": primitive, "ownership": "copy"},
            "abi": {
                "calling_convention": "system",
                "parameters": [{"index": 0, "passing": "direct"}],
                "return": {"passing": "direct"},
            },
        }
        alias = copy.deepcopy(symbol)
        alias["name"] = "fixture_second"
        document["exports"]["symbols"] = [symbol, alias]
        self.assertEqual(validation_errors(document, strict=False), [])

        alias["return"]["ownership"] = "owned"
        errors = validation_errors(document, strict=False)
        self.assertTrue(
            any(
                ".mangled: conflicting ABI, ownership, or control-flow contract "
                "for link identity 'fixture_shared_legacy_link'" in error
                for error in errors
            ),
            errors,
        )

    def test_link_identity_contract_resolves_declared_type_aliases(self):
        document = portable_document()
        document["exports"]["aliases"] = [
            {"name": "api::Value", "target": "impl::Value"},
            {"name": "api::ValueAlias", "target": "impl::Value"},
        ]
        symbol = {
            "name": "api::Value::read",
            "owner": "api::Value",
            "member_name": "read",
            "link_name": "fixture_alias_read",
            "kind": "method",
            "params": [],
            "return": None,
            "abi": {
                "calling_convention": "system",
                "receiver": {
                    "passing": "direct",
                    "type": {
                        "name": "api::Value",
                        "kind": "class",
                        "reference": "pointer",
                        "cpp_type": "api::Value *",
                    },
                    "ownership": "borrow",
                },
                "parameters": [],
                "return": {"passing": "direct"},
            },
        }
        alias = copy.deepcopy(symbol)
        alias["name"] = "api::ValueAlias::read"
        alias["owner"] = "api::ValueAlias"
        alias["abi"]["receiver"]["type"]["name"] = "api::ValueAlias"
        alias["abi"]["receiver"]["type"]["cpp_type"] = "api::ValueAlias *"
        document["exports"]["symbols"] = [symbol, alias]
        self.assertEqual(validation_errors(document), [])

        document["exports"]["aliases"][1]["target"] = "impl::OtherValue"
        errors = validation_errors(document)
        self.assertTrue(
            any(
                "conflicting ABI, ownership, or control-flow contract for link identity"
                in error
                for error in errors
            ),
            errors,
        )

    def test_schema_exposes_complete_table_adjustment_contract(self):
        properties = SCHEMA["$defs"]["baseAdjustment"]["properties"]
        expected = {
            "table_pointer_offset",
            "table_entry_offset",
            "table_entry_size",
            "table_entry_signed",
            "displacement_base_offset",
            "result",
            "null_preserving",
            "fallback_complete_object_offset",
        }
        self.assertTrue(expected.issubset(properties))
        self.assertIn("table_offset", properties)

    def test_virtual_base_requires_explicit_adjustment(self):
        missing = portable_document()
        missing["exports"]["layouts"] = [
            {
                "type_name": "fixture.Derived",
                "size": 64,
                "alignment": 8,
                "fields": [],
                "bases": [
                    {
                        "type_name": "fixture.Base",
                        "is_virtual": True,
                    }
                ],
            }
        ]
        errors = validation_errors(missing)
        self.assertTrue(any("adjustment" in error and "required" in error for error in errors), errors)

        constant = portable_document()
        add_base(constant, {"kind": "constant", "offset": 40})
        errors = validation_errors(constant)
        self.assertTrue(any("virtual base cannot use a constant adjustment" in error for error in errors), errors)

        incomplete_constant = portable_document()
        add_base(incomplete_constant, {"kind": "constant"})
        errors = validation_errors(incomplete_constant)
        self.assertTrue(any("'offset' is a required property" in error for error in errors), errors)

        incomplete_operation = portable_document(["direct", "stub"])
        add_base(
            incomplete_operation,
            {"kind": "operation", "operation": {"availability": "required"}},
        )
        errors = validation_errors(incomplete_operation)
        self.assertTrue(any("operation adjustment requires" in error for error in errors), errors)

        unresolved_operation = portable_document(["direct", "stub"])
        add_base(
            unresolved_operation,
            {"kind": "operation", "operation": {"symbol": "fixture_missing_adjust"}},
        )
        errors = validation_errors(unresolved_operation)
        self.assertTrue(any("unresolved symbol reference" in error for error in errors), errors)

        intrinsic_operation = portable_document(["direct", "stub"])
        add_base(
            intrinsic_operation,
            {
                "kind": "operation",
                "operation": {
                    "intrinsic": "fixture.virtual_base.adjust",
                    "requires_stub": True,
                },
            },
        )
        self.assertEqual(validation_errors(intrinsic_operation), [])

    def test_complete_table_adjustment_is_directly_executable(self):
        document = portable_document()
        adjustment = direct_table_adjustment()
        add_base(document, adjustment)

        self.assertTrue(validator.table_adjustment_is_directly_executable(adjustment))
        self.assertEqual(validation_errors(document), [])

    def test_fallback_only_table_fact_requires_stub_but_is_valid(self):
        adjustment = {
            "kind": "table",
            "fallback_complete_object_offset": 40,
            "null_preserving": True,
        }
        document = portable_document(["direct", "stub"])
        add_base(document, adjustment)

        self.assertFalse(validator.table_adjustment_is_directly_executable(adjustment))
        self.assertEqual(validation_errors(document), [])

        document["profile"]["consumer_modes"] = ["direct"]
        errors = validation_errors(document)
        self.assertTrue(any("descriptive only" in error for error in errors), errors)

    def test_partial_table_contract_is_not_hidden_by_fallback(self):
        document = portable_document(["stub"])
        adjustment = {
            "kind": "table",
            "table_pointer_offset": 8,
            "fallback_complete_object_offset": 40,
        }
        add_base(document, adjustment)

        errors = validation_errors(document)
        self.assertTrue(any("not directly executable" in error for error in errors), errors)

    def test_required_runtime_operation_needs_real_implementation(self):
        document = portable_document()
        runtime_operation = {
            "type_name": "fixture.Base",
            "capabilities": ["upcast"],
            "operations": {"upcast": {"availability": "required"}},
        }
        document["exports"]["runtime_type_operations"] = [runtime_operation]

        errors = validation_errors(document)
        self.assertTrue(any("needs a symbol" in error for error in errors), errors)

        runtime_operation["operations"]["upcast"]["intrinsic"] = "dci.layout.upcast"
        self.assertEqual(validation_errors(document), [])

        runtime_operation["operations"]["upcast"] = {
            "availability": "required",
            "dispatch_table": "fixture.Base.dispatch",
            "slot": 2,
        }
        errors = validation_errors(document)
        self.assertTrue(any("unresolved dispatch table" in error for error in errors), errors)

        document["exports"]["dispatch_tables"] = [
            {
                "id": "fixture.Base.dispatch",
                "class_name": "fixture.Base",
                "entries": [
                    {
                        "index": 2,
                        "offset": 16,
                        "kind": "function",
                        "link_name": "fixture_dispatch_cast",
                    }
                ],
            }
        ]
        errors = validation_errors(document)
        self.assertTrue(any("no resolvable ABI symbol" in error for error in errors), errors)

        pointer_type = {
            "name": "fixture.Base",
            "kind": "class",
            "nullable": True,
            "reference": "pointer",
        }
        document["exports"]["symbols"] = [
            {
                "name": "fixture_dispatch_cast",
                "link_name": "fixture_dispatch_cast",
                "kind": "function",
                "params": [
                    {"name": "value", "type": pointer_type, "ownership": "borrow"}
                ],
                "return": {"type": pointer_type, "ownership": "borrow"},
                "abi": {
                    "calling_convention": "system",
                    "parameters": [{"index": 0, "passing": "direct"}],
                    "return": {"passing": "direct"},
                },
            }
        ]
        self.assertEqual(validation_errors(document), [])

        document["exports"]["dispatch_tables"][0]["entries"][0]["index"] = 3
        errors = validation_errors(document)
        self.assertTrue(any("entries for slot 2" in error for error in errors), errors)

    def test_lifecycle_required_operations_need_implementations(self):
        document = portable_document()
        document["exports"]["layouts"] = [
            {
                "type_name": "fixture.Owned",
                "size": 8,
                "alignment": 8,
                "fields": [],
                "lifecycle": {
                    "ownership_model": "unique",
                    "copy_semantics": "operation",
                    "move_semantics": "forbidden",
                    "destruction": "operation",
                    "moved_from_state": "valid",
                    "operations": {
                        "copy": {"availability": "required"},
                        "destroy": {"availability": "required"},
                    },
                },
            }
        ]

        errors = validation_errors(document)
        self.assertTrue(any("operations.copy" in error and "needs a" in error for error in errors), errors)
        self.assertTrue(any("operations.destroy" in error and "needs a" in error for error in errors), errors)

        document["exports"]["layouts"][0]["lifecycle"]["operations"] = {
            "copy": {
                "availability": "required",
                "intrinsic": "fixture.lifecycle.copy",
            },
            "destroy": {
                "availability": "required",
                "intrinsic": "fixture.lifecycle.destroy",
            },
        }
        self.assertEqual(validation_errors(document), [])

        document["exports"]["layouts"][0]["lifecycle"]["operations"]["copy"] = {
            "symbol": "fixture_missing_copy"
        }
        errors = validation_errors(document)
        self.assertTrue(any("unresolved symbol reference" in error for error in errors), errors)

    def test_strict_symbols_require_ownership_contracts(self):
        document = portable_document()
        primitive = {
            "name": "i32",
            "kind": "primitive",
            "nullable": False,
            "reference": "value",
        }
        symbol = {
            "name": "fixture_owned_value",
            "link_name": "fixture_owned_value",
            "kind": "function",
            "params": [{"name": "value", "type": primitive, "ownership": "copy"}],
            "return": {"type": primitive, "ownership": "copy"},
            "abi": {
                "calling_convention": "system",
                "parameters": [{"index": 0, "passing": "direct"}],
                "return": {"passing": "direct"},
            },
        }
        document["exports"]["symbols"] = [symbol]
        self.assertEqual(validation_errors(document, strict=True), [])

        missing_parameter = copy.deepcopy(document)
        del missing_parameter["exports"]["symbols"][0]["params"][0]["ownership"]
        errors = validation_errors(missing_parameter, strict=True)
        self.assertTrue(any("params[0].ownership" in error for error in errors), errors)
        self.assertEqual(validation_errors(missing_parameter, strict=False), [])

        missing_return = copy.deepcopy(document)
        del missing_return["exports"]["symbols"][0]["return"]["ownership"]
        errors = validation_errors(missing_return, strict=True)
        self.assertTrue(any("return.ownership" in error for error in errors), errors)
        self.assertEqual(validation_errors(missing_return, strict=False), [])

    def test_abi_lowering_requires_shape_facts_and_power_of_two_alignment(self):
        document = portable_document()
        value_type = {
            "name": "fixture.Value",
            "kind": "record",
            "nullable": False,
            "reference": "value",
        }
        document["exports"]["symbols"] = [
            {
                "name": "fixture_take_value",
                "link_name": "fixture_take_value",
                "kind": "function",
                "params": [
                    {"name": "value", "type": value_type, "ownership": "copy"}
                ],
                "return": None,
                "abi": {
                    "calling_convention": "system",
                    "parameters": [
                        {"index": 0, "passing": "byval", "size": 16, "alignment": 16}
                    ],
                    "return": {"passing": "ignore"},
                },
            }
        ]
        self.assertEqual(validation_errors(document), [])

        for field in ("size", "alignment"):
            with self.subTest(passing="byval", field=field):
                incomplete = copy.deepcopy(document)
                del incomplete["exports"]["symbols"][0]["abi"]["parameters"][0][field]
                errors = validation_errors(incomplete)
                self.assertTrue(any(f"'{field}' is a required property" in error for error in errors), errors)

    def test_wide_pointer_split_uses_measured_view_size(self):
        document = portable_document()
        slice_type = {
            "name": "[u8]",
            "kind": "opaque",
            "nullable": False,
            "reference": "pointer",
            "wide": True,
            "metadata_kind": "len",
            "size": 16,
            "alignment": 8,
        }
        document["exports"]["symbols"] = [{
            "name": "slice_len",
            "link_name": "slice_len",
            "kind": "function",
            "params": [{"name": "slice", "type": slice_type, "ownership": "borrow"}],
            "return": None,
            "abi": {
                "calling_convention": "system",
                "parameters": [{
                    "index": 0,
                    "passing": "split",
                    "size": 16,
                    "registers": ["ptr", "i64"],
                    "register_offsets": [0, 8],
                }],
                "return": {"passing": "ignore"},
            },
        }]
        self.assertEqual(validation_errors(document, strict=True), [])

    def test_indirect_storage_size_matches_resolved_source_layout(self):
        document = portable_document()
        value_type = {
            "name": "fixture.Value",
            "kind": "record",
            "nullable": False,
            "reference": "value",
        }
        document["exports"]["layouts"] = [
            {
                "type_name": "fixture.Value",
                "size": 16,
                "alignment": 8,
                "fields": [],
            }
        ]
        document["exports"]["symbols"] = [
            {
                "name": "fixture_roundtrip",
                "link_name": "fixture_roundtrip",
                "kind": "function",
                "params": [
                    {"name": "value", "type": value_type, "ownership": "copy"}
                ],
                "return": {"type": value_type, "ownership": "owned"},
                "abi": {
                    "calling_convention": "system",
                    "parameters": [
                        {"index": 0, "passing": "byval", "size": 16, "alignment": 8}
                    ],
                    "return": {"passing": "sret", "size": 16, "alignment": 8},
                    "hidden_parameters": [
                        {"passing": "sret", "size": 16, "alignment": 8}
                    ],
                },
            }
        ]
        self.assertEqual(validation_errors(document), [])

        mutations = {
            "parameter": lambda candidate: candidate["exports"]["symbols"][0]["abi"][
                "parameters"
            ][0].update({"size": 8}),
            "return": lambda candidate: candidate["exports"]["symbols"][0]["abi"][
                "return"
            ].update({"size": 8}),
            "hidden sret": lambda candidate: candidate["exports"]["symbols"][0]["abi"][
                "hidden_parameters"
            ][0].update({"size": 8}),
        }
        for position, mutate in mutations.items():
            with self.subTest(position=position):
                mismatched = copy.deepcopy(document)
                mutate(mismatched)
                errors = validation_errors(mismatched)
                self.assertTrue(
                    any("does not match the 16-byte source type" in error for error in errors),
                    errors,
                )

        unresolved = copy.deepcopy(document)
        unresolved["exports"]["layouts"] = []
        unknown_type = {
            "name": "fixture.OpaqueValue",
            "kind": "opaque",
            "nullable": False,
            "reference": "value",
        }
        symbol = unresolved["exports"]["symbols"][0]
        symbol["params"][0]["type"] = unknown_type
        symbol["return"]["type"] = unknown_type
        symbol["abi"]["parameters"][0].update({"size": 7, "alignment": 1})
        symbol["abi"]["return"].update({"size": 9, "alignment": 1})
        symbol["abi"]["hidden_parameters"][0].update({"size": 9, "alignment": 1})
        self.assertEqual(validation_errors(unresolved), [])

    def test_coerce_width_matches_lowering_and_resolved_source_layout(self):
        document = portable_document()
        value_type = {
            "name": "fixture.Word",
            "kind": "record",
            "nullable": False,
            "reference": "value",
        }
        document["exports"]["layouts"] = [
            {
                "type_name": "fixture.Word",
                "size": 4,
                "alignment": 4,
                "fields": [],
            }
        ]
        document["exports"]["symbols"] = [
            {
                "name": "fixture_take_word",
                "link_name": "fixture_take_word",
                "kind": "function",
                "params": [
                    {"name": "value", "type": value_type, "ownership": "copy"}
                ],
                "return": None,
                "abi": {
                    "calling_convention": "system",
                    "parameters": [
                        {
                            "index": 0,
                            "passing": "coerce",
                            "size": 4,
                            "coerce_to": {
                                "name": "i32",
                                "kind": "primitive",
                                "reference": "value",
                            },
                        }
                    ],
                    "return": {"passing": "ignore"},
                },
            }
        ]
        self.assertEqual(validation_errors(document), [])

        wrong_target = copy.deepcopy(document)
        lowering = wrong_target["exports"]["symbols"][0]["abi"]["parameters"][0]
        lowering["coerce_to"]["name"] = "i64"
        errors = validation_errors(wrong_target)
        self.assertTrue(any("target width is 8 bytes" in error for error in errors), errors)

        wrong_source = copy.deepcopy(document)
        lowering = wrong_source["exports"]["symbols"][0]["abi"]["parameters"][0]
        lowering["size"] = 8
        lowering["coerce_to"]["name"] = "i64"
        errors = validation_errors(wrong_source)
        self.assertTrue(
            any("does not match the 4-byte source type" in error for error in errors),
            errors,
        )

        bad_alignment = copy.deepcopy(document)
        bad_alignment["exports"]["symbols"][0]["abi"]["parameters"][0]["alignment"] = 3
        errors = validation_errors(bad_alignment)
        self.assertTrue(any("positive power of two" in error for error in errors), errors)

        coerced = copy.deepcopy(document)
        coerced_lowering = coerced["exports"]["symbols"][0]["abi"]["parameters"][0]
        coerced_lowering.clear()
        coerced_lowering.update(
            {
                "index": 0,
                "passing": "coerce",
                "size": 4,
                "coerce_to": {
                    "name": "i32",
                    "kind": "primitive",
                    "nullable": False,
                    "reference": "value",
                },
            }
        )
        self.assertEqual(validation_errors(coerced), [])
        for field in ("size", "coerce_to"):
            with self.subTest(passing="coerce", field=field):
                incomplete = copy.deepcopy(coerced)
                del incomplete["exports"]["symbols"][0]["abi"]["parameters"][0][field]
                errors = validation_errors(incomplete)
                if field == "size":
                    self.assertTrue(
                        any(
                            "'size' is a required property" in error
                            or "not valid under any of the given schemas" in error
                            or "requires a positive byte size" in error
                            for error in errors
                        ),
                        errors,
                    )
                else:
                    self.assertTrue(
                        any(
                            "'coerce_to' is a required property" in error
                            or "not valid under any of the given schemas" in error
                            or "required for coerce lowering" in error
                            for error in errors
                        ),
                        errors,
                    )

    def test_direct_runtime_cast_requires_normalized_boundary_contract(self):
        document = portable_document()
        base_pointer = {
            "name": "fixture.Base",
            "kind": "class",
            "nullable": True,
            "reference": "pointer",
        }
        derived_pointer = {
            "name": "fixture.Derived",
            "kind": "class",
            "nullable": True,
            "reference": "pointer",
        }
        document["exports"]["symbols"] = [
            {
                "name": "fixture_checked_downcast",
                "link_name": "fixture_checked_downcast",
                "kind": "function",
                "calling_convention": "system",
                "params": [
                    {"name": "value", "type": base_pointer, "ownership": "borrow"}
                ],
                "return": {"type": derived_pointer, "ownership": "borrow"},
                "abi": {
                    "calling_convention": "system",
                    "parameters": [{"index": 0, "passing": "direct"}],
                    "return": {"passing": "direct"},
                },
            }
        ]
        operation = {
            "symbol": "fixture_checked_downcast",
            "availability": "required",
        }
        document["exports"]["runtime_type_operations"] = [
            {
                "type_name": "fixture.Derived",
                "capabilities": ["downcast"],
                "operations": {"downcast": operation},
            }
        ]

        errors = validation_errors(document)
        for field in (*validator.DIRECT_RUNTIME_CAST_FIELDS, "no_unwind"):
            self.assertTrue(any(f".{field}:" in error for error in errors), errors)

        operation.update(
            {
                "source_type": "fixture.Base",
                "target_type": "fixture.Derived",
                "failure": "null",
                "null_behavior": "return_null",
                "no_unwind": True,
            }
        )
        self.assertEqual(validation_errors(document), [])

        consuming = copy.deepcopy(document)
        consuming["exports"]["symbols"][0]["params"][0]["ownership"] = "move"
        consuming["exports"]["symbols"][0]["return"]["ownership"] = "owned"
        errors = validation_errors(consuming)
        self.assertTrue(any("nullable borrowed pointer" in error for error in errors), errors)

        conflicting_reference = copy.deepcopy(document)
        original_symbol = conflicting_reference["exports"]["symbols"][0]
        original_symbol["semantic_id"] = "fixture.runtime.cast"
        conflicting_reference["exports"]["runtime_type_operations"][0]["operations"][
            "downcast"
        ]["symbol"] = "fixture.runtime.cast"
        alias_symbol = copy.deepcopy(original_symbol)
        alias_symbol["name"] = "fixture_checked_downcast_alias"
        alias_symbol["abi"]["calling_convention"] = "vectorcall"
        conflicting_reference["exports"]["symbols"].append(alias_symbol)
        errors = validation_errors(conflicting_reference)
        self.assertTrue(any("conflicting symbol contracts for reference" in error for error in errors), errors)

        document["exports"]["runtime_type_operations"].append(
            copy.deepcopy(document["exports"]["runtime_type_operations"][0])
        )
        errors = validation_errors(document)
        self.assertTrue(any("type_name: duplicate 'fixture.Derived'" in error for error in errors), errors)

    def test_dispatch_tables_and_vtables_alias_must_agree(self):
        document = portable_document()
        table = {
            "id": "fixture.Base.dispatch",
            "class_name": "fixture.Base",
            "entries": [],
        }
        document["exports"]["dispatch_tables"] = [copy.deepcopy(table)]
        document["exports"]["vtables"] = [copy.deepcopy(table)]
        self.assertEqual(validation_errors(document), [])

        document["exports"]["vtables"][0]["class_name"] = "fixture.Other"
        errors = validation_errors(document)
        self.assertTrue(any("differs from" in error for error in errors), errors)

    def test_translated_and_shared_control_flow_need_boundary_data(self):
        translated = portable_document()
        translated["control_flow"] = {
            "default_boundary": "translated",
            "propagation": {"mode": "translated"},
        }
        errors = validation_errors(translated)
        self.assertTrue(any("translator_symbol" in error for error in errors), errors)

        translated["control_flow"]["propagation"]["translator_symbol"] = (
            "fixture_translate_failure"
        )
        self.assertEqual(validation_errors(translated), [])

        shared = portable_document()
        shared["control_flow"] = {
            "default_boundary": "shared_abi",
            "propagation": {"mode": "shared_abi"},
        }
        errors = validation_errors(shared)
        self.assertTrue(any("propagation.abi" in error for error in errors), errors)

        shared["control_flow"]["propagation"]["abi"] = "fixture-unwind-v1"
        self.assertEqual(validation_errors(shared), [])

    def test_per_symbol_propagation_boundary_needs_shared_data(self):
        document = portable_document()

        def symbol_with(control_flow):
            return {
                "name": "throwing",
                "kind": "function",
                "link_name": "throwing",
                "calling_convention": "c",
                "params": [],
                "return": None,
                "abi": {
                    "calling_convention": "c",
                    "parameters": [],
                    "return": {"passing": "ignore"},
                },
                "control_flow": control_flow,
            }

        document["exports"]["symbols"] = [
            symbol_with({
                "default_boundary": "translated",
                "propagation": {"mode": "translated"},
            })
        ]
        errors = validation_errors(document)
        self.assertTrue(any("translator_symbol" in error for error in errors), errors)

        document["exports"]["symbols"] = [
            symbol_with({
                "default_boundary": "translated",
                "propagation": {
                    "mode": "translated",
                    "translator_symbol": "throwing_translate",
                },
            })
        ]
        self.assertEqual(validation_errors(document), [])

    def test_profile_consumer_modes_are_closed_and_match_stub_requirements(self):
        for modes in ([], ["direct", "direct"], ["dynamic"]):
            with self.subTest(modes=modes):
                document = portable_document(modes)
                self.assertNotEqual(validator.schema_errors(SCHEMA, document), [])

        document = portable_document(["direct"])
        document["exports"]["runtime_type_operations"] = [
            {
                "type_name": "fixture.Base",
                "capabilities": ["cast"],
                "operations": {
                    "cast": {"availability": "optional", "requires_stub": True}
                },
            }
        ]
        errors = validation_errors(document)
        self.assertTrue(any("stub consumer mode" in error for error in errors), errors)

    def test_normalized_stub_request_requires_stub_mode_and_is_consistent(self):
        # A well-formed stub request under a stub-capable profile validates.
        document = portable_document(["direct", "stub"])
        document["exports"]["stub_requests"] = [
            {
                "id": "fixture.Base.downcast",
                "kind": "cast_wrapper",
                "reason": "requires_stub",
                "target": "fixture.Base",
                "input": {"abi": "system"},
                "output": {"abi": "system"},
                "operation": {
                    "source_type": "fixture.Base",
                    "target_type": "fixture.Derived",
                    "requires_stub": True,
                },
                "capabilities": ["emit-source", "compile-object"],
                "control_flow": {
                    "default_boundary": "no_unwind",
                    "propagation": {"mode": "forbidden"},
                },
                "wrapper": {"link_name": "fixture_base_to_derived_stub"},
            }
        ]
        self.assertEqual(validation_errors(document), [])

        # The same request without the stub consumer mode is rejected.
        direct_only = copy.deepcopy(document)
        direct_only["profile"]["consumer_modes"] = ["direct"]
        errors = validation_errors(direct_only)
        self.assertTrue(
            any("does not declare" in error and "stub" in error for error in errors),
            errors,
        )

    def test_stub_request_wrapper_must_not_reuse_existing_symbol(self):
        document = portable_document(["direct", "stub"])
        document["exports"]["symbols"] = [
            {
                "name": "existing",
                "kind": "function",
                "link_name": "existing_symbol",
                "calling_convention": "c",
                "params": [],
                "return": None,
            }
        ]
        document["exports"]["stub_requests"] = [
            {
                "id": "fixture.wrap",
                "kind": "operation_wrapper",
                "wrapper": {"link_name": "existing_symbol"},
            }
        ]
        errors = validation_errors(document)
        self.assertTrue(any("collides with" in error for error in errors), errors)

    def test_duplicate_stub_request_ids_and_wrappers_are_rejected(self):
        document = portable_document(["direct", "stub"])
        document["exports"]["stub_requests"] = [
            {"id": "dup", "kind": "operation_wrapper", "wrapper": {"link_name": "w0"}},
            {"id": "dup", "kind": "operation_wrapper", "wrapper": {"link_name": "w0"}},
        ]
        errors = validation_errors(document)
        self.assertTrue(any("duplicate stub request" in error for error in errors), errors)
        self.assertTrue(any("duplicate wrapper" in error for error in errors), errors)

    def _forwardable_symbol(self):
        return {
            "name": "probe",
            "kind": "function",
            "link_name": "probe_symbol",
            "calling_convention": "c",
            "params": [
                {
                    "name": "value",
                    "type": {
                        "name": "i32",
                        "kind": "primitive",
                        "nullable": False,
                        "reference": "value",
                    },
                    "ownership": "copy",
                }
            ],
            "return": {
                "type": {
                    "name": "i32",
                    "kind": "primitive",
                    "nullable": False,
                    "reference": "value",
                },
                "ownership": "copy",
            },
            "abi": {
                "calling_convention": "c",
                "parameters": [{"index": 0, "passing": "direct", "size": 4}],
                "return": {"passing": "direct", "size": 4},
            },
        }

    def test_forward_direct_synthesis_accepts_resolvable_target(self):
        document = portable_document(["direct", "stub"])
        document["exports"]["symbols"] = [self._forwardable_symbol()]
        document["exports"]["stub_requests"] = [
            {
                "id": "probe.forward",
                "kind": "operation_wrapper",
                "target": "probe",
                "synthesis": {"strategy": "forward_direct"},
                "wrapper": {"link_name": "probe_forward_wrapper"},
            }
        ]
        self.assertEqual(validation_errors(document), [])

    def test_reverse_override_synthesis_accepts_resolvable_target(self):
        document = portable_document(["direct", "stub"])
        document["exports"]["symbols"] = [self._forwardable_symbol()]
        document["exports"]["stub_requests"] = [
            {
                "id": "probe.reverse",
                "kind": "reverse_override",
                "target": "probe",
                "synthesis": {"strategy": "reverse_override"},
                "wrapper": {"link_name": "probe_reverse_wrapper"},
            }
        ]
        self.assertEqual(validation_errors(document), [])

    def test_forward_direct_synthesis_rejects_unresolved_target(self):
        document = portable_document(["direct", "stub"])
        document["exports"]["symbols"] = [self._forwardable_symbol()]
        document["exports"]["stub_requests"] = [
            {
                "id": "probe.forward",
                "kind": "operation_wrapper",
                "synthesis": {"strategy": "forward_direct", "target": "missing"},
                "wrapper": {"link_name": "probe_forward_wrapper"},
            }
        ]
        errors = validation_errors(document)
        self.assertTrue(
            any("unresolved forward_direct target" in error for error in errors), errors
        )

    def test_forward_direct_synthesis_requires_a_target(self):
        document = portable_document(["direct", "stub"])
        document["exports"]["symbols"] = [self._forwardable_symbol()]
        document["exports"]["stub_requests"] = [
            {
                "id": "probe.forward",
                "kind": "operation_wrapper",
                "synthesis": {"strategy": "forward_direct"},
                "wrapper": {"link_name": "probe_forward_wrapper"},
            }
        ]
        errors = validation_errors(document)
        self.assertTrue(
            any("requires a target symbol reference" in error for error in errors), errors
        )

    def test_unsupported_synthesis_strategy_is_rejected(self):
        document = portable_document(["direct", "stub"])
        document["exports"]["symbols"] = [self._forwardable_symbol()]
        document["exports"]["stub_requests"] = [
            {
                "id": "probe.forward",
                "kind": "operation_wrapper",
                "target": "probe",
                "synthesis": {"strategy": "reverse_thunk"},
                "wrapper": {"link_name": "probe_forward_wrapper"},
            }
        ]
        errors = validation_errors(document)
        # The schema enum already rejects the unknown strategy; the semantic pass
        # additionally reports it when schema validation is bypassed.
        self.assertTrue(
            any("reverse_thunk" in error for error in errors), errors
        )

    def _translate_unwind_pair(self):
        orig = self._forwardable_symbol()
        orig["control_flow"] = {
            "default_boundary": "no_unwind",
            "propagation": {"mode": "forbidden"},
            "unwind": "may_unwind",
        }
        translator = self._forwardable_symbol()
        translator["name"] = "probe_translated"
        translator["link_name"] = "probe_translated"
        translator["control_flow"] = {
            "default_boundary": "translated",
            "propagation": {
                "mode": "translated",
                "translator_symbol": "probe_translated",
            },
            "unwind": "no_unwind",
        }
        return orig, translator

    def test_translate_unwind_is_accepted_when_wrapper_is_exported_translator(self):
        document = portable_document(["direct", "stub"])
        orig, translator = self._translate_unwind_pair()
        document["exports"]["symbols"] = [orig, translator]
        document["exports"]["stub_requests"] = [
            {
                "id": "probe.translate",
                "kind": "operation_wrapper",
                "target": "probe_symbol",
                "synthesis": {"strategy": "translate_unwind", "target": "probe_symbol"},
                "wrapper": {"link_name": "probe_translated"},
            }
        ]
        self.assertEqual(validation_errors(document), [])

    def test_translate_unwind_rejects_missing_translator_symbol(self):
        document = portable_document(["direct", "stub"])
        orig, _translator = self._translate_unwind_pair()
        document["exports"]["symbols"] = [orig]
        document["exports"]["stub_requests"] = [
            {
                "id": "probe.translate",
                "kind": "operation_wrapper",
                "target": "probe_symbol",
                "synthesis": {"strategy": "translate_unwind", "target": "probe_symbol"},
                "wrapper": {"link_name": "probe_translated"},
            }
        ]
        errors = validation_errors(document)
        self.assertTrue(
            any("unresolved translator" in error for error in errors), errors
        )

    def test_translate_unwind_rejects_wrapper_equal_to_target(self):
        document = portable_document(["direct", "stub"])
        orig, _translator = self._translate_unwind_pair()
        document["exports"]["symbols"] = [orig]
        document["exports"]["stub_requests"] = [
            {
                "id": "probe.translate",
                "kind": "operation_wrapper",
                "target": "probe_symbol",
                "synthesis": {"strategy": "translate_unwind", "target": "probe_symbol"},
                "wrapper": {"link_name": "probe_symbol"},
            }
        ]
        errors = validation_errors(document)
        self.assertTrue(
            any("cannot be the may_unwind target" in error for error in errors), errors
        )

    def _rc_operation_symbol(self, name, link, returns_value):
        symbol = {
            "name": name,
            "kind": "function",
            "link_name": link,
            "calling_convention": "c",
            "params": [
                {
                    "name": "handle",
                    "type": {
                        "name": "fixture.Handle",
                        "kind": "class",
                        "nullable": False,
                        "reference": "pointer",
                    },
                    "ownership": "borrow" if returns_value else "move",
                }
            ],
            "return": (
                {
                    "type": {
                        "name": "fixture.Handle",
                        "kind": "class",
                        "nullable": False,
                        "reference": "value",
                    },
                    "ownership": "owned",
                }
                if returns_value
                else None
            ),
        }
        return symbol

    def test_shared_retain_release_lifecycle_validates(self):
        document = portable_document()
        document["exports"]["symbols"] = [
            self._rc_operation_symbol("handle_retain", "handle_retain", True),
            self._rc_operation_symbol("handle_release", "handle_release", False),
        ]
        document["exports"]["layouts"] = [
            {
                "type_name": "fixture.Handle",
                "size": 8,
                "alignment": 8,
                "fields": [],
                "lifecycle": {
                    "ownership_model": "shared",
                    "copy_semantics": "retain",
                    "move_semantics": "trivial",
                    "destruction": "release",
                    "moved_from_state": "valid",
                    "allocator_domain": "fixture-rc",
                    "operations": {
                        "retain": {
                            "symbol": "handle_retain",
                            "availability": "required",
                            "no_unwind": True,
                        },
                        "release": {
                            "symbol": "handle_release",
                            "availability": "required",
                            "no_unwind": True,
                        },
                    },
                },
            }
        ]
        self.assertEqual(validation_errors(document, strict=False), [])

    def test_shared_retain_without_retain_operation_is_rejected(self):
        document = portable_document()
        document["exports"]["symbols"] = [
            self._rc_operation_symbol("handle_release", "handle_release", False),
        ]
        document["exports"]["layouts"] = [
            {
                "type_name": "fixture.Handle",
                "size": 8,
                "alignment": 8,
                "fields": [],
                "lifecycle": {
                    "ownership_model": "shared",
                    "copy_semantics": "retain",
                    "move_semantics": "trivial",
                    "destruction": "release",
                    "moved_from_state": "valid",
                    "operations": {
                        "release": {
                            "symbol": "handle_release",
                            "availability": "required",
                            "no_unwind": True,
                        }
                    },
                },
            }
        ]
        errors = validation_errors(document, strict=False)
        self.assertTrue(
            any("operations.retain" in error for error in errors), errors
        )

    def test_legacy_abi_json_alias_remains_non_strict_compatible(self):
        legacy = {
            "dci": "1.0",
            "kind": "abi",
            "source": {"language": "cpp"},
            "exports": {
                "layouts": [
                    {
                        "type_name": "LegacyDerived",
                        "size": 16,
                        "alignment": 8,
                        "fields": [],
                        "bases": [
                            {
                                "type_name": "LegacyBase",
                                "adjustment": {
                                    "kind": "table",
                                    "table_offset": 4,
                                },
                            }
                        ],
                    }
                ],
                "symbols": [],
            },
        }

        self.assertEqual(validation_errors(copy.deepcopy(legacy), strict=False), [])


if __name__ == "__main__":
    unittest.main()
