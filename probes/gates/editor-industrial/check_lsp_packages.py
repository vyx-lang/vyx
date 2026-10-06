"""Verify one standard provider, its module fragments and SDK read-only bindings."""
import argparse
import hashlib
import json
import os
import pathlib
import tempfile

from measure_lsp import Client
from check_lsp_semantic import path_from_uri, position


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--lsp', required=True, type=pathlib.Path)
    ap.add_argument('--result', required=True, type=pathlib.Path)
    args = ap.parse_args()
    exe = args.lsp.resolve()
    result = {'status': 'failed', 'lsp': str(exe),
              'lsp_sha256': hashlib.sha256(exe.read_bytes()).hexdigest(), 'checks': []}
    samples = []
    previous = os.environ.pop('VYX_STD_PACKAGES', None)
    try:
        with tempfile.TemporaryDirectory(prefix='vyx-package-bindings-') as temporary:
            base = pathlib.Path(temporary).resolve()
            workspace = base / 'workspace'
            workspace.mkdir()
            main_file = workspace / 'main.vyx'
            main_text = ('module application;\nuse std.editor_provider;\n'
                         'public fn main() -> i32 { return std.editor_provider.selected()'
                         ' + std.editor_provider.helper(); }\n')
            main_file.write_text(main_text, encoding='utf-8')
            def provider(root, value):
                src = root / 'editor' / 'src'
                src.mkdir(parents=True)
                selected, helper = src / 'primary.vyx', src / 'extension.vyx'
                selected.write_text(f'module std.editor_provider;\npublic fn selected() -> i32 {{ return {value}; }}\n', encoding='utf-8')
                helper.write_text('module std.editor_provider;\npublic fn helper() -> i32 { return 7; }\n', encoding='utf-8')
                (root / 'registry').write_text('editor/src/primary.vyx\neditor/src/extension.vyx\n', encoding='utf-8')
                return selected, helper
            local = provider(workspace / 'std_packages', 11)
            override_root = base / 'override_packages'
            override = provider(override_root, 22)
            legacy = workspace / 'std'
            legacy.mkdir()
            (legacy / 'editor.vyx').write_text('module std.editor_provider;\npublic fn selected() -> i32 { return 99; }\n', encoding='utf-8')
            for explicit, expected in ((False, local), (True, override)):
                if explicit:
                    os.environ['VYX_STD_PACKAGES'] = str(override_root)
                client = Client(exe, workspace)
                serial = 0
                def request(method, params):
                    nonlocal serial
                    serial += 1
                    client.send(method, params, serial)
                    return client.wait(lambda m: m.get('id') == serial, 120, 1536, samples)['result']
                try:
                    request('initialize', {'rootUri': workspace.as_uri(), 'capabilities': {
                        'workspace': {'workspaceEdit': {'documentChanges': True}}}})
                    client.send('initialized', {})
                    for word, declaration in zip(('selected', 'helper'), expected):
                        params = {'textDocument': {'uri': main_file.as_uri()},
                                  'position': position(main_text, word + '()', word)}
                        found = request('textDocument/references', {**params, 'context': {'includeDeclaration': True}})
                        actual = [path_from_uri(item['uri']) for item in found]
                        assert sorted(actual) == sorted((main_file, declaration)), (explicit, word, found)
                        prepared = request('textDocument/prepareRename', params)
                        if explicit:
                            assert prepared is None, prepared
                        else:
                            assert prepared, prepared
                            edit = request('textDocument/rename', {**params, 'newName': word + '_verified'})
                            edited = {path_from_uri(item['textDocument']['uri']) for item in edit['documentChanges']}
                            assert edited == {main_file, declaration}, edit
                    result['checks'].append('explicit-provider-read-only' if explicit else 'workspace-provider-module-fragments-and-legacy-exclusion')
                    request('shutdown', {})
                    client.send('exit')
                    assert client.p.wait(timeout=10) == 0
                except BaseException:
                    result['stderr_tail'] = list(client.stderr)
                    raise
                finally:
                    client.close()
            result.update(status='passed', peak_mib=max(samples))
    except BaseException as exc:
        result['error'] = repr(exc)
        raise
    finally:
        if previous is not None:
            os.environ['VYX_STD_PACKAGES'] = previous
        else:
            os.environ.pop('VYX_STD_PACKAGES', None)
        args.result.parent.mkdir(parents=True, exist_ok=True)
        args.result.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
