[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Compiler,
    [string]$OutputDir = ''
)

$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$compilerPath = (Resolve-Path -LiteralPath $Compiler).Path
$compilerLeaf = [IO.Path]::GetFileName($compilerPath)
$compilerNorm = $compilerPath.Replace('\', '/').ToLowerInvariant()
if ($compilerLeaf -match '(?i)^(boot|seed)(\.|$)' -or
    $compilerNorm -match '/bootstrap_compiler/(seed[^/]*|deprecated_seeds)(/|$)') {
    throw "Effect gate requires an explicit SDK compiler from the current build; refusing '$compilerPath'."
}
if (-not $OutputDir) {
    $OutputDir = Join-Path $PSScriptRoot ('.runs/' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
}
$outRoot = [IO.Path]::GetFullPath($OutputDir)
if (Test-Path -LiteralPath $outRoot) { throw "Refusing to overwrite existing output: $outRoot" }
New-Item -ItemType Directory -Force -Path $outRoot | Out-Null

$gateEnvNames = @('PATH', 'LLVM_ROOT', 'VYX_STD_PACKAGES',
                  'VYX_PHASE_SUMMARY', 'VYX_BOOTSTRAP_PROFILE',
                  'VYX_EFFECT_MANIFEST_OUT', 'VYX_EFFECT_MANIFEST_IN',
                  'VYX_EFFECT_SCHEMAS_IN', 'VYX_EFFECT_SOURCE_STAMP',
                  'VYX_EFFECT_COMPILER_STAMP', 'VYX_EFFECT_TARGET',
                  'VYX_EFFECT_ABI', 'VYX_EFFECT_SCOPE', 'VYX_DCI_EXPORT_OUT')
$savedGateEnv = @{}
foreach ($name in $gateEnvNames) {
    $savedGateEnv[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
try {
$runtimeOut = Join-Path $repo 'bootstrap_compiler/out'
$clangBin = Join-Path $repo 'clang/bin'
$env:PATH = "$runtimeOut;$clangBin;$env:PATH"
$env:LLVM_ROOT = Join-Path $repo 'clang'
$env:VYX_STD_PACKAGES = Join-Path $repo 'bootstrap_compiler/std_packages'
$env:VYX_PHASE_SUMMARY = '1'
$env:VYX_BOOTSTRAP_PROFILE = '1'

function Clear-EffectManifestEnv {
    foreach ($name in @('VYX_EFFECT_MANIFEST_OUT','VYX_EFFECT_MANIFEST_IN',
                        'VYX_EFFECT_SCHEMAS_IN',
                        'VYX_EFFECT_SOURCE_STAMP','VYX_EFFECT_COMPILER_STAMP',
                        'VYX_EFFECT_TARGET','VYX_EFFECT_ABI','VYX_EFFECT_SCOPE',
                        'VYX_DCI_EXPORT_OUT')) {
        Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue
    }
}
Clear-EffectManifestEnv

function Write-Utf8NoBom([string]$Path, [string]$Text) {
    [IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false))
}
function Invoke-Capture([string]$File, [string[]]$Arguments, [string]$WorkingDirectory) {
    $stdout = Join-Path $WorkingDirectory ([IO.Path]::GetFileName($File) + '.stdout.log')
    $stderr = Join-Path $WorkingDirectory ([IO.Path]::GetFileName($File) + '.stderr.log')
    $proc = Start-Process -FilePath $File -ArgumentList $Arguments -WorkingDirectory $WorkingDirectory `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr -NoNewWindow -PassThru -Wait
    $text = ''
    if (Test-Path -LiteralPath $stdout) { $text += Get-Content -LiteralPath $stdout -Raw }
    if (Test-Path -LiteralPath $stderr) { $text += Get-Content -LiteralPath $stderr -Raw }
    [pscustomobject]@{ Exit = [int]$proc.ExitCode; Text = $text; Stdout = $stdout; Stderr = $stderr }
}
function Invoke-Vyx([string[]]$Arguments, [string]$WorkingDirectory) {
    return Invoke-Capture $compilerPath $Arguments $WorkingDirectory
}
function Assert-Equal([string]$Name, [object]$Actual, [object]$Expected) {
    if ($Actual -ne $Expected) { throw "$Name expected '$Expected', got '$Actual'" }
}

$compilerHash = (Get-FileHash -LiteralPath $compilerPath -Algorithm SHA256).Hash
$versionProbe = & $compilerPath '--version' 2>&1 | Out-String
$versionExit = $LASTEXITCODE
if ($versionExit -ne 0) { throw "compiler --version failed with exit $versionExit" }

# Custom attributes use the compiler-owned declarative schema registry.  The
# myAttr handler validates its named arguments, emits a handled Effect result,
# and can derive the existing retain lowering capability.
$customDir = Join-Path $outRoot 'custom-attrs'
New-Item -ItemType Directory -Force -Path $customDir | Out-Null
$customSrc = Join-Path $PSScriptRoot 'fixtures/custom_attrs.vyx'
$manifestConsumerSrc = Join-Path $PSScriptRoot 'fixtures/manifest_consumer.vyx'
$customExe = Join-Path $customDir 'custom_attrs.exe'
$manifestOut = Join-Path $customDir 'custom_attrs.effect.manifest'
$env:VYX_EFFECT_MANIFEST_OUT = $manifestOut
$customBuild = Invoke-Vyx @('--src=file', $customSrc, '--emit=exe', '-o', $customExe) $customDir
Write-Utf8NoBom (Join-Path $customDir 'compile.log') $customBuild.Text
if ($customBuild.Exit -ne 0) { throw "custom attribute compile failed (exit $($customBuild.Exit)); see $($customBuild.Stdout)" }
if (-not (Test-Path -LiteralPath $manifestOut)) { throw "Effect manifest export missing: $manifestOut" }
$manifestHeader = Get-Content -LiteralPath $manifestOut -TotalCount 1
if ($manifestHeader -notmatch '^H\t1\tvyx\tvyx\.aot\.v1\tunit\t<unit>\tsource:\d+:-?\d+\tbootstrap\.effect\.v1\t-?\d+$') {
    throw "unexpected Effect manifest header: $manifestHeader"
}
$manifestHeaderFields = $manifestHeader -split "`t"
if ($manifestHeaderFields.Count -ne 9) { throw "malformed Effect manifest header: $manifestHeader" }
$manifestSourceStamp = $manifestHeaderFields[6]
$manifestInspect = & python (Join-Path $repo 'tools/effect_manifest.py') validate $manifestOut `
    --target vyx --abi vyx.aot.v1 --scope unit --source $manifestSourceStamp `
    --compiler bootstrap.effect.v1 2>&1 | Out-String
$manifestInspectExit = $LASTEXITCODE
Write-Utf8NoBom (Join-Path $customDir 'manifest.inspect.log') $manifestInspect
if ($manifestInspectExit -ne 0 -or $manifestInspect -notmatch 'effect-manifest: PASS') {
    throw "Effect manifest inspect/validation failed (exit $manifestInspectExit): $manifestInspect"
}
$customRun = Invoke-Capture $customExe @() $customDir
Write-Utf8NoBom (Join-Path $customDir 'run.log') $customRun.Text
if ($customRun.Exit -ne 0 -or $customRun.Text -notmatch 'custom-attr-aot-ok') {
    throw "custom attribute AOT smoke failed: exit=$($customRun.Exit) output=$($customRun.Text)"
}
$customDump = Invoke-Vyx @('--src=file', $customSrc, '--dump-mir2') $customDir
Write-Utf8NoBom (Join-Path $customDir 'custom.mir.log') $customDump.Text
if ($customDump.Exit -ne 0 -or $customDump.Text -notmatch '(?m)name=custom_kept\b') {
    throw "registered myAttr handler did not authorize retained custom_kept function: exit=$($customDump.Exit)"
}
$effectRows = @([regex]::Matches($customBuild.Text, '(?m)\[MOSP effect\][^\r\n]*') | ForEach-Object { $_.Value })
if ($effectRows.Count -eq 0) { throw 'custom attribute compile produced no [MOSP effect] summary' }
$effectRow = $effectRows[-1]
$factMatch = [regex]::Match($effectRow, 'facts=(\d+)')
$handlerMatch = [regex]::Match($effectRow, 'handlers=(\d+)')
$unregisteredMatch = [regex]::Match($effectRow, 'unregistered=(\d+)')
$dispatchedMatch = [regex]::Match($effectRow, 'dispatched=(\d+)')
$malformedMatch = [regex]::Match($effectRow, 'malformed=(\d+)')
if (-not $factMatch.Success -or -not $handlerMatch.Success -or -not $unregisteredMatch.Success -or -not $malformedMatch.Success -or -not $dispatchedMatch.Success) {
    throw "unparseable Effect summary: $effectRow"
}
if ([int]$factMatch.Groups[1].Value -lt 2) { throw "registered + custom facts not observed: $effectRow" }
if ([int]$handlerMatch.Groups[1].Value -lt 1) { throw "built-in handler registry empty: $effectRow" }
if ([int]$unregisteredMatch.Groups[1].Value -ne 0) { throw "registered myAttr unexpectedly remained opaque: $effectRow" }
if ([int]$dispatchedMatch.Groups[1].Value -lt 3) { throw "custom + reflect handlers were not dispatched: $effectRow" }
Assert-Equal 'malformed custom fact count' ([int]$malformedMatch.Groups[1].Value) 0

$invalidSrc = Join-Path $PSScriptRoot 'fixtures/custom_attr_invalid.vyx'
$invalidBuild = Invoke-Vyx @('--src=file', $invalidSrc, '--dump-mir2') $customDir
Write-Utf8NoBom (Join-Path $customDir 'invalid.compile.log') $invalidBuild.Text
if ($invalidBuild.Exit -eq 0 -or $invalidBuild.Text -notmatch 'EFFECT-ARGS') {
    throw "invalid registered myAttr arguments were accepted: exit=$($invalidBuild.Exit)"
}

# Package supplied schemas are loaded as data and bound to a compiler-owned
# generic handler.  This exercises a qualified custom attribute without adding
# executable plugin code to the compiler process.
$declarativeDir = Join-Path $outRoot 'declarative-handler'
New-Item -ItemType Directory -Force -Path $declarativeDir | Out-Null
$declarativeSchema = Join-Path $declarativeDir 'schemas.effect'
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'fixtures/declarative_schemas.txt') -Destination $declarativeSchema
$declarativeSrc = Join-Path $PSScriptRoot 'fixtures/declarative_use.vyx'
$declarativeExe = Join-Path $declarativeDir 'declarative_use.exe'
# Keep the producer manifest immutable while this independent package-schema
# smoke runs.  Otherwise the generic fixture would overwrite the artifact
# whose source stamp is consumed by the cross-module acceptance check below.
Clear-EffectManifestEnv
$env:VYX_EFFECT_SCHEMAS_IN = $declarativeSchema
$declarativeBuild = Invoke-Vyx @('--src=file', $declarativeSrc, '--emit=exe', '-o', $declarativeExe) $declarativeDir
Write-Utf8NoBom (Join-Path $declarativeDir 'compile.log') $declarativeBuild.Text
if ($declarativeBuild.Exit -ne 0) { throw "declarative schema compile failed (exit $($declarativeBuild.Exit)); see $declarativeDir" }
$declarativeRun = Invoke-Capture $declarativeExe @() $declarativeDir
Write-Utf8NoBom (Join-Path $declarativeDir 'run.log') $declarativeRun.Text
if ($declarativeRun.Exit -ne 0 -or $declarativeRun.Text -notmatch 'declarative-handler-aot-ok') {
    throw "declarative schema AOT smoke failed: exit=$($declarativeRun.Exit) output=$($declarativeRun.Text)"
}
$declarativeDump = Invoke-Vyx @('--src=file', $declarativeSrc, '--dump-mir2') $declarativeDir
Write-Utf8NoBom (Join-Path $declarativeDir 'mir.log') $declarativeDump.Text
if ($declarativeDump.Exit -ne 0 -or $declarativeDump.Text -notmatch '(?m)name=declarative_kept\b') {
    throw 'package schema retain consequence did not authorize declarative_kept in MIR'
}
Clear-EffectManifestEnv

# Feed the producer artifact back through the compiler's explicit consumer
# boundary. Source/compiler stamps are required so stale metadata cannot
# silently authorize a later lowering phase.
$acceptedDir = Join-Path $outRoot 'manifest-in-accepted'
New-Item -ItemType Directory -Force -Path $acceptedDir | Out-Null
Clear-EffectManifestEnv
$env:VYX_EFFECT_MANIFEST_IN = $manifestOut
$env:VYX_EFFECT_SOURCE_STAMP = $manifestSourceStamp
$env:VYX_EFFECT_COMPILER_STAMP = 'bootstrap.effect.v1'
$env:VYX_EFFECT_TARGET = 'vyx'
$env:VYX_EFFECT_ABI = 'vyx.aot.v1'
$env:VYX_EFFECT_SCOPE = 'unit'
$acceptedManifestOut = Join-Path $acceptedDir 'accepted.effect.manifest'
$env:VYX_EFFECT_MANIFEST_OUT = $acceptedManifestOut
$acceptedExe = Join-Path $acceptedDir 'accepted.exe'
$acceptedBuild = Invoke-Vyx @('--src=file', $manifestConsumerSrc, '--emit=exe', '-o', $acceptedExe) $acceptedDir
Write-Utf8NoBom (Join-Path $acceptedDir 'compile.log') $acceptedBuild.Text
if ($acceptedBuild.Exit -ne 0 -or $acceptedBuild.Text -notmatch '(?i)effect-manifest.*accepted') {
    throw "manifest consumer accepted-path failed (exit $($acceptedBuild.Exit)); see $acceptedDir"
}
if (-not (Test-Path -LiteralPath $acceptedManifestOut)) { throw "manifest consumer export missing: $acceptedManifestOut" }
$acceptedManifestText = Get-Content -LiteralPath $acceptedManifestOut -Raw
$acceptedRecordCount = ([regex]::Matches($acceptedManifestText, '(?m)^R\t')).Count
if ($acceptedRecordCount -lt 2) {
    throw "manifest consumer did not publish imported facts: records=$acceptedRecordCount"
}
$acceptedEffectRows = @([regex]::Matches($acceptedBuild.Text, '(?m)\[MOSP effect\][^\r\n]*') | ForEach-Object { $_.Value })
$acceptedEffect = if ($acceptedEffectRows.Count -gt 0) { $acceptedEffectRows[-1] } else { '' }
$acceptedRun = Invoke-Capture $acceptedExe @() $acceptedDir
Write-Utf8NoBom (Join-Path $acceptedDir 'run.log') $acceptedRun.Text
if ($acceptedRun.Exit -ne 0 -or $acceptedRun.Text -notmatch 'manifest-consumer-aot-ok') {
    throw "manifest consumer AOT run failed: exit=$($acceptedRun.Exit) output=$($acceptedRun.Text)"
}

function Assert-RejectedManifest([string]$Label, [string]$SourceStamp, [string]$CompilerStamp, [string]$ExpectedReason) {
    $dir = Join-Path $outRoot ("manifest-in-" + $Label)
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    Clear-EffectManifestEnv
    $env:VYX_EFFECT_MANIFEST_IN = $manifestOut
    $env:VYX_EFFECT_SOURCE_STAMP = $SourceStamp
    $env:VYX_EFFECT_COMPILER_STAMP = $CompilerStamp
    $env:VYX_EFFECT_TARGET = 'vyx'
    $env:VYX_EFFECT_ABI = 'vyx.aot.v1'
    $env:VYX_EFFECT_SCOPE = 'unit'
    $exe = Join-Path $dir ($Label + '.exe')
    $build = Invoke-Vyx @('--src=file', $manifestConsumerSrc, '--emit=exe', '-o', $exe) $dir
    Write-Utf8NoBom (Join-Path $dir 'compile.log') $build.Text
    if ($build.Exit -eq 0 -or $build.Text -notmatch [regex]::Escape("Effect manifest rejected: $ExpectedReason")) {
        throw "manifest $Label rejection failed: exit=$($build.Exit) output=$($build.Text)"
    }
    return [ordered]@{ status='expected_fail'; exit=$build.Exit; reason=$ExpectedReason }
}
$sourceStale = Assert-RejectedManifest 'source-stale' 'source:stale' 'bootstrap.effect.v1' 'source-stale'
$compilerStale = Assert-RejectedManifest 'compiler-stale' $manifestSourceStamp 'bootstrap.effect.v2' 'compiler-stale'
Clear-EffectManifestEnv

# Schema contents are a semantic dependency of compiler-produced manifests.
# Loading a different valid package schema must invalidate the old producer
# even when target, ABI, scope, source, and compiler stamps still match.
$schemaStaleDir = Join-Path $outRoot 'manifest-in-schema-stale'
New-Item -ItemType Directory -Force -Path $schemaStaleDir | Out-Null
$schemaStaleTable = Join-Path $schemaStaleDir 'schemas.effect'
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'fixtures/declarative_schemas.txt') -Destination $schemaStaleTable
Clear-EffectManifestEnv
$env:VYX_EFFECT_MANIFEST_IN = $manifestOut
$env:VYX_EFFECT_SOURCE_STAMP = $manifestSourceStamp
$env:VYX_EFFECT_COMPILER_STAMP = 'bootstrap.effect.v1'
$env:VYX_EFFECT_TARGET = 'vyx'
$env:VYX_EFFECT_ABI = 'vyx.aot.v1'
$env:VYX_EFFECT_SCOPE = 'unit'
$env:VYX_EFFECT_SCHEMAS_IN = $schemaStaleTable
$schemaStaleExe = Join-Path $schemaStaleDir 'schema-stale.exe'
$schemaStaleBuild = Invoke-Vyx @('--src=file', $manifestConsumerSrc, '--emit=exe', '-o', $schemaStaleExe) $schemaStaleDir
Write-Utf8NoBom (Join-Path $schemaStaleDir 'compile.log') $schemaStaleBuild.Text
if ($schemaStaleBuild.Exit -eq 0 -or $schemaStaleBuild.Text -notmatch 'Effect manifest rejected: schema-stale') {
    throw "manifest schema-stale rejection failed: exit=$($schemaStaleBuild.Exit) output=$($schemaStaleBuild.Text)"
}
$schemaStale = [ordered]@{ status='expected_fail'; exit=$schemaStaleBuild.Exit; reason='schema-stale' }
Clear-EffectManifestEnv

# The retain handler is a concrete Effect -> lowering connection: an
# uncalled function receives a validated codegen root, while the same source
# without retain is removed from the streaming MIR schedule.
$retainDir = Join-Path $outRoot 'retain-lowering'
New-Item -ItemType Directory -Force -Path $retainDir | Out-Null
$retainSrc = Join-Path $PSScriptRoot 'fixtures/retain_effect.vyx'
$retainNoneSrc = Join-Path $PSScriptRoot 'fixtures/retain_none.vyx'
$retainDump = Invoke-Vyx @('--src=file', $retainSrc, '--dump-mir2') $retainDir
Write-Utf8NoBom (Join-Path $retainDir 'retain.mir.log') $retainDump.Text
if ($retainDump.Exit -ne 0 -or $retainDump.Text -notmatch 'mir2\.unit functions=2 ' -or $retainDump.Text -notmatch 'name=kept ') {
    throw "retain handler did not preserve uncalled kept function (exit $($retainDump.Exit))"
}
$retainCount = [int]([regex]::Match($retainDump.Text, '(?m)^mir2\.unit functions=(\d+)').Groups[1].Value)
$noneDump = Invoke-Vyx @('--src=file', $retainNoneSrc, '--dump-mir2') $retainDir
Write-Utf8NoBom (Join-Path $retainDir 'control.mir.log') $noneDump.Text
if ($noneDump.Exit -ne 0 -or $noneDump.Text -notmatch 'mir2\.unit functions=1 ' -or $noneDump.Text -match 'name=kept ') {
    throw "retain control unexpectedly preserved uncalled kept function (exit $($noneDump.Exit))"
}
$retainControlCount = [int]([regex]::Match($noneDump.Text, '(?m)^mir2\.unit functions=(\d+)').Groups[1].Value)
$retainExe = Join-Path $retainDir 'retain.exe'
$retainBuild = Invoke-Vyx @('--src=file', $retainSrc, '--emit=exe', '-o', $retainExe) $retainDir
Write-Utf8NoBom (Join-Path $retainDir 'compile.log') $retainBuild.Text
if ($retainBuild.Exit -ne 0) { throw "retain AOT compile failed: exit=$($retainBuild.Exit)" }
$retainRun = Invoke-Capture $retainExe @() $retainDir
Write-Utf8NoBom (Join-Path $retainDir 'run.log') $retainRun.Text
if ($retainRun.Exit -ne 0 -or $retainRun.Text -notmatch 'retain-effect-aot-ok') {
    throw "retain AOT run failed: exit=$($retainRun.Exit) output=$($retainRun.Text)"
}
$retainInvalidSrc = Join-Path $PSScriptRoot 'fixtures/retain_invalid.vyx'
$retainInvalid = Invoke-Vyx @('--src=file', $retainInvalidSrc, '--dump-mir2') $retainDir
Write-Utf8NoBom (Join-Path $retainDir 'invalid.log') $retainInvalid.Text
if ($retainInvalid.Exit -eq 0 -or $retainInvalid.Text -notmatch 'EFFECT-ARGS') {
    throw "retain invalid-argument path was not rejected (exit $($retainInvalid.Exit))"
}

# Async/task is a concrete Effect -> coroutine lowering connection.  The
# source uses only the public @[async] spelling; Effect normalization stamps
# |effect:async| before HIR builds the await state machine.  Keep this gate
# independent from the large async stress suite while checking both MIR yield
# generation and native execution.
$asyncDir = Join-Path $outRoot 'async-lowering'
New-Item -ItemType Directory -Force -Path $asyncDir | Out-Null
$asyncSrc = Join-Path $repo 'probes/gates/async/await_split.vyx'
$asyncDump = Invoke-Vyx @('--src=file', $asyncSrc, '--dump-mir2') $asyncDir
Write-Utf8NoBom (Join-Path $asyncDir 'async.mir.log') $asyncDump.Text
if ($asyncDump.Exit -ne 0 -or $asyncDump.Text -notmatch 'term=7') {
    throw "async Effect lowering did not produce MIR yield (exit $($asyncDump.Exit))"
}
$asyncExe = Join-Path $asyncDir 'async.exe'
$asyncBuild = Invoke-Vyx @('--src=file', $asyncSrc, '--emit=exe', '-o', $asyncExe) $asyncDir
Write-Utf8NoBom (Join-Path $asyncDir 'compile.log') $asyncBuild.Text
if ($asyncBuild.Exit -ne 0) { throw "async Effect AOT compile failed: exit $($asyncBuild.Exit)" }
$asyncRun = Invoke-Capture $asyncExe @() $asyncDir
Write-Utf8NoBom (Join-Path $asyncDir 'run.log') $asyncRun.Text
if ($asyncRun.Exit -ne 0) {
    throw "async Effect AOT run failed: exit $($asyncRun.Exit) output=$($asyncRun.Text)"
}
$asyncInvalidSrc = Join-Path $PSScriptRoot 'fixtures/async_invalid.vyx'
$asyncInvalid = Invoke-Vyx @('--src=file', $asyncInvalidSrc, '--dump-mir2') $asyncDir
Write-Utf8NoBom (Join-Path $asyncDir 'invalid.log') $asyncInvalid.Text
if ($asyncInvalid.Exit -eq 0 -or $asyncInvalid.Text -notmatch 'EFFECT-ARGS') {
    throw "async invalid-argument path was not rejected (exit $($asyncInvalid.Exit))"
}

# The Effect boundary must also hold for the packages shipped with the SDK,
# not just for probes that happen to use the same syntax.  This fixture imports
# the real std.vio package, calls its platform-selected implementation, and
# carries a local platform marker so the compiler-owned handler is observable.
$stdlibDir = Join-Path $outRoot 'stdlib-effect'
New-Item -ItemType Directory -Force -Path $stdlibDir | Out-Null
$stdlibSrc = Join-Path $PSScriptRoot 'fixtures/stdlib_effect.vyx'
$stdlibManifest = Join-Path $stdlibDir 'stdlib.effect.manifest'
$env:VYX_EFFECT_MANIFEST_OUT = $stdlibManifest
$stdlibDump = Invoke-Vyx @('--src=file', $stdlibSrc, '--dump-mir2') $stdlibDir
Write-Utf8NoBom (Join-Path $stdlibDir 'stdlib.mir.log') $stdlibDump.Text
if ($stdlibDump.Exit -ne 0 -or $stdlibDump.Text -notmatch 'std\.vio\.') {
    throw "standard-library Effect import did not resolve std.vio (exit $($stdlibDump.Exit))"
}
if (-not (Test-Path -LiteralPath $stdlibManifest)) {
    throw "standard-library Effect manifest was not exported: $stdlibManifest"
}
$stdlibManifestText = Get-Content -LiteralPath $stdlibManifest -Raw
if ($stdlibManifestText -notmatch '(?m)^R\tstdlib_platform_value\tplatform\t') {
    throw 'standard-library Effect manifest lost the platform.select fact for the fixture'
}
Clear-EffectManifestEnv
$stdlibRows = @([regex]::Matches($stdlibDump.Text, '(?m)\[MOSP effect\][^\r\n]*') | ForEach-Object { $_.Value })
if ($stdlibRows.Count -eq 0) { throw 'standard-library fixture produced no Effect summary' }
$stdlibEffect = $stdlibRows[-1]
$stdlibFacts = [regex]::Match($stdlibEffect, 'facts=(\d+)')
$stdlibDispatched = [regex]::Match($stdlibEffect, 'dispatched=(\d+)')
if (-not $stdlibFacts.Success -or -not $stdlibDispatched.Success -or
    [int]$stdlibFacts.Groups[1].Value -lt 1 -or
    [int]$stdlibDispatched.Groups[1].Value -lt 1) {
    throw "standard-library Effect summary did not record a handled fact: $stdlibEffect"
}
$stdlibExe = Join-Path $stdlibDir 'stdlib_effect.exe'
$stdlibBuild = Invoke-Vyx @('--src=file', $stdlibSrc, '--emit=exe', '-o', $stdlibExe) $stdlibDir
Write-Utf8NoBom (Join-Path $stdlibDir 'compile.log') $stdlibBuild.Text
if ($stdlibBuild.Exit -ne 0) { throw "standard-library Effect AOT compile failed: exit $($stdlibBuild.Exit)" }
$stdlibRun = Invoke-Capture $stdlibExe @() $stdlibDir
Write-Utf8NoBom (Join-Path $stdlibDir 'run.log') $stdlibRun.Text
if ($stdlibRun.Exit -ne 0 -or $stdlibRun.Text -notmatch 'stdlib-effect-aot-ok') {
    throw "standard-library Effect AOT run failed: exit=$($stdlibRun.Exit) output=$($stdlibRun.Text)"
}

# DCI import is a real Effect -> driver boundary.  The positive fixture uses
# an existing binary contract and only dumps MIR, so this gate exercises path
# discovery and lifecycle binding without depending on a native linker.  The
# negative fixture puts the same valid path on a field: the raw scanner must
# not authorize it, and Sema must reject the target before binding the ABI.
$dciEffectDir = Join-Path $outRoot 'dci-import-boundary'
New-Item -ItemType Directory -Force -Path $dciEffectDir | Out-Null
$dciPositiveSrc = Join-Path $repo 'probes/gates/dci-failure/checked_divide.vyx'
$dciPositive = Invoke-Vyx @('--src=file', $dciPositiveSrc, '--dump-mir2') $dciEffectDir
Write-Utf8NoBom (Join-Path $dciEffectDir 'positive.log') $dciPositive.Text
if ($dciPositive.Exit -ne 0 -or
    $dciPositive.Text -notmatch '(?m)dci-lifecycle[^\r\n]*0 errors') {
    throw "DCI Effect import positive path failed (exit $($dciPositive.Exit)); see $dciEffectDir"
}
$dciNegativeSrc = Join-Path $PSScriptRoot 'fixtures/dci_import_invalid_target.vyx'
$dciNegative = Invoke-Vyx @('--src=file', $dciNegativeSrc, '--dump-mir2') $dciEffectDir
Write-Utf8NoBom (Join-Path $dciEffectDir 'negative.log') $dciNegative.Text
if ($dciNegative.Exit -eq 0 -or
    $dciNegative.Text -notmatch 'EFFECT-TARGET' -or
    $dciNegative.Text -notmatch 'EffectPlan') {
    throw "DCI Effect import invalid-target path was not rejected before binding (exit $($dciNegative.Exit)); see $dciEffectDir"
}

# DCI export is a two-part boundary: the validated Effect marker keeps the
# declaration as an AOT root, while an explicit selector emits a data-only
# sidecar that downstream bridge tooling can consume.  No selector means no
# sidecar is silently invented.
$dciExportDir = Join-Path $outRoot 'dci-export-boundary'
New-Item -ItemType Directory -Force -Path $dciExportDir | Out-Null
$dciExportOut = Join-Path $dciExportDir 'exports.dci-export'
$dciExportManifestOut = Join-Path $dciExportDir 'exports.effect.manifest'
$dciExportSrc = Join-Path $PSScriptRoot 'fixtures/dci_export.vyx'
$env:VYX_DCI_EXPORT_OUT = $dciExportOut
$env:VYX_EFFECT_MANIFEST_OUT = $dciExportManifestOut
$dciExport = Invoke-Vyx @('--src=file', $dciExportSrc, '--dump-mir2') $dciExportDir
Write-Utf8NoBom (Join-Path $dciExportDir 'build.log') $dciExport.Text
if ($dciExport.Exit -ne 0 -or -not (Test-Path -LiteralPath $dciExportOut)) {
    throw "DCI export Effect fixture failed (exit $($dciExport.Exit)); see $dciExportDir"
}
$dciExportText = Get-Content -LiteralPath $dciExportOut -Raw
if (($dciExportText -notmatch '(?m)^H\tMOSP-DCI-EXPORT\t1\t') -or ($dciExportText -notmatch '(?m)^R\teffect_gate_exported\t') -or ($dciExportText -notmatch '(?m)\tdci_export\t')) {
    throw "DCI export sidecar is missing the sealed declaration record; see $dciExportOut"
}
$dciExportInspect = & python (Join-Path $repo 'tools/effect_manifest.py') validate-dci-export $dciExportOut --manifest $dciExportManifestOut 2>&1 | Out-String
$dciExportInspectExit = $LASTEXITCODE
Write-Utf8NoBom (Join-Path $dciExportDir 'sidecar.inspect.log') $dciExportInspect
if ($dciExportInspectExit -ne 0 -or $dciExportInspect -notmatch 'dci-export: PASS') {
    throw "DCI export provenance inspection failed (exit $dciExportInspectExit): $dciExportInspect"
}
$dciArtifactProvenance = & python (Join-Path $PSScriptRoot 'check_artifact_provenance.py') $dciExportOut $dciExportManifestOut 2>&1 | Out-String
$dciArtifactProvenanceExit = $LASTEXITCODE
Write-Utf8NoBom (Join-Path $dciExportDir 'artifact.provenance.log') $dciArtifactProvenance
if ($dciArtifactProvenanceExit -ne 0 -or $dciArtifactProvenance -notmatch 'effect-artifact-provenance: PASS') {
    throw "DCI export provenance mutation gate failed (exit $dciArtifactProvenanceExit): $dciArtifactProvenance"
}
$dciExportIr = Join-Path $dciExportDir 'dci_export.ll'
$dciExportIrBuild = Invoke-Vyx @('--src=file', $dciExportSrc, '--emit=ir', '-o', $dciExportIr) $dciExportDir
Write-Utf8NoBom (Join-Path $dciExportDir 'ir.log') $dciExportIrBuild.Text
if ($dciExportIrBuild.Exit -ne 0 -or -not (Test-Path -LiteralPath $dciExportIr)) {
    throw "DCI export IR emission failed: exit $($dciExportIrBuild.Exit)"
}
$dciExportIrText = Get-Content -LiteralPath $dciExportIr -Raw
if ($dciExportIrText -notmatch '(?m)define .*@effect_gate_add\(') {
    throw "DCI export symbol selector did not reach the emitted AOT symbol"
}
$dciExportExe = Join-Path $dciExportDir 'dci_export.exe'
$dciExportRunBuild = Invoke-Vyx @('--src=file', $dciExportSrc, '--emit=exe', '-o', $dciExportExe) $dciExportDir
Write-Utf8NoBom (Join-Path $dciExportDir 'compile.log') $dciExportRunBuild.Text
if ($dciExportRunBuild.Exit -ne 0) { throw "DCI export AOT compile failed: exit $($dciExportRunBuild.Exit)" }
$dciExportProcess = Start-Process -FilePath $dciExportExe -WorkingDirectory $dciExportDir -Wait -PassThru -NoNewWindow
if ($dciExportProcess.ExitCode -ne 0) { throw "DCI export AOT run failed: exit $($dciExportProcess.ExitCode)" }
$dciExportInvalidOut = Join-Path $dciExportDir 'invalid.dci-export'
$env:VYX_DCI_EXPORT_OUT = $dciExportInvalidOut
$dciExportInvalid = Invoke-Vyx @('--src=file', $customSrc, '--dump-mir2') $dciExportDir
if ($dciExportInvalid.Exit -eq 0 -or $dciExportInvalid.Text -notmatch 'no sealed dci_export Effect') {
    throw "DCI export selector accepted a unit without dci_export (exit $($dciExportInvalid.Exit))"
}
Clear-EffectManifestEnv

# Platform selection is also a real Effect boundary.  The fixture contains
# inactive declarations with unresolved names; only the compiler-authorized
# `platform.select` marker may remove them before semantic resolution.
$platformDir = Join-Path $outRoot 'platform-select'
New-Item -ItemType Directory -Force -Path $platformDir | Out-Null
$platformSrc = Join-Path $repo 'tests/cases/platform_attr_filter.vyx'
$platformExe = Join-Path $platformDir 'platform.exe'
$platformBuild = Invoke-Vyx @('--src=file', $platformSrc, '--emit=exe', '-o', $platformExe) $platformDir
Write-Utf8NoBom (Join-Path $platformDir 'compile.log') $platformBuild.Text
if ($platformBuild.Exit -ne 0) { throw "platform Effect compile failed: exit $($platformBuild.Exit)" }
$platformRun = Invoke-Capture $platformExe @() $platformDir
Write-Utf8NoBom (Join-Path $platformDir 'run.log') $platformRun.Text
if ($platformRun.Exit -ne 0) {
    throw "platform Effect AOT run failed: exit $($platformRun.Exit) output=$($platformRun.Text)"
}

# Derive and comptime are semantic Effect consumers. Their source attributes
# are accepted by registered handlers before trait synthesis and constant
# folding; these AOT runs prove the generated methods and folded values remain
# usable after the marker boundary.
$semanticDir = Join-Path $outRoot 'semantic-effects'
New-Item -ItemType Directory -Force -Path $semanticDir | Out-Null
$deriveSrc = Join-Path $repo 'probes/gates/derive/enum_unit.vyx'
$deriveExe = Join-Path $semanticDir 'derive.exe'
$deriveBuild = Invoke-Vyx @('--src=file', $deriveSrc, '--emit=exe', '-o', $deriveExe) $semanticDir
Write-Utf8NoBom (Join-Path $semanticDir 'derive.compile.log') $deriveBuild.Text
if ($deriveBuild.Exit -ne 0) { throw "derive Effect compile failed: exit $($deriveBuild.Exit)" }
$deriveRun = Invoke-Capture $deriveExe @() $semanticDir
Write-Utf8NoBom (Join-Path $semanticDir 'derive.run.log') $deriveRun.Text
if ($deriveRun.Exit -ne 0) { throw "derive Effect AOT run failed: exit $($deriveRun.Exit)" }
$comptimeSrc = Join-Path $repo 'probes/gates/comptime/fold.vyx'
$comptimeExe = Join-Path $semanticDir 'comptime.exe'
$comptimeBuild = Invoke-Vyx @('--src=file', $comptimeSrc, '--emit=exe', '-o', $comptimeExe) $semanticDir
Write-Utf8NoBom (Join-Path $semanticDir 'comptime.compile.log') $comptimeBuild.Text
if ($comptimeBuild.Exit -ne 0) { throw "comptime Effect compile failed: exit $($comptimeBuild.Exit)" }
$comptimeRun = Invoke-Capture $comptimeExe @() $semanticDir
Write-Utf8NoBom (Join-Path $semanticDir 'comptime.run.log') $comptimeRun.Text
if ($comptimeRun.Exit -ne 0) { throw "comptime Effect AOT run failed: exit $($comptimeRun.Exit)" }

# The fixture below exercises parser-level cases (version, scope, malformed
# rows) in addition to the compiler boundary above.
$manifestDir = Join-Path $outRoot 'manifest-api'
New-Item -ItemType Directory -Force -Path $manifestDir | Out-Null
Copy-Item -LiteralPath (Join-Path $repo 'bootstrap_compiler/src/core/facts/effect_manifest.vyx') `
    -Destination (Join-Path $manifestDir 'effect_manifest.vyx')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'fixtures/manifest_api.vyx') `
    -Destination (Join-Path $manifestDir 'main.vyx')
$manifestToml = @'
[package]
name = "mosp_effect_manifest_gate"
version = "0.1.0"
entry = "main.vyx"

[build]
cache_dir = ".cache"
output_dir = "target"
threads = 1

[target.manifest_gate]
type = "executable"
entry = "main.vyx"
sources = ["effect_manifest.vyx"]
'@
Write-Utf8NoBom (Join-Path $manifestDir 'Vyx.toml') $manifestToml
$manifestBuild = Invoke-Vyx @('build', '--target', 'manifest_gate', '-j1') $manifestDir
Write-Utf8NoBom (Join-Path $manifestDir 'compile.log') $manifestBuild.Text
if ($manifestBuild.Exit -ne 0) { throw "manifest API build failed (exit $($manifestBuild.Exit)); see $($manifestDir)" }
$manifestExe = Join-Path $manifestDir 'target/manifest_gate.exe'
if (-not (Test-Path -LiteralPath $manifestExe)) { throw "manifest API executable missing: $manifestExe" }
$manifestRun = Invoke-Capture $manifestExe @() $manifestDir
Write-Utf8NoBom (Join-Path $manifestDir 'run.log') $manifestRun.Text
if ($manifestRun.Exit -ne 0) {
    throw "manifest API fixture rejected expected cases or crashed (exit $($manifestRun.Exit)); see $($manifestDir)/run.log"
}
$requiredTokens = @('manifest-fresh','target-mismatch','abi-mismatch','scope-mismatch','source-stale','compiler-stale','version-stale','malformed-rejected','manifest-blank-line-rejected','manifest-order-independent-fingerprint','manifest-dependency-invalidation','manifest-identity-conflict','manifest-encoding-and-flags-rejected')
foreach ($token in $requiredTokens) {
    if ($manifestRun.Text -notmatch [regex]::Escape($token)) { throw "manifest evidence missing token '$token'" }
}

# Cross declaration/module Effect graph.  Edge rows are persisted in the
# manifest dependency channel, and a deterministic DFS rejects a back edge
# with a source-qualified EFFECT-CYCLE diagnostic before sealing.
$cycleDir = Join-Path $outRoot 'effect-cycle'
New-Item -ItemType Directory -Force -Path $cycleDir | Out-Null
Copy-Item -LiteralPath (Join-Path $repo 'bootstrap_compiler/src/core/facts/effect_manifest.vyx') `
    -Destination (Join-Path $cycleDir 'effect_manifest.vyx')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'fixtures/effect_cycle.vyx') `
    -Destination (Join-Path $cycleDir 'main.vyx')
$cycleToml = @'
[package]
name = "mosp_effect_cycle_gate"
version = "0.1.0"
entry = "main.vyx"

[build]
cache_dir = ".cache"
output_dir = "target"
threads = 1

[target.cycle_gate]
type = "executable"
entry = "main.vyx"
sources = ["effect_manifest.vyx"]
'@
Write-Utf8NoBom (Join-Path $cycleDir 'Vyx.toml') $cycleToml
$cycleBuild = Invoke-Vyx @('build', '--target', 'cycle_gate', '-j1') $cycleDir
Write-Utf8NoBom (Join-Path $cycleDir 'compile.log') $cycleBuild.Text
if ($cycleBuild.Exit -ne 0) { throw "Effect cycle gate build failed (exit $($cycleBuild.Exit)); see $cycleDir" }
$cycleExe = Join-Path $cycleDir 'target/cycle_gate.exe'
if (-not (Test-Path -LiteralPath $cycleExe)) { throw "Effect cycle gate executable missing: $cycleExe" }
$cycleRun = Invoke-Capture $cycleExe @() $cycleDir
Write-Utf8NoBom (Join-Path $cycleDir 'run.log') $cycleRun.Text
if ($cycleRun.Exit -ne 0 -or $cycleRun.Text -notmatch 'effect-edge-wire-ok' `
    -or $cycleRun.Text -notmatch 'effect-cycle-rejected') {
    throw "Effect cycle gate failed: exit=$($cycleRun.Exit) output=$($cycleRun.Text)"
}

# DCI lifecycle/exception obligations are exported as ordinary MOSP Effect
# records and typed edge dependencies.  The consumer has no local DCI source;
# acceptance therefore proves that the same EffectPlan imports and seals the
# cross-module graph before HIR lowering.
$dciBridgeDir = Join-Path $outRoot 'dci-effect-bridge'
New-Item -ItemType Directory -Force -Path $dciBridgeDir | Out-Null
python (Join-Path $repo 'probes/gates/dci-exceptions/make_contract.py')
if ($LASTEXITCODE -ne 0) { throw 'DCI obligation contract generation failed' }
$dciManifest = Join-Path $dciBridgeDir 'dci.obligations.effect.manifest'
$dciExceptionDcib = Join-Path $repo 'probes/gates/dci-exceptions/module_exception.dcib'
$dciLifecycleDcib = Join-Path $repo 'probes/gates/dci-exceptions/module_lifecycle.dcib'
python (Join-Path $PSScriptRoot 'fixtures/emit_dci_effect_manifest.py') `
    $dciManifest $dciExceptionDcib $dciLifecycleDcib
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $dciManifest)) {
    throw 'DCI Effect manifest bridge failed'
}
Clear-EffectManifestEnv
$env:VYX_EFFECT_MANIFEST_IN = $dciManifest
$env:VYX_EFFECT_SOURCE_STAMP = 'module_exception.dcib+module_lifecycle.dcib'
$env:VYX_EFFECT_COMPILER_STAMP = 'bootstrap.effect.v1'
$env:VYX_EFFECT_TARGET = 'vyx'
$env:VYX_EFFECT_ABI = 'vyx.aot.v1'
$env:VYX_EFFECT_SCOPE = 'unit'
$env:VYX_EFFECT_MANIFEST_OUT = Join-Path $dciBridgeDir 'consumer.effect.manifest'
$dciBridgeExe = Join-Path $dciBridgeDir 'consumer.exe'
$dciBridgeBuild = Invoke-Vyx @('--src=file', $manifestConsumerSrc, '--emit=exe', '-o', $dciBridgeExe) $dciBridgeDir
Write-Utf8NoBom (Join-Path $dciBridgeDir 'compile.log') $dciBridgeBuild.Text
if ($dciBridgeBuild.Exit -ne 0) { throw "DCI Effect manifest consumer failed (exit $($dciBridgeBuild.Exit)); see $dciBridgeDir" }
$dciBridgeRun = Invoke-Capture $dciBridgeExe @() $dciBridgeDir
Write-Utf8NoBom (Join-Path $dciBridgeDir 'run.log') $dciBridgeRun.Text
if ($dciBridgeRun.Exit -ne 0 -or $dciBridgeRun.Text -notmatch 'manifest-consumer-aot-ok') {
    throw "DCI Effect manifest consumer AOT failed: exit=$($dciBridgeRun.Exit) output=$($dciBridgeRun.Text)"
}
$dciBridgeManifestOut = $env:VYX_EFFECT_MANIFEST_OUT
$dciBridgeManifest = Get-Content -LiteralPath $dciBridgeManifestOut -Raw
if (($dciBridgeManifest -notmatch '(?m)^R\tdci:exception:contract_throw\tdci.exception\t') -or
    ($dciBridgeManifest -notmatch '(?m)^R\tdci:lifecycle:eh::Guard\tdci.lifecycle\t') -or
    ($dciBridgeManifest -notmatch '(?m)^D\tedge:dci:exception:contract_throw->dci:lifecycle:eh::Guard\tcleanup-before-propagate\t\d+')) {
    throw 'DCI Effect manifest consumer lost lifecycle/exception facts or cleanup edge'
}
Clear-EffectManifestEnv

# Source-level Effect declarations are contextual, declaration-only syntax.
# They must enter the EffectPlan and manifest path while remaining absent from
# HIR/MIR lowering.  The negative fixture exercises duplicate and unknown
# fields before any executable body is considered.
$syntaxDir = Join-Path $outRoot 'effect-declaration-syntax'
New-Item -ItemType Directory -Force -Path $syntaxDir | Out-Null
$syntaxSrc = Join-Path $PSScriptRoot 'fixtures/effect_decl.vyx'
$syntaxExe = Join-Path $syntaxDir 'effect_decl.exe'
$syntaxManifest = Join-Path $syntaxDir 'effect_decl.effect.manifest'
$env:VYX_EFFECT_MANIFEST_OUT = $syntaxManifest
$syntaxBuild = Invoke-Vyx @('--src=file', $syntaxSrc, '--emit=exe', '-o', $syntaxExe) $syntaxDir
Write-Utf8NoBom (Join-Path $syntaxDir 'compile.log') $syntaxBuild.Text
if ($syntaxBuild.Exit -ne 0) {
    throw "source Effect declaration compile failed (exit $($syntaxBuild.Exit)); see $syntaxDir"
}
if (-not (Test-Path -LiteralPath $syntaxManifest)) {
    throw "source Effect declaration manifest was not exported: $syntaxManifest"
}
$syntaxManifestText = Get-Content -LiteralPath $syntaxManifest -Raw
if ($syntaxManifestText -notmatch '(?m)^R\treflect_alias\teffect\t') {
    throw 'source Effect declaration did not cross the manifest boundary as an effect fact'
}
$syntaxRun = Invoke-Capture $syntaxExe @() $syntaxDir
Write-Utf8NoBom (Join-Path $syntaxDir 'run.log') $syntaxRun.Text
if ($syntaxRun.Exit -ne 0 -or $syntaxRun.Text -notmatch 'effect-decl-parser-ok') {
    throw "source Effect declaration AOT run failed: exit=$($syntaxRun.Exit) output=$($syntaxRun.Text)"
}
$syntaxDump = Invoke-Vyx @('--src=file', $syntaxSrc, '--dump-mir2') $syntaxDir
Write-Utf8NoBom (Join-Path $syntaxDir 'mir.log') $syntaxDump.Text
if (($syntaxDump.Exit -ne 0) -or
    ($syntaxDump.Text -notmatch '(?m)name=main\b') -or
    ($syntaxDump.Text -match '(?m)name=reflect_alias\b')) {
    throw "source Effect declaration leaked into HIR/MIR or dump failed (exit $($syntaxDump.Exit))"
}
$syntaxInvalidSrc = Join-Path $PSScriptRoot 'fixtures/effect_decl_invalid.vyx'
$syntaxInvalid = Invoke-Vyx @('--src=file', $syntaxInvalidSrc, '--dump-mir2') $syntaxDir
Write-Utf8NoBom (Join-Path $syntaxDir 'invalid.log') $syntaxInvalid.Text
if ($syntaxInvalid.Exit -eq 0 -or
    ($syntaxInvalid.Text -notmatch 'duplicate effect field' -and
     $syntaxInvalid.Text -notmatch 'unknown effect field')) {
    throw "invalid source Effect declaration was accepted: exit=$($syntaxInvalid.Exit)"
}
Clear-EffectManifestEnv

# Project-level `.attr` loading is deliberately tested through `build`, not
# First exercise the source parser directly, built from the current module.
# This covers boundaries that an end-to-end positive project cannot prove.
$attrApiDir = Join-Path $outRoot 'attribute-source-api'
New-Item -ItemType Directory -Force -Path $attrApiDir | Out-Null
Copy-Item -LiteralPath (Join-Path $repo 'bootstrap_compiler/src/core/facts/effect_attributes.vyx') `
    -Destination (Join-Path $attrApiDir 'effect_attributes.vyx')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'fixtures/attribute_source_api.vyx') `
    -Destination (Join-Path $attrApiDir 'main.vyx')
Write-Utf8NoBom (Join-Path $attrApiDir 'Vyx.toml') @'
[package]
name = "attribute_source_api"
version = "0.1.0"
entry = "main.vyx"
[build]
output_dir = "target"
cache_dir = ".cache"
[target.attribute_source_api]
type = "executable"
entry = "main.vyx"
sources = ["effect_attributes.vyx"]
'@
$attrApiBuild = Invoke-Vyx @('build', '--target', 'attribute_source_api', '-j1') $attrApiDir
Write-Utf8NoBom (Join-Path $attrApiDir 'compile.log') $attrApiBuild.Text
if ($attrApiBuild.Exit -ne 0) { throw "attribute source API build failed: $($attrApiBuild.Exit); see $attrApiDir" }
$attrApiRun = Invoke-Capture (Join-Path $attrApiDir 'target/attribute_source_api.exe') @() $attrApiDir
Write-Utf8NoBom (Join-Path $attrApiDir 'run.log') $attrApiRun.Text
if ($attrApiRun.Exit -ne 0 -or $attrApiRun.Text -notmatch 'attribute-source-api-aot-ok') {
    throw "attribute source API rejected boundary cases: exit=$($attrApiRun.Exit); see $attrApiDir"
}

# Library interface policy must survive a cache hit and an interrupted build.
Clear-EffectManifestEnv
$interfaceDir = Join-Path $outRoot 'interface-cache'
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'fixtures/interface_cache') -Destination $interfaceDir -Recurse
foreach ($target in @('stub', 'disabled')) {
    foreach ($mode in @('cold', 'warm')) {
        $built = Invoke-Vyx @('build', '--target', $target, '-j1') $interfaceDir
        Write-Utf8NoBom (Join-Path $interfaceDir "$target-$mode.log") $built.Text
        if ($built.Exit -ne 0) { throw "$target $mode interface-policy build failed: $($built.Exit)" }
        if (Test-Path -LiteralPath (Join-Path $interfaceDir "target/$target.vyi")) {
            throw "$target $mode emitted a disabled library interface"
        }
    }
}
$interfaceEnabled = Invoke-Vyx @('build', '--target', 'enabled', '-j1') $interfaceDir
Write-Utf8NoBom (Join-Path $interfaceDir 'enabled-cold.log') $interfaceEnabled.Text
if ($interfaceEnabled.Exit -ne 0) { throw "enabled interface build failed: $($interfaceEnabled.Exit)" }
$enabledVyi = Join-Path $interfaceDir 'target/enabled.vyi'
if (!(Test-Path -LiteralPath $enabledVyi) -or (Get-Content -LiteralPath $enabledVyi -Raw) -notmatch 'interface_cache_value') {
    throw 'enabled library interface is missing its public declaration'
}
$enabledVyiHash = (Get-FileHash -LiteralPath $enabledVyi).Hash
Remove-Item -LiteralPath $enabledVyi
$interfaceRepaired = Invoke-Vyx @('build', '--target', 'enabled', '-j1') $interfaceDir
Write-Utf8NoBom (Join-Path $interfaceDir 'enabled-repair.log') $interfaceRepaired.Text
if ($interfaceRepaired.Exit -ne 0 -or !(Test-Path -LiteralPath $enabledVyi) -or
    (Get-FileHash -LiteralPath $enabledVyi).Hash -ne $enabledVyiHash) {
    throw 'warm library build did not restore the same enabled interface'
}
Clear-EffectManifestEnv

# Project-level `.attr` loading is deliberately tested through `build`, not
# through the legacy VYX_EFFECT_SCHEMAS_IN escape hatch.  The fixture contains
# a root-discovered file and an explicitly listed subdirectory file; the latter
# is also the base schema for a forward `extends` reference.
$attrProjectDir = Join-Path $outRoot 'attribute-project'
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'fixtures/attribute_project') `
    -Destination $attrProjectDir -Recurse
$attrManifest = Join-Path $attrProjectDir 'target/attribute_project.effect.manifest'
$env:VYX_EFFECT_MANIFEST_OUT = $attrManifest
$attrBuild1 = Invoke-Vyx @('build', '--target', 'attribute_project', '-j2') $attrProjectDir
Write-Utf8NoBom (Join-Path $attrProjectDir 'build-cold.log') $attrBuild1.Text
if ($attrBuild1.Exit -ne 0) {
    throw "project attribute cold build failed (exit $($attrBuild1.Exit)); see $attrProjectDir"
}
$attrExe = Join-Path $attrProjectDir 'target/attribute_project.exe'
if (-not (Test-Path -LiteralPath $attrExe)) { throw "project attribute executable missing: $attrExe" }
$attrRun = Invoke-Capture $attrExe @() $attrProjectDir
Write-Utf8NoBom (Join-Path $attrProjectDir 'run.log') $attrRun.Text
if ($attrRun.Exit -ne 0 -or $attrRun.Text -notmatch 'attribute-project-aot-ok') {
    throw "project attribute AOT run failed: exit=$($attrRun.Exit) output=$($attrRun.Text)"
}
if (-not (Test-Path -LiteralPath $attrManifest)) { throw "project attribute manifest missing: $attrManifest" }
$attrManifest1 = Get-Content -LiteralPath $attrManifest -Raw
if ($attrManifest1 -notmatch '(?m)^R\tattr_kept\tacme\.layout\t') {
    throw 'project attribute manifest omitted acme.layout fact'
}
if ($attrManifest1 -notmatch 'align=16' -or $attrManifest1 -notmatch 'label="startup"' -or $attrManifest1 -notmatch 'code=255') {
    throw 'project attribute arguments were not canonicalized in manifest'
}
if ($attrManifest1 -notmatch '(?m)^R\tAttrRecord\tacme\.layout\t' -or
    $attrManifest1 -notmatch '(?m)^R\tAttrRecord\.value\tacme\.base\t') {
    throw 'project attribute schema did not validate and export type/field facts'
}
if ($attrManifest1 -notmatch '(?m)^R\tAttrOtherRecord\.value\tacme\.base\t') {
    throw 'same-named fields lost their owner identity in Effect facts'
}
$attrSchema1 = [regex]::Match($attrManifest1, '(?m)^D\tschema\teffect\.schemas\.v1\t(-?\d+)').Groups[1].Value
if ($attrSchema1.Length -eq 0) { throw 'project attribute manifest omitted schema dependency digest' }

# A second build exercises the object cache with the same aggregate schemas.
$attrBuild2 = Invoke-Vyx @('build', '--target', 'attribute_project', '-j2') $attrProjectDir
Write-Utf8NoBom (Join-Path $attrProjectDir 'build-hot.log') $attrBuild2.Text
if ($attrBuild2.Exit -ne 0) { throw "project attribute hot build failed (exit $($attrBuild2.Exit))" }

# Changing a selected `.attr` file is a semantic input.  The manifest digest
# and cache key must change even though the Vyx source and generated executable
# are otherwise identical.
$commonAttr = Join-Path $attrProjectDir 'attrs/common.attr'
$commonText = Get-Content -LiteralPath $commonAttr -Raw
Write-Utf8NoBom $commonAttr ($commonText.Replace('align: i32 = 0;', 'align: i32 = 1;'))
$attrBuild3 = Invoke-Vyx @('build', '--target', 'attribute_project', '-j2') $attrProjectDir
Write-Utf8NoBom (Join-Path $attrProjectDir 'build-schema-change.log') $attrBuild3.Text
if ($attrBuild3.Exit -ne 0) { throw "project attribute schema-change build failed (exit $($attrBuild3.Exit))" }
$attrManifest2 = Get-Content -LiteralPath $attrManifest -Raw
$attrSchema2 = [regex]::Match($attrManifest2, '(?m)^D\tschema\teffect\.schemas\.v1\t(-?\d+)').Groups[1].Value
if ($attrSchema2.Length -eq 0 -or $attrSchema1 -eq $attrSchema2) {
    throw "schema change did not invalidate manifest digest: before=$attrSchema1 after=$attrSchema2"
}

# A conflicting inherited parameter is rejected before source compilation.
$conflictAttr = Join-Path $attrProjectDir 'attrs/conflict.attr'
$tomlPath = Join-Path $attrProjectDir 'Vyx.toml'
$tomlText = Get-Content -LiteralPath $tomlPath -Raw
Write-Utf8NoBom $tomlPath ($tomlText.Replace('"attrs/common.attr"]', '"attrs/common.attr", "attrs/conflict.attr"]'))
$attrConflict = Invoke-Vyx @('build', '--target', 'attribute_project', '-j2') $attrProjectDir
Write-Utf8NoBom (Join-Path $attrProjectDir 'build-conflict.log') $attrConflict.Text
if ($attrConflict.Exit -eq 0 -or $attrConflict.Text -notmatch '(?i)(conflict|duplicate|inherit|schema)') {
    throw "conflicting inherited attribute schema was accepted: exit=$($attrConflict.Exit)"
}

# The source DSL deliberately reuses Vyx's sized scalar names.  Bare `int`
# and unknown schema fields must fail as input diagnostics rather than being
# silently widened or ignored.
$invalidTypeDir = Join-Path $outRoot 'attribute-invalid-type'
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'fixtures/attribute_project') `
    -Destination $invalidTypeDir -Recurse
