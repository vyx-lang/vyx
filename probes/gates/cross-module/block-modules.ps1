param(
    [Parameter(Mandatory=$true)][string]$Compiler,
    [string]$OutputDir = '',
    [string]$CaseFilter = ''
)
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$Compiler = (Resolve-Path -LiteralPath $Compiler).Path
$fixture = Join-Path $repoRoot 'tests/projects/block_module_contract'
$negativeFixture = Join-Path $PSScriptRoot 'block_modules'
if (-not $OutputDir) { $OutputDir = Join-Path $PSScriptRoot ('.runs/block-modules-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff')) }
$runRoot = [IO.Path]::GetFullPath($OutputDir)
if (Test-Path -LiteralPath $runRoot) { throw "Refusing to overwrite existing output: $runRoot" }
[void][IO.Directory]::CreateDirectory($runRoot)
. (Join-Path $repoRoot 'bootstrap_compiler/scripts/VyxTestProcess.ps1')
$identity = [ordered]@{ compiler=$Compiler; sha256=(Get-FileHash -LiteralPath $Compiler -Algorithm SHA256).Hash }
$results = [Collections.Generic.List[object]]::new()
$failures = [Collections.Generic.List[string]]::new()
$savedPath = $env:PATH
$savedLlvmRoot = $env:LLVM_ROOT
$savedCodegenUnits = $env:VYX_CODEGEN_UNITS
$env:VYX_CODEGEN_UNITS = '1'
if (-not $env:LLVM_ROOT -and (Test-Path -LiteralPath (Join-Path $repoRoot 'clang'))) { $env:LLVM_ROOT = Join-Path $repoRoot 'clang' }
if ($env:LLVM_ROOT) { $env:PATH = (Join-Path $env:LLVM_ROOT 'bin') + [IO.Path]::PathSeparator + $env:PATH }

function Write-BlockText([string]$Path, [string]$Text) {
    [IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false))
}

