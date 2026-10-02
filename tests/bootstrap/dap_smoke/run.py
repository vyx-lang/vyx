#!/usr/bin/env python3
import argparse
import json
import os
import pathlib
import queue
import shutil
import subprocess
import tempfile
import threading
import time


class DapClient:
    def __init__(self, executable: pathlib.Path, cwd: pathlib.Path, environment):
        self.process = subprocess.Popen(
            [str(executable)],
            cwd=cwd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=environment,
        )
        self.sequence = 1
        self.messages = queue.Queue()
        self.deferred = []
        threading.Thread(target=self._read_loop, daemon=True).start()

    def _read_loop(self):
        try:
            while True:
                length = None
                while True:
                    line = self.process.stdout.readline()
                    if not line:
                        raise EOFError("DAP stdout closed")
                    if line in (b"\r\n", b"\n"):
                        break
                    if line.lower().startswith(b"content-length:"):
                        length = int(line.split(b":", 1)[1])
                if length is None:
                    raise RuntimeError("missing Content-Length")
                body = self.process.stdout.read(length)
                if len(body) != length:
                    raise EOFError("short DAP response")
                self.messages.put(json.loads(body))
        except BaseException as exc:
            self.messages.put(exc)

    def request(self, command, arguments=None):
        sequence = self.sequence
        self.sequence += 1
        message = {
            "seq": sequence,
            "type": "request",
            "command": command,
            "arguments": arguments or {},
        }
        body = json.dumps(message, separators=(",", ":")).encode("utf-8")
        self.process.stdin.write(f"Content-Length: {len(body)}\r\n\r\n".encode("ascii"))
        self.process.stdin.write(body)
        self.process.stdin.flush()
        return sequence

    def wait_for(self, predicate, timeout=15):
        deadline = time.monotonic() + timeout
        while True:
            for index, message in enumerate(self.deferred):
                if predicate(message):
                    return self.deferred.pop(index)
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(f"DAP message timeout; deferred={self.deferred!r}")
            message = self.messages.get(timeout=remaining)
            if isinstance(message, BaseException):
                stderr = self.process.stderr.read().decode("utf-8", errors="replace")
                raise RuntimeError(f"DAP reader failed: {message}; stderr={stderr}") from message
            if predicate(message):
                return message
            self.deferred.append(message)

    def response(self, request_sequence, timeout=15):
        message = self.wait_for(
            lambda item: item.get("type") == "response" and item.get("request_seq") == request_sequence,
            timeout,
        )
        assert message.get("success"), message
        return message

    def event(self, name, timeout=15):
        return self.wait_for(
            lambda item: item.get("type") == "event" and item.get("event") == name,
            timeout,
        )

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait(timeout=3)


