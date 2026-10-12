param([Parameter(Mandatory)][string]$Compiler)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$Compiler = (Resolve-Path -LiteralPath $Compiler).Path
$run = Join-Path $PSScriptRoot ('.runs/cgu-policy-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $run | Out-Null
$source = [IO.File]::ReadAllText((Join-Path $repo 'bootstrap_compiler/src/codegen/llvm/llvm_lower.vyx'))
$text = "extern `"C`" { fn getenv(name: rawptr) -> rawptr; }`n"
foreach ($name in @('llvm_mir_env_value','llvm_mir_parse_positive_i32','llvm_mir_codegen_unit_limit')) {
    $start = [regex]::Match($source, '(?m)^fn ' + $name + '\(')
    if (!$start.Success) { throw "Missing $name" }
    $tail = $source.Substring($start.Index)
    $end = [regex]::Match($tail, '(?m)^}\s*$')
    if (!$end.Success) { throw "Unclosed $name" }
    $text += $tail.Substring(0, $end.Index + $end.Length) + "`n"
}
$text += 'fn main() -> i32 { let expected = llvm_mir_parse_positive_i32(getArg(1)); let actual = llvm_mir_codegen_unit_limit(); if (actual != expected) { print("CGU mismatch actual=" + actual.toString()); return 1; } return 0; }'
[IO.File]::WriteAllText((Join-Path $run 'policy.vyx'), $text, [Text.UTF8Encoding]::new($false))
$saved = @{}
foreach ($name in @('VYX_CODEGEN_UNITS','VYX_CGU_EMIT_THREADS','VYX_CGU_THREADS')) { $saved[$name] = [Environment]::GetEnvironmentVariable($name) }
try {
    foreach ($opt in @(0,2)) {
        $exe = Join-Path $run "policy-O$opt.exe"
        & $Compiler --src=file (Join-Path $run 'policy.vyx') --emit=exe "-O$opt" -o $exe *> (Join-Path $run "compile-O$opt.log")
        if ($LASTEXITCODE -ne 0) { throw "CGU policy O$opt compilation failed" }
        $cases = @(
            @{ units=''; threads=''; alternate=''; expected=16; args=@() },
            @{ units=''; threads='1'; alternate=''; expected=1; args=@() },
            @{ units=''; threads=''; alternate='1'; expected=1; args=@() },
            @{ units=''; threads='2'; alternate='1'; expected=16; args=@() },
            @{ units='4'; threads='1'; alternate=''; expected=4; args=@() },
            @{ units='4'; threads='1'; alternate=''; expected=3; args=@('--codegen-units','3') },
            @{ units=''; threads='1'; alternate=''; expected=8; args=@('-C','codegen-units=8') }
        )
        foreach ($case in $cases) {
            $env:VYX_CODEGEN_UNITS = $case.units; $env:VYX_CGU_EMIT_THREADS = $case.threads; $env:VYX_CGU_THREADS = $case.alternate
            & $exe ([string]$case.expected) @($case.args)
            if ($LASTEXITCODE -ne 0) { throw "CGU policy O$opt case failed: $($case | ConvertTo-Json -Compress)" }
        }
    }
} finally {
    foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name]) }
}
@{ compiler=$Compiler; sha256=(Get-FileHash $Compiler -Algorithm SHA256).Hash; result='PASS'; checks=14 } | ConvertTo-Json | Set-Content (Join-Path $run 'result.json')
Write-Output "CGU policy O0/O2 14/14 PASS: $run"
