"""POSIX gate processes: Linux address-space bounds, Darwin sampled group RSS."""
import os
import signal
import subprocess
import sys
import time


SAMPLE_SECONDS = 0.05


def memory_policy(platform=None):
    if (platform or sys.platform) == "darwin":
        return "Darwin process-group sampled RSS"
    return "per-process RLIMIT_AS"


def parse_group_rss(text, group):
    total_kib = 0
    for line in text.splitlines():
        if not line.strip():
            continue
        fields = line.split()
        if len(fields) != 2:
            raise RuntimeError("RSS monitor returned malformed pgid/rss fields")
        try:
            pgid = int(fields[0])
        except ValueError as error:
            raise RuntimeError("RSS monitor returned nonnumeric pgid fields") from error
        if pgid < 0:
            raise RuntimeError("RSS monitor returned negative pgid fields")
        if pgid == group:
            try:
                rss = int(fields[1])
            except ValueError as error:
                raise RuntimeError("RSS monitor returned nonnumeric rss fields") from error
            if rss < 0:
                raise RuntimeError("RSS monitor returned negative rss fields")
            total_kib += rss
    return total_kib * 1024


def sample_group_rss(group, timeout):
    result = subprocess.run(["/bin/ps", "-A", "-o", "pgid=,rss="], capture_output=True,
                            text=True, timeout=timeout, env={**os.environ, "LC_ALL": "C"})
    if result.returncode != 0:
        raise RuntimeError(f"RSS monitor ps failed ({result.returncode}): {result.stderr.strip()}")
    if not result.stdout.strip():
        raise RuntimeError("RSS monitor returned no process snapshot")
    return parse_group_rss(result.stdout, group)


def execute(command, *, cwd, env, stdout, stderr, timeout, memory_mb, platform=None):
    if timeout <= 0 or memory_mb <= 0:
        raise ValueError("POSIX execution requires positive time and memory limits")
    darwin = (platform or sys.platform) == "darwin"
    limit = memory_mb * 1024 * 1024

    def address_space_limit():
        import resource
        resource.setrlimit(resource.RLIMIT_AS, (limit, limit))

    # Darwin does not provide Linux's usable RLIMIT_AS contract. In particular,
    # do not attempt it in preexec_fn, where setup errors obscure even --version.
    proc = subprocess.Popen(command, cwd=cwd, env=env, stdout=stdout, stderr=stderr,
                            start_new_session=True,
                            preexec_fn=None if darwin else address_space_limit)
    timed_out = False
    memory_exceeded = False
    peak_rss = None
    deadline = time.monotonic() + timeout
    try:
        if darwin:
            while proc.poll() is None:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    timed_out = True
                    break
                try:
                    rss = sample_group_rss(proc.pid, min(2, remaining))
                except subprocess.TimeoutExpired:
                    if time.monotonic() >= deadline:
                        timed_out = True
                        break
                    raise RuntimeError("RSS monitor ps timed out")
                peak_rss = max(peak_rss or 0, rss)
                if rss > limit:
                    memory_exceeded = True
                    break
                try:
                    proc.wait(timeout=min(SAMPLE_SECONDS, max(0.001, deadline - time.monotonic())))
                except subprocess.TimeoutExpired:
                    pass
        else:
            try:
                proc.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                timed_out = True
    finally:
        # Includes descendants left behind by an exited root and monitor errors.
        # Only this invocation's new process group is terminated.
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        proc.wait()
    exit_code = -902 if memory_exceeded else -901 if timed_out else proc.returncode
    return {"ExitCode": exit_code, "TimedOut": timed_out, "MemoryExceeded": memory_exceeded,
            "DialogCaught": False, "JobLimited": False, "PeakWorkingSetMB": None,
            "PeakPrivateCommitMB": None, "MemoryPolicy": memory_policy(platform),
            "SampledPeakGroupRssMB": None if peak_rss is None else round(peak_rss / (1024 * 1024), 3),
            "MemorySampleIntervalMs": SAMPLE_SECONDS * 1000 if darwin else None}
