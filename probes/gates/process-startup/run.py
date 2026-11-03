"""Build and exercise the production POSIX compiler child launcher."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import platform
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if os.name != 'posix':
        print('POSIX process startup: SKIP (requires a POSIX host)')
        raise SystemExit(77)
    gate = Path(__file__).resolve().parent
    root = gate.parents[2]
    out = (args.output or gate / '.cache').resolve()
    out.mkdir(parents=True, exist_ok=True)
    compiler = shutil.which(os.environ.get('CXX', 'clang++')) or shutil.which('c++')
    if not compiler:
        raise RuntimeError('A native C++ compiler is required')
    command = [compiler, '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread',
               '-I' + str(root / 'vyx_codegen/src'), str(gate / 'probe.cpp'), '-o', str(out / 'probe')]
    subprocess.run(command, check=True, timeout=90)
    with tempfile.TemporaryDirectory(prefix='vyx startup space ') as temporary:
        result = subprocess.run([str(out / 'probe'), temporary], capture_output=True, text=True, timeout=60)
    print(result.stdout, end='')
    print(result.stderr, end='')
    record = dict(platform=platform.platform(), command=command, exit=result.returncode,
                  launcher_sha256=hashlib.sha256((root / 'vyx_codegen/src/bootstrap_process_posix.hpp').read_bytes()).hexdigest(),
                  stdout=result.stdout, stderr=result.stderr)
    (out / 'verification.json').write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
    raise SystemExit(result.returncode)


if __name__ == '__main__':
    main()
