param(
    [Parameter(Mandatory)][string]$Compiler,
    [string]$ManifestSource,
    [ValidateRange(128,8192)][int]$Scale=512
)
$ErrorActionPreference='Stop'
if(-not $IsWindows){throw 'This memory gate requires Windows Job accounting'}
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$Compiler=(Resolve-Path -LiteralPath $Compiler).Path
if(-not $ManifestSource){$ManifestSource=Join-Path $repo 'bootstrap_compiler/src/core/facts/effect_manifest.vyx'}
$ManifestSource=(Resolve-Path -LiteralPath $ManifestSource).Path
$run=Join-Path $PSScriptRoot ('.runs/'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
[void][IO.Directory]::CreateDirectory($run)
Copy-Item -LiteralPath $ManifestSource -Destination (Join-Path $run 'effect_manifest.vyx')
$rows=@(for($i=0;$i -lt $Scale;$i++){'record-'+$i.ToString('D8')+"`t"+('payload-'*12)})
$inputText=($rows[($Scale-1)..0] -join "`n")
$expectedText=($rows -join "`n")+"`n"
function Quote-Vyx([string]$text){return '"'+$text.Replace('\','\\').Replace('"','\"').Replace("`t",'\t').Replace("`n",'\n')+'"'}
$source=@'
module manifest_memory;
use bootstrap.effect_manifest;
fn main() -> i32 {
    if (effect_manifest_sorted_lines("") != "") { return 1; }
    if (effect_manifest_sorted_lines("\nb\na\nb\n") != "a\nb\nb\n") { return 2; }
    if (effect_manifest_sorted_lines("z\n中\na") != "a\nz\n中\n") { return 3; }
    let input = __INPUT__;
    let expected = __EXPECTED__;
    if (effect_manifest_sorted_lines(input) != expected) { return 4; }
    print("Effect manifest memory ok");
    return 0;
}
'@
$source=$source.Replace('__INPUT__',(Quote-Vyx $inputText)).Replace('__EXPECTED__',(Quote-Vyx $expectedText))
[IO.File]::WriteAllText((Join-Path $run 'main.vyx'),$source,[Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText((Join-Path $run 'Vyx.toml'),@'
[package]
name = "manifest_memory"
version = "0.1.0"
entry = "main.vyx"
[target.probe]
type = "executable"
entry = "main.vyx"
auto_sources = false
sources = ["main.vyx", "effect_manifest.vyx"]
'@,[Text.UTF8Encoding]::new($false))
. (Join-Path $repo 'bootstrap_compiler/scripts/VyxTestProcess.ps1')
$env:PATH=(Split-Path $Compiler)+';'+$env:PATH
$records=[Collections.Generic.List[object]]::new()
foreach($step in @('build','run')){
    $executable=$Compiler
    $arguments=@('--src=project',$run,'--target','probe','-j2','-O0','-o',(Join-Path $run 'probe.exe'))
    $limit=4096
    if($step -eq 'run'){$executable=Join-Path $run 'probe.exe';$arguments=@();$limit=256}
    $timer=[Diagnostics.Stopwatch]::StartNew()
    $result=Invoke-VyxProcess -FilePath $executable -ArgumentList $arguments -WorkingDirectory $run -TimeoutSec 120 -MemoryLimitMB $limit -StdoutLog (Join-Path $run "$step.stdout.log") -StderrLog (Join-Path $run "$step.stderr.log") -DialogLog (Join-Path $run "$step.dialog.log")
    $timer.Stop()
    $records.Add(@{step=$step;command=$executable+' '+($arguments -join ' ');seconds=$timer.Elapsed.TotalSeconds;result=$result})
    @{compiler=$Compiler;sha256=(Get-FileHash $Compiler).Hash;manifest_sha256=(Get-FileHash $ManifestSource).Hash;scale=$Scale;records=$records.ToArray()}|ConvertTo-Json -Depth 6|Set-Content (Join-Path $run 'results.json') -Encoding utf8NoBOM
    if($result.ExitCode -ne 0){throw "$step failed: $($result.ExitCode); $run"}
    if($step -eq 'run' -and [IO.File]::ReadAllText($result.StdoutLog).Trim() -ne 'Effect manifest memory ok'){throw 'Unexpected native output'}
}
Write-Output "Effect manifest memory PASS; $run"
