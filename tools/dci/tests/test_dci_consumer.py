"""Linux-capable Core Consumer gate for the Vyx DCI implementation.

The end-to-end Rust fixtures under ``tests/projects/dci_rust_generic`` require a
``x86_64-pc-windows-msvc`` rustc to build the native static library and can only
link/run on Windows.  The Consumer's descriptor validation, Stub-request
recognition (SPEC 8.2.1 / 16.1) and exception-boundary rules (SPEC 11), however,
are exercised entirely through ``boot --emit=ir`` and need no foreign toolchain.

This module drives the built bootstrap compiler against hand-written contracts,
encoded to canonical ``.dcib`` exactly like a real build, and asserts the
Consumer accepts well-formed Direct contracts and fails closed with precise
diagnostics otherwise.  It is skipped when the compiler has not been built.
"""

from __future__ import annotations

import copy
import json
import os
import re
import subprocess
import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
DCI_TOOLS = REPO_ROOT / "tools" / "dci"
DCIB_TOOL = DCI_TOOLS / "dcib.py"
FIXTURE_DIR = REPO_ROOT / "tests" / "checks" / "dci" / "consumer"
BASE_DCI = FIXTURE_DIR / "dci" / "base.dci"
HOST_SRC = FIXTURE_DIR / "src" / "host.vyx"


def _find_compiler() -> Path | None:
    out_dir = REPO_ROOT / "bootstrap_compiler" / "out"
    # `.exe` first: a stale extensionless `boot` (an ELF from a Linux build)
    # otherwise wins the lookup on Windows and every spawn dies with
    # WinError 193 ("%1 is not a valid Win32 application").
    for name in ("boot.exe", "vyxc.exe", "boot", "vyxc"):
        candidate = out_dir / name
        if candidate.exists():
            return candidate
    return None


COMPILER = _find_compiler()


