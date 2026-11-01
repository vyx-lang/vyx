# SDK test / benchmark runner gate

`run.py --compiler <fresh-sdk-compiler>` tests the actual `vyxc test` and
`vyxc bench` commands.
It creates temporary projects, including paths containing spaces, then checks
discovery, execution and failure reporting. It records the compiler SHA-256,
commands, exit codes and captured output in `.cache/verification.json`.

The regression includes an existing application `main`, calls to that `main`
from a test, void assertions, integer statuses including 256, parser/lexer
errors, unsupported signatures, nonexistent paths, incorrect options, exact
and substring filters, duplicate inputs, two tests with the same name in
different files, child crashes, timeouts, fail-fast and concurrent invocations.
An empty selection is a failure unless `--allow-empty` was explicitly supplied.

```powershell
python probes/gates/test-runner/run.py --compiler bootstrap_compiler/out/vyxc.exe
./probes/gates/cli-tools/run.ps1 -Compiler bootstrap_compiler/out/vyxc.exe
```

Compiler test semantics and current limits are documented in the
[testing guide](../../../docs/TESTING_GUIDE.md#sdk-test-and-benchmark-commands).
The gate also exercises source-package dependencies, native C linking, private
unit helpers, application output preservation, dependency cache invalidation,
ignore selection, repeated isolated execution, JSON/JUnit parsing and host platform
selection. Benchmark checks independently recompute mean, median and standard
deviation from real child-process samples and test baseline validation/regressions.
Additional cases cover source-library targets, DCI contracts and native artifacts,
explicit multi-CGU emission through mixed-separator cache paths, owning String/Vec
barriers, a large test list, output limits and complete JSON/XML serialization.
The gate rejects fractional baseline counts/durations, mismatched sample counts
or targets, duplicate JSON fields and decoded NUL. It independently recalculates
the scaled-threshold variance and checks that a cached baseline survives several
file-local compiler arenas. Binary stdout exercises NUL, malformed UTF-8, valid
multibyte characters and XML-forbidden characters while preserving the raw log.
Report/source/native-input collisions and independent build-job budgets are
also covered.

For a repeatable discovery/report scaling measurement:

```sh
python probes/gates/test-runner/measure.py --compiler /path/to/fresh/vyxc --reference /path/to/comparison/vyxc
```

This generates both many tests in one file and many single-test files, alternates
the compiler order across repeated runs, and checks every result count. JSON
evidence includes compiler hashes, commands, raw timings and medians. Set
`--tests 512,1024,2048`, `--files 64,128,256`, `--runs 3` or `--output` to adjust
the corpus and record location. These wall-clock measurements include discovery
and report writing; they do not claim compilation or test-body throughput.
Run under a stable system load. Timing is not an absolute CI pass/fail threshold.

The runner does not provide async tests, test methods, automatic parameterized
discovery, doc tests, coverage instrumentation or cross-device execution.
