#!/usr/bin/env python3
"""Repackage a prepared Termux payload with an isolated private ELF closure."""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import shlex
import struct
import subprocess
import tempfile

from termux_elf import metadata, verify_runpath, verify_version_associations


ANDROID_SYSTEM_LIBRARIES = frozenset({
    'libc.so', 'libm.so', 'libdl.so', 'liblog.so', 'libandroid.so', 'libz.so',
})
DEFAULT_PREFIX = '/data/data/com.termux/files/usr'
# These are incidental utilities in the LLVM dependency payload. They belong
# to Termux packages and may need libraries outside the private SDK closure.
NON_LLVM_UTILITIES = frozenset({
    'zstd', 'zstdcat', 'zstdgrep', 'zstdless', 'zstdmt', 'unzstd',
    'clear', 'reset', 'tset', 'ncursesw6-config', 'xml2-config',
})


def command(*args):
    result = subprocess.run(args, text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(f'{args[0]} exited {result.returncode}:\n'
                           f'{result.stdout}{result.stderr}')
    return result.stdout.strip()


def sha256(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def dynamic_elf(path):
    with path.open('rb') as source:
        header = source.read(64)
        if header[:4] != b'\x7fELF':
            return False
        if header[4:6] != b'\x02\x01':
            raise ValueError(f'Expected little-endian ELF64: {path}')
        offset = struct.unpack_from('<Q', header, 32)[0]
        entry_size, count = struct.unpack_from('<HH', header, 54)
        for index in range(count):
            source.seek(offset + index * entry_size)
            if struct.unpack('<I', source.read(4))[0] == 2:  # PT_DYNAMIC
                return True
    return False


def isolate_libraries(sdk, installed_sdk, patchelf, *,
                      system_libraries=ANDROID_SYSTEM_LIBRARIES, android=True,
                      previous_namespace=()):
    """Bind only this SDK's ELF dependencies; leave child environments untouched.

    Unique private SONAMEs and RUNPATH keep SDK libraries out of child tool
    environments. Preserve ordinary runtime libraries for application linking.
    """
    sdk = sdk.resolve()
    previous = {item['public_file']: item for item in previous_namespace}
    libraries = {}
    library_records = {}
    for directory in (sdk / 'lib', sdk / 'toolchain/lib'):
        for path in sorted(directory.glob('*.so*')):
            actual = path.resolve(strict=True)
            if not actual.is_relative_to(sdk):
                raise ValueError(f'Library alias escapes SDK: {path}')
            if not dynamic_elf(actual):
                raise ValueError(f'Expected dynamic ELF library: {path}')
            relative = path.relative_to(sdk).as_posix()
            measured_soname = command(patchelf, '--print-soname', str(actual))
            if actual not in library_records:
                saved = previous.get(relative)
                source_soname = saved['source_soname'] if saved else measured_soname
                # Android uses the loaded file's basename when DT_SONAME is
                # absent. Record that measured file identity explicitly.
                if not source_soname:
                    source_soname = actual.name
                if not source_soname or '/' in source_soname or '\\' in source_soname:
                    raise ValueError(f'Expected a plain library SONAME: {path}')
                backend = relative == 'lib/libvyx_compiler_backend.so'
                private_name = source_soname if backend else 'libvyx_sdk_' + source_soname
                private_path = actual if backend else sdk / 'toolchain/lib/llvm-private' / private_name
                if saved and measured_soname not in {source_soname, private_name}:
                    raise ValueError(f'Base payload SONAME differs from namespace manifest: {path}')
                library_records[actual] = {
                    'public_file': relative, 'source_soname': source_soname,
                    'private_soname': private_name, 'private_file': private_path.relative_to(sdk).as_posix(),
                    'actual': actual, 'private_path': private_path, 'aliases': [], 'backend': backend,
                    'retained': saved is not None,
                }
            record = library_records[actual]
            record['aliases'].append(path)
            for key in (path.name, measured_soname, record['source_soname'],
                        record['private_soname'], str(installed_sdk / relative),
                        str(installed_sdk / record['private_file'])):
                if not key:
                    continue
                if key in libraries and libraries[key] is not record:
                    raise ValueError(f'Ambiguous SDK SONAME {key}: {path}')
                libraries[key] = record

    elf_paths = sorted({path.resolve()
                        for directory in (sdk / 'bin', sdk / 'lib',
                                          sdk / 'toolchain/bin', sdk / 'toolchain/lib')
                        for path in directory.rglob('*')
                        if path.is_file() and dynamic_elf(path)})
    # Check every incoming dependency before creating the private namespace.
    original_dependencies = {}
    original_versions = {}
    for path in elf_paths:
        if not path.is_relative_to(sdk):
            raise ValueError(f'ELF alias escapes SDK: {path}')
        if android:
            with path.open('rb') as source:
                source.seek(18)
                if struct.unpack('<H', source.read(2))[0] != 183:
                    raise ValueError(f'Non-AArch64 ELF in Termux package: {path}')
        dependencies = command(patchelf, '--print-needed', str(path)).splitlines()
        original_dependencies[path] = dependencies
        original_versions[path] = [record['versions'] for record in metadata(path)['version_needs']]
        for dependency in dependencies:
            if dependency not in libraries and dependency not in system_libraries:
                raise ValueError(f'Unresolved SDK dependency {dependency} in {path}')

    namespace = sdk / 'toolchain/lib/llvm-private'
    namespace.mkdir(parents=True, exist_ok=True)
    public_paths = set()
    private_paths = set()
    for record in library_records.values():
        actual, private = record['actual'], record['private_path']
        if record['retained']:
            if not private.resolve(strict=True).is_relative_to(sdk):
                raise ValueError(f'Private library alias escapes SDK: {private}')
            if command(patchelf, '--print-soname', str(private)) != record['private_soname']:
                raise ValueError(f'Retained private library SONAME mismatch: {private}')
            private_paths.add(private)
            if actual != private:
                public_paths.add(actual)
            continue
        if record['backend']:
            private_paths.add(actual)
        elif record['source_soname'] == 'libLLVM.so':
            # LLVM is compiler-only. Move its large image instead of shipping
            # two copies; retain its ordinary linker filename as an alias.
            if actual != private:
                shutil.move(str(actual), str(private))
            for alias in record['aliases']:
                alias.unlink(missing_ok=True)
                alias.symlink_to(os.path.relpath(private, alias.parent))
            original_dependencies[private] = original_dependencies.pop(actual)
            original_versions[private] = original_versions.pop(actual)
            private_paths.add(private)
        else:
            shutil.copy2(actual, private)
            original_dependencies[private] = original_dependencies[actual]
            original_versions[private] = original_versions[actual]
            public_paths.add(actual)
            private_paths.add(private)
        if command(patchelf, '--print-soname', str(private)) != record['private_soname']:
            command(patchelf, '--page-size', '16384' if android else '4096',
                    '--set-soname', record['private_soname'], str(private))
        if actual in public_paths:
            if command(patchelf, '--print-soname', str(actual)) != record['source_soname']:
                command(patchelf, '--set-soname', record['source_soname'], str(actual))

    records = []
    for path, dependencies in sorted(original_dependencies.items()):
        public = path in public_paths
        replacements = []
        providers = {}
        for dependency in dependencies:
            if dependency in system_libraries:
                continue
            record = libraries[dependency]
            use_private = not public or record['source_soname'] == 'libLLVM.so' or record['backend']
            replacement = record['private_soname'] if use_private else record['source_soname']
            providers[replacement] = record['private_path'] if use_private else record['actual']
            if replacement != dependency:
                replacements.append((dependency, replacement))
        arguments = [patchelf]
        if android:
            arguments += ['--page-size', '16384']
        for previous, replacement in replacements:
            arguments += ['--replace-needed', previous, replacement]
        # Paths cover compiler/bin, backend/lib, LLVM tools/bin and the private
        # namespace itself. They never become inherited loader environment.
        runpath = '$ORIGIN:$ORIGIN/llvm-private:$ORIGIN/../lib:$ORIGIN/../lib/llvm-private:$ORIGIN/../toolchain/lib/llvm-private'
        # Do not rewrite an already isolated dependency. Repeated patchelf
        # relocation can damage previously enlarged Android load segments.
        change_runpath = command(patchelf, '--print-rpath', str(path)) != runpath
        if change_runpath:
            arguments += ['--set-rpath', runpath]
        if replacements or change_runpath:
            command(*arguments, str(path))
        dependencies = command(patchelf, '--print-needed', str(path)).splitlines()
        version_needs = verify_version_associations(path, providers, system_libraries)
        verify_runpath(path, providers)
        if [record['versions'] for record in version_needs] != original_versions[path]:
            raise ValueError(f'Packaging changed symbol version requirements: {path}')
        for dependency in dependencies:
            if dependency in system_libraries:
                continue
            if dependency not in providers or not providers[dependency].is_file():
                raise ValueError(f'Missing installed SDK dependency: {dependency}')
        records.append({'file': path.relative_to(sdk).as_posix(),
                        'needed': dependencies, 'version_needs': version_needs,
                        'providers': {name: provider.relative_to(sdk).as_posix() for name, provider in providers.items()},
                        'sha256': sha256(path)})
    bindings = [{key: record[key] for key in ('public_file', 'source_soname', 'private_soname', 'private_file')}
                for record in library_records.values()]
    return records, bindings


def package(args):
    work = args.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=True)
    # WSL's mounted NTFS may expose DEBIAN as mode 0777 even after chmod.
    # Keep package trees on the Linux filesystem; only results use work_dir.
    stage = Path(tempfile.mkdtemp(prefix='vyx-termux-stage-'))
    command('dpkg-deb', '-R', str(args.base_package.resolve()), str(stage))
    prefix = PurePosixPath(args.prefix)
    if not prefix.is_absolute() or '..' in prefix.parts:
        raise ValueError('Termux prefix must be an absolute normalized path')
    installed_sdk = prefix / 'lib/vyx-sdk'
    sdk = stage / str(installed_sdk).lstrip('/')
    source_root = args.source_root.resolve() if args.source_root else None
    if source_root:
        assets = {
            'std': source_root / 'bootstrap_compiler/std',
            'std_packages': source_root / 'bootstrap_compiler/std_packages',
            'tools': source_root / 'bootstrap_compiler/tools',
        }
        for relative, source in assets.items():
            if not source.is_dir():
                raise ValueError(f'Missing current SDK source assets: {source}')
            destination = sdk / relative
            if destination.exists():
                if not destination.resolve().is_relative_to(sdk.resolve()):
                    raise ValueError(f'SDK source destination escapes staging: {destination}')
                shutil.rmtree(destination)
            shutil.copytree(source, destination, ignore=shutil.ignore_patterns(
                '.git', '.cache', 'out', 'target', '__pycache__', '*.pyc', '*.exe'))
        shutil.copytree(source_root / 'tools/dci', sdk / 'tools/dci', dirs_exist_ok=True,
                        ignore=shutil.ignore_patterns('.git', '.cache', 'out', 'target', '__pycache__', '*.pyc', '*.exe'))
        shutil.copytree(source_root / 'tools/packages', sdk / 'tools/packages', dirs_exist_ok=True,
                        ignore=shutil.ignore_patterns('.git', '.cache', 'out', 'target', '__pycache__', '*.pyc', '*.exe'))
        shutil.copy2(source_root / 'tools/__init__.py', sdk / 'tools/__init__.py')
        for notice in ('LICENSE', 'LICENSE-MIT', 'LICENSE-APACHE'):
            shutil.copy2(source_root / notice, sdk / notice)
    out = args.compiler_out.resolve()
    runtime = out / 'runtimes/android/arm64/ndk'
    inputs = {
        'bin/vyxc.bin': out / 'boot',
        'lib/libvyx_compiler_backend.so': runtime / 'libvyx_compiler_backend.so',
        'lib/libvyx_runtime.a': runtime / 'libvyx_runtime.a',
        'lib/vyx_runtime.vyi': runtime / 'vyx_runtime.vyi',
        'lib/vyx_runtime.vyi.vyx-cost': runtime / 'vyx_runtime.vyi.vyx-cost',
    }
    input_hashes = {relative: sha256(source) for relative, source in inputs.items()}
    for relative, source in inputs.items():
        shutil.copy2(source, sdk / relative)
    os.chmod(sdk / 'bin/vyxc.bin', 0o755)
    for name in NON_LLVM_UTILITIES:
        utility = sdk / 'toolchain/bin' / name
        if utility.exists() or utility.is_symlink():
            utility.unlink()
    manifest_path = sdk / 'SDK-MANIFEST.json'
    manifest = json.loads(manifest_path.read_text())
    # Newly built libraries have their own measured SONAMEs. Namespace records
    # from the base package describe only the payload retained from that base.
    retained_namespace = [record for record in manifest.get('private_libraries', ())
                          if record['public_file'] not in inputs]
    records, bindings = isolate_libraries(sdk, installed_sdk, args.patchelf,
                                         previous_namespace=retained_namespace)

    launcher = stage / str(prefix / 'bin/vyxc').lstrip('/')
    launcher.write_text(f'''#!/data/data/com.termux/files/usr/bin/sh
set -eu
VYX_HOME={shlex.quote(str(installed_sdk))}
export VYX_HOME
export LLVM_ROOT="$VYX_HOME/toolchain"
export LLVM_BIN="$VYX_HOME/toolchain/bin"
export VYX_RUNTIME_ROOT="$VYX_HOME/lib"
export VYX_STD_PACKAGES="$VYX_HOME/std_packages"
export VYX_DCI_TOOLS="$VYX_HOME/tools/dci"
export PYTHONPATH="$VYX_HOME${{PYTHONPATH:+:$PYTHONPATH}}"
# ELF dependencies select private SDK libraries. Keep the caller's loader
# environment for Termux clang, linkers, Python and other child processes.
exec "$VYX_HOME/bin/vyxc.bin" "$@"
''')
    os.chmod(launcher, 0o755)
    command('sh', '-n', str(launcher))
    manifest.update(version=args.version, private_library_binding='private-soname-runpath',
                    private_libraries=bindings,
                    install_prefix=str(prefix), exports_ld_library_path=False,
                    input_artifact_sha256=input_hashes,
                    artifact_sha256={relative: sha256(sdk / relative) for relative in inputs})
    if source_root:
        manifest['source_asset_sha256'] = {
            path.relative_to(sdk).as_posix(): sha256(path)
            for relative in ('std', 'std_packages', 'tools')
            for path in sorted((sdk / relative).rglob('*')) if path.is_file()
        }
    manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')
    control = stage / 'DEBIAN/control'
    control.write_text('\n'.join('Version: ' + args.version if line.startswith('Version: ')
                                 else line for line in control.read_text().splitlines()) + '\n')
    temporary = work / 'verified.deb'
    print('Building Termux package...', flush=True)
    print(command('dpkg-deb', '--build', '--root-owner-group', '-Zxz', '-z6',
                  str(stage), str(temporary)), flush=True)
    inspection = Path(tempfile.mkdtemp(prefix='vyx-termux-verify-'))
    command('dpkg-deb', '-R', str(temporary), str(inspection))
    installed = inspection / str(installed_sdk).lstrip('/')
    for record in records:
        assert sha256(installed / record['file']) == record['sha256'], record['file']
        providers = {dependency: installed / relative for dependency, relative in record['providers'].items()}
        verify_version_associations(installed / record['file'], providers, ANDROID_SYSTEM_LIBRARIES)
        verify_runpath(installed / record['file'], providers)
    for relative, digest in manifest['artifact_sha256'].items():
        assert sha256(installed / relative) == digest, relative
    for relative, digest in manifest.get('source_asset_sha256', {}).items():
        assert sha256(installed / relative) == digest, relative
    assert 'LD_LIBRARY_PATH=' not in launcher.read_text()
    assert b'insufficient or unknown build memory headroom' not in (installed / 'bin/vyxc.bin').read_bytes()
    assert not (installed / 'runtimes/android/arm64/ndk').exists()
    assert not any(path.suffix == '.exe' or path.name in {'.cache', '__pycache__', 'target'}
                   for path in installed.rglob('*'))
    for name in ('LICENSE', 'LICENSE-MIT', 'LICENSE-APACHE'):
        assert (installed / name).is_file(), name
    destination = args.destination.resolve()
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        shutil.copy2(destination, work / ('previous-' + sha256(destination)[:12] + '.deb'))
    os.replace(temporary, destination)
    digest = sha256(destination)
    destination.with_name(destination.name + '.sha256').write_text(digest + '  ' + destination.name + '\n')
    result = {'package': str(destination), 'version': args.version, 'sha256': digest,
              'size_bytes': destination.stat().st_size, 'input_artifact_sha256': input_hashes,
              'artifact_sha256': manifest['artifact_sha256'], 'elf_closure': records,
              'validation': 'package roundtrip, private closure, Android SONAME/verneed, private symbol versions, loader environment, licenses',
              'not_executed': ['Android device install', 'Termux native project build']}
    if source_root:
        result['source_root'] = str(source_root)
        result['source_asset_count'] = len(manifest['source_asset_sha256'])
    (work / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({key: value for key, value in result.items() if key != 'elf_closure'}, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base-package', required=True, type=Path)
    parser.add_argument('--compiler-out', required=True, type=Path)
    parser.add_argument('--source-root', type=Path,
                        help='Current source snapshot supplying standard library, DCI tools and licenses')
    parser.add_argument('--destination', required=True, type=Path)
    parser.add_argument('--work-dir', required=True, type=Path)
    parser.add_argument('--version', required=True)
    parser.add_argument('--prefix', default=DEFAULT_PREFIX)
    parser.add_argument('--patchelf', default='patchelf')
    package(parser.parse_args())


if __name__ == '__main__':
    main()
