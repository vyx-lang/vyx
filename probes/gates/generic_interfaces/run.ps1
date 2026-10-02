param([string]$Compiler = '')
$ErrorActionPreference = 'Stop'
function Write-Utf8NoBom([string]$Path, [string]$Text) { [IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false)) }
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
if (-not $Compiler) { $Compiler = Join-Path $repo 'bootstrap_compiler/out/boot.exe' }
$Compiler = (Resolve-Path $Compiler).Path
$run = Join-Path $PSScriptRoot ('.runs/' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $run, (Join-Path $run 'interfaces'), (Join-Path $run 'consumer') -Force | Out-Null
$source = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'producer.vyx'))
$consumer = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'consumer.vyx'))
[IO.File]::WriteAllText((Join-Path $run 'direct.vyx'), $source + $consumer.Replace('use GenericContract;', ''))
Copy-Item (Join-Path $PSScriptRoot 'producer.vyx') (Join-Path $run 'producer.vyx')
Copy-Item (Join-Path $PSScriptRoot 'consumer.vyx') (Join-Path $run 'consumer/main.vyx')
Copy-Item (Join-Path $PSScriptRoot 'private_access.vyx') (Join-Path $run 'private_access.vyx')
$env:LLVM_ROOT = Join-Path $repo 'clang'
$env:PATH = (Split-Path $Compiler) + ';' + $env:PATH
Push-Location $run
try {
    & $Compiler --src=file direct.vyx --emit=exe -o direct.exe *> direct.log
    if ($LASTEXITCODE -ne 0) { throw 'Direct-source control compilation failed; inspect direct.log' }
    & ./direct.exe
    if ($LASTEXITCODE -ne 0) { throw 'Direct-source control execution failed' }
    & $Compiler --src=file producer.vyx --emit=vyi --vyi-shallow -o interfaces/GenericContract.vyi *> emit.log
    if ($LASTEXITCODE -ne 0) { throw 'Interface emission failed' }
    Push-Location consumer
    try {
        & $Compiler --src=file main.vyx --interface-root ../interfaces --emit=exe -o consumer.exe *> consumer.log
        $actual = $LASTEXITCODE
        if ($actual -eq 0) { & ./consumer.exe; if ($LASTEXITCODE -ne 0) { throw 'Interface consumer returned incorrect result' } }
    } finally { Pop-Location }
    $privateText=& $Compiler --src=file (Join-Path $run 'private_access.vyx') --interface-root (Join-Path $run 'interfaces') --emit=exe -o (Join-Path $run 'private.exe') 2>&1 | Out-String
    $privateExit=$LASTEXITCODE
    Write-Utf8NoBom (Join-Path $run 'private_access.log') $privateText
    if ($privateExit -eq 0 -or $privateText -notmatch 'E3000') { throw 'Private generic helper access was accepted' }
    $artifactPath=Join-Path $run 'interfaces/GenericContract.vyi'
    $artifact=[IO.File]::ReadAllText($artifactPath)
    if ($artifact -notmatch 'VYX_TEMPLATE_ARTIFACT_BEGIN v=1' -or $artifact -notmatch 'payload=[0-9A-F]{64}') { throw 'Versioned generic payload was not emitted' }
    $negativeCases=[ordered]@{
        surface=$artifact.Replace('public class GenericContract.Cell', 'public class GenericContract.Xell')
        payload=([regex]::Replace($artifact,'(VYX_TEMPLATE_ARTIFACT_PAYLOAD )([0-9A-F])',{param($m) $m.Groups[1].Value + $(if($m.Groups[2].Value -eq '0'){'1'}else{'0'})},1))
        version=$artifact.Replace('VYX_TEMPLATE_ARTIFACT_BEGIN v=1 ', 'VYX_TEMPLATE_ARTIFACT_BEGIN v=2 ')
        module=([regex]::Replace($artifact,' module=[0-9A-F]+ source=', ' module=00 source=',1))
        truncated=$artifact.Replace('// VYX_TEMPLATE_ARTIFACT_END','')
        duplicate=($artifact + '// VYX_TEMPLATE_ARTIFACT_END' + "`n")
    }
    $rejections=[ordered]@{}
    foreach($case in $negativeCases.Keys) {
        if ($negativeCases[$case] -eq $artifact) { throw "Mutation failed: $case" }
        Write-Utf8NoBom $artifactPath $negativeCases[$case]
        Remove-Item (Join-Path $run 'consumer/.cache') -Recurse -Force -ErrorAction SilentlyContinue
        $text=& $Compiler --src=file (Join-Path $run 'consumer/main.vyx') --interface-root (Join-Path $run 'interfaces') --emit=exe -o (Join-Path $run "$case.exe") 2>&1 | Out-String
        $rejections[$case]=$LASTEXITCODE
        Write-Utf8NoBom (Join-Path $run "$case.log") $text
        if ($LASTEXITCODE -eq 0) { throw "Invalid artifact accepted: $case" }
    }
    Write-Utf8NoBom $artifactPath $artifact
    [ordered]@{ compiler_sha256=(Get-FileHash $Compiler).Hash; direct_exit=0; interface_consumer_exit=$actual; private_access_exit=$privateExit; artifact_rejections=$rejections; run=$run } |
        ConvertTo-Json | Set-Content result.json
    if ($actual -ne 0) { Get-Content consumer/consumer.log -Tail 25; Write-Host "UNRESOLVED: public generic interface lacks callable implementation ($run)"; exit 1 }
    Write-Host "generic interface: OK ($run)"
} finally { Pop-Location }
