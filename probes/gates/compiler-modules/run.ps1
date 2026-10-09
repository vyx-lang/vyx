param(
    [Parameter(Mandatory=$true)][string]$Compiler,
    [string]$OutputDir = ''
)
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$Compiler = (Resolve-Path -LiteralPath $Compiler).Path
$fixture = Join-Path $repoRoot 'tests/projects/impl_module_contract'
if (-not $OutputDir) { $OutputDir = Join-Path $PSScriptRoot ('.runs/' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff')) }
$runRoot = [IO.Path]::GetFullPath($OutputDir)
if (Test-Path -LiteralPath $runRoot) { throw "Refusing to overwrite existing output: $runRoot" }
[void][IO.Directory]::CreateDirectory($runRoot)
. (Join-Path $repoRoot 'bootstrap_compiler/scripts/VyxTestProcess.ps1')
$identity = @{ compiler=$Compiler; sha256=(Get-FileHash -LiteralPath $Compiler -Algorithm SHA256).Hash }
$identity | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $runRoot 'compiler.json') -Encoding utf8
$results = [Collections.Generic.List[object]]::new()

function Write-ContractText([string]$Path, [string]$Text) {
    [IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false))
}

function Invoke-ContractCompiler([string]$Variant, [string]$Project, [string[]]$Arguments) {
    $stdout = Join-Path $runRoot "$Variant.stdout.log"
    $stderr = Join-Path $runRoot "$Variant.stderr.log"
    $process = Invoke-VyxProcess -FilePath $Compiler -WorkingDirectory $Project -TimeoutSec 120 `
        -ArgumentList $Arguments -StdoutLog $stdout -StderrLog $stderr `
        -DialogLog (Join-Path $runRoot "$Variant.dialog.log")
    $diagnostics = [IO.File]::ReadAllText($stdout) + [IO.File]::ReadAllText($stderr)
    return [pscustomobject]@{
        variant=$Variant; exit_code=$process.ExitCode; diagnostics=$diagnostics
        compile_tasks=([regex]::Matches($diagnostics, '(?m)^\[\d+/\d+\] compile ')).Count
        stdout=$stdout; stderr=$stderr
    }
}

function Add-ContractResult($Result, [bool]$Passed) {
    $results.Add(@{
        variant=$Result.variant; exit_code=$Result.exit_code; passed=$Passed
        compile_tasks=$Result.compile_tasks; stdout=$Result.stdout; stderr=$Result.stderr
    })
    Write-Host "$($Result.variant) passed=$Passed exit=$($Result.exit_code) compile_tasks=$($Result.compile_tasks)"
    if (-not $Passed) { Write-Host $Result.diagnostics }
}

function Test-PublicInterface([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return $false }
    $interface = [IO.File]::ReadAllText($Path)
    return $interface.Contains('fn bump(') -and $interface.Contains('fn read(') `
        -and -not $interface.Contains('fn hidden(') -and -not $interface.Contains('PrivateCounter')
}

$project = Join-Path $runRoot 'positive'
$sourceRoot = Join-Path $project 'src'
[void][IO.Directory]::CreateDirectory($sourceRoot)
Copy-Item -LiteralPath (Join-Path $fixture 'Vyx.toml') -Destination $project
foreach ($source in Get-ChildItem -LiteralPath (Join-Path $fixture 'src') -File -Filter '*.vyx') {
    Copy-Item -LiteralPath $source.FullName -Destination $sourceRoot
}
$interfaceRoot = Join-Path $project 'target/.vyx_interfaces/impl_module_contract'
$vyi = Join-Path $interfaceRoot 'module_contract/counter.vyi'
foreach ($variant in @('positive', 'warm', 'incremental')) {
    $expected = 42
    if ($variant -eq 'incremental') {
        $inspection = Join-Path $sourceRoot 'inspection.vyx'
        $body = [IO.File]::ReadAllText($inspection)
        if (-not $body.Contains('return self.hidden();')) { throw 'incremental method anchor missing' }
        Write-ContractText $inspection ($body.Replace('return self.hidden();', 'return self.hidden() + 1;'))
        $main = Join-Path $sourceRoot 'main.vyx'
        $body = [IO.File]::ReadAllText($main)
        if (-not $body.Contains('answer != 42')) { throw 'incremental expectation anchor missing' }
        Write-ContractText $main ($body.Replace('answer != 42', 'answer != 43'))
        $expected = 43
    }
    $result = Invoke-ContractCompiler $variant $project @('--src=project', '.', '--run=aot', '-O2', '-j1')
    $passed = $result.exit_code -eq 0 -and $result.diagnostics -match "(?m)^$expected\r?$" `
        -and (Test-PublicInterface $vyi)
    if ($variant -eq 'warm') { $passed = $passed -and $result.compile_tasks -eq 0 }
    if ($variant -eq 'incremental') {
        $passed = $passed -and $result.diagnostics -match '(?m)^\[\d+/\d+\] compile .+ -> .*module_module_contract_counter\.(?:obj|o)\r?$'
    }
    Add-ContractResult $result $passed
}

