#!/usr/bin/env python3
"""Verify lossless CLI formatting and optional matching LSP formatting."""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile
import time

REPO = Path(__file__).resolve().parents[3]
MESSY = b'fn main()->i32{let x=1;if(x>0){return x;}return 0;}'
RUNTIME = b'''use std.string;
fn add(a:i32,b:i32)->i32{return a+b;}
fn main()->i32{
var total:i32=0;for(var i:i32=0;i<5;i=i+1){total=add(total,i);}
if(total!=10){return 2;}
let s="quotes: \\"; brackets: { [ ] }; // is text";
print(s);print("fmt-runtime-ok");return 0;
}
'''


def fingerprint(source: bytes) -> list[bytes]:
    """Independent fixture oracle: ignore whitespace only outside literals/comments."""
    result = []
    i = 3 if source.startswith(b'\xef\xbb\xbf') else 0
    while i < len(source):
        if source[i] in b' \t\r\n':
            i += 1
            continue
        start = i
        if source.startswith(b'//', i):
            while i < len(source) and source[i] not in b'\r\n':
                i += 1
        elif source.startswith(b'/*', i):
            i += 2
            depth = 1
            while i < len(source) and depth:
                if source.startswith(b'/*', i):
                    depth += 1
                    i += 2
                elif source.startswith(b'*/', i):
                    depth -= 1
                    i += 2
                else:
                    i += 1
            assert depth == 0, 'unterminated block comment in fixture'
        elif source.startswith(b'"""', i):
            i = source.index(b'"""', i + 3) + 3
        elif source[i] in b'"\'':
            quote = source[i]
            # Labels start with an apostrophe but have no closing quote.
            if quote == 39 and source.find(b"'", i + 1, i + 5) < 0:
                i += 1
            else:
                i += 1
                while i < len(source):
                    if source[i] == 92:
                        i += 2
                    elif source[i] == quote:
                        i += 1
                        break
                    else:
                        i += 1
        else:
            i += 1
        result.append(source[start:i])
    return result


class Gate:
    def __init__(self, compiler: Path, output: Path, root: Path):
        self.compiler, self.output, self.root = compiler, output, root
        self.env = dict(os.environ)
        self.env['PATH'] = str(compiler.parent) + os.pathsep + self.env.get('PATH', '')
        self.records = []

    def command(self, *args, cwd=None, expected=0, timeout=120):
        command = [str(self.compiler), *map(str, args)]
        result = subprocess.run(command, cwd=cwd or self.root, env=self.env,
                                capture_output=True, timeout=timeout)
        if expected is None:
            assert result.returncode != 0, f'expected failure: {command}'
        else:
            assert result.returncode == expected, (
                f'{command}: exit {result.returncode}, expected {expected}\n'
                + (result.stdout + result.stderr).decode(errors='replace'))
        return result

    def source(self, name, text):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(text)
        return path

    def check(self, name, operation):
        start = time.monotonic()
        record = dict(name=name, passed=False)
        try:
            operation()
            record['passed'] = True
        except Exception as exc:
            record['error'] = str(exc)
        record['seconds'] = round(time.monotonic() - start, 4)
        self.records.append(record)
        (self.output / 'verification.partial.json').write_text(
            json.dumps(self.records, indent=2), encoding='utf-8')
        print(f'{name}: {"PASS" if record["passed"] else "FAIL"}', flush=True)
        if 'error' in record:
            print(record['error'], flush=True)

    def stable(self, name, source, flags=()):
        path = self.source(name + '.vyx', source)
        result = self.command('fmt', '--stdout', *flags, path)
        assert path.read_bytes() == source, '--stdout modified input'
        formatted = result.stdout
        assert fingerprint(source) == fingerprint(formatted), 'changed literal/comment/token bytes'
        assert source.startswith(b'\xef\xbb\xbf') == formatted.startswith(b'\xef\xbb\xbf')
        self.command('fmt', *flags, path)
        assert path.read_bytes() == formatted, '--stdout and write disagree'
        self.command('fmt', '--check', *flags, path)
        again = self.command('fmt', '--stdout', *flags, path).stdout
        assert again == formatted, 'formatter is not idempotent'
        return formatted


