param(
    [string]$Compiler = '',
    [string]$OutputDir = '',
    [ValidateRange(32, 1024)][int]$Iterations = 32
)
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
if (-not $Compiler) { $Compiler = Join-Path $repoRoot 'bootstrap_compiler/out/vyxc.exe' }
$Compiler = (Resolve-Path -LiteralPath $Compiler).Path
if (-not $OutputDir) { $OutputDir = Join-Path $PSScriptRoot ('.runs/' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff')) }
$runRoot = [IO.Path]::GetFullPath($OutputDir)
if (Test-Path -LiteralPath $runRoot) { throw "Refusing to overwrite existing output: $runRoot" }
[void][IO.Directory]::CreateDirectory($runRoot)
. (Join-Path $repoRoot 'bootstrap_compiler/scripts/VyxTestProcess.ps1')
$python = (Get-Command python -ErrorAction Stop).Source
$compilerDir = Split-Path -Parent $Compiler
$identity = [ordered]@{ compiler=$Compiler; sha256=(Get-FileHash -LiteralPath $Compiler -Algorithm SHA256).Hash; artifacts=@() }
foreach ($pattern in @('vyx_compiler_backend*.dll', 'vyx_runtime*.lib')) {
    foreach ($artifact in Get-ChildItem -LiteralPath $compilerDir -File -Filter $pattern) {
        $identity.artifacts += [ordered]@{ path=$artifact.FullName; sha256=(Get-FileHash -LiteralPath $artifact.FullName -Algorithm SHA256).Hash }
    }
}
$results = [Collections.Generic.List[object]]::new()
$executables = [ordered]@{}
$savedPath = $env:PATH
$savedLlvmRoot = $env:LLVM_ROOT
$env:LLVM_ROOT = Join-Path $repoRoot 'clang'
$env:PATH = (Join-Path $env:LLVM_ROOT 'bin') + ';' + $compilerDir + ';' + $env:PATH

function Invoke-FailureProcess([string]$Name, [string]$File, [string]$Directory, [string[]]$Arguments, [switch]$Run) {
    $stdout = Join-Path $runRoot "$Name.stdout.log"
    $stderr = Join-Path $runRoot "$Name.stderr.log"
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $process = Invoke-VyxProcess -FilePath $File -WorkingDirectory $Directory -ArgumentList $Arguments -TimeoutSec 180 `
        -StdoutLog $stdout -StderrLog $stderr -DialogLog (Join-Path $runRoot "$Name.dialog.log")
    $timer.Stop()
    $diagnostics = [IO.File]::ReadAllText($stdout) + [IO.File]::ReadAllText($stderr)
    $passed = $process.ExitCode -eq 0
    if ($Run) { $passed = $passed -and $diagnostics -match '(?m)^dci failure payload OK\r?$' }
    $results.Add([ordered]@{
        phase=$Name; passed=$passed; exit_code=$process.ExitCode; file=$File; directory=$Directory; arguments=$Arguments
        elapsed_ms=$timer.ElapsedMilliseconds; stdout=$stdout; stderr=$stderr
    })
    Write-Host "$Name passed=$passed exit=$($process.ExitCode)"
    if (-not $passed) { Write-Host $diagnostics }
    if (-not $passed -and -not $Run) { throw "$Name failed; logs: $runRoot" }
}

try {
    Invoke-FailureProcess 'compiler-version' $Compiler $repoRoot @('--version')
    $inputs = Join-Path $runRoot 'inputs'
    $contracts = Join-Path $inputs 'contracts'
    $generated = Join-Path $inputs 'generated'
    [void][IO.Directory]::CreateDirectory($contracts)
    [void][IO.Directory]::CreateDirectory($generated)
    foreach ($file in @('checked_divide.vyx', 'failure_probe_support.cpp')) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $file) -Destination $inputs
    }
    $manifest = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'Vyx.toml'))
    $dciRoot = (Join-Path $repoRoot 'tools/dci').Replace('\', '/')
    $manifest = $manifest.Replace('../../../tools/dci/', $dciRoot + '/')
    [IO.File]::WriteAllText((Join-Path $inputs 'Vyx.toml'), $manifest, [Text.UTF8Encoding]::new($false))

    $dciTool = Join-Path $repoRoot 'tools/dci/dci.py'
    $header = Join-Path $repoRoot 'tools/dci/bench/fixtures/abi_fixtures.hpp'
    $dcib = Join-Path $contracts 'abi_fixtures.dcib'
    $stub = Join-Path $generated 'translate_unwind.cpp'
    Invoke-FailureProcess 'adapter' $python $repoRoot @($dciTool, 'adapter', '--language', 'cpp', $header,
        '-o', $dcib, '--stub-out', $stub, '--triplet', 'windows_x64')
    $contractJson = Join-Path $runRoot 'contract.json'
    Invoke-FailureProcess 'decode' $python $repoRoot @($dciTool, 'decode', $dcib, $contractJson)
    $contract = [IO.File]::ReadAllText($contractJson) | ConvertFrom-Json
    $failureLayouts = @($contract.exports.layouts | Where-Object { $_.type_name -eq 'dci::Failure' })
    $contractPassed = $failureLayouts.Count -eq 1
    if ($contractPassed) {
        $layout = $failureLayouts[0]
        $contractPassed = $layout.size -eq 64 -and $layout.alignment -eq 8 -and $layout.representation -eq 'stable' `
            -and $layout.traits.trivial -eq $false -and $layout.is_trivially_destructible -eq $false `
            -and $layout.lifecycle.destroy.link_name -eq 'dci_failure_destroy' -and $layout.lifecycle.destroy.trivial -eq $false
    }
    $results.Add([ordered]@{ phase='failure-layout-contract'; passed=$contractPassed; exit_code=$null; contract=$contractJson })
    if (-not $contractPassed) { throw 'Failure contract must describe the stable 64-byte record and nontrivial destroy operation' }

    foreach ($level in @('O0', 'O2')) {
        $project = Join-Path $runRoot $level
        Copy-Item -LiteralPath $inputs -Destination $project -Recurse
        $ir = Join-Path $project 'checked_divide.ll'
        Invoke-FailureProcess "$level-ir" $Compiler $project @('--src=file', 'checked_divide.vyx', '--dci', 'contracts/abi_fixtures.dcib', '--emit=ir', "-$level", '-o', $ir)
        $irText = [IO.File]::ReadAllText($ir)
        $irPassed = $irText -match '(?m)^\s*(?:%[^=]+\s*=\s*)?call\b[^\r\n]*@dci_tr_checked_divide_abi_YAHHH_Z\b' `
            -and $irText -notmatch '__CxxFrameHandler3|__gxx_personality_v0' `
            -and $irText -notmatch '\?checked_divide@abi@@YAHHH@Z'
        $results.Add([ordered]@{ phase="$level-translator-contract"; passed=$irPassed; exit_code=$null; ir=$ir })
        if (-not $irPassed) { throw "$level IR must call the no-unwind translator without original throwing symbol or Vyx EH" }
        Invoke-FailureProcess "$level-build" $Compiler $project @('build', '--target', 'dci_failure_probe', '-j4', "-$level")
        $exe = Join-Path $project 'target/dci_failure_probe.exe'
        if (-not (Test-Path -LiteralPath $exe)) { throw "$level executable missing: $exe" }
        $executables[$level] = [ordered]@{
            path=$exe; sha256=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
            iterations=$Iterations; checked_and_destroyed_errors_per_process=16
        }
        for ($iteration = 1; $iteration -le $Iterations; $iteration++) {
            Invoke-FailureProcess "$level-run-$iteration" $exe $project @() -Run
        }
    }
} finally {
    [ordered]@{ identity=$identity; executables=$executables; tests=@($results.ToArray()); run=$runRoot } | ConvertTo-Json -Depth 7 |
        Set-Content -LiteralPath (Join-Path $runRoot 'results.json') -Encoding utf8
    $env:PATH = $savedPath
    $env:LLVM_ROOT = $savedLlvmRoot
}
if ($results | Where-Object { -not $_.passed }) { exit 1 }
Write-Host "dci-failure: PASS ($Iterations native runs at each of O0/O2; $runRoot)"