$invalidTypeAttr = Join-Path $invalidTypeDir 'attrs/common.attr'
$invalidTypeText = Get-Content -LiteralPath $invalidTypeAttr -Raw
Write-Utf8NoBom $invalidTypeAttr ($invalidTypeText.Replace('align: i32 = 0;', 'align: int = 0;'))
$invalidTypeBuild = Invoke-Vyx @('build', '--target', 'attribute_project', '-j1') $invalidTypeDir
Write-Utf8NoBom (Join-Path $invalidTypeDir 'build-invalid-type.log') $invalidTypeBuild.Text
if ($invalidTypeBuild.Exit -eq 0 -or
    $invalidTypeBuild.Text -notmatch 'E0001.*schema rejected: type') {
    throw "bare int schema type was accepted: exit=$($invalidTypeBuild.Exit)"
}

$invalidFieldDir = Join-Path $outRoot 'attribute-invalid-field'
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'fixtures/attribute_project') `
    -Destination $invalidFieldDir -Recurse
$invalidFieldAttr = Join-Path $invalidFieldDir 'acme.attr'
$invalidFieldText = Get-Content -LiteralPath $invalidFieldAttr -Raw
Write-Utf8NoBom $invalidFieldAttr ($invalidFieldText.Replace('version: 1;', "version: 1;`n    unknown: true;"))
$invalidFieldBuild = Invoke-Vyx @('build', '--target', 'attribute_project', '-j1') $invalidFieldDir
Write-Utf8NoBom (Join-Path $invalidFieldDir 'build-invalid-field.log') $invalidFieldBuild.Text
if ($invalidFieldBuild.Exit -eq 0 -or
    $invalidFieldBuild.Text -notmatch 'E0001.*schema rejected: field') {
    throw "unknown schema field was accepted: exit=$($invalidFieldBuild.Exit)"
}
Clear-EffectManifestEnv

