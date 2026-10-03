param(
    [ValidateSet('arm64-v8a','x86_64')][string]$Abi='x86_64',
    [ValidateSet('java','kotlin')][string]$Language='java',
    [string]$Compiler,
    [string]$AndroidSdk='E:/Android/sdk',
    [string]$AndroidNdk='E:/Android/sdk/ndk/30.0.15729638',
    [int]$Jobs=2,
    [string]$DeviceSerial='',
    [string]$ProjectDirectory='',
    [switch]$PackageOnly,
    [switch]$Interaction,
    [switch]$StatefulInteraction,
    [switch]$CustomRender
)
$ErrorActionPreference='Stop'
if ($StatefulInteraction -and !$Interaction) { throw '-StatefulInteraction requires -Interaction' }
if ($CustomRender -and $Interaction) { throw '-CustomRender and -Interaction are separate fixtures' }
$sdk=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$repo=Split-Path $sdk
if (!$Compiler) { $Compiler=Join-Path $repo 'bootstrap_compiler/out/boot.exe' }
$Compiler=(Resolve-Path -LiteralPath $Compiler).Path
$env:ZYN_VYXC=$Compiler
$env:VYX_COMPILER=$Compiler
$env:ZYN_SDK_ROOT=$sdk
$env:ZYN_ANDROID_ABI=$Abi
$env:LLVM_ROOT=Join-Path $repo 'clang'
$env:ANDROID_HOME=$AndroidSdk
$env:ANDROID_SDK_ROOT=$AndroidSdk
$env:ANDROID_NDK_HOME=$AndroidNdk
$env:ANDROID_NDK_ROOT=$AndroidNdk
$target=if($Abi -eq 'arm64-v8a'){'android-arm64'}else{'android-x64'}
$zyn=Join-Path $sdk 'bin/zyn.ps1'
if (!$ProjectDirectory) { $ProjectDirectory=Join-Path $PSScriptRoot ('.runs/'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff')+'-'+$Abi+'-'+$Language) }
$ProjectDirectory=[IO.Path]::GetFullPath($ProjectDirectory)
New-Item -ItemType Directory -Force -Path $ProjectDirectory | Out-Null
function Run-Step([string]$Name,[scriptblock]$Body) {
    & $Body *> (Join-Path $ProjectDirectory ($Name+'.log'))
    if ($LASTEXITCODE) { throw "$Name failed ($LASTEXITCODE): $ProjectDirectory/$Name.log" }
}
Push-Location $ProjectDirectory
try {
    if (!(Test-Path Vyx.toml)) { Run-Step 'new' { & $zyn new zyn_android_smoke . --name 'Zyn Android' } }
    Run-Step 'init-android' { & $zyn init-android --language $Language }
    if ($Interaction) {
        $fixture=if($StatefulInteraction){'interaction_stateful_main.vyx'}else{'interaction_main.vyx'}
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $fixture) -Destination 'src/main.vyx' -Force
        $androidManifest='android/app/src/main/AndroidManifest.xml'
        $manifestText=Get-Content -LiteralPath $androidManifest -Raw
        if (!$manifestText.Contains('android.permission.CAMERA')) {
            $manifestText=$manifestText.Replace('<application ', '<uses-permission android:name="android.permission.CAMERA" />' + "`n" + '    <application ')
            Set-Content -LiteralPath $androidManifest -Value $manifestText
        }
    }
    if ($CustomRender) {
        $sample=Join-Path $repo 'samples/projects/zyn_custom_render_view_smoke'
        Copy-Item -LiteralPath (Join-Path $sample 'src/main.vyx') -Destination 'src/main.vyx' -Force
        New-Item -ItemType Directory -Force -Path 'assets/shaders' | Out-Null
        Copy-Item -LiteralPath (Join-Path $sample 'assets/shaders/triangle.slang') -Destination 'assets/shaders/triangle.slang' -Force
    }
    if (!$PackageOnly) { Run-Step 'native-build' { & $zyn build --target $target "-j$Jobs" } }
    Run-Step 'publish' { & $Compiler --src=file (Join-Path $sdk 'tools/zyn_publish.vyx') --run=aot -- --target $target --debug }
    $apk=Join-Path $ProjectDirectory "dist/$target/zyn_android_smoke-$target.apk"
    if (!(Test-Path -LiteralPath $apk)) { throw "Missing APK: $apk" }
    $readelf=Join-Path $AndroidNdk 'toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe'
    $native=Join-Path $ProjectDirectory 'target/libzyn_android_smoke.so'
    $symbols=& $readelf --dyn-syms $native
    if ($LASTEXITCODE -or !($symbols -match '\bSDL_main\b') -or !($symbols -match '\bmain\b')) { throw 'Vyx entry/SDL_main not exported' }
    $symbols | Set-Content symbols.log
    $frameworkSymbols=& $readelf --dyn-syms (Join-Path $sdk "out/android/$Abi/libZyn.so")
    if ($LASTEXITCODE -or !($frameworkSymbols -match '\bbcmp\b')) { throw 'Android API 29 bcmp compatibility symbol missing from libZyn.so' }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip=[IO.Compression.ZipFile]::OpenRead($apk)
    try {
        $entries=@($zip.Entries.FullName)
        foreach($required in @("lib/$Abi/libzyn_android_smoke.so","lib/$Abi/libZyn.so","lib/$Abi/libSDL3.so","lib/$Abi/libCacao.so","lib/$Abi/libc++_shared.so","lib/$Abi/libfreetype.so","lib/$Abi/libharfbuzz.so","lib/$Abi/libzynicuuc.so","lib/$Abi/libzynicui18n.so","lib/$Abi/libzynicudata.so",'assets/Zyn.runtime.toml','assets/nut/solid.slang')) {
            if($required -notin $entries) { throw "APK missing $required" }
        }
        if ($CustomRender -and 'assets/zyn/assets/shaders/triangle.slang' -notin $entries) {
            throw 'APK missing CustomRenderView shader'
        }
        if('assets/Vyx.toml' -in $entries) { throw 'Build manifest leaked into the APK' }
        $runtimeStream=$zip.GetEntry('assets/Zyn.runtime.toml').Open()
        $runtimeReader=[IO.StreamReader]::new($runtimeStream)
        try { $runtimeToml=$runtimeReader.ReadToEnd() } finally { $runtimeReader.Dispose() }
        if (!$runtimeToml.Contains('[zyn.app]') -or $runtimeToml.Contains('[build]') -or $runtimeToml.Contains('[zyn.publish')) {
            throw 'APK runtime manifest contains build-only configuration'
        }
        if (![regex]::IsMatch($runtimeToml,'(?m)^name\s*=\s*"[^"]+"') -or
            ![regex]::IsMatch($runtimeToml,'(?m)^id\s*=\s*"[^"]+"')) {
            throw 'APK runtime manifest contains unquoted TOML strings'
        }
        $entries | Set-Content apk-entries.txt
    } finally { $zip.Dispose() }
    $devicePassed=$false
    $interactionPassed=$false
    if ($DeviceSerial) {
        $adb=Join-Path $AndroidSdk 'platform-tools/adb.exe'
        function Assert-NoAndroidErrorDialog([string]$Phase) {
            & $adb -s $DeviceSerial shell uiautomator dump /sdcard/zyn-window.xml *> (Join-Path $ProjectDirectory "ui-$Phase.log")
            if ($LASTEXITCODE -ne 0) { throw "Could not inspect Android window after $Phase" }
            $hierarchy=((& $adb -s $DeviceSerial shell cat /sdcard/zyn-window.xml) -join '')
            $hierarchy | Set-Content (Join-Path $ProjectDirectory "ui-$Phase.xml")
            if ($hierarchy.Contains('text="SDL Error"') -or $hierarchy.Contains('dlopen failed')) {
                throw "SDL failed to start after $Phase; see ui-$Phase.xml"
            }
        }
        Run-Step 'install' { & $adb -s $DeviceSerial install -r $apk }
        if ($Interaction) {
            & $adb -s $DeviceSerial shell pm clear dev.vyx.zyn.zyn_android_smoke | Out-Null
            if ($LASTEXITCODE -ne 0) { throw 'Could not reset interaction test application state' }
        }
        & $adb -s $DeviceSerial logcat -c
        Run-Step 'launch' { & $adb -s $DeviceSerial shell am start -W -n dev.vyx.zyn.zyn_android_smoke/org.vyx.zyn.MainActivity }
        Start-Sleep -Seconds 8
        $pidBefore=((& $adb -s $DeviceSerial shell pidof dev.vyx.zyn.zyn_android_smoke) -join '').Trim()
        if(!$pidBefore) { throw 'App exited before frame capture' }
        & $adb -s $DeviceSerial shell screencap -p /data/local/tmp/zyn-android-before.png
        & $adb -s $DeviceSerial pull /data/local/tmp/zyn-android-before.png before.png
        Assert-NoAndroidErrorDialog 'launch'
        & $adb -s $DeviceSerial shell input keyevent KEYCODE_HOME
        Start-Sleep -Seconds 2
        & $adb -s $DeviceSerial shell am start -W -n dev.vyx.zyn.zyn_android_smoke/org.vyx.zyn.MainActivity
        Start-Sleep -Seconds 5
        $pidAfter=((& $adb -s $DeviceSerial shell pidof dev.vyx.zyn.zyn_android_smoke) -join '').Trim()
        if (!$pidAfter -or $pidAfter -ne $pidBefore) { throw 'App process lost during pause/resume' }
        & $adb -s $DeviceSerial shell screencap -p /data/local/tmp/zyn-android-after.png
        & $adb -s $DeviceSerial pull /data/local/tmp/zyn-android-after.png after.png
        Assert-NoAndroidErrorDialog 'resume'
        if ($CustomRender) {
            foreach ($slot in 1,2) {
                $marker=((& $adb -s $DeviceSerial shell run-as dev.vyx.zyn.zyn_android_smoke cat "files/target/custom_render_$slot.txt") -join '').Trim()
                if ($LASTEXITCODE -ne 0 -or $marker -ne 'triangle') { throw "CustomRenderView slot $slot did not record a frame" }
            }
            Add-Type -AssemblyName System.Drawing
            foreach ($capture in @('before.png','after.png')) {
                $bitmap=[System.Drawing.Bitmap]::new((Join-Path $ProjectDirectory $capture))
                try {
                    $cyan=$false
                    $magenta=$false
                    for ($y=100; $y -lt [int]($bitmap.Height / 2); $y+=5) {
                        for ($x=0; $x -lt $bitmap.Width; $x+=5) {
                            $pixel=$bitmap.GetPixel($x,$y)
                            if ($pixel.R -lt 150 -and $pixel.G -gt 140 -and $pixel.B -gt 160) { $cyan=$true }
                            if ($pixel.R -gt 175 -and $pixel.G -lt 160 -and $pixel.B -gt 90) { $magenta=$true }
                        }
                    }
                    if (!$cyan -or !$magenta) { throw "CustomRenderView triangle colors missing from $capture" }
                } finally { $bitmap.Dispose() }
            }
        }
        if ($Interaction) {
            $sizeText=(& $adb -s $DeviceSerial shell wm size) -join ' '
            $densityText=(& $adb -s $DeviceSerial shell wm density) -join ' '
            $sizeMatch=[regex]::Match($sizeText,'(?:Override|Physical) size:\s*(\d+)x\d+')
            $densityMatch=[regex]::Match($densityText,'(?:Override|Physical) density:\s*(\d+)')
            if (!$sizeMatch.Success -or !$densityMatch.Success) { throw 'Could not read emulator size or density' }
            $tapX=[int]::Parse($sizeMatch.Groups[1].Value) / 2
            $scale=[int]::Parse($densityMatch.Groups[1].Value) / 160.0
            function Read-ProbeFile([string]$Name) {
                $value=(& $adb -s $DeviceSerial shell run-as dev.vyx.zyn.zyn_android_smoke cat "files/$Name") -join ''
                if ($LASTEXITCODE -ne 0) { throw "Missing app probe file: $Name" }
                return $value.Trim()
            }
            function Tap-Probe([double]$Ydp) {
                $tapY=[int][Math]::Round($Ydp * $scale)
                & $adb -s $DeviceSerial shell input tap $tapX $tapY
                if ($LASTEXITCODE -ne 0) { throw "Could not tap probe at $tapX,$tapY" }
                Start-Sleep -Milliseconds 700
            }
            function Tap-PermissionChoice([string]$Choice) {
                & $adb -s $DeviceSerial shell uiautomator dump /sdcard/zyn-permission.xml | Out-Null
                if ($LASTEXITCODE -ne 0) { throw 'Permission dialog was not inspectable' }
                $dialog=(& $adb -s $DeviceSerial shell cat /sdcard/zyn-permission.xml) -join ''
                $choiceMatch=[regex]::Match($dialog,'resource-id="[^"]*:id/permission_'+$Choice+'_button"[^>]*bounds="\[(\d+),(\d+)\]\[(\d+),(\d+)\]"')
                if (!$choiceMatch.Success) { throw "Android $Choice permission button was not shown" }
                $choiceX=[int](([int]$choiceMatch.Groups[1].Value+[int]$choiceMatch.Groups[3].Value)/2)
                $choiceY=[int](([int]$choiceMatch.Groups[2].Value+[int]$choiceMatch.Groups[4].Value)/2)
                & $adb -s $DeviceSerial shell input tap $choiceX $choiceY
                if ($LASTEXITCODE -ne 0) { throw "Could not choose Android permission $Choice" }
                Start-Sleep -Milliseconds 700
            }
            Tap-Probe 285
            if ((Read-ProbeFile 'touch_count.txt') -ne '1') { throw 'A touch did not activate exactly one Zyn action' }
            if ($StatefulInteraction) {
                if ((Read-ProbeFile 'stateful_frame_seen.txt') -ne '1') { throw 'Stateful frame update was not called' }
                if ([int](Read-ProbeFile 'stateful_touch_events.txt') -lt 1) { throw 'Stateful platform update was not called' }
            }
            & $adb -s $DeviceSerial shell screencap -p /data/local/tmp/zyn-android-tap.png
            & $adb -s $DeviceSerial pull /data/local/tmp/zyn-android-tap.png interaction-after-tap.png | Out-Null
            if ($LASTEXITCODE -ne 0) { throw 'Could not capture the view after touch' }
            Tap-Probe 140
            Tap-PermissionChoice 'allow'
            Tap-Probe 220
            if ((Read-ProbeFile 'permission_state.txt') -ne '1') { throw 'Android permission grant callback was not delivered' }
            & $adb -s $DeviceSerial shell pm revoke dev.vyx.zyn.zyn_android_smoke android.permission.CAMERA
            if ($LASTEXITCODE -ne 0) { throw 'Could not reset camera permission for denial test' }
            & $adb -s $DeviceSerial shell am start -W -n dev.vyx.zyn.zyn_android_smoke/org.vyx.zyn.MainActivity | Out-Null
            Start-Sleep -Seconds 2
            Tap-Probe 140
            Tap-PermissionChoice 'deny'
            Tap-Probe 220
            if ((Read-ProbeFile 'permission_state.txt') -ne '0') { throw 'Android permission denial callback was not delivered' }
            $interactionPassed=$true
        }
        & $adb -s $DeviceSerial logcat -d | Set-Content logcat.txt
        $devicePassed=$true
    }
    @{abi=$Abi;language=$Language;apk=$apk;apk_sha256=(Get-FileHash $apk).Hash;compiler_sha256=(Get-FileHash $Compiler).Hash;device=$DeviceSerial;device_lifecycle_passed=$devicePassed;interaction_passed=$interactionPassed;stateful_interaction=$StatefulInteraction.IsPresent;custom_render=$CustomRender.IsPresent;visual_review='Inspect before.png and after.png; a surviving process alone does not prove rendering'} |
        ConvertTo-Json | Set-Content results.json
    Write-Output "Android gate artifacts: $ProjectDirectory"
} finally { Pop-Location }
