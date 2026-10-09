param(
    [Parameter(Mandatory=$true)][string]$Compiler,
    [string]$OutputDir = ''
)
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$Compiler = (Resolve-Path -LiteralPath $Compiler).Path
$fixture = Join-Path $repoRoot 'tests/projects/global_module_contract'
if (-not $OutputDir) { $OutputDir = Join-Path $PSScriptRoot ('.runs/storage-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff')) }
$runRoot = [IO.Path]::GetFullPath($OutputDir)
if (Test-Path -LiteralPath $runRoot) { throw "Refusing to overwrite existing output: $runRoot" }
[void][IO.Directory]::CreateDirectory($runRoot)
. (Join-Path $repoRoot 'bootstrap_compiler/scripts/VyxTestProcess.ps1')
$identity = [ordered]@{ compiler=$Compiler; sha256=(Get-FileHash -LiteralPath $Compiler -Algorithm SHA256).Hash }
$results = [Collections.Generic.List[object]]::new()
$savedPath = $env:PATH
$savedLlvmRoot = $env:LLVM_ROOT
if (-not $env:LLVM_ROOT -and (Test-Path -LiteralPath (Join-Path $repoRoot 'clang'))) {
    $env:LLVM_ROOT = Join-Path $repoRoot 'clang'
}
if ($env:LLVM_ROOT) { $env:PATH = (Join-Path $env:LLVM_ROOT 'bin') + [IO.Path]::PathSeparator + $env:PATH }

function Write-StorageText([string]$Path, [string]$Text) {
    [IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false))
}

function Invoke-StorageProcess([string]$Variant, [string]$File, [string]$Directory, [string[]]$Arguments) {
    $stdout = Join-Path $runRoot "$Variant.stdout.log"
    $stderr = Join-Path $runRoot "$Variant.stderr.log"
    $process = Invoke-VyxProcess -FilePath $File -WorkingDirectory $Directory -TimeoutSec 120 `
        -ArgumentList $Arguments -StdoutLog $stdout -StderrLog $stderr `
        -DialogLog (Join-Path $runRoot "$Variant.dialog.log")
    $diagnostics = [IO.File]::ReadAllText($stdout) + [IO.File]::ReadAllText($stderr)
    return [pscustomobject]@{
        variant=$Variant; exit_code=$process.ExitCode; diagnostics=$diagnostics
        file=$File; directory=$Directory; arguments=$Arguments
        compile_tasks=([regex]::Matches($diagnostics, '(?m)^\[\d+/\d+\] compile ')).Count
        stdout=$stdout; stderr=$stderr
    }
}

function Add-StorageResult($Result, [bool]$Passed) {
    $results.Add([ordered]@{
        variant=$Result.variant; exit_code=$Result.exit_code; passed=$Passed
        file=$Result.file; directory=$Result.directory; arguments=$Result.arguments
        compile_tasks=$Result.compile_tasks; stdout=$Result.stdout; stderr=$Result.stderr
    })
    Write-Host "$($Result.variant) passed=$Passed exit=$($Result.exit_code) compile_tasks=$($Result.compile_tasks)"
    if (-not $Passed) { Write-Host $Result.diagnostics }
}

function Invoke-StorageCompile([string]$Variant, [string]$Directory, [string[]]$Arguments) {
    $result = Invoke-StorageProcess $Variant $Compiler $Directory $Arguments
    Add-StorageResult $result ($result.exit_code -eq 0)
    if ($result.exit_code -ne 0) { throw "$Variant failed; logs: $runRoot" }
}

