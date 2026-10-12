#!/usr/bin/env python3
"""Run a real ELF parent/child regression for SDK/system LLVM isolation."""

import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import shutil
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--patchelf', default='patchelf')
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[3]
    sys.path.insert(0, str(repo / 'bootstrap_compiler/scripts'))
    spec = importlib.util.spec_from_file_location('termux_package',
                repo / 'bootstrap_compiler/scripts/package_termux_sdk.py')
    packaging = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(packaging)
    root = Path(tempfile.mkdtemp(prefix='vyx-llvm-environment-'))
    sdk = root / 'sdk'
    system = root / 'system'
    for path in (sdk / 'bin', sdk / 'lib', sdk / 'toolchain/lib', system):
        path.mkdir(parents=True, exist_ok=True)

    def compile_file(name, source, *options):
        path = root / (name + '.c')
        path.write_text(source)
        subprocess.run([args.cc, str(path), *options], check=True)

    compile_file('llvm20', 'int llvm20_tokenize(void) { return 20; }\n',
                 '-shared', '-fPIC', '-Wl,-soname,libLLVM.so', '-o', str(system / 'libLLVM.so'))
    version_script = root / 'llvm21.map'
    version_script.write_text('LLVM_21.1 { global: llvm21_tokenize; local: *; };\n')
    compile_file('llvm21', 'int llvm21_tokenize(void) { return 21; }\n',
                 '-shared', '-fPIC', '-Wl,-soname,libLLVM.so',
                 '-Wl,--version-script,' + str(version_script),
                 '-o', str(sdk / 'toolchain/lib/libLLVM.so'))
    clang = system / 'clang-20'
    compile_file('clang', 'extern int llvm20_tokenize(void);\n'
                 'int main(void) { return llvm20_tokenize() == 20 ? 0 : 1; }\n',
                 '-L' + str(system), '-lLLVM', '-Wl,-rpath,' + str(system), '-o', str(clang))
    parent = sdk / 'bin/vyxc.bin'
    compile_file('compiler', r'''
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
extern int llvm21_tokenize(void);
int main(int argc, char **argv) {
    if (llvm21_tokenize() != 21) return 2;
    if (argc == 2 && strcmp(argv[1], "--child") == 0) return 0;
    if (argc != 2) return 3;
    pid_t child = fork();
    if (child == 0) { execl(argv[1], argv[1], (char *)0); _exit(120); }
    int status;
    if (child < 0 || waitpid(child, &status, 0) != child) return 4;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return 5;
    child = fork();
    if (child == 0) { execl(argv[0], argv[0], "--child", (char *)0); _exit(121); }
    if (child < 0 || waitpid(child, &status, 0) != child) return 6;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 7;
}
''', '-L' + str(sdk / 'toolchain/lib'), '-lLLVM',
                 '-Wl,-rpath,$ORIGIN/../toolchain/lib', '-o', str(parent))
    inherited = os.environ.copy()
    inherited['LD_LIBRARY_PATH'] = str(sdk / 'toolchain/lib')
    before = subprocess.run([str(parent), str(clang)], env=inherited, capture_output=True, text=True)
    assert before.returncode != 0 and 'llvm20_tokenize' in before.stderr, before.stderr
    # Reproduce the previous Android failure: --replace-needed updates vn_file
    # to an absolute path while the provider SONAME remains libLLVM.so.
    legacy = root / 'legacy-vyxc.bin'
    shutil.copy2(parent, legacy)
    private_llvm = sdk / 'toolchain/lib/libLLVM.so'
    subprocess.run([args.patchelf, '--replace-needed', 'libLLVM.so',
                    str(private_llvm), str(legacy)], check=True)
    try:
        packaging.verify_version_associations(legacy, {str(private_llvm): private_llvm},
                                              frozenset({'libc.so.6'}))
    except ValueError as error:
        assert 'SONAME mismatch' in str(error), str(error)
    else:
        raise AssertionError('The old package must fail Android SONAME lookup')
    # Exercise the same binding helper as the Android package. This host uses
    # glibc, so the system SONAME and machine checks differ from Android.
    _, bindings = packaging.isolate_libraries(sdk, sdk, args.patchelf,
        system_libraries=frozenset({'libc.so.6'}), android=False)
    llvm_name = 'libvyx_sdk_libLLVM.so'
    requirements = packaging.verify_version_associations(parent, {llvm_name: private_llvm},
                                                         frozenset({'libc.so.6'}))
    llvm_requirement = next(record for record in requirements if record['file'] == llvm_name)
    assert [version['name'] for version in llvm_requirement['versions']] == ['LLVM_21.1']
    wrong_provider = root / 'unversioned-provider.so'
    shutil.copy2(system / 'libLLVM.so', wrong_provider)
    subprocess.run([args.patchelf, '--set-soname', llvm_name, str(wrong_provider)], check=True)
    try:
        packaging.verify_version_associations(parent, {llvm_name: wrong_provider},
                                              frozenset({'libc.so.6'}))
    except ValueError as error:
        assert 'Missing private symbol version' in str(error), str(error)
    else:
        raise AssertionError('A provider without LLVM_21.1 must be rejected')
    retained_hashes = {record['private_file']: packaging.sha256(sdk / record['private_file'])
                       for record in bindings}
    packaging.isolate_libraries(sdk, sdk, args.patchelf,
        system_libraries=frozenset({'libc.so.6'}), android=False,
        previous_namespace=bindings)
    assert retained_hashes == {name: packaging.sha256(sdk / name)
                               for name in retained_hashes}, 'Retained isolated libraries were rewritten'
    caller = os.environ.copy()
    caller['LD_LIBRARY_PATH'] = str(system)
    after = subprocess.run([str(parent), str(clang)], env=caller, capture_output=True, text=True)
    assert after.returncode == 0, after.stderr
    caller.pop('LD_LIBRARY_PATH')
    clean = subprocess.run([str(parent), str(clang)], env=caller, capture_output=True, text=True)
    assert clean.returncode == 0, clean.stderr
    result = {'before': 'reproduced child LLVM symbol failure',
              'after': 'private compiler + system clang + nested compiler pass',
              'caller_library_path': 'preserved', 'unset_library_path': 'pass',
              'previous_release_reuse': 'pass',
              'retained_private_library_hashes': 'unchanged',
              'old_android_verneed_mismatch': 'reproduced and rejected',
              'llvm_symbol_version_retained': 'LLVM_21.1',
              'wrong_version_provider': 'rejected',
              'platform': 'Linux ELF; not an Android device run'}
    (root / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
