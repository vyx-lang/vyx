param(
    [string]$Compiler = "",
    [switch]$KeepArtifacts
)

$ErrorActionPreference = 'Stop'
$fixtureRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$project = $fixtureRoot
$repo = (Resolve-Path (Join-Path $project '..\..\..')).Path
$isWindowsPlatform = $env:OS -eq 'Windows_NT'
$exeSuffix = if ($isWindowsPlatform) { '.exe' } else { '' }
$staticPrefix = if ($isWindowsPlatform) { '' } else { 'lib' }
$staticSuffix = if ($isWindowsPlatform) { '.lib' } else { '.a' }
if ([string]::IsNullOrWhiteSpace($Compiler)) {
    $Compiler = Join-Path $repo 'bootstrap_compiler\out\boot.exe'
}
$Compiler = (Resolve-Path $Compiler).Path

function Remove-GeneratedArtifacts {
    @('.cache', '.cpp_export', 'target', 'out') | ForEach-Object {
        $path = Join-Path $project $_
        if (Test-Path -LiteralPath $path) {
            $resolved = (Resolve-Path -LiteralPath $path).Path
                if (-not $resolved.StartsWith($project + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
                    throw "refusing cleanup outside owned fixture: $resolved"
                }
                Remove-Item -LiteralPath $resolved -Recurse -Force
        }
    }
    Get-ChildItem -LiteralPath (Join-Path $project 'deps') -Directory | ForEach-Object {
        foreach ($name in @('.cache', 'target', 'out')) {
            $path = Join-Path $_.FullName $name
            if (Test-Path -LiteralPath $path) {
                $resolved = (Resolve-Path -LiteralPath $path).Path
                if (-not $resolved.StartsWith($project + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
                    throw "refusing cleanup outside owned fixture: $resolved"
                }
                Remove-Item -LiteralPath $resolved -Recurse -Force
            }
        }
    }
}

# Work in an owned copy, preserving other runs' caches and artifacts.
$runRoot = Join-Path $repo ("tests/.cache/template_run_{0}_{1}" -f $PID, [DateTime]::UtcNow.Ticks)
function Copy-FixtureTree([string]$Source, [string]$Destination) {
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    foreach ($item in Get-ChildItem -LiteralPath $Source -Force) {
        if ($item.Name -in @('.cache', '.cpp_export', 'target', 'out')) { continue }
        $destinationItem = Join-Path $Destination $item.Name
        if ($item.PSIsContainer) { Copy-FixtureTree $item.FullName $destinationItem }
        else { Copy-Item -LiteralPath $item.FullName -Destination $destinationItem }
    }
}
Copy-FixtureTree $fixtureRoot $runRoot
$project = (Resolve-Path -LiteralPath $runRoot).Path

# A prior interrupted run must not turn a stale output directory into evidence
# that the selected manifest target built an unselected dependency.
if (-not $KeepArtifacts) {
    Remove-GeneratedArtifacts
}

function Invoke-Target([string]$Name) {
    Push-Location $project
    try {
        & $Compiler build --target $Name
        if ($LASTEXITCODE -ne 0) { throw "build --target $Name failed: $LASTEXITCODE" }
        $exe = Join-Path $project ("target\{0}{1}" -f $Name, $exeSuffix)
        if (-not (Test-Path -LiteralPath $exe)) { throw "missing output: $exe" }
        & $exe
        if ($LASTEXITCODE -ne 0) { throw "$Name returned $LASTEXITCODE" }
    }
    finally {
        Pop-Location
    }
}

try {
    # A real AOT link proves removed template libraries/search paths do not
    # reach the linker. IR emission exercises the same manifest resolver.
    Invoke-Target manifest_target_templates
    $irOut = Join-Path $project 'template-probe.ll'
    & $Compiler --src=project $project --emit=ir -o $irOut
    if ($LASTEXITCODE -ne 0) { throw "manifest LLVM IR export failed: $LASTEXITCODE" }
    if (-not (Test-Path -LiteralPath $irOut -PathType Leaf)) { throw 'missing template IR output' }

    Invoke-Target app_a
    if (-not (Test-Path -LiteralPath (Join-Path $project ('target\prepare' + $exeSuffix)))) {
        throw 'usage="build" target prepare was not built for app_a'
    }
    if (Test-Path -LiteralPath (Join-Path $project ('target\app_b' + $exeSuffix))) {
        throw 'selecting app_a unexpectedly built app_b'
    }
    $flavorAArtifact = 'deps\flavor_a\out\' + $staticPrefix + 'flavor_a' + $staticSuffix
    if (-not (Test-Path -LiteralPath (Join-Path $project $flavorAArtifact))) {
        throw 'app_a target-level dependency override was not built'
    }
    foreach ($unexpected in @(
        'deps\flavor_default\out',
        'deps\flavor_common\out',
        'deps\removed\out',
        'deps\local_b\out'
    )) {
        if (Test-Path -LiteralPath (Join-Path $project $unexpected)) {
            throw "app_a unexpectedly built $unexpected"
        }
    }
    Invoke-Target app_b
    Write-Host 'manifest target template example passed'
}
finally {
    if (-not $KeepArtifacts) {
        Remove-GeneratedArtifacts
    }
}