def cli_cases(gate: Gate):
    fixtures = {
        'basic': MESSY,
        'comments': b'// heading\n/// documentation\nfn foo()->i32{/* block /* nested */ kept */let x=1;// trailing\nreturn x;}// eof',
        'literals': b'fn f(){let s="space  inside \\"quoted\\" // /* {}";let c=\'\\n\';let t=f"number ${1+2}";}\n',
        'triple-string': b'fn f(){let s="""\n    alpha  \n      beta\n    """;}\n',
        'for-header': b'fn f()->i32{var n:i32=0;for(var i:i32=0;i<4;i=i+1){n=n+i;}return n;}',
        'generics': b'use std.collections;\nfn identity<T>(value:T)->T{return value;}\nfn f(){var v=Vec::<i32>.new();v.push(identity<i32>(1));}',
        'operators': b'fn f(a:i32,b:i32)->i32{let x=a+b*2;let c=(a<b)&&(b>0);if(!c){return -x;}return a<<2;}',
        'unary-anti-fusion': b'fn f()->i32{return - -1;}\nfn g(a:i32,b:i32)->i32{return a - -b;}\n',
        'comment-after-delimiters': b'@[inline]/* attribute */fn f(){/* open */let n=1;/* statement */}/* closed */fn g(){}',
        'semicolon-free-fields': b'class Pair{public left:i32\npublic right:i32\n}\n',
        'labeled-loop': b"fn f(){'outer: while(true){while(true){break 'outer;}}}\n",
        'attributes': b'@[inline]\nfn f(x:i32)->i32{return x;}\n',
        'empty': b'',
        'comment-only': b'// text\n/* more\n text */',
        'bom': b'\xef\xbb\xbf' + MESSY,
        'crlf': b'// Windows\r\n' + MESSY + b'\r\n',
        'blank-lines': b'fn a(){}\n\n\nfn b(){\n\nreturn;\n}\n',
        'unicode': '// 中文 😀\nfn f(){let s="保留 😀";}\n'.encode(),
    }
    for name, source in fixtures.items():
        gate.check(name, lambda n=name, s=source: gate.stable(n, s))

    for name, source, doc_lines in (
        ('mixed-eol-doc-lf-first',
         b'// LF header\n/// CRLF documentation\r\n/// LF documentation\nfn f()->i32{return 0;}\n',
         (b'/// CRLF documentation\r\n', b'/// LF documentation\n')),
        ('mixed-eol-doc-crlf-first',
         b'// CRLF header\r\n/// LF documentation\n/// CRLF documentation\r\nfn f()->i32{return 0;}\r\n',
         (b'/// LF documentation\n', b'/// CRLF documentation\r\n')),
    ):
        def mixed_docs(n=name, s=source, lines=doc_lines):
            actual = gate.stable(n, s)
            assert all(line in actual for line in lines), 'changed documentation comment line endings'
        gate.check(name, mixed_docs)

    def golden_basic():
        actual = gate.stable('golden-basic', b'fn main()->i32{return 0;}')
        assert actual == b'fn main() -> i32 {\n    return 0;\n}\n', actual

    gate.check('golden-basic', golden_basic)

    def golden_return_group():
        actual = gate.stable('golden-return-group', b'fn identity(value:i32)->i32{return(value);}')
        expected = b'fn identity(value: i32) -> i32 {\n    return (value);\n}\n'
        assert actual == expected, actual

    gate.check('golden-return-group', golden_return_group)

    def indentation():
        two = gate.stable('two-spaces', MESSY, ('--indent-width', '2'))
        assert b'\n  let x = 1;' in two, two
        tabs = gate.stable('tabs', MESSY, ('--tabs',))
        assert b'\n\tlet x = 1;' in tabs, tabs

    gate.check('indent-options', indentation)

    def line_endings():
        data = gate.stable('crlf-strict', b'fn f(){\r\nlet s="""\r\n  raw\n bytes\r\n""";\r\n}\r\n')
        assert b'"""\r\n  raw\n bytes\r\n"""' in data
        outside = data.replace(b'"""\r\n  raw\n bytes\r\n"""', b'"x"')
        assert b'\n' not in outside.replace(b'\r\n', b''), 'inserted LF into CRLF source'

    gate.check('line-endings-preserve-literal', line_endings)

    def check_only():
        path = gate.source('check-only.vyx', MESSY)
        gate.command('fmt', '--check', path, expected=1)
        assert path.read_bytes() == MESSY

    gate.check('check-does-not-write', check_only)

    def invalid(arguments):
        path = gate.source('untouched.vyx', MESSY)
        gate.command('fmt', path, *arguments, expected=2)
        assert path.read_bytes() == MESSY, 'invalid invocation modified a source file'

    for name, arguments in {
        'unknown-option': ['--unknown'], 'missing-path': ['missing.vyx'],
        'version-option-isolation': ['--version'],
        'project-option-isolation': ['--src=project'],
        'bad-indent-zero': ['--indent-width', '0'],
        'bad-indent-large': ['--indent-width', '17'],
        'missing-indent-value': ['--indent-width'],
        'bad-indent-text': ['--indent-width', 'abc'],
    }.items():
        gate.check(name, lambda a=arguments: invalid(a))

    for name, source in {
        'bad-syntax': b'fn f(){let x=;}',
        'bad-string': b'fn f(){let s="unterminated;}',
        'bad-comment': b'fn f(){/* unfinished',
        'embedded-nul': MESSY + b'\x00ignored',
    }.items():
        def rejected(n=name, s=source):
            path = gate.source(n + '.vyx', s)
            gate.command('fmt', path, expected=None)
            assert path.read_bytes() == s
        gate.check(name, rejected)

    def file_permissions():
        path = gate.source('permissions.vyx', MESSY)
        old = gate.source('permissions.vyx.old', b'user backup')
        before = path.stat().st_mode
        gate.command('fmt', path)
        assert old.read_bytes() == b'user backup'
        assert path.stat().st_mode == before
        assert not list(path.parent.glob(path.name + '*.tmp')), 'left publication temp behind'

    gate.check('publish-preserves-backups-mode', file_permissions)

    def readonly():
        path = gate.source('read-only.vyx', MESSY)
        original_mode = path.stat().st_mode
        path.chmod(original_mode & ~(stat.S_IWUSR | stat.S_IWGRP | stat.S_IWOTH))
        try:
            gate.command('fmt', path, expected=None)
            assert path.read_bytes() == MESSY
        finally:
            path.chmod(original_mode)

    gate.check('read-only-not-replaced', readonly)

    def hardlink():
        path = gate.source('hardlink.vyx', MESSY)
        alias = gate.root / 'hardlink-alias.vyx'
        os.link(path, alias)
        gate.command('fmt', path, expected=2)
        assert path.read_bytes() == alias.read_bytes() == MESSY
        assert os.path.samefile(path, alias), 'formatter broke a hard link'

    gate.check('hardlink-not-replaced', hardlink)

    def extension_and_dash():
        text = gate.source('unsupported.txt', MESSY)
        source = gate.source('valid-with-bad-extension.vyx', MESSY)
        gate.command('fmt', source, text, expected=2)
        assert text.read_bytes() == source.read_bytes() == MESSY
        leading = gate.source('-dash.vyx', MESSY)
        gate.command('fmt', '--', leading.name)
        gate.command('fmt', '--check', '--', leading.name)

    gate.check('extension-and-option-terminator', extension_and_dash)

    def directory():
        path = gate.source('directory/src/a.vyx', MESSY)
        duplicate = gate.source('directory/src/b.vyx', MESSY)
        for name in ('.cache', 'target', 'build', 'out', '.git'):
            gate.source('directory/src/' + name + '/ignored.vyx', b'invalid syntax')
        result = gate.command('fmt', path.parent, path, path.parent)
        assert result.stdout.count(b'reformatted') == 2, result.stdout
        assert path.read_bytes() == duplicate.read_bytes()
        gate.command('fmt', '--check', path.parent)
        gate.command('fmt', '--stdout', path, duplicate, expected=2)

    gate.check('directory-dedup-exclusions', directory)

    def concurrent():
        path = gate.source('concurrent.vyx', MESSY)
        expected = gate.command('fmt', '--stdout', path).stdout
        with ThreadPoolExecutor(max_workers=4) as pool:
            results = list(pool.map(lambda _: subprocess.run(
                [str(gate.compiler), 'fmt', str(path)], cwd=gate.root,
                env=gate.env, capture_output=True, timeout=60), range(4)))
        assert all(r.returncode in (0, 2) for r in results), [r.stderr for r in results]
        assert path.read_bytes() == expected

    gate.check('concurrent-publication', concurrent)

    def runtime():
        path = gate.source('runtime.vyx', RUNTIME)
        before = gate.command('--run=aot', '--src=file', path).stdout
        assert b'fmt-runtime-ok' in before
        gate.command('fmt', path)
        after = gate.command('--run=aot', '--src=file', path).stdout
        assert before == after, 'formatted source changed AOT output'

    gate.check('aot-before-after', runtime)


