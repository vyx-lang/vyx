param([Parameter(Mandatory)][string]$Compiler)
$ErrorActionPreference='Stop'
$Compiler=(Resolve-Path $Compiler).Path
$run=Join-Path $PSScriptRoot ('.runs/'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $run,(Join-Path $run 'interfaces') -Force|Out-Null
$source=[Text.StringBuilder]::new()
[void]$source.AppendLine('module Demand;')
[void]$source.AppendLine('public fn used(value: i64) -> i64 { return value + 1; }')
[void]$source.AppendLine('public fn identity<T>(value: T) -> T { return value; }')
[void]$source.AppendLine('public class Keys { public static fn head() -> i64 { return 17; } }')
[void]$source.AppendLine('public class Library { public value: i64;')
[void]$source.AppendLine('public fn used(&self) -> i64 { return self.middle(); }')
[void]$source.AppendLine('private fn middle(&self) -> i64 { return self.inner(); }')
[void]$source.AppendLine('private fn inner(&self) -> i64 { return self.value + Keys.head() - 17; }')
for($i=0;$i -lt 128;$i++) {
 [void]$source.AppendLine(('public fn unused_method_'+$i+'(&self) -> i64 { return self.value + '+$i+'; }'))
}
[void]$source.AppendLine('}')
for($i=0;$i -lt 128;$i++) {
 [void]$source.AppendLine(('public fn unused_function_'+$i+'(value: i64) -> i64 { return value + '+$i+'; }'))
}
[IO.File]::WriteAllText((Join-Path $run 'producer.vyx'),$source.ToString())
[IO.File]::WriteAllText((Join-Path $run 'consumer.vyx'),@'
use Demand;
fn main() -> i32 {
    let library = Library { value: 17 };
    if (used(40 as i64) != 41 || library.used() != 17 || identity::<i32>(23) != 23) { return 1; }
    print("interface-fact-demand: OK");
    return 0;
}
'@)
[IO.File]::WriteAllText((Join-Path $run 'negative.vyx'),'use Demand; fn main() -> i32 { used(true); return 0; }')
$savedPath=$env:PATH
$savedProfile=$env:VYX_BOOTSTRAP_PROFILE
Push-Location $run
try {
 $env:PATH=(Split-Path $Compiler)+';'+$env:PATH
 $env:VYX_BOOTSTRAP_PROFILE='0'
 # Check the producer bodies before producing the transported interface.
 & $Compiler --src=file producer.vyx --emit=obj --export-top-level-roots --export-method-roots -o producer.obj *> producer.log
 if($LASTEXITCODE -ne 0){throw 'Producer body validation failed'}
 & $Compiler --src=file producer.vyx --emit=vyi --vyi-shallow -o interfaces/Demand.vyi *> interface.log
 if($LASTEXITCODE -ne 0){throw 'Interface emission failed'}
 $env:VYX_BOOTSTRAP_PROFILE='2'
 # Use normal SDK library discovery. --interface-root would independently
 # suppress the eager pass and conceal this regression.
 & $Compiler --src=file consumer.vyx --emit=exe -L interfaces -o consumer.exe *> consumer.log
 if($LASTEXITCODE -ne 0){throw 'Interface consumer failed'}
 if((Get-Content consumer.log -Raw) -match '\[res-in\][^\r\n]*unused_(method|function)_') {
   throw 'Consumer eagerly resolved an unused interface body'
 }
 & ./consumer.exe *> run.log
 if($LASTEXITCODE -ne 0){throw 'Interface consumer returned the wrong result'}
 & $Compiler --src=file negative.vyx --emit=obj -L interfaces -o negative.obj *> negative.log
 if($LASTEXITCODE -eq 0 -or (Get-Content negative.log -Raw) -notmatch 'error:') {
   throw 'Invalid call through the interface was accepted'
 }
 Write-Host "PASS: reached functions/methods/generic bodies, unused interface facts skipped, invalid call rejected; $run"
} finally {
 Pop-Location
 $env:PATH=$savedPath
 $env:VYX_BOOTSTRAP_PROFILE=$savedProfile
}
