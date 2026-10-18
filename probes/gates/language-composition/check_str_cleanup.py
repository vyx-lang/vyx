"""Count actual allocations/releases in emitted str-iteration code (Windows AOT)."""
import argparse
import json
import os
import re
import shutil
from pathlib import Path
from suite import Executor, compiler_identity, digest, log_text, unique_dir, write_json

def succeeded(result):
    status = result['result']
    return status['ExitCode'] == 0 and not any(status[k] for k in ('TimedOut', 'MemoryExceeded', 'DialogCaught'))

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', required=True)
    parser.add_argument('--out', required=True)
    parser.add_argument('--cases', default='str_clone_iteration',
        help='comma separated allocation fixtures from the language-composition edges')
    args = parser.parse_args()
    if os.name != 'nt':
        print('Windows CRT allocation interception gate: skipped on this platform')
        return 77
    here = Path(__file__).resolve().parent
    clone_counts = {'str_clone_iteration': 4, 'str_array_iteration': 4,
        'owned_array_clone': -1, 'owned_array_lifetime': -1, 'format_cleanup': 5,
        'conditional_raw_param_lifetime': -1, 'conditional_owned_param_lifetime': -1,
        'named_owned_arguments': -1, 'ordinary_args_ownership': -1}
    selected = args.cases.split(',')
    if len(set(selected)) != len(selected) or any(name not in clone_counts for name in selected):
        parser.error('unknown or duplicate allocation fixture')
    expected = json.loads((here/'edges/expected.json').read_text(encoding='utf8'))
    repo = here.parents[2]
    out = unique_dir(args.out)
    engine = Executor(Path(args.compiler), 120, 2048)
    rows = []
    try:
        identity = compiler_identity(engine)
        sdk = engine.compiler.parent
        clang = repo/'clang/bin/clang++.exe'
        for name, opt in ((name, opt) for name in selected for opt in ('0', '2')):
            directory = out/f'{name}-O{opt}'
            directory.mkdir()
            source = directory/'main.vyx'
            shutil.copyfile(here/f'edges/{name}.vyx', source)
            ir = directory/'probe.ll'
            build = engine.execute(engine.compiler, ['--src=file', source, '--emit=ir', f'-O{opt}', '-o', ir], directory, 'compile', '1')
            row = {'name': name, 'optimization': opt, 'expected_clones': clone_counts[name],
                'source_sha256': digest(source), 'build': build, 'pass': False}
            if succeeded(build):
                text = ir.read_text(encoding='utf8')
                hooks = {'malloc': 'probe_malloc', 'calloc': 'probe_calloc', 'free': 'probe_free'}
                for symbol, hook in hooks.items():
                    text = re.sub(r'@(vyx_runtime_'+symbol+'|'+symbol+r')(?=[(\s])', '@'+hook, text)
                for symbol, hook in (('vyx_string_clone_len_abi', 'probe_clone'), ('vyx_string_alloc_abi', 'probe_string_alloc'),
                    ('vyx_free_runtime_string', 'probe_runtime_free'), ('vyx_free_pooled_string', 'probe_pooled_free')):
                    text = re.sub('@'+symbol+r'(?=[(\s])', '@'+hook, text)
                text, count = re.subn(r'@main(?=\()', '@probe_entry', text)
                if count != 1:
                    raise ValueError(f'expected one native main definition, found {count}')
                hooked = directory/'hooked.ll'
                hooked.write_text(text, encoding='utf8')
                exe = directory/'probe.exe'
                link = engine.execute(clang, [hooked, here/'native/str_cleanup.cpp', f'-L{sdk}',
                    '-lvyx_runtime', '-ltbb12', '-lsynchronization', '-lws2_32', '-std=c++17',
                    '-fms-runtime-lib=static', '-fuse-ld=lld', '-o', exe], directory, 'link', '1')
                row.update(link=link, hooked_ir_sha256=digest(hooked), harness_sha256=digest(here/'native/str_cleanup.cpp'))
                if succeeded(link):
                    run = engine.execute(exe, [str(clone_counts[name])], directory, 'run', '1')
                    actual = log_text(directory/'run.stdout.log').replace('\r\n', '\n')
                    counts = re.search(r'tracked=(\d+)/(\d+) live=(\d+) clones=(\d+)\n$', actual)
                    output = expected[name]['output']
                    row.update(run=run, output=actual)
                    row['pass'] = (succeeded(run) and counts is not None and actual.startswith(output)
                        and counts[1] == counts[2] and int(counts[1]) > 0 and counts[3] == '0'
                        and (clone_counts[name] < 0 or int(counts[4]) == clone_counts[name])
                        and not log_text(directory/'run.stderr.log'))
            rows.append(row)
            write_json(directory/'result.json', row)
            print(f'{name} allocation balance/O{opt}: {"pass" if row["pass"] else "FAIL"}', flush=True)
        write_json(out/'results.json', {'compiler': identity, 'results': rows,
            'passed': sum(r['pass'] for r in rows), 'failed': sum(not r['pass'] for r in rows)})
    finally:
        engine.close()
    return 0 if all(r['pass'] for r in rows) else 1

if __name__ == '__main__':
    raise SystemExit(main())
