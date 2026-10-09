param(
    [Parameter(Mandatory=$true)][string]$Compiler,
    [string]$OutputDir = '',
    [string]$CaseFilter = ''
)
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$Compiler = (Resolve-Path -LiteralPath $Compiler).Path
$fixture = Join-Path $PSScriptRoot 'storage_paths'
if (-not $OutputDir) { $OutputDir = Join-Path $PSScriptRoot ('.runs/storage-paths-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff')) }
$runRoot = [IO.Path]::GetFullPath($OutputDir)
if (Test-Path -LiteralPath $runRoot) { throw "Refusing to overwrite existing output: $runRoot" }
[void][IO.Directory]::CreateDirectory($runRoot)
. (Join-Path $repoRoot 'bootstrap_compiler/scripts/VyxTestProcess.ps1')
$identity = [ordered]@{ compiler=$Compiler; sha256=(Get-FileHash -LiteralPath $Compiler -Algorithm SHA256).Hash }
$results = [Collections.Generic.List[object]]::new()
$failures = [Collections.Generic.List[string]]::new()
$evidence = [ordered]@{}
$savedPath = $env:PATH
$savedLlvmRoot = $env:LLVM_ROOT
$savedCodegenUnits = $env:VYX_CODEGEN_UNITS
$env:VYX_CODEGEN_UNITS = '1'
if (-not $env:LLVM_ROOT -and (Test-Path -LiteralPath (Join-Path $repoRoot 'clang'))) { $env:LLVM_ROOT = Join-Path $repoRoot 'clang' }
if ($env:LLVM_ROOT) { $env:PATH = (Join-Path $env:LLVM_ROOT 'bin') + [IO.Path]::PathSeparator + $env:PATH }

function Write-PathText([string]$Path, [string]$Text) {
    [IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false))
}

function Invoke-PathProcess([string]$Name, [string]$File, [string]$Directory, [string[]]$Arguments, [string]$ExpectedOutput = '', [switch]$MissingOwner, [switch]$AmbiguousGlobal, [string]$ExpectedDiagnostic = '') {
    $stdout = Join-Path $runRoot "$Name.stdout.log"
    $stderr = Join-Path $runRoot "$Name.stderr.log"
    $process = Invoke-VyxProcess -FilePath $File -WorkingDirectory $Directory -TimeoutSec 120 `
        -ArgumentList $Arguments -StdoutLog $stdout -StderrLog $stderr -DialogLog (Join-Path $runRoot "$Name.dialog.log")
    $diagnostics = [IO.File]::ReadAllText($stdout) + [IO.File]::ReadAllText($stderr)
    $passed = $process.ExitCode -eq 0
    if ($ExpectedOutput) { $passed = $passed -and $diagnostics -match ('(?m)^' + [regex]::Escape($ExpectedOutput) + '\r?$') }
    if ($MissingOwner) { $passed = $process.ExitCode -ne 0 -and $diagnostics -match '(?i)(undefined|unresolved)' -and $diagnostics -match 'hidden(?:_|_5F)counter' }
    if ($AmbiguousGlobal) {
        $passed = $process.ExitCode -ne 0 -and $diagnostics -match '(?i)ambig' -and $diagnostics -match 'counter' `
            -and $diagnostics -match 'AmbiguousMain\.vyx:\d+:\d+:'
    }
    if ($ExpectedDiagnostic) { $passed = $process.ExitCode -ne 0 -and $diagnostics.Contains($ExpectedDiagnostic) }
    $results.Add([ordered]@{
        variant=$Name; exit_code=$process.ExitCode; passed=$passed; file=$File; directory=$Directory
        arguments=$Arguments; codegen_units=$env:VYX_CODEGEN_UNITS; expected_diagnostic=$ExpectedDiagnostic; stdout=$stdout; stderr=$stderr
    })
    Write-Host "$Name passed=$passed exit=$($process.ExitCode)"
    if (-not $passed) { Write-Host $diagnostics; throw "$Name failed" }
}

