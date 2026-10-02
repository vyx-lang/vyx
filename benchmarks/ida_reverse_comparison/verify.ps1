param(
    [string]$OutDir = "",
    [string]$LlvmReadObj = "llvm-readobj",
    [string]$LlvmPdbUtil = "llvm-pdbutil"
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $OutDir = Join-Path $root "out"
}
$OutDir = [System.IO.Path]::GetFullPath($OutDir)

$strippedPrograms = [ordered]@{
    vyx  = Join-Path $OutDir "vyx.exe"
    cpp  = Join-Path $OutDir "cpp.exe"
    rust = Join-Path $OutDir "rust.exe"
}
$symbolPrograms = [ordered]@{
    vyx  = Join-Path $OutDir "vyx.symbols.exe"
    cpp  = Join-Path $OutDir "cpp.symbols.exe"
    rust = Join-Path $OutDir "rust.symbols.exe"
}
$symbolDatabases = [ordered]@{
    vyx  = Join-Path $OutDir "vyx.symbols.pdb"
    cpp  = Join-Path $OutDir "cpp.symbols.pdb"
    rust = Join-Path $OutDir "rust.symbols.pdb"
}

$cases = @(
    [pscustomobject]@{
        Name = "accepted"
        Token = "00112233445566778899aabbccddeeff102132435465768798a9babcbddcedfe"
        ExitCode = 0
        Output = "ACCEPT 6317825462749680818"
    },
    [pscustomobject]@{
        Name = "all-zero"
        Token = "0000000000000000000000000000000000000000000000000000000000000000"
        ExitCode = 1
        Output = "REJECT 14776973174501940746"
    },
    [pscustomobject]@{
        Name = "all-ff"
        Token = "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
        ExitCode = 1
        Output = "REJECT 11938257067672613651"
    },
    [pscustomobject]@{
        Name = "uppercase"
        Token = "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF"
        ExitCode = 1
        Output = "REJECT 15150610185237785609"
    },
    [pscustomobject]@{
        Name = "short"
        Token = "0011"
        ExitCode = 2
        Output = "INVALID"
    },
    [pscustomobject]@{
        Name = "non-hex"
        Token = "g0112233445566778899aabbccddeeff102132435465768798a9babcbddcedfe"
        ExitCode = 2
        Output = "INVALID"
    }
)

function Invoke-Probe([string]$FilePath, [string]$Token) {
    $start = [System.Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $FilePath
    $start.Arguments = $Token
    $start.WorkingDirectory = Split-Path -Parent $FilePath
    $start.UseShellExecute = $false
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $start
    if (-not $process.Start()) {
        throw "Failed to start $FilePath"
    }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $process.WaitForExit()
    return [pscustomobject]@{
        ExitCode = $process.ExitCode
        Stdout = $stdoutTask.GetAwaiter().GetResult().Trim()
        Stderr = $stderrTask.GetAwaiter().GetResult().Trim()
    }
}

$requiredArtifacts = @(
    $strippedPrograms.GetEnumerator()
    $symbolPrograms.GetEnumerator()
    $symbolDatabases.GetEnumerator()
)
$missing = @($requiredArtifacts | Where-Object {
    -not (Test-Path -LiteralPath $_.Value -PathType Leaf)
})
if ($missing.Count -gt 0) {
    throw "Missing IDA comparison artifacts: $(($missing.Value) -join ', ')"
}

$readObjCommand = Get-Command $LlvmReadObj -ErrorAction Stop
$pdbUtilCommand = Get-Command $LlvmPdbUtil -ErrorAction Stop
foreach ($program in @($strippedPrograms.GetEnumerator()) + @($symbolPrograms.GetEnumerator())) {
    $headers = & $readObjCommand.Source --file-headers $program.Value 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
        throw "llvm-readobj failed for $($program.Name): $headers"
    }
    if ($headers -notmatch "IMAGE_FILE_MACHINE_AMD64") {
        throw "$($program.Name) is not a Windows x86_64 PE image"
    }
}

foreach ($program in $strippedPrograms.GetEnumerator()) {
    $debugDirectory = & $readObjCommand.Source --coff-debug-directory $program.Value 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
        throw "debug-directory inspection failed for $($program.Name): $debugDirectory"
    }
    if ($debugDirectory -match "Type: CodeView") {
        throw "$($program.Name) stripped PE still references a PDB"
    }

    $symbols = & $readObjCommand.Source --symbols $program.Value 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
        throw "symbol inspection failed for $($program.Name): $symbols"
    }
    if ($symbols -match "parse_hex|arx_mix|state_walk|verify_token") {
        throw "$($program.Name) still exposes a comparison-kernel symbol"
    }
}

foreach ($program in $symbolPrograms.GetEnumerator()) {
    $debugDirectory = & $readObjCommand.Source --coff-debug-directory $program.Value 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
        throw "debug-directory inspection failed for $($program.Name): $debugDirectory"
    }
    if ($debugDirectory -notmatch "Type: CodeView") {
        throw "$($program.Name) symbolized PE has no CodeView/PDB reference"
    }
}

foreach ($database in $symbolDatabases.GetEnumerator()) {
    $pdbSymbols = & $pdbUtilCommand.Source dump -symbols $database.Value 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
        throw "PDB inspection failed for $($database.Name): $pdbSymbols"
    }
    foreach ($kernel in @("parse_hex", "arx_mix", "state_walk", "verify_token")) {
        if ($pdbSymbols -notmatch $kernel) {
            throw "$($database.Name) PDB does not contain $kernel"
        }
    }
}

foreach ($case in $cases) {
    $baseline = $null
    $allPrograms = @($strippedPrograms.GetEnumerator()) + @($symbolPrograms.GetEnumerator())
    foreach ($program in $allPrograms) {
        $result = Invoke-Probe $program.Value $case.Token
        if ($result.ExitCode -ne $case.ExitCode) {
            throw "$($program.Name)/$($case.Name): exit $($result.ExitCode), expected $($case.ExitCode); stderr=$($result.Stderr)"
        }
        if ($result.Stdout -cne $case.Output) {
            throw "$($program.Name)/$($case.Name): stdout '$($result.Stdout)', expected '$($case.Output)'"
        }
        $fingerprint = "$($result.ExitCode)`n$($result.Stdout)"
        if ($null -eq $baseline) {
            $baseline = $fingerprint
        } elseif ($fingerprint -cne $baseline) {
            throw "$($case.Name): behavior differs between languages"
        }
    }
}

Write-Host "ida_reverse_comparison: PASS (3 symbolized + 3 stripped PE files, 3 PDB files, $($cases.Count) behavior cases)"
