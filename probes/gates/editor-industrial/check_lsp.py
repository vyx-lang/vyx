"""Check editor synchronization, escaped JSON and UTF-16 positions over stdio."""
import argparse
import pathlib
import tempfile

from measure_lsp import Client


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--lsp', type=pathlib.Path, required=True)
    parser.add_argument('--workspace', type=pathlib.Path, required=True)
    args = parser.parse_args()
    workspace = args.workspace.resolve()
    client = Client(args.lsp.resolve(), workspace)
    samples = []
    wait = lambda predicate: client.wait(predicate, 60, 1536, samples)
    serial = 0

    def request(method, params):
        nonlocal serial
        serial += 1
        client.send(method, params, serial)
        return wait(lambda m: m.get('id') == serial)['result']

    temporary = tempfile.TemporaryDirectory(prefix='vyx-lsp-sync-')
    temp_root = pathlib.Path(temporary.name).resolve()
    assert temp_root.is_relative_to(pathlib.Path(tempfile.gettempdir()).resolve())
    disk_file = temp_root / '界面 + synchronization.vyx'
    disk_file.write_text('fn disk_version() -> i32 { return 0; }', encoding='utf-8')
    uri = disk_file.as_uri()
    document = {'uri': uri}
    text = 'fn main() -> i32 { /*中文🙂 "uri": "fake"*/ let old_name: i32 = 1; return old_name; }\n'
    try:
        request('initialize', {'rootUri': workspace.as_uri(), 'capabilities': {}})
        client.send('initialized', {})
        client.send('textDocument/didOpen', {'textDocument': {
            **document, 'languageId': 'vyx', 'version': 1, 'text': text}})
        wait(lambda m: m.get('method') == 'textDocument/publishDiagnostics'
             and m.get('params', {}).get('uri') == uri)

        positions = []
        start = 0
        while (start := text.find('old_name', start)) >= 0:
            col = len(text[:start].encode('utf-16-le')) // 2
            positions.append(col)
            start += len('old_name')
        assert len(positions) == 2
        client.send('textDocument/didChange', {'textDocument': {**document, 'version': 3},
            'contentChanges': [{'range': {'start': {'line': 0, 'character': col},
                'end': {'line': 0, 'character': col + 8}}, 'text': 'new_name'} for col in positions]})
        changed = wait(lambda m: m.get('method') == 'textDocument/publishDiagnostics'
                       and m.get('params', {}).get('uri') == uri)
        assert not changed['params']['diagnostics'], changed
        assert changed['params']['version'] == 3, changed
        symbols = request('textDocument/documentSymbol', {'textDocument': document})
        assert any(s['name'] == 'main' for s in symbols), symbols
        definition = request('textDocument/definition', {'textDocument': document,
            'position': {'line': 0, 'character': positions[1] + 1}})
        location = definition[0] if isinstance(definition, list) else definition
        assert location and location['targetUri'] == uri, definition
        assert location['targetSelectionRange']['start'] == {'line': 0, 'character': positions[0]}, location

        # A request after a stale notification is a processing barrier. A stale
        # change must neither erase symbols nor replace the current diagnostic.
        client.send('textDocument/didChange', {'textDocument': {**document, 'version': 2},
            'contentChanges': [{'text': 'fn stale_only() -> i32 { return 0; }'}]})
        assert request('textDocument/documentSymbol', {'textDocument': document}) == symbols
        for _ in range(50):
            assert request('textDocument/diagnostic', {'textDocument': document})['items'] == []

        signature_document = {'uri': (temp_root / 'signature.vyx').as_uri()}
        signature_source = ('fn probe_sum(a: i32, b: i32) -> i32 { return a + b; }\n'
            'fn main() -> i32 { /*中文🙂*/ return probe_sum(1, 2); }\n')
        client.send('textDocument/didOpen', {'textDocument': {**signature_document,
            'languageId': 'vyx', 'version': 1, 'text': signature_source}})
        call_line = signature_source.splitlines()[1]
        call_column = len(call_line[:call_line.index('2);')].encode('utf-16-le')) // 2
        signature = request('textDocument/signatureHelp', {'textDocument': signature_document,
            'position': {'line': 1, 'character': call_column}})
        assert signature and signature['activeParameter'] == 1, signature
        assert 'probe_sum' in signature['signatures'][0]['label'], signature
        assert request('textDocument/signatureHelp', {'textDocument': signature_document,
            'position': {'line': 1, 'character': 0}}) is None
        client.send('textDocument/didClose', {'textDocument': signature_document})

        client.send('textDocument/didChange', {'textDocument': {**document, 'version': 4},
            'contentChanges': [{'text': ''}]})
        wait(lambda m: m.get('method') == 'textDocument/publishDiagnostics'
             and m.get('params', {}).get('uri') == uri)
        assert request('textDocument/documentSymbol', {'textDocument': document}) == []
        assert request('textDocument/semanticTokens/full', {'textDocument': document})['data'] == []
        assert request('textDocument/signatureHelp', {'textDocument': document,
            'position': {'line': 0, 'character': 0}}) is None
        client.send('textDocument/didChange', {'textDocument': {**document, 'version': 5},
            'contentChanges': [{'range': {'start': {'line': 0, 'character': 0},
                'end': {'line': 0, 'character': 0}}, 'text': 'fn buffer_only() -> i32 { return 0; }'}]})
        wait(lambda m: m.get('method') == 'textDocument/publishDiagnostics'
             and m.get('params', {}).get('uri') == uri)
        assert [s['name'] for s in request('textDocument/documentSymbol', {'textDocument': document})] == ['buffer_only']
        client.send('textDocument/didSave', {'textDocument': document, 'text': ''})
        wait(lambda m: m.get('method') == 'textDocument/publishDiagnostics'
             and m.get('params', {}).get('uri') == uri)
        assert request('textDocument/documentSymbol', {'textDocument': document}) == []
        client.send('textDocument/didClose', {'textDocument': document})
        # Several buffers share a server, but analysis storage and versions must
        # remain independent after request cleanup and vector growth/removal.
        buffers = [(temp_root / f'buffer-{i}.vyx').as_uri() for i in range(50)]
        for i, buffer in enumerate(buffers):
            client.send('textDocument/didOpen', {'textDocument': {'uri': buffer,
                'languageId': 'vyx', 'version': 1,
                'text': f'fn opened_{i}() -> i32 {{ let value: i32 = {i}; return value; }}'}})
        for i in reversed(range(len(buffers))):
            buffer = {'uri': buffers[i]}
            names = [s['name'] for s in request('textDocument/documentSymbol', {'textDocument': buffer})]
            assert f'opened_{i}' in names, names
            client.send('textDocument/didChange', {'textDocument': {**buffer, 'version': 2},
                'contentChanges': [{'text': f'fn changed_{i}() -> i32 {{ return {i}; }}'}]})
            names = [s['name'] for s in request('textDocument/documentSymbol', {'textDocument': buffer})]
            assert f'changed_{i}' in names and f'opened_{i}' not in names, names
            client.send('textDocument/didClose', {'textDocument': buffer})
        request('shutdown', {})
        client.send('exit')
        client.p.wait(timeout=10)
        assert client.p.returncode == 0
        print('LSP_SYNC=PASS incremental UTF-16 JSON-escaping stale-version empty-buffer pull-diagnostics shutdown')
    finally:
        client.close()
        temporary.cleanup()


if __name__ == '__main__':
    main()