# Explicit schemas may live outside the package. A warm build must revalidate
# them after edits instead of reusing the previously accepted object.
$externalRoot = Join-Path $outRoot 'attribute-external'
New-Item -ItemType Directory -Force -Path $externalRoot | Out-Null
$externalDir = Join-Path $externalRoot 'project'
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'fixtures/attribute_project') -Destination $externalDir -Recurse
$externalSchema = Join-Path $externalRoot 'shared.attr'
Copy-Item -LiteralPath (Join-Path $externalDir 'attrs/common.attr') -Destination $externalSchema
$externalToml = Join-Path $externalDir 'Vyx.toml'
Write-Utf8NoBom $externalToml ((Get-Content -LiteralPath $externalToml -Raw).Replace('attrs/common.attr', '../shared.attr'))
$externalCold = Invoke-Vyx @('build', '--target', 'attribute_project', '-j1') $externalDir
Write-Utf8NoBom (Join-Path $externalDir 'cold.log') $externalCold.Text
if ($externalCold.Exit -ne 0) { throw "external schema cold build failed: $($externalCold.Exit)" }
$externalRun = Invoke-Capture (Join-Path $externalDir 'target/attribute_project.exe') @() $externalDir
if ($externalRun.Exit -ne 0) { throw "external schema AOT run failed: $($externalRun.Exit)" }
Write-Utf8NoBom $externalSchema ((Get-Content -LiteralPath $externalSchema -Raw).Replace('align: i32 = 0;', 'align: int = 0;'))
$externalChanged = Invoke-Vyx @('build', '--target', 'attribute_project', '-j1') $externalDir
Write-Utf8NoBom (Join-Path $externalDir 'changed.log') $externalChanged.Text
if ($externalChanged.Exit -eq 0 -or $externalChanged.Text -notmatch 'E0001.*schema rejected: type') {
    throw "warm build bypassed changed external schema: $($externalChanged.Exit)"
}

