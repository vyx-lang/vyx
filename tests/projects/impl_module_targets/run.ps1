param([Parameter(Mandatory=$true)][string]$Compiler)

$ErrorActionPreference = 'Stop'
if ($null -ne (Get-Variable PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue)) {
    $PSNativeCommandUseErrorActionPreference = $false
}
$fixture = $PSScriptRoot
$repo = (Resolve-Path -LiteralPath (Join-Path $fixture '../../..')).Path
$Compiler = (Resolve-Path -LiteralPath $Compiler).Path
$runRoot = Join-Path $repo ('tests/.cache/impl_module_targets_{0}_{1}' -f $PID, [DateTime]::UtcNow.Ticks)
$compilerDir = Split-Path -Parent $Compiler
$originalPath = $env:PATH
$originalLdPath = $env:LD_LIBRARY_PATH
$env:PATH = $compilerDir + [IO.Path]::PathSeparator + $env:PATH
$env:LD_LIBRARY_PATH = $compilerDir + [IO.Path]::PathSeparator + $env:LD_LIBRARY_PATH
$suffix = if ($env:OS -eq 'Windows_NT') { '.exe' } else { '' }
$records = @()
New-Item -ItemType Directory -Path $runRoot | Out-Null
try {
    foreach ($optimization in @(0,2)) {
        foreach ($mode in @('build','project-run','views-first')) {
            $project = Join-Path $runRoot ("$mode-O$optimization")
            New-Item -ItemType Directory -Path $project | Out-Null
            Copy-Item -LiteralPath (Join-Path $fixture 'Vyx.toml') -Destination $project
            Copy-Item -LiteralPath (Join-Path $fixture 'src') -Destination $project -Recurse
            if ($mode -eq 'views-first') {
                $manifest = Join-Path $project 'Vyx.toml'
                $text = (Get-Content -LiteralPath $manifest -Raw).Replace(
                    '"src/left.vyx", "src/right.vyx", "src/left_view.vyx", "src/right_view.vyx", "src/right_short_view.vyx", "src/mode.vyx", "src/mode_view.vyx"',
                    '"src/left_view.vyx", "src/right_view.vyx", "src/right_short_view.vyx", "src/mode_view.vyx", "src/mode.vyx", "src/right.vyx", "src/left.vyx"')
                [IO.File]::WriteAllText($manifest, $text, [Text.UTF8Encoding]::new($false))
            }
            $log = Join-Path $project 'verification.log'
            Push-Location $project
            try {
                if ($mode -ne 'project-run') {
                    & $Compiler build --target impl_module_targets "-O$optimization" *> $log
                    if ($LASTEXITCODE -ne 0) { throw "build failed; see $log" }
                    & (Join-Path $project ('target/impl_module_targets' + $suffix)) *>> $log
                } else {
                    & $Compiler --run=aot --src=project . --target impl_module_targets "-O$optimization" *> $log
                }
                if ($LASTEXITCODE -ne 0) { throw "$mode O$optimization failed; see $log" }
                if ((Get-Content -LiteralPath $log -Raw) -notmatch 'impl_module_targets: OK') {
                    throw "missing runtime assertion marker; see $log"
                }
                $records += @{mode=$mode; optimization=$optimization; pass=$true; log=$log}
            } finally { Pop-Location }
            Write-Output "$mode O$optimization PASS"
        }
    }
    $negative = Join-Path $runRoot 'ambiguous'
    New-Item -ItemType Directory -Path $negative | Out-Null
    Copy-Item -LiteralPath (Join-Path $fixture 'src') -Destination $negative -Recurse
    $view = Join-Path $negative 'src/left_view.vyx'
    $source = (Get-Content -LiteralPath $view -Raw).Replace('use impl_models.left;',
        "use impl_models.left;`nuse impl_models.right;").Replace('return self.read();', 'return 42;')
    [IO.File]::WriteAllText($view, $source, [Text.UTF8Encoding]::new($false))
    $sources = @('main','left','right','left_view','right_view','right_short_view','mode','mode_view') | ForEach-Object { Join-Path $negative "src/$_.vyx" }
    $sourceList = Join-Path $negative 'sources.txt'
    [IO.File]::WriteAllLines($sourceList, [string[]]$sources, [Text.UTF8Encoding]::new($false))
    $log = Join-Path $negative 'verification.log'
    & $Compiler --src=file (Join-Path $negative 'src/main.vyx') --project-unit-sources $sourceList --stop-after-sema *> $log
    if ($LASTEXITCODE -eq 0 -or (Get-Content -LiteralPath $log -Raw) -notmatch "name 'Record' is ambiguous") {
        throw "ambiguous impl target accepted or misdiagnosed; see $log"
    }
    $records += @{mode='ambiguous-target'; pass=$true; log=$log}
    Write-Output 'ambiguous-target PASS'
    @{compiler=$Compiler; sha256=(Get-FileHash -LiteralPath $Compiler -Algorithm SHA256).Hash; records=$records} |
        ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $runRoot 'results.json') -Encoding utf8
    Write-Output "Results: $runRoot"
} finally {
    $env:PATH = $originalPath
    $env:LD_LIBRARY_PATH = $originalLdPath
}
exit 0
