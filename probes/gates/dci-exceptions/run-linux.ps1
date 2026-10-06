param([string]$Compiler = '')
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
if (!$Compiler) { $Compiler = Join-Path $repo 'bootstrap_compiler/out/boot.exe' }
$env:PATH = "$repo/bootstrap_compiler/out;$env:PATH"
$clang = Join-Path $repo 'clang/bin/clang++.exe'
$run = Join-Path $PSScriptRoot 'out'
New-Item -ItemType Directory -Force $run | Out-Null
python (Join-Path $PSScriptRoot 'make_contract.py') linux
if ($LASTEXITCODE -ne 0) { throw 'Linux contract generation failed' }
python (Join-Path $PSScriptRoot 'check_obligations.py')
if ($LASTEXITCODE -ne 0) { throw 'Linux cross-module obligation graph failed' }
python (Join-Path $PSScriptRoot 'check_direct.py') $Compiler '--triplet=x86_64-unknown-linux-gnu'
if ($LASTEXITCODE -ne 0) { throw 'Linux compiler-side DCI obligation graph was not fail-closed' }
function WslPath([string]$path) {
    $p = [IO.Path]::GetFullPath($path).Replace('\','/')
    return '/mnt/' + $p.Substring(0,1).ToLowerInvariant() + $p.Substring(2)
}
$linuxProbe = WslPath $PSScriptRoot
& wsl -d Ubuntu -e g++ -c "$linuxProbe/native.cpp" -std=c++17 -O2 -fexceptions -o "$linuxProbe/out/native-linux.o"
if ($LASTEXITCODE -ne 0) { throw 'Linux native harness compilation failed' }
try {
    foreach ($level in @('O0', 'O2')) {
        $ir = Join-Path $run "linux-$level.ll"
        $obj = Join-Path $run "linux-$level.o"
        $hooked = Join-Path $run "linux-$level.tracked.ll"
        & $Compiler --src=file (Join-Path $PSScriptRoot 'main.vyx') --triplet=x86_64-unknown-linux-gnu --emit=ir "-$level" -o $ir
        if ($LASTEXITCODE -ne 0) { throw "$level Linux IR emission failed" }
        $text = [regex]::Replace([IO.File]::ReadAllText($ir), '@(vyx_runtime_malloc|malloc)(?=[(\s])', '@tracked_malloc')
        $text = [regex]::Replace($text, '@(vyx_runtime_calloc|calloc)(?=[(\s])', '@tracked_calloc')
        $text = [regex]::Replace($text, '@(vyx_runtime_free|free)(?=[(\s])', '@tracked_free')
        [IO.File]::WriteAllText($hooked,$text)
        & $clang -c -target x86_64-unknown-linux-gnu $hooked -ffunction-sections -o $obj
        if ($LASTEXITCODE -ne 0) { throw "$level ELF object emission failed" }
        $linuxObj = WslPath $obj
        $linuxExe = "$linuxProbe/out/linux-$level"
        & wsl -d Ubuntu -e g++ $linuxObj "$linuxProbe/out/native-linux.o" '-Wl,--gc-sections' -o $linuxExe
        if ($LASTEXITCODE -ne 0) { throw "$level Linux link failed" }
        & wsl -d Ubuntu -e $linuxExe
        if ($LASTEXITCODE -ne 0) { throw "$level Itanium shared unwind failed: $LASTEXITCODE" }
        $doubleLog = Join-Path $run "linux-$level.double.log"
        # Redirect inside Linux: WSL host warnings use UTF-16 stderr and can
        # otherwise corrupt the UTF-8 native abort marker in a merged log.
        & wsl -d Ubuntu -e bash -c 'ulimit -c 0; DCI_EH_DOUBLE_THROW=1 "$1" > "$2" 2>&1' _ $linuxExe (WslPath $doubleLog)
        $code = $LASTEXITCODE
        if ($code -ne 134 -or !(Select-String -Path $doubleLog -Pattern '^SECOND_EXCEPTION_THROWN$' -Quiet)) {
            throw "$level expected SIGABRT during double unwind, got $code"
        }
        Write-Output "$level double unwind terminated OK"
    }
    foreach ($level in @('O0', 'O2')) {
        $ir = Join-Path $run "linux-inline-$level.ll"
        $obj = Join-Path $run "linux-inline-$level.o"
        & $Compiler --src=file (Join-Path $PSScriptRoot 'inline.vyx') --triplet=x86_64-unknown-linux-gnu --emit=ir "-$level" -o $ir
        if ($LASTEXITCODE -ne 0) { throw "$level Linux inline IR emission failed" }
        & $clang -c -target x86_64-unknown-linux-gnu $ir -ffunction-sections -o $obj
        if ($LASTEXITCODE -ne 0) { throw "$level inline ELF object emission failed" }
        $linuxExe = "$linuxProbe/out/linux-inline-$level"
        & wsl -d Ubuntu -e g++ (WslPath $obj) "$linuxProbe/out/native-linux.o" '-Wl,--gc-sections' -o $linuxExe
        if ($LASTEXITCODE -ne 0) { throw "$level Linux inline link failed" }
        & wsl -d Ubuntu -e $linuxExe
        if ($LASTEXITCODE -ne 0) { throw "$level inline stack cleanup failed: $LASTEXITCODE" }
    }
} finally {
    python (Join-Path $PSScriptRoot 'make_contract.py')
}