# Disabled discovery uses only the manifest list, even with malformed root
# metadata present. The ignored file must not contaminate the source registry.
$explicitDir = Join-Path $outRoot 'attribute-explicit-only'
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'fixtures/attribute_project') -Destination $explicitDir -Recurse
$explicitToml = Join-Path $explicitDir 'Vyx.toml'
$explicitText = (Get-Content -LiteralPath $explicitToml -Raw).Replace('[effect]', "[effect]`nauto_discover = false")
Write-Utf8NoBom $explicitToml ($explicitText.Replace('"attrs/common.attr"]', '"attrs/common.attr", "acme.attr"]'))
Write-Utf8NoBom (Join-Path $explicitDir 'ignored.attr') 'this is deliberately invalid and must not be loaded'
$explicitBuild = Invoke-Vyx @('build', '--target', 'attribute_project', '-j1') $explicitDir
Write-Utf8NoBom (Join-Path $explicitDir 'compile.log') $explicitBuild.Text
if ($explicitBuild.Exit -ne 0) { throw "auto_discover=false loaded unselected schema: $($explicitBuild.Exit)" }
$explicitRun = Invoke-Capture (Join-Path $explicitDir 'target/attribute_project.exe') @() $explicitDir
if ($explicitRun.Exit -ne 0) { throw "explicit-only schema AOT run failed: $($explicitRun.Exit)" }