function Invoke-PathCase([string]$Name, [scriptblock]$Body) {
    if ($CaseFilter -and $Name -notmatch $CaseFilter) { return }
    try { & $Body }
    catch { $failures.Add("${Name}: $($_.Exception.Message)"); Write-Host "FAIL ${Name}: $($_.Exception.Message)" }
}

function Test-AssociatedIrDeterminism([string]$Module, [string]$Directory, [string[]]$CommonArguments) {
    $irPath = Join-Path $Directory "$Module.ll"
    $arguments = $CommonArguments + @('--emit=ir', '--export-top-level-roots', '-o', $irPath)
    $expected = [ordered]@{
        'mir.global.AssociatedStorage.AssociatedStorage_2EBox_3A_3A_3Ci32_3E_2EVALUE' = 17
        'mir.global.AssociatedStorage.AssociatedStorage_2EBox_3A_3A_3Ci64_3E_2EVALUE' = 17
        'mir.global.AssociatedStorage.AssociatedStorage_2ESecond_3A_3A_3Ci32_3E_2EVALUE' = 23
        'mir.global.AssociatedStorage.AssociatedStorage_2ESecond_3A_3A_3Ci64_3E_2EVALUE' = 23
    }
    $runs = [Collections.Generic.List[object]]::new()
    $firstHash = ''
    $passed = $true
    for ($iteration = 1; $iteration -le 16; $iteration++) {
        # Keep source, arguments and output pathname identical. A fresh compiler
        # process must reproduce all four closed owners on every invocation.
        if (Test-Path -LiteralPath $irPath) { Remove-Item -LiteralPath $irPath }
        Invoke-PathProcess "$Module-ir-$iteration" $Compiler $Directory $arguments
        $ir = [IO.File]::ReadAllText($irPath)
        $hash = (Get-FileHash -LiteralPath $irPath -Algorithm SHA256).Hash
        if (-not $firstHash) {
            $firstHash = $hash
            Copy-Item -LiteralPath $irPath -Destination (Join-Path $Directory "$Module.baseline.ll")
        }
        $globalCount = ([regex]::Matches($ir, '(?m)^@mir\.global\.AssociatedStorage\.')).Count
        $shapePassed = $globalCount -eq $expected.Count
        foreach ($symbol in $expected.Keys) {
            $pattern = '(?m)^@' + [regex]::Escape($symbol) + ' = (?:weak_odr|linkonce_odr) constant i32 ' + $expected[$symbol] + '(?:,|\r?$)'
            if ($ir -notmatch $pattern) { $shapePassed = $false }
        }
        $hasOpenOwner = $ir -match '@mir\.global\.AssociatedStorage\.AssociatedStorage_2E(?:Box|Second)_2EVALUE\b'
        $shapePassed = $shapePassed -and -not $hasOpenOwner
        $stable = $hash -eq $firstHash
        $runs.Add([ordered]@{ iteration=$iteration; sha256=$hash; closed_owners=$globalCount; shape_passed=$shapePassed; stable=$stable; has_open_owner=$hasOpenOwner })
        $evidence.associated_ir[$Module] = [ordered]@{ ir=$irPath; expected_owners=$expected; runs=@($runs.ToArray()) }
        if (-not $shapePassed -or -not $stable) {
            $passed = $false
            Copy-Item -LiteralPath $irPath -Destination (Join-Path $Directory "$Module.failure-$iteration.ll")
            Write-Host "$Module iteration ${iteration}: shape=$shapePassed stable=$stable open_owner=$hasOpenOwner"
        }
    }
    $results.Add([ordered]@{ variant="$Module-ir-determinism"; kind='ir-contract'; exit_code=$null; passed=$passed; ir=$irPath; repetitions=$runs.Count; sha256=$firstHash })
    Write-Host "$Module-ir-determinism passed=$passed repetitions=$($runs.Count) sha256=$firstHash"
    if (-not $passed) { throw "$Module repeated IR changed or lost closed associated-constant storage" }
}

