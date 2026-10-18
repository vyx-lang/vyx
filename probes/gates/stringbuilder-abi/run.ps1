param([Parameter(Mandatory)][string]$Compiler)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$Compiler=(Resolve-Path -LiteralPath $Compiler).Path
$run=Join-Path $PSScriptRoot ('.runs/'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $run | Out-Null
. (Join-Path $repo 'bootstrap_compiler/scripts/VyxTestProcess.ps1')
$env:PATH=(Split-Path $Compiler)+';'+$env:PATH
$results=[Collections.Generic.List[object]]::new()
function Invoke-Step([string]$Name,[string]$Executable,[string[]]$Arguments,[string]$Directory,[string]$Expected=''){
    $r=Invoke-VyxProcess -FilePath $Executable -ArgumentList $Arguments -WorkingDirectory $Directory `
      -TimeoutSec 120 -MemoryLimitMB 4096 -StdoutLog (Join-Path $run "$Name.stdout.log") `
      -StderrLog (Join-Path $run "$Name.stderr.log") -DialogLog (Join-Path $run "$Name.dialog.log")
    $results.Add([ordered]@{name=$Name;command=$Executable+' '+($Arguments -join ' ');result=$r})
    [ordered]@{compiler=$Compiler;sha256=(Get-FileHash $Compiler -Algorithm SHA256).Hash;records=$results.ToArray()} | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $run 'results.json') -Encoding utf8NoBOM
    if($r.ExitCode -ne 0){throw "$Name failed: $($r.ExitCode)"}
    if($Expected -and ([IO.File]::ReadAllText($r.StdoutLog).Replace("`r`n","`n") -ne $Expected)){throw "$Name produced unexpected output"}
}
foreach($case in @('repro','cases')){
    foreach($opt in @('-O0','-O2')){
        foreach($mode in @('file','project')){
            $name="$case-$mode-$opt"
            $project=Join-Path $run $name
            New-Item -ItemType Directory -Path $project | Out-Null
            Copy-Item (Join-Path $PSScriptRoot "$case.vyx") (Join-Path $project 'main.vyx')
            if($mode -eq 'project'){
                [IO.File]::WriteAllText((Join-Path $project 'Vyx.toml'),@'
[package]
name = "builder_probe"
version = "0.1.0"
entry = "main.vyx"
[dependencies]
core = { path = "std:core", target = "std_core" }
collections = { path = "std:collections", target = "std_collections" }
[target.probe]
type = "executable"
entry = "main.vyx"
auto_sources = false
sources = ["main.vyx"]
'@,[Text.UTF8Encoding]::new($false))
                $inputArgs=@('--src=project',$project,'--target','probe','-j2')
            }else{$inputArgs=@('--src=file',(Join-Path $project 'main.vyx'))}
            $ir=Join-Path $project 'main.ll'
            Invoke-Step ($name+'-ir') $Compiler ($inputArgs+@('--emit=ir',$opt,'-o',$ir)) $project
            $text=[IO.File]::ReadAllText($ir)
            if($text -match '\bcall\b[^\r\n]*@vyx_string_builder_(?:create|append_i64|append_u64)_abi\('){throw "$name substituted the legacy StringBuilder ABI"}
            if($opt -eq '-O0' -and $text -notmatch 'call void @[^\r\n]*StringBuilder[^\r\n]*create[^\r\n]*sret\('){throw "$name did not call the aggregate factory ABI"}
            $exe=Join-Path $project 'probe.exe'
            Invoke-Step ($name+'-compile') $Compiler ($inputArgs+@('--emit=exe',$opt,'-o',$exe)) $project
            $expected=if($case -eq 'repro'){"4429`n"}else{"StringBuilder ABI ok`n"}
            Invoke-Step ($name+'-run') $exe @() $project $expected
        }
    }
}
Write-Host "StringBuilder ABI: file/project O0/O2, native factory, storage and shadowing PASS; $run"
