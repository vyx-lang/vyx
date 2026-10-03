# P0/P1 acceptance for the CGU parallel work.
#
# Object bytes must not depend on thread timing, so every parallelism mode has
# to produce the same output for the same input:
#   - `VYX_CGU_PARALLEL=off`  pins emission to the main thread (reference build)
#   - `VYX_CGU_PARALLEL=lazy` hands finished modules to the emit pool
#   - `VYX_CGU_PARALLEL=full` reserved for per-CGU parallel lowering
# Run the same project under each mode and diff the SHA-256 of every emitted
# object plus the `.cgu.list` manifest.  A project compiled several times in the
# same mode must also reproduce itself.
#
# One more run enables `VYX_CGU_PHASES=1`, which drives the same code through
# the split materialise/optimise/lower/release seam instead of the fused path.
# Its objects must match too, and its log must actually contain the phase
# summary -- otherwise the seam would be untested code that merely compiles.
#
# Two more runs pin the emit pool to 1 and to 16 threads.  Pool sizing is
# machine-derived by default (`-Z threads=N` / `--threads=N` / `VYX_CGU_THREADS`
# override it), so the check has to prove both that the request is honoured and
# that the thread count does not leak into the object bytes.
#
# Each run gets its own copy of the sources so no cache directory has to be
# deleted between modes -- the cache is content addressed, and a warm cache
# would make the comparison vacuous.
#
# The linked executable is deliberately not compared: linking may embed
# timestamps, and the multi-CGU async runtime has a known pre-existing
# exit-time heap corruption unrelated to emission.
[CmdletBinding()]
param(
    [string]$BootstrapCompiler = "",
    [string]$Project = "",
    [int]$Reps = 3,
    [string]$OutDir = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$scriptDir = $PSScriptRoot
$testsRoot = (Resolve-Path -LiteralPath (Join-Path $scriptDir "..\..")).Path
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $testsRoot "..")).Path

if ([string]::IsNullOrWhiteSpace($BootstrapCompiler)) {
    $BootstrapCompiler = Join-Path $repoRoot "bootstrap_compiler\out\boot.exe"
}
if ([string]::IsNullOrWhiteSpace($Project)) {
    $Project = Join-Path $testsRoot "projects\industrial_mir_stress"
}
if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $OutDir = Join-Path $repoRoot "tests\.cache\cgu_parallel_determinism"
}

if (!(Test-Path -LiteralPath $BootstrapCompiler)) { throw "compiler not found: $BootstrapCompiler" }
if (!(Test-Path -LiteralPath $Project)) { throw "project not found: $Project" }
$Compiler = (Resolve-Path -LiteralPath $BootstrapCompiler).Path
$Project = (Resolve-Path -LiteralPath $Project).Path

$stamp = (Get-Date).ToString("yyyyMMdd-HHmmss")
$OutDir = Join-Path $OutDir $stamp
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null

# Emission artefacts: the crate object, its per-CGU companions and the manifest
# that names them.  Nothing else under the cache is this check's business.
function Get-ArtifactHashes([string]$ProjectDir) {
    $map = @{}
    foreach ($sub in @(".cache", "target")) {
        $root = Join-Path $ProjectDir $sub
        if (!(Test-Path -LiteralPath $root)) { continue }
        $files = Get-ChildItem -LiteralPath $root -Recurse -File -ErrorAction SilentlyContinue |
                 Where-Object { $_.Name -like "*.obj" -or $_.Name -like "*.obj.cgu.*" -or $_.Name -like "*.cgu.list" }
        foreach ($f in $files) {
            $rel = $f.FullName.Substring($ProjectDir.Length).TrimStart('\', '/')
            $map[$rel] = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash
        }
    }
    return $map
}

function Invoke-CguBuild([string]$Label, [string]$Mode, [string]$Phases = "", [string]$Retain = "", [string]$Threads = "") {
    $runRoot = Join-Path $OutDir ("proj_" + $Label)
    New-Item -ItemType Directory -Path $runRoot -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $Project "Vyx.toml") -Destination $runRoot -Force
    Copy-Item -LiteralPath (Join-Path $Project "src") -Destination $runRoot -Recurse -Force

    $log = Join-Path $OutDir ("build_" + $Label + ".log")
    $oldMode = $env:VYX_CGU_PARALLEL
    $oldSummary = $env:VYX_PHASE_SUMMARY
    $oldPhases = $env:VYX_CGU_PHASES
    $oldRetain = $env:VYX_CGU_RETAIN
    $oldThreads = $env:VYX_CGU_THREADS
    $env:VYX_CGU_PARALLEL = $Mode
    $env:VYX_PHASE_SUMMARY = "1"
    if (![string]::IsNullOrEmpty($Phases)) { $env:VYX_CGU_PHASES = $Phases } else { $env:VYX_CGU_PHASES = $null }
    if (![string]::IsNullOrEmpty($Retain)) { $env:VYX_CGU_RETAIN = $Retain } else { $env:VYX_CGU_RETAIN = $null }
    if (![string]::IsNullOrEmpty($Threads)) { $env:VYX_CGU_THREADS = $Threads } else { $env:VYX_CGU_THREADS = $null }
    try {
        $proc = Start-Process -FilePath $Compiler -ArgumentList "build" `
            -WorkingDirectory $runRoot -NoNewWindow -Wait -PassThru `
            -RedirectStandardOutput $log -RedirectStandardError ($log + ".err")
        $code = $proc.ExitCode
    } finally {
        $env:VYX_CGU_PARALLEL = $oldMode
        $env:VYX_PHASE_SUMMARY = $oldSummary
        $env:VYX_CGU_PHASES = $oldPhases
        $env:VYX_CGU_RETAIN = $oldRetain
        $env:VYX_CGU_THREADS = $oldThreads
    }
    $hashes = Get-ArtifactHashes $runRoot
    $summary = @()
    if (Test-Path -LiteralPath $log) {
        $summary = @(Select-String -LiteralPath $log -Pattern '\[llvm-cgu\] (parallel=|phases |retain-bodies=|units=|task |budget )' -ErrorAction SilentlyContinue |
                     ForEach-Object { $_.Line.Trim() })
    }
    return [pscustomobject]@{ Label = $Label; Mode = $Mode; Phases = $Phases; Retain = $Retain; Threads = $Threads; ExitCode = $code; Hashes = $hashes; Summary = $summary }
}