function Invoke-BlockProcess([string]$Name, [string]$File, [string]$Directory, [string[]]$Arguments,
    [switch]$Run, [switch]$Warm, [string]$DiagnosticFile = '', [int]$DiagnosticLine = 0, [string]$DiagnosticName = '') {
    $stdout = Join-Path $runRoot "$Name.stdout.log"
    $stderr = Join-Path $runRoot "$Name.stderr.log"
    $process = Invoke-VyxProcess -FilePath $File -WorkingDirectory $Directory -TimeoutSec 120 `
        -ArgumentList $Arguments -StdoutLog $stdout -StderrLog $stderr -DialogLog (Join-Path $runRoot "$Name.dialog.log")
    $diagnostics = [IO.File]::ReadAllText($stdout) + [IO.File]::ReadAllText($stderr)
    $passed = $process.ExitCode -eq 0
    $compileTasks = ([regex]::Matches($diagnostics, '(?m)^\[\d+/\d+\] compile ')).Count
    if ($Run) { $passed = $passed -and $diagnostics -match '(?m)^block module contract OK\r?$' }
    if ($Warm) { $passed = $passed -and $compileTasks -eq 0 }
    if ($DiagnosticFile) {
        $diagnosticPattern = [regex]::Escape($DiagnosticFile) + ':' + $DiagnosticLine + ':\d+: error: E2100: [^\r\n]*\b' + [regex]::Escape($DiagnosticName) + '\b'
        $passed = $process.ExitCode -ne 0 -and $diagnostics -match $diagnosticPattern
    }
    $results.Add([ordered]@{
        variant=$Name; exit_code=$process.ExitCode; passed=$passed; file=$File; directory=$Directory
        arguments=$Arguments; compile_tasks=$compileTasks; diagnostic_file=$DiagnosticFile; diagnostic_line=$DiagnosticLine
        diagnostic_name=$DiagnosticName; stdout=$stdout; stderr=$stderr
    })
    Write-Host "$Name passed=$passed exit=$($process.ExitCode) compile_tasks=$compileTasks"
    if (-not $passed) { Write-Host $diagnostics; throw "$Name failed" }
}

function Invoke-BlockCase([string]$Name, [scriptblock]$Body) {
    if ($CaseFilter -and $Name -notmatch $CaseFilter) { return }
    try { & $Body }
    catch { $failures.Add("${Name}: $($_.Exception.Message)"); Write-Host "FAIL ${Name}: $($_.Exception.Message)" }
}

try {
    Invoke-BlockCase 'root-scope' {
        $case = Join-Path $runRoot 'root-scope'
        [void][IO.Directory]::CreateDirectory($case)
        Copy-Item -LiteralPath (Join-Path $negativeFixture 'RootScope.vyx') -Destination $case
        foreach ($optimization in @('O0', 'O2')) {
            Invoke-BlockProcess "root-scope-$optimization-link" $Compiler $case @('--src=file', 'RootScope.vyx',
                '--emit=exe', "-$optimization", '-o', "root-$optimization.exe")
            Invoke-BlockProcess "root-scope-$optimization-run" (Join-Path $case "root-$optimization.exe") $case @() -Run
        }
    }

    Invoke-BlockCase 'single-file' {
        $case = Join-Path $runRoot 'single-file'
        [void][IO.Directory]::CreateDirectory($case)
        $body = [IO.File]::ReadAllText((Join-Path $fixture 'src/scopes.vyx')) + "`n" +
            [IO.File]::ReadAllText((Join-Path $fixture 'src/main.vyx'))
        Write-BlockText (Join-Path $case 'blocks.vyx') $body
        foreach ($optimization in @('O0', 'O2')) {
            Invoke-BlockProcess "single-file-$optimization-link" $Compiler $case @('--src=file', 'blocks.vyx',
                '--emit=exe', "-$optimization", '-o', "blocks-$optimization.exe")
            Invoke-BlockProcess "single-file-$optimization-run" (Join-Path $case "blocks-$optimization.exe") $case @() -Run
        }
    }

    Invoke-BlockCase 'unity' {
        $case = Join-Path $runRoot 'unity'
        [void][IO.Directory]::CreateDirectory($case)
        foreach ($source in @('main.vyx', 'scopes.vyx')) {
            Copy-Item -LiteralPath (Join-Path $fixture "src/$source") -Destination $case
        }
        Write-BlockText (Join-Path $case 'unit.sources') "scopes.vyx`n"
        foreach ($optimization in @('O0', 'O2')) {
            Invoke-BlockProcess "unity-$optimization-link" $Compiler $case @('--src=file', 'main.vyx', '--project-unit-sources', 'unit.sources',
                '--emit=exe', "-$optimization", '-o', "blocks-$optimization.exe")
            Invoke-BlockProcess "unity-$optimization-run" (Join-Path $case "blocks-$optimization.exe") $case @() -Run
        }
    }

    foreach ($interface in @('shallow', 'full')) {
        Invoke-BlockCase "interface-$interface" {
            $case = Join-Path $runRoot "interface-$interface"
            $interfaces = Join-Path $case 'interfaces'
            $producer = Join-Path $case 'producer'
            $consumer = Join-Path $case 'consumer'
            foreach ($path in @($interfaces, $producer, $consumer)) { [void][IO.Directory]::CreateDirectory($path) }
            $source = Join-Path $producer 'scopes.vyx'
            Copy-Item -LiteralPath (Join-Path $fixture 'src/scopes.vyx') -Destination $source
            $interfaceArguments = @('--src=file', 'scopes.vyx', '--emit=vyi', '-o', '../interfaces/BlockScopeRoot.vyi')
            if ($interface -eq 'shallow') { $interfaceArguments += '--vyi-shallow' }
            Invoke-BlockProcess "$interface-interface" $Compiler $producer $interfaceArguments
            Invoke-BlockProcess "$interface-producer-object" $Compiler $producer @('--src=file', 'scopes.vyx', '--emit=obj',
                '--export-top-level-roots', '-o', 'scopes.obj')
            Move-Item -LiteralPath $source -Destination ($source + '.archived')
            if (@(Get-ChildItem -LiteralPath $case -Recurse -File -Filter '*.vyx').Count -ne 0) { throw 'Block producer source isolation violated' }
            Copy-Item -LiteralPath (Join-Path $fixture 'src/main.vyx') -Destination $consumer
            Invoke-BlockProcess "$interface-consumer-link" $Compiler $consumer @('--src=file', 'main.vyx', '--interface-root', '../interfaces',
                '--emit=exe', '-o', 'blocks.exe', '--link-obj', '../producer/scopes.obj')
            Invoke-BlockProcess "$interface-consumer-run" (Join-Path $consumer 'blocks.exe') $consumer @() -Run
        }
    }

    foreach ($buildMode in @('unity', 'parallel')) {
        Invoke-BlockCase "project-$buildMode" {
            $case = Join-Path $runRoot "project-$buildMode"
            $sourceRoot = Join-Path $case 'src'
            [void][IO.Directory]::CreateDirectory($sourceRoot)
            $manifest = [IO.File]::ReadAllText((Join-Path $fixture 'Vyx.toml'))
            if ($buildMode -eq 'parallel') { $manifest = $manifest.Replace('parallel_vyx = false', 'parallel_vyx = true') }
            Write-BlockText (Join-Path $case 'Vyx.toml') $manifest
            foreach ($source in @('main.vyx', 'scopes.vyx')) {
                Copy-Item -LiteralPath (Join-Path $fixture "src/$source") -Destination $sourceRoot
            }
            Invoke-BlockProcess "project-$buildMode-cold" $Compiler $case @('--src=project', '.', '--run=aot', '-O2', '-j1') -Run
            Invoke-BlockProcess "project-$buildMode-warm" $Compiler $case @('--src=project', '.', '--run=aot', '-O2', '-j1') -Run -Warm
        }
    }

    $negativeCases = @(
        @{ name='DuplicateVar'; line=4; symbol='counter' },
        @{ name='DuplicateConst'; line=4; symbol='LIMIT' },
        @{ name='ReopenedVar'; line=6; symbol='counter' },
        @{ name='ReopenedConst'; line=6; symbol='LIMIT' },
        @{ name='ReopenedNested'; line=9; symbol='counter' },
        @{ name='RestoredParent'; line=6; symbol='counter' }
    )
    foreach ($negative in $negativeCases) {
        Invoke-BlockCase "negative-$($negative.name)" {
            $case = Join-Path $runRoot $negative.name
            [void][IO.Directory]::CreateDirectory($case)
            $sourceName = $negative.name + '.vyx'
            Copy-Item -LiteralPath (Join-Path $negativeFixture $sourceName) -Destination $case
            Invoke-BlockProcess "reject-$($negative.name)" $Compiler $case @('--src=file', $sourceName, '--emit=exe', '-o', 'invalid.exe') `
                -DiagnosticFile $sourceName -DiagnosticLine $negative.line -DiagnosticName $negative.symbol
        }
    }

    Invoke-BlockCase 'negative-project-duplicate' {
        $case = Join-Path $runRoot 'project-duplicate'
        [void][IO.Directory]::CreateDirectory($case)
        foreach ($sourceName in @('ProjectDuplicateMain.vyx', 'ProjectDuplicateA.vyx', 'ProjectDuplicateB.vyx')) {
            Copy-Item -LiteralPath (Join-Path $negativeFixture $sourceName) -Destination $case
        }
        Write-BlockText (Join-Path $case 'unit.sources') "ProjectDuplicateA.vyx`nProjectDuplicateB.vyx`n"
        Invoke-BlockProcess 'reject-project-duplicate' $Compiler $case @('--src=file', 'ProjectDuplicateMain.vyx',
            '--project-unit-sources', 'unit.sources', '--emit=exe', '-o', 'invalid.exe') `
            -DiagnosticFile 'ProjectDuplicateB.vyx' -DiagnosticLine 3 -DiagnosticName 'counter'
    }

    Invoke-BlockCase 'negative-project-manifest-duplicate' {
        $case = Join-Path $runRoot 'project-manifest-duplicate'
        [void][IO.Directory]::CreateDirectory($case)
        foreach ($sourceName in @('ProjectDuplicateMain.vyx', 'ProjectDuplicateA.vyx', 'ProjectDuplicateB.vyx')) {
            Copy-Item -LiteralPath (Join-Path $negativeFixture $sourceName) -Destination $case
        }
        Write-BlockText (Join-Path $case 'Vyx.toml') @'
[package]
name = "project_duplicate"
version = "0.1.0"
entry = "ProjectDuplicateMain.vyx"
[build]
output_dir = "target"
[target.project_duplicate]
type = "executable"
entry = "ProjectDuplicateMain.vyx"
auto_sources = false
parallel_vyx = false
sources = ["ProjectDuplicateA.vyx", "ProjectDuplicateB.vyx"]
'@
        Invoke-BlockProcess 'reject-project-manifest-duplicate' $Compiler $case @('build', '--target', 'project_duplicate', '-j1') `
            -DiagnosticFile 'ProjectDuplicateB.vyx' -DiagnosticLine 3 -DiagnosticName 'counter'
    }
} finally {
    [ordered]@{ identity=$identity; tests=@($results.ToArray()); failures=@($failures.ToArray()); run=$runRoot } |
        ConvertTo-Json -Depth 7 | Set-Content -LiteralPath (Join-Path $runRoot 'results.json') -Encoding utf8
    $env:PATH = $savedPath
    $env:LLVM_ROOT = $savedLlvmRoot
    $env:VYX_CODEGEN_UNITS = $savedCodegenUnits
}
if ($results.Count -eq 0) { throw 'No block module cases selected' }
if ($failures.Count -gt 0 -or ($results | Where-Object { -not $_.passed })) { exit 1 }
Write-Host "block module storage: PASS ($runRoot)"
