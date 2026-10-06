"""Binding references/rename over stdio, then build and run the edited project."""
import argparse
import hashlib
import json
import pathlib
import shutil
import subprocess
import tempfile
import time
import urllib.parse

from measure_lsp import Client


def position(text, needle, word, occurrence=0):
    start = -1
    for _ in range(occurrence + 1):
        start = text.index(needle, start + 1)
    start += needle.index(word)
    prefix = text[:start]
    return {"line": prefix.count("\n"), "character": len(prefix.rsplit("\n", 1)[-1].encode("utf-16-le")) // 2}


def path_from_uri(value):
    value = urllib.parse.unquote(urllib.parse.urlsplit(value).path)
    if len(value) > 2 and value[0] == "/" and value[2] == ":":
        value = value[1:]
    return pathlib.Path(value).resolve()


def apply_edits(text, edits):
    lines = text.splitlines(keepends=True)
    def offset(pos):
        row = lines[pos["line"]]
        column = len(row.encode("utf-16-le")[:pos["character"] * 2].decode("utf-16-le"))
        return sum(map(len, lines[:pos["line"]])) + column
    result = text
    for edit in sorted(edits, key=lambda e: (e["range"]["start"]["line"], e["range"]["start"]["character"]), reverse=True):
        result = result[:offset(edit["range"]["start"])] + edit["newText"] + result[offset(edit["range"]["end"]):]
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lsp", type=pathlib.Path, required=True)
    ap.add_argument("--compiler", type=pathlib.Path, required=True)
    ap.add_argument("--fixture", type=pathlib.Path, default=pathlib.Path("tests/projects/editor_semantic_rename"))
    ap.add_argument("--result", type=pathlib.Path, required=True)
    args = ap.parse_args()
    lsp, compiler = args.lsp.resolve(), args.compiler.resolve()
    result = {"status": "failed", "lsp": str(lsp), "compiler": str(compiler),
              "lsp_sha256": hashlib.sha256(lsp.read_bytes()).hexdigest(),
              "compiler_sha256": hashlib.sha256(compiler.read_bytes()).hexdigest(), "checks": []}
    samples = []
    with tempfile.TemporaryDirectory(prefix="vyx-semantic-中文-") as temporary:
        workspace = pathlib.Path(temporary).resolve()
        shutil.copytree(args.fixture.resolve(), workspace, dirs_exist_ok=True,
                        ignore=shutil.ignore_patterns("target", ".cache"))
        client = Client(lsp, workspace)
        serial = 0
        def request(method, params, error=False):
            nonlocal serial
            serial += 1
            client.send(method, params, serial)
            try:
                answer = client.wait(lambda m: m.get("id") == serial, 120, 1536, samples)
            except RuntimeError as exc:
                if error:
                    assert "-32803" in str(exc) or "-32602" in str(exc), exc
                    return None
                raise
            assert not error, f"unsafe rename accepted: {answer}"
            return answer["result"]
        def source(file): return (workspace / "src" / file).read_text(encoding="utf-8")
        def params(file, needle, word, occurrence=0):
            return {"textDocument": {"uri": (workspace / "src" / file).as_uri()},
                    "position": position(source(file), needle, word, occurrence)}
        def references(file, needle, word, expected, declaration=True, occurrence=0):
            p = params(file, needle, word, occurrence)
            p["context"] = {"includeDeclaration": declaration}
            found = request("textDocument/references", p)
            actual = {(path_from_uri(r["uri"]).name, r["range"]["start"]["line"], r["range"]["start"]["character"]) for r in found}
            wanted = {(name, position(source(name), text, token, nth)["line"],
                       position(source(name), text, token, nth)["character"])
                      for name, text, token, nth in expected}
            assert actual == wanted, (file, needle, found, wanted)
            result["checks"].append(f"references:{file}:{word}:{declaration}")
            return found
        def rename(file, needle, word, new_name, apply=False, error=False):
            p = params(file, needle, word)
            p["newName"] = new_name
            edit = request("textDocument/rename", p, error=error)
            if error:
                result["checks"].append(f"reject:{word}->{new_name}")
                return
            assert edit and edit.get("documentChanges"), edit
            if apply:
                for document in edit["documentChanges"]:
                    path = path_from_uri(document["textDocument"]["uri"])
                    assert path.is_relative_to(workspace), path
                    path.write_text(apply_edits(path.read_text(encoding="utf-8"), document["edits"]), encoding="utf-8")
                result["checks"].append(f"rename:{word}->{new_name}")
            return edit
        try:
            request("initialize", {"rootUri": workspace.as_uri(), "capabilities": {
                "workspace": {"workspaceEdit": {"documentChanges": True}}}})
            client.send("initialized", {})
            price = [("catalog.vyx", "fn price", "price", 0),
                     ("main.vyx", "editor.catalog.price(3)", "price", 0),
                     ("pricing.vyx", "editor.catalog.price;", "price", 0),
                     ("pricing.vyx", "calculate(record.value)", "calculate", 0),
                     ("pricing.vyx", "calculate(value) + adjust", "calculate", 0)]
            references("catalog.vyx", "fn price", "price", price)
            references("catalog.vyx", "fn price", "price", price[1:], declaration=False)
            references("pricing.vyx", "use calculate", "calculate", [
                ("pricing.vyx", "use calculate", "calculate", 0), *price[-2:]])
            references("pricing.vyx", "let price", "price", [
                ("pricing.vyx", "let price", "price", 0),
                ("pricing.vyx", "+ price +", "price", 0)])
            references("pricing.vyx", "let price", "price", [
                ("pricing.vyx", "let price", "price", 1),
                ("pricing.vyx", "price != 40", "price", 0)], occurrence=1)
            references("catalog.vyx", "fn amount", "amount", [
                ("catalog.vyx", "fn amount", "amount", 0),
                ("main.vyx", "item.amount()", "amount", 0)])
            references("catalog.vyx", "value: i32", "value", [
                ("catalog.vyx", "value: i32", "value", 0),
                ("catalog.vyx", "self.value", "value", 0),
                ("pricing.vyx", "record.value", "value", 0),
                ("main.vyx", "Item { value", "value", 0)])
            references("shared_a.vyx", "fn supplement", "supplement", [
                ("shared_a.vyx", "fn supplement", "supplement", 0),
                ("shared_b.vyx", "supplement(value)", "supplement", 0)])
            references("catalog.vyx", "enum Status", "Status", [
                ("catalog.vyx", "enum Status", "Status", 0),
                ("main.vyx", "Status.Ready", "Status", 0),
                ("main.vyx", "Status.Ready", "Status", 1)])
            references("catalog.vyx", "Ready, Pending", "Ready", [
                ("catalog.vyx", "Ready, Pending", "Ready", 0),
                ("main.vyx", "Status.Ready", "Ready", 0),
                ("main.vyx", "Status.Ready", "Ready", 1)])
            references("catalog.vyx", "identity<T>", "T", [
                ("catalog.vyx", "identity<T>", "T", 0),
                ("catalog.vyx", "value: T", "T", 0),
                ("catalog.vyx", "-> T", "T", 0)])
            references("catalog.vyx", "class Slot<T>", "T", [
                ("catalog.vyx", "class Slot<T>", "T", 0),
                ("catalog.vyx", "value: T", "T", 1),
                ("catalog.vyx", "-> T", "T", 1)])
            references("catalog.vyx", "fn scale", "scale", [
                ("catalog.vyx", "fn scale", "scale", 0),
                ("catalog.vyx", "fn scale", "scale", 1),
                ("pricing.vyx", "scale(value)", "scale", 0),
                ("pricing.vyx", "scale(value, 3)", "scale", 0)])
            references("catalog.vyx", "var requests", "requests", [
                ("catalog.vyx", "var requests", "requests", 0),
                ("main.vyx", "requests =", "requests", 0),
                ("main.vyx", "requests +", "requests", 0),
                ("main.vyx", "requests !=", "requests", 0)])
            references("pricing.vyx", "let captured", "captured", [
                ("pricing.vyx", "let captured", "captured", 0),
                ("pricing.vyx", "[captured]", "captured", 0),
                ("pricing.vyx", "extra + captured", "captured", 0)])
            references("pricing.vyx", "(extra: i32)", "extra", [
                ("pricing.vyx", "(extra: i32)", "extra", 0),
                ("pricing.vyx", "extra + captured", "extra", 0)])
            references("pricing.vyx", "alias_shadow(calculate", "calculate", [
                ("pricing.vyx", "alias_shadow(calculate", "calculate", 0),
                ("pricing.vyx", "return calculate + 1", "calculate", 0)])
            assert request("textDocument/prepareRename", params("main.vyx", "fn main", "fn")) is None
            external = workspace / "src" / "external.vyx"
            external.write_text('module editor.external;\nextern "C" { fn native_entry(value: i32) -> i32; }\n', encoding="utf-8")
            assert request("textDocument/prepareRename", params("external.vyx", "fn native_entry", "native_entry")) is None
            rename("external.vyx", "fn native_entry", "native_entry", "changed_abi", error=True)
            external.unlink()
            sdk_source = compiler.parent.parent / "std_packages" / "io" / "src" / "fs.vyx"
            if sdk_source.is_file():
                sdk_text = sdk_source.read_text(encoding="utf-8-sig")
                sdk_uri = sdk_source.as_uri()
                sdk_query = {"textDocument": {"uri": sdk_uri}, "position": position(sdk_text, "public fn readFile", "readFile")}
                assert request("textDocument/prepareRename", sdk_query) is None
                client.send("textDocument/didOpen", {"textDocument": {"uri": sdk_uri, "languageId": "vyx", "version": 1, "text": sdk_text}})
                assert request("textDocument/prepareRename", sdk_query) is None
                request("textDocument/rename", {**sdk_query, "newName": "sdk_changed"}, error=True)
                client.send("textDocument/didClose", {"textDocument": {"uri": sdk_uri}})
                result["checks"].append("SDK-source-remains-read-only-after-didOpen")
            # Disk changes, new files and unsaved imports invalidate semantic facts.
            shipping = workspace / "src" / "shipping.vyx"
            shipping.write_text("module editor.shipping;\npublic fn shipping() -> i32 { return editor.catalog.price(4); }\n", encoding="utf-8")
            references("catalog.vyx", "fn price", "price", price + [
                ("shipping.vyx", "price(4)", "price", 0)])
            shipping.unlink()
            references("catalog.vyx", "fn price", "price", price)
            pricing_uri = (workspace / "src" / "pricing.vyx").as_uri()
            overlay = source("pricing.vyx") + "\npublic fn unsaved_order(value: i32) -> i32 { return calculate(value); }\n"
            client.send("textDocument/didOpen", {"textDocument": {
                "uri": pricing_uri, "languageId": "vyx", "version": 41, "text": overlay}})
            p = params("catalog.vyx", "fn price", "price")
            p["context"] = {"includeDeclaration": True}
            assert len(request("textDocument/references", p)) == len(price) + 1
            edit = rename("catalog.vyx", "fn price", "price", "temporary_quote")
            assert next(d for d in edit["documentChanges"] if path_from_uri(d["textDocument"]["uri"]).name == "pricing.vyx")["textDocument"]["version"] == 41
            client.send("textDocument/didChange", {"textDocument": {"uri": pricing_uri, "version": 40}, "contentChanges": [{"text": ""}]})
            assert len(request("textDocument/references", p)) == len(price) + 1
            client.send("textDocument/didChange", {"textDocument": {"uri": pricing_uri, "version": 42}, "contentChanges": [{"text": "module editor.pricing;\npublic fn broken( {"}]})
            assert request("textDocument/references", {"textDocument": {"uri": pricing_uri}, "position": {"line": 1, "character": 11}, "context": {"includeDeclaration": True}}) == []
            client.send("textDocument/didClose", {"textDocument": {"uri": pricing_uri}})
            references("catalog.vyx", "fn price", "price", price)
            result["checks"].append("unsaved-versioned-edits-stale-versions-parse-errors-close-and-new-files")
            for word in ("fn", "a.b", "a b", "", "1bad"):
                rename("catalog.vyx", "fn price", "price", word, error=True)
            rename("pricing.vyx", "let price", "price", "surcharge", error=True)
            rename("pricing.vyx", "let price", "price", "record", error=True)
            rename("catalog.vyx", "class Item", "Item", "Product", error=True)
            # Apply real edits across imports, nominal annotations, instantiated
            # generic arguments, enum constructors, fields, methods and source parts.
            for file, needle, word, replacement in [
                ("catalog.vyx", "fn price", "price", "quote_price"),
                ("pricing.vyx", "use calculate", "calculate", "estimate"),
                ("catalog.vyx", "class Item", "Item", "StockItem"),
                ("catalog.vyx", "value: i32", "value", "quantity"),
                ("catalog.vyx", "fn amount", "amount", "count"),
                ("catalog.vyx", "enum Status", "Status", "State"),
                ("catalog.vyx", "Ready, Pending", "Ready", "Available"),
                ("shared_a.vyx", "fn supplement", "supplement", "add_delivery"),
                ("catalog.vyx", "identity<T>", "T", "ValueType"),
                ("catalog.vyx", "class Slot<T>", "T", "SlotValueType"),
                ("catalog.vyx", "fn scale", "scale", "scale_order"),
                ("catalog.vyx", "var requests", "requests", "order_count"),
                ("pricing.vyx", "let captured", "captured", "preview_base"),
                ("catalog.vyx", "module editor.catalog", "catalog", "warehouse"),
            ]:
                rename(file, needle, word, replacement, apply=True)
            unchanged_audit = source("audit.vyx")
            assert "class Item" in unchanged_audit and "value: i32" in unchanged_audit and "fn amount" in unchanged_audit
            assert '"price calculate Item value"' in source("pricing.vyx")
            built = subprocess.run([str(compiler), "build", "-j2"], cwd=workspace,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=180)
            assert built.returncode == 0, built.stdout.decode("utf-8", errors="replace")
            executable = workspace / "target" / ("editor_semantic_rename.exe" if pathlib.os.name == "nt" else "editor_semantic_rename")
            ran = subprocess.run([str(executable)], cwd=workspace, timeout=30)
            assert ran.returncode == 0, ran.returncode
            result["checks"].append("renamed-project:AOT-build-and-run")
            request("shutdown", {})
            client.send("exit")
            assert client.p.wait(timeout=10) == 0
            client.close()
            # Editors without documentChanges must receive ordinary URI-keyed
            # edits, with the same binding validation and cross-file coverage.
            client = Client(lsp, workspace)
            serial = 0
            request("initialize", {"rootUri": workspace.as_uri(), "capabilities": {}})
            client.send("initialized", {})
            edit = request("textDocument/rename", {
                **params("catalog.vyx", "fn quote_price", "quote_price"),
                "newName": "quote_price_legacy_client"})
            assert set(edit) == {"changes"} and len(edit["changes"]) >= 2, edit
            assert all(path_from_uri(key).is_relative_to(workspace) for key in edit["changes"])
            result["checks"].append("legacy-client:URI-keyed-changes")
            request("shutdown", {})
            client.send("exit")
            assert client.p.wait(timeout=10) == 0
            result["peak_mib"] = max(samples)
            result["status"] = "passed"
        except BaseException as exc:
            result["error"] = repr(exc)
            result["stderr_tail"] = list(client.stderr)
            snapshot = args.result.resolve().with_suffix("").with_name(args.result.stem + "-sources")
            shutil.copytree(workspace / "src", snapshot / "src", dirs_exist_ok=True)
            shutil.copy2(workspace / "Vyx.toml", snapshot / "Vyx.toml")
            result["failure_sources"] = str(snapshot)
            raise
        finally:
            client.close()
            args.result.parent.mkdir(parents=True, exist_ok=True)
            args.result.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
