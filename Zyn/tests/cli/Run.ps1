param([string]$Compiler, [switch]$SkipCompile)
$ErrorActionPreference = 'Stop'
$sdk = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$repo = Split-Path $sdk
if (!$Compiler) { $Compiler = Join-Path $repo 'bootstrap_compiler/out/boot.exe' }
$Compiler = (Resolve-Path -LiteralPath $Compiler).Path
$env:LLVM_ROOT = Join-Path $repo 'clang'
$env:ZYN_SDK_ROOT = $sdk
$env:ANDROID_HOME = 'E:/Android/sdk'
$run = Join-Path $PSScriptRoot ('.runs/' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $run | Out-Null
$cli = Join-Path $run 'zyn-cli.exe'
$publisher = Join-Path $run 'zyn-publish.exe'
foreach ($tool in @(@('zyn_cli.vyx', $cli), @('zyn_publish.vyx', $publisher))) {
    & $Compiler --src=file (Join-Path $sdk "tools/$($tool[0])") --emit=exe -o $tool[1] *> (Join-Path $run ($tool[0] + '.build.log'))
    if ($LASTEXITCODE) { throw "$($tool[0]) compile failed; see $run" }
}
$results = [Collections.Generic.List[object]]::new()
function Invoke-Case([string]$Name, [scriptblock]$Body) {
    $directory = Join-Path $run $Name
    New-Item -ItemType Directory -Path $directory | Out-Null
    Push-Location $directory
    try { & $Body; $results.Add(@{case=$Name; passed=$true}) } finally { Pop-Location }
}
function Assert([bool]$Value, [string]$Message) { if (!$Value) { throw $Message } }
Invoke-Case 'current-directory' {
    & $cli new demo .; Assert ($LASTEXITCODE -eq 0) 'new name . failed'
    Assert (Test-Path src/main.vyx) 'missing main'
    Assert (!(Test-Path demo)) 'created unwanted nested directory'
    Assert ((Get-Content Vyx.toml -Raw) -match 'name = "demo"') 'wrong package name'
    $before = (Get-FileHash Vyx.toml).Hash
    & $cli new demo .; Assert ($LASTEXITCODE -ne 0) 'overwrote existing project'
    Assert ((Get-FileHash Vyx.toml).Hash -eq $before) 'manifest changed'
}
Invoke-Case 'default-directory' {
    & $cli new demo; Assert ($LASTEXITCODE -eq 0) 'default directory failed'
    Assert (Test-Path demo/src/main.vyx) 'wrong default directory'
}
Invoke-Case 'spaces-and-unrelated-files' {
    New-Item -ItemType Directory -Path 'app space' | Out-Null
    Set-Content 'app space/keep.txt' 'user-owned'
    & $cli new demo 'app space' --name 'Display Name'; Assert ($LASTEXITCODE -eq 0) 'spaces failed'
    Assert ((Get-Content 'app space/keep.txt' -Raw).Trim() -eq 'user-owned') 'changed unrelated file'
    Assert ((Get-Content 'app space/Vyx.toml' -Raw) -match 'Display Name') 'display name missing'
}
Invoke-Case 'main-conflict' {
    New-Item -ItemType Directory src | Out-Null
    Set-Content src/main.vyx 'user source'
    & $cli new demo .; Assert ($LASTEXITCODE -ne 0) 'main conflict accepted'
    Assert (!(Test-Path Vyx.toml)) 'partial manifest written before conflict check'
    Assert ((Get-Content src/main.vyx -Raw).Trim() -eq 'user source') 'source overwritten'
    & $cli new demo . --force; Assert ($LASTEXITCODE -ne 0) 'force bypassed ownership'
}
foreach ($language in @('java', 'kotlin')) {
    Invoke-Case "android-$language" {
        & $cli new demo .; Assert ($LASTEXITCODE -eq 0) 'new failed'
        & $publisher --init-android --language $language; Assert ($LASTEXITCODE -eq 0) 'Android init failed'
        $activity = 'android/app/src/main/java/org/vyx/zyn/ZynActivity.java'
        Assert (Test-Path android/app/src/main/java/org/libsdl/app/SDLActivity.java) 'missing real SDL Java'
        Assert ((Get-Content android/app/src/main/AndroidManifest.xml -Raw) -match 'org.vyx.zyn.MainActivity') 'wrong activity'
        $extension = if ($language -eq 'java') {'java'} else {'kt'}
        Assert (Test-Path "android/app/src/main/java/org/vyx/zyn/MainActivity.$extension") 'wrong language'
        Add-Content $activity '// user extension'
        $before = (Get-FileHash $activity).Hash
        & $publisher --init-android --language $language; Assert ($LASTEXITCODE -eq 0) 'idempotent init failed'
        Assert ((Get-FileHash $activity).Hash -eq $before) 'replaced user Activity'
        $unexpanded = Get-ChildItem android -Recurse -File | Where-Object Extension -in '.java','.kt','.kts','.xml' | Select-String '@@[A-Z_]+@@'
        Assert (!$unexpanded) 'unexpanded template tokens'
    }
}
Invoke-Case 'android-conflict' {
    & $cli new demo .
    New-Item -ItemType Directory android | Out-Null
    Set-Content android/build.gradle.kts '// user build'
    & $publisher --init-android; Assert ($LASTEXITCODE -ne 0) 'Android overwrite accepted'
    Assert (!(Test-Path android/app)) 'partial Android project created'
    Assert ((Get-Content android/build.gradle.kts -Raw).Trim() -eq '// user build') 'build file overwritten'
}
@{compiler=$Compiler; compiler_sha256=(Get-FileHash $Compiler).Hash; cases=$results; scope='CLI generation and file safety; APK/render gates separate'} |
    ConvertTo-Json -Depth 5 | Set-Content (Join-Path $run 'results.json')
Write-Output "CLI gate passed: $run"
