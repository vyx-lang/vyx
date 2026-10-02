# Explicit PowerShell contract-check dispatcher.
# Module/project execution remains tests/run_all_modules.ps1.
[CmdletBinding()]
param(
    [string]$Check = "",
    [string]$Suite = "",
    [switch]$All,
    [switch]$IncludeManual,
    [switch]$ListOnly,
    [string]$BootstrapCompiler = "",
    [string[]]$Arguments = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$testsRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $testsRoot "..")).Path

$checks = @(
    [pscustomobject]@{ Name = "bootstrap_conformance"; Suite = "bootstrap"; Tier = "targeted"; Path = "bootstrap/run.ps1"; Description = "bootstrap module-registry and parser-recovery contracts" },
    [pscustomobject]@{ Name = "bootstrap_diagnostics"; Suite = "diagnostics"; Tier = "manual"; Path = "diagnostics/bootstrap_diagnostics.ps1"; Description = "bootstrap diagnostic and self-host consistency matrix" },
    [pscustomobject]@{ Name = "borrow_invalidation"; Suite = "diagnostics"; Tier = "targeted"; Path = "diagnostics/borrow_invalidation/run.ps1"; Description = "borrow invalidation diagnostic contract" },
    [pscustomobject]@{ Name = "dci_rust_adapter"; Suite = "dci"; Tier = "manual"; Path = "dci/rust_adapter/run.ps1"; Description = "Rust DCI adapter generation and contract validation" },
    [pscustomobject]@{ Name = "vyi_alias_sizeof_layout"; Suite = "vyi"; Tier = "targeted"; Path = "../projects/vyi_alias_sizeof_layout/run.ps1"; Description = "VYI alias sizeof layout integration contract" },
    [pscustomobject]@{ Name = "llvm_function_attrs"; Suite = "llvm"; Tier = "targeted"; Path = "llvm/llvm_function_attrs.ps1"; Description = "LLVM function attribute contract" },
    [pscustomobject]@{ Name = "llvm_header_string_len"; Suite = "llvm"; Tier = "targeted"; Path = "llvm/llvm_header_string_len.ps1"; Description = "LLVM/MIR2CPP decimal string-header ABI" },
    [pscustomobject]@{ Name = "llvm_lifetime_intrinsics"; Suite = "llvm"; Tier = "targeted"; Path = "llvm/llvm_lifetime_intrinsics.ps1"; Description = "LLVM lifetime intrinsic contract" },
    [pscustomobject]@{ Name = "llvm_pointer_gep"; Suite = "llvm"; Tier = "targeted"; Path = "llvm/llvm_pointer_gep.ps1"; Description = "pointer/GEP lowering contract" },
    [pscustomobject]@{ Name = "mir_analysis"; Suite = "mir"; Tier = "targeted"; Path = "mir/mir_analysis.ps1"; Description = "MIR analysis contract" },
    [pscustomobject]@{ Name = "mir_cfg_simplify"; Suite = "mir"; Tier = "targeted"; Path = "mir/mir_cfg_simplify.ps1"; Description = "MIR CFG simplification" },
    [pscustomobject]@{ Name = "mir_copy_propagation"; Suite = "mir"; Tier = "targeted"; Path = "mir/mir_copy_propagation.ps1"; Description = "MIR copy propagation" },
    [pscustomobject]@{ Name = "mir_dce"; Suite = "mir"; Tier = "targeted"; Path = "mir/mir_dce.ps1"; Description = "MIR dead-code elimination" },
    [pscustomobject]@{ Name = "mir_fixed_point"; Suite = "mir"; Tier = "targeted"; Path = "mir/mir_fixed_point.ps1"; Description = "MIR fixed-point stability" },
    [pscustomobject]@{ Name = "mir_local_facts"; Suite = "mir"; Tier = "targeted"; Path = "mir/mir_local_facts.ps1"; Description = "MIR local-facts contract" },
    [pscustomobject]@{ Name = "mir_pass"; Suite = "mir"; Tier = "targeted"; Path = "mir/mir_pass.ps1"; Description = "MIR pass pipeline" },
    [pscustomobject]@{ Name = "mir_sccp"; Suite = "mir"; Tier = "targeted"; Path = "mir/mir_sccp.ps1"; Description = "MIR sparse conditional constant propagation" },
    [pscustomobject]@{ Name = "mir_select_formation"; Suite = "mir"; Tier = "targeted"; Path = "mir/mir_select_formation.ps1"; Description = "MIR select formation" },
    [pscustomobject]@{ Name = "mir_storage_lifetime"; Suite = "mir"; Tier = "targeted"; Path = "mir/mir_storage_lifetime.ps1"; Description = "MIR storage/lifetime lowering" },
    [pscustomobject]@{ Name = "box_stack_promotion"; Suite = "memory"; Tier = "targeted"; Path = "memory/box_stack_promotion.ps1"; Description = "non-escaping Box stack promotion" },
    [pscustomobject]@{ Name = "class_auto_stack_local"; Suite = "memory"; Tier = "targeted"; Path = "memory/class_auto_stack_local.ps1"; Description = "automatic trivial class-local storage" },
    [pscustomobject]@{ Name = "class_auto_stack_multiblock"; Suite = "memory"; Tier = "targeted"; Path = "memory/class_auto_stack_multiblock.ps1"; Description = "automatic class-local storage across control flow" },
    [pscustomobject]@{ Name = "class_value_abi"; Suite = "memory"; Tier = "targeted"; Path = "memory/class_value_abi.ps1"; Description = "native class default value ABI (stack/sret, new stays heap)" },
    [pscustomobject]@{ Name = "ref_arc_allocation"; Suite = "memory"; Tier = "targeted"; Path = "memory/ref_arc_allocation.ps1"; Description = "Ref ARC allocation contract" },
    [pscustomobject]@{ Name = "runtime_memory_attrs"; Suite = "memory"; Tier = "targeted"; Path = "memory/runtime_memory_attrs.ps1"; Description = "runtime memory ABI attributes" },
    [pscustomobject]@{ Name = "string_iterators"; Suite = "runtime"; Tier = "targeted"; Path = "runtime/string_iterators.ps1"; Description = "string iterator runtime behavior" },
    [pscustomobject]@{ Name = "string_pool_ownership"; Suite = "runtime"; Tier = "targeted"; Path = "runtime/string_pool_ownership.ps1"; Description = "string pool ownership contract" },
    [pscustomobject]@{ Name = "tutorial_examples"; Suite = "language"; Tier = "targeted"; Path = "language/tutorial_examples.ps1"; Description = "tutorial source and manifest examples" },
    [pscustomobject]@{ Name = "reference_receiver_ergonomics"; Suite = "language"; Tier = "targeted"; Path = "language/reference_receiver_ergonomics.ps1"; Description = "uniform member/index access for references and typed pointers" },
    [pscustomobject]@{ Name = "cgu_parallel_determinism"; Suite = "llvm"; Tier = "targeted"; Path = "llvm/cgu_parallel_determinism.ps1"; Description = "multi-CGU object emission is byte-identical across parallelism modes" },
    [pscustomobject]@{ Name = "compare_compiler_perf"; Suite = "performance"; Tier = "manual"; Path = "performance/compare_compiler_perf.ps1"; Description = "cross-compiler compilation comparison" },
    [pscustomobject]@{ Name = "compare_language_benchmarks"; Suite = "performance"; Tier = "manual"; Path = "performance/compare_language_benchmarks.ps1"; Description = "cross-language runtime benchmark" }
)