try {
    Invoke-PathCase 'shallow-const-initializer-rejected' {
        $case = Join-Path $runRoot 'shallow-const'
        $interfaces = Join-Path $case 'interfaces'
        [void][IO.Directory]::CreateDirectory($interfaces)
        Copy-Item -LiteralPath (Join-Path $fixture 'StorageConstant.vyi') -Destination $interfaces
        Copy-Item -LiteralPath (Join-Path $fixture 'ShallowConstantConsumer.vyx') -Destination $case
        Invoke-PathProcess 'shallow-const-ir-rejected' $Compiler $case @('--src=file', 'ShallowConstantConsumer.vyx', '--interface-root', 'interfaces',
            '--emit=ir', '-o', 'unavailable.ll') -ExpectedDiagnostic 'interface static StorageConstant.LIMIT has no initializer available for constant evaluation'
    }

    Invoke-PathCase 'block-module-storage' {
        $case = Join-Path $runRoot 'block-module-storage'
        [void][IO.Directory]::CreateDirectory($case)
        Copy-Item -LiteralPath (Join-Path $fixture 'BlockModuleStorage.vyx') -Destination $case
        Invoke-PathProcess 'block-module-ir' $Compiler $case @('--src=file', 'BlockModuleStorage.vyx', '--emit=ir', '--export-top-level-roots', '-o', 'block.ll')
        $irPath = Join-Path $case 'block.ll'
        $ir = [IO.File]::ReadAllText($irPath)
        $expected = [ordered]@{
            'mir.global.BlockStorageRoot.root_5Fbefore' = 5
            'mir.global.StorageLeft.value_5Fleft' = 17
            'mir.global.StorageLeft_2ENested.value_5Fnested' = 23
            'mir.global.StorageLeft.left_5Fafter' = 19
            'mir.global.StorageRight.value_5Fright' = 29
            'mir.global.StorageRight_2EExplicitScope.explicit_5Fscope' = 37
            'mir.global.BlockStorageRoot.root_5Fafter' = 31
        }
        $globalCount = ([regex]::Matches($ir, '(?m)^@mir\.global\.')).Count
        $passed = $globalCount -eq $expected.Count
        foreach ($symbol in $expected.Keys) {
            $pattern = '(?m)^@' + [regex]::Escape($symbol) + ' = (?:dso_local )?global i32 ' + $expected[$symbol] + '(?:,|\r?$)'
            if ($ir -notmatch $pattern) { $passed = $false; Write-Host "Missing block storage definition: $symbol = $($expected[$symbol])" }
        }
        $results.Add([ordered]@{ variant='block-storage-owners'; kind='ir-contract'; exit_code=$null; passed=$passed; ir=$irPath; global_count=$globalCount })
        $evidence.block_global_initializers = $expected
        Write-Host "block-storage-owners passed=$passed globals=$globalCount"
        if (-not $passed) { throw 'Block module storage ownership/initializers differ from the source scopes' }
    }

    Invoke-PathCase 'full-interface-associated-const' {
        $evidence.associated_ir = [ordered]@{}
        $case = Join-Path $runRoot 'associated-const'
        $interfaces = Join-Path $case 'interfaces'
        [void][IO.Directory]::CreateDirectory($interfaces)
        $producer = Join-Path $case 'AssociatedStorage.vyx'
        Copy-Item -LiteralPath (Join-Path $fixture 'AssociatedStorage.vyx') -Destination $producer
        Invoke-PathProcess 'associated-full-interface' $Compiler $case @('--src=file', 'AssociatedStorage.vyx', '--emit=vyi', '-o', 'interfaces/AssociatedStorage.vyi')
        $surface = [IO.File]::ReadAllText((Join-Path $interfaces 'AssociatedStorage.vyi'))
        if ($surface -notmatch 'VYX_TEMPLATE_ARTIFACT_BEGIN v=1' -or $surface -notmatch 'payload=[0-9A-F]{64}') {
            throw 'AssociatedStorage full interface lacks its versioned template artifact'
        }
        Move-Item -LiteralPath $producer -Destination ($producer + '.archived')
        $objects = @()
        foreach ($module in @('AssociatedA', 'AssociatedB')) {
            $directory = Join-Path $case $module
            [void][IO.Directory]::CreateDirectory($directory)
            $source = Join-Path $directory "$module.vyx"
            Copy-Item -LiteralPath (Join-Path $fixture "$module.vyx") -Destination $source
            $common = @('--src=file', "$module.vyx", '--interface-root', '../interfaces')
            Invoke-PathProcess "$module-interface" $Compiler $directory ($common + @('--emit=vyi', '--vyi-shallow', '-o', "../interfaces/$module.vyi"))
            Test-AssociatedIrDeterminism $module $directory $common
            $obj = Join-Path $directory "$module.obj"
            Invoke-PathProcess "$module-object" $Compiler $directory ($common + @('--emit=obj', '--export-top-level-roots', '-o', $obj))
            $objects += $obj
            Move-Item -LiteralPath $source -Destination ($source + '.archived')
            if (@(Get-ChildItem -LiteralPath $case -Recurse -File -Filter '*.vyx').Count -ne 0) { throw 'Associated constant interface isolation violated' }
        }
        $app = Join-Path $case 'app'
        [void][IO.Directory]::CreateDirectory($app)
        Copy-Item -LiteralPath (Join-Path $fixture 'AssociatedMain.vyx') -Destination $app
        # Associated constants are specialized with the generic type. Neither
        # consumer may depend on a nonexistent native producer specialization.
        Invoke-PathProcess 'associated-consumers-link' $Compiler $app @('--src=file', 'AssociatedMain.vyx', '--interface-root', '../interfaces',
            '--emit=exe', '-o', 'associated.exe', '--link-obj', $objects[0], '--link-obj', $objects[1])
        Invoke-PathProcess 'associated-consumers-run' (Join-Path $app 'associated.exe') $app @() 'associated const storage OK'
    }

    foreach ($scopeCase in @('unity-ambiguous-global', 'unity-own-global')) {
        Invoke-PathCase $scopeCase {
            $case = Join-Path $runRoot $scopeCase
            [void][IO.Directory]::CreateDirectory($case)
            $main = if ($scopeCase -eq 'unity-ambiguous-global') { 'AmbiguousMain.vyx' } else { 'OwnGlobalMain.vyx' }
            foreach ($file in @($main, 'ScopeLeft.vyx', 'ScopeRight.vyx')) {
                Copy-Item -LiteralPath (Join-Path $fixture $file) -Destination $case
            }
            Write-PathText (Join-Path $case 'unit.sources') "ScopeLeft.vyx`nScopeRight.vyx`n"
            if ($scopeCase -eq 'unity-ambiguous-global') {
                Invoke-PathProcess 'ambiguous-global-rejected' $Compiler $case @('--src=file', $main, '--project-unit-sources', 'unit.sources',
                    '--emit=exe', '-o', 'ambiguous.exe') -AmbiguousGlobal
            } else {
                Invoke-PathProcess 'own-global-link' $Compiler $case @('--src=file', $main, '--project-unit-sources', 'unit.sources', '--emit=exe', '-o', 'own.exe')
                Invoke-PathProcess 'own-global-run' (Join-Path $case 'own.exe') $case @() 'own global scope OK'
            }
        }
    }

    foreach ($order in @('global-first', 'function-first')) {
        Invoke-PathCase "scoped-symbol-order-$order" {
            $case = Join-Path $runRoot "scoped-symbol-order-$order"
            [void][IO.Directory]::CreateDirectory($case)
            foreach ($file in @('ScopedOrderMain.vyx', 'ScopedValue.vyx', 'ScopedCallable.vyx')) {
                Copy-Item -LiteralPath (Join-Path $fixture $file) -Destination $case
            }
            $sources = if ($order -eq 'global-first') { "ScopedValue.vyx`nScopedCallable.vyx`n" } else { "ScopedCallable.vyx`nScopedValue.vyx`n" }
            Write-PathText (Join-Path $case 'unit.sources') $sources
            Invoke-PathProcess "scoped-$order-link" $Compiler $case @('--src=file', 'ScopedOrderMain.vyx', '--project-unit-sources', 'unit.sources',
                '--emit=exe', '-o', 'scoped.exe')
            Invoke-PathProcess "scoped-$order-run" (Join-Path $case 'scoped.exe') $case @() 'scoped symbol order OK'
        }
    }

    Invoke-PathCase 'full-interface-private-global' {
        $case = Join-Path $runRoot 'full-interface'
        $interfaces = Join-Path $case 'interfaces'
        [void][IO.Directory]::CreateDirectory($interfaces)
        $objects = @()
        foreach ($module in @('PrivateStorage', 'GenericA', 'GenericB')) {
            $directory = Join-Path $case $module
            [void][IO.Directory]::CreateDirectory($directory)
            $source = Join-Path $directory "$module.vyx"
            Copy-Item -LiteralPath (Join-Path $fixture "$module.vyx") -Destination $source
            $common = @('--src=file', "$module.vyx", '--interface-root', '../interfaces')
            Invoke-PathProcess "$module-full-interface" $Compiler $directory ($common + @('--emit=vyi', '-o', "../interfaces/$module.vyi"))
            if ($module -eq 'PrivateStorage') {
                $surface = [IO.File]::ReadAllText((Join-Path $interfaces "$module.vyi"))
                if ($surface -notmatch 'VYX_TEMPLATE_ARTIFACT_BEGIN v=1' -or $surface -notmatch 'payload=[0-9A-F]{64}') {
                    throw 'PrivateStorage full interface lacks its versioned template artifact'
                }
                if ($surface -match '(?m)^public (let|var|const) hidden_counter') { throw 'Private storage leaked into public interface surface' }
                $evidence.private_template_artifact = Join-Path $interfaces "$module.vyi"
            }
            $obj = Join-Path $directory "$module.obj"
            Invoke-PathProcess "$module-object" $Compiler $directory ($common + @('--emit=obj', '--export-top-level-roots', '-o', $obj))
            $objects += $obj
            Move-Item -LiteralPath $source -Destination ($source + '.archived')
            if (@(Get-ChildItem -LiteralPath $case -Recurse -File -Filter '*.vyx').Count -ne 0) { throw 'Full interface isolation violated' }
        }
        $app = Join-Path $case 'app'
        [void][IO.Directory]::CreateDirectory($app)
        Copy-Item -LiteralPath (Join-Path $fixture 'GenericMain.vyx') -Destination $app
        $arguments = @('--src=file', 'GenericMain.vyx', '--interface-root', '../interfaces', '--emit=exe', '-o', 'generic.exe')
        foreach ($obj in $objects) { $arguments += @('--link-obj', $obj) }
        Invoke-PathProcess 'generic-link' $Compiler $app $arguments
        Invoke-PathProcess 'generic-run' (Join-Path $app 'generic.exe') $app @() 'private generic storage OK'
        Write-PathText (Join-Path $app 'MissingPrivateOwner.vyx') "module MissingPrivateOwner;`nuse GenericA;`nfn main() -> i32 { return first(); }`n"
        Invoke-PathProcess 'generic-missing-owner' $Compiler $app @('--src=file', 'MissingPrivateOwner.vyx', '--interface-root', '../interfaces',
            '--emit=exe', '-o', 'missing.exe', '--link-obj', $objects[1]) -MissingOwner
    }

    Invoke-PathCase 'source-import' {
        $case = Join-Path $runRoot 'source-import'
        [void][IO.Directory]::CreateDirectory($case)
        foreach ($file in @('SourceMain.vyx', 'PrivateStorage.vyx')) { Copy-Item -LiteralPath (Join-Path $fixture $file) -Destination $case }
        Invoke-PathProcess 'source-import-link' $Compiler $case @('--src=file', 'SourceMain.vyx', '--module-source', 'PrivateStorage.vyx', '--emit=exe', '-o', 'source.exe')
        Invoke-PathProcess 'source-import-run' (Join-Path $case 'source.exe') $case @() 'source import storage OK'
    }

    Invoke-PathCase 'root-partition' {
        $case = Join-Path $runRoot 'root-partition'
        [void][IO.Directory]::CreateDirectory((Join-Path $case 'interfaces'))
        $producer = Join-Path $case 'PartitionStorage.vyx'
        Copy-Item -LiteralPath (Join-Path $fixture 'PartitionStorage.vyx') -Destination $producer
        Invoke-PathProcess 'partition-interface' $Compiler $case @('--src=file', 'PartitionStorage.vyx', '--emit=vyi', '--vyi-shallow', '-o', 'interfaces/PartitionStorage.vyi')
        foreach ($root in @('partition_read', 'partition_update')) {
            Invoke-PathProcess "$root-object" $Compiler $case @('--src=file', 'PartitionStorage.vyx', '--emit=obj',
                '--export-top-level-roots', '--export-root-names', 'partition_read|partition_update', '--emit-root-names', $root, '-o', "$root.obj")
        }
        Move-Item -LiteralPath $producer -Destination ($producer + '.archived')
        $app = Join-Path $case 'app'
        [void][IO.Directory]::CreateDirectory($app)
        Copy-Item -LiteralPath (Join-Path $fixture 'PartitionMain.vyx') -Destination $app
        Invoke-PathProcess 'partition-link' $Compiler $app @('--src=file', 'PartitionMain.vyx', '--interface-root', '../interfaces', '--emit=exe',
            '--link-obj', '../partition_read.obj', '--link-obj', '../partition_update.obj', '-o', 'partition.exe')
        Invoke-PathProcess 'partition-run' (Join-Path $app 'partition.exe') $app @() 'root partition storage OK'
    }

    Invoke-PathCase 'multiple-codegen-units' {
        $env:VYX_CODEGEN_UNITS = '4'
        $case = Join-Path $runRoot 'cgu'
        [void][IO.Directory]::CreateDirectory($case)
        $projectSources = Join-Path $repoRoot 'tests/projects/global_module_contract/src'
        $sources = @()
        foreach ($source in Get-ChildItem -LiteralPath $projectSources -File -Filter '*.vyx') {
            Copy-Item -LiteralPath $source.FullName -Destination $case
            if ($source.Name -ne 'main.vyx') { $sources += $source.Name }
        }
        Write-PathText (Join-Path $case 'unit.sources') (($sources -join "`n") + "`n")
        Write-PathText (Join-Path $case 'decorated.sources') ((@('main.vyx') + $sources -join "`n") + "`n")
        Invoke-PathProcess 'cgu-link' $Compiler $case @('--src=file', 'main.vyx', '--project-unit-sources', 'unit.sources',
            '--vyi-decorated-sources', 'decorated.sources', '--emit=exe', '--keep-obj', '-o', 'cgu.exe')
        $list = Join-Path $case 'cgu.exe.tmp.obj.cgu.list'
        if (-not (Test-Path -LiteralPath $list)) { throw 'CGU sidecar missing; multiple codegen units were not exercised' }
        $units = @([IO.File]::ReadAllLines($list) | Where-Object { $_.Trim().Length -gt 0 })
        if ($units.Count -lt 2) { throw "Expected at least two codegen units, got $($units.Count)" }
        $evidence.cgu_object_list = $list
        $evidence.cgu_units = $units.Count
        foreach ($unit in $units) {
            $path = if ([IO.Path]::IsPathRooted($unit)) { $unit } else { Join-Path $case $unit }
            if (-not (Test-Path -LiteralPath $path)) { throw "Recorded codegen unit missing: $path" }
        }
        Invoke-PathProcess 'cgu-run' (Join-Path $case 'cgu.exe') $case @() 'global module contract OK'
    }
    if ($results.Count -eq 0) { $failures.Add("No storage path case matched: $CaseFilter") }
} finally {
    [ordered]@{ identity=$identity; case_filter=$CaseFilter; tests=@($results.ToArray()); failures=@($failures.ToArray()); evidence=$evidence; run=$runRoot } | ConvertTo-Json -Depth 9 |
        Set-Content -LiteralPath (Join-Path $runRoot 'results.json') -Encoding utf8
    $env:PATH = $savedPath
    $env:LLVM_ROOT = $savedLlvmRoot
    $env:VYX_CODEGEN_UNITS = $savedCodegenUnits
}
if ($failures.Count -gt 0) { exit 1 }
Write-Host "cross-module storage paths: PASS ($runRoot)"
