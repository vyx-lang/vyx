param([string]$Compiler = '')
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
if (-not $Compiler) { $Compiler = Join-Path $repo 'bootstrap_compiler/out/boot.exe' }
$Compiler = (Resolve-Path $Compiler).Path
$run = Join-Path $PSScriptRoot ('.runs/peers-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
$env:LLVM_ROOT = Join-Path $repo 'clang'
$results = @()
foreach ($jobs in @(1,20)) {
    $project = Join-Path $run "j$jobs"
    New-Item -ItemType Directory -Path $project -Force | Out-Null
    Copy-Item (Join-Path $PSScriptRoot 'project/*') -Destination $project -Recurse
    Push-Location $project
    try {
        & $Compiler build --target indexed_peers "-j$jobs" *> build.log
        if ($LASTEXITCODE -ne 0) { Get-Content build.log -Tail 30; throw "Peer project build failed j$jobs" }
        & ./target/indexed_peers.exe
        if ($LASTEXITCODE -ne 0) { throw "Cross-file peer call failed j$jobs" }
        $lists = @(Get-ChildItem .cache -Filter 'peer_unit_*.sources' -File)
        if ($lists.Count -ne 2) { throw "Expected two peer lists, got $($lists.Count)" }
        $before = Get-FileHash target/indexed_peers.exe
        & $Compiler build --target indexed_peers "-j$jobs" *> noop.log
        if ($LASTEXITCODE -ne 0) { throw "Peer no-op build failed j$jobs" }
        if (Select-String -Path noop.log -Pattern '\] (compile|link) ') { throw "Peer no-op scheduled work j$jobs" }
        if ((Get-FileHash target/indexed_peers.exe).Hash -ne $before.Hash) { throw "Peer no-op changed output j$jobs" }
        $results += [ordered]@{jobs=$jobs; build_exit=0; run_exit=0; noop_exit=0; peer_lists=$lists.Count; executable_sha256=$before.Hash}
    } finally { Pop-Location }
}
[ordered]@{compiler_sha256=(Get-FileHash $Compiler).Hash; cases=$results; run=$run} | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $run 'result.json')
Write-Host "Same-module peer builds and no-op checks: OK ($run)"