$plan = @([pscustomobject]@{ Label = "off"; Mode = "off"; Phases = ""; Retain = ""; Threads = "" })
for ($i = 1; $i -le [Math]::Max($Reps, 1); $i++) {
    $plan += [pscustomobject]@{ Label = ("lazy" + $i); Mode = "lazy"; Phases = ""; Retain = ""; Threads = "" }
}
$plan += [pscustomobject]@{ Label = "full"; Mode = "full"; Phases = ""; Retain = ""; Threads = "" }
$plan += [pscustomobject]@{ Label = "phases"; Mode = "lazy"; Phases = "1"; Retain = ""; Threads = "" }
# Pool sizing must not change the bytes either: pin it to 1 (serial emission)
# and to the unit count, and check the compiler actually honoured the request.
$plan += [pscustomobject]@{ Label = "threads1"; Mode = "lazy"; Phases = ""; Retain = ""; Threads = "1" }
$plan += [pscustomobject]@{ Label = "threads16"; Mode = "lazy"; Phases = ""; Retain = ""; Threads = "16" }

$runs = @()
foreach ($step in $plan) {
    Write-Host ("[run] " + $step.Label + " (VYX_CGU_PARALLEL=" + $step.Mode + ")") -ForegroundColor Cyan
    $r = Invoke-CguBuild $step.Label $step.Mode $step.Phases $step.Retain $step.Threads
    $runs += $r
    Write-Host ("      exit=" + $r.ExitCode + " artefacts=" + $r.Hashes.Count)
    foreach ($line in $r.Summary) { Write-Host ("      " + $line) -ForegroundColor DarkGray }
}

$failures = @()
$reference = $runs[0]
$referenceLabel = $reference.Label
if ($reference.ExitCode -ne 0) { $failures += ($referenceLabel + ": build exit=" + $reference.ExitCode) }
if ($reference.Hashes.Count -eq 0) { $failures += ($referenceLabel + ": no emission artefacts found") }

$refKeys = @($reference.Hashes.Keys | Sort-Object)
foreach ($r in $runs) {
    if ($r.Label -eq $referenceLabel) { continue }
    if ($r.ExitCode -ne 0) { $failures += ($r.Label + ": build exit=" + $r.ExitCode) }
    if ($r.Hashes.Count -eq 0) { $failures += ($r.Label + ": no emission artefacts found") }
    $curKeys = @($r.Hashes.Keys | Sort-Object)
    if (($refKeys -join "|") -ne ($curKeys -join "|")) {
        $onlyRef = @($refKeys | Where-Object { $curKeys -notcontains $_ })
        $onlyCur = @($curKeys | Where-Object { $refKeys -notcontains $_ })
        if ($onlyRef.Count -gt 0) { $failures += ($r.Label + ": missing vs " + $referenceLabel + " -> " + ($onlyRef -join ", ")) }
        if ($onlyCur.Count -gt 0) { $failures += ($r.Label + ": extra vs " + $referenceLabel + " -> " + ($onlyCur -join ", ")) }
        continue
    }
    foreach ($k in $refKeys) {
        if ($reference.Hashes[$k] -ne $r.Hashes[$k]) {
            $failures += ($r.Label + ": bytes differ -> " + $k)
        }
    }
}

# The phase-split run has to prove it exercised the seam: identical bytes
# alone would also be produced if the flag did nothing at all.
$phasesRun = $runs | Where-Object { $_.Label -eq "phases" } | Select-Object -First 1
if ($null -ne $phasesRun) {
    $phaseLine = @($phasesRun.Summary | Where-Object { $_ -match '\[llvm-cgu\] phases fns=' })
    if ($phaseLine.Count -eq 0) {
        $failures += "phases: VYX_CGU_PHASES=1 produced no '[llvm-cgu] phases' summary"
    } elseif ($phaseLine[0] -notmatch 'fns=[1-9]') {
        $failures += ("phases: phase summary reported no functions -> " + $phaseLine[0])
    }
    if (@($phasesRun.Summary | Where-Object { $_ -match '\[phases=split\]' }).Count -eq 0) {
        $failures += "phases: emit summary did not report [phases=split]"
    }
}