try {
    $isolated = Join-Path $runRoot 'isolated'
    $interfaces = Join-Path $isolated 'interfaces/global_contract'
    [void][IO.Directory]::CreateDirectory($interfaces)
    $objectPaths = [Collections.Generic.List[string]]::new()
    # Publish each module, then archive its sources before creating the next
    # consumer. Import resolution has .vyi files but no producer .vyx fallback.
    foreach ($module in @('storage', 'peer', 'reader', 'writer', 'peer_reader')) {
        $directory = Join-Path $isolated $module
        [void][IO.Directory]::CreateDirectory($directory)
        $source = Join-Path $directory "$module.vyx"
        Copy-Item -LiteralPath (Join-Path $fixture "src/$module.vyx") -Destination $source
        $common = @('--src=file', "$module.vyx", '--interface-root', '../interfaces')
        $unitSources = @($source)
        if ($module -eq 'storage') {
            $secondary = Join-Path $directory 'storage_text.vyx'
            Copy-Item -LiteralPath (Join-Path $fixture 'src/storage_text.vyx') -Destination $secondary
            Write-StorageText (Join-Path $directory 'unit.sources') "storage_text.vyx`n"
            $common += @('--unit-sources', 'unit.sources')
            $unitSources += $secondary
        }
        Invoke-StorageCompile "$module-interface" $directory ($common + @('--emit=vyi', '--vyi-shallow', '-o', "../interfaces/global_contract/$module.vyi"))
        Invoke-StorageCompile "$module-ir" $directory ($common + @('--emit=ir', '--export-top-level-roots', '-o', "$module.ll"))
        $objectPath = Join-Path $directory "$module.obj"
        Invoke-StorageCompile "$module-object" $directory ($common + @('--emit=obj', '--export-top-level-roots', '-o', $objectPath))
        $objectPaths.Add($objectPath)
        foreach ($unitSource in $unitSources) {
            Move-Item -LiteralPath $unitSource -Destination ($unitSource + '.archived')
        }
        if (@(Get-ChildItem -LiteralPath $isolated -Recurse -File -Filter '*.vyx').Count -ne 0) {
            throw 'Isolation violated: producer/consumer source remained available to a later module'
        }
    }
    $app = Join-Path $isolated 'app'
    [void][IO.Directory]::CreateDirectory($app)
    Copy-Item -LiteralPath (Join-Path $fixture 'src/main.vyx') -Destination (Join-Path $app 'main.vyx')
    $linkArguments = @('--src=file', 'main.vyx', '--interface-root', '../interfaces', '--emit=exe')
    foreach ($objectPath in $objectPaths) { $linkArguments += @('--link-obj', $objectPath) }
    $result = Invoke-StorageProcess 'isolated-link' $Compiler $app ($linkArguments + @('-o', 'storage.exe'))
    Add-StorageResult $result ($result.exit_code -eq 0)
    if ($result.exit_code -eq 0) {
        $result = Invoke-StorageProcess 'isolated-run' (Join-Path $app 'storage.exe') $app @()
        Add-StorageResult $result ($result.exit_code -eq 0 -and $result.diagnostics -match '(?m)^global module contract OK\r?$')
    }

    # A consumer must not define an imported mutable global. Omitting its owner
    # must leave unresolved storage, rather than silently linking private copies.
    Write-StorageText (Join-Path $app 'missing_owner.vyx') @'
module global_contract.missing_owner;
use global_contract.writer;
fn main() -> i32 { return writer_read(); }
'@
    $missingOwner = @('--src=file', 'missing_owner.vyx', '--interface-root', '../interfaces', '--emit=exe',
        '-o', 'missing-owner.exe', '--link-obj', (Join-Path $isolated 'writer/writer.obj'))
    $result = Invoke-StorageProcess 'missing-owner-link' $Compiler $app $missingOwner
    Add-StorageResult $result ($result.exit_code -ne 0 -and $result.diagnostics -match '(?i)(undefined|unresolved)' -and $result.diagnostics -match 'counter')

    $project = Join-Path $runRoot 'project'
    $sourceRoot = Join-Path $project 'src'
    [void][IO.Directory]::CreateDirectory($sourceRoot)
    Copy-Item -LiteralPath (Join-Path $fixture 'Vyx.toml') -Destination $project
    foreach ($source in Get-ChildItem -LiteralPath (Join-Path $fixture 'src') -File -Filter '*.vyx') {
        Copy-Item -LiteralPath $source.FullName -Destination $sourceRoot
    }
    foreach ($variant in @('cold', 'warm', 'incremental')) {
        if ($variant -eq 'incremental') {
            # Change a secondary producer source without changing its public
            # declarations; both consumers must observe the rebuilt storage.
            foreach ($file in @('storage_text.vyx', 'reader.vyx', 'writer.vyx')) {
                $path = Join-Path $sourceRoot $file
                $body = [IO.File]::ReadAllText($path)
                if (-not $body.Contains('producer string')) { throw "incremental anchor missing: $file" }
                Write-StorageText $path ($body.Replace('producer string', 'updated producer string'))
            }
        }
        $result = Invoke-StorageProcess "project-$variant" $Compiler $project @('--src=project', '.', '--run=aot', '-O2', '-j1')
        $passed = $result.exit_code -eq 0 -and $result.diagnostics -match '(?m)^global module contract OK\r?$'
        if ($variant -eq 'warm') { $passed = $passed -and $result.compile_tasks -eq 0 }
        if ($variant -eq 'incremental') {
            $passed = $passed -and $result.diagnostics -match '(?m)^\[\d+/\d+\] compile .+ -> .*module_global_contract_storage\.(?:obj|o)\r?$'
        }
        Add-StorageResult $result $passed
    }
} finally {
    [ordered]@{ identity=$identity; tests=@($results.ToArray()); run=$runRoot } | ConvertTo-Json -Depth 6 |
        Set-Content -LiteralPath (Join-Path $runRoot 'results.json') -Encoding utf8
    $env:PATH = $savedPath
    $env:LLVM_ROOT = $savedLlvmRoot
}
if ($results | Where-Object { -not $_.passed }) { exit 1 }
Write-Host "cross-module storage: PASS ($runRoot)"
