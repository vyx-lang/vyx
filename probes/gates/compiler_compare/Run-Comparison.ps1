param(
    [Parameter(Mandatory)][string]$Compiler,
    [string]$CppCompiler = (Join-Path $PSScriptRoot '../../../clang/bin/clang++.exe'),
    [string]$CMake = 'D:/Jetbrains/CLion/bin/cmake/win/x64/bin/cmake.exe',
    [string]$Ninja = 'D:/LLVM/bin/ninja.exe',
    [string]$LlvmNm = (Join-Path $PSScriptRoot '../../../clang/bin/llvm-nm.exe'),
    [ValidateRange(4, 4096)][int]$UnitCount = 64,
    [ValidateSet('Flat', 'Layered')][string]$Layout = 'Layered',
    [ValidateRange(2, 128)][int]$Fanout = 8,
    [ValidateRange(4, 256)][int]$Rounds = 24,
    [ValidateRange(1, 20)][int]$Repetitions = 1,
    [int[]]$Jobs = @(1, 20),
    [ValidateRange(50, 10000)][int]$SampleIntervalMs = 100,
    [string]$TargetTriple = 'x86_64-pc-windows-msvc',
    [string]$OutputDir = (Join-Path $PSScriptRoot ('.runs/compare-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff')))
)

