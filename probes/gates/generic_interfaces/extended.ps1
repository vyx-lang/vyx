param([string]$Compiler = '')

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
if (-not $Compiler) { $Compiler = Join-Path $repo 'bootstrap_compiler/out/boot.exe' }
$Compiler = (Resolve-Path $Compiler).Path
$clang = Join-Path $repo 'clang/bin/clang++.exe'
$runtime = Join-Path $repo 'bootstrap_compiler/out'
$fixture = Join-Path $PSScriptRoot 'extended'
$run = Join-Path $PSScriptRoot ('.runs/extended-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Force -Path $run, (Join-Path $run 'interfaces'), (Join-Path $run 'consumers') | Out-Null

# The standard library's authoritative `std.collections` interface is built
# with the compiler and intentionally has no template payload (stdlib bodies
# are compiled as a unit).  Place that real interface beside the producer
# artifacts so imported Dict<K,V> calls exercise the same public API used by
# ordinary projects.
$stdCollectionsInterface = Join-Path $repo 'bootstrap_compiler/out/.vyx_interfaces/boot/std/collections.vyi'
if (Test-Path $stdCollectionsInterface) {
    New-Item -ItemType Directory -Force -Path (Join-Path $run 'interfaces/std') | Out-Null
    Copy-Item $stdCollectionsInterface (Join-Path $run 'interfaces/std/collections.vyi')
}

function Write-Utf8NoBom([string]$Path, [string]$Text) {
    [IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false))
}

$env:LLVM_ROOT = Join-Path $repo 'clang'
$env:PATH = (Join-Path $repo 'clang/bin') + ';' + $env:PATH
$results = [Collections.Generic.List[object]]::new()

function Invoke-Logged([string]$Name, [string]$WorkDir, [string]$Exe, [string[]]$CommandArgs) {
    $log = Join-Path $run ($Name + '.log')
    Push-Location $WorkDir
    try {
        & $Exe @CommandArgs *> $log
        $code = $LASTEXITCODE
    } finally { Pop-Location }
    $results.Add([pscustomobject]@{ name = $Name; exit = $code; log = $log })
    return $code
}

function Require-Compiler([string]$Name, [string]$Source, [string]$Artifact) {
    $code = Invoke-Logged $Name $run $Compiler @('--src=file', $Source, '--emit=vyi', '-o', $Artifact)
    if ($code -ne 0) { throw "$Name failed; inspect $((Join-Path $run ($Name + '.log')))" }
    $text = [IO.File]::ReadAllText($Artifact)
    if ($text -notmatch 'VYX_TEMPLATE_ARTIFACT_BEGIN v=1' -or $text -notmatch 'payload=[0-9A-F]{64}') {
        throw "$Name emitted no versioned template payload; inspect $Artifact"
    }
}

function Run-InterfaceCase([string]$Name, [string]$ProducerSource, [string]$ConsumerSource, [string]$ArtifactName, [string]$UnitSources = '') {
    $artifact = Join-Path $run ('interfaces/' + $ArtifactName + '.vyi')
    Require-Compiler ($Name + '-producer') $ProducerSource $artifact
    $consumerDir = Join-Path $run ('consumers/' + $Name)
    New-Item -ItemType Directory -Force -Path $consumerDir | Out-Null
    $consumer = Join-Path $consumerDir 'main.vyx'
    Copy-Item $ConsumerSource $consumer
    # Deliberately keep only the emitted interface in the import root.  The
    # consumer process has no producer source path available to fall back to.
    $consumerArgs = [Collections.Generic.List[string]]::new()
    $consumerArgs.Add('--src=file'); $consumerArgs.Add($consumer)
    $consumerArgs.Add('--interface-root'); $consumerArgs.Add((Join-Path $run 'interfaces'))
    if ($UnitSources) { $consumerArgs.Add('--interface-unit-sources'); $consumerArgs.Add($UnitSources) }
    $consumerArgs.Add('--emit=exe'); $consumerArgs.Add('-o'); $consumerArgs.Add((Join-Path $consumerDir 'consumer.exe'))
    $code = Invoke-Logged ($Name + '-consumer') $consumerDir $Compiler $consumerArgs.ToArray()
    if ($code -eq 0) {
        & (Join-Path $consumerDir 'consumer.exe') *> (Join-Path $run ($Name + '-run.log'))
        $runCode = $LASTEXITCODE
        $results.Add([pscustomobject]@{ name = $Name + '-run'; exit = $runCode; log = (Join-Path $run ($Name + '-run.log')) })
        if ($runCode -ne 0) { throw "$Name consumer returned $runCode; inspect $((Join-Path $run ($Name + '-run.log')))" }
    } else {
        throw "$Name interface consumer failed; inspect $((Join-Path $run ($Name + '-consumer.log'))) for the exact compiler diagnostic"
    }
}

$failures = [Collections.Generic.List[string]]::new()
function Capture-Case([string]$Name, [scriptblock]$Body) {
    try { & $Body; Write-Host "PASS $Name" }
    catch { $message = "${Name}: $($_.Exception.Message)"; $failures.Add($message); Write-Host "FAIL $message" }
}

