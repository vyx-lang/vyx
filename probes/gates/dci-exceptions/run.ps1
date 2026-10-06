param([string]$Compiler = '')
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
if (!$Compiler) { $Compiler = Join-Path $repo 'bootstrap_compiler/out/boot.exe' }
$Compiler = (Resolve-Path -LiteralPath $Compiler).Path
$runtime = Split-Path -Parent $Compiler
if (!(Test-Path -LiteralPath (Join-Path $runtime 'vyx_runtime.lib'))) {
    $runtime = Join-Path (Split-Path -Parent $runtime) 'lib'
}
if (!(Test-Path -LiteralPath (Join-Path $runtime 'vyx_runtime.lib'))) {
    throw "Matching SDK runtime missing for $Compiler"
}
$env:PATH = "$runtime;$env:PATH"
$clang = Join-Path $repo 'clang/bin/clang++.exe'
$run = Join-Path $PSScriptRoot 'out'
New-Item -ItemType Directory -Force $run | Out-Null
python (Join-Path $PSScriptRoot 'make_contract.py')
if ($LASTEXITCODE -ne 0) { throw 'contract generation failed' }
python (Join-Path $PSScriptRoot 'check_obligations.py')
if ($LASTEXITCODE -ne 0) { throw 'cross-module obligation graph failed' }
python (Join-Path $PSScriptRoot 'check_direct.py') $Compiler
if ($LASTEXITCODE -ne 0) { throw 'compiler-side DCI obligation graph was not fail-closed' }
$direct = Join-Path $run 'direct-dci'
New-Item -ItemType Directory -Force $direct | Out-Null
$directLog = Join-Path $direct 'compile.log'
& $Compiler --src=file (Join-Path $PSScriptRoot 'main.vyx') `
    --dci (Join-Path $PSScriptRoot 'module_exception.dcib') `
    --dci (Join-Path $PSScriptRoot 'module_lifecycle.dcib') `
    --dump-mir2 *> $directLog
if ($LASTEXITCODE -ne 0) {
    throw "direct DCI consumer obligation import failed: exit=$LASTEXITCODE; see $directLog"
}
& $clang -c (Join-Path $PSScriptRoot 'native.cpp') -O2 -std=c++17 -fexceptions -fcxx-exceptions -fms-runtime-lib=static -o (Join-Path $run 'native.obj')
if ($LASTEXITCODE -ne 0) { throw 'native harness compilation failed' }
foreach ($level in @('O0', 'O2')) {
    $ir = Join-Path $run "$level.ll"
    $hooked = Join-Path $run "$level.tracked.ll"
    $exe = Join-Path $run "$level.exe"
    & $Compiler --src=file (Join-Path $PSScriptRoot 'main.vyx') --emit=ir "-$level" -o $ir
    if ($LASTEXITCODE -ne 0) { throw "$level IR emission failed" }
    $text = [IO.File]::ReadAllText($ir)
    $text = [regex]::Replace($text, '@(vyx_runtime_malloc|malloc)(?=[(\s])', '@tracked_malloc')
    $text = [regex]::Replace($text, '@(vyx_runtime_calloc|calloc)(?=[(\s])', '@tracked_calloc')
    $text = [regex]::Replace($text, '@(vyx_runtime_free|free)(?=[(\s])', '@tracked_free')
    [IO.File]::WriteAllText($hooked, $text)
    & $clang $hooked (Join-Path $run 'native.obj') "-L$runtime" -lvyx_runtime -ltbb12 -lsynchronization -lws2_32 -fms-runtime-lib=static -fuse-ld=lld -o $exe
    if ($LASTEXITCODE -ne 0) { throw "$level executable link failed" }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "$level shared unwind regression failed: $LASTEXITCODE" }
    $env:DCI_EH_DOUBLE_THROW = '1'
    try {
        $doubleStdout = Join-Path $run "$level.double.stdout.log"
        $doubleStderr = Join-Path $run "$level.double.stderr.log"
        # Start-Process captures native stderr verbatim. PowerShell's direct
        # invocation would turn the expected abort diagnostic into a formatted
        # NativeCommandError record and pollute the log we inspect.
        $doubleProcess = Start-Process -FilePath $exe -RedirectStandardOutput $doubleStdout `
            -RedirectStandardError $doubleStderr -Wait -PassThru -WindowStyle Hidden
        $code = $doubleProcess.ExitCode
        $doubleText = [IO.File]::ReadAllText($doubleStdout) + [IO.File]::ReadAllText($doubleStderr)
        if ($code -ne -1073740791 -or $doubleText -notmatch '(?m)^SECOND_EXCEPTION_THROWN\s*$') {
            throw "$level expected abort during double unwind, got $code"
        }
        Write-Output "$level double unwind terminated OK"
    } finally { Remove-Item Env:DCI_EH_DOUBLE_THROW -ErrorAction SilentlyContinue }
}
foreach ($level in @('O0', 'O2')) {
    $ir = Join-Path $run "inline-$level.ll"
    $exe = Join-Path $run "inline-$level.exe"
    & $Compiler --src=file (Join-Path $PSScriptRoot 'inline.vyx') --emit=ir "-$level" -o $ir
    if ($LASTEXITCODE -ne 0) { throw "$level inline IR emission failed" }
    & $clang $ir (Join-Path $run 'native.obj') "-L$runtime" -lvyx_runtime -ltbb12 -lsynchronization -lws2_32 -fms-runtime-lib=static -fuse-ld=lld -o $exe
    if ($LASTEXITCODE -ne 0) { throw "$level inline executable link failed" }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "$level inline stack cleanup failed: $LASTEXITCODE" }
}