# A legacy generated table and project schemas can be consumed together;
# duplicate identity across the two inputs must be rejected.
$compatDir = Join-Path $outRoot 'attribute-legacy-merge'
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'fixtures/attribute_project') -Destination $compatDir -Recurse
$compatSchema = Join-Path $compatDir 'legacy.schemas'
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'fixtures/declarative_schemas.txt') -Destination $compatSchema
$compatMain = Join-Path $compatDir 'main.vyx'
$compatSource = @'
@[storage.cache(level="abi", retain=true)]
fn attr_legacy() -> i32 { return 9; }
'@
Write-Utf8NoBom $compatMain ($compatSource + "`n" + (Get-Content -LiteralPath $compatMain -Raw))
$compatManifest = Join-Path $compatDir 'target/compat.effect.manifest'
$env:VYX_EFFECT_SCHEMAS_IN = $compatSchema
$env:VYX_EFFECT_MANIFEST_OUT = $compatManifest
$compatBuild = Invoke-Vyx @('build', '--target', 'attribute_project', '-j1') $compatDir
Write-Utf8NoBom (Join-Path $compatDir 'compile.log') $compatBuild.Text
if ($compatBuild.Exit -ne 0) { throw "legacy table/schema merge failed: $($compatBuild.Exit)" }
$compatWire = Get-Content -LiteralPath $compatManifest -Raw
if ($compatWire -notmatch '(?m)^R\tattr_legacy\tstorage\.cache\t' -or
    $compatWire -notmatch '(?m)^R\tattr_kept\tacme\.layout\t') { throw 'legacy merge omitted one schema origin' }