Capture-Case 'free-generic' {
    Run-InterfaceCase 'free-generic' (Join-Path $fixture 'ProducerFree.vyx') (Join-Path $fixture 'FreeConsumer.vyx') 'ProducerFree'
}

Capture-Case 'same-short-name' {
    # Both modules deliberately export the same short function name.  Emit
    # both artifacts before compiling the consumer so import lookup must use
    # their fully qualified module identity.
    Require-Compiler 'same-short-name-alpha-producer' (Join-Path $fixture 'ModuleAlpha.vyx') (Join-Path $run 'interfaces/Alpha.vyi')
    Require-Compiler 'same-short-name-beta-producer' (Join-Path $fixture 'ModuleBeta.vyx') (Join-Path $run 'interfaces/Beta.vyi')
    $namesDir = Join-Path $run 'consumers/same-short-name'; New-Item -ItemType Directory -Force $namesDir | Out-Null
    $names = Join-Path $namesDir 'main.vyx'; Copy-Item (Join-Path $fixture 'NamesConsumer.vyx') $names
    $namesCode = Invoke-Logged 'same-short-name-consumer' $namesDir $Compiler @('--src=file', $names, '--interface-root', (Join-Path $run 'interfaces'), '--emit=exe', '-o', (Join-Path $namesDir 'consumer.exe'))
    if ($namesCode -ne 0) { throw "same-short-name consumer with Alpha and Beta failed; inspect $((Join-Path $run 'same-short-name-consumer.log'))" }
    & (Join-Path $namesDir 'consumer.exe') *> (Join-Path $run 'same-short-name-run.log')
    if ($LASTEXITCODE -ne 0) { throw "same-short-name consumer with Alpha and Beta returned $LASTEXITCODE" }
}

Capture-Case 'std-dict-string-i64' {
    $stdUnitSources = Join-Path $run 'std.unit_sources'
    (Resolve-Path (Join-Path $repo 'bootstrap_compiler/std/dict.vyx')).Path | Set-Content $stdUnitSources
    Run-InterfaceCase 'std-dict-string-i64' (Join-Path $fixture 'DictProducer.vyx') (Join-Path $fixture 'DictConsumer.vyx') 'DictProducer' $stdUnitSources
}

Capture-Case 'duplicate-consumer-link' {
    # Compile two independent consumers to objects.  Both instantiate the
    # same imported generic specialization; linking them together catches
    # duplicate strong definitions and missing COMDAT/weak ownership.
    $aDir = Join-Path $run 'consumers/duplicate-a'; $bDir = Join-Path $run 'consumers/duplicate-b'
    New-Item -ItemType Directory -Force -Path $aDir, $bDir | Out-Null
    $aSrc = Join-Path $aDir 'main.vyx'; $bSrc = Join-Path $bDir 'main.vyx'
    Copy-Item (Join-Path $fixture 'DuplicateA.vyx') $aSrc
    Copy-Item (Join-Path $fixture 'DuplicateB.vyx') $bSrc
    $aObj = Join-Path $aDir 'a.obj'; $bObj = Join-Path $bDir 'b.obj'
    if ((Invoke-Logged 'duplicate-a-object' $aDir $Compiler @('--src=file', $aSrc, '--interface-root', (Join-Path $run 'interfaces'), '--emit=obj', '-o', $aObj)) -ne 0) { throw "duplicate-a object compile failed; inspect $((Join-Path $run 'duplicate-a-object.log'))" }
    if ((Invoke-Logged 'duplicate-b-object' $bDir $Compiler @('--src=file', $bSrc, '--interface-root', (Join-Path $run 'interfaces'), '--emit=obj', '-o', $bObj)) -ne 0) { throw "duplicate-b object compile failed; inspect $((Join-Path $run 'duplicate-b-object.log'))" }
    $linkLog = Join-Path $run 'duplicate-link.log'
    & $clang $aObj $bObj (Join-Path $fixture 'link_main.cpp') "-L$runtime" -lvyx_runtime -ltbb12 -lsynchronization -lws2_32 -fms-runtime-lib=static -fuse-ld=lld -o (Join-Path $run 'duplicate-link.exe') *> $linkLog
    $linkCode = $LASTEXITCODE
    $results.Add([pscustomobject]@{ name = 'duplicate-link'; exit = $linkCode; log = $linkLog })
    if ($linkCode -ne 0) { throw "duplicate consumer link failed; inspect $linkLog" }
    & (Join-Path $run 'duplicate-link.exe') *> (Join-Path $run 'duplicate-run.log')
    if ($LASTEXITCODE -ne 0) { throw "duplicate consumer executable returned $LASTEXITCODE; inspect $((Join-Path $run 'duplicate-run.log'))" }
}

[ordered]@{ compiler_sha256 = (Get-FileHash $Compiler).Hash; results = $results; failures = $failures; run = $run } | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $run 'result.json')
if ($failures.Count -gt 0) {
    Write-Error ("generic interface extended: FAILED ($($failures.Count) case(s)); logs=$run")
    exit 1
}
Write-Host "generic interface extended: OK ($run)"
exit 0
