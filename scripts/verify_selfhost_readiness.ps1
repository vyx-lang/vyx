param(
    [Alias("Compiler")]
    [string]$HostCompiler = "",
    [switch]$AllStress,
    [switch]$IncludeLegacySemantic,
    [switch]$ListOnly
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

function Resolve-Compiler {
    param([string]$Explicit)
    if ($Explicit -ne "") { return (Resolve-Path $Explicit).Path }
    if ($env:VYX_HOST_VYXC -and (Test-Path $env:VYX_HOST_VYXC)) {
        return (Resolve-Path $env:VYX_HOST_VYXC).Path
    }
    foreach ($cand in @(
        (Join-Path $repoRoot "build\vyxc.exe"),
        (Join-Path $repoRoot "cmake-build-debug\vyxc.exe")
    )) {
        if (Test-Path $cand) { return (Resolve-Path $cand).Path }
    }
    return $null
}

function RepoRel {
    param([string]$Path)
    $full = (Resolve-Path $Path).Path
    if ($full.StartsWith($repoRoot, [StringComparison]::OrdinalIgnoreCase)) {
        return $full.Substring($repoRoot.Length).TrimStart('\')
    }
    return $full
}

$compiler = Resolve-Compiler $HostCompiler
if (-not $compiler) {
    Write-Error "Host compiler not found. Build vyxc or pass -HostCompiler."
}

# Current-language selfhost readiness does not depend on deleted bootstrap
# compiler sources. These are compiler-shaped workloads that the current
# parser + Sema design should accept. Syntax-only validity is not enough:
# an old test that violates current ownership semantics is a legacy semantic
# test, not proof that a conforming program miscompiles.
$manifest = @(
    "tests\cases\lang_validation\test_selfhost_bootstrap_gate.vyx",
    "tests\cases\lang_validation\test_selfhost_adversarial_gate.vyx",
    "tests\cases\stress\test_bootstrap_patterns.vyx",
    "tests\cases\stress\test_generics.vyx",
    "tests\cases\stress\test_nested_generics.vyx",
    "tests\cases\stress\test_strings_collections.vyx",
    "tests\cases\stress\test_oop.vyx",
    "tests\cases\stress\test_functions.vyx",
    "tests\cases\stress\test_error_handling.vyx",
    "tests\cases\stress\test_match_pattern.vyx"
)

$legacySemantic = @(
    # These currently pass parsing but violate the active ownership model:
    # e.g. `sort(v); is_sorted(v); v.get(...)` where `sort(v)` takes the
    # owning Vec by value. Keep them visible behind an explicit switch while
    # the stdlib API/borrow design is being settled.
    "tests\cases\stress\test_containers_stress.vyx",
    "tests\cases\stress\test_std_library.vyx"
)

if ($AllStress) {
    $manifest = @(Get-ChildItem -Path (Join-Path $repoRoot "tests\cases\stress") -Filter "*.vyx" -File |
        Sort-Object FullName |
        ForEach-Object { RepoRel $_.FullName })
} elseif ($IncludeLegacySemantic) {
    $manifest = @($manifest + $legacySemantic)
}

$missing = @()
foreach ($rel in $manifest) {
    if (-not (Test-Path (Join-Path $repoRoot $rel))) { $missing += $rel }
}
if ($missing.Count -gt 0) {
    Write-Host "selfhost readiness: missing test input(s)"
    foreach ($m in $missing) { Write-Host "  $m" }
    exit 2
}

if ($ListOnly) {
    foreach ($rel in $manifest) { Write-Host $rel }
    exit 0
}

Write-Host "selfhost readiness: compiler=$compiler"
Write-Host "selfhost readiness: tests=$($manifest.Count)"
Write-Host ""

$failed = 0
Push-Location $repoRoot
try {
    foreach ($rel in $manifest) {
        Write-Host "[selfhost-readiness] $rel"
        & $compiler --run (Join-Path $repoRoot $rel)
        if ($LASTEXITCODE -ne 0) {
            Write-Host "  FAILED (exit $LASTEXITCODE)"
            $failed += 1
        }
    }
} finally {
    Pop-Location
}

Write-Host ""
if ($failed -gt 0) {
    Write-Host "selfhost readiness: FAIL ($failed / $($manifest.Count) failed)"
    exit 1
}

Write-Host "selfhost readiness: PASS"
exit 0
