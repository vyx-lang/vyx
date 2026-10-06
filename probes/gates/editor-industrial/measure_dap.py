"""Exercise the actual SDK DAP engine: deep stacks, paged STL, stops and output."""
import argparse
import importlib.util
import json
import os
import pathlib
import subprocess
import time

import psutil

ROOT = pathlib.Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("dap_smoke", ROOT / "tests/bootstrap/dap_smoke/run.py")
smoke = importlib.util.module_from_spec(spec)
spec.loader.exec_module(smoke)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--adapter", type=pathlib.Path, required=True)
    ap.add_argument("--cxx", type=pathlib.Path, required=True)
    ap.add_argument("--result", type=pathlib.Path, required=True)
    ap.add_argument("--limit-mb", type=float, default=768)
    ap.add_argument("--growth-mb", type=float, default=64)
    args = ap.parse_args()
    result_path = args.result.resolve()
    result_path.parent.mkdir(parents=True, exist_ok=True)
    source = pathlib.Path(__file__).with_name("native_debuggee.cpp")
    exe = result_path.parent / ("native_debuggee.exe" if os.name == "nt" else "native_debuggee")
    command = [str(args.cxx.resolve()), "-g", "-O0", str(source), "-o", str(exe)]
    if os.name == "nt":
        command += ["-gcodeview", "-fuse-ld=lld", "-Wl,/debug", "-Wl,/incremental:no"]
    compiled = subprocess.run(command, capture_output=True, text=True)
    if compiled.returncode:
        raise RuntimeError(compiled.stdout + compiled.stderr)
    line = next(i for i, s in enumerate(source.read_text().splitlines(), 1) if "DAP_BREAKPOINT" in s)
    client = smoke.DapClient(args.adapter.resolve(), exe.parent, os.environ.copy())
    result = {"adapter": str(args.adapter.resolve()), "status": "failed", "stops": 100,
              "records": 100000, "stack_depth": 32, "compile_command": command}
    memory = []
    started = time.monotonic()
    target_pid = None

    def sample():
        parent = psutil.Process(client.process.pid)
        processes = [parent] + parent.children(recursive=True)
        total = 0
        for p in processes:
            try:
                if p.pid == target_pid or p.name().lower().startswith("native_debuggee"):
                    continue
                info = p.memory_info()
                total += getattr(info, "private", info.rss)
            except psutil.NoSuchProcess:
                pass
        mb = total / 1024**2
        memory.append(mb)
        if mb > args.limit_mb:
            raise MemoryError(f"adapter family exceeded {args.limit_mb} MiB: {mb:.1f}")
        return mb

    try:
        caps = client.response(client.request("initialize", {
            "adapterID": "vyx", "pathFormat": "path", "linesStartAt1": True,
            "columnsStartAt1": True, "supportsVariablePaging": True,
            "supportsRunInTerminalRequest": False,
        }))["body"]
        assert caps.get("supportsConfigurationDoneRequest"), caps
        launch = client.request("launch", {"program": str(exe), "cwd": str(exe.parent), "stopOnEntry": False})
        client.event("initialized")
        breakpoints = client.response(client.request("setBreakpoints", {
            "source": {"path": str(source)}, "breakpoints": [{"line": line}],
        }))["body"]["breakpoints"]
        assert breakpoints[0].get("verified"), breakpoints
        client.response(client.request("configurationDone"))
        client.response(launch)
        warm = None
        for stop in range(100):
            event = client.event("stopped")
            thread = event["body"]["threadId"]
            frames = client.response(client.request("stackTrace", {
                "threadId": thread, "startFrame": 0, "levels": 5,
            }))["body"]["stackFrames"]
            assert 0 < len(frames) <= 5, len(frames)
            scopes = client.response(client.request("scopes", {"frameId": frames[0]["id"]}))["body"]["scopes"]
            locals_scope = next(s for s in scopes if s["name"].lower() in ("locals", "local"))
            variables = client.response(client.request("variables", {
                "variablesReference": locals_scope["variablesReference"], "start": 0, "count": 32,
            }))["body"]["variables"]
            vector = next(v for v in variables if v["name"] == "records")
            page = client.response(client.request("variables", {
                "variablesReference": vector["variablesReference"], "filter": "indexed", "start": 50000, "count": 32,
            }))["body"]["variables"]
            assert len(page) == 32, f"variable pagination returned {len(page)} children"
            assert page[0]["name"] == "[50000]" and page[-1]["name"] == "[50031]", page
            assert vector.get("indexedVariables") == 100000, vector
            if stop == 0:
                empty = client.response(client.request("variables", {
                    "variablesReference": vector["variablesReference"], "filter": "indexed", "start": 100001, "count": 32,
                }))["body"]["variables"]
                assert empty == [], empty
                named = client.response(client.request("variables", {
                    "variablesReference": vector["variablesReference"], "filter": "named", "start": 0, "count": 1,
                }))["body"]["variables"]
                assert len(named) == 1 and named[0]["name"] == "[raw]", named
                global_scope = next(s for s in scopes if s["name"].lower() in ("globals", "global"))
                globals_page = client.response(client.request("variables", {
                    "variablesReference": global_scope["variablesReference"], "start": 10, "count": 5,
                }))["body"]["variables"]
                assert len(globals_page) <= 5, globals_page
            client.response(client.request("evaluate", {
                "expression": "iteration", "frameId": frames[0]["id"], "context": "watch",
            }))
            mb = sample()
            if stop == 9:
                warm = mb
            client.response(client.request("continue", {"threadId": thread}))
        result.update(warm_mb=warm, stopped_mb=memory[-1], stop_growth_mb=memory[-1] - warm)
        assert memory[-1] - warm <= args.growth_mb, result
        # Drain events while running. Preserve no output transcript in the client.
        output_bytes = 0
        while True:
            event = client.wait_for(lambda m: m.get("type") == "event" and m.get("event") in ("output", "exited"), 30)
            if event["event"] == "exited":
                assert event["body"]["exitCode"] == 0, event
                break
            if event.get("body", {}).get("category") == "stdout":
                output_bytes += len(event["body"].get("output", "").encode())
            sample()
        client.event("terminated")
        result.update(output_bytes=output_bytes, output_finished_mb=sample())
        assert output_bytes >= 4 * 1024**2, result
        client.response(client.request("disconnect", {"terminateDebuggee": True}))
        client.process.stdin.close()
        client.process.wait(timeout=10)
        assert client.process.returncode == 0
        result["status"] = "passed"
    except Exception as exc:
        result["error"] = f"{type(exc).__name__}: {exc}"
    finally:
        result.update(peak_private_mb=max(memory, default=0), elapsed_seconds=time.monotonic() - started,
                      stderr="".join(client.stderr_lines))
        client.close()
        result_path.write_text(json.dumps(result, indent=2), encoding="utf-8")
        print(json.dumps({k: v for k, v in result.items() if k not in ("compile_command", "stderr")}))
    return 0 if result["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
