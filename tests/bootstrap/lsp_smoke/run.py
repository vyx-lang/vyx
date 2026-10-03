#!/usr/bin/env python3
import argparse
import json
import pathlib
import queue
import shutil
import subprocess
import tempfile
import threading


class LspClient:
    def __init__(self, executable: pathlib.Path, cwd: pathlib.Path):
        self.process = subprocess.Popen(
            [str(executable), "--stdio"],
            cwd=cwd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        self.responses = queue.Queue()
        threading.Thread(target=self._read_loop, daemon=True).start()

    def _read_loop(self):
        try:
            while True:
                length = None
                while True:
                    line = self.process.stdout.readline()
                    if not line:
                        raise EOFError("LSP stdout closed")
                    if line in (b"\r\n", b"\n"):
                        break
                    if line.lower().startswith(b"content-length:"):
                        length = int(line.split(b":", 1)[1])
                if length is None:
                    raise RuntimeError("missing Content-Length")
                body = self.process.stdout.read(length)
                if len(body) != length:
                    raise EOFError("short LSP response")
                self.responses.put(json.loads(body))
        except BaseException as exc:
            self.responses.put(exc)

    def send(self, message):
        body = json.dumps(message, separators=(",", ":")).encode("utf-8")
        self.process.stdin.write(f"Content-Length: {len(body)}\r\n\r\n".encode("ascii"))
        self.process.stdin.write(body)
        self.process.stdin.flush()

    def receive(self, timeout=15):
        value = self.responses.get(timeout=timeout)
        if isinstance(value, BaseException):
            raise value
        return value

    def receive_response(self, request_id, timeout=15):
        while True:
            message = self.receive(timeout)
            if message.get("id") == request_id:
                return message

    def wait_diagnostics(self, uri, timeout=15):
        while True:
            message = self.receive(timeout)
            if message.get("method") != "textDocument/publishDiagnostics":
                continue
            params = message.get("params") or {}
            if params.get("uri") == uri:
                return params.get("diagnostics") or []

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait(timeout=3)


def run_once(executable: pathlib.Path, iteration: int):
    # Exercise URI percent encoding used by real Windows workspaces such as
    # `E:/Dev/C++/...`; both '+' and spaces must round-trip through rootUri and
    # symbol/document URIs or cross-module completion silently disappears.
    case = pathlib.Path(tempfile.gettempdir()) / f"vyx+lsp smoke_{iteration}"
    shutil.rmtree(case, ignore_errors=True)
    case.mkdir(parents=True)
    source = (
        "module smoke;\n"
        "use Add;\n"
        "fn add(a: i32, b: i32) -> i32 { return a + b; }\n"
        "fn main() -> i32 { let t: Test; var cc = t.add(1, 2, 3); let x = add(1, 2); print(cc); return x; }\n"
    )
    interface_path = case / "add.vyi"
    interface_path.write_text(
        "module Add;\n"
        "public class Add.Test {\n"
        "    public n: i32;\n"
        "    public fn add(self: Add.Test, a: i32, b: i32, c: i32) -> i32;\n"
        "}\n",
        encoding="utf-8",
    )
    (case / "add_legacy.vyi").write_text(
        "module Add;\n"
        "public class Add.Test {\n"
        "    public fn add(self: Add.Test, a: i32, b: i32) -> i32;\n"
        "}\n",
        encoding="utf-8",
    )
    source_path = case / "main.vyx"
    source_path.write_text(source, encoding="utf-8")
    uri = source_path.as_uri()
    client = LspClient(executable, case)
    try:
        # LSP4J/JetBrains emits JSON-RPC request IDs as strings.  The server
        # must preserve the ID representation exactly in every response.
        client.send({"jsonrpc": "2.0", "id": "1", "method": "initialize", "params": {"rootUri": case.as_uri()}})
        initialize = client.receive_response("1")
        assert initialize["id"] == "1", initialize
        capabilities = initialize["result"]["capabilities"]
        assert capabilities["semanticTokensProvider"]
        assert capabilities["documentSymbolProvider"]
        assert capabilities["signatureHelpProvider"]
        assert capabilities["documentHighlightProvider"]
        assert capabilities["workspaceSymbolProvider"]
        assert capabilities["codeActionProvider"]
        assert capabilities["diagnosticProvider"]
        sync = capabilities["textDocumentSync"]
        assert isinstance(sync, dict), sync
        assert sync.get("change") == 2, sync
        assert (sync.get("save") or {}).get("includeText") is True, sync

        client.send({
            "jsonrpc": "2.0",
            "method": "textDocument/didOpen",
            "params": {"textDocument": {"uri": uri, "languageId": "vyx", "version": 1, "text": source}},
        })

        main_line_number = 3
        main_line = source.splitlines()[main_line_number]
        completion_character = main_line.rindex("add") + len("add")
        client.send({
            "jsonrpc": "2.0",
            "id": "2",
            "method": "textDocument/completion",
            "params": {"textDocument": {"uri": uri}, "position": {"line": main_line_number, "character": completion_character}},
        })
        completion = client.receive_response("2")
        assert completion["id"] == "2", completion
        add_item = next(item for item in completion["result"]["items"] if item["label"] == "add")
        assert add_item["insertText"] == "add(${1:a}, ${2:b})$0"
        assert add_item["insertTextFormat"] == 2

        member_character = main_line.index("t.add") + len("t.")
        client.send({
            "jsonrpc": "2.0",
            "id": "20",
            "method": "textDocument/completion",
            "params": {
                "textDocument": {"uri": uri},
                "position": {"line": main_line_number, "character": member_character},
            },
        })
        member_items = client.receive_response("20")["result"]["items"]
        member_labels = {item["label"] for item in member_items}
        assert {"add", "n"}.issubset(member_labels), member_items
        assert "fn" not in member_labels and "class" not in member_labels, member_items
        member_add = next(item for item in member_items if item["label"] == "add")
        assert "i32" in member_add["detail"], member_add
        add_overloads = [item for item in member_items if item["label"] == "add"]
        assert len(add_overloads) == 2, add_overloads
        assert {item["insertText"] for item in add_overloads} == {
            "add(${1:a}, ${2:b})$0",
            "add(${1:a}, ${2:b}, ${3:c})$0",
        }, add_overloads

        # A valid modern header can be newer than the LSP light parser.  The
        # lexer fallback must still retain function identity for value
        # references and version-qualified calls instead of painting them as
        # generic variables.
        advanced_source = (
            '@[version("1.0.0")]\n'
            '@[variant("release")]\n'
            "module advanced;\n"
            'use Add@2.0.0["_1"];\n'
            "fn func(a: i32) -> i32 { return a + 1; }\n"
            'fn main() -> i32 { let f: fn(i32) -> i32 = func; return add@1.0.0["_1"](f(1), 2); }\n'
        )
        advanced_path = case / "advanced.vyx"
        advanced_path.write_text(advanced_source, encoding="utf-8")
        advanced_uri = advanced_path.as_uri()
        client.send({
            "jsonrpc": "2.0",
            "method": "textDocument/didOpen",
            "params": {
                "textDocument": {
                    "uri": advanced_uri,
                    "languageId": "vyx",
                    "version": 1,
                    "text": advanced_source,
                }
            },
        })
        client.send({
            "jsonrpc": "2.0",
            "id": "22",
            "method": "textDocument/semanticTokens/full",
            "params": {"textDocument": {"uri": advanced_uri}},
        })
        advanced_data = client.receive_response("22")["result"]["data"]
        advanced_tokens = []
        advanced_line = 0
        advanced_character = 0
        advanced_lines = advanced_source.splitlines()
        for offset in range(0, len(advanced_data), 5):
            delta_line, delta_character, length, token_type, modifiers = advanced_data[offset:offset + 5]
            advanced_line += delta_line
            advanced_character = (
                delta_character if delta_line else advanced_character + delta_character
            )
            token_text = advanced_lines[advanced_line][advanced_character:advanced_character + length]
            advanced_tokens.append((advanced_line, advanced_character, token_text, token_type, modifiers))
        advanced_body_line = 5
        assert any(
            line == 4 and token_text == "func" and token_type == 11 and modifiers & 1
            for line, _, token_text, token_type, modifiers in advanced_tokens
        ), advanced_tokens
        assert any(
            line == advanced_body_line and token_text == "func" and token_type == 11
            and not (modifiers & 1)
            for line, _, token_text, token_type, modifiers in advanced_tokens
        ), advanced_tokens
        assert any(
            line == advanced_body_line and token_text == "add" and token_type == 11
            for line, _, token_text, token_type, _ in advanced_tokens
        ), advanced_tokens

        client.send({
            "jsonrpc": "2.0",
            "id": "23",
            "method": "textDocument/documentSymbol",
            "params": {"textDocument": {"uri": advanced_uri}},
        })
        advanced_symbols = client.receive_response("23")["result"]
        assert {"func", "main"}.issubset({symbol["name"] for symbol in advanced_symbols}), advanced_symbols

        hover_character = main_line.index("print(cc)") + len("print(c")
        client.send({
            "jsonrpc": "2.0",
            "id": "21",
            "method": "textDocument/hover",
            "params": {
                "textDocument": {"uri": uri},
                "position": {"line": main_line_number, "character": hover_character},
            },
        })
        hover = client.receive_response("21")["result"]
        assert hover is not None and "var cc: i32" in hover["contents"]["value"], hover

        definition_character = main_line.rindex("add(1, 2)")
        client.send({
            "jsonrpc": "2.0",
            "id": "24",
            "method": "textDocument/definition",
            "params": {
                "textDocument": {"uri": uri},
                "position": {"line": main_line_number, "character": definition_character},
            },
        })
        definition = client.receive_response("24")["result"]
        # CLion copies a plain Location range into targetSelectionRange and then
        # drops a goto whose selection is the same word as the caret. A link
        # keeps the caret as the origin and the declaration body as the target.
        assert definition is not None and "targetUri" in definition, definition
        assert definition["targetUri"] == uri, definition
        target = definition["targetRange"]
        assert target["start"]["line"] == 2, definition
        assert (target["end"]["line"], target["end"]["character"]) > (
            target["start"]["line"], target["start"]["character"]
        ), definition
        assert definition["originSelectionRange"]["start"]["line"] == main_line_number, definition
        assert definition["targetSelectionRange"]["start"] != definition["originSelectionRange"]["start"], definition

        client.send({
            "jsonrpc": "2.0",
            "id": "35",
            "method": "textDocument/references",
            "params": {
                "textDocument": {"uri": uri},
                "position": {"line": main_line_number, "character": definition_character},
                "context": {"includeDeclaration": True},
            },
        })
        references = client.receive_response("35")["result"] or []
        reference_lines = {item["range"]["start"]["line"] for item in references}
        assert 2 in reference_lines, references
        assert main_line_number in reference_lines, references
        assert len(references) >= 2, references

        incomplete = source.replace("add(1, 2);", "add(")
        client.send({
            "jsonrpc": "2.0",
            "method": "textDocument/didChange",
            "params": {"textDocument": {"uri": uri, "version": 2}, "contentChanges": [{"text": incomplete}]},
        })
        client.send({
            "jsonrpc": "2.0",
            "id": "3",
            "method": "textDocument/completion",
            "params": {"textDocument": {"uri": uri}, "position": {"line": main_line_number, "character": completion_character}},
        })
        incomplete_completion = client.receive_response("3")
        assert any(item["label"] == "add" for item in incomplete_completion["result"]["items"])

        unsaved = source.replace("fn add(", "fn unsavedSum(").replace("add(1, 2)", "unsavedSum(1, 2)")
        unsaved_main_line = unsaved.splitlines()[main_line_number]
        client.send({
            "jsonrpc": "2.0",
            "method": "textDocument/didChange",
            "params": {"textDocument": {"uri": uri, "version": 3}, "contentChanges": [{"text": unsaved}]},
        })
        client.send({
            "jsonrpc": "2.0",
            "id": "4",
            "method": "textDocument/semanticTokens/full",
            "params": {"textDocument": {"uri": uri}},
        })
        semantic_data = client.receive_response("4")["result"]["data"]
        assert semantic_data and len(semantic_data) % 5 == 0
        semantic_tokens = []
        semantic_line = 0
        semantic_character = 0
        for offset in range(0, len(semantic_data), 5):
            delta_line, delta_character, length, token_type, modifiers = semantic_data[offset:offset + 5]
            semantic_line += delta_line
            semantic_character = delta_character if delta_line else semantic_character + delta_character
            semantic_tokens.append((semantic_line, semantic_character, length, token_type, modifiers))
        assert any(
            line == 2 and character == 3 and length == len("unsavedSum")
            and token_type == 11 and modifiers & 1
            for line, character, length, token_type, modifiers in semantic_tokens
        ), semantic_tokens
        member_add_col = unsaved_main_line.index("t.add") + len("t.")
        assert any(
            line == main_line_number and character == member_add_col
            and length == len("add") and token_type == 12
            for line, character, length, token_type, _ in semantic_tokens
        ), semantic_tokens
        cc_col = unsaved_main_line.index("cc")
        assert any(
            line == main_line_number and character == cc_col
            and length == len("cc") and token_type == 8
            for line, character, length, token_type, _ in semantic_tokens
        ), semantic_tokens

        client.send({
            "jsonrpc": "2.0",
            "id": "5",
            "method": "textDocument/documentSymbol",
            "params": {"textDocument": {"uri": uri}},
        })
        symbols = client.receive_response("5")["result"]
        assert any(symbol["name"] == "unsavedSum" for symbol in symbols)

        signature_character = unsaved_main_line.rindex("2") + 1
        client.send({
            "jsonrpc": "2.0",
            "id": "6",
            "method": "textDocument/signatureHelp",
            "params": {"textDocument": {"uri": uri}, "position": {"line": main_line_number, "character": signature_character}},
        })
        signature = client.receive_response("6")["result"]
        assert signature is not None
        assert signature["activeParameter"] == 1, signature
        assert "unsavedSum(" in signature["signatures"][0]["label"]

        parse_src = "fn main() -> i32 {\n    return 0\n}\n"
        parse_path = case / "missing_semi.vyx"
        parse_path.write_text(parse_src, encoding="utf-8")
        parse_uri = parse_path.as_uri()
        client.send({
            "jsonrpc": "2.0",
            "method": "textDocument/didOpen",
            "params": {
                "textDocument": {
                    "uri": parse_uri,
                    "languageId": "vyx",
                    "version": 1,
                    "text": parse_src,
                }
            },
        })
        parse_diags = client.wait_diagnostics(parse_uri)
        assert parse_diags, parse_diags
        parse_codes = {item.get("code") for item in parse_diags}
        assert parse_codes & {"E0001", "E0002"}, parse_diags
        parse_range = parse_diags[0]["range"]
        assert parse_range["end"]["character"] > parse_range["start"]["character"] or (
            parse_range["end"]["line"] > parse_range["start"]["line"]
        ), parse_range

        unknown_src = "fn main() -> i32 {\n    return does_not_exist;\n}\n"
        unknown_path = case / "unknown_name.vyx"
        unknown_path.write_text(unknown_src, encoding="utf-8")
        unknown_uri = unknown_path.as_uri()
        client.send({
            "jsonrpc": "2.0",
            "method": "textDocument/didOpen",
            "params": {
                "textDocument": {
                    "uri": unknown_uri,
                    "languageId": "vyx",
                    "version": 1,
                    "text": unknown_src,
                }
            },
        })
        unknown_diags = client.wait_diagnostics(unknown_uri)
        unknown_item = next(
            item for item in unknown_diags if item.get("code") == "E2000"
        )
        unknown_width = (
            unknown_item["range"]["end"]["character"]
            - unknown_item["range"]["start"]["character"]
        )
        assert unknown_width >= len("does_not_exist"), unknown_item

        client.send({
            "jsonrpc": "2.0",
            "id": "25",
            "method": "textDocument/codeAction",
            "params": {
                "textDocument": {"uri": parse_uri},
                "range": {"start": {"line": 0, "character": 0}, "end": {"line": 2, "character": 1}},
                "context": {"diagnostics": parse_diags},
            },
        })
        parse_actions = client.receive_response("25")["result"] or []
        help_titles = [
            action.get("title", "")
            for action in parse_actions
            if "help:" in action.get("title", "").lower() or "建议:" in action.get("title", "")
        ]
        assert not help_titles, parse_actions
        insert_actions = [action for action in parse_actions if action.get("title") == "Insert ';'"]
        assert insert_actions, parse_actions
        insert_edit = insert_actions[0]["edit"]["changes"][parse_uri][0]
        assert insert_edit["newText"] == ";"
        assert insert_edit["range"]["start"]["line"] == 1, insert_edit
        assert insert_edit["range"]["start"]["character"] == len("    return 0"), insert_edit

        highlight_src = (
            "fn add(a: i32, b: i32) -> i32 { return a + b; }\n"
            "fn main() -> i32 { return add(1, 2); }\n"
        )
        highlight_path = case / "highlight.vyx"
        highlight_path.write_text(highlight_src, encoding="utf-8")
        highlight_uri = highlight_path.as_uri()
        client.send({
            "jsonrpc": "2.0",
            "method": "textDocument/didOpen",
            "params": {
                "textDocument": {
                    "uri": highlight_uri,
                    "languageId": "vyx",
                    "version": 1,
                    "text": highlight_src,
                }
            },
        })
        client.wait_diagnostics(highlight_uri)
        add_use = highlight_src.splitlines()[1].index("add")
        client.send({
            "jsonrpc": "2.0",
            "id": "26",
            "method": "textDocument/documentHighlight",
            "params": {
                "textDocument": {"uri": highlight_uri},
                "position": {"line": 1, "character": add_use},
            },
        })
        highlights = client.receive_response("26")["result"] or []
        highlight_texts = []
        highlight_lines = highlight_src.splitlines()
        for item in highlights:
            rng = item["range"]
            highlight_texts.append(highlight_lines[rng["start"]["line"]][rng["start"]["character"]:rng["end"]["character"]])
        assert highlight_texts.count("add") >= 2, highlights
        assert {item.get("kind") for item in highlights} <= {1, 2, 3, None}, highlights

        client.send({
            "jsonrpc": "2.0",
            "id": "27",
            "method": "workspace/symbol",
            "params": {"query": "unsavedSum"},
        })
        workspace_symbols = client.receive_response("27")["result"] or []
        assert any(symbol.get("name") == "unsavedSum" for symbol in workspace_symbols), workspace_symbols
        client.send({
            "jsonrpc": "2.0",
            "id": "28",
            "method": "workspace/symbol",
            "params": {"query": "add"},
        })
        add_symbols = client.receive_response("28")["result"] or []
        assert any(symbol.get("name") == "add" for symbol in add_symbols), add_symbols

        extra_src = "fn main() -> i32 {\n    return 0;\n}\n}\n"
        extra_path = case / "extra_brace.vyx"
        extra_path.write_text(extra_src, encoding="utf-8")
        extra_uri = extra_path.as_uri()
        client.send({
            "jsonrpc": "2.0",
            "method": "textDocument/didOpen",
            "params": {
                "textDocument": {
                    "uri": extra_uri,
                    "languageId": "vyx",
                    "version": 1,
                    "text": extra_src,
                }
            },
        })
        extra_diags = client.wait_diagnostics(extra_uri)
        extra_codes = {item.get("code") for item in extra_diags}
        assert extra_codes & {"E0001", "E0002"}, extra_diags

        client.send({
            "jsonrpc": "2.0",
            "id": "29",
            "method": "textDocument/diagnostic",
            "params": {"textDocument": {"uri": extra_uri}},
        })
        pull = client.receive_response("29")["result"]
        assert pull["kind"] == "full", pull
        pull_codes = {item.get("code") for item in pull.get("items") or []}
        assert pull_codes & {"E0001", "E0002"}, pull

        extra_ok = "fn main() -> i32 {\n    return 0;\n}\n"
        client.send({
            "jsonrpc": "2.0",
            "method": "textDocument/didChange",
            "params": {
                "textDocument": {"uri": extra_uri, "version": 2},
                "contentChanges": [{
                    "range": {
                        "start": {"line": 3, "character": 0},
                        "end": {"line": 4, "character": 0},
                    },
                    "text": "",
                }],
            },
        })
        extra_fixed = client.wait_diagnostics(extra_uri)
        extra_fixed_codes = {item.get("code") for item in extra_fixed}
        assert not (extra_fixed_codes & {"E0001", "E0002"}), extra_fixed
        client.send({
            "jsonrpc": "2.0",
            "id": "31",
            "method": "textDocument/documentSymbol",
            "params": {"textDocument": {"uri": extra_uri}},
        })
        extra_symbols = client.receive_response("31")["result"] or []
        main_symbol = next((symbol for symbol in extra_symbols if symbol.get("name") == "main"), None)
        assert main_symbol is not None, extra_symbols
        # CLion sticky lines compare selectionRange.start against range.end and
        # throw when the text offset is not strictly before the line end.
        for symbol in extra_symbols:
            selection = symbol["selectionRange"]
            full = symbol["range"]
            sel_start = (selection["start"]["line"], selection["start"]["character"])
            sel_end = (selection["end"]["line"], selection["end"]["character"])
            full_end = (full["end"]["line"], full["end"]["character"])
            assert sel_start < sel_end, symbol
            assert sel_start < full_end, symbol
            assert full["start"]["line"] < full["end"]["line"] or (
                full["start"]["line"] == full["end"]["line"]
                and full["start"]["character"] < full["end"]["character"]
            ), symbol
        assert main_symbol["selectionRange"]["end"]["character"] == (
            main_symbol["selectionRange"]["start"]["character"] + len("main")
        ), main_symbol
        client.send({
            "jsonrpc": "2.0",
            "id": "32",
            "method": "textDocument/diagnostic",
            "params": {"textDocument": {"uri": extra_uri}},
        })
        extra_pull_fixed = client.receive_response("32")["result"]
        extra_pull_fixed_codes = {item.get("code") for item in extra_pull_fixed.get("items") or []}
        assert not (extra_pull_fixed_codes & {"E0001", "E0002"}), extra_pull_fixed

        client.send({
            "jsonrpc": "2.0",
            "method": "textDocument/didChange",
            "params": {
                "textDocument": {"uri": extra_uri, "version": 3},
                "contentChanges": [{"text": extra_src}],
            },
        })
        extra_bad_again = client.wait_diagnostics(extra_uri)
        assert {item.get("code") for item in extra_bad_again} & {"E0001", "E0002"}, extra_bad_again
        extra_path.write_text(extra_ok, encoding="utf-8")
        client.send({
            "jsonrpc": "2.0",
            "method": "textDocument/didSave",
            "params": {"textDocument": {"uri": extra_uri}, "text": extra_ok},
        })
        extra_saved = client.wait_diagnostics(extra_uri)
        extra_saved_codes = {item.get("code") for item in extra_saved}
        assert not (extra_saved_codes & {"E0001", "E0002"}), extra_saved
        client.send({
            "jsonrpc": "2.0",
            "id": "33",
            "method": "textDocument/diagnostic",
            "params": {"textDocument": {"uri": extra_uri}},
        })
        extra_pull_saved = client.receive_response("33")["result"]
        extra_pull_saved_codes = {item.get("code") for item in extra_pull_saved.get("items") or []}
        assert not (extra_pull_saved_codes & {"E0001", "E0002"}), extra_pull_saved

        array_src = "fn main() { var a = [1, 2, 3]; print(a[0]); }\n"
        array_path = case / "array_index.vyx"
        array_path.write_text(array_src, encoding="utf-8")
        array_uri = array_path.as_uri()
        client.send({
            "jsonrpc": "2.0",
            "method": "textDocument/didOpen",
            "params": {
                "textDocument": {
                    "uri": array_uri,
                    "languageId": "vyx",
                    "version": 1,
                    "text": array_src,
                }
            },
        })
        array_diags = client.wait_diagnostics(array_uri)
        client.send({
            "jsonrpc": "2.0",
            "id": "30",
            "method": "textDocument/diagnostic",
            "params": {"textDocument": {"uri": array_uri}},
        })
        array_pull = client.receive_response("30")["result"]
        assert array_pull["kind"] == "full", array_pull

        # Sema notes can carry a 1-based column past the end of a short line.
        # CLion clamps both ends onto the same document offset and then rejects
        # the sticky line. Every emitted symbol must stay strictly ordered
        # after that clamp.
        sticky_src = (
            "struct Pair<T> {\n"
            "    value: T;\n"
            "}\n"
            "fn make() {\n"
            "    return Pair<i32>{value: 1};\n"
            "}\n"
        )
        sticky_path = case / "sticky_symbols.vyx"
        sticky_path.write_text(sticky_src, encoding="utf-8")
        sticky_uri = sticky_path.as_uri()
        client.send({
            "jsonrpc": "2.0",
            "method": "textDocument/didOpen",
            "params": {
                "textDocument": {
                    "uri": sticky_uri,
                    "languageId": "vyx",
                    "version": 1,
                    "text": sticky_src,
                }
            },
        })
        client.wait_diagnostics(sticky_uri)
        client.send({
            "jsonrpc": "2.0",
            "id": "34",
            "method": "textDocument/documentSymbol",
            "params": {"textDocument": {"uri": sticky_uri}},
        })
        sticky_symbols = client.receive_response("34")["result"] or []
        assert sticky_symbols, sticky_symbols
        sticky_lines = sticky_src.splitlines()

        def sticky_offset(line, character):
            if line < 0 or line >= len(sticky_lines):
                return None
            base = sum(len(item) + 1 for item in sticky_lines[:line])
            return base + min(max(character, 0), len(sticky_lines[line]))

        for symbol in sticky_symbols:
            selection = symbol["selectionRange"]
            full = symbol["range"]
            start = sticky_offset(selection["start"]["line"], selection["start"]["character"])
            selection_end = sticky_offset(selection["end"]["line"], selection["end"]["character"])
            full_end = sticky_offset(full["end"]["line"], full["end"]["character"])
            assert start is not None and selection_end is not None and full_end is not None, symbol
            assert start < selection_end and start < full_end, symbol

        client.send({"jsonrpc": "2.0", "id": "7", "method": "shutdown", "params": {}})
        client.receive_response("7")
        client.send({"jsonrpc": "2.0", "method": "exit", "params": {}})
        client.process.stdin.close()
        return_code = client.process.wait(timeout=3)
        stderr = client.process.stderr.read()
        assert return_code == 0
        assert not stderr
        print(
            f"run={iteration} completion={add_item['insertText']} "
            f"members={','.join(sorted(member_labels))} hover=cc:i32 "
            f"semanticInts={len(semantic_data)} symbols={len(symbols)} "
            f"definition=add parseDiags={len(parse_diags)} unknownWidth={unknown_width} "
            f"codeActions={len(insert_actions)} highlights={len(highlights)} workspace={len(workspace_symbols)} "
            f"extraBrace={len(extra_diags)} pull={len(pull.get('items') or [])} arrayDiags={len(array_diags)} "
            f"advancedFunctions=PASS active={signature['activeParameter']} unsavedBuffer=PASS "
            f"liveUpdate=PASS"
        )
    finally:
        client.close()
        shutil.rmtree(case, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("executable", type=pathlib.Path)
    parser.add_argument("--runs", type=int, default=3)
    args = parser.parse_args()
    executable = args.executable.resolve()
    for iteration in range(1, args.runs + 1):
        run_once(executable, iteration)
    print("LSP_SMOKE=PASS")


if __name__ == "__main__":
    main()
