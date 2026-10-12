"""Check inferred return types through shallow interfaces and project/AOT builds."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import itertools
import subprocess
from datetime import datetime


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', type=Path, required=True)
    args = parser.parse_args()
    compiler = args.compiler.resolve(strict=True)
    gate = Path(__file__).resolve().parent
    repo = gate.parents[2]
    out = gate / '.runs' / datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    out.mkdir(parents=True)
    env = dict(os.environ, VYX_BOOTSTRAP_PROFILE='0', VYX_MIR_TIMING='0', VYX_LLVM_TIMING='0')
    env['PATH'] = str(compiler.parent) + os.pathsep + env.get('PATH', '')
    records = []

    def invoke(name, command, cwd=repo, expected=0, diagnostic='', stdout=None):
        result = subprocess.run([str(x) for x in command], cwd=cwd, env=env,
                                capture_output=True, timeout=120)
        text = (result.stdout + result.stderr).decode(errors='replace')
        passed = result.returncode == expected and diagnostic in text
        if stdout is not None:
            passed &= result.stdout.replace(b'\r\n', b'\n') == stdout
        (out / (name + '.log')).write_text(text, encoding='utf-8')
        records.append(dict(name=name, passed=bool(passed), exit=result.returncode,
                            command=[str(x) for x in command]))
        print(name, 'PASS' if passed else 'FAIL', flush=True)
        if not passed:
            print(text[-5000:], flush=True)
        return passed

    suffix = '.exe' if os.name == 'nt' else ''
    variants = {
        'scalar': ('public fn value() { return 42; }', 'return value() - 42;', 0, ''),
        'generic': ('public fn value<T>(x: T) { return x; }', 'return value::<i32>(42) - 42;', 0, ''),
        'private-helper': ('fn hidden() { return 42; } public fn value() { return hidden(); }', 'return value() - 42;', 0, ''),
        'private-access': ('fn hidden() { return 42; } public fn value() { return hidden(); }', 'return hidden();', 1, 'cannot access'),
        'void': ('public fn value() { print(42); }', 'value(); return 0;', 0, ''),
        'method': ('public class Box { public x: i32\n public fn set(self, x: i32) { self.x = x; } }',
                   'var b = Box{0}; b.set(42); return b.x - 42;', 0, ''),
        'method-value': ('public class Box { public x: i32\n public fn get(self) { return self.x; } }',
                         'var b = Box{42}; return b.get() - 42;', 0, ''),
        'branch': ('public fn value() { if (true) { return 42; } return 7; }', 'return value() - 42;', 0, ''),
        'match': ('public fn value() { match (1) { case 1: { return 42; } default: { return 7; } } }', 'return value() - 42;', 0, ''),
        'bad-void-body': ('public fn value() { nonexistent(); }', 'value(); return 0;', 1, 'undefined'),
        'bad-explicit-body': ('public fn value() -> i32 { return nonexistent(); }', 'return value();', 1, 'undefined'),
        'bad-mixed-return': ('public fn value() { if (true) { return 42; } return false; }', 'return value();', 1, 'mismatch'),
    }
    for (label, (lib, entry, expected, diagnostic)), opt in itertools.product(variants.items(), (0, 2)):
        name = f'{label}-O{opt}'
        work = out / name
        (work / 'src').mkdir(parents=True)
        (work / 'src/helper.vyx').write_text('module Helper;\n' + lib + '\n', encoding='utf-8')
        (work / 'src/main.vyx').write_text('use Helper;\nfn main() -> i32 { ' + entry + ' }\n', encoding='utf-8')
        (work / 'Vyx.toml').write_text('''[package]
name = "return_probe"
version = "0.1.0"
entry = "src/main.vyx"
[build]
output_dir = "target"
[target.return_probe]
type = "executable"
entry = "src/main.vyx"
auto_sources = false
parallel_vyx = true
sources = ["src/helper.vyx"]
''', encoding='utf-8')
        if invoke(name + '-build', [compiler, 'build', '-j20', f'-O{opt}'], work, expected, diagnostic) and expected == 0:
            invoke(name + '-run', [work / ('target/return_probe' + suffix)])
    report = dict(compiler=str(compiler), sha256=hashlib.sha256(compiler.read_bytes()).hexdigest(), records=records)
    (out / 'results.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(f'{sum(x["passed"] for x in records)}/{len(records)} PASS: {out}')
    return 0 if all(x['passed'] for x in records) else 1


if __name__ == '__main__':
    raise SystemExit(main())
