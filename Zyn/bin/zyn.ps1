param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ZynArgs
)

$ErrorActionPreference = "Stop"

$env:SEM_NOGPFAULTERRORBOX = "1"
if (-not ("Win32.Sem" -as [type])) {
    try {
        Add-Type -Namespace Win32 -Name Sem -MemberDefinition '[DllImport("kernel32.dll")] public static extern uint SetErrorMode(uint mode);' -ErrorAction SilentlyContinue
    } catch {}
}
try { [Win32.Sem]::SetErrorMode(0x0001 -bor 0x0002 -bor 0x8000) | Out-Null } catch {}

$scriptDir = $PSScriptRoot
$sdkRoot = Split-Path -Parent $scriptDir
$repoRoot = Split-Path -Parent $sdkRoot

function Resolve-ZynFile {
    param([string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path)) { return "" }
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        return (Resolve-Path -LiteralPath $Path).Path
    }
    return ""
}

$compiler = ""
if (-not [string]::IsNullOrWhiteSpace($env:ZYN_VYXC)) {
    $compiler = Resolve-ZynFile $env:ZYN_VYXC
}

if ([string]::IsNullOrWhiteSpace($compiler)) {
    $candidates = @(
        (Join-Path $scriptDir "vyxc.exe"),
        (Join-Path $scriptDir "vyxc"),
        (Join-Path $scriptDir "cache\vyxc.exe"),
        (Join-Path $scriptDir "cache\vyxc"),
        (Join-Path $sdkRoot "toolchain\vyxc.exe"),
        (Join-Path $sdkRoot "toolchain\vyxc"),
        (Join-Path $sdkRoot "out\vyxc.exe"),
        (Join-Path $sdkRoot "out\vyxc"),
        (Join-Path $repoRoot "bootstrap_compiler\out\vyxc.exe"),
        (Join-Path $repoRoot "bootstrap_compiler\out\vyxc")
    )
    foreach ($candidate in $candidates) {
        $hit = Resolve-ZynFile $candidate
        if (-not [string]::IsNullOrWhiteSpace($hit)) {
            $compiler = $hit
            break
        }
    }
}

if ([string]::IsNullOrWhiteSpace($compiler)) {
    Write-Error "zyn: could not find an SDK-local vyxc. Place vyxc under Zyn/bin, Zyn/toolchain, or build the repo-local self-host compiler output."
    exit 1
}

$clangRoot = Join-Path $repoRoot "clang"
if ([string]::IsNullOrWhiteSpace($env:LLVM_ROOT) -and (Test-Path -LiteralPath $clangRoot -PathType Container)) {
    $env:LLVM_ROOT = $clangRoot
}

$env:ZYN_SDK_ROOT = (Resolve-Path -LiteralPath $sdkRoot).Path
$env:VYX_COMPILER = $compiler

$cli = Join-Path $env:ZYN_SDK_ROOT "tools\zyn_cli.vyx"
if (-not (Test-Path -LiteralPath $cli -PathType Leaf)) {
    Write-Error "zyn: missing CLI source: $cli"
    exit 2
}

# A full SDK build can fan out to every hardware thread.  Do not keep the
# AOT-compiled CLI process alive around that build: on Windows this needlessly
# nests two compiler runtimes and used to make highly parallel cold builds
# unstable.  Run the compiler as the top-level process, then invoke the CLI
# only for the cheap runtime-asset staging step.
if ($ZynArgs.Count -gt 0 -and $ZynArgs[0] -eq "build") {
    $buildArgs = @()
    if ($ZynArgs.Count -gt 1) {
        $buildArgs = $ZynArgs[1..($ZynArgs.Count - 1)]
    }

    $androidBuild = $false
    for ($index = 0; $index -lt $buildArgs.Count; $index++) {
        $valueIndex = -1
        $value = ''
        if ($buildArgs[$index] -in '--target','--triplet' -and $index + 1 -lt $buildArgs.Count) {
            $valueIndex = $index + 1
            $value = $buildArgs[$valueIndex]
        } elseif ($buildArgs[$index].StartsWith('--target=') -or $buildArgs[$index].StartsWith('--triplet=')) {
            $value = ($buildArgs[$index] -split '=', 2)[1]
        }
        $triple = switch ($value) {
            'android-arm64' { 'aarch64-linux-android29' }
            'android-x64' { 'x86_64-linux-android29' }
            default { $value }
        }
        if ($triple -match '^(aarch64|x86_64)-.*android') {
            $androidBuild = $true
            $env:ZYN_ANDROID_ABI = if ($Matches[1] -eq 'aarch64') { 'arm64-v8a' } else { 'x86_64' }
            if ($valueIndex -ge 0) {
                $buildArgs[$index] = '--triplet'
                $buildArgs[$valueIndex] = $triple
            } else { $buildArgs[$index] = '--triplet=' + $triple }
        }
    }

    & $compiler "build" @buildArgs
    $buildExitCode = $LASTEXITCODE
    if ($buildExitCode -ne 0) {
        exit $buildExitCode
    }
    if ($androidBuild) {
        Write-Output 'zyn: Android native build complete; use publish --target android-arm64 (or android-x64) --debug to package an APK.'
        exit 0
    }

    & $compiler "--src=file" $cli "--run=aot" "--" "stage"
    exit $LASTEXITCODE
}

& $compiler "--src=file" $cli "--run=aot" "--" @ZynArgs
exit $LASTEXITCODE