def run(compiler: pathlib.Path, adapter: pathlib.Path):
    case = pathlib.Path(tempfile.gettempdir()) / "vyx_dap_smoke"
    shutil.rmtree(case, ignore_errors=True)
    case.mkdir(parents=True)
    source_path = case / "main.vyx"
    program_path = case / ("main.exe" if os.name == "nt" else "main")
    if os.name == "nt":
        delay_declaration = 'extern "C" { fn Sleep(milliseconds: u32); fn fflush(stream: rawptr) -> i32; }'
        delay_call = "    Sleep(2000);"
    else:
        delay_declaration = 'extern "C" { fn usleep(microseconds: u32) -> i32; fn fflush(stream: rawptr) -> i32; }'
        delay_call = "    usleep(2000000);"
    source_lines = [
        "module dap_smoke;",
        delay_declaration,
        "fn add(a: i32, b: i32) {",
        "    let result = a + b;",
        "    return result;",
        "}",
        "fn main() -> i32 {",
        "    let value = add(20, 22);",
        "    print(value);",
        "    fflush(null);",
        delay_call,
        "    print(value + 1);",
        "    return 0;",
        "}",
    ]
    breakpoint_line = source_lines.index("    return result;") + 1
    source_path.write_text("\n".join(source_lines) + "\n", encoding="utf-8")
    compile_result = subprocess.run(
        [
            str(compiler),
            "-g",
            "--src=file",
            str(source_path),
            "--emit=exe",
            "-o",
            str(program_path),
        ],
        cwd=case,
        capture_output=True,
        text=True,
    )
    assert compile_result.returncode == 0, compile_result.stdout + compile_result.stderr
    assert program_path.is_file(), program_path

    environment = os.environ.copy()
    if os.name == "nt" and adapter.stem.lower() == "lldb-dap":
        python_home = pathlib.Path(
            subprocess.check_output(
                [os.environ.get("PYTHON", "python"), "-c", "import sys; print(sys.base_prefix)"],
                text=True,
            ).strip()
        )
        assert any(python_home.glob("python3??.dll")), f"Python runtime DLL not found in {python_home}"
        environment["PYTHONHOME"] = str(python_home)
        environment["PATH"] = str(python_home) + os.pathsep + environment.get("PATH", "")

    client = DapClient(adapter, case, environment)
    try:
        initialize = client.request(
            "initialize",
            {
                "clientID": "vyx-dap-smoke",
                "adapterID": "lldb",
                "pathFormat": "path",
                "linesStartAt1": True,
                "columnsStartAt1": True,
                "supportsVariableType": True,
                "supportsRunInTerminalRequest": False,
            },
        )
        capabilities = client.response(initialize)["body"]
        assert capabilities.get("supportsConfigurationDoneRequest")

        launch = client.request(
            "launch",
            {
                "program": str(program_path),
                "cwd": str(case),
                "args": [],
                "env": {},
                "stopOnEntry": False,
            },
        )
        client.event("initialized")

        set_breakpoints = client.request(
            "setBreakpoints",
            {
                "source": {"name": source_path.name, "path": str(source_path)},
                "breakpoints": [{"line": breakpoint_line}],
                "sourceModified": False,
            },
        )
        breakpoints = client.response(set_breakpoints)["body"]["breakpoints"]
        assert breakpoints and breakpoints[0].get("verified"), breakpoints

        configuration_done = client.request("configurationDone")
        client.response(configuration_done)
        client.response(launch)

        stopped = client.event("stopped")
        assert stopped["body"]["reason"] == "breakpoint", stopped
        thread_id = stopped["body"]["threadId"]

        stack_request = client.request("stackTrace", {"threadId": thread_id, "startFrame": 0, "levels": 20})
        frames = client.response(stack_request)["body"]["stackFrames"]
        source_frames = [frame for frame in frames if frame.get("source", {}).get("path") == str(source_path)]
        assert source_frames, frames
        frame = source_frames[0]
        assert frame["line"] == breakpoint_line, frame

        scopes_request = client.request("scopes", {"frameId": frame["id"]})
        scopes = client.response(scopes_request)["body"]["scopes"]
        assert scopes, scopes
        variable_names = set()
        for scope in scopes:
            reference = scope.get("variablesReference", 0)
            if not reference:
                continue
            variables_request = client.request("variables", {"variablesReference": reference})
            variables = client.response(variables_request)["body"]["variables"]
            variable_names.update(item["name"] for item in variables)
        assert {"a", "b"}.issubset(variable_names), variable_names

        hover_request = client.request(
            "evaluate",
            {"expression": "result", "frameId": frame["id"], "context": "hover"},
        )
        hover_body = client.response(hover_request)["body"]
        assert "42" in hover_body["result"], hover_body
        assert hover_body.get("type"), hover_body

        evaluate_request = client.request(
            "evaluate",
            {"expression": "a + b", "frameId": frame["id"], "context": "watch"},
        )
        evaluated = client.response(evaluate_request)["body"]["result"]
        assert "42" in evaluated, evaluated

        next_request = client.request("next", {"threadId": thread_id, "singleThread": True})
        client.response(next_request)
        stepped = client.event("stopped")
        assert stepped["body"]["reason"] == "step", stepped

        continue_request = client.request("continue", {"threadId": thread_id, "singleThread": False})
        stream_started = time.monotonic()
        client.response(continue_request)
        streamed = client.wait_for(
            lambda item: item.get("type") == "event"
            and item.get("event") == "output"
            and item.get("body", {}).get("category") == "stdout"
            and "42" in item.get("body", {}).get("output", ""),
            timeout=1.5,
        )
        stream_elapsed = time.monotonic() - stream_started
        assert "42" in streamed["body"]["output"], streamed
        assert stream_elapsed < 1.5, stream_elapsed
        client.event("exited")
        client.event("terminated")
        client.process.wait(timeout=5)
        stderr = client.process.stderr.read()
        assert not stderr, stderr.decode("utf-8", errors="replace")
        print(
            "DAP_SMOKE=PASS "
            f"breakpointLine={frame['line']} variables={','.join(sorted(variable_names))} "
            f"hover={hover_body['result']} evaluate={evaluated} streamMs={stream_elapsed * 1000:.0f}"
        )
    finally:
        client.close()
        shutil.rmtree(case, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("compiler", type=pathlib.Path)
    parser.add_argument("adapter", type=pathlib.Path)
    args = parser.parse_args()
    run(args.compiler.resolve(), args.adapter.resolve())


if __name__ == "__main__":
    main()