$compatRun = Invoke-Capture (Join-Path $compatDir 'target/attribute_project.exe') @() $compatDir
if ($compatRun.Exit -ne 0) { throw "legacy merge AOT run failed: $($compatRun.Exit)" }
Write-Utf8NoBom $compatSchema "MOSP-EFFECT-SCHEMAS`t1`nS`tacme.base`t1`tfunction|type|field`talign:i32:0`trecord`n"
$compatConflict = Invoke-Vyx @('build', '--target', 'attribute_project', '-j1') $compatDir
Write-Utf8NoBom (Join-Path $compatDir 'conflict.log') $compatConflict.Text
if ($compatConflict.Exit -eq 0 -or $compatConflict.Text -notmatch 'duplicate:acme.base') { throw 'legacy/source identity conflict was accepted' }
Clear-EffectManifestEnv

$summary = [ordered]@{
    status = 'pass'
    compiler_path = $compilerPath
    compiler_sha256 = $compilerHash
    compiler_version = $versionProbe.Trim()
    target = if ($env:VYX_TARGET) { $env:VYX_TARGET } else { 'host-default' }
    abi = 'vyx.aot.v1; compiler Effect manifest boundary exercised'
    custom_attr = [ordered]@{ status='pass'; evidence=$effectRow; run_exit=$customRun.Exit }
    retain_lowering = [ordered]@{ status='pass'; retained_functions=$retainCount; control_functions=$retainControlCount; run_exit=$retainRun.Exit; invalid_args=[ordered]@{ status='expected_fail'; exit=$retainInvalid.Exit; reason='EFFECT-ARGS' }; evidence='accepted dce.retain result -> HIR codegen_root; invalid arguments fail closed' }
    async_lowering = [ordered]@{ status='pass'; mir_yield='term=7'; run_exit=$asyncRun.Exit; invalid_args=[ordered]@{ status='expected_fail'; exit=$asyncInvalid.Exit; reason='EFFECT-ARGS' }; evidence='accepted async.lower result -> HIR async task flag and MIR await split' }
    stdlib_effect = [ordered]@{ status='pass'; summary=$stdlibEffect; manifest=$stdlibManifest; run_exit=$stdlibRun.Exit; evidence='real std.vio package import + platform.select fact + native AOT execution' }
    dci_import_boundary = [ordered]@{ status='pass'; positive=[ordered]@{ status='pass'; exit=$dciPositive.Exit; evidence='source dci_import -> authorized Effect marker -> automatic .dcib lifecycle binding' }; invalid_target=[ordered]@{ status='expected_fail'; exit=$dciNegative.Exit; reason='EFFECT-TARGET'; evidence='raw path discovery cannot bypass Effect target validation' } }
    dci_export_boundary = [ordered]@{ status='pass'; sidecar=$dciExportOut; build_exit=$dciExport.Exit; run_exit=$dciExportProcess.ExitCode; invalid_selector=[ordered]@{ status='expected_fail'; exit=$dciExportInvalid.Exit; reason='no sealed dci_export Effect' }; provenance_inspect=$dciExportInspect.Trim(); provenance_mutation_gate=$dciArtifactProvenance.Trim(); evidence='validated dci_export marker -> AOT root + canonical provenance-checked sidecar' }
    platform_select = [ordered]@{ status='pass'; run_exit=$platformRun.Exit; evidence='accepted platform.select result -> declaration filtering before semantic resolution' }
    semantic_effects = [ordered]@{ status='pass'; derive_run_exit=$deriveRun.Exit; comptime_run_exit=$comptimeRun.Exit; evidence='derive.expand -> trait synthesis; comptime.evaluate -> constant folding' }
    manifest_codec = [ordered]@{ status='pass'; evidence=$requiredTokens; run_exit=$manifestRun.Exit }
    effect_dependency_graph = [ordered]@{
        status='pass'
        build_exit=$cycleBuild.Exit
        run_exit=$cycleRun.Exit
        evidence=@('cross-module edge rows round-trip', 'deterministic DFS cycle diagnostic', 'cycle insertion rejected before seal')
    }
    dci_effect_obligation_graph = [ordered]@{
        status='pass'
        producer=$dciManifest
        consumer_manifest=$dciBridgeManifestOut
        build_exit=$dciBridgeBuild.Exit
        run_exit=$dciBridgeRun.Exit
        evidence=@('lifecycle and exception records imported into EffectPlan', 'cleanup-before-propagate edge preserved', 'consumer AOT passed')
    }
    effect_declaration_syntax = [ordered]@{
        status='pass'
        build_exit=$syntaxBuild.Exit
        run_exit=$syntaxRun.Exit
        invalid=[ordered]@{ status='expected_fail'; exit=$syntaxInvalid.Exit; reason='duplicate/unknown effect field' }
        manifest=$syntaxManifest
        evidence='contextual top-level effect declaration -> EffectPlan fact + manifest row; no HIR/MIR item or runtime symbol'
    }
    attribute_schema_project = [ordered]@{
        status='pass'
        cold_build_exit=$attrBuild1.Exit
        hot_build_exit=$attrBuild2.Exit
        schema_change_build_exit=$attrBuild3.Exit
        schema_digest_before=$attrSchema1
        schema_digest_after=$attrSchema2
        conflict=[ordered]@{ status='expected_fail'; exit=$attrConflict.Exit }
        invalid_type=[ordered]@{ status='expected_fail'; exit=$invalidTypeBuild.Exit; reason='Vyx scalar type required' }
        invalid_field=[ordered]@{ status='expected_fail'; exit=$invalidFieldBuild.Exit; reason='unknown schema field' }
        source_api=[ordered]@{ status='pass'; run_exit=$attrApiRun.Exit; evidence='scalar bounds, forward extends, cycles, conflicts, required fields, block/string boundaries' }
        external_cache=[ordered]@{ status='pass'; cold_exit=$externalCold.Exit; changed=[ordered]@{ status='expected_fail'; exit=$externalChanged.Exit }; evidence='selected schema outside package invalidates warm object cache' }
        explicit_only=[ordered]@{ status='pass'; run_exit=$explicitRun.Exit; evidence='auto_discover=false rejects no unselected input and keeps schema files out of source inventory' }
        legacy_merge=[ordered]@{ status='pass'; run_exit=$compatRun.Exit; conflict_exit=$compatConflict.Exit; evidence='legacy table merged with package schemas; duplicate identity rejected' }
        evidence='root .attr auto-discovery + Vyx.toml attr_files + multiple blocks + i32 + extends + cache/schema invalidation'
    }
    build_interface_policy = [ordered]@{
        status='pass'
        evidence='stub and emit_vyi=false stay disabled on cold/warm builds; enabled interface regenerated byte-identically after deletion'
        enabled_vyi_sha256=$enabledVyiHash
    }
    manifest_cli_cross_module = [ordered]@{
        status='pass'
        producer_manifest=$manifestOut
        accepted=[ordered]@{ status='pass'; run_exit=$acceptedRun.Exit; evidence='effect-manifest accepted + imported manifest records=' + $acceptedRecordCount; effect_summary=$acceptedEffect; manifest=$acceptedManifestOut }
        source_stale=$sourceStale
        compiler_stale=$compilerStale
        schema_stale=$schemaStale
    }
    output_dir = $outRoot
}
Write-Utf8NoBom (Join-Path $outRoot 'result.json') ($summary | ConvertTo-Json -Depth 8)
Write-Output "mosp-effect: PASS custom attr + .attr project discovery + manifest boundary + DCI import/export authorization + lowering consumers"
Write-Output "summary=$outRoot/result.json"
} finally {
    foreach ($name in $gateEnvNames) {
        if ($null -eq $savedGateEnv[$name]) {
            Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue
        } else {
            [Environment]::SetEnvironmentVariable($name, $savedGateEnv[$name], 'Process')
        }
    }
}
