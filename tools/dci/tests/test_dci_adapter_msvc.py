import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


MODULE_PATH = Path(__file__).parents[1] / "dci_adapter_msvc.py"
SPEC = importlib.util.spec_from_file_location("dci_adapter_msvc", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
adapter = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = adapter
SPEC.loader.exec_module(adapter)


class DciAdapterMsvcTests(unittest.TestCase):

    def test_discovers_project_header_closure_without_exporting_sdk_headers(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            include = root / "include"
            nested = include / "nested"
            nested.mkdir(parents=True)
            umbrella = include / "Api.hpp"
            model = nested / "Model.h"
            outside = root / "external.h"
            umbrella.write_text('#include "nested/Model.h"\n#include <vector>\n', encoding="utf-8")
            model.write_text('#include "../external.h"\n', encoding="utf-8")
            outside.write_text('struct NotPublic {};\n', encoding="utf-8")

            headers = adapter.discover_project_headers([str(umbrella)], [str(include)])
            self.assertEqual(headers, [str(umbrella.resolve()), str(model.resolve())])

    def test_discovers_all_public_header_roots(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            include = Path(temp_dir) / "include"
            nested = include / "nested"
            nested.mkdir(parents=True)
            (include / "Api.hpp").write_text("", encoding="utf-8")
            (nested / "Model.h").write_text("", encoding="utf-8")
            (include / "Ignored.cpp").write_text("", encoding="utf-8")

            headers = adapter.discover_public_header_roots([str(include)])

            self.assertEqual(
                headers,
                sorted(
                    [
                        str((include / "Api.hpp").resolve()),
                        str((nested / "Model.h").resolve()),
                    ]
                ),
            )

    def test_discovers_split_namespace_declarations(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            header = Path(temp_dir) / "Api.hpp"
            header.write_text(
                "namespace Api\n{\nstruct Record {};\nenum class Kind { A };\n}\n",
                encoding="utf-8",
            )

            self.assertEqual(adapter.discover_record_names([str(header)]), ["Api::Record"])
            self.assertIn("Api::Kind", adapter.discover_enum_underlying([str(header)]))

    def test_discovers_qt_style_split_class_brace(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            header = Path(temp_dir) / "qpoint.h"
            header.write_text(
                "class QPointF;\n"
                "class QPoint\n"
                "{\n"
                "    int xp;\n"
                "    int yp;\n"
                "    int manhattanLength() const;\n"
                "};\n"
                "class Q_CORE_EXPORT QString\n"
                "{\n"
                "    int size() const;\n"
                "};\n"
                "class Q_CORE_EXPORT QFile : public QFileDevice\n"
                "{\n"
                "    bool exists() const;\n"
                "};\n"
                "int qVersion();\n",
                encoding="utf-8",
            )
            records = adapter.discover_record_names([str(header)])
            self.assertIn("QPoint", records)
            self.assertIn("QString", records)
            self.assertIn("QFile", records)
            self.assertNotIn("QPointF", records)
            free_names = adapter.discover_free_function_names([str(header)])
            self.assertIn("qVersion", free_names)
            self.assertNotIn("manhattanLength", free_names)
            self.assertNotIn("size", free_names)
            self.assertNotIn("exists", free_names)

    def test_discovers_over_aligned_records_declared_with_alignas(self):
        # `struct alignas(64) OA_EXPORT Over64 { ... };` is the standard spelling
        # for an over-aligned foreign object.  The alignment specifier sits
        # between the class-key and the name, so a discovery regex that only
        # tolerates ALL-CAPS export macros (or `final`) never sees the record:
        # its layout is then never measured and every by-value symbol that
        # mentions it is rejected as "no verified layout/lifecycle" -- a fact
        # the producer had already produced.
        with tempfile.TemporaryDirectory() as temp_dir:
            header = Path(temp_dir) / "Overalign.hpp"
            header.write_text(
                "#define OA_EXPORT __declspec(dllexport)\n"
                "struct alignas(16) OA_EXPORT Over16 {\n"
                "    unsigned long long x[2];\n"
                "};\n"
                "struct alignas(64) OA_EXPORT Over64 {\n"
                "    unsigned long long x[8];\n"
                "};\n"
                "struct alignas(64) Over64Bare {\n"
                "    unsigned long long x[8];\n"
                "};\n"
                "class alignas(32) Over32Class {\n"
                "    unsigned long long x[4];\n"
                "};\n"
                "struct __declspec(align(64)) DeclspecAligned {\n"
                "    unsigned long long x[8];\n"
                "};\n"
                "struct OA_EXPORT Plain {\n"
                "    int x;\n"
                "};\n",
                encoding="utf-8",
            )
            records = adapter.discover_record_names([str(header)])
            self.assertIn("Over16", records)
            self.assertIn("Over64", records)
            self.assertIn("Over64Bare", records)
            self.assertIn("Over32Class", records)
            self.assertIn("DeclspecAligned", records)
            # The ALL-CAPS export macro must keep working, and the macro itself
            # must never be mistaken for the record name.
            self.assertIn("Plain", records)
            self.assertNotIn("OA_EXPORT", records)

    def test_file_scope_template_does_not_swallow_following_qt_class(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            header = Path(temp_dir) / "qstring.h"
            header.write_text(
                "template<typename T>\n"
                "class QList\n"
                "{\n"
                "};\n"
                "class QPoint\n"
                "{\n"
                "    template<std::size_t I> friend int get();\n"
                "};\n"
                "class QPointF\n"
                "{\n"
                "};\n"
                "template<typename T>\n"
                "void qDeleteAll(const T &);\n"
                "class Q_CORE_EXPORT QString\n"
                "{\n"
                "    int size() const;\n"
                "};\n",
                encoding="utf-8",
            )
            records = adapter.discover_record_names([str(header)])
            self.assertNotIn("QList", records)
            self.assertIn("QPoint", records)
            self.assertIn("QPointF", records)
            self.assertIn("QString", records)

    def test_discovers_class_with_preprocessor_between_name_and_brace(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            header = Path(temp_dir) / "qcoreapplication.h"
            header.write_text(
                "class Q_CORE_EXPORT QCoreApplication\n"
                "#ifndef QT_NO_QOBJECT\n"
                "    : public QObject\n"
                "#endif\n"
                "{\n"
                "    void exec();\n"
                "};\n"
                "int qApp_helper();\n",
                encoding="utf-8",
            )
            records = adapter.discover_record_names([str(header)])
            self.assertIn("QCoreApplication", records)
            free_names = adapter.discover_free_function_names([str(header)])
            self.assertIn("qApp_helper", free_names)
            self.assertNotIn("exec", free_names)

    def test_nested_class_is_not_a_top_level_record(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            header = Path(temp_dir) / "qvariant.h"
            header.write_text(
                "class Q_CORE_EXPORT QVariant\n"
                "{\n"
                "    struct PrivateShared {};\n"
                "    class Private {};\n"
                "};\n",
                encoding="utf-8",
            )
            records = adapter.discover_record_names([str(header)])
            self.assertEqual(records, ["QVariant"])

    def test_skips_docs_macros_and_template_endif_before_class(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            header = Path(temp_dir) / "qt.h"
            header.write_text(
                "/*\n"
                "        class QClass : public QObject\n"
                "        {\n"
                "        };\n"
                "*/\n"
                "#define Q_OBJECT \\\n"
                "    struct QPrivateSignal { explicit QPrivateSignal() = default; };\n"
                "template <bool UseChar8T>\n"
                "#endif\n"
                "class QBasicUtf8StringView\n"
                "{\n"
                "};\n"
                "class QString\n"
                "{\n"
                "};\n",
                encoding="utf-8",
            )
            records = adapter.discover_record_names([str(header)])
            self.assertNotIn("QClass", records)
            self.assertNotIn("QPrivateSignal", records)
            self.assertNotIn("QBasicUtf8StringView", records)
            self.assertIn("QString", records)

    def test_api_macro_class_scope_does_not_leak_virtual_as_free_function(self):
        # A third-party header that guards its class with an ALL-CAPS export
        # macro (spdlog's SPDLOG_API, fmt's FMT_API, ...) must still be seen as
        # a class scope, otherwise its virtual members leak out as free
        # functions and resolve against unrelated globals (e.g. ``std::log``).
        with tempfile.TemporaryDirectory() as temp_dir:
            header = Path(temp_dir) / "Sink.hpp"
            header.write_text(
                "namespace spdlog {\n"
                "namespace sinks {\n"
                "class SPDLOG_API sink {\n"
                "public:\n"
                "    virtual void log(const details::log_msg &msg) = 0;\n"
                "    virtual void flush() = 0;\n"
                "};\n"
                "}\n"
                "}\n"
                "void real_free_function(int x);\n",
                encoding="utf-8",
            )

            free_names = adapter.discover_free_function_names([str(header)])
            self.assertNotIn("log", free_names)
            self.assertNotIn("spdlog::sinks::log", free_names)
            self.assertNotIn("flush", free_names)
            self.assertIn("real_free_function", free_names)
            self.assertIn(
                "spdlog::sinks::sink", adapter.discover_record_names([str(header)])
            )

    def test_vtable_force_uses_typeid_not_delete(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            probe = Path(temp_dir) / "probe.cpp"
            probe.write_text("#include <cstddef>\n", encoding="utf-8")
            dtor = adapter.Symbol(
                name="~QIODeviceBase",
                owner="QIODeviceBase",
                member_name="~QIODeviceBase",
                mangled="??1QIODeviceBase@@UEAA@XZ",
                kind="destructor",
                calling_convention="thiscall",
                params=[],
                ret={"type": {"name": "void", "kind": "primitive"}},
                is_virtual=True,
                visibility="protected",
            )
            adapter.append_vtable_force_uses(probe, [dtor])
            text = probe.read_text(encoding="utf-8")
            self.assertIn("typeid(*value)", text)
            self.assertNotIn("delete value", text)

    def test_adapter_job_count_prefers_cli_then_env_then_cpu_cap(self):
        previous_jobs = adapter.ADAPTER_JOBS
        previous_env = os.environ.get("DCI_ADAPTER_JOBS")
        try:
            os.environ["DCI_ADAPTER_JOBS"] = "3"
            adapter.ADAPTER_JOBS = 12
            self.assertEqual(adapter.adapter_job_count(), 12)
            adapter.ADAPTER_JOBS = None
            self.assertEqual(adapter.adapter_job_count(), 3)
            del os.environ["DCI_ADAPTER_JOBS"]
            with mock.patch.object(adapter.os, "cpu_count", return_value=64):
                self.assertEqual(adapter.adapter_job_count(), 32)
            with mock.patch.object(adapter.os, "cpu_count", return_value=None):
                self.assertEqual(adapter.adapter_job_count(), 4)
        finally:
            adapter.ADAPTER_JOBS = previous_jobs
            if previous_env is None:
                os.environ.pop("DCI_ADAPTER_JOBS", None)
            else:
                os.environ["DCI_ADAPTER_JOBS"] = previous_env

    def test_clang_ast_dump_filter_texts_keeps_filter_order_under_workers(self):
        import threading
        import time

        started = threading.Barrier(3)

        def fake_run(cmd):
            name = next(
                arg.split("=", 1)[1]
                for arg in cmd
                if str(arg).startswith("-ast-dump-filter=")
            )
            started.wait(timeout=2)
            if name == "B":
                time.sleep(0.05)
            return f"dump:{name}"

        previous_jobs = adapter.ADAPTER_JOBS
        try:
            adapter.ADAPTER_JOBS = 8
            with mock.patch.object(adapter, "run", side_effect=fake_run):
                texts = list(
                    adapter.clang_ast_dump_filter_texts(
                        "clang++",
                        "c++17",
                        "x86_64-pc-windows-msvc",
                        Path("dci_probe.cpp"),
                        [],
                        ["A", "B", "C"],
                    )
                )
        finally:
            adapter.ADAPTER_JOBS = previous_jobs
            self.assertEqual(texts, ["dump:A", "dump:B", "dump:C"])

    def test_destructor_probe_owners_skips_inaccessible(self):
        public = adapter.Symbol(
            name="~PublicDtor",
            owner="PublicDtor",
            member_name="~PublicDtor",
            mangled="??1PublicDtor@@QEAA@XZ",
            kind="destructor",
            calling_convention="thiscall",
            params=[],
            ret=None,
            visibility="public",
        )
        protected = adapter.Symbol(
            name="~QIODeviceBase",
            owner="QIODeviceBase",
            member_name="~QIODeviceBase",
            mangled="??1QIODeviceBase@@UEAA@XZ",
            kind="destructor",
            calling_convention="thiscall",
            params=[],
            ret=None,
            visibility="protected",
        )
        private = adapter.Symbol(
            name="~QStandardPaths",
            owner="QStandardPaths",
            member_name="~QStandardPaths",
            mangled="??1QStandardPaths@@AEAA@XZ",
            kind="destructor",
            calling_convention="thiscall",
            params=[],
            ret=None,
            visibility="private",
        )
        self.assertEqual(
            adapter.destructor_probe_owners([protected, public, private]),
            ["PublicDtor"],
        )

    def test_direct_destructor_probe_drops_inaccessible_owners(self):
        clang = _clangxx_path()
        if clang is None:
            self.skipTest("clang++ required")
        with tempfile.TemporaryDirectory() as temp_dir:
            header = Path(temp_dir) / "api.hpp"
            header.write_text(
                "class PublicDtor { public: ~PublicDtor() {} int x; };\n"
                "class ProtectedDtor { protected: ~ProtectedDtor() {} int y; };\n"
                "class PrivateDtor { private: ~PrivateDtor() {} int z; };\n",
                encoding="utf-8",
            )
            found = adapter.direct_destructor_symbols(
                clang,
                "c++17",
                "x86_64-pc-windows-msvc",
                [str(header)],
                [],
                ["PublicDtor", "ProtectedDtor", "PrivateDtor"],
            )
        self.assertIn("PublicDtor", found)
        self.assertTrue(found["PublicDtor"])
        self.assertNotIn("ProtectedDtor", found)
        self.assertNotIn("PrivateDtor", found)

    def test_api_macro_class_scope_does_not_leak_member_alias(self):
        # ``using`` members inside an export-macro-guarded class must not be
        # mistaken for top-level type aliases.
        with tempfile.TemporaryDirectory() as temp_dir:
            header = Path(temp_dir) / "Api.hpp"
            header.write_text(
                "namespace demo {\n"
                "class DEMO_API Widget {\n"
                "public:\n"
                "    using self_t = Widget;\n"
                "};\n"
                "using widget_ref = Widget;\n"
                "}\n",
                encoding="utf-8",
            )

            aliases = {alias.name for alias in adapter.discover_type_aliases([str(header)])}
            self.assertIn("demo::widget_ref", aliases)
            self.assertNotIn("demo::self_t", aliases)

    def test_maps_cstdint_and_scoped_enum_to_primitive_abi_types(self):
        previous = adapter.CPP_ENUM_UNDERLYING
        try:
            adapter.CPP_ENUM_UNDERLYING = {"Api::Kind": "uint32_t"}
            self.assertEqual(adapter.dci_type("uint64_t")["name"], "u64")
            self.assertEqual(adapter.dci_type("Kind", "Api::Record")["name"], "u32")
            self.assertEqual(
                adapter.dci_type("Kind", "Api::Record")["enum_name"], "Api.Kind"
            )
        finally:
            adapter.CPP_ENUM_UNDERLYING = previous

    def test_dci_type_alias_cycle_does_not_recurse(self):
        previous = adapter.CPP_TYPE_ALIAS_TARGETS
        try:
            adapter.CPP_TYPE_ALIAS_TARGETS = {"Foo": "Bar", "Bar": "Foo"}
            info = adapter.dci_type("Foo")
            self.assertEqual(info["kind"], "class")
            self.assertEqual(info["name"], "Foo")
        finally:
            adapter.CPP_TYPE_ALIAS_TARGETS = previous

    @staticmethod
    def make_symbol(
        name,
        params=None,
        ret="void",
        *,
        mangled=None,
        is_const=False,
    ):
        return adapter.Symbol(
            name=name,
            owner="",
            member_name=name.rsplit("::", 1)[-1],
            mangled=mangled or f"?{name.replace('::', '@')}@@YAXXZ",
            kind="function",
            calling_convention="cxx_free_function",
            params=[
                {
                    "name": parameter_name,
                    "type": adapter.dci_type(cpp_type),
                    "location": "abi",
                }
                for parameter_name, cpp_type in (params or [])
            ],
            ret=(
                None
                if ret is None
                else {"type": adapter.dci_type(ret), "location": "abi"}
            ),
            is_const=is_const,
            unwind="no_unwind",
        )

    @staticmethod
    def copyable_record(name="Value"):
        return adapter.RecordLayout(
            name,
            size=16,
            alignment=8,
            traits={"pod": True, "trivially_copyable": True},
            lifecycle={
                "copy_construct": {
                    "available": True,
                    "accessible": True,
                    "trivial": True,
                },
                "move_construct": {
                    "available": True,
                    "accessible": True,
                    "trivial": True,
                },
                "destroy": {
                    "available": True,
                    "accessible": True,
                    "trivial": True,
                },
            },
        )

    def test_discovers_structured_ownership_annotations(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            header = Path(temp_dir) / "Api.hpp"
            header.write_text(
                """
/* dci-ownership
{
  "api::transfer(const Item*,Item*)": {
    "parameters": {"0": "borrow", "1": "move"},
    "return": "owned"
  }
}
dci-ownership-end */
""",
                encoding="utf-8",
            )
            rules = adapter.discover_ownership_annotations([str(header)])
            self.assertEqual(
                rules["api::transfer(const Item*,Item*)"],
                {
                    "parameters": {"0": "borrow", "1": "move"},
                    "return": "owned",
                },
            )

            malformed = Path(temp_dir) / "Malformed.hpp"
            malformed.write_text(
                "/* dci-ownership {\"api::bad()\": } dci-ownership-end */",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                adapter.AdapterContractError, "invalid dci-ownership JSON"
            ):
                adapter.discover_ownership_annotations([str(malformed)])

    def test_ownership_selector_must_resolve_exactly_once(self):
        symbol = self.make_symbol(
            "api::inspect", [("value", "const Item &")], mangled="?inspect@@1"
        )
        selector = "api::inspect(const Item&)"
        self.assertEqual(adapter.symbol_selector(symbol), selector)

        adapter.apply_ownership_annotations(
            [symbol], {selector: {"parameters": {"0": "borrow"}}}
        )
        self.assertEqual(symbol.parameter_ownerships, {0: "borrow"})

        duplicate = self.make_symbol(
            "api::inspect", [("value", "const Item &")], mangled="?inspect@@2"
        )
        with self.assertRaisesRegex(
            adapter.AdapterContractError, "resolved to 2 symbols"
        ):
            adapter.apply_ownership_annotations(
                [symbol, duplicate],
                {selector: {"parameters": {"0": "borrow"}}},
            )
        with self.assertRaisesRegex(
            adapter.AdapterContractError, "resolved to 0 symbols"
        ):
            adapter.apply_ownership_annotations(
                [symbol], {"api::missing()": {"parameters": {}}}
            )

    def test_ownership_annotations_bind_to_exported_alias_entities(self):
        source = adapter.Symbol(
            name="api::Template<int>::chain",
            owner="api::Template<int>",
            member_name="chain",
            mangled="?chain@?$Template@H@api@@QEAAAEAV12@H@Z",
            kind="method",
            calling_convention="cxx_method",
            params=[
                {
                    "name": "value",
                    "type": adapter.dci_type("int"),
                    "location": "abi",
                }
            ],
            ret={
                "type": adapter.dci_type("Template<int> &"),
                "location": "abi",
            },
            unwind="no_unwind",
        )
        aliases = [
            adapter.TypeAlias("api::Value", "api::Template<int>"),
            adapter.TypeAlias("api::ValueAlias", "api::Template<int>"),
        ]
        symbols = adapter.copy_symbols_for_aliases([source], aliases)
        selectors = {adapter.symbol_selector(symbol) for symbol in symbols}
        self.assertEqual(
            selectors,
            {"api::Value::chain(int)", "api::ValueAlias::chain(int)"},
        )

        adapter.apply_ownership_annotations(
            symbols,
            {
                selector: {"return": "borrow"}
                for selector in selectors
            },
        )
        self.assertEqual(
            [adapter.symbol_json(symbol)["return"]["ownership"] for symbol in symbols],
            ["borrow", "borrow"],
        )

    def test_explicit_pointer_ownership_is_emitted(self):
        symbol = self.make_symbol(
            "api::transfer",
            [
                ("source", "const Item *"),
                ("target", "Item *"),
                ("consumed", "Item *"),
            ],
            ret="Item *",
        )
        selector = adapter.symbol_selector(symbol)
        adapter.apply_ownership_annotations(
            [symbol],
            {
                selector: {
                    "parameters": {
                        "0": "borrow",
                        "1": "borrow_mut",
                        "2": "move",
                    },
                    "return": "owned",
                }
            },
        )

        emitted = adapter.symbol_json(symbol)
        self.assertEqual(
            [parameter["ownership"] for parameter in emitted["params"]],
            ["borrow", "borrow_mut", "move"],
        )
        self.assertEqual(emitted["return"]["ownership"], "owned")

    def test_ownership_is_inferred_for_primitive_value_and_references(self):
        symbol = self.make_symbol(
            "api::combine",
            [
                ("count", "int"),
                ("value", "Value"),
                ("read_only", "const Value &"),
                ("mutable_value", "Value &"),
                ("moved_value", "Value &&"),
            ],
            ret="int",
        )

        emitted = adapter.symbol_json(symbol, [self.copyable_record()])
        self.assertEqual(
            [parameter["ownership"] for parameter in emitted["params"]],
            ["copy", "copy", "borrow", "borrow_mut", "move"],
        )
        self.assertEqual(emitted["return"]["ownership"], "copy")

    def test_raw_pointer_without_annotation_is_rejected(self):
        symbol = self.make_symbol("api::inspect", [("value", "const Item *")])
        with tempfile.TemporaryDirectory() as temp_dir:
            output = Path(temp_dir) / "contract.dci"
            adapter.write_abi(
                output,
                "x86_64-pc-windows-msvc",
                [],
                [],
                [symbol],
                [],
                [],
            )
            document = json.loads(output.read_text(encoding="utf-8"))

        self.assertEqual(document["exports"]["symbols"], [])
        rejected = document["exports"]["rejected_symbols"]
        self.assertEqual(rejected[0]["selector"], adapter.symbol_selector(symbol))
        self.assertIn("requires explicit ownership annotation", rejected[0]["reason"])

    def test_void_return_is_serialized_as_null(self):
        symbol = self.make_symbol("api::notify", [("code", "int")], ret="void")
        emitted = adapter.symbol_json(symbol)
        self.assertIsNone(emitted["return"])
        self.assertEqual(emitted["abi"]["return"]["type"]["name"], "void")

        with tempfile.TemporaryDirectory() as temp_dir:
            output = Path(temp_dir) / "contract.dci"
            adapter.write_abi(
                output,
                "x86_64-pc-windows-msvc",
                [],
                [],
                [symbol],
                [],
                [],
            )
            document = json.loads(output.read_text(encoding="utf-8"))
        self.assertIsNone(document["exports"]["symbols"][0]["return"])

    def test_constructor_machine_return_follows_target_abi(self):
        symbol = adapter.Symbol(
            name="api::Box::constructor",
            owner="api::Box",
            member_name="constructor",
            mangled="??0Box@api@@QEAA@XZ",
            kind="constructor",
            calling_convention="cxx_constructor",
            params=[],
            ret=None,
            unwind="no_unwind",
        )

        windows = adapter.symbol_json(symbol, target="x86_64-pc-windows-msvc")
        self.assertIsNone(windows["return"])
        self.assertEqual(windows["abi"]["calling_convention"], "win64")
        self.assertEqual(
            windows["abi"]["return"],
            {
                "passing": "direct",
                "type": {
                    "name": "api::Box",
                    "kind": "class",
                    "reference": "pointer",
                },
            },
        )
        self.assertEqual(windows["abi"]["receiver"]["passing"], "direct")
        self.assertEqual(
            windows["abi"]["receiver"]["type"],
            windows["abi"]["return"]["type"],
        )

        linux = adapter.symbol_json(symbol, target="x86_64-pc-linux-gnu")
        self.assertIsNone(linux["return"])
        self.assertEqual(linux["abi"]["calling_convention"], "cdecl")
        self.assertEqual(
            linux["abi"]["return"],
            {
                "passing": "direct",
                "type": {"name": "void", "kind": "primitive"},
            },
        )

    def test_unsupported_symbol_is_preserved_with_rejection_reason(self):
        valid = self.make_symbol("api::valid", [("value", "int")], mangled="?valid@@")
        unsupported = self.make_symbol(
            "api::unsupported", [("value", "Opaque")], mangled="?unsupported@@"
        )
        with tempfile.TemporaryDirectory() as temp_dir:
            output = Path(temp_dir) / "contract.dci"
            adapter.write_abi(
                output,
                "x86_64-pc-windows-msvc",
                [],
                [],
                [valid, unsupported],
                [],
                [],
            )
            document = json.loads(output.read_text(encoding="utf-8"))

        self.assertEqual(
            [symbol["name"] for symbol in document["exports"]["symbols"]],
            ["api::valid"],
        )
        rejected = document["exports"]["rejected_symbols"]
        self.assertEqual(rejected[0]["name"], "api::unsupported")
        self.assertIn("no verified layout/lifecycle", rejected[0]["reason"])

    def test_record_layout_captures_bitfields_and_virtual_bases(self):
        dump = """
*** Dumping AST Record Layout
         0 | struct Sample
         0 |   (Sample vbtable pointer)
     8:0-4 |   unsigned int bits
    8:5-11 |   int signed_bits
        16 |   struct VirtualBase (virtual base)
        16 |     int value
           | [sizeof=24, align=8,
           |  nvsize=16, nvalign=8]
"""
        records = adapter.parse_record_layouts(dump, {"Sample"})
        self.assertEqual(len(records), 1)
        record = records[0]
        self.assertEqual(record.vbptr_offsets, [0])
        self.assertEqual(record.fields[0].bit_offset, 0)
        self.assertEqual(record.fields[0].bit_width, 5)
        self.assertEqual(record.fields[0].storage_size, 4)
        self.assertFalse(record.fields[0].is_signed)
        self.assertEqual(record.fields[1].bit_offset, 5)
        self.assertTrue(record.fields[1].is_signed)
        self.assertTrue(record.bases[0].is_virtual)
        self.assertEqual(record.bases[0].adjustment["kind"], "table")

    def test_virtual_base_ir_becomes_language_neutral_adjustment(self):
        body = """
  %6 = getelementptr inbounds i8, ptr %3, i64 8
  %7 = load ptr, ptr %6, align 8
  %8 = getelementptr inbounds i32, ptr %7, i32 2
  %9 = load i32, ptr %8, align 4
"""
        adjustment = adapter.virtual_base_adjustment_from_ir(body, 40)
        self.assertIsNotNone(adjustment)
        self.assertEqual(adjustment["table_pointer_offset"], 8)
        self.assertEqual(adjustment["table_entry_offset"], 8)
        self.assertEqual(adjustment["displacement_base_offset"], 8)
        self.assertEqual(adjustment["fallback_complete_object_offset"], 40)

    def test_virtual_base_ir_with_verified_static_offset_becomes_constant(self):
        body = """
  %6 = getelementptr inbounds i8, ptr %3, i64 56
  br label %7
  %8 = phi ptr [ %6, %5 ], [ null, %1 ]
"""
        adjustment = adapter.virtual_base_adjustment_from_ir(body, 56)
        self.assertEqual(
            adjustment,
            {
                "kind": "constant",
                "offset": 56,
                "null_preserving": True,
            },
        )

    def test_virtual_base_ir_does_not_infer_constant_from_fallback(self):
        body = """
  %6 = getelementptr inbounds i8, ptr %3, i64 8
  %7 = load ptr, ptr %6, align 8
"""
        self.assertIsNone(adapter.virtual_base_adjustment_from_ir(body, 8))

    def test_ast_contract_captures_traits_and_lifecycle(self):
        record = {
            "kind": "CXXRecordDecl",
            "name": "Ops",
            "tagUsed": "struct",
            "completeDefinition": True,
            "definitionData": {
                "copyCtor": {"nonTrivial": True, "userDeclared": True},
                "moveCtor": {"exists": True, "nonTrivial": True, "userDeclared": True},
                "copyAssign": {"trivial": True, "needsImplicit": True},
                "moveAssign": {},
                "defaultCtor": {"exists": True, "nonTrivial": True, "userProvided": True},
                "dtor": {"nonTrivial": True, "userDeclared": True},
                "isPolymorphic": True,
                "isStandardLayout": False,
            },
            "bases": [
                {
                    "access": "public",
                    "isVirtual": True,
                    "type": {"qualType": "Root"},
                }
            ],
            "inner": [
                {
                    "kind": "CXXConstructorDecl",
                    "name": "Ops",
                    "mangledName": "ctor",
                    "type": {"qualType": "void ()"},
                },
                {
                    "kind": "CXXConstructorDecl",
                    "name": "Ops",
                    "mangledName": "copy",
                    "type": {"qualType": "void (const Ops &)"},
                    "inner": [{"kind": "ParmVarDecl", "type": {"qualType": "const Ops &"}}],
                },
                {
                    "kind": "CXXConstructorDecl",
                    "name": "Ops",
                    "mangledName": "move",
                    "explicitlyDeleted": True,
                    "type": {"qualType": "void (Ops &&) noexcept"},
                    "inner": [{"kind": "ParmVarDecl", "type": {"qualType": "Ops &&"}}],
                },
                {
                    "kind": "CXXDestructorDecl",
                    "name": "~Ops",
                    "mangledName": "dtor",
                    "type": {"qualType": "void () noexcept"},
                },
            ],
        }
        contract = adapter.ast_record_contracts([record], {"Ops"})["Ops"]
        self.assertTrue(contract["traits"]["polymorphic"])
        self.assertTrue(contract["traits"]["has_virtual_bases"])
        self.assertEqual(contract["lifecycle"]["copy_construct"]["link_name"], "copy")
        self.assertFalse(contract["lifecycle"]["move_construct"]["available"])
        self.assertTrue(contract["lifecycle"]["move_construct"]["deleted"])
        self.assertEqual(contract["lifecycle"]["destroy"]["unwind"], "no_unwind")

    def test_member_parameter_record_type_uses_owner_namespace(self):
        record_names = {
            "abi_complex::NativeDriver",
            "abi_complex::AbstractSink",
            "other::AbstractSink",
        }
        record = {
            "kind": "CXXRecordDecl",
            "name": "NativeDriver",
            "tagUsed": "class",
            "completeDefinition": True,
            "inner": [
                {
                    "kind": "CXXMethodDecl",
                    "name": "dispatch",
                    "mangledName": "?dispatch@NativeDriver@abi_complex@@QEBAHPEAVAbstractSink@2@H@Z",
                    "type": {"qualType": "int (AbstractSink *, int) const noexcept"},
                }
            ],
        }

        symbols = adapter.ast_symbols([record], record_names, set())

        self.assertEqual(len(symbols), 1)
        dispatch = symbols[0]
        self.assertEqual(dispatch.name, "abi_complex::NativeDriver::dispatch")
        self.assertEqual(dispatch.params[0]["type"]["cpp_type"], "AbstractSink *")
        self.assertEqual(dispatch.params[0]["type"]["name"], "abi_complex.AbstractSink")
        self.assertEqual(
            adapter.symbol_for_vtable_entry(
                symbols,
                "abi_complex::NativeDriver",
                "dispatch",
                ["AbstractSink *", "int"],
                "method",
                record_names,
            ),
            dispatch.mangled,
        )
        self.assertEqual(
            adapter.resolve_dci_record_type_name(
                "AbstractSink",
                "client::NativeDriver",
                {"one::AbstractSink", "two::AbstractSink"},
            ),
            "AbstractSink",
        )

    def test_relative_qualified_param_resolves_via_owner_scope(self):
        # `virtual void log(const details::log_msg &)` written inside
        # `spdlog::sinks::sink` records the param as `details::log_msg`, which is
        # relative to the enclosing `spdlog` namespace, not global.  It must
        # resolve to the fully-qualified record so it matches the layout name.
        record_names = {
            "spdlog::details::log_msg",
            "spdlog::sinks::sink",
            "spdlog::formatter",
        }
        self.assertEqual(
            adapter.resolve_dci_record_type_name(
                "details.log_msg",
                "spdlog::sinks::sink",
                record_names,
            ),
            "spdlog.details.log_msg",
        )
        # An unrelated relative name that does not resolve in any enclosing
        # scope is left untouched (no cross-namespace guessing).
        self.assertEqual(
            adapter.resolve_dci_record_type_name(
                "other.thing",
                "spdlog::sinks::sink",
                record_names,
            ),
            "other.thing",
        )
        # A genuinely global-qualified name still matches exactly.
        self.assertEqual(
            adapter.resolve_dci_record_type_name(
                "spdlog.details.log_msg",
                "spdlog::sinks::sink",
                record_names,
            ),
            "spdlog.details.log_msg",
        )

    def test_enclosing_namespaces_order(self):
        self.assertEqual(
            adapter.enclosing_namespaces("spdlog::sinks::sink"),
            ["spdlog::sinks", "spdlog"],
        )
        self.assertEqual(adapter.enclosing_namespaces("Global"), [])

    def test_template_record_type_uses_root_owner_namespace(self):
        record_names = {
            "dci_stress::TaggedBox<int, dci_stress::IntTag>",
            "other::TaggedBox<int, dci_stress::IntTag>",
        }
        symbol = adapter.Symbol(
            name="dci_stress::TaggedOwner::set_pair",
            owner="dci_stress::TaggedOwner",
            member_name="set_pair",
            mangled="?set_pair@TaggedOwner@dci_stress@@QEAAAEAV12@HH@Z",
            kind="method",
            calling_convention="cxx_method",
            params=[],
            ret={
                "type": adapter.dci_type("TaggedBox<int, dci_stress::IntTag> &"),
                "location": "abi",
            },
        )

        adapter.qualify_symbol_record_types(symbol, record_names)

        self.assertEqual(
            symbol.ret["type"]["name"],
            "dci_stress.TaggedBox<int,dci_stress.IntTag>",
        )
        self.assertEqual(
            adapter.resolve_dci_record_type_name(
                "TaggedBox<int, dci_stress.IntTag>",
                "dci_stress::TaggedOwner",
                record_names,
            ),
            "dci_stress.TaggedBox<int,dci_stress.IntTag>",
        )

    def test_enum_name_qualification_is_constrained(self):
        old_enums = adapter.CPP_ENUM_UNDERLYING
        try:
            adapter.CPP_ENUM_UNDERLYING = {"dci_stress.Color": "int"}
            free_color = adapter.dci_type("Color")
            self.assertEqual(free_color["name"], "i32")
            self.assertEqual(free_color["enum_name"], "dci_stress.Color")

            adapter.CPP_ENUM_UNDERLYING = {
                "one.Color": "int",
                "two.Color": "unsigned int",
            }
            self.assertEqual(adapter.resolve_dci_enum_type_name("Color", ""), "")
            self.assertEqual(adapter.dci_type("Color")["kind"], "class")
            self.assertEqual(
                adapter.dci_type("Color", "one::Driver")["enum_name"],
                "one.Color",
            )
        finally:
            adapter.CPP_ENUM_UNDERLYING = old_enums

    def test_vtable_entry_matches_full_parameter_type_contract(self):
        record_names = {"abi_complex::NativeDriver", "abi_complex::AbstractSink"}

        def make_symbol(parameter_type, mangled):
            symbol = adapter.Symbol(
                name="abi_complex::NativeDriver::dispatch",
                owner="abi_complex::NativeDriver",
                member_name="dispatch",
                mangled=mangled,
                kind="method",
                calling_convention="cxx_virtual_method",
                params=[
                    {
                        "name": "sink",
                        "type": adapter.dci_type(parameter_type),
                        "location": "abi",
                    }
                ],
                ret={"type": adapter.dci_type("void"), "location": "abi"},
                is_virtual=True,
            )
            adapter.qualify_symbol_record_types(symbol, record_names)
            return symbol

        pointer = make_symbol("AbstractSink *", "dispatch_pointer")
        const_pointer = make_symbol("const AbstractSink *", "dispatch_const_pointer")
        reference = make_symbol("AbstractSink &", "dispatch_reference")
        symbols = [pointer, const_pointer, reference]

        self.assertEqual(
            adapter.symbol_for_vtable_entry(
                symbols,
                "abi_complex::NativeDriver",
                "dispatch",
                ["AbstractSink *"],
                "method",
                record_names,
            ),
            pointer.mangled,
        )
        self.assertEqual(
            adapter.symbol_for_vtable_entry(
                symbols,
                "abi_complex::NativeDriver",
                "dispatch",
                ["const AbstractSink *"],
                "method",
                record_names,
            ),
            const_pointer.mangled,
        )
        self.assertEqual(
            adapter.symbol_for_vtable_entry(
                symbols,
                "abi_complex::NativeDriver",
                "dispatch",
                ["AbstractSink &"],
                "method",
                record_names,
            ),
            reference.mangled,
        )

    def test_vtable_parser_does_not_treat_thunk_annotations_as_entries(self):
        dump = """
VFTable for 'A' in 'RightDerived' (2 entries).
 0 | RightDerived RTTI
 1 | RightDerived::~RightDerived() [vector deleting]

Thunks for 'RightDerived::~RightDerived()' (1 entry).
 0 | [this adjustment: -16 non-virtual]

VFTable for 'PolyBase' (2 entries).
 0 | PolyBase RTTI
 1 | int PolyBase::value()
"""

        tables = adapter.parse_vtable_layouts(
            dump, {"RightDerived", "A", "PolyBase"}, []
        )

        self.assertEqual(len(tables), 2)
        self.assertEqual(tables[0].class_name, "RightDerived")
        self.assertEqual(
            [entry.name for entry in tables[0].entries],
            [
                "RightDerived RTTI",
                "RightDerived::~RightDerived() [vector deleting]",
            ],
        )
        self.assertEqual(tables[1].class_name, "PolyBase")
        self.assertEqual(
            [entry.name for entry in tables[1].entries],
            ["PolyBase RTTI", "int PolyBase::value()"],
        )

    def test_document_has_generic_schema_and_aggregate_lowering(self):
        big = adapter.RecordLayout(
            "Big",
            size=16,
            alignment=8,
            traits={
                "can_pass_in_registers": False,
                "pod": True,
                "polymorphic": True,
                "trivially_copyable": True,
            },
            lifecycle={
                "copy_construct": {
                    "available": True,
                    "accessible": True,
                    "trivial": True,
                },
                "destroy": {
                    "available": True,
                    "accessible": True,
                    "trivial": True,
                },
            },
            has_vtable=True,
        )
        symbol = adapter.Symbol(
            name="roundtrip",
            owner="",
            member_name="roundtrip",
            mangled="?roundtrip@@YA?AUBig@@U1@@Z",
            kind="function",
            calling_convention="cxx_free_function",
            params=[{"name": "value", "type": adapter.dci_type("Big"), "location": "abi"}],
            ret={"type": adapter.dci_type("Big"), "location": "abi"},
            unwind="no_unwind",
        )
        vtable = adapter.VTable(
            "Big", "Big", [adapter.VTableEntry(0, 0, "Big RTTI", "rtti")]
        )
        with tempfile.TemporaryDirectory() as temp_dir:
            output = Path(temp_dir) / "contract.dci"
            adapter.write_abi(output, "x86_64-pc-windows-msvc", [], [big], [symbol], [vtable], [])
            document = json.loads(output.read_text(encoding="utf-8"))
        self.assertEqual(document["schema"]["name"], "dci")
        self.assertIn(".abi.json", document["schema"]["file_extensions"])
        self.assertEqual(document["control_flow"]["boundary"], "no_unwind")
        emitted = document["exports"]["symbols"][0]
        self.assertEqual(emitted["link_name"], symbol.mangled)
        self.assertEqual(emitted["abi"]["params"][0]["passing"], "indirect")
        self.assertEqual(emitted["abi"]["return"]["passing"], "sret")
        runtime = document["exports"]["layouts"][0]["runtime_type"]
        self.assertTrue(runtime["operations"]["checked_downcast"]["requires_stub"])
        self.assertEqual(
            document["exports"]["dispatch_tables"], document["exports"]["vtables"]
        )
        self.assertEqual(document["exports"]["dispatch_tables"][0]["class_name"], "Big")

    def test_register_aggregates_copy_compiler_codegen_types(self):
        for size, token in ((1, "i8"), (2, "i16"), (4, "i32"), (8, "i64")):
            with self.subTest(size=size):
                record = adapter.RecordLayout(
                    f"RegisterValue{size}",
                    size=size,
                    alignment=size,
                    traits={
                        "can_pass_in_registers": True,
                        "trivially_copyable": True,
                    },
                )
                type_info = adapter.dci_type(record.type_name)
                signature = {
                    "trivially_copyable": True,
                    "memory": False,
                    "llvm_args": [token],
                    "ret_llvm": token,
                    "ret_sret": False,
                }
                parameter = adapter.abi_value_lowering(
                    type_info,
                    [record],
                    "parameter",
                    "x86_64-pc-windows-msvc",
                    {record.type_name: signature},
                )
                returned = adapter.abi_value_lowering(
                    type_info,
                    [record],
                    "return",
                    "x86_64-pc-windows-msvc",
                    {record.type_name: signature},
                )
                for lowering in (parameter, returned):
                    self.assertEqual(lowering["passing"], "coerce")
                    self.assertEqual(lowering["coerce_to"]["name"], token)
                    self.assertEqual(lowering["size"], size)
                    self.assertEqual(lowering["alignment"], size)
        # canPassInRegisters without a CodeGen dump is not enough to invent iN.
        record = adapter.RecordLayout(
            "RegisterValue8",
            size=8,
            alignment=8,
            traits={"can_pass_in_registers": True, "trivially_copyable": True},
        )
        with self.assertRaises(adapter.AdapterContractError):
            adapter.abi_value_lowering(
                adapter.dci_type(record.type_name),
                [record],
                "parameter",
                "x86_64-pc-windows-msvc",
            )

    def test_nested_field_type_gets_its_own_layout(self):
        # ``parse_record_layouts(allowed=record_names)`` keeps only the top-level
        # API record; a nested field type such as ``fmt::basic_string_view<char>``
        # (``log_msg.payload``) is emitted in the same dump but dropped.  The
        # closure must adopt it so Vyx can read ``.data_`` / ``.size_``.
        dump = """
*** Dumping AST Record Layout
         0 | class spdlog::details::log_msg
         0 |   fmt::basic_string_view<char> payload
           | [sizeof=16, align=8,
           |  nvsize=16, nvalign=8]

*** Dumping AST Record Layout
         0 | class fmt::basic_string_view<char>
         0 |   const char * data_
         8 |   unsigned long size_
           | [sizeof=16, align=8,
           |  nvsize=16, nvalign=8]
"""
        layouts, allowed = adapter.expand_layout_closure(
            dump, ["spdlog::details::log_msg"]
        )
        by_name = {record.type_name: record for record in layouts}
        self.assertIn("fmt::basic_string_view<char>", by_name)
        self.assertIn("fmt::basic_string_view<char>", allowed)
        view = by_name["fmt::basic_string_view<char>"]
        self.assertEqual(view.size, 16)
        self.assertEqual(view.alignment, 8)
        self.assertEqual(
            [(field.name, field.offset) for field in view.fields],
            [("data_", 0), ("size_", 8)],
        )

    def test_scoped_enum_maps_to_primitive_after_probe_populates_map(self):
        # ``dci_type`` alone spells a bare enum as ``kind=class`` (no layout);
        # once the selected-compiler probe records the underlying integer in
        # ``CPP_ENUM_UNDERLYING``, ``rewrite_enum_symbol_types`` must relower a
        # symbol's enum parameter to the ``iN`` primitive with an ``enum_name``.
        pre = adapter.dci_type("spdlog::level::level_enum", "spdlog::logger")
        self.assertEqual(pre["kind"], "class")
        symbol = adapter.Symbol(
            name="spdlog::logger::log",
            owner="spdlog::logger",
            member_name="log",
            mangled="_ZN6spdlog6logger3logE",
            kind="function",
            calling_convention="cxx_member_function",
            params=[
                {
                    "name": "level",
                    "type": adapter.dci_type(
                        "spdlog::level::level_enum", "spdlog::logger"
                    ),
                    "location": "abi",
                }
            ],
            ret=None,
            unwind="no_unwind",
        )
        previous = adapter.CPP_ENUM_UNDERLYING
        try:
            adapter.CPP_ENUM_UNDERLYING = {"spdlog::level::level_enum": "int"}
            adapter.rewrite_enum_symbol_types([symbol])
        finally:
            adapter.CPP_ENUM_UNDERLYING = previous
        param_type = symbol.params[0]["type"]
        self.assertEqual(param_type["kind"], "primitive")
        self.assertEqual(param_type["name"], "i32")
        self.assertEqual(param_type["enum_name"], "spdlog.level.level_enum")

    def test_itanium_compiler_llvm_types_are_copied_into_the_descriptor(self):
        record = adapter.RecordLayout(
            "fmt::basic_string_view<char>",
            size=16,
            alignment=8,
            traits={"trivially_copyable": True, "standard_layout": True},
        )
        type_info = adapter.dci_type(record.type_name)
        signature = {
            "trivially_copyable": True,
            "standard_layout": True,
            "memory": False,
            "llvm_args": ["ptr", "i64"],
            "ret_llvm": "{ptr,i64}",
            "ret_sret": False,
        }
        machine_signatures = {record.type_name: signature}
        param = adapter.abi_value_lowering(
            type_info,
            [record],
            "parameter",
            "x86_64-unknown-linux-gnu",
            machine_signatures,
        )
        self.assertEqual(param["passing"], "split")
        self.assertEqual(param["registers"], ["ptr", "i64"])
        self.assertEqual(param["size"], 16)
        returned = adapter.abi_value_lowering(
            type_info,
            [record],
            "return",
            "x86_64-unknown-linux-gnu",
            machine_signatures,
        )
        self.assertEqual(returned["passing"], "coerce")
        self.assertEqual(returned["registers"], ["ptr", "i64"])

        small = adapter.RecordLayout(
            "Handle",
            size=8,
            alignment=8,
            traits={"trivially_copyable": True, "standard_layout": True},
        )
        small_low = adapter.abi_value_lowering(
            adapter.dci_type(small.type_name),
            [small],
            "parameter",
            "x86_64-unknown-linux-gnu",
            {small.type_name: {
                "trivially_copyable": True,
                "standard_layout": True,
                "memory": False,
                "llvm_args": ["i64"],
                "ret_llvm": "i64",
                "ret_sret": False,
            }},
        )
        self.assertEqual(small_low["passing"], "coerce")
        self.assertEqual(small_low["coerce_to"]["name"], "i64")

    def test_itanium_nontrivial_or_unclassifiable_aggregate_fails_closed(self):
        record = adapter.RecordLayout(
            "fmt::basic_string_view<char>",
            size=16,
            alignment=8,
            traits={"trivially_copyable": True, "standard_layout": True},
        )
        type_info = adapter.dci_type(record.type_name)
        # No probe result at all: cannot invent a convention -> fail closed.
        with self.assertRaises(adapter.AdapterContractError):
            adapter.abi_value_lowering(
                type_info, [record], "parameter", "x86_64-unknown-linux-gnu", None
            )
        # SysV class nicknames without compiler LLVM types are not a lowering.
        class_only = {
            "trivially_copyable": True,
            "standard_layout": True,
            "memory": False,
            "classes": ["INTEGER", "INTEGER"],
        }
        with self.assertRaises(adapter.AdapterContractError):
            adapter.abi_value_lowering(
                type_info,
                [record],
                "parameter",
                "x86_64-unknown-linux-gnu",
                {record.type_name: class_only},
            )
        with self.assertRaises(adapter.AdapterContractError):
            adapter.abi_value_lowering(
                type_info,
                [record],
                "parameter",
                "x86_64-unknown-linux-gnu",
                {record.type_name: dict(class_only, classes=["SSE", "SSE"])},
            )
        # Compiler MEMORY CodeGen is an indirect/byval fact, not a rejection.
        mem_sig = {
            "trivially_copyable": True,
            "standard_layout": True,
            "memory": True,
            "byval": False,
            "llvm_args": [],
            "ret_sret": True,
            "ret_llvm": "void",
        }
        mem_low = adapter.abi_value_lowering(
            type_info,
            [record],
            "parameter",
            "x86_64-unknown-linux-gnu",
            {record.type_name: mem_sig},
        )
        self.assertEqual(mem_low["passing"], "indirect")
        # Probe says not trivially copyable: fail closed.
        with self.assertRaises(adapter.AdapterContractError):
            adapter.abi_value_lowering(
                type_info,
                [record],
                "parameter",
                "x86_64-unknown-linux-gnu",
                {record.type_name: dict(class_only, trivially_copyable=False, llvm_args=["i64"])},
            )

    def test_runtime_upcast_is_intrinsic_or_explicitly_stub_only(self):
        direct = adapter.RecordLayout(
            "DirectDerived",
            size=16,
            alignment=8,
            bases=[
                adapter.BaseLayout(
                    "Base",
                    0,
                    adjustment={"kind": "constant", "offset": 0},
                )
            ],
        )
        virtual_constant = adapter.RecordLayout(
            "StaticVirtualDerived",
            size=24,
            alignment=8,
            bases=[
                adapter.BaseLayout(
                    "VirtualBase",
                    16,
                    is_virtual=True,
                    adjustment={"kind": "constant", "offset": 16},
                )
            ],
        )
        virtual_table = adapter.RecordLayout(
            "TableVirtualDerived",
            size=24,
            alignment=8,
            bases=[
                adapter.BaseLayout(
                    "VirtualBase",
                    16,
                    is_virtual=True,
                    adjustment={
                        "kind": "table",
                        "table_pointer_offset": 0,
                        "table_entry_offset": 4,
                        "table_entry_size": 4,
                        "table_entry_signed": True,
                        "displacement_base_offset": 0,
                        "result": "object_plus_base_plus_displacement",
                        "null_preserving": True,
                    },
                )
            ],
        )
        fallback = adapter.RecordLayout(
            "FallbackDerived",
            size=24,
            alignment=8,
            bases=[
                adapter.BaseLayout(
                    "VirtualBase",
                    16,
                    is_virtual=True,
                    adjustment={
                        "kind": "table",
                        "fallback_complete_object_offset": 16,
                    },
                )
            ],
        )

        operations = adapter.runtime_type_operations_json(
            [direct, virtual_constant, virtual_table, fallback], [], rtti_enabled=False
        )
        direct_upcast = operations[0]["operations"]["upcast"]
        constant_upcast = operations[1]["operations"]["upcast"]
        table_upcast = operations[2]["operations"]["upcast"]
        fallback_upcast = operations[3]["operations"]["upcast"]
        self.assertEqual(direct_upcast["availability"], "required")
        self.assertEqual(direct_upcast["intrinsic"], "dci.layout.upcast")
        self.assertEqual(constant_upcast["availability"], "required")
        self.assertEqual(table_upcast["availability"], "required")
        self.assertEqual(fallback_upcast["availability"], "optional")
        self.assertTrue(fallback_upcast["requires_stub"])
        self.assertNotIn("intrinsic", fallback_upcast)

    def test_explicit_calling_convention_does_not_break_type_split(self):
        ret, params, _ = adapter.split_function_type(
            "int (int) __attribute__((vectorcall))"
        )
        self.assertEqual(ret, "int")
        self.assertEqual(params, ["int"])
        self.assertEqual(
            adapter.declared_calling_convention("int (int) __attribute__((vectorcall))"),
            "vectorcall",
        )

    def test_std_fixed_width_integer_aliases_are_primitives(self):
        self.assertEqual(adapter.dci_type("std::uint32_t")["name"], "u32")
        self.assertEqual(adapter.dci_type("std::int32_t")["name"], "i32")
        self.assertEqual(adapter.dci_type("std::uint64_t")["name"], "u64")
        self.assertEqual(adapter.dci_type("std::int64_t")["name"], "i64")

    def test_std_size_t_is_a_pointer_width_primitive(self):
        # ``std::size_t`` is an unsigned integer ABI, not a class needing
        # layout/lifecycle and not a UniquePtr/Pin ownership wrapper.
        for spelled in (
            "size_t",
            "std::size_t",
            "std.size_t",
            "::size_t",
            "::std::size_t",
            "const std::size_t",
        ):
            info = adapter.dci_type(spelled)
            self.assertEqual(info["kind"], "primitive", spelled)
            self.assertEqual(info["name"], "u64", spelled)
            self.assertEqual(
                adapter.parameter_ownership(info, [], "abi::Buffer::constructor"),
                "copy",
                spelled,
            )
            self.assertEqual(
                adapter.return_ownership(info, [], "abi::Buffer::size"),
                "copy",
                spelled,
            )
        self.assertEqual(adapter.dci_type("std::ptrdiff_t")["name"], "i64")
        self.assertEqual(adapter.dci_type("std::uintptr_t")["name"], "u64")
        self.assertEqual(adapter.dci_type("std::intptr_t")["name"], "i64")

    def test_unknown_typedef_can_alias_onto_a_primitive(self):
        saved = dict(adapter.CPP_TYPE_ALIAS_TARGETS)
        try:
            adapter.register_desugared_alias("MySize", "unsigned long long")
            info = adapter.dci_type("MySize")
            self.assertEqual(info["kind"], "primitive")
            self.assertEqual(info["name"], "u64")
            self.assertEqual(info.get("alias_name"), "MySize")
        finally:
            adapter.CPP_TYPE_ALIAS_TARGETS.clear()
            adapter.CPP_TYPE_ALIAS_TARGETS.update(saved)

    def test_size_t_primitive_is_not_shadowed_by_desugaring(self):
        saved = dict(adapter.CPP_TYPE_ALIAS_TARGETS)
        try:
            adapter.register_desugared_alias("size_t", "unsigned int")
            adapter.register_desugared_alias("std::size_t", "unsigned int")
            self.assertEqual(adapter.dci_type("size_t")["name"], "u64")
            self.assertEqual(adapter.dci_type("std::size_t")["name"], "u64")
        finally:
            adapter.CPP_TYPE_ALIAS_TARGETS.clear()
            adapter.CPP_TYPE_ALIAS_TARGETS.update(saved)

    def test_cdecl_attribute_is_not_parsed_as_a_parameter(self):
        self.assertEqual(
            adapter.split_function_type("uint32_t (void) __attribute__((cdecl))"),
            ("uint32_t", [], False),
        )
        self.assertEqual(
            adapter.split_function_type(
                "int32_t (uint32_t, int32_t) __attribute__((cdecl))"
            ),
            ("int32_t", ["uint32_t", "int32_t"], False),
        )

    def test_unverified_record_symbols_are_rejected_by_selected_compiler(self):
        gcc = adapter.cpp_toolchains.CppToolchain(
            "gcc", "g++", "14", "g++ 14", "x86_64-linux-gnu", "gnu"
        )
        method = adapter.Symbol(
            name="api::Widget::poke",
            owner="api::Widget",
            member_name="poke",
            mangled="_ZN3api6Widget4pokeEi",
            kind="method",
            calling_convention="cxx_method",
            params=[{"name": "x", "type": adapter.dci_type("int"), "location": "abi"}],
            ret={"type": adapter.dci_type("int"), "location": "abi"},
            unwind="no_unwind",
        )
        record = adapter.RecordLayout("api::Widget", size=8, alignment=8)
        with tempfile.TemporaryDirectory() as temp_dir:
            output = Path(temp_dir) / "contract.dci"
            adapter.write_abi(
                output,
                "x86_64-unknown-linux-gnu",
                [],
                [record],
                [method],
                [],
                [],
                toolchain=gcc,
                unverified_records={"api::Widget"},
            )
            document = json.loads(output.read_text(encoding="utf-8"))
        self.assertEqual(document["exports"]["symbols"], [])
        rejected = document["exports"]["rejected_symbols"]
        self.assertEqual(rejected[0]["name"], "api::Widget::poke")
        self.assertIn("could not extract stable layout facts", rejected[0]["reason"])
        self.assertIn("gcc", rejected[0]["reason"])

    def test_effective_flags_are_recorded_on_contract_compiler(self):
        gcc = adapter.cpp_toolchains.CppToolchain(
            "gcc", "g++", "14", "g++ 14", "x86_64-linux-gnu", "gnu"
        )
        with tempfile.TemporaryDirectory() as temp_dir:
            output = Path(temp_dir) / "contract.dci"
            adapter.write_abi(
                output,
                "x86_64-unknown-linux-gnu",
                [],
                [],
                [],
                [],
                [],
                toolchain=gcc,
                compiler_flags=["-DFEATURE_ON=1", "-fpack-struct=1"],
            )
            document = json.loads(output.read_text(encoding="utf-8"))
        self.assertEqual(
            document["source"]["compiler"]["flags"], ["-DFEATURE_ON=1", "-fpack-struct=1"]
        )
        self.assertEqual(document["source"]["compiler"]["vendor"], "gcc")
        self.assertEqual(
            document["source"]["compiler"]["target_triplet"], "x86_64-linux-gnu"
        )

    def test_extract_cmake_compile_flags_parses_compile_commands(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            include = root / "include"
            include.mkdir()
            header = include / "api.hpp"
            header.write_text("#pragma once\n", encoding="utf-8")
            build = root / "cmake-build"
            build.mkdir()
            (build / "compile_commands.json").write_text(
                json.dumps(
                    [
                        {
                            "directory": str(build),
                            "file": str(root / "src" / "api.cpp"),
                            "command": (
                                f"/usr/bin/g++ -I{include} -DFEATURE_ON=1 "
                                "-std=c++20 -fpack-struct=1 -O2 -Wall -c "
                                f"{root / 'src' / 'api.cpp'} -o api.o"
                            ),
                        },
                        {
                            "directory": str(build),
                            "file": str(root / "other" / "unrelated.cpp"),
                            "arguments": [
                                "/usr/bin/g++",
                                "-I/opt/other/include",
                                "-DUNRELATED=1",
                                "-c",
                                str(root / "other" / "unrelated.cpp"),
                            ],
                        },
                    ]
                ),
                encoding="utf-8",
            )
            flags = adapter.extract_cmake_compile_flags(str(build), [str(header)])
        self.assertIn(f"-I{include}", flags)
        self.assertIn("-DFEATURE_ON=1", flags)
        self.assertIn("-std=c++20", flags)
        self.assertIn("-fpack-struct=1", flags)
        # Warnings/optimization/-c/-o/input are dropped; unrelated TU is skipped.
        self.assertNotIn("-O2", flags)
        self.assertNotIn("-Wall", flags)
        self.assertNotIn("-DUNRELATED=1", flags)

    def test_extract_cmake_compile_flags_keeps_quoted_windows_paths(self):
        """Native CMake command strings must retain backslashes and spaces."""
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            include = root / "Program Files" / "dci include"
            include.mkdir(parents=True)
            header = include / "api.hpp"
            header.write_text("#pragma once\n", encoding="utf-8")
            build = root / "cmake-build"
            build.mkdir()
            command = (
                f'g++ -I"{include}" -DQUOTED=1 -std=c++20 -c '
                f'"{root / "src" / "api.cpp"}" -o api.o'
            )
            (build / "compile_commands.json").write_text(
                json.dumps([{"directory": str(build), "command": command}]),
                encoding="utf-8",
            )
            flags = adapter.extract_cmake_compile_flags(str(build), [str(header)])
        self.assertIn(f"-I{include}", flags)
        self.assertIn("-DQUOTED=1", flags)
        self.assertIn("-std=c++20", flags)

    def test_extract_cmake_compile_flags_requires_compile_commands(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            with self.assertRaisesRegex(
                adapter.AdapterContractError, "compile_commands.json"
            ):
                adapter.extract_cmake_compile_flags(temp_dir, [])

    def test_load_compile_flags_reads_file_and_inline_values(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            flag_file = Path(temp_dir) / "compile_flags.txt"
            flag_file.write_text(
                "-Iinclude\n# a comment\n-DFOO=1\n\n", encoding="utf-8"
            )
            flags = adapter.load_compile_flags(
                [str(flag_file), "-DBAR=2", "-fshort-enums -m32"]
            )
        self.assertEqual(
            flags, ["-Iinclude", "-DFOO=1", "-DBAR=2", "-fshort-enums", "-m32"]
        )

    def test_dcib_output_keeps_debug_json_diagnostic(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            output = root / "contract.dcib"
            debug_json = root / "contract.debug.json"
            adapter.write_abi(
                output,
                "x86_64-pc-windows-msvc",
                [],
                [],
                [],
                [],
                [],
                debug_json_path=debug_json,
            )
            document = adapter.dcib.decode(output.read_bytes())
            debug_document = json.loads(debug_json.read_text(encoding="utf-8"))

        self.assertEqual(document["schema"]["encoding"], "dcib")
        self.assertIn(".dcib", document["schema"]["file_extensions"])
        self.assertEqual(debug_document, document)


REPO_ROOT = Path(__file__).resolve().parents[3]
FIXTURE_HPP = REPO_ROOT / "tools" / "dci" / "bench" / "fixtures" / "abi_fixtures.hpp"
FIXTURE_CPP = REPO_ROOT / "tools" / "dci" / "bench" / "fixtures" / "abi_fixtures.cpp"
RUNTIME_DIR = REPO_ROOT / "tools" / "dci" / "runtime"
LOCAL_CLANGXX = REPO_ROOT / "clang" / "bin" / "clang++.exe"


def _clangxx_path() -> str | None:
    if LOCAL_CLANGXX.is_file():
        return str(LOCAL_CLANGXX)
    return shutil.which("clang++") or shutil.which("clang++.exe")


@unittest.skipUnless(
    _clangxx_path() is not None and FIXTURE_HPP.is_file(),
    "clang++ and abi_fixtures.hpp are required for translate_unwind Adapter tests",
)
class DciTranslateUnwindAdapterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.clangxx = _clangxx_path()
        cls._tmpdir = tempfile.TemporaryDirectory(prefix="vyx_dci_tw_adapt_")
        cls.temp = Path(cls._tmpdir.name)
        cls.out = cls.temp / "abi_fixtures.dcib"
        cls.debug = cls.temp / "abi_fixtures.dci.json"
        cls.stub = cls.temp / "abi_fixtures_translate_unwind.cpp"
        env = dict(os.environ)
        env.setdefault("LLVM_ROOT", str(REPO_ROOT / "clang"))
        rc = adapter.main(
            [
                "--out",
                str(cls.out),
                "--debug-json-out",
                str(cls.debug),
                "--stub-out",
                str(cls.stub),
                "--include",
                str(FIXTURE_HPP),
                "--toolchain",
                "clang",
                "--cxx",
                cls.clangxx,
                "--target",
                "x86_64-pc-windows-msvc",
                "--std",
                "c++17",
            ]
        )
        if rc != 0:
            raise unittest.SkipTest(f"C++ Adapter failed with rc={rc}")
        cls.document = json.loads(cls.debug.read_text(encoding="utf-8"))
        cls.stub_source = cls.stub.read_text(encoding="utf-8") if cls.stub.is_file() else ""

    @classmethod
    def tearDownClass(cls) -> None:
        cls._tmpdir.cleanup()

    def test_throwing_checked_divide_emits_translate_unwind_translator(self) -> None:
        symbols = self.document["exports"]["symbols"]
        orig = next(
            (
                item
                for item in symbols
                if (item.get("member_name") or item.get("name") or "").endswith(
                    "checked_divide"
                )
                and item.get("control_flow", {}).get("unwind") == "may_unwind"
            ),
            None,
        )
        self.assertIsNotNone(orig, "checked_divide was not exported as may_unwind")
        self.assertEqual(orig["control_flow"]["unwind"], "may_unwind")
        requests = [
            request
            for request in self.document["exports"].get("stub_requests") or []
            if (request.get("synthesis") or {}).get("strategy") == "translate_unwind"
            and request.get("target") in {orig["link_name"], orig["name"]}
        ]
        self.assertEqual(len(requests), 1, requests)
        wrapper = requests[0]["wrapper"]["link_name"]
        self.assertTrue(wrapper.startswith("dci_tr_"), wrapper)
        translators = [item for item in symbols if item.get("link_name") == wrapper]
        self.assertEqual(len(translators), 1, translators)
        tr = translators[0]
        self.assertEqual(tr["control_flow"]["unwind"], "no_unwind")
        self.assertEqual(tr["control_flow"]["propagation"]["mode"], "forbidden")
        self.assertTrue((tr.get("name") or "").endswith("_translated"))

    def test_every_translate_unwind_request_targets_an_exported_may_unwind_symbol(self) -> None:
        by_link = {
            item.get("link_name"): item
            for item in self.document["exports"]["symbols"]
            if item.get("link_name")
        }
        requests = [
            request
            for request in self.document["exports"].get("stub_requests") or []
            if (request.get("synthesis") or {}).get("strategy") == "translate_unwind"
        ]
        self.assertGreaterEqual(len(requests), 1)
        for request in requests:
            target = request.get("target") or request.get("synthesis", {}).get("target")
            wrapper = request["wrapper"]["link_name"]
            self.assertIn(target, by_link)
            self.assertEqual(by_link[target]["control_flow"]["unwind"], "may_unwind")
            self.assertIn(wrapper, by_link)
            self.assertTrue(wrapper.startswith("dci_tr_"))
            self.assertNotEqual(wrapper, target)

    def test_generated_stub_catches_checked_divide_without_editing_the_header(self) -> None:
        header = FIXTURE_HPP.read_text(encoding="utf-8")
        self.assertIn("int32_t checked_divide(int32_t a, int32_t b);", header)
        orig = next(
            item
            for item in self.document["exports"]["symbols"]
            if (item.get("member_name") or item.get("name") or "").endswith(
                "checked_divide"
            )
            and item.get("control_flow", {}).get("unwind") == "may_unwind"
        )
        request = next(
            request
            for request in self.document["exports"]["stub_requests"]
            if request.get("target") in {orig["link_name"], orig["name"]}
        )
        link = request["wrapper"]["link_name"]
        self.assertIn(link, self.stub_source)
        probe = self.temp / "checked_divide_probe.cpp"
        probe.write_text(
            self.stub_source
            + "\n#include <cstdio>\n#include <cstring>\n"
            + f"""
int main() {{
    auto okv = {link}(10, 2);
    if (okv.tag != 0 || okv.ok != 5) {{
        std::fprintf(stderr, "ok path failed tag=%d ok=%d\\n", (int)okv.tag, (int)okv.ok);
        return 1;
    }}
    auto bad = {link}(1, 0);
    if (bad.tag != 1) {{
        std::fprintf(stderr, "error tag failed\\n");
        return 1;
    }}
    if (bad.err.message == nullptr || std::strstr(bad.err.message, "division") == nullptr) {{
        std::fprintf(stderr, "error message missing\\n");
        return 1;
    }}
    dci_failure_destroy(&okv.err);
    dci_failure_destroy(&bad.err);
    return 0;
}}
""",
            encoding="utf-8",
        )
        exe = self.temp / "checked_divide_probe.exe"
        compile_cmd = [
            self.clangxx,
            "-std=c++17",
            "-O0",
            "-fexceptions",
            "-fcxx-exceptions",
            "-fms-compatibility",
            "-target",
            "x86_64-pc-windows-msvc",
            "-I",
            str(RUNTIME_DIR),
            "-I",
            str(FIXTURE_HPP.parent),
            str(probe),
            str(FIXTURE_CPP),
            str(RUNTIME_DIR / "dci_failure_common.cpp"),
            str(RUNTIME_DIR / "dci_msvc_failure.cpp"),
            "-o",
            str(exe),
        ]
        compiled = subprocess.run(
            compile_cmd,
            capture_output=True,
            text=True,
            errors="replace",
        )
        self.assertEqual(
            compiled.returncode,
            0,
            compiled.stdout + "\n" + compiled.stderr,
        )
        ran = subprocess.run([str(exe)], capture_output=True, text=True, errors="replace")
        self.assertEqual(ran.returncode, 0, ran.stdout + "\n" + ran.stderr)


if __name__ == "__main__":
    unittest.main()
