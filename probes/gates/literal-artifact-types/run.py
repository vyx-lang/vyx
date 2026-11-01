"""Compare integer typing in source bodies and reconstructed imported artifacts."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', required=True, type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    compiler = args.compiler.resolve(strict=True)
    gate = Path(__file__).resolve().parent
    repo = gate.parents[2]
    output = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix='literal-types-', dir=repo / '.cache'))
    output.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, VYX_STD_PACKAGES=str(repo / 'bootstrap_compiler/std_packages'))
    env['PATH'] = str(compiler.parent) + os.pathsep + env.get('PATH', '')
    rows = []

    def invoke(name, command, expected=0, diagnostic=None):
        command = list(map(str, command))
        try:
            result = subprocess.run(command, cwd=output, env=env, capture_output=True, timeout=120)
            data = result.stdout + result.stderr
            passed = result.returncode == expected and (diagnostic is None or diagnostic.encode() in data)
            row = dict(name=name, command=command, exit=result.returncode, passed=passed)
        except subprocess.TimeoutExpired as error:
            data = (error.stdout or b'') + (error.stderr or b'')
            row = dict(name=name, command=command, passed=False, timeout=True)
        (output / (name + '.log')).write_bytes(data)
        rows.append(row)
        print(name + ': ' + ('PASS' if row['passed'] else 'FAIL'), flush=True)
        if not row['passed']:
            print(data.decode('utf-8', 'replace')[-4000:], flush=True)
        return row['passed']

    interfaces = output / 'interfaces'
    interfaces.mkdir(exist_ok=True)
    interface_ok = invoke('interface', [compiler, '--src=file', gate / 'provider.vyx',
                          '--emit=vyi', '--vyi-shallow', '-o', interfaces / 'literal_probe.vyi'])
    source_consumer = output / 'source-consumer.vyx'
    provider = (gate / 'provider.vyx').read_text(encoding='utf-8')
    consumer = (gate / 'consumer.vyx').read_text(encoding='utf-8')
    source_consumer.write_text(provider + '\n' + consumer.replace('use literal_probe;', ''), encoding='utf-8')
    for name, source, flags in (
        ('source', source_consumer, []),
        ('artifact', gate / 'consumer.vyx', ['--interface-root', interfaces]),
        ('utf8', gate / 'utf8.vyx', []),
    ):
        if name == 'artifact' and not interface_ok:
            continue
        invoke(name + '-mir', [compiler, '--src=file', source, '--emit=mir-verify', '--export-all', *flags])
        for optimization in (0, 2):
            label = name + '-O' + str(optimization)
            exe = output / (label + ('.exe' if os.name == 'nt' else ''))
            if invoke(label + '-build', [compiler, '--src=file', source, '--emit=exe',
                                        '-O' + str(optimization), '-o', exe, *flags]):
                invoke(label + '-run', [exe])
    for name, (body, diagnostic) in {
        'reject-narrowing': ('fn narrow(x:i32)->i32{return x;}\nfn main()->i32{let x:i64=7;return narrow(x);}',
                             'E1000: call arg: type mismatch i64 vs i32'),
        'reject-ambiguity': ('fn pick(x:i8)->i32{return 1;}\nfn pick(x:u8)->i32{return 2;}\nfn main()->i32{return pick(7);}',
                            "E2300: ambiguous call to 'pick'"),
    }.items():
        source = output / (name + '.vyx')
        source.write_text(body, encoding='utf-8')
        invoke(name, [compiler, '--src=file', source, '--emit=mir-verify', '--export-all'],
               expected=1, diagnostic=diagnostic)
    report = dict(compiler=str(compiler), sha256=hashlib.sha256(compiler.read_bytes()).hexdigest(),
                  passed=sum(row['passed'] for row in rows), checks=rows)
    (output / 'verification.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(f"literal types: {report['passed']}/{len(rows)} passed; {output}", flush=True)
    return 0 if all(row['passed'] for row in rows) else 1


if __name__ == '__main__':
    raise SystemExit(main())
