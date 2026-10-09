param([string]$Compiler = (Join-Path $PSScriptRoot '../../../bootstrap_compiler/out/boot.exe'))
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$Compiler = (Resolve-Path -LiteralPath $Compiler).Path
$runDir = Join-Path $PSScriptRoot ('.runs/epoch-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $runDir | Out-Null
$sema = [IO.File]::ReadAllText((Join-Path $repo 'bootstrap_compiler/src/core/sema/sema.vyx'))
$policy = [IO.File]::ReadAllText((Join-Path $repo 'bootstrap_compiler/src/core/sema/policy.vyx'))

function Extract-Function([string]$Text, [string]$Name) {
    $found = [regex]::Match($Text, '(?m)^[ \t]*(?:public )?fn ' + [regex]::Escape($Name) + '\s*\(')
    if (-not $found.Success) { throw "Missing production function $Name" }
    $depth=0; $quoted=[char]0; $lineComment=$false; $blockComment=$false
    for ($i=$Text.IndexOf('{',$found.Index); $i -lt $Text.Length; $i++) {
        $c=$Text[$i]; $next=if($i+1 -lt $Text.Length){$Text[$i+1]}else{[char]0}
        if($lineComment){if($c -eq "`n"){$lineComment=$false};continue}
        if($blockComment){if($c -eq '*' -and $next -eq '/'){$blockComment=$false;$i++};continue}
        if($quoted -ne [char]0){if($c -eq '\'){$i++;continue};if($c -eq $quoted){$quoted=[char]0};continue}
        if($c -eq '/' -and $next -eq '/'){$lineComment=$true;$i++;continue}
        if($c -eq '/' -and $next -eq '*'){$blockComment=$true;$i++;continue}
        if($c -eq '"' -or $c -eq "'"){$quoted=$c;continue}
        if($c -eq '{'){$depth++}
        if($c -eq '}'){$depth--;if($depth -eq 0){return $Text.Substring($found.Index,$i-$found.Index+1)}}
    }
    throw "Unterminated production function $Name"
}

$methods = @('sema_lazy_load_pending_until_symbol','sema_symbol_variants_known','sema_is_core_prelude_symbol') | ForEach-Object { Extract-Function $sema $_ }
$all = $sema + "`n" + $policy
$queue = [Collections.Generic.Queue[string]]::new()
foreach($name in @('generic_call_base','module_last_segment','intern_cat2','intern_cat4','str_view_range')){$queue.Enqueue($name)}
$seen = [Collections.Generic.HashSet[string]]::new()
$functions = [Collections.Generic.List[string]]::new()
while($queue.Count -gt 0){
    $name=$queue.Dequeue()
    if(-not $seen.Add($name)){continue}
    $body=Extract-Function $all $name
    $functions.Add($body)
    foreach($call in [regex]::Matches($body,'\b(policy_\w+)\s*\(')){
        if(-not $seen.Contains($call.Groups[1].Value)){$queue.Enqueue($call.Groups[1].Value)}
    }
}
$extern=[regex]::Match($policy,'(?ms)^extern "C" \{.*?^\}').Value
$fixture=[IO.File]::ReadAllText((Join-Path $PSScriptRoot 'epoch_contract.vyx')).Replace('// PRODUCTION_METHODS',($methods -join "`n`n"))
$unit=Join-Path $runDir 'epoch.vyx'
[IO.File]::WriteAllText($unit,"module frontend_epoch_contract;`nuse std.clone;`nuse std.string;`n"+$extern+"`n"+($functions -join "`n`n")+"`n"+$fixture)
$oldPath=$env:PATH; $oldLlvm=$env:LLVM_ROOT
try {
    $env:LLVM_ROOT=Join-Path $repo 'clang'
    $env:PATH=(Split-Path $Compiler)+';'+$env:PATH
    & $Compiler --src=file $unit --emit=exe -o (Join-Path $runDir 'epoch.exe') -L (Split-Path $Compiler) -l vyx_compiler_backend *> (Join-Path $runDir 'compile.log')
    if($LASTEXITCODE -ne 0){Get-Content (Join-Path $runDir 'compile.log') -Tail 70;throw 'Epoch contract compilation failed'}
    & (Join-Path $runDir 'epoch.exe') *> (Join-Path $runDir 'run.log')
    $exitCode=$LASTEXITCODE
    Get-Content (Join-Path $runDir 'run.log')
    if($exitCode -ne 0){throw "Epoch contract failed: $exitCode"}
    [ordered]@{compiler_sha256=(Get-FileHash -LiteralPath $Compiler).Hash;sema_sha256=(Get-FileHash (Join-Path $repo 'bootstrap_compiler/src/core/sema/sema.vyx')).Hash;exit=$exitCode;scope='Production lazy lookup control flow, controlled loader/known predicates; not a full Sema integration test.'} | ConvertTo-Json | Set-Content (Join-Path $runDir 'result.json')
} finally {$env:PATH=$oldPath;$env:LLVM_ROOT=$oldLlvm}
Write-Host "artifacts: $runDir"
