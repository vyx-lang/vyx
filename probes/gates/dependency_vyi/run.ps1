param([string]$Compiler = '')
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
if (!$Compiler) { $Compiler = Join-Path $repo 'bootstrap_compiler/out/boot.exe' }
$Compiler = (Resolve-Path -LiteralPath $Compiler).Path
$env:LLVM_ROOT = Join-Path $repo 'clang'
$run = Join-Path $PSScriptRoot ('.runs/' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
$library = Join-Path $run 'library'
$private = Join-Path $library '.vyx_interfaces/package/.files'
New-Item -ItemType Directory -Force -Path $private | Out-Null

@'
module Library;
public class Library.Widget {
    public static fn answer() -> i32;
}
'@ | Set-Content (Join-Path $library 'Library.vyi')
@'
module Library;
public class Library.Unrelated {
    public static fn other() -> i32;
}
'@ | Set-Content (Join-Path $private 'first.vyi')
@'
use Widget = Library.Widget;
fn main() -> i32 { return Widget.answer(); }
'@ | Set-Content (Join-Path $run 'consumer.vyx')

& $Compiler --src=file (Join-Path $run 'consumer.vyx') --emit=obj `
    -o (Join-Path $run 'consumer.obj') -L $library *> (Join-Path $run 'compile.log')
if ($LASTEXITCODE -ne 0) {
    Get-Content (Join-Path $run 'compile.log')
    throw 'Published interface was shadowed by a private per-file interface'
}
if (!(Test-Path (Join-Path $run 'consumer.obj'))) { throw 'Compiler did not produce an object' }
Write-Output "dependency_vyi: OK ($run)"
