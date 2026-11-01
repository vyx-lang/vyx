"""Measure real CLI discovery/report scaling; optionally compare another compiler.

Timings are evidence, not a hardware-independent CI pass/fail threshold. Each
timed invocation must enumerate every generated test and produce valid JSON.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import tempfile
import time


def counts(text: str) -> list[int]:
    values = [int(value) for value in text.split(',')]
    if not values or any(value < 1 or value > 10000 for value in values):
        raise argparse.ArgumentTypeError('counts must be comma-separated integers in 1..10000')
    return values


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--reference', type=Path)
    parser.add_argument('--tests', type=counts, default=counts('512,1024,2048'))
    parser.add_argument('--files', type=counts, default=counts('64,128,256'))
    parser.add_argument('--runs', type=int, default=3)
    parser.add_argument('--timeout', type=float, default=60)
    parser.add_argument('--output', type=Path, default=Path(__file__).parent / '.cache/scaling.json')
    args = parser.parse_args()
    if not 1 <= args.runs <= 20 or not 0 < args.timeout <= 600:
        parser.error('runs must be 1..20 and timeout must be positive and at most 600 seconds')
    compilers = {'current': args.compiler.resolve(strict=True)}
    if args.reference:
        compilers['reference'] = args.reference.resolve(strict=True)
    record = {
        'schema_version': 1,
        'compilers': {label: {'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
                      for label, path in compilers.items()},
        'runs': [], 'summaries': [],
    }
    args.output = args.output.resolve()
    args.output.parent.mkdir(parents=True, exist_ok=True)

    def save() -> None:
        args.output.write_text(json.dumps(record, indent=2), encoding='utf-8')

    def invoke(label: str, directory: Path, expected: set[str], warmup: bool = False) -> float:
        compiler = compilers[label]
        env = dict(os.environ)
        env['PATH'] = str(compiler.parent) + os.pathsep + env.get('PATH', '')
        command = [str(compiler), 'test', '--standalone', '--list', '--report', 'report.json']
        started = time.perf_counter()
        result = subprocess.run(command, cwd=directory, env=env, capture_output=True, timeout=args.timeout)
        elapsed = time.perf_counter() - started
        if result.returncode != 0:
            raise RuntimeError(f'{label}: exit {result.returncode}: {(result.stdout + result.stderr).decode(errors="replace")}')
        report = json.loads((directory / 'report.json').read_text(encoding='utf-8'))
        if (report['summary']['selected'] != len(expected) or len(report['results']) != len(expected)
                or {row['name'] for row in report['results']} != expected
                or any(row['status'] != 'listed' for row in report['results'])):
            raise RuntimeError(f'{label}: incomplete discovery/report for {len(expected)} tests')
        if not warmup:
            record['runs'].append({'compiler': label, 'mode': directory.name.split('-')[0],
                                   'count': len(expected), 'seconds': elapsed, 'command': command,
                                   'report_bytes': (directory / 'report.json').stat().st_size})
            save()
        return elapsed

    with tempfile.TemporaryDirectory(prefix='vyxc runner scaling ') as temp:
        root = Path(temp)
        warm = root / 'warmup/tests'
        warm.mkdir(parents=True)
        (warm / 'a.vyx').write_text('@[test] fn warmup(){}', encoding='utf-8')
        for label in compilers:
            invoke(label, warm.parent, {(warm / 'a.vyx').as_posix() + '::warmup'}, warmup=True)
        for mode, sizes in (('tests', args.tests), ('files', args.files)):
            for count in sizes:
                directory = root / f'{mode}-{count}'
                (directory / 'tests').mkdir(parents=True)
                if mode == 'tests':
                    (directory / 'tests/a.vyx').write_text(
                        '\n'.join(f'@[test] fn case_{index:05}(){{}}' for index in range(count)), encoding='utf-8')
                    identities = {(directory / 'tests/a.vyx').as_posix() + f'::case_{index:05}' for index in range(count)}
                else:
                    for index in range(count):
                        (directory / f'tests/case_{index:05}.vyx').write_text('@[test] fn case_test(){}', encoding='utf-8')
                    identities = {(directory / f'tests/case_{index:05}.vyx').as_posix() + '::case_test' for index in range(count)}
                for iteration in range(args.runs):
                    labels = list(compilers)
                    if iteration % 2:
                        labels.reverse()
                    for label in labels:
                        elapsed = invoke(label, directory, identities)
                        print(f'{mode} {count} {label} run {iteration + 1}: {elapsed:.4f}s', flush=True)
                for label in compilers:
                    times = [row['seconds'] for row in record['runs']
                             if row['compiler'] == label and row['mode'] == mode and row['count'] == count]
                    record['summaries'].append({'compiler': label, 'mode': mode, 'count': count,
                                                'median_seconds': statistics.median(times),
                                                'min_seconds': min(times), 'max_seconds': max(times)})
                save()
    print(f'Scaling evidence: {args.output}', flush=True)


if __name__ == '__main__':
    main()
