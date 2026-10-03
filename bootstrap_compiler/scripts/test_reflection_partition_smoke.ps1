$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$compiler = Join-Path $root "out\boot.exe"
if (-not (Test-Path -LiteralPath $compiler)) {
    throw "[reflection-partition] compiler missing: $compiler"
}

$testRoot = Join-Path $root "out\reflection_partition_smoke"
$outRoot = (Resolve-Path -LiteralPath (Join-Path $root "out")).Path
if (Test-Path -LiteralPath $testRoot) {
    $resolvedTestRoot = (Resolve-Path -LiteralPath $testRoot).Path
    if (-not $resolvedTestRoot.StartsWith($outRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "[reflection-partition] refusing cleanup outside compiler out: $resolvedTestRoot"
    }
    Remove-Item -LiteralPath $resolvedTestRoot -Recurse -Force
}

function New-VyxProject {
    param(
        [Parameter(Mandatory=$true)][string]$Path,
        [Parameter(Mandatory=$true)][string]$Name,
        [Parameter(Mandatory=$true)][string]$Source
    )

    $srcDir = Join-Path $Path "src"
    New-Item -ItemType Directory -Force -Path $srcDir | Out-Null
    Set-Content -LiteralPath (Join-Path $Path "Vyx.toml") -Encoding utf8 -Value @"
[package]
name = "$Name"
version = "0.1.0"
entry = "src/main.vyx"

[build]
output_dir = "target"
cache_dir = ".cache"
threads = 10

[target.$Name]
type = "executable"
entry = "src/main.vyx"
"@
    Set-Content -LiteralPath (Join-Path $srcDir "main.vyx") -Encoding utf8 -Value $Source
}

try {
    $directProject = Join-Path $testRoot "direct"
    New-VyxProject -Path $directProject -Name "reflection_direct_smoke" -Source @'
use std.reflect;

public interface ITest {
    fn call(self);
}

@[reflect("TestCRef", alias=["TestC"])]
public class TestC : ITest {
    public TestC() {}
    public fn call(self) { print("TestC call"); }
}

fn main() -> i32 {
    let tc = TestC();
    tc.call();
    let reflected = getType("TestC");
    if (!reflected.valid) { return 2; }
    return 0;
}
'@

    $directOutput = & $compiler --src=project $directProject --run=aot 2>&1
    $directExit = $LASTEXITCODE
    if ($directExit -ne 0 -or (($directOutput -join "`n") -notmatch 'TestC call')) {
        Write-Host ($directOutput -join [Environment]::NewLine)
        throw "[reflection-partition] direct reflection + ordinary call failed (exit=$directExit)"
    }

    $partitionProject = Join-Path $testRoot "partitioned"
    $source = [System.Text.StringBuilder]::new()
    [void]$source.AppendLine('use std.reflect;')
    [void]$source.AppendLine('@[reflect("ReflectedAlias", alias=["Reflected"])]')
    [void]$source.AppendLine('public class Reflected {')
    [void]$source.AppendLine('    public value: i32;')
    [void]$source.AppendLine('    public fn get(self) -> i32 { return self.value; }')
    [void]$source.AppendLine('}')
    for ($i = 0; $i -lt 205; $i++) {
        [void]$source.AppendLine("public fn root_$i() -> i32 { return $i; }")
    }
    [void]$source.AppendLine('fn main() -> i32 {')
    [void]$source.AppendLine('    let value = Reflected { value: 7 };')
    [void]$source.AppendLine('    if (value.get() != 7) { return 1; }')
    [void]$source.AppendLine('    let reflected = getType("Reflected");')
    [void]$source.AppendLine('    if (!reflected.valid) { return 2; }')
    [void]$source.AppendLine('    return 0;')
    [void]$source.AppendLine('}')
    New-VyxProject -Path $partitionProject -Name "reflection_partition_smoke" -Source $source.ToString()

    Push-Location $partitionProject
    try {
        $buildOutput = & $compiler build 2>&1
        $buildExit = $LASTEXITCODE
    } finally {
        Pop-Location
    }
    if ($buildExit -ne 0) {
        Write-Host ($buildOutput -join [Environment]::NewLine)
        throw "[reflection-partition] partitioned build failed (exit=$buildExit)"
    }

    $exe = Join-Path $partitionProject "target\reflection_partition_smoke.exe"
    if (-not (Test-Path -LiteralPath $exe)) {
        throw "[reflection-partition] partitioned executable missing: $exe"
    }
    & $exe
    $runExit = $LASTEXITCODE
    if ($runExit -ne 0) {
        throw "[reflection-partition] partitioned executable failed (exit=$runExit)"
    }

    Write-Host "[reflection-partition] PASS - direct and forced multi-root partitions"
} finally {
    if (Test-Path -LiteralPath $testRoot) {
        $resolvedTestRoot = (Resolve-Path -LiteralPath $testRoot).Path
        if ($resolvedTestRoot.StartsWith($outRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
            Remove-Item -LiteralPath $resolvedTestRoot -Recurse -Force
        }
    }
}