def lsp_cases(gate: Gate, executable: Path):
    spec = importlib.util.spec_from_file_location('fmt_lsp_client', REPO / 'tests/bootstrap/lsp_smoke/run.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    class StrictClient(module.LspClient):
        def _read_loop(self):
            try:
                while True:
                    length = None
                    while True:
                        line = self.process.stdout.readline()
                        if not line:
                            raise EOFError('LSP stdout closed')
                        if line in (b'\r\n', b'\n'):
                            break
                        name, separator, value = line.partition(b':')
                        if not separator or name.lower() not in (b'content-length', b'content-type'):
                            raise RuntimeError(f'non-protocol stdout: {line!r}')
                        if name.lower() == b'content-length':
                            length = int(value)
                    if length is None or not 0 <= length <= 32 * 1024 * 1024:
                        raise RuntimeError(f'invalid Content-Length: {length}')
                    payload = self.process.stdout.read(length)
                    if len(payload) != length:
                        raise EOFError('short LSP frame')
                    self.responses.put(json.loads(payload))
            except BaseException as exc:
                self.responses.put(exc)

    folder = gate.root / 'editor'
    folder.mkdir()
    client = StrictClient(executable, folder, gate.output / 'lsp-trace.log')
    path = folder / 'buffer.vyx'
    path.write_bytes(MESSY)
    uri = path.as_uri()
    version = 0
    request_id = 0

    def request(method, params):
        nonlocal request_id
        request_id += 1
        client.send(dict(jsonrpc='2.0', id=request_id, method=method, params=params))
        return client.receive_response(request_id, timeout=30)

    def buffer(text):
        nonlocal version
        version += 1
        if version == 1:
            client.send(dict(jsonrpc='2.0', method='textDocument/didOpen', params=dict(
                textDocument=dict(uri=uri, languageId='vyx', version=version, text=text))))
        else:
            client.send(dict(jsonrpc='2.0', method='textDocument/didChange', params=dict(
                textDocument=dict(uri=uri, version=version), contentChanges=[dict(text=text)])))

    try:
        response = request('initialize', dict(rootUri=folder.as_uri(), capabilities={}, processId=None))
        capabilities = response['result']['capabilities']
        assert capabilities.get('documentFormattingProvider') is True
        assert capabilities.get('documentRangeFormattingProvider') is not True
        client.send(dict(jsonrpc='2.0', method='initialized', params={}))
        for width, spaces in ((4, True), (2, True), (4, False)):
            def options_case(w=width, s=spaces):
                source = (MESSY + ' // 中文 😀'.encode()).decode()
                buffer(source)
                cli_path = gate.source('lsp-cli.vyx', source.encode())
                flags = ['--indent-width', str(w)] + ([] if s else ['--tabs'])
                expected = gate.command('fmt', '--stdout', *flags, cli_path).stdout.decode()
                result = request('textDocument/formatting', dict(textDocument=dict(uri=uri),
                    options=dict(tabSize=w, insertSpaces=s)))
                edits = result.get('result')
                assert isinstance(edits, list) and len(edits) == 1, result
                assert edits[0]['newText'] == expected, result
                assert edits[0]['range'] == dict(start=dict(line=0, character=0),
                    end=dict(line=0, character=len(source.encode('utf-16-le')) // 2)), result
                buffer(expected)
                unchanged = request('textDocument/formatting', dict(textDocument=dict(uri=uri),
                    options=dict(tabSize=w, insertSpaces=s)))
                assert unchanged.get('result') == [], unchanged
                assert path.read_bytes() == MESSY, 'LSP changed disk file'
            gate.check(f'lsp-options-{width}-{spaces}', options_case)

        def invalid_buffer():
            buffer('fn broken(){let x=;}')
            result = request('textDocument/formatting', dict(textDocument=dict(uri=uri),
                options=dict(tabSize=4, insertSpaces=True)))
            assert result.get('error', {}).get('code') == -32803, result
            buffer(MESSY.decode())
            for options in (dict(tabSize=0, insertSpaces=True), dict(tabSize=2.5, insertSpaces=True),
                            dict(tabSize=4, insertSpaces='true'), False):
                result = request('textDocument/formatting', dict(textDocument=dict(uri=uri), options=options))
                assert result.get('error', {}).get('code') == -32602, result
        gate.check('lsp-reject-invalid', invalid_buffer)

        def range_rejected():
            result = request('textDocument/rangeFormatting', dict(textDocument=dict(uri=uri),
                options=dict(tabSize=4, insertSpaces=True),
                range=dict(start=dict(line=0, character=3), end=dict(line=0, character=8))))
            assert result.get('error', {}).get('code') == -32601, result
        gate.check('lsp-range-does-not-rewrite-document', range_rejected)
        request('shutdown', None)
        client.send(dict(jsonrpc='2.0', method='exit'))
        assert client.process.wait(timeout=10) == 0
    finally:
        client.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--lsp', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--corpus', action='store_true', help='also format copies of real compiler/library sources')
    parser.add_argument('--stress', action='store_true', help='also measure 1k/4k function formatting')
    args = parser.parse_args()
    compiler = args.compiler.resolve(strict=True)
    output = (args.output or Path(__file__).parent / '.cache').resolve()
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='vyx formatter space ') as temporary:
        gate = Gate(compiler, output, Path(temporary))
        cli_cases(gate)
        if args.corpus:
            for relative in (
                'bootstrap_compiler/src/core/syntax/lexer.vyx',
                'bootstrap_compiler/src/core/tooling/formatter.vyx',
                'bootstrap_compiler/src/hir/model/hir_ids.vyx',
                'bootstrap_compiler/src/core/sema/lang_items.vyx',
                'bootstrap_compiler/std_packages/core/src/string.vyx',
            ):
                path = REPO / relative
                gate.check('corpus-' + path.stem, lambda p=path: gate.stable('corpus-' + p.stem, p.read_bytes()))
        if args.stress:
            for count in (1000, 4000):
                def stress(n=count):
                    source = ''.join(f'fn format_case_{i}(x:i32)->i32{{return x+{i};}}\n' for i in range(n)).encode()
                    gate.stable(f'stress-{n}', source)
                gate.check(f'stress-{count}', stress)
        if args.lsp:
            gate.check('lsp-session', lambda: lsp_cases(gate, args.lsp.resolve(strict=True)))
        report = dict(compiler=str(compiler), sha256=hashlib.sha256(compiler.read_bytes()).hexdigest(),
                      lsp=str(args.lsp.resolve()) if args.lsp else None,
                      lsp_sha256=hashlib.sha256(args.lsp.read_bytes()).hexdigest() if args.lsp else None,
                      skipped=[] if args.lsp else ['LSP (no --lsp supplied)'],
                      records=gate.records)
        failed = sum(not record['passed'] for record in gate.records)
        report['passed'], report['failed'] = len(gate.records) - failed, failed
        if failed:
            shutil.copytree(gate.root, output / 'failed-inputs', dirs_exist_ok=True,
                            ignore=shutil.ignore_patterns('*.exe', '*.obj', '*.lib', '.cache', 'target'))
        (output / 'verification.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
        print(f'formatter: {report["passed"]} passed, {failed} failed; {output / "verification.json"}')
        return 1 if failed else 0


if __name__ == '__main__':
    raise SystemExit(main())
