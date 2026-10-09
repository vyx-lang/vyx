param([string]$BaseUrl = 'http://127.0.0.1:4197', [string]$TestRoot)
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
if (!$TestRoot) { $TestRoot = Join-Path $repoRoot ('out/sdk-setup-tests/windows-' + [Guid]::NewGuid().ToString('N')) }
$testRoot = [IO.Path]::GetFullPath($TestRoot)
$originalPath = $env:Path
$originalUserPath = [Environment]::GetEnvironmentVariable('Path', 'User')
New-Item -ItemType Directory -Path $testRoot -Force | Out-Null
function Assert($condition, $message) { if (!$condition) { throw $message } }
try {
    $installer = [scriptblock]::Create((Invoke-RestMethod "$BaseUrl/install/windows.ps1"))
    $installDir = Join-Path $testRoot 'SDK with spaces'
    $configDir = Join-Path $testRoot 'config'
    $script:answers = New-Object 'Collections.Generic.Queue[string]'
    function Read-Host { param($Prompt) Write-Host $Prompt; return $script:answers.Dequeue() }
    $script:answers.Enqueue('3')
    & $installer -BaseUrl "$BaseUrl/sdk" -ConfigDir $configDir -NoPersistPath
    Assert (!(Test-Path -LiteralPath $configDir)) 'Cancel created configuration.'
    $script:answers.Enqueue('2'); $script:answers.Enqueue($installDir)
    & $installer -BaseUrl "$BaseUrl/sdk" -ConfigDir $configDir -NoPersistPath
    $stateFile = Join-Path $installDir 'install.json'
    $state = Get-Content -LiteralPath $stateFile -Raw | ConvertFrom-Json
    $first = $state.sha256
    Assert ((Get-Command vyxc).Source -eq (Join-Path $state.bin 'vyxc.exe')) 'The PowerShell session did not select the installed SDK.'
    Assert ([Environment]::GetEnvironmentVariable('Path','User') -eq $originalUserPath) 'The test changed user PATH.'
    Assert ((Get-Command vyxup).Source -eq (Join-Path $installDir 'bin/vyxup.cmd')) 'The manager is not on PATH.'
    $project = Join-Path $testRoot 'hello-project'
    New-Item -ItemType Directory -Path (Join-Path $project 'src') -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $project 'src/main.vyx') -Value 'fn main() -> i32 { print("installer-ok"); return 0; }' -Encoding ASCII
    Set-Content -LiteralPath (Join-Path $project 'Vyx.toml') -Value "[package]`nname = `"installer_hello`"`nversion = `"0.1.0`"`nentry = `"src/main.vyx`"`n`n[build]`noutput_dir = `"target`"`n`n[target.installer_hello]`ntype = `"executable`"`nentry = `"src/main.vyx`"`n" -Encoding ASCII
    Push-Location $project
    try { & vyxc build --run=aot -j1; Assert ($LASTEXITCODE -eq 0) 'Installed SDK AOT failed.' } finally { Pop-Location }
    & $installer -BaseUrl "$BaseUrl/sdk" -ConfigDir $configDir -Yes -NoPersistPath
    Assert (@($env:Path -split ';' | Where-Object { $_ -eq $state.bin }).Count -eq 1) 'Repeated setup duplicated PATH.'
    & vyxup update -BaseUrl "$BaseUrl/update/sdk" -NoPersistPath
    Assert ($LASTEXITCODE -eq 0) 'Installed manager update failed.'
    $state = Get-Content -LiteralPath $stateFile -Raw | ConvertFrom-Json
    Assert ($state.sha256 -ne $first -and $state.previous -eq $first) 'Update lost the previous revision.'
    Assert ((Get-Content -LiteralPath (Join-Path $configDir 'install.json') -Raw | ConvertFrom-Json).root -eq $installDir) 'Update lost the custom root.'
    Assert (Test-Path -LiteralPath (Join-Path $installDir 'current/SETUP-TEST-REVISION')) 'The stable current junction did not select the update.'
    $second = $state.sha256
    & vyxup rollback -NoPersistPath
    Assert ($LASTEXITCODE -eq 0) 'Offline rollback failed.'
    Assert ((Get-Content -LiteralPath $stateFile -Raw | ConvertFrom-Json).sha256 -eq $first) 'Rollback selected the wrong SDK.'
    & vyxup rollback -NoPersistPath
    Assert ($LASTEXITCODE -eq 0) 'Second rollback failed.'
    Assert ((Get-Content -LiteralPath $stateFile -Raw | ConvertFrom-Json).sha256 -eq $second) 'Rollback lost the next revision.'
    $before = Get-Content -LiteralPath $stateFile -Raw
    foreach ($variant in @('bad-checksum','bad-startup')) {
        $rejected = $false
        try { & $installer update -BaseUrl "$BaseUrl/$variant/sdk" -ConfigDir $configDir -NoPersistPath }
        catch { Write-Host "Expected rejection: $($_.Exception.Message)"; $rejected = $true }
        Assert $rejected "The $variant archive was accepted."
        Assert ((Get-Content -LiteralPath $stateFile -Raw) -eq $before) 'A rejected update changed SDK state.'
        Assert (Test-Path -LiteralPath (Join-Path $installDir 'current/SETUP-TEST-REVISION')) 'A rejected update changed the current junction.'
    }
    & vyxup update -BaseUrl "$BaseUrl/bad-checksum/sdk" -NoPersistPath
    Assert ($LASTEXITCODE -ne 0) 'Installed manager returned success for a rejected update.'
    Assert (!(Get-ChildItem -LiteralPath $installDir -Filter '.staging-*')) 'Setup left a staging directory.'
    $external = Join-Path $testRoot 'external'
    New-Item -ItemType Directory -Path $external | Out-Null
    Set-Content -LiteralPath (Join-Path $external 'keep.txt') -Value 'keep'
    New-Item -ItemType Junction -Path (Join-Path $installDir "sdk/$first/external-link") -Target $external | Out-Null
    Set-Content -LiteralPath (Join-Path $installDir 'user-note.txt') -Value 'keep'
    $script:answers.Enqueue('n')
    & $installer uninstall -ConfigDir $configDir -NoPersistPath
    Assert (Test-Path -LiteralPath $stateFile) 'Uninstall ignored cancellation.'
    & vyxup uninstall -Yes -NoPersistPath
    Assert ($LASTEXITCODE -eq 0) 'Installed uninstaller failed.'
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    while ((Test-Path -LiteralPath (Join-Path $installDir 'bin/vyxup.cmd')) -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 100 }
    Assert (!(Test-Path -LiteralPath (Join-Path $installDir 'bin/vyxup.cmd'))) 'Launcher cleanup did not complete.'
    Assert (!(Test-Path -LiteralPath $stateFile) -and !(Test-Path -LiteralPath (Join-Path $installDir 'current')) -and !(Test-Path -LiteralPath (Join-Path $configDir 'install.json'))) 'Uninstall left SDK state or configuration.'
    Assert (Test-Path -LiteralPath (Join-Path $external 'keep.txt')) 'Uninstall followed a junction.'
    Assert (Test-Path -LiteralPath (Join-Path $installDir 'user-note.txt')) 'Uninstall deleted an unrelated file.'
    Assert (Test-Path -LiteralPath (Join-Path $project 'src/main.vyx')) 'Uninstall deleted a project.'
    Assert ([Environment]::GetEnvironmentVariable('Path','User') -eq $originalUserPath) 'The test changed user PATH.'
    Write-Host 'PASS: Windows menu/cancel, custom directory memory, real SDK AOT, update/stable PATH, offline rollback, rejected checksum/startup with nonzero exit, uninstall cancellation, launcher cleanup, junction safety and unrelated-file retention. User PATH preserved.'
    Write-Host "Test evidence retained at $testRoot"
} finally { $env:Path = $originalPath }
