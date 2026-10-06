"""Measure binding queries against the real SDK compiler project."""
import argparse
import hashlib
import json
import pathlib
import statistics
import time

from measure_lsp import Client
from check_lsp_semantic import position, path_from_uri


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--lsp', required=True, type=pathlib.Path)
    ap.add_argument('--workspace', required=True, type=pathlib.Path)
    ap.add_argument('--document', required=True, type=pathlib.Path)
    ap.add_argument('--needle', default='public fn parse_unit')
    ap.add_argument('--symbol', default='parse_unit')
    ap.add_argument('--rename-symbol', default='parser_auto_qualify_std_modules')
    ap.add_argument('--rename-needle', default='public fn parser_auto_qualify_std_modules')
    ap.add_argument('--iterations', default=25, type=int)
    ap.add_argument('--result', required=True, type=pathlib.Path)
    args = ap.parse_args()
    exe, workspace, doc = args.lsp.resolve(), args.workspace.resolve(), args.document.resolve()
    text = doc.read_text(encoding='utf-8-sig')
    uri = doc.as_uri()
    client = Client(exe, workspace)
    samples, times, warmed = [], [], []
    result = {'status': 'failed', 'lsp': str(exe), 'lsp_sha256': hashlib.sha256(exe.read_bytes()).hexdigest(), 'workspace': str(workspace), 'iterations': args.iterations}
    for name in ('vyx_compiler_backend.dll', 'libvyx_compiler_backend.so', 'libvyx_compiler_backend.dylib'):
        backend = exe.parent / name
        if backend.is_file():
            result['backend_sha256'] = hashlib.sha256(backend.read_bytes()).hexdigest()
            break
    serial = 0
    def request(method, params):
        nonlocal serial
        serial += 1
        client.send(method, params, serial)
        return client.wait(lambda m: m.get('id') == serial, 180, 1536, samples)['result']
    p = {'textDocument': {'uri': uri}, 'position': position(text, args.needle, args.symbol)}
    try:
        request('initialize', {'rootUri': workspace.as_uri(), 'capabilities': {'workspace': {'workspaceEdit': {'documentChanges': True}}}})
        client.send('initialized', {})
        p['context'] = {'includeDeclaration': True}
        started = time.monotonic()
        first = request('textDocument/references', p)
        result['cold_seconds'] = time.monotonic() - started
        files = {path_from_uri(item['uri']) for item in first}
        assert len(files) >= 3 and len(first) >= 5, first
        result['reference_count'], result['reference_files'] = len(first), sorted(map(str, files))
        for iteration in range(args.iterations):
            started = time.monotonic()
            found = request('textDocument/references', p)
            assert found == first, (iteration, found)
            times.append(time.monotonic() - started)
            if iteration >= 10: warmed.append(samples[-1])
        # parse_unit is also used by the obsolete probe_emit tool, whose
        # backend APIs no longer exist. Refuse its rename rather than ignoring
        # errors in a source file that would receive edits. Separately verify
        # a safe rename in the compiler's current source closure.
        if args.symbol == 'parse_unit' and (workspace / 'tools/probe_emit.vyx').is_file():
            try:
                request('textDocument/rename', {**p, 'newName': args.symbol + '_editor_verified'})
            except RuntimeError as exc:
                assert '-32803' in str(exc) and 'semantic analysis' in str(exc), exc
                result['invalid_legacy_source_rename'] = 'refused'
            else:
                raise AssertionError('rename accepted despite invalid legacy source')
        rename_position = {'textDocument': {'uri': uri},
                           'position': position(text, args.rename_needle, args.rename_symbol)}
        edit = request('textDocument/rename', {**rename_position, 'newName': args.rename_symbol + '_editor_verified'})
        assert len(edit['documentChanges']) >= 3, edit
        result['rename_symbol'] = args.rename_symbol
        result['rename_documents'] = len(edit['documentChanges'])
        assert not warmed or max(warmed) - min(warmed) <= 64, warmed
        request('shutdown', {})
        client.send('exit')
        assert client.p.wait(timeout=10) == 0
        result.update(status='passed', peak_mib=max(samples), warmed_growth_mib=max(warmed)-min(warmed) if warmed else 0, cached_median_seconds=statistics.median(times), cached_max_seconds=max(times))
    except BaseException as exc:
        result.update(error=repr(exc), stderr_tail=list(client.stderr))
        raise
    finally:
        client.close()
        args.result.parent.mkdir(parents=True, exist_ok=True)
        args.result.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(result, ensure_ascii=False, indent=2))

if __name__ == '__main__': main()