$ErrorActionPreference = 'Stop'
if (-not $IsWindows) { throw 'This runner currently requires Windows and PowerShell 7.' }
$compilerPath = (Resolve-Path -LiteralPath $Compiler).Path
$cppPath = (Resolve-Path -LiteralPath $CppCompiler).Path
$cmakePath = (Resolve-Path -LiteralPath $CMake).Path
$ninjaPath = (Resolve-Path -LiteralPath $Ninja).Path
$nmPath = (Resolve-Path -LiteralPath $LlvmNm).Path
$matrix = [IO.Path]::GetFullPath($OutputDir)
$runsRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '.runs'))
if (-not $matrix.StartsWith($runsRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Comparison outputs must remain beneath compiler_compare/.runs/'
}
if (Test-Path -LiteralPath $matrix) { throw "Refusing to overwrite comparison: $matrix" }
if ($Jobs.Count -eq 0) { throw 'At least one worker count is required.' }
foreach ($jobCount in $Jobs) { if ($jobCount -lt 1 -or $jobCount -gt 1024) { throw "Invalid worker count: $jobCount" } }
foreach ($relative in @('measurements','requests','scripts')) { New-Item -ItemType Directory -Path (Join-Path $matrix $relative) -Force | Out-Null }
Get-ChildItem -LiteralPath $PSScriptRoot -Filter '*.ps1' -File | Copy-Item -Destination (Join-Path $matrix 'scripts')
$pwsh = (Get-Process -Id $PID).Path
$exitCode = 0
$rows = [Collections.Generic.List[object]]::new()
$toolPaths = [ordered]@{vyx=$compilerPath; cpp=$cppPath; cmake=$cmakePath; ninja=$ninjaPath; llvm_nm=$nmPath}
foreach ($name in @('vyx_compiler_backend.dll','vyx_runtime.lib','tbb12.dll')) {
    $path = Join-Path ([IO.Path]::GetDirectoryName($compilerPath)) $name
    if (Test-Path -LiteralPath $path) { $toolPaths[$name] = $path }
}
$identity = @($toolPaths.Keys | ForEach-Object {
    $item = Get-Item -LiteralPath $toolPaths[$_]
    $version = if ($_.EndsWith('.dll') -or $_.EndsWith('.lib')) { $null } else { (& $item.FullName --version 2>&1 | Out-String).Trim() }
    [ordered]@{role=$_; path=$item.FullName; size=$item.Length; sha256=(Get-FileHash $item.FullName -Algorithm SHA256).Hash; version=$version}
})
$repo = (& git -C ([IO.Path]::GetDirectoryName($compilerPath)) rev-parse --show-toplevel 2>$null | Out-String).Trim()
$sourceIdentity = $null
if ($LASTEXITCODE -eq 0 -and $repo) {
    $scope = @('bootstrap_compiler/src','bootstrap_compiler/std','bootstrap_compiler/Vyx.toml','vyx_codegen/src','vyx_codegen/include','vyx_codegen/CMakeLists.txt','tools/dci')
    $diffPath = Join-Path $matrix 'compiler-source.diff'
    & git -C $repo diff --binary --no-ext-diff --no-textconv "--output=$diffPath" HEAD -- @scope
    if ($LASTEXITCODE -ne 0) { throw 'Cannot record compiler-source diff.' }
    if (-not (Test-Path -LiteralPath $diffPath)) { [IO.File]::WriteAllText($diffPath,'') }
    $changed = @(((& git -C $repo diff --name-only -z HEAD -- @scope | Out-String) -split "`0") +
        ((& git -C $repo ls-files --others --exclude-standard -z -- @scope | Out-String) -split "`0") |
        ForEach-Object { $_.TrimEnd("`r","`n") } | Where-Object { $_ } | Sort-Object -Unique)
    $dirtyHashes = @($changed | ForEach-Object {
        $path=Join-Path $repo $_
        [ordered]@{path=$_; sha256=$(if (Test-Path -LiteralPath $path -PathType Leaf) {(Get-FileHash $path -Algorithm SHA256).Hash} else {$null})}
    })
    $sourceIdentity = [ordered]@{
        commit=(& git -C $repo rev-parse HEAD | Out-String).Trim()
        scoped_dirty_status=@(& git -C $repo status --short --untracked-files=all -- @scope)
        diff_sha256=(Get-FileHash $diffPath -Algorithm SHA256).Hash
        dirty_source_hashes=$dirtyHashes
        scope=$scope
        note='Checkout state describes candidate source, not proof that every edit is present in the tested binary.'
    }
}
[ordered]@{
    started_utc=[DateTime]::UtcNow.ToString('o'); tools=$identity; compiler_source=$sourceIdentity
    unit_count=$UnitCount; layout=$Layout; fanout=$Fanout; rounds=$Rounds; repetitions=$Repetitions; jobs=$Jobs
    target=$TargetTriple; optimization='O2'; lto=$false; sample_interval_ms=$SampleIntervalMs
    llvm_root=$env:LLVM_ROOT
    cache_policy='Fresh fixture per repetition/worker count; no-op then leaf body change then public interface change. No directories are cleaned.'
    timing_policy='CMake configure is measured separately. Vyx has no separate configure command; project planning remains included in build time.'
    verification='All leaf functions are called for three seeds and checked through main. Object symbols and executable exit status are verified after every stage.'
} | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath (Join-Path $matrix 'comparison.json') -Encoding utf8NoBOM

function Run-Observed([string]$Name,[string]$Executable,[string[]]$Arguments,[string]$Directory) {
    $output = Join-Path $matrix "measurements/$Name"
    $requestPath = Join-Path $matrix "requests/$Name.json"
    [ordered]@{Executable=$Executable; ArgumentList=$Arguments; WorkingDirectory=$Directory; OutputDir=$output; Label=$Name; SampleIntervalMs=$SampleIntervalMs} |
        ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $requestPath -Encoding utf8NoBOM
    & $pwsh -NoProfile -File (Join-Path $PSScriptRoot 'Invoke-Measurement.ps1') -RequestFile $requestPath | Out-Host
    $observedExit = $LASTEXITCODE
    if ($observedExit -ne 0) {
        $script:exitCode = $observedExit
        throw "Measured command failed with $observedExit; inspect $output"
    }
    return [pscustomobject]@{path=$output; result=(Get-Content -LiteralPath (Join-Path $output 'result.json') -Raw | ConvertFrom-Json)}
}
function Assert-ToolIdentity {
    foreach ($tool in $identity) {
        if ((Get-FileHash $tool.path -Algorithm SHA256).Hash -ne $tool.sha256) { throw "Tool changed during comparison: $($tool.path)" }
    }
}
function Verify-Artifacts([string]$Fixture,[string]$Language,[string]$ObservationPath) {
    $fixtureInfo = Get-Content -LiteralPath (Join-Path $Fixture 'fixture.json') -Raw | ConvertFrom-Json
    $project = Join-Path $Fixture $Language
    $exe = if ($Language -eq 'vyx') { Join-Path $project 'target/compiler_compare.exe' } else { Join-Path $project 'build/compiler_compare.exe' }
    & $exe
    if ($LASTEXITCODE -ne 0) { $script:exitCode=$LASTEXITCODE; throw "Every-leaf output verification failed: $exe" }
    $objectRoot = if ($Language -eq 'vyx') { Join-Path $project '.cache' } else { Join-Path $project 'build/CMakeFiles/compiler_compare.dir' }
    $objects = @(Get-ChildItem -LiteralPath $objectRoot -Recurse -File -Filter '*.obj' | Sort-Object FullName)
    if ($objects.Count -eq 0) { throw "No compiled objects: $objectRoot" }
    $responsePath = Join-Path $ObservationPath 'objects.rsp'
    @('--defined-only','--print-file-name') + @($objects | ForEach-Object { '"' + $_.FullName.Replace('\','/') + '"' }) |
        Set-Content -LiteralPath $responsePath -Encoding utf8NoBOM
    $symbols = @(& $nmPath "@$responsePath" 2>&1)
    if ($LASTEXITCODE -ne 0) { throw "llvm-nm failed: $responsePath" }
    $symbols | Set-Content -LiteralPath (Join-Path $ObservationPath 'object-symbols.txt') -Encoding utf8NoBOM
    $text = $symbols -join "`n"
    foreach ($symbol in $fixtureInfo.expected_symbol_names) {
        $symbolPattern = [regex]::Escape($symbol)
        if ($Language -eq 'vyx') { $symbolPattern += '|' + [regex]::Escape($symbol.Replace('_','_5F')) }
        if ($text -notmatch $symbolPattern) { throw "Expected generated code missing for $symbol in $Language objects" }
    }
    foreach ($object in $objects) {
        $escapedName = [regex]::Escape($object.Name)
        if (-not @($symbols | Where-Object { $_ -match $escapedName -and $_ -match '\s[Tt]\s' }).Count) {
            throw "Object contains no defined text symbol: $($object.FullName)"
        }
    }
    $files = @($objects) + @(Get-Item -LiteralPath $exe)
    if ($Language -eq 'vyx') {
        $files += @(Get-ChildItem -LiteralPath (Join-Path $project 'target') -Recurse -Force -File -Filter '*.vyi')
        Copy-Item -LiteralPath (Join-Path $objectRoot '.vyx_cache') -Destination (Join-Path $ObservationPath 'build-records.txt')
    } else {
        Copy-Item -LiteralPath (Join-Path $project 'build/compile_commands.json') -Destination (Join-Path $ObservationPath 'compile-commands.json')
    }
    [ordered]@{executable_exit=0; objects=$objects.Count; checked_symbols=$fixtureInfo.expected_symbol_names.Count; output_files=@($files | ForEach-Object {
        [ordered]@{path=[IO.Path]::GetRelativePath($Fixture,$_.FullName); bytes=$_.Length; sha256=(Get-FileHash $_.FullName -Algorithm SHA256).Hash}
    })} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $ObservationPath 'artifacts.json') -Encoding utf8NoBOM
}
function Save-Row([object]$Observation,[string]$Language,[string]$Stage,[int]$Repetition,[int]$Workers,[object[]]$Tasks) {
    $Tasks | Export-Csv -LiteralPath (Join-Path $Observation.path 'tasks.csv') -NoTypeInformation -Encoding utf8NoBOM
    $s = $Observation.result.summary
    $rows.Add([pscustomobject][ordered]@{
        language=$Language; stage=$Stage; repetition=$Repetition; jobs=$Workers; exit_code=$s.exit_code; wall_seconds=$s.wall_seconds
        parent_peak_rss_mib=$s.parent_peak_rss_bytes/1MB; parent_peak_private_mib=$s.parent_peak_private_bytes/1MB
        tree_peak_rss_mib=$s.tree_sampled_peak_rss_bytes/1MB; tree_peak_private_mib=$s.tree_sampled_peak_private_bytes/1MB
        observed_cpu_seconds=$s.observed_cpu_seconds; peak_active_children=$s.peak_active_children
        peak_vyx_children=$s.peak_active_vyx_compiler_children
        peak_native_compiler_children=$s.peak_active_native_compiler_children
        actual_tasks=$(if ($Stage -eq 'configure') {$null} else {$Tasks.Count})
        interface_tasks=@($Tasks | Where-Object {$_.kind -eq 'interface'}).Count
        object_tasks=@($Tasks | Where-Object {$_.kind -eq 'object'}).Count
        link_tasks=@($Tasks | Where-Object {$_.kind -eq 'link'}).Count
        result_file=(Join-Path $Observation.path 'result.json')
    })
    $rows | Export-Csv -LiteralPath (Join-Path $matrix 'runs.csv') -NoTypeInformation -Encoding utf8NoBOM
}
try {
    for ($rep=1; $rep -le $Repetitions; $rep++) {
        $order=@($Jobs); if (($rep % 2) -eq 0) { [Array]::Reverse($order) }
        foreach ($workers in $order) {
            Assert-ToolIdentity
            $prefix="rep$rep-j$workers"
            $fixture = & (Join-Path $PSScriptRoot 'New-PairedFixture.ps1') -UnitCount $UnitCount -Layout $Layout -Fanout $Fanout -Rounds $Rounds -OutputDir (Join-Path $matrix "fixtures/$prefix")
            $cppProject=Join-Path $fixture 'cpp'; $vyxProject=Join-Path $fixture 'vyx'
            $configureArgs=@('-S',$cppProject,'-B',(Join-Path $cppProject 'build'),'-G','Ninja',"-DCMAKE_MAKE_PROGRAM=$ninjaPath","-DCMAKE_CXX_COMPILER=$cppPath",'-DCMAKE_BUILD_TYPE=Release','-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG',"-DCMAKE_CXX_FLAGS=--target=$TargetTriple -fno-lto",'-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld -fno-lto','-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF')
            $configure=Run-Observed "$prefix-cpp-configure" $cmakePath $configureArgs $cppProject
            Save-Row $configure 'cpp' 'configure' $rep $workers @()
            foreach ($stage in @('cold','noop','leaf-body','public-interface')) {
                if ($stage -eq 'leaf-body') { & (Join-Path $PSScriptRoot 'Apply-FixtureChange.ps1') -FixtureDir $fixture -Kind LeafBody }
                if ($stage -eq 'public-interface') { & (Join-Path $PSScriptRoot 'Apply-FixtureChange.ps1') -FixtureDir $fixture -Kind PublicInterface }
                $languages=if (($rep % 2) -eq 0) { @('cpp','vyx') } else { @('vyx','cpp') }
                foreach ($language in $languages) {
                    Assert-ToolIdentity
                    $tasks=@()
                    if ($language -eq 'vyx') {
                        $observation=Run-Observed "$prefix-vyx-$stage" $compilerPath @('build','--target','compiler_compare',"-j$workers",'-O2','--triplet',$TargetTriple) $vyxProject
                        $lines=Get-Content -LiteralPath (Join-Path $observation.path 'stdout.log')
                        foreach ($line in $lines) {
                            $kind=if ($line -match '\] compile .*\.vyi') {'interface'} elseif ($line -match '\] compile .*\.obj') {'object'} elseif ($line -match '\] link ') {'link'} else {$null}
                            if ($kind) { $tasks += [pscustomobject]@{kind=$kind; record=$line} }
                        }
                    } else {
                        $ninjaLog=Join-Path $cppProject 'build/.ninja_log'
                        $previous=@(); if (Test-Path -LiteralPath $ninjaLog) { $previous=@(Get-Content -LiteralPath $ninjaLog) }
                        $observation=Run-Observed "$prefix-cpp-$stage" $cmakePath @('--build',(Join-Path $cppProject 'build'),'--parallel',"$workers",'--verbose') $cppProject
                        $current=@(Get-Content -LiteralPath $ninjaLog)
                        if ($current.Count -lt $previous.Count) { throw 'Ninja log compacted unexpectedly; cannot count appended tasks reliably.' }
                        $current | Set-Content -LiteralPath (Join-Path $observation.path 'ninja-log.txt') -Encoding utf8NoBOM
                        foreach ($record in @($current | Select-Object -Skip $previous.Count)) {
                            if ($record.StartsWith('#') -or [string]::IsNullOrWhiteSpace($record)) { continue }
                            $fields=$record -split "`t"
                            $kind=if ($fields[3] -like '*.obj') {'object'} elseif ($fields[3] -like '*.exe') {'link'} else {'other'}
                            $tasks += [pscustomobject]@{kind=$kind; record=$record}
                        }
                    }
                    Save-Row $observation $language $stage $rep $workers $tasks
                    Verify-Artifacts $fixture $language $observation.path
                }
            }
        }
    }
} catch {
    if ($exitCode -eq 0) { $exitCode=1 }
    Write-Error $_ -ErrorAction Continue
} finally {
    if ($rows.Count -gt 0) { $rows | Format-Table language,stage,jobs,wall_seconds,tree_peak_rss_mib,object_tasks,link_tasks -AutoSize }
    if (Test-Path -LiteralPath (Join-Path $matrix 'runs.csv')) {
        & (Join-Path $PSScriptRoot 'Summarize-Comparison.ps1') -InputDir $matrix
    }
    Write-Host "Comparison artifacts: $matrix"
}
exit $exitCode
