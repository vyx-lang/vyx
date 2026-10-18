param([Parameter(Mandatory)][string]$Compiler)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$Compiler=(Resolve-Path $Compiler).Path
$run=Join-Path $PSScriptRoot ('.runs/'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $run | Out-Null
. (Join-Path $repo 'bootstrap_compiler/scripts/VyxTestProcess.ps1')
$results=[Collections.Generic.List[object]]::new()
function Step([string]$Name,[string]$Exe,[string[]]$Arguments,[string]$Directory,[bool]$Success=$true){
    $r=Invoke-VyxProcess -FilePath $Exe -ArgumentList $Arguments -WorkingDirectory $Directory -TimeoutSec 120 -MemoryLimitMB 4096 -StdoutLog (Join-Path $run "$Name.stdout.log") -StderrLog (Join-Path $run "$Name.stderr.log") -DialogLog (Join-Path $run "$Name.dialog.log")
    $results.Add([ordered]@{name=$Name;command=$Exe+' '+($Arguments -join ' ');result=$r})
    [ordered]@{compiler=$Compiler;sha256=(Get-FileHash $Compiler).Hash;records=$results.ToArray()} | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $run 'results.json') -Encoding utf8NoBOM
    if($r.TimedOut -or $r.MemoryExceeded -or $r.DialogCaught){throw "$Name crashed or exceeded resources"}
    if($Success -and $r.ExitCode -ne 0){throw "$Name failed: $($r.ExitCode)"}
    if(!$Success -and $r.ExitCode -ne 1){throw "$Name did not produce a compile-time error: $($r.ExitCode)"}
    return $r
}
foreach($case in @('console','shadow','unsigned-method','atomic','index-type','pack-index','container-clone','global-widen')){
    foreach($opt in @('-O0','-O2')){
        foreach($mode in @('file','project')){
            $name="$case-$mode-$opt"
            $project=Join-Path $run $name
            New-Item -ItemType Directory -Path $project | Out-Null
            Copy-Item (Join-Path $PSScriptRoot "$case.vyx") (Join-Path $project 'main.vyx')
            if($mode -eq 'project'){
                [IO.File]::WriteAllText((Join-Path $project 'Vyx.toml'),@'
[package]
name = "semantic_probe"
version = "0.1.0"
entry = "main.vyx"
[dependencies]
core = { path = "std:core", target = "std_core" }
collections = { path = "std:collections", target = "std_collections" }
io = { path = "std:io", target = "std_io" }
[target.probe]
type = "executable"
entry = "main.vyx"
auto_sources = false
sources = ["main.vyx"]
'@,[Text.UTF8Encoding]::new($false))
                $inputArgs=@('--src=project',$project,'--target','probe','-j2')
            }else{$inputArgs=@('--src=file',(Join-Path $project 'main.vyx'))}
            $ir=Join-Path $project 'probe.ll'
            Step ($name+'-ir') $Compiler ($inputArgs+@('--emit=ir',$opt,'-o',$ir)) $project | Out-Null
            if([IO.File]::ReadAllText($ir) -match '\bcall\b[^\r\n]*@(?:print|println|vyx_print_bytes_abi)\('){throw "$name emitted a compiler print entry"}
            $exe=Join-Path $project 'probe.exe'
            Step ($name+'-build') $Compiler ($inputArgs+@('--emit=exe',$opt,'-o',$exe)) $project | Out-Null
            $r=Step ($name+'-run') $exe @() $project
            if($case -eq 'console'){
                $expected="4429/-42/18446744073709551615/true/你/owned/label`nA"+[char]0+"B`n42/42`n`n"
                $actual=[IO.File]::ReadAllText($r.StdoutLog).Replace("`r`n","`n")
                if($actual -cne $expected){throw "$name output differs: $actual"}
            }
        }
    }
}
foreach($case in @('custom-clone','trait-parameters')){
 foreach($opt in @('-O0','-O2')){
    $name="$case-$opt"
    $exe=Join-Path $run "$name.exe"
    $args=@('--src=file',(Join-Path $PSScriptRoot "$case.vyx"),'--no-implicit-std',$opt)
    if($case -eq 'trait-parameters'){$args=@('--src=file',(Join-Path $PSScriptRoot "$case.vyx"),$opt)}
    Step ($name+'-ir') $Compiler ($args+@('--emit=ir','-o',(Join-Path $run "$name.ll"))) $repo | Out-Null
    Step ($name+'-build') $Compiler ($args+@('--emit=exe','-o',$exe)) $repo | Out-Null
    Step ($name+'-run') $exe @() $repo | Out-Null
 }
}
$libraryProject=Join-Path $run 'library'
New-Item -ItemType Directory -Path $libraryProject | Out-Null
Copy-Item (Join-Path $PSScriptRoot 'library.vyx') (Join-Path $libraryProject 'main.vyx')
[IO.File]::WriteAllText((Join-Path $libraryProject 'Vyx.toml'),@'
[package]
name = "library_probe"
version = "0.1.0"
entry = "main.vyx"
[dependencies]
core = { path = "std:core", target = "std_core" }
[target.probe]
type = "static"
entry = "main.vyx"
auto_sources = false
sources = ["main.vyx"]
'@,[Text.UTF8Encoding]::new($false))
$savedEmitThreads=$env:VYX_CGU_EMIT_THREADS
try {
    foreach($threads in @('1','2')){
        $env:VYX_CGU_EMIT_THREADS=$threads
        $output=Join-Path $libraryProject "library-$threads.lib"
        Step "library-emit-$threads" $Compiler @('--src=project',$libraryProject,'--target','probe','-j2','--emit=lib','-o',$output) $libraryProject | Out-Null
    }
} finally {$env:VYX_CGU_EMIT_THREADS=$savedEmitThreads}
foreach($opt in @('-O0','-O2')){
    $project=Join-Path $run "native-$opt"
    New-Item -ItemType Directory -Path $project | Out-Null
    Copy-Item (Join-Path $PSScriptRoot 'native.vyx') (Join-Path $project 'main.vyx')
    Copy-Item (Join-Path $PSScriptRoot 'native.c') (Join-Path $project 'native.c')
    [IO.File]::WriteAllText((Join-Path $project 'Vyx.toml'),@'
[package]
name = "native_names"
version = "0.1.0"
entry = "main.vyx"
[target.probe]
type = "executable"
entry = "main.vyx"
auto_sources = false
sources = ["main.vyx", "native.c"]
'@,[Text.UTF8Encoding]::new($false))
    $args=@('--src=project',$project,'--target','probe','-j2',$opt)
    $exe=Join-Path $project 'probe.exe'
    Step "native-$opt-build" $Compiler ($args+@('--emit=exe','-o',$exe)) $project | Out-Null
    Step "native-$opt-run" $exe @() $project | Out-Null
}
foreach($case in @('missing-print','raw-deref','unknown-member','duplicate-provider','missing-clone','invalid-atomic','invalid-index','nonclone-container','unknown-trait')){
    $output=Join-Path $run "$case.exe"
    $stdFlags=@('--no-implicit-std')
    if($case -eq 'nonclone-container'){$stdFlags=@()}
    $r=Step $case $Compiler (@('--src=file',(Join-Path $PSScriptRoot "$case.vyx"))+ $stdFlags +@('--emit=exe','-o',$output)) $repo $false
    if(Test-Path $output){throw "$case published output despite missing semantic facts"}
    $diagnostic=[IO.File]::ReadAllText($r.StderrLog)+[IO.File]::ReadAllText($r.StdoutLog)
    if($diagnostic -notmatch '\.vyx:\d+:\d+: error: [A-Z]\d+'){throw "$case lacks a located diagnostic"}
    if($case -eq 'nonclone-container' -and $diagnostic -notmatch 'error: E1100: trait bound not satisfied') {throw 'Clone negative failed for a different reason'}
    if($case -eq 'unknown-trait' -and $diagnostic -notmatch 'error: E1100: unknown trait constraint') {throw 'Unknown trait was not diagnosed'}
}
Write-Host "Semantic fallback gate PASS; $run"
