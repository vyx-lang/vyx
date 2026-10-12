param([Parameter(Mandatory)][string]$Compiler, [string]$BaselineCompiler = '')
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$Compiler = (Resolve-Path $Compiler).Path
if ($BaselineCompiler) { $BaselineCompiler = (Resolve-Path $BaselineCompiler).Path }
$run = Join-Path $PSScriptRoot ('.runs/' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $run -Force | Out-Null
$savedPath = $env:PATH
$savedLlvm = $env:LLVM_ROOT
if (-not ('ArtifactGateMemory' -as [type])) {
    Add-Type @'
using System;
using System.Runtime.InteropServices;
public class ArtifactGateMemory {
 [StructLayout(LayoutKind.Sequential)] public struct Status {
  public uint length, load;
  public ulong totalPhysical, availablePhysical, totalCommit, availableCommit;
  public ulong totalVirtual, availableVirtual, availableExtendedVirtual;
 }
 [DllImport("kernel32.dll")] static extern bool GlobalMemoryStatusEx(ref Status status);
 public static Status Read() {
  var status = new Status(); status.length = (uint)Marshal.SizeOf(status);
  if (!GlobalMemoryStatusEx(ref status)) throw new Exception("Memory telemetry unavailable");
  return status;
 }
}
'@
}
$results = [Collections.Generic.List[object]]::new()
function Invoke-Checked([string]$Name, [string]$Tool, [string]$Arguments, [long]$MaxPrivate = 2147483648) {
    $stdout = Join-Path $run ($Name + '.log')
    $stderr = Join-Path $run ($Name + '.err.log')
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $launch = @{FilePath=$Tool; WorkingDirectory=$run; WindowStyle='Hidden';
        RedirectStandardOutput=$stdout; RedirectStandardError=$stderr; PassThru=$true}
    if ($Arguments) { $launch.ArgumentList = $Arguments }
    $proc = Start-Process @launch
    $peak = 0L
    $reason = ''
    while (-not $proc.HasExited) {
        $proc.Refresh()
        $memory = [ArtifactGateMemory]::Read()
        $peak = [Math]::Max($peak, $proc.PrivateMemorySize64)
        if ($proc.PrivateMemorySize64 -gt $MaxPrivate -or $memory.availablePhysical -lt 2147483648 `
            -or $memory.availableCommit -lt 2147483648) {
            $reason = 'memory guard'
            & taskkill.exe /PID $proc.Id /T /F | Out-Null
            break
        }
        Start-Sleep -Milliseconds 50
    }
    $proc.WaitForExit(); $proc.Refresh(); $watch.Stop()
    $results.Add([ordered]@{name=$Name; exit=$proc.ExitCode; seconds=$watch.Elapsed.TotalSeconds; peak_private=$peak; stopped_reason=$reason})
    if ($proc.ExitCode -ne 0 -or $reason) { throw "$Name failed; inspect $stdout and $stderr" }
}
function Read-Artifact([string]$Path) {
    $text = [IO.File]::ReadAllText($Path)
    $header = [regex]::Match($text, '(?m)^// VYX_TEMPLATE_ARTIFACT_BEGIN (.+)$')
    $payload = [regex]::Match($text, '(?m)^// VYX_TEMPLATE_ARTIFACT_PAYLOAD ([0-9A-F]+)\r?$')
    if (-not $header.Success -or -not $payload.Success) { throw 'Missing checked AST payload' }
    $bytes = [Convert]::FromHexString($payload.Groups[1].Value)
    $hash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes))
    if ($header.Value -notmatch (' payload=' + $hash + '(?: |$)')) { throw 'Payload digest mismatch' }
    $surface = $text.Substring(0, $header.Index)
    $surfaceHash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($surface)))
    if ($header.Value -notmatch (' surface=' + $surfaceHash + '(?: |$)')) { throw 'Surface digest mismatch' }
    $nodes = [regex]::Match($header.Value, ' nodes=(\d+)').Groups[1].Value
    return [ordered]@{surface=$surface; payload=$payload.Groups[1].Value; nodes=$nodes; bytes=$bytes.Length}
}
try {
    $env:LLVM_ROOT = Join-Path $repo 'clang'
    $env:PATH = (Split-Path $Compiler) + ';' + (Join-Path $env:LLVM_ROOT 'bin') + ';' + $env:PATH
    $source = Join-Path $run 'producer.vyx'
    $interface = Join-Path $run 'ArtifactScaling.vyi'
    [IO.File]::WriteAllText($source, 'module ArtifactScaling; public fn identity<T>(x: T) -> T { return x; }' + "`n")
    $emitArgs = '--src=file "' + $source + '" --emit=vyi --vyi-shallow -o "' + $interface + '"'
    Invoke-Checked 'small' $Compiler $emitArgs
    $small = Read-Artifact $interface
    if ($BaselineCompiler) {
        Invoke-Checked 'small-baseline' $BaselineCompiler $emitArgs
        $previous = Read-Artifact $interface
        if ($small.surface -ne $previous.surface -or $small.payload -ne $previous.payload -or $small.nodes -ne $previous.nodes) {
            throw 'v1 encoding changed for the compatibility fixture'
        }
    }
    foreach ($size in @(4096, 65536, 131072)) {
        $literal = '0123456789ABCDEF' * ($size / 16)
        $text = 'module ArtifactScaling;' + "`n" + 'public fn identity<T>(x: T) -> T { return x; }' + "`n"
        $text += 'public fn payload() { return "' + $literal + '"; }' + "`n"
        $text += 'public fn steps() -> i32 { var n: i32 = 0;' + ("n = n + 1;`n" * 2048) + 'return n; }' + "`n"
        for ($i = 0; $i -lt 2048; $i++) { $text += "public fn marker$i() -> i32 { return $i; }`n" }
        [IO.File]::WriteAllText($source, $text)
        Invoke-Checked ('emit-' + $size) $Compiler $emitArgs
        $artifact = Read-Artifact $interface
        if ($artifact.bytes -lt $size * 2) { throw 'Large literal was omitted from the checked graph' }
    }
    $consumer = @'
use ArtifactScaling;
use std.string;
fn main() -> i32 {
    let text = payload();
    if (text.len != 131072 || text.charAt(0) != 48 || text.charAt(131071) != 70) { return 1; }
    if (identity::<i32>(7) != 7 || steps() != 2048 || marker2047() != 2047) { return 2; }
    print("template artifact scaling PASS");
    return 0;
}
'@
    $consumerPath = Join-Path $run 'consumer.vyx'
    $exe = Join-Path $run 'consumer.exe'
    [IO.File]::WriteAllText($consumerPath, $consumer)
    Invoke-Checked 'consume' $Compiler ('--src=file "' + $consumerPath + '" --interface-root "' + $run + '" --emit=exe -o "' + $exe + '"') 536870912
    Invoke-Checked 'run-consumer' $exe ''
    $results | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $run 'result.json')
    Write-Host "PASS: artifact hashes, large payloads, chains and AOT consumer; $run"
} finally {
    $env:PATH = $savedPath; $env:LLVM_ROOT = $savedLlvm
    $results | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $run 'result.json')
}