# Cover source-level and qualified semantic identities, including an explicit
# @[vis(world)] on the private owner's impl. Project-private emission keeps it.
$sources = Join-Path $project 'interface.sources'
Write-ContractText $sources "src/arithmetic.vyx`nsrc/inspection.vyx`n"
foreach ($mode in @('shallow', 'full', 'project-private')) {
    $output = Join-Path $project "$mode.vyi"
    $arguments = @('--src=file', 'src/model.vyx', '--emit=vyi', '--unit-sources', $sources, '-o', $output)
    if ($mode -eq 'shallow') { $arguments += '--vyi-shallow' }
    if ($mode -eq 'project-private') { $arguments += '--vyi-project-private' }
    $result = Invoke-ContractCompiler "interface-$mode" $project $arguments
    $passed = $result.exit_code -eq 0 -and (Test-Path -LiteralPath $output)
    if ($passed -and $mode -eq 'project-private') {
        $interface = [IO.File]::ReadAllText($output)
        $passed = $interface.Contains('PrivateCounter') -and $interface.Contains('fn amplify(') `
            -and $interface.Contains('fn exposed(') -and $interface.Contains('fn hidden(')
    } elseif ($passed) {
        $passed = Test-PublicInterface $output
    }
    Add-ContractResult $result $passed
}

# A shallow unit has no imported owner declarations yet. Qualified external
# owners and primitives must retain their method contracts in both modes.
$qualified = Join-Path $runRoot 'qualified-owner'
[void][IO.Directory]::CreateDirectory($qualified)
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'qualified_owner.vyx') -Destination $qualified
foreach ($mode in @('shallow', 'full')) {
    $output = Join-Path $qualified "$mode.vyi"
    $arguments = @('--src=file', 'qualified_owner.vyx', '--emit=vyi', '--interface-root', $interfaceRoot, '-o', $output)
    if ($mode -eq 'shallow') { $arguments += '--vyi-shallow' }
    $result = Invoke-ContractCompiler "qualified-owner-$mode" $qualified $arguments
    $passed = $result.exit_code -eq 0 -and (Test-Path -LiteralPath $output)
    if ($passed) {
        $interface = [IO.File]::ReadAllText($output)
        $passed = $interface.Contains('impl module_contract.counter.Counter {') `
            -and $interface.Contains('fn extension(') -and $interface.Contains('impl i32 {') `
            -and $interface.Contains('fn double_value(') -and -not $interface.Contains('fn local_only(')
    }
    Add-ContractResult $result $passed
}

foreach ($variant in @('negative', 'private-owner-negative')) {
    $consumer = Join-Path $runRoot $variant
    [void][IO.Directory]::CreateDirectory($consumer)
    if ($variant -eq 'negative') {
        $body = [IO.File]::ReadAllText((Join-Path $fixture 'src/main.vyx'))
        if (-not $body.Contains('counter.read()')) { throw 'negative fixture substitution anchor missing' }
        $body = $body.Replace('counter.read()', 'counter.hidden()')
    } else {
        $body = @'
module module_contract.private_consumer;
use module_contract.counter;
fn main() -> i32 {
    var counter = PrivateCounter {};
    print(counter.amplify());
    return 0;
}
'@
    }
    Write-ContractText (Join-Path $consumer 'main.vyx') $body
    $result = Invoke-ContractCompiler $variant $consumer @(
        '--src=file', 'main.vyx', '--emit=ir', '-o', 'denied.ll', '--interface-root', $interfaceRoot
    )
    $pattern = if ($variant -eq 'negative') { "cannot call .hidden." } else { 'PrivateCounter' }
    $passed = $result.exit_code -ne 0 -and $result.diagnostics -match $pattern
    Add-ContractResult $result $passed
}

@{ identity=$identity; tests=@($results.ToArray()) } | ConvertTo-Json -Depth 5 |
    Set-Content -LiteralPath (Join-Path $runRoot 'results.json') -Encoding utf8
if ($results | Where-Object { -not $_.passed }) { exit 1 }
Write-Host "compiler module contracts: PASS ($runRoot)"
