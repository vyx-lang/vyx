param(
    [string]$Compiler = "",
    [switch]$KeepArtifacts
)

$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $MyInvocation.MyCommand.Path
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
            Remove-Item -LiteralPath $path -Recurse -Force
        }
    }
    Get-ChildItem -LiteralPath (Join-Path $project 'deps') -Directory | ForEach-Object {
        foreach ($name in @('.cache', 'target', 'out')) {
            $path = Join-Path $_.FullName $name
            if (Test-Path -LiteralPath $path) {
                Remove-Item -LiteralPath $path -Recurse -Force
            }
        }
    }
}

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
    $cppOut = Join-Path $project '.cpp_export'
    & $Compiler --src=project $project --emit=cpp -o $cppOut
    if ($LASTEXITCODE -ne 0) { throw "direct MIR2CPP export failed: $LASTEXITCODE" }
    $cmake = Get-Content -Raw -LiteralPath (Join-Path $cppOut 'CMakeLists.txt')
    if ($cmake.Contains('template_link_must_be_removed') -or
        $cmake.Contains('template_path_must_be_removed')) {
        throw 'direct MIR2CPP path ignored template link removals'
    }

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
