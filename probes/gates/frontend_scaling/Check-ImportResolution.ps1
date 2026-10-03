param(
    [string]$Compiler = (Join-Path $PSScriptRoot '../../../bootstrap_compiler/out/boot.exe'),
    [string]$Label = 'baseline',
    [string]$ReferenceResult = ''
)
$ErrorActionPreference = 'Stop'
$compilerPath = (Resolve-Path -LiteralPath $Compiler).Path
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$root = Join-Path $PSScriptRoot '.runs/import-resolution'
$cases = @(
    @{ Name='ordered'; Ok=$true; Main='use Resolution.Left; use Resolution.Right; fn main() -> i32 { return left(20) + right(21); }' },
    @{ Name='reversed'; Ok=$true; Main='use Resolution.Right; use Resolution.Left; fn main() -> i32 { return left(20) + right(21); }' },
    @{ Name='alias'; Ok=$true; Main='use chosen = Resolution.Right.right; fn main() -> i32 { return chosen(41); }' },
    @{ Name='qualified'; Ok=$true; Main='use Resolution.Left; use Resolution.Right; fn main() -> i32 { return Resolution.Right.right(41); }' },
    @{ Name='cycle'; Ok=$true; Main='use Resolution.CycleA; use Resolution.CycleB; fn main() -> i32 { return fromA() + fromB(); }' },
    @{ Name='generic'; Ok=$true; Main='use Resolution.Generic; fn main() -> i32 { return identity::<i32>(42); }' },
    @{ Name='nested_type_use'; Ok=$true; Main='use Resolution.Nested; use Resolution.Types; fn main() -> i32 { let value = make(); return inspect(value); }' },
    @{ Name='nested_use_alias'; Ok=$true; Main='use Payload = Resolution.Types.Payload; use inspect = Resolution.Nested.inspect; use Resolution.Types; fn main() -> i32 { let value: Payload = make(); return inspect(value); }' },
    @{ Name='missing'; Ok=$false; Main='use Resolution.Left; use Resolution.Right; fn main() -> i32 { return notDeclared(41); }' },
    @{ Name='wrong_type'; Ok=$false; Main='use Resolution.Right; fn main() -> i32 { return right(true); }' }
)
$rows = [Collections.Generic.List[object]]::new()
foreach ($case in $cases) {
    $dir = Join-Path $root $case.Name
    $interfaces = Join-Path $dir 'interfaces'
    New-Item -ItemType Directory -Path $interfaces -Force | Out-Null
    $definitions = @{
        'left.vyi' = 'module Resolution.Left; public fn left(n: i32) -> i32;'
        'right.vyi' = 'module Resolution.Right; public fn right(n: i32) -> i32;'
        'cycle_a.vyi' = 'module Resolution.CycleA; use Resolution.CycleB; public fn fromA() -> i32;'
        'cycle_b.vyi' = 'module Resolution.CycleB; use Resolution.CycleA; public fn fromB() -> i32;'
        'generic.vyi' = 'module Resolution.Generic; public fn identity<T>(value: T) -> T { return value; }'
        'types.vyi' = 'module Resolution.Types; public class Payload { public value: i32; } public fn make() -> Payload;'
        'nested.vyi' = 'module Resolution.Nested; use Payload = Resolution.Types.Payload; public fn inspect(value: Payload) -> i32;'
    }
    foreach ($item in $definitions.GetEnumerator()) {
        [IO.File]::WriteAllText((Join-Path $interfaces $item.Key), $item.Value.Replace('; ', ";`n").Replace('} public', "}`npublic"))
    }
    $inputPath = Join-Path $dir 'main.vyx'
    $irPath = Join-Path $dir ($Label + '.ll')
    [IO.File]::WriteAllText($inputPath, $case.Main.Replace('; ', ";`n"))
    $start = [Diagnostics.ProcessStartInfo]::new($compilerPath)
    $start.WorkingDirectory = $dir
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    foreach ($arg in @('--src=file', $inputPath, '--emit=ir', '-o', $irPath, '--interface-root', $interfaces, '--no-implicit-std')) {
        $start.ArgumentList.Add($arg)
    }
    $start.Environment['LLVM_ROOT'] = Join-Path $repo 'clang'
    $proc = [Diagnostics.Process]::new()
    $proc.StartInfo = $start
    if (-not $proc.Start()) { throw 'Failed to start compiler' }
    $outTask = $proc.StandardOutput.ReadToEndAsync()
    $errTask = $proc.StandardError.ReadToEndAsync()
    if (-not $proc.WaitForExit(20000)) {
        $proc.Kill($true)
        $proc.WaitForExit()
        throw "Import case $($case.Name) timed out"
    }
    $stdout = $outTask.GetAwaiter().GetResult()
    $stderr = $errTask.GetAwaiter().GetResult()
    [IO.File]::WriteAllText((Join-Path $dir ($Label + '.stdout')), $stdout)
    [IO.File]::WriteAllText((Join-Path $dir ($Label + '.stderr')), $stderr)
    $diagnostics = @([regex]::Matches($stdout + $stderr, 'error: E[0-9]+:[^\r\n]*') | ForEach-Object { $_.Value })
    $hash = if ($proc.ExitCode -eq 0 -and (Test-Path -LiteralPath $irPath)) { (Get-FileHash -LiteralPath $irPath).Hash } else { '' }
    $rows.Add([pscustomobject]@{case=$case.Name; expected_success=$case.Ok; exit=$proc.ExitCode; ir_sha256=$hash; diagnostics=$diagnostics})
    $proc.Dispose()
}
$result = [ordered]@{compiler=$compilerPath; compiler_sha256=(Get-FileHash -LiteralPath $compilerPath).Hash; cases=$rows.ToArray()}
$result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $root ($Label + '.json'))
$rows | Format-Table case,expected_success,exit,ir_sha256
if (@($rows | Where-Object { (($_.exit -eq 0) -ne $_.expected_success) -or (-not $_.expected_success -and $_.diagnostics.Count -eq 0) }).Count -ne 0) {
    throw 'Import resolution gate failed; inspect the saved diagnostics'
}
if ($ReferenceResult) {
    $reference = Get-Content -Raw -LiteralPath $ReferenceResult | ConvertFrom-Json
    foreach ($row in $rows) {
        $before = @($reference.cases | Where-Object { $_.case -eq $row.case })
        if ($before.Count -ne 1 -or $before[0].exit -ne $row.exit -or $before[0].ir_sha256 -ne $row.ir_sha256 -or
            (($before[0].diagnostics -join "`n") -cne ($row.diagnostics -join "`n"))) {
            throw "Import case $($row.case) differs from the reference IR/diagnostics"
        }
    }
    Write-Output 'Import IR and diagnostics match the reference.'
}