# Pool sizing is now driven by the machine (and overridable with
# `-Z threads=N` / `--threads=N` / `VYX_CGU_THREADS`).  An explicit request has
# to be honoured -- clamped only by `units`: since §8.1 removed the last-unit
# privilege, the main thread is itself a pool participant (labelled where=drain),
# so a units-sized pool can put every unit on a worker.
foreach ($r in $runs) {
    $parLine = @($r.Summary | Where-Object { $_ -match '\[llvm-cgu\] parallel=' }) | Select-Object -First 1
    $unitsLine = @($r.Summary | Where-Object { $_ -match '\[llvm-cgu\] units=' }) | Select-Object -First 1
    if ($parLine -notmatch 'emit-threads=(\d+)') {
        $failures += ($r.Label + ": no 'emit-threads=' in the parallel summary")
        continue
    }
    $reported = [int]$Matches[1]
    if ($unitsLine -match 'units=(\d+)') { $units = [int]$Matches[1] } else { $units = 0 }
    if (![string]::IsNullOrEmpty($r.Threads)) {
        $wanted = [int]$r.Threads
        if ($units -gt 1 -and $wanted -gt $units) { $wanted = $units }
        if ($reported -ne $wanted) {
            $failures += ("threads: " + $r.Label + " asked for " + $wanted + " but emitted with " + $reported)
        }
    } elseif ($r.Mode -ne "off" -and $units -gt 1 -and $reported -lt 2) {
        $failures += ("threads: " + $r.Label + " did not engage the emit pool by default (emit-threads=" + $reported + ")")
    }
}

# `full` is still a reserved mode: it must print the machine-readable fallback
# instead of silently running as lazy.  Bytes matching off/lazy is necessary
# but not sufficient -- the flag has to be observable.
$fullRun = $runs | Where-Object { $_.Label -eq "full" } | Select-Object -First 1
if ($null -ne $fullRun) {
    $fullPar = @($fullRun.Summary | Where-Object { $_ -match '\[llvm-cgu\] parallel=' }) | Select-Object -First 1
    if ($fullPar -notmatch 'parallel=full') {
        $failures += "full: summary did not report parallel=full"
    }
    if ($fullPar -notmatch 'effective=lazy reason=per-unit-arena') {
        $failures += "full: did not advertise the reserved fallback (effective=lazy reason=per-unit-arena)"
    }
}

# Each finished CGU must leave a task-attribution line so later stages can
# prove which worker owned which unit.  Serial `off` is all inline; a pooled
# run with units>1 must include at least one pool line -- the main thread's
# share shows up as where=drain since §8.1 made it a pool participant.
foreach ($r in $runs) {
    $unitsLine = @($r.Summary | Where-Object { $_ -match '\[llvm-cgu\] units=' }) | Select-Object -First 1
    if ($unitsLine -match 'units=(\d+)') { $units = [int]$Matches[1] } else { $units = 0 }
    $taskLines = @($r.Summary | Where-Object { $_ -match '\[llvm-cgu\] task unit=' })
    if ($units -gt 1 -and $taskLines.Count -lt $units) {
        $failures += ($r.Label + ": expected >= " + $units + " task lines, got " + $taskLines.Count)
    }
    if ($units -gt 1 -and $r.Mode -ne "off" -and ![string]::IsNullOrEmpty($r.Threads) -and ([int]$r.Threads -gt 0)) {
        if (@($taskLines | Where-Object { $_ -match 'where=pool' }).Count -eq 0) {
            $failures += ($r.Label + ": pooled run produced no where=pool task line")
        }
    }
}

# Body retention is deliberately off by default (see policy_cgu_retain_enabled).
# Pin that here: if a future change flips the default, this check has to say so
# rather than silently start exercising an unvalidated path.
foreach ($r in $runs) {
    if (@($r.Summary | Where-Object { $_ -match 'retain-bodies=1' }).Count -ne 0) {
        $failures += ("retain: " + $r.Label + " opted into VYX_CGU_RETAIN on the default path")
    }
}

Write-Host ""
Write-Host ("reference: " + $referenceLabel + " (" + $reference.Hashes.Count + " artefacts)") -ForegroundColor White
foreach ($k in $refKeys) {
    Write-Host ("  " + ($reference.Hashes[$k].Substring(0, 16)) + "  " + $k)
}
Write-Host ("logs: " + $OutDir) -ForegroundColor DarkGray

if ($failures.Count -gt 0) {
    Write-Host ""
    foreach ($f in $failures) { Write-Host ("FAIL " + $f) -ForegroundColor Red }
    exit 1
}
Write-Host ""
Write-Host ("PASS: " + $runs.Count + " runs, identical object bytes across off/lazy/full/phases/thread counts") -ForegroundColor Green
exit 0
