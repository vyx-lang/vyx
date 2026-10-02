$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$compiler = Join-Path $root "out\boot.exe"
$source = Join-Path $root "tests\receiver_modes_smoke.vyx"
if (-not (Test-Path -LiteralPath $compiler)) {
    throw "[receiver-modes] compiler missing: $compiler"
}
if (-not (Test-Path -LiteralPath $source)) {
    throw "[receiver-modes] source missing: $source"
}

$outRoot = (Resolve-Path -LiteralPath (Join-Path $root "out")).Path
$testRoot = Join-Path $outRoot "receiver_modes_smoke"

function Remove-TestRoot {
    if (-not (Test-Path -LiteralPath $testRoot)) { return }
    $resolved = (Resolve-Path -LiteralPath $testRoot).Path
    if (-not $resolved.StartsWith($outRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "[receiver-modes] refusing cleanup outside compiler out: $resolved"
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}

function Assert-Rejected {
    param(
        [Parameter(Mandatory=$true)][string]$Name,
        [Parameter(Mandatory=$true)][string]$SourceText,
        [Parameter(Mandatory=$true)][string]$Expected
    )

    $path = Join-Path $testRoot ($Name + ".vyx")
    Set-Content -LiteralPath $path -Encoding utf8 -Value $SourceText
    $output = & $compiler --src=file $path --emit=ir 2>&1
    $exitCode = $LASTEXITCODE
    $text = $output -join "`n"
    if ($exitCode -eq 0 -or $text -notmatch $Expected) {
        Write-Host $text
        throw "[receiver-modes] expected rejection '$Name' was not emitted"
    }
}

Remove-TestRoot
New-Item -ItemType Directory -Force -Path $testRoot | Out-Null

try {
    & $compiler --src=file $source --run=aot
    if ($LASTEXITCODE -ne 0) {
        throw "[receiver-modes] LLVM AOT smoke failed (exit=$LASTEXITCODE)"
    }

    Assert-Rejected -Name "shared_write" -Expected "E3000: cannot write through an immutable borrowed" -SourceText @'
class R {
    public value: i32;
    public fn shared(&self) -> i32 { self.value = 2; return self.value; }
}
fn main() -> i32 { let r = R { value: 1 }; return r.shared(); }
'@
    Assert-Rejected -Name "shared_calls_mut" -Expected "E3000: cannot call a mutable or owning method through '&self'" -SourceText @'
class R {
    public value: i32;
    public fn mutate(&mut self) -> i32 { self.value = 2; return self.value; }
    public fn bad(&self) -> i32 { return self.mutate(); }
}
fn main() -> i32 { let r = R { value: 1 }; return r.bad(); }
'@
    Assert-Rejected -Name "mut_calls_own" -Expected "E3000: cannot move the receiver out of a borrowed 'self'" -SourceText @'
class R {
    public value: i32;
    public fn consume(own self) -> i32 { return self.value; }
    public fn bad(&mut self) -> i32 { return self.consume(); }
}
fn main() -> i32 { var r = R { value: 1 }; return r.bad(); }
'@
    Assert-Rejected -Name "immutable_calls_mut" -Expected "E3000: cannot mutably borrow immutable receiver 'r'" -SourceText @'
class R {
    public value: i32;
    public fn mutate(&mut self) -> i32 { self.value = 2; return self.value; }
}
fn main() -> i32 { let r = R { value: 1 }; return r.mutate(); }
'@
    Assert-Rejected -Name "use_after_own" -Expected "E3100: use of moved value 'r'" -SourceText @'
class R {
    public value: i32;
    public fn consume(own self) -> i32 { return self.value; }
    public fn read(&self) -> i32 { return self.value; }
}
fn main() -> i32 { var r = R { value: 1 }; let x = r.consume(); return x + r.read(); }
'@

    $reinit = Join-Path $testRoot "reinit_after_own.vyx"
    Set-Content -LiteralPath $reinit -Encoding utf8 -Value @'
class R {
    public value: i32;
    public fn consume(own self) -> i32 { return self.value; }
    public fn read(&self) -> i32 { return self.value; }
}
fn main() -> i32 {
    var r = R { value: 1 };
    if (r.consume() != 1) { return 1; }
    r = R { value: 2 };
    if (r.read() != 2) { return 2; }
    return 0;
}
'@
    & $compiler --src=file $reinit --run=aot
    if ($LASTEXITCODE -ne 0) {
        throw "[receiver-modes] reinitialization after own-self failed (exit=$LASTEXITCODE)"
    }

    $cppOut = Join-Path $testRoot "cpp"
    & $compiler --src=file $source --emit=cpp -o $cppOut
    if ($LASTEXITCODE -ne 0) {
        throw "[receiver-modes] MIR2CPP generation failed (exit=$LASTEXITCODE)"
    }
    Push-Location $cppOut
    try {
        cmake --preset ninja-release
        if ($LASTEXITCODE -ne 0) {
            throw "[receiver-modes] MIR2CPP CMake configure failed (exit=$LASTEXITCODE)"
        }
        cmake --build --preset ninja-release --parallel
        if ($LASTEXITCODE -ne 0) {
            throw "[receiver-modes] MIR2CPP build failed (exit=$LASTEXITCODE)"
        }
        $exe = Get-ChildItem -LiteralPath "build\ninja-release" -Filter *.exe | Select-Object -First 1
        if ($null -eq $exe) {
            throw "[receiver-modes] MIR2CPP executable missing"
        }
        & $exe.FullName
        if ($LASTEXITCODE -ne 0) {
            throw "[receiver-modes] MIR2CPP executable failed (exit=$LASTEXITCODE)"
        }
    } finally {
        Pop-Location
    }

    Write-Host "[receiver-modes] PASS - compat/own/&mut/&self on LLVM and MIR2CPP"
} finally {
    Remove-TestRoot
}
