from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).parents[1] / "dci_adapter_msvc.py"
SPEC = importlib.util.spec_from_file_location("dci_adapter_cpp_multiabi", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
adapter = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = adapter
SPEC.loader.exec_module(adapter)


class MultiAbiLayoutTests(unittest.TestCase):
    def test_itanium_record_layout_accepts_dsize_and_vtable_pointer(self) -> None:
        dump = """
*** Dumping AST Record Layout
         0 | struct Value
         0 |   (Value vtable pointer)
         8 |   long payload
           | [sizeof=16, dsize=16, align=8,
           |  nvsize=16, nvalign=8]
"""
        records = adapter.parse_record_layouts(dump, {"Value"})
        self.assertEqual(len(records), 1)
        self.assertEqual(records[0].size, 16)
        self.assertEqual(records[0].alignment, 8)
        self.assertTrue(records[0].has_vtable)
        self.assertEqual(records[0].fields[0].name, "payload")

    def test_itanium_vtable_uses_target_pointer_width_and_address_point(self) -> None:
        symbol = adapter.Symbol(
            name="Poly::value",
            owner="Poly",
            member_name="value",
            mangled="_ZN4Poly5valueEi",
            kind="method",
            calling_convention="cxx_method",
            params=[{"name": "v", "type": adapter.dci_type("int")}],
            ret={"type": adapter.dci_type("int")},
            is_virtual=True,
        )
        dump = """
Vtable for 'Poly' (3 entries).
   0 | offset_to_top (0)
   1 | Poly RTTI
       -- (Poly, 0) vtable address --
   2 | int Poly::value(int)

VTable indices for 'Poly' (1 entries).
   0 | int Poly::value(int)
"""
        tables = adapter.parse_vtable_layouts(
            dump, {"Poly"}, [symbol], pointer_width=32
        )
        self.assertEqual(len(tables), 1)
        self.assertEqual([entry.offset for entry in tables[0].entries], [0, 4, 8])
        self.assertEqual(adapter.vtable_address_point_offset(tables[0]), 8)
        self.assertEqual(tables[0].entries[0].kind, "metadata")
        self.assertEqual(tables[0].entries[2].mangled, symbol.mangled)

    def test_itanium_secondary_address_points_become_separate_tables(self) -> None:
        dump = """
Vtable for 'Derived' (8 entries).
   0 | offset_to_top (0)
   1 | Derived RTTI
       -- (Left, 0) vtable address --
       -- (Derived, 0) vtable address --
   2 | int Derived::left()
   3 | int Derived::own()
   4 | offset_to_top (-8)
   5 | Derived RTTI
       -- (Right, 8) vtable address --
   6 | int Derived::right()
       [this adjustment: -8 non-virtual] method: int Right::right()
   7 | Derived::~Derived() [deleting]

VTable indices for 'Derived' (3 entries).
"""
        tables = adapter.parse_vtable_layouts(
            dump, {"Derived", "Left", "Right"}, [], pointer_width=64
        )
        self.assertEqual(len(tables), 2)
        self.assertEqual(tables[0].base_class, "Left")
        self.assertEqual(tables[1].base_class, "Right")
        self.assertEqual([entry.index for entry in tables[1].entries], [0, 1, 2, 3])
        self.assertEqual([entry.offset for entry in tables[1].entries], [0, 8, 16, 24])
        self.assertEqual(adapter.vtable_address_point_offset(tables[1]), 16)
        self.assertEqual(tables[1].entries[2].this_adjust, -8)
        self.assertEqual(tables[1].entries[2].mangled, "")

    def test_long_width_follows_verified_target_data_model(self) -> None:
        original = adapter.CPP_LONG_WIDTH
        try:
            adapter.CPP_LONG_WIDTH = 64
            self.assertEqual(adapter.dci_type("long")["name"], "i64")
            self.assertEqual(adapter.dci_type("unsigned long")["name"], "u64")
            self.assertEqual(adapter.bitfield_storage_size("unsigned long"), 8)
            self.assertFalse(adapter.bitfield_is_signed("unsigned long"))
            adapter.CPP_LONG_WIDTH = 32
            self.assertEqual(adapter.dci_type("long")["name"], "i32")
            self.assertEqual(adapter.dci_type("unsigned long")["name"], "u32")
            original_pointer = adapter.CPP_POINTER_WIDTH
            adapter.CPP_POINTER_WIDTH = 32
            self.assertEqual(adapter.dci_type("std::size_t")["name"], "u32")
            adapter.CPP_POINTER_WIDTH = 64
            self.assertEqual(adapter.dci_type("std::size_t")["name"], "u64")
            adapter.CPP_POINTER_WIDTH = original_pointer
        finally:
            adapter.CPP_LONG_WIDTH = original

    def test_itanium_aggregate_lowering_fails_closed_without_machine_probe(self) -> None:
        record = adapter.RecordLayout(
            "Pair", size=16, alignment=8, traits={"can_pass_in_registers": True}
        )
        with self.assertRaisesRegex(
            adapter.AdapterContractError, "verified machine-signature"
        ):
            adapter.abi_value_lowering(
                adapter.dci_type("Pair"),
                [record],
                "parameter",
                "x86_64-unknown-linux-gnu",
            )

    def test_clang_ir_copies_param_and_return_types_independently(self) -> None:
        ir = """
declare void @vyx_arg0(ptr, i64)
declare { i64, i64 } @vyx_ret0()
declare void @vyx_arg1(i64, i64)
declare i64 @vyx_ret1()
declare void @vyx_arg2(ptr noundef byval(%struct.vyx_t2) align 8)
declare void @vyx_ret2(ptr sret(%struct.vyx_t2) align 8)
declare void @vyx_arg3(double, double)
declare { double, double } @vyx_ret3()
"""
        span = adapter._clang_ir_argument_classes(ir, 0)
        assert span is not None
        self.assertEqual(span["llvm_args"], ["ptr", "i64"])
        self.assertEqual(span["ret_llvm"], "{i64,i64}")
        self.assertFalse(span["memory"])
        rect = adapter._clang_ir_argument_classes(ir, 1)
        assert rect is not None
        self.assertEqual(rect["llvm_args"], ["i64", "i64"])
        self.assertEqual(rect["ret_llvm"], "i64")
        mem = adapter._clang_ir_argument_classes(ir, 2)
        assert mem is not None
        self.assertTrue(mem["memory"])
        self.assertTrue(mem["byval"])
        self.assertTrue(mem["ret_sret"])
        sse = adapter._clang_ir_argument_classes(ir, 3)
        assert sse is not None
        self.assertEqual(sse["llvm_args"], ["double", "double"])
        self.assertEqual(sse["ret_llvm"], "{double,double}")

    def test_gcc_asm_copies_observed_registers_not_sizeof(self) -> None:
        asm = """
vyx_call0:
        call    vyx_ret0
        movq    %rax, %rdi
        movq    %rdx, %rsi
        call    vyx_arg0
        ret
vyx_call1:
        call    vyx_ret1
        movsd   %xmm0, %xmm0
        movsd   %xmm1, %xmm1
        call    vyx_arg1
        ret
vyx_call2:
        leaq    16(%rsp), %rdi
        call    vyx_ret2
        leaq    16(%rsp), %rdi
        call    vyx_arg2
        ret
"""
        pair = adapter._gcc_asm_argument_classes(asm, 0)
        assert pair is not None
        self.assertEqual(pair["llvm_args"], ["i64", "i64"])
        self.assertFalse(pair["memory"])
        self.assertEqual(pair["ret_llvm"], "{i64,i64}")
        sse = adapter._gcc_asm_argument_classes(asm, 1)
        assert sse is not None
        self.assertEqual(sse["llvm_args"], ["double", "double"])
        mem = adapter._gcc_asm_argument_classes(asm, 2)
        assert mem is not None
        self.assertTrue(mem["memory"])
        self.assertTrue(mem["ret_sret"])
        self.assertEqual(mem["llvm_args"], [])

    def test_public_api_headers_drop_inl_impls(self) -> None:
        kept = adapter.public_api_headers(
            [
                "/lib/spdlog/logger.h",
                "/lib/spdlog/logger-inl.h",
                "/lib/spdlog/details/log_msg-inl.hpp",
                "native/spdlog_dci.hpp",
            ]
        )
        self.assertEqual(
            kept,
            ["/lib/spdlog/logger.h", "native/spdlog_dci.hpp"],
        )

    def test_gcc_asm_ignores_verbose_source_comments(self) -> None:
        # g++ -fverbose-asm copies `void vyx_call0() { vyx_arg0(vyx_ret0()); }`
        # into a `#` comment. The substring "call" must not end the body.
        asm = """
vyx_call0:
	endbr64
	pushq	%rbp
# probe.cpp:9: extern "C" void vyx_call0() { vyx_arg0(vyx_ret0()); }
	call	vyx_ret0@PLT
# probe.cpp:9: extern "C" void vyx_call0() { vyx_arg0(vyx_ret0()); }
	movq	%rax, %rsi
	movq	%rdx, %rdi
	movq	%rax, %rcx
	movq	%rdx, %rbx
	movq	%rsi, %rdx
	movq	%rbx, %rax
	movq	%rdx, %rdi
	movq	%rax, %rsi
	call	vyx_arg0@PLT
	ret
"""
        pair = adapter._gcc_asm_argument_classes(asm, 0)
        assert pair is not None
        self.assertEqual(pair["llvm_args"], ["i64", "i64"])
        self.assertFalse(pair["memory"])
        self.assertEqual(pair["ret_llvm"], "{i64,i64}")

    def test_msvc_asm_copies_observed_registers_not_sizeof(self) -> None:
        asm = """
vyx_call0 PROC
        call    vyx_ret0
        mov     rcx, rax
        call    vyx_arg0
        ret
vyx_call0 ENDP
vyx_call1 PROC
        lea     rcx, QWORD PTR [rsp+32]
        call    vyx_ret1
        lea     rcx, QWORD PTR [rsp+32]
        call    vyx_arg1
        ret
vyx_call1 ENDP
"""
        handle = adapter._msvc_asm_argument_classes(asm, 0)
        assert handle is not None
        self.assertEqual(handle["llvm_args"], ["i64"])
        self.assertFalse(handle["memory"])
        self.assertEqual(handle["ret_llvm"], "i64")
        big = adapter._msvc_asm_argument_classes(asm, 1)
        assert big is not None
        self.assertTrue(big["memory"])
        self.assertTrue(big["ret_sret"])
        self.assertEqual(big["llvm_args"], [])

    def test_itanium_compiler_tokens_are_copied_not_collapsed_to_u128(self) -> None:
        view = adapter.RecordLayout(
            "fmt::basic_string_view<char>",
            size=16,
            alignment=8,
            fields=[
                adapter.FieldLayout("data_", "const char *", 0),
                adapter.FieldLayout("size_", "unsigned long", 8),
            ],
        )
        info = adapter.dci_type("fmt::basic_string_view<char>")
        # Clang CodeGen for a {ptr,size} view: two INTEGER eightbytes, typed
        # ptr then i64 — not a synthetic u128.
        span_sig = {
            "trivially_copyable": True,
            "standard_layout": True,
            "trivially_destructible": True,
            "memory": False,
            "llvm_args": ["ptr", "i64"],
            "ret_llvm": "{ptr,i64}",
            "ret_sret": False,
        }
        param = adapter.abi_value_lowering(
            info, [view], "parameter", "x86_64-unknown-linux-gnu", {view.type_name: span_sig}
        )
        self.assertEqual(param["passing"], "split")
        self.assertEqual(param["registers"], ["ptr", "i64"])
        self.assertEqual(param["register_offsets"], [0, 8])
        self.assertNotEqual(param.get("coerce_to", {}).get("name"), "u128")
        returned = adapter.abi_value_lowering(
            info, [view], "return", "x86_64-unknown-linux-gnu", {view.type_name: span_sig}
        )
        self.assertEqual(returned["passing"], "coerce")
        self.assertEqual(returned["registers"], ["ptr", "i64"])
        self.assertNotEqual(returned.get("coerce_to", {}).get("name"), "u128")

        two_i64 = adapter.RecordLayout("Pair", size=16, alignment=8)
        pair_info = adapter.dci_type("Pair")
        pair_sig = {
            "trivially_copyable": True,
            "standard_layout": True,
            "trivially_destructible": True,
            "memory": False,
            "llvm_args": ["i64", "i64"],
            "ret_llvm": "{i64,i64}",
            "ret_sret": False,
        }
        pair_param = adapter.abi_value_lowering(
            pair_info,
            [two_i64],
            "parameter",
            "x86_64-unknown-linux-gnu",
            {two_i64.type_name: pair_sig},
        )
        self.assertEqual(pair_param["passing"], "split")
        self.assertEqual(pair_param["registers"], ["i64", "i64"])

        # SysV class nicknames without compiler LLVM types are not a lowering.
        with self.assertRaisesRegex(adapter.AdapterContractError, "verified machine-signature"):
            adapter.abi_value_lowering(
                info,
                [view],
                "parameter",
                "x86_64-unknown-linux-gnu",
                {view.type_name: {
                    "trivially_copyable": True,
                    "standard_layout": True,
                    "memory": False,
                    "classes": ["INTEGER", "INTEGER"],
                }},
            )

    def test_itanium_single_eightbyte_integer_aggregate_coerces(self) -> None:
        record = adapter.RecordLayout("Handle", size=8, alignment=8)
        signatures = {
            "Handle": {
                "trivially_copyable": True,
                "standard_layout": True,
                "trivially_destructible": True,
                "memory": False,
                "llvm_args": ["i64"],
                "ret_llvm": "i64",
                "ret_sret": False,
            }
        }
        lowering = adapter.abi_value_lowering(
            adapter.dci_type("Handle"),
            [record],
            "parameter",
            "x86_64-unknown-linux-gnu",
            signatures,
        )
        self.assertEqual(lowering["passing"], "coerce")
        self.assertEqual(lowering["coerce_to"]["name"], "i64")
        ret = adapter.abi_value_lowering(
            adapter.dci_type("Handle"),
            [record],
            "return",
            "x86_64-unknown-linux-gnu",
            signatures,
        )
        self.assertEqual(ret["passing"], "coerce")
        self.assertEqual(ret["coerce_to"]["name"], "i64")

    def test_itanium_memory_and_sse_use_compiler_types(self) -> None:
        record = adapter.RecordLayout("Pair", size=16, alignment=8)
        info = adapter.dci_type("Pair")
        memory = {
            "trivially_copyable": True,
            "standard_layout": True,
            "trivially_destructible": True,
            "memory": True,
            "byval": True,
            "llvm_args": [],
            "ret_sret": True,
            "ret_llvm": "void",
        }
        param = adapter.abi_value_lowering(
            info, [record], "parameter", "x86_64-unknown-linux-gnu", {"Pair": memory}
        )
        self.assertEqual(param["passing"], "byval")
        ret = adapter.abi_value_lowering(
            info, [record], "return", "x86_64-unknown-linux-gnu", {"Pair": memory}
        )
        self.assertEqual(ret["passing"], "sret")
        sse = {
            "trivially_copyable": True,
            "standard_layout": True,
            "trivially_destructible": True,
            "memory": False,
            "llvm_args": ["double", "double"],
            "ret_llvm": "{double,double}",
            "ret_sret": False,
        }
        sse_param = adapter.abi_value_lowering(
            info, [record], "parameter", "x86_64-unknown-linux-gnu", {"Pair": sse}
        )
        self.assertEqual(sse_param["passing"], "split")
        self.assertEqual(sse_param["registers"], ["double", "double"])
        nontrivial = {
            "trivially_copyable": False,
            "standard_layout": True,
            "memory": False,
            "llvm_args": ["i64"],
            "ret_llvm": "i64",
        }
        with self.assertRaisesRegex(adapter.AdapterContractError, "verified machine-signature"):
            adapter.abi_value_lowering(
                info, [record], "parameter", "x86_64-unknown-linux-gnu", {"Pair": nontrivial}
            )

    def test_live_clang_codegen_tokens_are_copied(self) -> None:
        import shutil

        clang = shutil.which("clang++") or shutil.which("clang++.exe")
        if clang is None:
            self.skipTest("clang++ is required to dump CodeGen LLVM IR")
        toolchain = adapter.cpp_toolchains.CppToolchain(
            family="clang",
            executable=clang,
            version="0",
            version_line="clang",
            native_target="x86_64-unknown-linux-gnu",
            driver_mode="clang",
        )
        with tempfile.TemporaryDirectory() as temp_dir:
            header = Path(temp_dir) / "agg.hpp"
            header.write_text(
                "struct Span { const char *data; unsigned long long size; };\n"
                "struct Rect { int x; int y; int w; int h; };\n",
                encoding="utf-8",
            )
            signatures = adapter.probe_itanium_aggregate_signatures(
                toolchain,
                "c++17",
                "x86_64-unknown-linux-gnu",
                [str(header).replace("\\", "/")],
                ["Span", "Rect"],
                ["-target", "x86_64-unknown-linux-gnu"],
            )
        self.assertEqual(signatures["Span"]["llvm_args"], ["ptr", "i64"])
        self.assertNotEqual(signatures["Span"].get("ret_llvm"), "u128")
        self.assertEqual(signatures["Rect"]["llvm_args"], ["i64", "i64"])

    def test_nested_field_type_gets_its_own_layout(self) -> None:
        dump = """
*** Dumping AST Record Layout
         0 | class Msg
         0 |   fmt::basic_string_view<char> payload
           | [sizeof=16, dsize=16, align=8,
           |  nvsize=16, nvalign=8]

*** Dumping AST Record Layout
         0 | class fmt::basic_string_view<char>
         0 |   const char * data_
         8 |   unsigned long size_
           | [sizeof=16, dsize=16, align=8,
           |  nvsize=16, nvalign=8]
"""
        layouts, closed_names = adapter.expand_layout_closure(dump, ["Msg"])
        names = {record.type_name for record in layouts}
        # The top-level record only names the field type; the closure must pull
        # the nested aggregate in so Vyx can read `.data_`/`.size_`.
        self.assertIn("Msg", names)
        nested = next(
            record
            for record in layouts
            if "basic_string_view" in record.type_name
        )
        self.assertEqual(nested.size, 16)
        self.assertEqual(nested.alignment, 8)
        self.assertEqual(
            [(field.name, field.offset) for field in nested.fields],
            [("data_", 0), ("size_", 8)],
        )
        self.assertTrue(
            any("basic_string_view" in name for name in closed_names)
        )

    def test_enum_type_resolves_to_primitive_integer(self) -> None:
        original = dict(adapter.CPP_ENUM_UNDERLYING)
        try:
            adapter.CPP_ENUM_UNDERLYING = {"spdlog::level::level_enum": "int32_t"}
            absolute = adapter.dci_type("spdlog::level::level_enum")
            self.assertEqual(absolute["kind"], "primitive")
            self.assertEqual(absolute["name"], "i32")
            self.assertEqual(absolute["enum_name"], "spdlog.level.level_enum")
            # A relative qualification written inside `namespace spdlog` must
            # resolve through the owner's enclosing scopes, not be treated as a
            # class with no layout.
            relative = adapter.dci_type("level::level_enum", "spdlog::logger")
            self.assertEqual(relative["kind"], "primitive")
            self.assertEqual(relative["name"], "i32")
            self.assertEqual(relative["enum_name"], "spdlog.level.level_enum")
        finally:
            adapter.CPP_ENUM_UNDERLYING = original

    def test_function_pointer_signature_is_normalized_and_copyable(self) -> None:
        callback = adapter.dci_type("int (__stdcall *)(const char *, unsigned long)")
        self.assertEqual(callback["kind"], "function")
        self.assertEqual(callback["reference"], "pointer")
        self.assertTrue(callback["nullable"])
        self.assertEqual(callback["calling_convention"], "stdcall")
        # C++ ``char`` is a distinct one-byte character type in the C++ DCI
        # profile.  It must not be rewritten to ``i8``: ``signed char`` is a
        # different C++ type and is the spelling that maps to ``i8``.  Keeping
        # that distinction is required for exact template identity and for
        # materializing the original C++ member/function pointer.
        self.assertEqual(
            [parameter["name"] for parameter in callback["signature"]["params"]],
            ["char", "u32"],
        )
        signed_callback = adapter.dci_type(
            "int (__stdcall *)(const signed char *, unsigned long)"
        )
        self.assertEqual(
            [parameter["name"] for parameter in signed_callback["signature"]["params"]],
            ["i8", "u32"],
        )
        self.assertEqual(callback["signature"]["return"]["name"], "i32")
        self.assertEqual(
            adapter.parameter_ownership(callback, [], "set_callback"), "copy"
        )

    def test_function_pointer_typedef_resolves_to_its_abi_shape(self) -> None:
        original = dict(adapter.CPP_TYPE_ALIAS_TARGETS)
        try:
            adapter.CPP_TYPE_ALIAS_TARGETS = {
                "Callback": "int (__stdcall *)(int)"
            }
            callback = adapter.dci_type("Callback")
        finally:
            adapter.CPP_TYPE_ALIAS_TARGETS = original
        self.assertEqual(callback["kind"], "function")
        self.assertEqual(callback["alias_name"], "Callback")
        self.assertEqual(callback["signature"]["params"][0]["name"], "i32")

    def test_member_pointer_and_array_are_not_misreported_as_classes(self) -> None:
        member_pointer = adapter.dci_type("int (Widget::*)(int) const")
        array = adapter.dci_type("unsigned short [4]")
        matrix = adapter.dci_type("int [2][3]")
        self.assertEqual(member_pointer["kind"], "opaque")
        self.assertIn("ABI", member_pointer["unsupported_reason"])
        self.assertEqual(array["kind"], "vector")
        self.assertEqual(array["length"], 4)
        self.assertEqual(array["element"]["name"], "u16")
        self.assertEqual(matrix["length"], 2)
        self.assertEqual(matrix["element"]["length"], 3)
        self.assertEqual(matrix["name"], "[[i32;3];2]")

    def test_field_readonly_tracks_top_level_const_not_pointee_const(self) -> None:
        const_value = adapter.field_json(adapter.FieldLayout("value", "const int", 0))
        const_pointee = adapter.field_json(
            adapter.FieldLayout("borrowed", "const int *", 8)
        )
        const_pointer = adapter.field_json(
            adapter.FieldLayout("fixed", "int * const", 16)
        )
        self.assertTrue(const_value["is_readonly"])
        self.assertFalse(const_pointee["is_readonly"])
        self.assertTrue(const_pointer["is_readonly"])

    def test_pointer_cv_and_depth_are_preserved(self) -> None:
        pointer = adapter.dci_type("const int * const")
        nested = adapter.dci_type("const int **")
        self.assertTrue(pointer["pointee_const"])
        self.assertTrue(pointer["pointer_const"])
        self.assertEqual(pointer["pointee"]["name"], "i32")
        self.assertFalse(nested["pointee_const"])
        self.assertEqual(nested["pointee"]["reference"], "pointer")
        self.assertTrue(nested["pointee"]["pointee_const"])


class MultiFrontendContractTests(unittest.TestCase):
    def test_gcc_contract_records_frontend_and_itanium_abi_separately(self) -> None:
        gcc = adapter.cpp_toolchains.CppToolchain(
            family="gcc",
            executable="/usr/bin/g++",
            version="14.2.0",
            version_line="g++ 14.2.0",
            native_target="x86_64-linux-gnu",
            driver_mode="gnu",
        )
        clang = adapter.cpp_toolchains.CppToolchain(
            family="clang",
            executable="/usr/bin/clang++",
            version="20.1.0",
            version_line="clang version 20.1.0",
            native_target="x86_64-unknown-linux-gnu",
            driver_mode="clang",
        )
        with tempfile.TemporaryDirectory() as temp_dir:
            output = Path(temp_dir) / "api.json"
            adapter.write_abi(
                output,
                "x86_64-unknown-linux-gnu",
                ["include/api.hpp"],
                [],
                [],
                [],
                [],
                toolchain=gcc,
                fact_extractor=clang,
            )
            document = json.loads(output.read_text(encoding="utf-8"))
        self.assertEqual(document["producer"]["tool"], "gcc")
        self.assertEqual(document["producer"]["backend"], "itanium")
        self.assertEqual(document["producer"]["fact_extractor"]["name"], "clang")
        self.assertEqual(document["target"]["data_model"], "lp64")
        self.assertIn("gcc-itanium", document["profile_extension"])
        self.assertEqual(document["source"]["compiler"]["name"], "gcc")

    @mock.patch.object(adapter.subprocess, "run")
    def test_msvc_frontend_validation_uses_native_driver_syntax(
        self, run_process: mock.Mock
    ) -> None:
        run_process.return_value = mock.Mock(returncode=0, stdout="")
        msvc = adapter.cpp_toolchains.CppToolchain(
            "msvc", "cl.exe", "19.44", "MSVC 19.44", "x86_64-pc-windows-msvc", "cl"
        )
        adapter.validate_translation_unit_with_selected_compiler(
            msvc, "c++20", Path("probe.cpp"), ["/DAPI_EXPORT"]
        )
        command = run_process.call_args.args[0]
        self.assertEqual(command[:4], ["cl.exe", "/nologo", "/Zs", "/std:c++20"])
        self.assertIn("/DAPI_EXPORT", command)

    @mock.patch.object(adapter.subprocess, "run")
    def test_msvc_environment_is_initialized_from_compiler_install(
        self, run_process: mock.Mock
    ) -> None:
        run_process.return_value = mock.Mock(
            returncode=0,
            stdout="INCLUDE=C:\\VC\\include\nPATH=C:\\VC\\bin\n",
        )
        with tempfile.TemporaryDirectory() as temp_dir:
            install = Path(temp_dir) / "Visual Studio"
            compiler = (
                install / "VC" / "Tools" / "MSVC" / "14.44" / "bin"
                / "Hostx64" / "x64" / "cl.exe"
            )
            compiler.parent.mkdir(parents=True)
            compiler.touch()
            vcvars = install / "VC" / "Auxiliary" / "Build" / "vcvarsall.bat"
            vcvars.parent.mkdir(parents=True)
            vcvars.touch()
            toolchain = adapter.cpp_toolchains.CppToolchain(
                "msvc", str(compiler), "19.44", "MSVC 19.44",
                "x86_64-pc-windows-msvc", "cl",
            )
            with mock.patch.dict(adapter.os.environ, {"INCLUDE": ""}, clear=False):
                environment = adapter.msvc_validation_environment(
                    toolchain, "x86_64-pc-windows-msvc"
                )
        self.assertEqual(environment["INCLUDE"], "C:\\VC\\include")
        command_line = run_process.call_args.args[0]
        self.assertIsInstance(command_line, str)
        self.assertIn("vcvarsall.bat", command_line)
        self.assertIn("amd64", command_line)

    def test_portable_extractor_flags_are_translated_for_native_frontends(self) -> None:
        msvc = adapter.cpp_toolchains.CppToolchain(
            "msvc", "cl.exe", "19.44", "MSVC 19.44", "x86_64-pc-windows-msvc", "cl"
        )
        gcc = adapter.cpp_toolchains.CppToolchain(
            "gcc", "g++", "14", "g++ 14", "x86_64-linux-gnu", "gnu"
        )
        flags = ["-I", "include", "-DAPI=1", "-U", "OLD", "-Xclang", "ignored"]
        self.assertEqual(
            adapter.frontend_validation_args(msvc, ["/permissive-"], flags),
            ["/permissive-", "/Iinclude", "/DAPI=1", "/UOLD"],
        )
        self.assertEqual(
            adapter.frontend_validation_args(gcc, [], flags),
            ["-Iinclude", "-DAPI=1", "-UOLD"],
        )

    def test_selected_compiler_self_measures_layout_and_overrides_extracted(self) -> None:
        # The selected compiler (gcc) is the sole ABI authority. When Clang's
        # extracted size/alignment disagrees with gcc's, gcc's numbers win and
        # the record is NOT rejected -- this is a single-compiler self-check,
        # never a Clang-vs-gcc consensus.
        gcc = adapter.cpp_toolchains.CppToolchain(
            "gcc", "g++", "14", "g++ 14", "x86_64-linux-gnu", "gnu"
        )
        records = [
            adapter.RecordLayout("api::Value", size=999, alignment=1),
            adapter.RecordLayout("api::Missing", size=8, alignment=8),
        ]
        captured: dict[str, str] = {}

        def fake_probe(
            _toolchain: object,
            _std: str,
            probe: Path,
            _args: list[str],
            _target: str,
        ) -> str:
            captured["source"] = probe.read_text(encoding="utf-8")
            # gcc reports 24/8 for Value; Missing yields no measurement.
            return (
                "error: aggregate '__vyx_dci_size<0, 24>' has incomplete type\n"
                "error: aggregate '__vyx_dci_align<0, 8>' has incomplete type\n"
            )

        with mock.patch.object(
            adapter, "_run_self_layout_probe", side_effect=fake_probe
        ):
            unverified = adapter.measure_record_layout_with_selected_compiler(
                gcc,
                "c++20",
                "x86_64-unknown-linux-gnu",
                ["api.hpp"],
                records,
                ["-DAPI=1"],
            )
        self.assertIn("sizeof(api::Value)", captured["source"])
        self.assertIn("alignof(api::Value)", captured["source"])
        # gcc's numbers override Clang's extracted 999/1 without rejecting.
        self.assertEqual(records[0].size, 24)
        self.assertEqual(records[0].alignment, 8)
        # A layout the selected compiler cannot prove is fail-closed.
        self.assertEqual(unverified, {"api::Missing"})

    def test_clang_selection_skips_self_measurement(self) -> None:
        clang = adapter.cpp_toolchains.CppToolchain(
            "clang",
            "clang++",
            "20.1.0",
            "clang version 20.1.0",
            "x86_64-unknown-linux-gnu",
            "clang",
        )
        records = [adapter.RecordLayout("api::Value", size=999, alignment=1)]
        with mock.patch.object(adapter, "_run_self_layout_probe") as probe:
            unverified = adapter.measure_record_layout_with_selected_compiler(
                clang,
                "c++20",
                "x86_64-unknown-linux-gnu",
                ["api.hpp"],
                records,
                [],
            )
        probe.assert_not_called()
        self.assertEqual(unverified, set())
        self.assertEqual(records[0].size, 999)

    def test_gcc_self_measurement_uses_the_real_selected_compiler(self) -> None:
        import shutil

        if shutil.which("g++") is None:
            self.skipTest("g++ is required for the real self-measurement path")
        gcc = adapter.cpp_toolchains.CppToolchain(
            "gcc", "g++", "0", "g++", "x86_64-linux-gnu", "gnu"
        )
        with tempfile.TemporaryDirectory() as temp_dir:
            header = Path(temp_dir) / "api.hpp"
            header.write_text(
                "#pragma once\nnamespace api {\nstruct Value { long a; char b; };\n}\n",
                encoding="utf-8",
            )
            record = adapter.RecordLayout("api::Value", size=1, alignment=1)
            unverified = adapter.measure_record_layout_with_selected_compiler(
                gcc,
                "c++17",
                "x86_64-unknown-linux-gnu",
                [str(header)],
                [record],
                [],
            )
        self.assertEqual(unverified, set())
        # long(8)+char(1) padded to 16 with 8-byte alignment under the Itanium ABI.
        self.assertEqual(record.size, 16)
        self.assertEqual(record.alignment, 8)

    def test_vtable_probe_uses_only_unambiguous_virtual_members(self) -> None:
        symbols = [
            adapter.Symbol(
                name="Poly::value",
                owner="Poly",
                member_name="value",
                mangled="_ZN4Poly5valueEv",
                kind="method",
                calling_convention="cxx_method",
                params=[],
                ret={"type": adapter.dci_type("int")},
                is_virtual=True,
            )
        ]
        with tempfile.TemporaryDirectory() as temp_dir:
            probe = Path(temp_dir) / "probe.cpp"
            probe.write_text("struct Poly { virtual int value(); };\n", encoding="utf-8")
            adapter.append_vtable_force_uses(probe, symbols)
            text = probe.read_text(encoding="utf-8")
        self.assertIn("&Poly::value", text)


if __name__ == "__main__":
    unittest.main()
