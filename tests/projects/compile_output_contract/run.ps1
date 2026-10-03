param(
    [string]$BootstrapCompiler = "",
    [string]$Clang = "",
    [string]$LlvmRoot = ""
)

$ErrorActionPreference = "Stop"

$projectRoot = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $projectRoot "..\..\..")).Path
$windowsHost = if (Get-Variable -Name IsWindows -ErrorAction SilentlyContinue) { [bool]$IsWindows } else { $env:OS -eq "Windows_NT" }
if ([string]::IsNullOrWhiteSpace($BootstrapCompiler)) {
    $BootstrapCompiler = Join-Path $repoRoot ("bootstrap_compiler/out/boot" + $(if ($windowsHost) { ".exe" } else { "" }))
}
if ([string]::IsNullOrWhiteSpace($Clang)) {
    if ($windowsHost) {
        $Clang = Join-Path $repoRoot "clang/bin/clang++.exe"
    } else {
        $Clang = (Get-Command clang++ -ErrorAction Stop).Source
    }
}
$BootstrapCompiler = (Resolve-Path -LiteralPath $BootstrapCompiler).Path
$Clang = (Resolve-Path -LiteralPath $Clang).Path

$workRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("vyx-compile-output-contract-" + [Guid]::NewGuid().ToString("N"))
[void][System.IO.Directory]::CreateDirectory($workRoot)
$passed = $false
$originalFakeCxx = $env:VYX_FAKE_CXX
$originalLlvmRoot = $env:LLVM_ROOT
. (Join-Path $repoRoot "bootstrap_compiler/scripts/VyxTestProcess.ps1")

if ([string]::IsNullOrWhiteSpace($LlvmRoot) -and $windowsHost) {
    $LlvmRoot = Join-Path $repoRoot "clang"
}

try {
    Copy-Item -LiteralPath (Join-Path $projectRoot "Vyx.toml") -Destination $workRoot
    Copy-Item -LiteralPath (Join-Path $projectRoot "main.vyx") -Destination $workRoot
    Copy-Item -LiteralPath (Join-Path $projectRoot "native") -Destination $workRoot -Recurse

    $fakeSource = Join-Path $workRoot "fake_cxx.cpp"
    $fakeCompiler = Join-Path $workRoot ("fake_cxx" + $(if ($windowsHost) { ".exe" } else { "" }))
    [IO.File]::WriteAllText($fakeSource, "int main() { return 0; }`n")
    & $Clang $fakeSource -O0 -o $fakeCompiler
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $fakeCompiler -PathType Leaf)) {
        throw "failed to build the no-output compiler fixture"
    }

    $env:VYX_FAKE_CXX = $fakeCompiler
    if (-not [string]::IsNullOrWhiteSpace($LlvmRoot)) {
        $env:LLVM_ROOT = (Resolve-Path -LiteralPath $LlvmRoot).Path
    }
    $stdout = Join-Path $workRoot "build.stdout.log"
    $stderr = Join-Path $workRoot "build.stderr.log"
    $invokeArgs = @{
        FilePath = $BootstrapCompiler
        ArgumentList = @("build", "-j1")
        WorkingDirectory = $workRoot
        StdoutLog = $stdout
        StderrLog = $stderr
        DialogLog = (Join-Path $workRoot "build.dialog.log")
        TimeoutSec = 60
    }
    if ($windowsHost) {
        $invokeArgs.MemoryLimitMB = 4096
    }
    $run = Invoke-VyxProcess @invokeArgs
    $exitCode = [int]$run.ExitCode
    if ($run.TimedOut -or $run.DialogCaught -or $run.MemoryExceeded) {
        throw "compile output contract did not terminate cleanly: exit=$exitCode timeout=$($run.TimedOut) dialog=$($run.DialogCaught) memory=$($run.MemoryExceeded)"
    }

    if ($exitCode -eq 0) {
        throw "compiler success without its declared temporary output was accepted"
    }
    $diagnostics = [IO.File]::ReadAllText($stdout) + "`n" + [IO.File]::ReadAllText($stderr)
    if ($diagnostics.IndexOf("compile output finalize failed", [StringComparison]::OrdinalIgnoreCase) -lt 0) {
        throw "missing temporary output did not reach the finalize diagnostic"
    }
    if (Test-Path -LiteralPath (Join-Path $workRoot ".build-cache/.vyx_cache")) {
        throw "failed compile published a build cache record"
    }
    $outputName = "compile_output_contract" + $(if ($windowsHost) { ".exe" } else { "" })
    if (Test-Path -LiteralPath (Join-Path $workRoot ("out/" + $outputName))) {
        throw "failed compile produced a final link output"
    }

    $passed = $true
    Write-Host "compile_output_contract PASS"
} finally {
    $env:VYX_FAKE_CXX = $originalFakeCxx
    $env:LLVM_ROOT = $originalLlvmRoot
    if ($passed) {
        Remove-Item -LiteralPath $workRoot -Recurse -Force -ErrorAction SilentlyContinue
    } else {
        Write-Host "compile_output_contract retained_work_root=$workRoot"
    }
}
