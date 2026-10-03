param([string]$Compiler = '')

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
if (-not $Compiler) { $Compiler = Join-Path $repo 'bootstrap_compiler/out/boot.exe' }
$Compiler = (Resolve-Path $Compiler).Path
$run = Join-Path $PSScriptRoot ('.runs/long-chains-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
$interfaces = Join-Path $run 'interfaces'
$consumerDir = Join-Path $run 'consumer'
New-Item -ItemType Directory -Force -Path $interfaces, $consumerDir | Out-Null

function Write-Utf8NoBom([string]$Path, [string]$Text) {
    [IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false))
}
function Invoke-Compiler([string]$Name, [string[]]$CompilerArgs) {
    $log = Join-Path $run ($Name + '.log')
    Push-Location $run
    try { & $Compiler @CompilerArgs *> $log; $code = $LASTEXITCODE }
    finally { Pop-Location }
    if ($code -ne 0) { throw "$Name failed (exit=$code); inspect $log" }
}

# A flat statement chain is deliberately longer than TA_MAX_GRAPH_DEPTH.  It
# must remain valid because `next` is a list edge in the v1 graph, not a
# structural nesting edge.  The closure capture also exercises a nested stmt
# chain through the existing capture_tag=2 encoding.
$producerLines = [Collections.Generic.List[string]]::new()
$producerLines.Add('module ChainProbe;')
$producerLines.Add('public fn anchor<T>(value: T) -> T {')
$producerLines.Add('    let base: i64 = 5;')
$producerLines.Add('    let offset: i64 = 7;')
$producerLines.Add('    let add = [base, offset](x: i64) => x + base + offset;')
$producerLines.Add('    let warmup: i64 = add(1);')
for ($i = 0; $i -lt 620; $i++) { $producerLines.Add("    let s${i}: i32 = $i;") }
$producerLines.Add('    return value;')
$producerLines.Add('}')
$producer = Join-Path $run 'producer.vyx'
Write-Utf8NoBom $producer ($producerLines -join "`n")

$artifact = Join-Path $interfaces 'ChainProbe.vyi'
Invoke-Compiler 'producer' @('--src=file', $producer, '--emit=vyi', '-o', $artifact)
$artifactText = [IO.File]::ReadAllText($artifact)
if ($artifactText -notmatch 'VYX_TEMPLATE_ARTIFACT_BEGIN v=1') {
    throw 'flat >512 statement producer emitted no template artifact'
}
$nodeHeader = [regex]::Match($artifactText, ' nodes=([0-9]+)')
if (-not $nodeHeader.Success -or [int64]$nodeHeader.Groups[1].Value -le 512) {
    throw 'long-chain artifact did not record more than 512 graph nodes'
}

$consumer = Join-Path $consumerDir 'main.vyx'
Write-Utf8NoBom $consumer @'
use ChainProbe;
fn main() -> i32 {
    let got: i32 = ChainProbe::anchor::<i32>(42);
    if (got != 42) { return 1; }
    return 0;
}
'@
Invoke-Compiler 'consumer' @('--src=file', $consumer, '--interface-root', $interfaces, '--emit=exe', '-o', (Join-Path $consumerDir 'consumer.exe'))
& (Join-Path $consumerDir 'consumer.exe') *> (Join-Path $run 'consumer-run.log')
if ($LASTEXITCODE -ne 0) { throw "long-chain consumer returned $LASTEXITCODE" }

# Marker-only metadata (`|kind:class`) and an extern C declaration must not
# trigger an artifact.  This guards against serializing a non-generic
# renderer surface merely because the parser attached declaration markers.
$plain = Join-Path $run 'plain.vyx'
Write-Utf8NoBom $plain @'
module PlainSurface;
extern "C" { fn plain_add(x: i32) -> i32; }
public class PlainRecord { public value: i32; }
'@
$plainArtifact = Join-Path $interfaces 'PlainSurface.vyi'
Invoke-Compiler 'plain' @('--src=file', $plain, '--emit=vyi', '-o', $plainArtifact)
$plainText = [IO.File]::ReadAllText($plainArtifact)
if ($plainText -match 'VYX_TEMPLATE_ARTIFACT_BEGIN') {
    throw 'non-generic extern/record unexpectedly emitted a template artifact'
}

[ordered]@{
    compiler_sha256 = (Get-FileHash $Compiler).Hash
    producer_artifact = $artifact
    statement_count = 626
    artifact_nodes = [int64]$nodeHeader.Groups[1].Value
    consumer_exit = 0
    non_generic_artifact = $false
    run = $run
} | ConvertTo-Json | Set-Content (Join-Path $run 'result.json')
Write-Host "generic interface long chains: OK ($run)"
exit 0