if ($ListOnly -or (-not $All -and [string]::IsNullOrWhiteSpace($Check) -and [string]::IsNullOrWhiteSpace($Suite))) {
    $checks | Sort-Object Suite, Name | Select-Object Name, Suite, Tier, Description, Path | Format-Table -AutoSize
    exit 0
}

$selected = @($checks)
if (-not [string]::IsNullOrWhiteSpace($Check)) {
    $selected = @($selected | Where-Object { $_.Name -ieq $Check })
}
if (-not [string]::IsNullOrWhiteSpace($Suite)) {
    $selected = @($selected | Where-Object { $_.Suite -ieq $Suite })
}
if ($All -and -not $IncludeManual) {
    $selected = @($selected | Where-Object { $_.Tier -eq "targeted" })
}
if ($selected.Count -eq 0) {
    throw "No check selected. Use -ListOnly to inspect canonical names and suites."
}

foreach ($entry in ($selected | Sort-Object Suite, Name)) {
    $scriptPath = Join-Path $PSScriptRoot $entry.Path
    if (-not (Test-Path -LiteralPath $scriptPath)) {
        throw "check script missing: $($entry.Name): $scriptPath"
    }
    Write-Host "[check:$($entry.Suite)] $($entry.Name)"
    $global:LASTEXITCODE = 0
    if (-not [string]::IsNullOrWhiteSpace($BootstrapCompiler) -and
        $entry.Suite -notin @("dci", "performance")) {
        & $scriptPath -BootstrapCompiler $BootstrapCompiler @Arguments
    } else {
        & $scriptPath @Arguments
    }
    # A PowerShell check can return normally without setting LASTEXITCODE;
    # only a native process exit code is meaningful here.
    $exitCode = if ($null -eq $LASTEXITCODE) { 0 } else { [int]$LASTEXITCODE }
    if ($exitCode -ne 0) {
        throw "check failed: $($entry.Name) (exit $exitCode)"
    }
}
