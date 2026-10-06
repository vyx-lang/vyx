"""Verify SDK launcher cleanup on disconnect, protocol EOF and wrapper exit."""
import argparse
import os
import pathlib
import subprocess
import time

import psutil
from measure_dap import smoke


def assert_gone(processes):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        alive = []
        for pid, created in processes:
            try:
                p = psutil.Process(pid)
                if p.create_time() == created and p.status() != psutil.STATUS_ZOMBIE:
                    alive.append(pid)
            except psutil.NoSuchProcess:
                pass
        if not alive:
            return
        time.sleep(.05)
    raise AssertionError(f'debug session processes survive closure: {alive}')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--adapter', type=pathlib.Path, required=True)
    parser.add_argument('--program', type=pathlib.Path, required=True)
    args = parser.parse_args()
    adapter, program = args.adapter.resolve(), args.program.resolve()
    environment = os.environ.copy()
    environment['VYX_DAP_ADAPTER'] = str(program.parent / 'missing-adapter.exe')
    invalid = subprocess.run([str(adapter)], env=environment, capture_output=True, timeout=10)
    assert invalid.returncode != 0 and not invalid.stdout, 'explicit invalid adapter was accepted'

    for mode in ('disconnect', 'eof', 'wrapper-exit'):
        client = smoke.DapClient(adapter, program.parent, os.environ.copy())
        children = []
        try:
            client.response(client.request('initialize', {'adapterID': 'vyx', 'pathFormat': 'path',
                'linesStartAt1': True, 'columnsStartAt1': True}))
            launch = client.request('launch', {'program': str(program), 'cwd': str(program.parent), 'stopOnEntry': True})
            client.event('initialized')
            client.response(client.request('configurationDone'))
            client.response(launch)
            client.event('stopped')
            parent = psutil.Process(client.process.pid)
            descendants = parent.children(recursive=True)
            assert any(p.name().lower() == program.name.lower() for p in descendants), 'debuggee missing'
            children = [(p.pid, p.create_time()) for p in descendants]
            if mode == 'disconnect':
                client.response(client.request('disconnect', {'terminateDebuggee': True}))
                client.process.stdin.close()
                client.process.wait(timeout=10)
            elif mode == 'eof':
                client.process.stdin.close()
                client.process.wait(timeout=10)
            else:
                client.close()
            assert_gone(children)
        finally:
            client.close()
            # Cleanup only processes created by this test, even on a failed assertion.
            for pid, created in reversed(children):
                try:
                    p = psutil.Process(pid)
                    if p.create_time() == created:
                        p.kill()
                except psutil.NoSuchProcess:
                    pass
        print(f'DAP_CLEANUP=PASS mode={mode}')
    print('DAP_INVALID_OVERRIDE=PASS')


if __name__ == '__main__':
    main()
