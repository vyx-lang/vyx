"""Build and execute native engines under address/undefined/leak sanitizers."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tomllib
import hashlib

ROOT = Path(__file__).resolve().parents[3]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', default='clang')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if os.name == 'nt':
        parser.error('run this sanitizer gate on Linux or macOS')
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    package = ROOT / 'bootstrap_compiler/std_packages/regex'
    sources = tomllib.loads((package / 'Vyx.toml').read_text())['target']['std_regex']['sources']
    native = [package / source for source in sources]
    native += [ROOT / 'bootstrap_compiler/std_packages/serde/vendor/tomlc17/tomlc17.c',
               Path(__file__).with_name('native_engines.c')]
    command = [args.cc, '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
               '-DHAVE_CONFIG_H', '-DPCRE2_CODE_UNIT_WIDTH=8', '-DPCRE2_STATIC',
               '-I' + str(package / 'vendor/pcre2'),
               '-I' + str(ROOT / 'bootstrap_compiler/std_packages/serde/vendor/tomlc17'),
               *map(str, native), '-o', str(out / 'native-engines')]
    subprocess.run(command, check=True)
    environment = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:abort_on_error=1', UBSAN_OPTIONS='halt_on_error=1')
    # LeakSanitizer is not available with Apple's system allocator.
    if __import__('sys').platform == 'darwin': environment['ASAN_OPTIONS'] = 'detect_leaks=0:abort_on_error=1'
    result = subprocess.run([str(out / 'native-engines')], env=environment, capture_output=True)
    (out / 'run.log').write_bytes(result.stdout + result.stderr)
    inputs = native + [ROOT / 'bootstrap_compiler/std_packages/serde/native/toml_bridge.c',
                      *sorted((package / 'vendor/pcre2').glob('*.h')),
                      ROOT / 'bootstrap_compiler/std_packages/serde/vendor/tomlc17/tomlc17.h']
    (out / 'verification.json').write_text(json.dumps(dict(command=command, exit=result.returncode,
        cc=str(shutil.which(args.cc)), sources={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}), indent=2)+'\n')
    print((result.stdout + result.stderr).decode(errors='replace'))
    raise SystemExit(result.returncode)


if __name__ == '__main__': main()
