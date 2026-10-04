# Contributing to Vyx

[简体中文](CONTRIBUTING.zh-CN.md) · [Documentation index](docs/README.md)

Vyx contributions are judged by a small, reproducible loop: use a compatible
Release SDK compiler as Stage 0, build this checkout, and run the narrowest test
that proves the change. The
archived C++ host and stale `out/` binaries are not acceptance paths.

## Contribution licensing

Unless you explicitly state otherwise, contributions intentionally submitted for
inclusion in Vyx are licensed under the same `MIT OR Apache-2.0` terms as the
project, without additional conditions. Preserve existing third-party license
and copyright notices. The full license texts are [MIT](LICENSE-MIT) and
[Apache-2.0](LICENSE-APACHE).

## Before changing code

Read [language design](docs/LANGUAGE_DESIGN.md) (from `bootstrap_compiler/`)
and [the testing guide](docs/TESTING_GUIDE.md) for the executable process. Set the
Release SDK compiler path, then build the working tree:

```powershell
$env:VYX_BOOTSTRAP_VYXC = (Get-Command vyxc -ErrorAction Stop).Source
$env:LLVM_ROOT = (Resolve-Path .\clang).Path
$compilerTarget = [regex]::Match((Get-Content .\bootstrap_compiler\Vyx.toml -Raw), '(?m)^name\s*=\s*"([^"]+)"').Groups[1].Value

Push-Location .\bootstrap_compiler
& $env:VYX_BOOTSTRAP_VYXC build --target $compilerTarget -j4
if ($LASTEXITCODE -ne 0) { throw 'bootstrap build failed' }
Copy-Item -LiteralPath ".\out\$compilerTarget.exe" -Destination .\out\vyxc.exe -Force
Pop-Location

$compiler = (Resolve-Path .\bootstrap_compiler\out\vyxc.exe).Path
$runtime = (Resolve-Path .\bootstrap_compiler\out).Path
```

## Validate the smallest relevant contract

```powershell
# Language/module or manifest-project change.
powershell -File .\tests\run_all_modules.ps1 `
  -BootstrapOnly -BootstrapCompiler $compiler -RuntimeDir $runtime -RuntimeLib vyx_runtime `
  -PathFilter '<suite-or-file>'

# Named compiler/runtime/ABI contract.
powershell -File .\tests\checks\run_checks.ps1 -ListOnly
powershell -File .\tests\checks\run_checks.ps1 `
  -Check '<canonical-check-name>' -BootstrapCompiler $compiler
```

For compiler, runtime, or ABI work, add a source case when the behavior is
language-visible; use a project fixture or named check when it crosses a build,
ABI, or multi-stage boundary. A historical test count is not a substitute for
the log from your current compiler/source pair.

If the change is user-visible, update the matching living page under `docs/`
(testing guide, manifest, standard library, [language surface](docs/LANGUAGE_SURFACE.md),
or README) in the same change. New language surface belongs in
a tutorial lesson **and** the surface index; do not leave users to read
compiler sources.

## Pull request handoff

Include:

- what changed and the user-visible consequence;
- the Stage 0 compiler hash/version/path;
- the source commit and exact commands run;
- focused test output, and any relevant IR or project log;
- incompatible behavior or known remaining failure, if present.

Avoid generated material: `build/`, `bootstrap_compiler/out/`, `.cache/`,
`target/`, `benchmarks/out/`, `.ll`, `.obj`, `.exe`, `.dll`, `.pdb`, and one-off
debug logs do not belong in a change. Keep the checked-in source, manifests,
cases, and durable scripts instead.
