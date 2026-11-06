"""Check registry-only implicit SDK imports and explicit custom module sources."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    compiler = args.compiler.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    records = []
    with tempfile.TemporaryDirectory(prefix='vyx-package-discovery-') as temporary:
        root = Path(temporary)
        sdk = root / 'sdk'
        bin_dir = sdk / 'bin'
        bin_dir.mkdir(parents=True)
        isolated = bin_dir / compiler.name
        shutil.copy2(compiler, isolated)
        if os.name == 'nt':
            for library in compiler.parent.glob('*.dll'):
                shutil.copy2(library, bin_dir / library.name)
        project = root / 'project'
        project.mkdir()
        source = project / 'main.vyx'
        source.write_text('use std.flat_fixture;\nfn main() -> i32 { return flat_std_fixture_marker(); }\n', encoding='utf-8')
        legacy = sdk / 'std'
        legacy.mkdir()
        producer = legacy / 'clone.vyx'
        producer.write_text('module std.flat_fixture;\npublic fn flat_std_fixture_marker() -> i32 { return 41; }\n', encoding='utf-8')
        env = dict(os.environ)
        env.pop('VYX_STD_PACKAGES', None)
        # Resolve the supplied compiler's companion libraries without copying
        # its SDK sources or exposing any sibling standard-library provider.
        for variable in ('PATH', 'LD_LIBRARY_PATH', 'DYLD_LIBRARY_PATH'):
            env[variable] = str(compiler.parent) + os.pathsep + env.get(variable, '')

        def check(name, *, success, options=()):
            command = [str(isolated), '--src=file', str(source), '--stop-after-sema', *map(str, options)]
            result = subprocess.run(command, cwd=project, env=env, capture_output=True, timeout=60)
            text = (result.stdout + result.stderr).decode('utf-8', errors='replace')
            (output / (name + '.log')).write_text(text, encoding='utf-8')
            passed = result.returncode == 0 if success else (
                result.returncode in (1, 2) and 'flat_std_fixture_marker' in text and 'error' in text.lower())
            records.append({'name': name, 'exit': result.returncode, 'passed': passed, 'command': command})
            assert passed, f'{name}: exit={result.returncode}\n{text}'

        try:
            check('flat-sdk-is-not-a-provider', success=False)
            check('explicit-source-remains-supported', success=True, options=['--module-source', producer])
            packages = sdk / 'std_packages'
            package_source = packages / 'fixture/src/fixture.vyx'
            package_source.parent.mkdir(parents=True)
            shutil.copy2(producer, package_source)
            check('package-without-registry-is-not-a-provider', success=False)
            (packages / 'registry').write_text('fixture/src/fixture.vyx\n', encoding='utf-8')
            check('sdk-registry-resolves-module', success=True)
        finally:
            (output / 'results.json').write_text(json.dumps({
                'compiler': str(compiler), 'sha256': hashlib.sha256(compiler.read_bytes()).hexdigest(),
                'checks': records,
            }, indent=2) + '\n', encoding='utf-8')
    print(f'{len(records)}/{len(records)} package discovery checks passed')


if __name__ == '__main__':
    main()
