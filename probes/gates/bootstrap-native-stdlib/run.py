"""Exercise boot's native stdlib inputs with its actual parallel-source model."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import time
import tomllib
import tempfile

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'probes/gates/stdlib-contracts'))
from run import execute


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--build-root', type=Path, default=ROOT / 'bootstrap_compiler')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--opt', choices=('0', '2', 'both'), default='both')
    args = parser.parse_args()
    compiler = args.compiler.resolve(strict=True)
    build = args.build_root.resolve(strict=True)
    std = build / 'std_packages'
    manifest = build / 'Vyx.toml'
    boot = tomllib.loads(manifest.read_text(encoding='utf8'))['target']['boot']
    native = [(build / p).resolve(strict=True) for p in boot['sources']
              if (build / p).resolve().is_relative_to(std.resolve()) and Path(p).suffix in ('.c', '.cpp', '.cc', '.cxx')]
    if not native:
        raise RuntimeError('boot has no declared native standard-library inputs')
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    # Release git archives have no package out/. Reproduce that source-only
    # state; otherwise local .vyi products can shadow or duplicate the explicit
    # parallel_std_sources inputs. Never delete another build's products.
    current_std = Path(tempfile.mkdtemp(prefix='current-std-', dir=out))
    shutil.copytree(std, current_std, dirs_exist_ok=True,
        ignore=shutil.ignore_patterns('.cache', 'out', 'target', '*.vyi', '*.vyi.*', '*.o', '*.obj', '*.a', '*.lib'))
    project = Path(tempfile.mkdtemp(prefix='project-', dir=out))
    fixture = Path(__file__).with_name('main.vyx')
    shutil.copy2(fixture, project / 'main.vyx')
    text = ('[package]\nname="bootstrap_native_stdlib"\nversion="0.1.0"\nentry="main.vyx"\n'
            '[build]\ncache_dir=".cache"\noutput_dir="target"\n'
            '[target.bootstrap_native_stdlib]\ntype="executable"\nentry="main.vyx"\nauto_sources=false\n'
            'parallel_vyx=true\nparallel_std_sources=' + json.dumps(current_std.as_posix()) + '\n'
            'parallel_std_excludes=' + json.dumps(boot['parallel_std_excludes']) + '\n'
            'sources=' + json.dumps([p.as_posix() for p in native]) + '\n'
            'include_paths=' + json.dumps([(build / p).resolve().as_posix() for p in boot['include_paths']]) + '\n')
    (project / 'Vyx.toml').write_text(text, encoding='utf8')
    environment = dict(os.environ, VYX_STD_PACKAGES=str(current_std))
    inputs = {str(manifest): sha(manifest), str(fixture): sha(fixture),
              **{str(p): sha(p) for p in std.rglob('*') if p.is_file() and p.suffix in ('.vyx', '.c', '.h', '.toml')
                 and not any(part in ('.cache', 'out', 'target') for part in p.relative_to(std).parts)}}
    records = []
    identity = sha(compiler)
    visibility = Path(__file__).with_name('visibility.vyx')
    inputs[str(visibility)] = sha(visibility)
    interface = out / 'visibility.vyi'
    command = [str(compiler), '--src=file', str(visibility), '--emit=vyi', '--vyi-shallow', '-o', str(interface)]
    code, log = execute(command, project, environment, 60)
    exported = interface.read_text(encoding='utf8') if interface.is_file() else ''
    impls = [line for line in exported.splitlines() if line.strip().startswith('impl')]
    public_impl = any('Clone' in line and 'PublicHandle' in line for line in impls)
    leaked_private = any('PrivateTrait' in line or 'PrivateHandle' in line for line in impls)
    passed = code == 0 and public_impl and not leaked_private
    (out / 'visibility.log').write_bytes(log)
    records.append(dict(command=command, name='shallow-imported-generic-trait', exit=code, passed=passed))
    print('shallow trait visibility: ' + ('PASS' if passed else 'FAIL'), flush=True)
    if not passed: print(log[-3000:].decode(errors='replace') + '\n' + '\n'.join(impls), flush=True)
    consumer = project / 'trait_consumer.vyx'
    consumer.write_text('''use std.core;
use bootstrap_visibility_probe;
fn main() -> i32 {
    let value = PublicHandle::<i32> { token: 42 };
    let copied: PublicHandle<i32> = value.clone();
    assert(copied.token == 42, "imported generic Clone body");
    print("shallow generic trait body ok"); return 0;
}
''', encoding='utf8')
    modules = project / 'trait.modules'
    modules.write_text(interface.as_posix() + '\n', encoding='utf8')
    command = [str(compiler), '--run=aot', '--src=file', str(consumer), '--module-sources-file', str(modules),
               '-o', str(project / ('trait_consumer.exe' if os.name == 'nt' else 'trait_consumer'))]
    code, log = execute(command, project, environment, 60)
    passed = code == 0 and b'shallow generic trait body ok' in log
    records.append(dict(command=command, name='shallow-generic-trait-body', exit=code, passed=passed))
    (out / 'trait-consumer.log').write_bytes(log)
    print('shallow generic trait body: ' + ('PASS' if passed else 'FAIL'), flush=True)
    if not passed: print(log[-4000:].decode(errors='replace'), flush=True)
    named_fixture = Path(__file__).with_name('named_std.vyx')
    inputs[str(named_fixture)] = sha(named_fixture)
    named_interface = out / 'named_std.vyi'
    command = [str(compiler), '--src=file', str(named_fixture), '--emit=vyi', '--vyi-shallow', '-o', str(named_interface)]
    code, log = execute(command, project, environment, 60)
    (out / 'named-std-interface.log').write_bytes(log)
    records.append(dict(command=command, name='named-stdlib-interface', exit=code, passed=code == 0))
    named_consumer = project / 'named_consumer.vyx'
    named_consumer.write_text('''use std.core;
use std.bootstrap_artifact_probe;
fn main() -> i32 {
    let value = ProbeCell::<i32> { value: 42 };
    assert(value.read() == 7, "qualified standard-library artifact types");
    print("named stdlib artifact ok"); return 0;
}
''', encoding='utf8')
    named_modules = project / 'named.modules'
    named_modules.write_text(named_interface.as_posix() + '\n', encoding='utf8')
    command = [str(compiler), '--run=aot', '--src=file', str(named_consumer), '--module-sources-file', str(named_modules),
               '-o', str(project / ('named_consumer.exe' if os.name == 'nt' else 'named_consumer'))]
    code, log = execute(command, project, environment, 60)
    passed = code == 0 and b'named stdlib artifact ok' in log
    records.append(dict(command=command, name='named-stdlib-artifact-types', exit=code, passed=passed))
    (out / 'named-std-consumer.log').write_bytes(log)
    print('named stdlib artifact types: ' + ('PASS' if passed else 'FAIL'), flush=True)
    if not passed: print(log[-4000:].decode(errors='replace'), flush=True)
    # A package interface and an implicit source can have different stems.
    # The selected interface is the module contract; a second source transport
    # must not introduce a conflicting definition, regardless of row order.
    conflicting = project / 'other_stem.vyx'
    conflicting.write_text('module std.bootstrap_artifact_probe;\npublic class ProbeCell<T> { public value: bool; }\n', encoding='utf8')
    for order, paths in (('interface-first', (named_interface, conflicting)),
                         ('source-first', (conflicting, named_interface))):
        order_modules = project / (order + '.modules')
        order_modules.write_text(''.join(p.as_posix() + '\n' for p in paths), encoding='utf8')
        order_command = list(command)
        order_command[command.index('--module-sources-file') + 1] = str(order_modules)
        code, log = execute(order_command, project, environment, 60)
        passed = code == 0 and b'named stdlib artifact ok' in log
        records.append(dict(command=order_command, name='interface-transport-' + order, exit=code, passed=passed))
        (out / (order + '.log')).write_bytes(log)
        print('interface transport ' + order + ': ' + ('PASS' if passed else 'FAIL'), flush=True)
        if not passed: print(log[-4000:].decode(errors='replace'), flush=True)
    stale_directory = project / 'old-output'
    stale_directory.mkdir()
    (stale_directory / 'probe.vyi').write_text(
        'module std.bootstrap_artifact_probe;\npublic class ProbeCell<T> { public value: bool; }\n', encoding='utf8')
    source_modules = project / 'explicit-source.modules'
    source_modules.write_text(named_fixture.as_posix() + '\n', encoding='utf8')
    source_command = command + ['-L', str(stale_directory)]
    source_command[command.index('--module-sources-file') + 1] = str(source_modules)
    code, log = execute(source_command, project, environment, 60)
    passed = code == 0 and b'named stdlib artifact ok' in log
    records.append(dict(command=source_command, name='explicit-source-before-old-library-interface', exit=code, passed=passed))
    (out / 'explicit-source.log').write_bytes(log)
    print('explicit source priority: ' + ('PASS' if passed else 'FAIL'), flush=True)
    if not passed: print(log[-4000:].decode(errors='replace'), flush=True)
    ref_interface = out / 'ref.vyi'
    command = [str(compiler), '--src=file', str(current_std / 'core/src/ref.vyx'), '--emit=vyi', '--vyi-shallow', '-o', str(ref_interface)]
    code, log = execute(command, project, environment, 60)
    ref_surface = ref_interface.read_text(encoding='utf8') if ref_interface.is_file() else ''
    passed = code == 0 and '// VYX_TEMPLATE_ARTIFACT_BEGIN v=1 ' in ref_surface
    records.append(dict(command=command, name='stdlib-generic-template-artifact', exit=code, passed=passed))
    (out / 'ref-interface.log').write_bytes(log)
    print('stdlib generic template artifact: ' + ('PASS' if passed else 'FAIL'), flush=True)
    if not passed: print(log[-3000:].decode(errors='replace'), flush=True)
    for opt in ('0', '2') if args.opt == 'both' else (args.opt,):
        command = [str(compiler), '--src=project', str(project), '--target', 'bootstrap_native_stdlib', '--run=aot', '-j4', '-O' + opt]
        started = time.monotonic()
        code, log = execute(command, project, environment, 180)
        (out / ('O' + opt + '.log')).write_bytes(log)
        passed = code == 0 and b'bootstrap native stdlib closure ok' in log
        records.append(dict(command=command, name='parallel-native-closure', opt=opt, exit=code, passed=passed, seconds=time.monotonic()-started))
        print('bootstrap native stdlib O' + opt + ': ' + ('PASS' if passed else 'FAIL'), flush=True)
        if not passed: print(log[-6000:].decode(errors='replace'), flush=True)
    report = dict(compiler=str(compiler), compiler_sha256=identity, inputs=inputs, standard_library_snapshot=str(current_std),
                  native_sources=list(map(str, native)), records=records,
                  inputs_changed=any(sha(Path(p)) != digest for p, digest in inputs.items()),
                  compiler_changed=sha(compiler) != identity)
    report['passed'] = all(r['passed'] for r in records) and not report['inputs_changed'] and not report['compiler_changed']
    (out / 'verification.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf8')
    raise SystemExit(not report['passed'])


if __name__ == '__main__': main()
