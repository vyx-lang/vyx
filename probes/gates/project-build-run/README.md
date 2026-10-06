# Project build and run

The fixture in `tests/projects/project_build_run/` links a native C++ dependency,
uses a target output directory containing spaces, and checks that the manifest's
postbuild hook completes before execution.

From the repository root, with a newly built SDK compiler:

```powershell
$env:LLVM_ROOT = (Resolve-Path .\clang).Path
$env:PATH = "$env:LLVM_ROOT\bin;$env:PATH"
python .\probes\gates\project-build-run\run.py `
    --compiler (Get-Command vyxc -ErrorAction Stop).Source `
    --result .\.runs\project-build-run\checks.json `
    --qt-project .\probes\gates\dci-qt-counter `
    --qt-root E:\Qt\6.7.3\msvc2019_64
```

Omit the two Qt options when that Windows Qt fixture is unavailable. The JSON
records the actual compiler hash and checks executed; Qt runs are not claimed
when omitted. Results and generated fixture copies belong in `.runs/`.

Checks cover the project selector and build shorthand, explicit and automatic
target selection, dependencies, warm link caching, external archive changes
with preserved size/mtime, program exit codes, literal
argv (including empty strings, quotes, backslashes, Unicode and flag-like
arguments), compiler-selected debugger artifacts, non-executable and ambiguous
targets, cross-platform run rejection, and prevention of stale execution after
a failed compilation or runtime asset deployment. A deployment failure must
restore the previous executable; retrying after removing the blocked path must
deploy the asset and run the program.

All `--src=project <dir>` commands use the manifest builder. `vyxc build` selects
the current project with the same options. IR emission uses the planned compilation
units, including DCI contracts, and merges their LLVM modules with the bundled
backend. `--emit=obj` publishes the target's object set. Neither mode links or runs
the selected executable. The gate checks target selection, output paths, multiple
modules, artifact recovery, and the Qt IR command in addition to native AOT runs.
TOML scripts still use `vyxc run <script>`.