@unittest.skipUnless(
    COMPILER is not None and BASE_DCI.exists(),
    "bootstrap compiler is not built (run `vyxc build --target boot`)",
)
class DciConsumerGate(unittest.TestCase):
    """Drive the Core Consumer over canonical DCIB contracts on any host."""

    @classmethod
    def setUpClass(cls) -> None:
        cls._base = json.loads(BASE_DCI.read_text(encoding="utf-8"))
        cls._env = dict(os.environ)
        lib = str((REPO_ROOT / "bootstrap_compiler" / "out"))
        existing = cls._env.get("LD_LIBRARY_PATH", "")
        cls._env["LD_LIBRARY_PATH"] = lib + (os.pathsep + existing if existing else "")

    def _encode(self, document: dict, name: str) -> Path:
        json_path = Path(self._tmp) / f"{name}.dci"
        dcib_path = Path(self._tmp) / f"{name}.dcib"
        json_path.write_text(json.dumps(document, indent=1), encoding="utf-8")
        subprocess.run(
            [sys.executable, str(DCIB_TOOL), "encode", str(json_path), str(dcib_path)],
            check=True,
            capture_output=True,
        )
        return dcib_path

    def _consume(self, document: dict, name: str):
        dcib_path = self._encode(document, name)
        out_ll = Path(self._tmp) / f"{name}.ll"
        result = subprocess.run(
            [
                str(COMPILER),
                "--src=file",
                str(HOST_SRC),
                "--emit=ir",
                "--triplet=" + document["target"]["triple"],
                "--dci",
                str(dcib_path),
                "-o",
                str(out_ll),
            ],
            capture_output=True,
            text=True,
            env=self._env,
        )
        return result, out_ll

    def setUp(self) -> None:
        import tempfile

        self._tmpdir = tempfile.TemporaryDirectory()
        self._tmp = self._tmpdir.name

    def tearDown(self) -> None:
        self._tmpdir.cleanup()

    def _base_doc(self) -> dict:
        return copy.deepcopy(self._base)

    def assertConsumed(self, document: dict, name: str) -> str:
        result, out_ll = self._consume(document, name)
        combined = result.stdout + result.stderr
        self.assertEqual(
            result.returncode,
            0,
            f"{name} was rejected unexpectedly:\n{combined}",
        )
        self.assertTrue(out_ll.exists(), f"{name} did not emit IR:\n{combined}")
        return out_ll.read_text(encoding="utf-8")

    def assertRejected(self, document: dict, name: str, needle: str) -> None:
        result, _ = self._consume(document, name)
        combined = result.stdout + result.stderr
        self.assertEqual(
            result.returncode,
            1,
            f"{name} did not fail with the normal diagnostic exit code:\n{combined}",
        )
        self.assertIn(needle, combined, f"{name} diagnostic mismatch:\n{combined}")

    # --- baseline -------------------------------------------------------

    def test_direct_contract_is_consumed(self) -> None:
        ir = self.assertConsumed(self._base_doc(), "baseline")
        self.assertIn("contract_probe", ir)

    # --- item 1: normalized stub-request recognition + routing ----------

    def _with_stub_request(self, *, stub_mode: bool) -> dict:
        document = self._base_doc()
        if stub_mode:
            document["profile"]["consumer_modes"] = ["direct", "stub"]
        document["exports"]["stub_requests"] = [
            {
                "id": "probe.reverse.1",
                "kind": "operation_wrapper",
                "wrapper": {"link_name": "contract_probe_stub_wrapper"},
                "reason": "requires_stub",
                "target": "contract_probe",
            }
        ]
        return document

    def test_stub_request_requires_stub_consumer_mode(self) -> None:
        self.assertRejected(
            self._with_stub_request(stub_mode=False),
            "stub_request_direct_only",
            "declares stub_requests but the profile has no stub Consumer mode",
        )

    def test_stub_request_is_recognized_when_stub_mode_is_declared(self) -> None:
        # Routing a requires_stub operation to the Stub backend is a build-time
        # concern; the Consumer must accept the descriptor and still lower the
        # Direct surface the host actually calls.
        ir = self.assertConsumed(
            self._with_stub_request(stub_mode=True), "stub_request_with_mode"
        )
        self.assertIn("contract_probe", ir)

    def test_duplicate_stub_request_wrapper_is_rejected(self) -> None:
        document = self._with_stub_request(stub_mode=True)
        document["exports"]["stub_requests"].append(
            {
                "id": "probe.reverse.2",
                "kind": "operation_wrapper",
                "wrapper": {"link_name": "contract_probe_stub_wrapper"},
                "reason": "requires_stub",
                "target": "contract_probe",
            }
        )
        self.assertRejected(
            document,
            "stub_request_dup_wrapper",
            "duplicate DCI stub request wrapper",
        )

    def test_duplicate_stub_request_id_is_rejected(self) -> None:
        document = self._with_stub_request(stub_mode=True)
        clone = copy.deepcopy(document["exports"]["stub_requests"][0])
        clone["wrapper"]["link_name"] = "contract_probe_stub_wrapper_alt"
        document["exports"]["stub_requests"].append(clone)
        self.assertRejected(
            document,
            "stub_request_dup_id",
            "duplicate DCI stub request id",
        )

    def test_stub_request_wrapper_must_not_shadow_exported_symbol(self) -> None:
        document = self._with_stub_request(stub_mode=True)
        document["exports"]["stub_requests"][0]["wrapper"]["link_name"] = "contract_probe"
        self.assertRejected(
            document,
            "stub_request_collision",
            "collides with an exported symbol",
        )

    # --- item 1b: in-Consumer wrapper synthesis + call rebinding ---------

    def _with_synth_wrapper(self) -> dict:
        document = self._base_doc()
        document["profile"]["consumer_modes"] = ["direct", "stub"]
        document["exports"]["stub_requests"] = [
            {
                "id": "probe.forward.1",
                "kind": "operation_wrapper",
                "reason": "requires_stub",
                "target": "contract_probe",
                "synthesis": {"strategy": "forward_direct"},
                "wrapper": {"link_name": "contract_probe_forward_wrapper"},
            }
        ]
        return document

    def test_forward_direct_wrapper_is_synthesized(self) -> None:
        # The Consumer emits the wrapper itself and forwards to the Direct target;
        # no external Stub backend is required for the forwarding case.
        ir = self.assertConsumed(self._with_synth_wrapper(), "synth_forward")
        self.assertRegex(ir, r"define[^\n]*@contract_probe_forward_wrapper")
        self.assertIn("@contract_probe", ir)

    def test_forward_direct_wrapper_unresolved_target_is_rejected(self) -> None:
        document = self._with_synth_wrapper()
        document["exports"]["stub_requests"][0]["target"] = "does_not_exist"
        self.assertRejected(
            document,
            "synth_forward_bad_target",
            "is not a uniquely resolvable exported symbol",
        )

    def test_unsupported_synthesis_strategy_is_rejected(self) -> None:
        document = self._with_synth_wrapper()
        document["exports"]["stub_requests"][0]["synthesis"]["strategy"] = "reverse_thunk"
        self.assertRejected(
            document,
            "synth_bad_strategy",
            "unsupported DCI stub wrapper synthesis strategy",
        )

    def _with_translate_unwind(self) -> dict:
        document = self._base_doc()
        document["profile"]["consumer_modes"] = ["direct", "stub"]
        throw_fn = copy.deepcopy(document["exports"]["symbols"][0])
        throw_fn["name"] = "throw_probe"
        throw_fn["link_name"] = "throw_probe"
        throw_fn["control_flow"] = {
            "default_boundary": "no_unwind",
            "propagation": {"mode": "forbidden"},
            "unwind": "may_unwind",
            "boundary_action": "stub_required",
        }
        translator = self._translator_symbol()
        translator["name"] = "dci_tr_throw_probe"
        translator["link_name"] = "dci_tr_throw_probe"
        translator["control_flow"] = {
            "default_boundary": "no_unwind",
            "boundary": "no_unwind",
            "propagation": {"mode": "forbidden"},
            "unwind": "no_unwind",
            "boundary_action": "direct",
        }
        document["exports"]["symbols"].extend([throw_fn, translator])
        document["exports"]["stub_requests"] = [
            {
                "id": "throw.translate.1",
                "kind": "operation_wrapper",
                "reason": "requires_stub",
                "target": "throw_probe",
                "synthesis": {"strategy": "translate_unwind", "target": "throw_probe"},
                "wrapper": {"link_name": "dci_tr_throw_probe"},
            }
        ]
        return document

    def test_translate_unwind_is_not_llvm_synthesized(self) -> None:
        ir = self.assertConsumed(self._with_translate_unwind(), "synth_translate")
        self.assertIn("contract_probe", ir)
        self.assertNotRegex(ir, r"define[^\n]*@dci_tr_throw_probe")
        self.assertNotIn("personality", ir.lower())
        self.assertNotIn("__CxxFrameHandler3", ir)
        self.assertNotIn("__gxx_personality_v0", ir)
        self.assertNotIn("landingpad", ir)
        self.assertNotIn("cleanuppad", ir)

    def test_translate_unwind_missing_translator_is_rejected(self) -> None:
        document = self._with_translate_unwind()
        document["exports"]["symbols"] = [
            symbol
            for symbol in document["exports"]["symbols"]
            if symbol.get("link_name") != "dci_tr_throw_probe"
        ]
        self.assertRejected(
            document,
            "synth_translate_missing",
            "is not an exported Direct symbol",
        )

    # --- item 2: cross-language exception boundary ----------------------

    def test_translated_boundary_requires_translator_symbol(self) -> None:
        document = self._base_doc()
        document["control_flow"] = {
            "default_boundary": "translated",
            "propagation": {"mode": "translated"},
        }
        self.assertRejected(
            document,
            "translated_no_translator",
            "boundary `translated` requires a declared translator symbol",
        )

    def test_shared_abi_boundary_requires_shared_abi_identity(self) -> None:
        document = self._base_doc()
        document["control_flow"] = {
            "default_boundary": "shared_abi",
            "propagation": {"mode": "shared_abi"},
        }
        self.assertRejected(
            document,
            "shared_abi_no_identity",
            "boundary `shared_abi` requires a declared shared propagation ABI",
        )

    def test_translated_boundary_with_data_is_still_unsupported_in_direct_mode(self) -> None:
        # A translated boundary needs a Stub to convert control flow to a value.
        document = self._base_doc()
        document["control_flow"] = {
            "default_boundary": "translated",
            "propagation": {
                "mode": "translated",
                "translator_symbol": "contract_translate_failure",
            },
        }
        self.assertRejected(
            document,
            "translated_with_data",
            "requires an unsupported propagation ABI",
        )

    def test_shared_abi_boundary_with_data_emits_personality(self) -> None:
        document = self._base_doc()
        document["control_flow"] = {
            "default_boundary": "shared_abi",
            "propagation": {"mode": "shared_abi", "abi": "dci.eh.msvc-cxx.v1"},
        }
        ir = self.assertConsumed(document, "shared_abi_with_data")
        self.assertTrue(
            "__CxxFrameHandler3" in ir or "__gxx_personality_v0" in ir,
            ir,
        )
        self.assertIn("uwtable", ir.lower())
        self.assertRegex(ir, r"invoke[^\n]*@contract_probe")
        self.assertTrue(
            "cleanuppad" in ir or "landingpad" in ir,
            ir,
        )
        self.assertNotIn("__cxa_begin_catch", ir)
        self.assertNotIn("__cxa_throw", ir)

    def test_shared_abi_unknown_identity_is_rejected(self) -> None:
        for abi in ("arbitrary-nonempty-name", "dci.eh.msvc-cxx.v2", "dci.eh.itanium-cxx.v1"):
            with self.subTest(abi=abi):
                document = self._base_doc()
                document["control_flow"] = {
                    "default_boundary": "shared_abi",
                    "propagation": {"mode": "shared_abi", "abi": abi},
                }
                self.assertRejected(document, "unknown_shared_abi", "requires an unsupported propagation ABI")

    def test_shared_abi_requires_participating_caller(self) -> None:
        document = self._base_doc()
        document["exports"]["symbols"][0]["control_flow"] = {
            "boundary": "shared_abi",
            "propagation": {"mode": "shared_abi", "abi": "dci.eh.msvc-cxx.v1"},
        }
        self.assertRejected(document, "unprotected_shared_call", "caller participating")

    def test_reverse_override_wrapper_is_synthesized(self) -> None:
        document = self._with_synth_wrapper()
        document["exports"]["stub_requests"][0]["id"] = "probe.reverse.1"
        document["exports"]["stub_requests"][0]["kind"] = "reverse_override"
        document["exports"]["stub_requests"][0]["synthesis"]["strategy"] = "reverse_override"
        document["exports"]["stub_requests"][0]["wrapper"]["link_name"] = "contract_probe_reverse_wrapper"
        ir = self.assertConsumed(document, "synth_reverse")
        self.assertRegex(ir, r"define[^\n]*@contract_probe_reverse_wrapper")
        self.assertIn("@contract_probe", ir)

    def _translator_symbol(self) -> dict:
        return {
            "name": "contract_translate_failure",
            "link_name": "contract_translate_failure",
            "kind": "function",
            "calling_convention": "system",
            "params": [
                {
                    "name": "code",
                    "type": {
                        "name": "i32",
                        "kind": "primitive",
                        "nullable": False,
                        "reference": "value",
                    },
                    "location": "abi",
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
                "location": "abi",
                "ownership": "copy",
            },
            "abi": {
                "calling_convention": "system",
                "variadic": False,
                "parameters": [{"index": 0, "passing": "direct", "size": 4}],
                "return": {"passing": "direct", "size": 4},
            },
        }

    def test_translated_boundary_with_resolvable_translator_is_consumed(self) -> None:
        # A well-formed translated boundary whose translator resolves to a Direct
        # no-unwind symbol is now realized at runtime: the Consumer accepts it and
        # keeps the foreign surface no-unwind (the producer converts to a value
        # before crossing, so no unwind reaches Vyx).
        document = self._base_doc()
        document["exports"]["symbols"].append(self._translator_symbol())
        document["control_flow"] = {
            "default_boundary": "translated",
            "propagation": {
                "mode": "translated",
                "translator_symbol": "contract_translate_failure",
            },
        }
        ir = self.assertConsumed(document, "translated_realized")
        self.assertIn("contract_probe", ir)

    def test_translated_boundary_with_unresolved_translator_is_rejected(self) -> None:
        document = self._base_doc()
        document["control_flow"] = {
            "default_boundary": "translated",
            "propagation": {
                "mode": "translated",
                "translator_symbol": "not_an_exported_symbol",
            },
        }
        self.assertRejected(
            document,
            "translated_unresolved",
            "requires an unsupported propagation ABI",
        )


@unittest.skipUnless(
    COMPILER is not None,
    "bootstrap compiler is not built (run `vyxc build --target boot`)",
)
class DciSharedLifecycleGate(unittest.TestCase):
    """Drive the HIR shared-ownership (retain/release) binder on any host.

    A ``shared`` type whose lifecycle declares ``copy_semantics = retain`` and
    ``destruction = release`` must balance its refcount with an automatic release
    at scope exit, exactly like the operation-owned unique drop path.  These
    contracts are consumed through ``boot --emit=ir`` and need no foreign
    toolchain, so the binder is exercised directly on Linux.
    """

    HOST = (
        'extern "dci" {\n'
        "    struct rc.Handle {\n"
        "        public slot: i64;\n"
        "    };\n"
        "    fn rc_handle_create(seed: i64) -> rc.Handle;\n"
        "    fn rc_handle_retain(value: *rc.Handle) -> rc.Handle;\n"
        "    fn rc_handle_release(value: *rc.Handle);\n"
        "}\n\n"
        "fn main() -> i32 {\n"
        "    var h = rc_handle_create(1);\n"
        "    return 0;\n"
        "}\n"
    )

    @classmethod
    def setUpClass(cls) -> None:
        cls._env = dict(os.environ)
        lib = str((REPO_ROOT / "bootstrap_compiler" / "out"))
        existing = cls._env.get("LD_LIBRARY_PATH", "")
        cls._env["LD_LIBRARY_PATH"] = lib + (os.pathsep + existing if existing else "")

    def setUp(self) -> None:
        import tempfile

        self._tmpdir = tempfile.TemporaryDirectory()
        self._tmp = self._tmpdir.name

    def tearDown(self) -> None:
        self._tmpdir.cleanup()

    @staticmethod
    def _record(name: str, reference: str = "value") -> dict:
        return {"name": name, "kind": "record", "nullable": False, "reference": reference}

    def _fn_symbol(self, name: str, params: list, returns_value: bool) -> dict:
        return {
            "name": name,
            "link_name": name,
            "kind": "function",
            "calling_convention": "c",
            "params": params,
            "return": (
                {"type": self._record("rc.Handle"), "ownership": "owned"}
                if returns_value
                else None
            ),
            "abi": {
                "calling_convention": "c",
                "variadic": False,
                "parameters": [
                    {"index": i, "passing": "direct", "size": 8}
                    for i in range(len(params))
                ],
                "return": (
                    {"passing": "direct", "size": 8}
                    if returns_value
                    else {"passing": "ignore"}
                ),
            },
        }

    def _shared_document(
        self,
        *,
        ownership_model: str = "shared",
        copy_semantics: str = "retain",
        destruction: str = "release",
        include_retain: bool = True,
    ) -> dict:
        i64 = {"name": "i64", "kind": "primitive", "nullable": False, "reference": "value"}
        handle_ptr = self._record("rc.Handle", "pointer")
        operations = {
            "create": {
                "symbol": "rc_handle_create",
                "availability": "required",
                "no_unwind": True,
            },
            "release": {
                "symbol": "rc_handle_release",
                "availability": "required",
                "no_unwind": True,
            },
        }
        if include_retain:
            operations["retain"] = {
                "symbol": "rc_handle_retain",
                "availability": "required",
                "no_unwind": True,
            }
        return {
            "dci": "1.0",
            "kind": "abi",
            "profile": {
                "id": "rc.gate",
                "version": "1.0",
                "level": "L2",
                "consumer_modes": ["direct"],
                "lifecycle_binding": "automatic",
            },
            "source": {"language": "fixture"},
            "target": {
                "triple": "x86_64-pc-windows-msvc",
                "architecture": "x86_64",
                "pointer_width": 64,
                "endianness": "little",
                "abi": "system",
            },
            "control_flow": {
                "default_boundary": "no_unwind",
                "propagation": {"mode": "forbidden"},
            },
            "exports": {
                "layouts": [
                    {
                        "type_name": "rc.Handle",
                        "size": 8,
                        "alignment": 8,
                        "representation": "stable",
                        "is_pod": True,
                        "is_trivially_destructible": False,
                        "fields": [{"name": "slot", "offset": 0, "type": i64}],
                        "lifecycle": {
                            "ownership_model": ownership_model,
                            "copy_semantics": copy_semantics,
                            "move_semantics": "trivial",
                            "destruction": destruction,
                            "moved_from_state": "valid",
                            "allocator_domain": "rc",
                            "operations": operations,
                        },
                    }
                ],
                "symbols": [
                    self._fn_symbol(
                        "rc_handle_create",
                        [{"name": "seed", "type": i64, "ownership": "copy"}],
                        True,
                    ),
                    self._fn_symbol(
                        "rc_handle_retain",
                        [{"name": "value", "type": handle_ptr, "ownership": "borrow"}],
                        True,
                    ),
                    self._fn_symbol(
                        "rc_handle_release",
                        [{"name": "value", "type": handle_ptr, "ownership": "move"}],
                        False,
                    ),
                ],
            },
        }

    def _compile(self, document: dict, name: str):
        json_path = Path(self._tmp) / f"{name}.dci"
        dcib_path = Path(self._tmp) / f"{name}.dcib"
        host_path = Path(self._tmp) / f"{name}.vyx"
        out_ll = Path(self._tmp) / f"{name}.ll"
        json_path.write_text(json.dumps(document, indent=1), encoding="utf-8")
        host_path.write_text(self.HOST, encoding="utf-8")
        subprocess.run(
            [sys.executable, str(DCIB_TOOL), "encode", str(json_path), str(dcib_path)],
            check=True,
            capture_output=True,
        )
        result = subprocess.run(
            [
                str(COMPILER),
                "--src=file",
                str(host_path),
                "--emit=ir",
                "--dci",
                str(dcib_path),
                "-o",
                str(out_ll),
            ],
            capture_output=True,
            text=True,
            env=self._env,
        )
        return result, out_ll

    def test_shared_local_is_released_at_scope_exit(self) -> None:
        result, out_ll = self._compile(self._shared_document(), "shared_release")
        combined = result.stdout + result.stderr
        self.assertEqual(result.returncode, 0, combined)
        ir = out_ll.read_text(encoding="utf-8")
        self.assertEqual(len(re.findall(r"call[^\n]*@rc_handle_release\(", ir)), 1)

    def test_retain_semantics_requires_a_retain_operation(self) -> None:
        result, _ = self._compile(
            self._shared_document(include_retain=False), "shared_no_retain"
        )
        combined = result.stdout + result.stderr
        self.assertEqual(result.returncode, 1, combined)
        self.assertIn("requires operation `retain`", combined)

    def test_release_destruction_requires_shared_ownership(self) -> None:
        result, _ = self._compile(
            self._shared_document(ownership_model="unique"), "shared_wrong_owner"
        )
        combined = result.stdout + result.stderr
        self.assertEqual(result.returncode, 1, combined)
        self.assertIn("requires shared ownership", combined)


@unittest.skipUnless(
    COMPILER is not None,
    "bootstrap compiler is not built (run `vyxc build --target boot`)",
)
class DciAutomaticCopyGate(unittest.TestCase):
    """Bare local-to-local init becomes a Descriptor copy when move is forbidden."""

    HOST = (
        'extern "dci" {\n'
        "    struct box.Handle {\n"
        "        public slot: i64;\n"
        "    };\n"
        "    fn box_handle_create(seed: i64) -> box.Handle;\n"
        "    fn box_handle_copy(value: *box.Handle) -> box.Handle;\n"
        "    fn box_handle_move(value: *box.Handle) -> box.Handle;\n"
        "    fn box_handle_destroy(value: *box.Handle);\n"
        "}\n\n"
        "fn main() -> i32 {\n"
        "    var original = box_handle_create(1);\n"
        "    var copied = original;\n"
        "    return 0;\n"
        "}\n"
    )

    HOST_REUSE = (
        'extern "dci" {\n'
        "    struct box.Handle {\n"
        "        public slot: i64;\n"
        "    };\n"
        "    fn box_handle_create(seed: i64) -> box.Handle;\n"
        "    fn box_handle_copy(value: *box.Handle) -> box.Handle;\n"
        "    fn box_handle_move(value: *box.Handle) -> box.Handle;\n"
        "    fn box_handle_destroy(value: *box.Handle);\n"
        "}\n\n"
        "fn main() -> i32 {\n"
        "    var original = box_handle_create(1);\n"
        "    var first = original;\n"
        "    var second = original;\n"
        "    return 0;\n"
        "}\n"
    )

    HOST_FIELD_AFTER = (
        'extern "dci" {\n'
        "    struct box.Handle {\n"
        "        public slot: i64;\n"
        "    };\n"
        "    fn box_handle_create(seed: i64) -> box.Handle;\n"
        "    fn box_handle_copy(value: *box.Handle) -> box.Handle;\n"
        "    fn box_handle_move(value: *box.Handle) -> box.Handle;\n"
        "    fn box_handle_destroy(value: *box.Handle);\n"
        "}\n\n"
        "fn main() -> i32 {\n"
        "    var original = box_handle_create(1);\n"
        "    var first = original;\n"
        "    return original.slot as i32;\n"
        "}\n"
    )

    @classmethod
    def setUpClass(cls) -> None:
        cls._env = dict(os.environ)
        lib = str((REPO_ROOT / "bootstrap_compiler" / "out"))
        existing = cls._env.get("LD_LIBRARY_PATH", "")
        cls._env["LD_LIBRARY_PATH"] = lib + (os.pathsep + existing if existing else "")

    def setUp(self) -> None:
        import tempfile

        self._tmpdir = tempfile.TemporaryDirectory()
        self._tmp = self._tmpdir.name

    def tearDown(self) -> None:
        self._tmpdir.cleanup()

    @staticmethod
    def _record(name: str, reference: str = "value") -> dict:
        return {"name": name, "kind": "record", "nullable": False, "reference": reference}

    def _fn_symbol(self, name: str, params: list, returns_value: bool) -> dict:
        return {
            "name": name,
            "link_name": name,
            "kind": "function",
            "calling_convention": "c",
            "params": params,
            "return": (
                {"type": self._record("box.Handle"), "ownership": "owned"}
                if returns_value
                else None
            ),
            "abi": {
                "calling_convention": "c",
                "variadic": False,
                "parameters": [
                    {"index": i, "passing": "direct", "size": 8}
                    for i in range(len(params))
                ],
                "return": (
                    {"passing": "direct", "size": 8}
                    if returns_value
                    else {"passing": "ignore"}
                ),
            },
        }

    def _copy_document(
        self,
        *,
        copy_semantics: str = "operation",
        move_semantics: str = "forbidden",
        moved_from_state: str = "valid",
        allocator_domain: str = "box",
        extra_layout_domain: str | None = None,
    ) -> dict:
        i64 = {"name": "i64", "kind": "primitive", "nullable": False, "reference": "value"}
        handle_ptr = self._record("box.Handle", "pointer")
        operations = {
            "create": {
                "symbol": "box_handle_create",
                "availability": "required",
                "no_unwind": True,
            },
            "destroy": {
                "symbol": "box_handle_destroy",
                "availability": "required",
                "no_unwind": True,
            },
        }
        if copy_semantics == "operation":
            operations["copy"] = {
                "symbol": "box_handle_copy",
                "availability": "required",
                "no_unwind": True,
            }
        if move_semantics == "operation":
            operations["move"] = {
                "symbol": "box_handle_move",
                "availability": "required",
                "no_unwind": True,
            }
        lifecycle = {
            "ownership_model": "unique",
            "copy_semantics": copy_semantics,
            "move_semantics": move_semantics,
            "destruction": "operation",
            "moved_from_state": moved_from_state,
            "operations": operations,
        }
        if allocator_domain is not None:
            lifecycle["allocator_domain"] = allocator_domain
        layouts = [
            {
                "type_name": "box.Handle",
                "size": 8,
                "alignment": 8,
                "representation": "stable",
                "is_pod": True,
                "is_trivially_destructible": False,
                "fields": [{"name": "slot", "offset": 0, "type": i64}],
                "lifecycle": lifecycle,
            }
        ]
        if extra_layout_domain is not None:
            other_ops = {
                "create": {
                    "symbol": "box_handle_create",
                    "availability": "required",
                    "no_unwind": True,
                },
                "destroy": {
                    "symbol": "box_handle_destroy",
                    "availability": "required",
                    "no_unwind": True,
                },
            }
            layouts.append(
                {
                    "type_name": "rc.Handle",
                    "size": 8,
                    "alignment": 8,
                    "representation": "stable",
                    "is_pod": True,
                    "is_trivially_destructible": False,
                    "fields": [{"name": "slot", "offset": 0, "type": i64}],
                    "lifecycle": {
                        "ownership_model": "unique",
                        "copy_semantics": "forbidden",
                        "move_semantics": "forbidden",
                        "destruction": "operation",
                        "moved_from_state": "valid",
                        "allocator_domain": extra_layout_domain,
                        "operations": other_ops,
                    },
                }
            )
        symbols = [
            self._fn_symbol(
                "box_handle_create",
                [{"name": "seed", "type": i64, "ownership": "copy"}],
                True,
            ),
            self._fn_symbol(
                "box_handle_copy",
                [{"name": "value", "type": handle_ptr, "ownership": "borrow"}],
                True,
            ),
            self._fn_symbol(
                "box_handle_destroy",
                [{"name": "value", "type": handle_ptr, "ownership": "move"}],
                False,
            ),
        ]
        if move_semantics == "operation":
            symbols.insert(
                2,
                self._fn_symbol(
                    "box_handle_move",
                    [{"name": "value", "type": handle_ptr, "ownership": "move"}],
                    True,
                ),
            )
        return {
            "dci": "1.0",
            "kind": "abi",
            "profile": {
                "id": "box.copy.gate",
                "version": "1.0",
                "level": "L2",
                "consumer_modes": ["direct"],
                "lifecycle_binding": "automatic",
            },
            "source": {"language": "fixture"},
            "target": {
                "triple": "x86_64-pc-windows-msvc",
                "architecture": "x86_64",
                "pointer_width": 64,
                "endianness": "little",
                "abi": "system",
            },
            "control_flow": {
                "default_boundary": "no_unwind",
                "propagation": {"mode": "forbidden"},
            },
            "exports": {
                "layouts": layouts,
                "symbols": symbols,
            },
        }

    def _compile(self, document: dict, name: str, host: str | None = None):
        json_path = Path(self._tmp) / f"{name}.dci"
        dcib_path = Path(self._tmp) / f"{name}.dcib"
        host_path = Path(self._tmp) / f"{name}.vyx"
        out_ll = Path(self._tmp) / f"{name}.ll"
        json_path.write_text(json.dumps(document, indent=1), encoding="utf-8")
        host_path.write_text(host if host is not None else self.HOST, encoding="utf-8")
        subprocess.run(
            [sys.executable, str(DCIB_TOOL), "encode", str(json_path), str(dcib_path)],
            check=True,
            capture_output=True,
        )
        result = subprocess.run(
            [
                str(COMPILER),
                "--src=file",
                str(host_path),
                "--emit=ir",
                "--dci",
                str(dcib_path),
                "-o",
                str(out_ll),
            ],
            capture_output=True,
            text=True,
            env=self._env,
        )
        return result, out_ll

    def test_bare_local_init_is_rewritten_to_copy(self) -> None:
        result, out_ll = self._compile(self._copy_document(), "auto_copy")
        combined = result.stdout + result.stderr
        self.assertEqual(result.returncode, 0, combined)
        ir = out_ll.read_text(encoding="utf-8")
        self.assertEqual(len(re.findall(r"call[^\n]*@box_handle_copy\(", ir)), 1)
        self.assertEqual(len(re.findall(r"call[^\n]*@box_handle_destroy\(", ir)), 2)

    def test_copy_forbidden_bare_init_is_rejected(self) -> None:
        result, _ = self._compile(
            self._copy_document(copy_semantics="forbidden"), "copy_forbidden"
        )
        combined = result.stdout + result.stderr
        self.assertEqual(result.returncode, 1, combined)
        self.assertIn("forbidden by its DCI move/copy contract", combined)

    def test_reuse_copies_then_last_use_moves(self) -> None:
        result, out_ll = self._compile(
            self._copy_document(
                copy_semantics="operation",
                move_semantics="operation",
            ),
            "path_copy_then_move",
            host=self.HOST_REUSE,
        )
        combined = result.stdout + result.stderr
        self.assertEqual(result.returncode, 0, combined)
        ir = out_ll.read_text(encoding="utf-8")
        self.assertEqual(len(re.findall(r"call[^\n]*@box_handle_copy\(", ir)), 1)
        self.assertEqual(len(re.findall(r"call[^\n]*@box_handle_move\(", ir)), 1)

    def test_last_use_moves_when_copy_also_available(self) -> None:
        result, out_ll = self._compile(
            self._copy_document(
                copy_semantics="operation",
                move_semantics="operation",
            ),
            "path_last_use_move",
        )
        combined = result.stdout + result.stderr
        self.assertEqual(result.returncode, 0, combined)
        ir = out_ll.read_text(encoding="utf-8")
        self.assertEqual(len(re.findall(r"call[^\n]*@box_handle_move\(", ir)), 1)
        self.assertIsNone(re.search(r"call[^\n]*@box_handle_copy\(", ir))

    def test_later_field_use_copies_instead_of_moving(self) -> None:
        result, out_ll = self._compile(
            self._copy_document(
                copy_semantics="operation",
                move_semantics="operation",
            ),
            "path_field_use_copy",
            host=self.HOST_FIELD_AFTER,
        )
        combined = result.stdout + result.stderr
        self.assertEqual(result.returncode, 0, combined)
        ir = out_ll.read_text(encoding="utf-8")
        self.assertEqual(len(re.findall(r"call[^\n]*@box_handle_copy\(", ir)), 1)
        self.assertIsNone(re.search(r"call[^\n]*@box_handle_move\(", ir))

    def test_reuse_without_copy_is_rejected(self) -> None:
        result, _ = self._compile(
            self._copy_document(
                copy_semantics="forbidden",
                move_semantics="operation",
            ),
            "path_reuse_no_copy",
            host=self.HOST_REUSE,
        )
        combined = result.stdout + result.stderr
        self.assertEqual(result.returncode, 1, combined)
        self.assertIn("still used", combined)

    def test_empty_allocator_domain_is_rejected(self) -> None:
        result, _ = self._compile(
            self._copy_document(allocator_domain=""),
            "empty_allocator_domain",
        )
        combined = result.stdout + result.stderr
        self.assertEqual(result.returncode, 1, combined)
        self.assertIn("allocator_domain", combined)

    def test_allocator_domain_symbol_conflict_is_rejected(self) -> None:
        result, _ = self._compile(
            self._copy_document(extra_layout_domain="rc"),
            "allocator_domain_conflict",
        )
        combined = result.stdout + result.stderr
        self.assertEqual(result.returncode, 1, combined)
        self.assertIn("allocator_domain", combined)

    def test_destructible_only_is_accepted_for_last_use_move(self) -> None:
        result, out_ll = self._compile(
            self._copy_document(
                copy_semantics="operation",
                move_semantics="operation",
                moved_from_state="destructible_only",
            ),
            "destructible_only_last_use",
        )
        combined = result.stdout + result.stderr
        self.assertEqual(result.returncode, 0, combined)
        ir = out_ll.read_text(encoding="utf-8")
        self.assertEqual(len(re.findall(r"call[^\n]*@box_handle_move\(", ir)), 1)


def _llvm_root() -> str | None:
    root = os.environ.get("LLVM_ROOT")
    if root and Path(root).exists():
        return root
    for candidate in ("/usr/lib/llvm-22", "/usr/lib/llvm-21", "/usr/lib/llvm-20"):
        if Path(candidate).exists():
            return candidate
    return None


@unittest.skipUnless(
    COMPILER is not None and _llvm_root() is not None,
    "bootstrap compiler or LLVM toolchain is unavailable for a full build",
)
class DciDescriptorCacheGate(unittest.TestCase):
    """Prove the incremental Descriptor cache is content-addressed (SPEC 16.5).

    A ``.dcib`` contract can be regenerated in place with an identical size and a
    restored mtime.  A stat-only cache key (mtime + size) would then produce a
    false hit and silently reuse a stale object; the content-addressed key must
    force a rebuild of every compile unit that consumes the descriptor.
    """

    @classmethod
    def setUpClass(cls) -> None:
        cls._env = dict(os.environ)
        lib = str((REPO_ROOT / "bootstrap_compiler" / "out"))
        existing = cls._env.get("LD_LIBRARY_PATH", "")
        cls._env["LD_LIBRARY_PATH"] = lib + (os.pathsep + existing if existing else "")
        cls._env["LLVM_ROOT"] = _llvm_root()

    def _build(self, project: Path) -> str:
        result = subprocess.run(
            [str(COMPILER), "build", "-j", "4"],
            cwd=str(project),
            capture_output=True,
            text=True,
            env=self._env,
        )
        self.assertEqual(
            result.returncode,
            0,
            f"build failed:\n{result.stdout}\n{result.stderr}",
        )
        return result.stdout + result.stderr

    def _encode(self, dci_json: dict, dcib_path: Path) -> None:
        json_path = dcib_path.with_suffix(".dci")
        json_path.write_text(json.dumps(dci_json, indent=1), encoding="utf-8")
        subprocess.run(
            [sys.executable, str(DCIB_TOOL), "encode", str(json_path), str(dcib_path)],
            check=True,
            capture_output=True,
        )

    def test_descriptor_content_change_invalidates_compile_cache(self) -> None:
        import tempfile

        base = json.loads(BASE_DCI.read_text(encoding="utf-8"))
        with tempfile.TemporaryDirectory() as tmp:
            project = Path(tmp)
            (project / "src").mkdir()
            (project / "dci").mkdir()
            (project / "src" / "main.vyx").write_text(
                'extern "dci" {\n'
                "    fn contract_probe(value: i32) -> i32;\n"
                "}\n\n"
                "fn main() -> i32 {\n"
                "    return 0;\n"
                "}\n",
                encoding="utf-8",
            )
            (project / "Vyx.toml").write_text(
                "[package]\n"
                'name = "dci_cache_gate"\n'
                'version = "0.1.0"\n\n'
                "[target.dci_cache_gate]\n"
                'kind = "vyx"\n'
                'entry = "src/main.vyx"\n'
                'output = "out"\n'
                'dci_file = "dci/base.dcib"\n',
                encoding="utf-8",
            )
            dcib = project / "dci" / "base.dcib"
            self._encode(base, dcib)

            self.assertIn("compile src/main.vyx", self._build(project))
            self.assertNotIn(
                "compile src/main.vyx",
                self._build(project),
                "unchanged descriptor unexpectedly recompiled its consumer",
            )

            # Regenerate a different but equally sized contract, then restore the
            # original size and mtime so only content differs.
            stat = os.stat(dcib)
            variant = json.loads(json.dumps(base))
            variant["profile"]["version"] = "2.0"  # same textual length as "1.0"
            variant_dcib = project / "dci" / "variant.dcib"
            self._encode(variant, variant_dcib)
            self.assertEqual(
                variant_dcib.stat().st_size,
                stat.st_size,
                "variant contract is not the same byte length; test cannot isolate content",
            )
            dcib.write_bytes(variant_dcib.read_bytes())
            os.utime(dcib, ns=(stat.st_atime_ns, stat.st_mtime_ns))
            self.assertEqual(dcib.stat().st_size, stat.st_size)
            self.assertEqual(dcib.stat().st_mtime_ns, stat.st_mtime_ns)

            self.assertIn(
                "compile src/main.vyx",
                self._build(project),
                "content-only descriptor change failed to invalidate the compile cache",
            )


if __name__ == "__main__":
    unittest.main()
