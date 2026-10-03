param(
    [string]$Compiler = (Join-Path $PSScriptRoot '../../../clang/bin/clang++.exe'),
    [string]$OutputDir = (Join-Path $PSScriptRoot ('.runs/' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff')))
)

$ErrorActionPreference = 'Stop'
if (-not $IsWindows -or $PSVersionTable.PSVersion.Major -lt 7) { throw 'This runner requires Windows PowerShell 7.' }
$compilerPath = (Resolve-Path -LiteralPath $Compiler).Path
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$outputPath = [IO.Path]::GetFullPath($OutputDir)
$runsRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '.runs'))
if (-not $outputPath.StartsWith($runsRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'OutputDir must remain beneath pointer_region_abi/.runs/.'
}
if (Test-Path -LiteralPath $outputPath) { throw "Refusing to overwrite $outputPath" }
New-Item -ItemType Directory -Path $outputPath -Force | Out-Null

function Invoke-ProbeCommand([string]$File, [string[]]$Arguments, [string]$Name, [int]$TimeoutMs = 30000) {
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $File
    $info.WorkingDirectory = $repository
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    foreach ($argument in $Arguments) { $info.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::Start($info)
    try {
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $timedOut = -not $process.WaitForExit($TimeoutMs)
        if ($timedOut) { $process.Kill($true); $process.WaitForExit() }
        $stdout = $stdoutTask.GetAwaiter().GetResult()
        $stderr = $stderrTask.GetAwaiter().GetResult()
        $stdout | Set-Content -LiteralPath (Join-Path $outputPath "$Name.stdout.log") -Encoding utf8NoBOM
        $stderr | Set-Content -LiteralPath (Join-Path $outputPath "$Name.stderr.log") -Encoding utf8NoBOM
        return [pscustomobject]@{ name=$Name; exit_code=$process.ExitCode; timed_out=$timedOut; stdout=$stdout; stderr=$stderr }
    } finally { $process.Dispose() }
}

$inputs = @('probes/gates/pointer_region_abi/src/probe.cpp', 'vyx_codegen/src/vyx_pointer_handle_rt.inc', 'vyx_codegen/include/vyx_pointer_region_abi.h')
$sourceHashes = @($inputs | ForEach-Object {
    $path = Join-Path $repository $_
    [ordered]@{ path=$_; sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash }
})
$exe = Join-Path $outputPath 'pointer_region_abi.exe'
$compileArgs = @('-std=c++17', '-O2', '-DNDEBUG', (Join-Path $PSScriptRoot 'src/probe.cpp'), '-o', $exe)
[ordered]@{
    schema_version=1
    started_utc=[DateTime]::UtcNow.ToString('o')
    compiler=$compilerPath
    compiler_sha256=(Get-FileHash -LiteralPath $compilerPath -Algorithm SHA256).Hash
    compile_argv=$compileArgs
    sources=$sourceHashes
    scope='Actual native pointer runtime include plus independent canonical-byte-offset loads; no backend or boot rebuild.'
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $outputPath 'metadata.json') -Encoding utf8NoBOM

$compile = Invoke-ProbeCommand $compilerPath $compileArgs 'compile'
if ($compile.timed_out -or $compile.exit_code -ne 0) {
    Write-Host $compile.stderr
    throw "Native probe compilation failed (exit $($compile.exit_code), timeout $($compile.timed_out)). Logs: $outputPath"
}
$cases = @(
    @{ name='positive'; expect_success=$true; needle='pointer_region_abi: positive OK' },
    @{ name='borrow_shared_delete'; expect_success=$false; needle='vyx: cannot delete borrowed pointer handle' },
    @{ name='borrow_mutable_delete'; expect_success=$false; needle='vyx: cannot delete borrowed pointer handle' },
    @{ name='dangling'; expect_success=$false; needle='vyx: dangling pointer handle dereference' },
    @{ name='range_dangling'; expect_success=$false; needle='vyx: dangling pointer handle dereference' },
    @{ name='shape_oob'; expect_success=$false; needle='vyx: pointer index out of bounds' },
    @{ name='bounds_oob'; expect_success=$false; needle='vyx: pointer index out of bounds' }
)
$results = @()
foreach ($case in $cases) {
    $result = Invoke-ProbeCommand $exe @($case.name) $case.name 10000
    $log = if ($case.expect_success) { $result.stdout } else { $result.stderr }
    $exitMatches = if ($case.expect_success) { $result.exit_code -eq 0 } else { $result.exit_code -ne 0 }
    $passed = -not $result.timed_out -and $exitMatches -and $log.Contains($case.needle)
    $results += [ordered]@{ name=$case.name; passed=$passed; exit_code=$result.exit_code; timed_out=$result.timed_out; expected_message=$case.needle }
    Write-Host "$($case.name): passed=$passed exit=$($result.exit_code)"
}
$allPassed = @($results | Where-Object { -not $_.passed }).Count -eq 0
[ordered]@{ passed=$allPassed; results=$results } | ConvertTo-Json -Depth 4 |
    Set-Content -LiteralPath (Join-Path $outputPath 'result.json') -Encoding utf8NoBOM
if (-not $allPassed) { throw "pointer_region_abi gate failed. Logs: $outputPath" }
Write-Host "pointer_region_abi: OK. Logs: $outputPath"
