# --src=file must resolve std.* from std_packages/registry, not the flat std/ mirror.
$ErrorActionPreference = "Stop"
$OutputEncoding = [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
[Console]::InputEncoding = [System.Text.Encoding]::UTF8

$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
$Boot = Join-Path $Root "bootstrap_compiler\out\boot.exe"
$OutDir = Join-Path $Root "bootstrap_compiler\out"
$PkgDir = Join-Path $Root "bootstrap_compiler\std_packages"
$Registry = Join-Path $PkgDir "registry"
$env:PATH = "$OutDir;$env:PATH"
if (-not (Test-Path -LiteralPath $Boot)) {
    Write-Host "FAIL: missing $Boot"
    exit 1
}

$fail = 0
if (-not (Test-Path -LiteralPath $Registry)) {
    Write-Host "FAIL: missing std_packages/registry"
    $fail++
}

$expected = New-Object "System.Collections.Generic.HashSet[string]"
Get-ChildItem -LiteralPath $PkgDir -Directory | ForEach-Object {
    $packageDir = $_.FullName
    $packageName = $_.Name
    $toml = Join-Path $packageDir "Vyx.toml"
    if (-not (Test-Path -LiteralPath $toml)) { return }
    $text = [System.IO.File]::ReadAllText($toml)
    [regex]::Matches($text, '"([^"]+\.vyx)"') | ForEach-Object {
        $rel = $_.Groups[1].Value.Replace('\', '/')
        $onDisk = Join-Path $packageDir $rel.Replace('/', '\')
        if (Test-Path -LiteralPath $onDisk) {
            [void]$expected.Add("$packageName/$rel")
        }
    }
}

$listed = New-Object "System.Collections.Generic.HashSet[string]"
if (Test-Path -LiteralPath $Registry) {
    $regText = [System.IO.File]::ReadAllText($Registry)
    foreach ($raw in ($regText -split "`r?`n")) {
        $line = $raw.Trim()
        if ($line.Length -eq 0 -or $line.StartsWith("#") -or $line.StartsWith("//")) { continue }
        $line = $line.Replace('\', '/')
        [void]$listed.Add($line)
        $abs = Join-Path $PkgDir $line.Replace('/', '\')
        if (-not (Test-Path -LiteralPath $abs)) {
            Write-Host "FAIL: registry path missing on disk: $line"
            $fail++
        }
    }
}

foreach ($rel in ($expected | Sort-Object)) {
    if (-not $listed.Contains($rel)) {
        Write-Host "FAIL: registry missing $rel"
        $fail++
    }
}
foreach ($rel in ($listed | Sort-Object)) {
    if (-not $expected.Contains($rel)) {
        Write-Host "FAIL: registry extra $rel"
        $fail++
    }
}

$helloDir = Join-Path $env:TEMP "vyx-src-file-packages"
New-Item -ItemType Directory -Force -Path $helloDir | Out-Null
$hello = Join-Path $helloDir "hello.vyx"
$exe = Join-Path $helloDir "hello.exe"
$src = "use std.string;`r`n`r`nfn main() -> i32 {`r`n print(`"hello`");`r`n return 0;`r`n}`r`n"
$utf8 = New-Object System.Text.UTF8Encoding $false
[System.IO.File]::WriteAllText($hello, $src, $utf8)
$env:VYX_DEBUG_IMPORT = "1"
$log = & $Boot --src=file $hello --dump-mir2 2>&1 | Out-String
$env:VYX_DEBUG_IMPORT = ""
if ($log -notmatch "std_packages[/\\]core[/\\]src[/\\]string\.vyx") {
    Write-Host "FAIL: --src=file did not load std.string from std_packages"
    $fail++
}
if ($log -match "bootstrap_compiler[/\\]std[/\\]string\.vyx") {
    Write-Host "FAIL: --src=file still loaded flat std/string.vyx"
    $fail++
}

& $Boot --src=file $hello --emit=exe -o $exe -L $OutDir -l vyx_runtime
if ($LASTEXITCODE -ne 0) {
    Write-Host "FAIL: TEMP hello did not compile"
    $fail++
} else {
    & $exe
    if ($LASTEXITCODE -ne 0) {
        Write-Host "FAIL: TEMP hello exit=$LASTEXITCODE"
        $fail++
    }
}

if ($fail -ne 0) {
    Write-Host "src_file_packages FAIL $fail"
    exit 1
}
Write-Host "OK: src_file_packages"
exit 0
