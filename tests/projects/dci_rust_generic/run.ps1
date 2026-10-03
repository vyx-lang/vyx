param(
    [string]$BootstrapCompiler = "",
    [string]$RuntimeDir = "",
    [string]$Rustc = ""
)

$ErrorActionPreference = "Stop"
if ($env:OS -ne "Windows_NT") {
    Write-Host "dci_rust_generic: SKIP (fixture requires rustc x86_64-pc-windows-msvc)"
    exit 77
}

$projectRoot = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $projectRoot "..\..\..")).Path
$cache = Join-Path $projectRoot ".cache"
$target = Join-Path $projectRoot "target"
$src = Join-Path $projectRoot "src\main.vyx"
$directSource = Join-Path $projectRoot "src\direct_contracts.vyx"
$zstSource = Join-Path $projectRoot "src\zst_unsupported.vyx"
$classValueSource = Join-Path $projectRoot "src\class_value_unsupported.vyx"
$classReturnSource = Join-Path $projectRoot "src\class_return_unsupported.vyx"
$lifecycleValueSource = Join-Path $projectRoot "src\lifecycle_value_unsupported.vyx"
$hiddenParameterSource = Join-Path $projectRoot "src\hidden_parameter_unsupported.vyx"
$runtimeDeclaredSource = Join-Path $projectRoot "src\runtime_cast_declared_helper.vyx"
$bitfieldSource = Join-Path $projectRoot "src\bitfield_contracts.vyx"
$rustSource = Join-Path $projectRoot "native\lib.rs"
$dciFile = Join-Path $projectRoot "dci\rust_generic.dci"
$directDciFile = Join-Path $projectRoot "dci\direct_contracts.dci"
$zstDciFile = Join-Path $projectRoot "dci\zst_unsupported.dci"
$classValueDciFile = Join-Path $projectRoot "dci\class_value_unsupported.dci"
$stubDciFile = Join-Path $projectRoot "dci\rust_stub_backend.dci"
$stubBackendTool = Join-Path $projectRoot "tools\rust_stub_backend.py"
$dcibTool = Join-Path $repoRoot "tools\dci\dcib.py"
$nativeLib = Join-Path $cache "dci_rust_generic.lib"
$ir = Join-Path $cache "dci_rust_generic.ll"
$languageNeutralDci = Join-Path $cache "dci_rust_generic_language_neutral.dci"
$languageNeutralIr = Join-Path $cache "dci_rust_generic_language_neutral.ll"
$exe = Join-Path $target "dci_rust_generic.exe"
$directIr = Join-Path $cache "direct_contracts.ll"
$directExe = Join-Path $target "dci_direct_contracts.exe"
$bitfieldIr = Join-Path $cache "bitfield_contracts.ll"
$bitfieldExe = Join-Path $target "dci_bitfield_contracts.exe"
$stubExe = Join-Path $target "dci_rust_stub_backend.exe"
$validator = Join-Path $repoRoot "tools\dci\dci_validate.py"

if ([string]::IsNullOrWhiteSpace($BootstrapCompiler)) {
    $BootstrapCompiler = Join-Path $repoRoot "bootstrap_compiler\out\boot.exe"
}
$BootstrapCompiler = (Resolve-Path $BootstrapCompiler).Path

if ([string]::IsNullOrWhiteSpace($Rustc)) {
    $rustCommand = Get-Command rustc -ErrorAction Stop
    $Rustc = $rustCommand.Source
}
$Rustc = (Resolve-Path $Rustc).Path

if (-not [string]::IsNullOrWhiteSpace($RuntimeDir)) {
    $RuntimeDir = (Resolve-Path $RuntimeDir).Path
    $env:Path = $RuntimeDir + ";" + $env:Path
}

New-Item -ItemType Directory -Force -Path $cache | Out-Null
New-Item -ItemType Directory -Force -Path $target | Out-Null

function Fail-Test {
    param([string]$Message)
    Write-Host "FAILED: $Message"
    exit 1
}

function Assert-Test {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        Fail-Test $Message
    }
}

function Invoke-Checked {
    param([string]$FilePath, [string[]]$ArgumentList, [string]$Name)
    $out = Join-Path $cache ($Name + ".out.log")
    $err = Join-Path $cache ($Name + ".err.log")
    $effectiveArguments = Convert-DciArguments -ArgumentList $ArgumentList
    & $FilePath @effectiveArguments > $out 2> $err
    if ($LASTEXITCODE -ne 0) {
        Write-Host "FAILED: $Name"
        if (Test-Path $out) { Get-Content $out -Tail 100 }
        if (Test-Path $err) { Get-Content $err -Tail 100 }
        exit $LASTEXITCODE
    }
}

function Invoke-ExpectedFailure {
    param(
        [string]$FilePath,
        [string[]]$ArgumentList,
        [string]$Name,
        [string]$ExpectedDiagnostic
    )
    $out = Join-Path $cache ($Name + ".out.log")
    $err = Join-Path $cache ($Name + ".err.log")
    $effectiveArguments = Convert-DciArguments -ArgumentList $ArgumentList
    & $FilePath @effectiveArguments > $out 2> $err
    $exitCode = $LASTEXITCODE
    $diagnostics = @()
    if (Test-Path $out) { $diagnostics += Get-Content $out }
    if (Test-Path $err) { $diagnostics += Get-Content $err }
    $diagnosticText = $diagnostics -join "`n"
    if ($exitCode -eq 0) {
        Fail-Test "$Name unexpectedly succeeded"
    }
    # A rejected contract must be a compiler diagnostic, never a crash or an
    # arbitrary process failure that merely happened to print the right text.
    if ($exitCode -ne 1) {
        Write-Host $diagnosticText
        Fail-Test "$Name exited with $exitCode instead of the normal diagnostic exit code 1"
    }
    if ($diagnosticText.IndexOf($ExpectedDiagnostic, [StringComparison]::Ordinal) -lt 0) {
        Write-Host $diagnosticText
        Fail-Test "$Name did not report: $ExpectedDiagnostic"
    }
}

function Write-JsonUtf8 {
    param([object]$Document, [string]$Path)
    $json = $Document | ConvertTo-Json -Depth 100
    [IO.File]::WriteAllText($Path, $json, [Text.UTF8Encoding]::new($false))
}

function Get-Sha256Hex {
    param([string]$Path)
    $stream = [IO.File]::OpenRead($Path)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return [BitConverter]::ToString($sha.ComputeHash($stream)).Replace("-", "")
    } finally {
        $sha.Dispose()
        $stream.Dispose()
    }
}

# Keep JSON only as an inspectable test source.  Every compiler invocation
# receives the canonical binary contract generated from that source.
function Convert-DciJsonToDcib {
    param([string]$JsonPath)
    Assert-Test ([IO.Path]::GetExtension($JsonPath) -eq ".dci") `
        "DCIB fixture source must be a .dci diagnostic document: $JsonPath"
    $dcibPath = [IO.Path]::ChangeExtension($JsonPath, ".dcib")
    Invoke-Checked -FilePath "python" `
        -ArgumentList @($dcibTool, "encode", $JsonPath, $dcibPath) `
        -Name ("encode_" + [IO.Path]::GetFileNameWithoutExtension($JsonPath))
    return $dcibPath
}

function Convert-DciArguments {
    param([string[]]$ArgumentList)
    $converted = [System.Collections.Generic.List[string]]::new()
    for ($index = 0; $index -lt $ArgumentList.Count; $index++) {
        $argument = $ArgumentList[$index]
        $converted.Add($argument)
        if ($argument -eq "--dci") {
            Assert-Test ($index + 1 -lt $ArgumentList.Count) "--dci is missing its contract path"
            $index++
            $dciPath = $ArgumentList[$index]
            if ([IO.Path]::GetExtension($dciPath) -eq ".dci") {
                $dciPath = Convert-DciJsonToDcib -JsonPath $dciPath
            }
            Assert-Test ([IO.Path]::GetExtension($dciPath) -eq ".dcib") `
                "compiler DCI input must be .dcib: $dciPath"
            $converted.Add($dciPath)
        }
    }
    return $converted.ToArray()
}

function Normalize-DirectIr {
    param([string]$Text)
    return (($Text -split '\r?\n' | Where-Object {
        $_ -notmatch '^; ModuleID = ' -and $_ -notmatch '^source_filename = '
    }) -join "`n").TrimEnd()
}

$rustVersion = (& $Rustc --version)
$rustVerbose = (& $Rustc -vV) -join "`n"
# This fixture deliberately pins the native producer ABI to the toolchain used
# to create its static library. Update the descriptor and this guard together.
Assert-Test ($rustVersion -match '^rustc 1\.92\.0(?:\s|$)') "fixture requires rustc 1.92.0, found: $rustVersion"
Assert-Test ($rustVerbose -match '(?m)^host:\s*x86_64-pc-windows-msvc\s*$') `
    "fixture requires the x86_64-pc-windows-msvc rustc host"

Invoke-Checked -FilePath "python" `
    -ArgumentList @($validator, "--strict", $dciFile) `
    -Name "validate_dci"

Invoke-Checked -FilePath "python" `
    -ArgumentList @($validator, "--strict", $directDciFile) `
    -Name "validate_direct_contracts_dci"

Invoke-Checked -FilePath "python" `
    -ArgumentList @($validator, "--strict", $zstDciFile) `
    -Name "validate_zst_contract_dci"

Invoke-Checked -FilePath "python" `
    -ArgumentList @($validator, "--strict", $classValueDciFile) `
    -Name "validate_class_value_contract_dci"

Invoke-Checked -FilePath "python" `
    -ArgumentList @($validator, "--strict", $stubDciFile) `
    -Name "validate_rust_stub_backend_dci"

# Vyx.toml consumes this binary artifact during the external-stub project
# build below.  The adjacent .dci remains the debug/validator source.
$stubDcibFile = Convert-DciJsonToDcib -JsonPath $stubDciFile
Assert-Test (Test-Path -LiteralPath $stubDcibFile) "failed to create external stub DCIB contract"

Invoke-Checked -FilePath $Rustc `
    -ArgumentList @(
        "--crate-name", "dci_rust_generic_native",
        "--crate-type", "staticlib",
        "--edition", "2021",
        "--target", "x86_64-pc-windows-msvc",
        "-C", "panic=abort",
        "-C", "opt-level=2",
        $rustSource,
        "-o", $nativeLib
    ) `
    -Name "rustc_staticlib"

Assert-Test (Test-Path -LiteralPath $nativeLib) "rustc did not produce the native static library"

Invoke-Checked -FilePath $BootstrapCompiler `
    -ArgumentList @("--src=file", $src, "--emit=ir", "--dci", $dciFile, "-o", $ir) `
    -Name "emit_ir"

Invoke-Checked -FilePath $BootstrapCompiler `
    -ArgumentList @(
        "--src=file", $src, "--emit=exe",
        "--dci", $dciFile,
        "-o", $exe,
        "--link-obj", $nativeLib
    ) `
    -Name "emit_exe"

Assert-Test (Test-Path -LiteralPath $exe) "Vyx DCI consumer did not produce the executable"

$runOut = Join-Path $cache "run_exe.out.log"
$runErr = Join-Path $cache "run_exe.err.log"
& $exe > $runOut 2> $runErr
if ($LASTEXITCODE -ne 0) {
    Write-Host "FAILED: run_exe (exit $LASTEXITCODE)"
    if (Test-Path $runOut) { Get-Content $runOut -Tail 100 }
    if (Test-Path $runErr) { Get-Content $runErr -Tail 100 }
    exit $LASTEXITCODE
}

$actual = ((Get-Content $runOut) -join "`n").Trim()
Assert-Test ($actual -eq "dci_rust_generic OK") "unexpected executable output: $actual"

Invoke-Checked -FilePath $BootstrapCompiler `
    -ArgumentList @("--src=file", $bitfieldSource, "--emit=ir", "--dci", $dciFile, "-o", $bitfieldIr) `
    -Name "emit_bitfield_contracts_ir"

Invoke-Checked -FilePath $BootstrapCompiler `
    -ArgumentList @(
        "--src=file", $bitfieldSource, "--emit=exe",
        "--dci", $dciFile,
        "-o", $bitfieldExe,
        "--link-obj", $nativeLib
    ) `
    -Name "emit_bitfield_contracts_exe"

$bitfieldRunOut = Join-Path $cache "run_bitfield_contracts.out.log"
$bitfieldRunErr = Join-Path $cache "run_bitfield_contracts.err.log"
& $bitfieldExe > $bitfieldRunOut 2> $bitfieldRunErr
if ($LASTEXITCODE -ne 0) {
    Write-Host "FAILED: run_bitfield_contracts (exit $LASTEXITCODE)"
    if (Test-Path $bitfieldRunOut) { Get-Content $bitfieldRunOut -Tail 100 }
    if (Test-Path $bitfieldRunErr) { Get-Content $bitfieldRunErr -Tail 100 }
    exit $LASTEXITCODE
}
$bitfieldActual = ((Get-Content $bitfieldRunOut) -join "`n").Trim()
Assert-Test ($bitfieldActual -eq "dci_bitfield_contracts OK") `
    "unexpected bitfield contract executable output: $bitfieldActual"
$bitfieldIrText = Get-Content -Raw -LiteralPath $bitfieldIr
Assert-Test ([regex]::IsMatch($bitfieldIrText, 'and i32 [^\r\n]+, 7') -and `
             [regex]::IsMatch($bitfieldIrText, 'lshr i32 [^\r\n]+, 3')) `
    "generated IR is missing Descriptor-driven bitfield reads"
Assert-Test ([regex]::IsMatch($bitfieldIrText, 'and i32 [^\r\n]+, -8') -and `
             [regex]::IsMatch($bitfieldIrText, 'and i32 [^\r\n]+, -9')) `
    "generated IR is missing Descriptor-driven bitfield read-modify-write"
Assert-Test ([regex]::IsMatch($bitfieldIrText, 'lshr i32 [^\r\n]+, 28') -and `
             [regex]::IsMatch($bitfieldIrText, 'xor i32 [^\r\n]+, 8') -and `
             [regex]::IsMatch($bitfieldIrText, 'sub i32 [^\r\n]+, 8') -and `
             [regex]::IsMatch($bitfieldIrText, 'and i32 [^\r\n]+, 268435455') -and `
             [regex]::IsMatch($bitfieldIrText, 'or i32 [^\r\n]+, -536870912')) `
    "generated IR is missing signed msb0 bitfield read/write lowering"
Assert-Test ([regex]::IsMatch($bitfieldIrText, 'getelementptr(?: inbounds)? i8, ptr [^\r\n]+, i64 4')) `
    "generated IR is missing nonzero bitfield storage offset lowering"

Invoke-Checked -FilePath $BootstrapCompiler `
    -ArgumentList @("--src=file", $directSource, "--emit=ir", "--dci", $directDciFile, "-o", $directIr) `
    -Name "emit_direct_contracts_ir"

Invoke-Checked -FilePath $BootstrapCompiler `
    -ArgumentList @("--src=file", $directSource, "--emit=exe", "--dci", $directDciFile, "-o", $directExe) `
    -Name "emit_direct_contracts_exe"

$directRunOut = Join-Path $cache "run_direct_contracts.out.log"
$directRunErr = Join-Path $cache "run_direct_contracts.err.log"
& $directExe > $directRunOut 2> $directRunErr
if ($LASTEXITCODE -ne 0) {
    Write-Host "FAILED: run_direct_contracts (exit $LASTEXITCODE)"
    if (Test-Path $directRunOut) { Get-Content $directRunOut -Tail 100 }
    if (Test-Path $directRunErr) { Get-Content $directRunErr -Tail 100 }
    exit $LASTEXITCODE
}
$directActual = ((Get-Content $directRunOut) -join "`n").Trim()
Assert-Test ($directActual -eq "dci_direct_contracts OK") `
    "unexpected Direct contract executable output: $directActual"

$variantCases = @(
    @{ Name = "stub_only"; Expected = "this DCI compilation requires the Direct Consumer mode" },
    @{ Name = "translated"; Expected = "requires an unsupported propagation ABI" },
    # A shared_abi boundary with a *declared* ABI identity is now a supported
    # propagation contract (SPEC 11/16.4 realizability); the fail-closed lock
    # that remains is the malformed form: shared_abi without the ABI identity
    # must be rejected with its precise data diagnostic, never the generic
    # fallback.
    @{ Name = "shared_abi"; Expected = "requires a declared shared propagation ABI" },
    @{ Name = "hidden_inalloca"; Expected = "unsupported DCI hidden parameter contract" }
)

foreach ($case in $variantCases) {
    $variant = Get-Content -Raw -LiteralPath $directDciFile | ConvertFrom-Json
    $variantSource = $directSource
    if ($case.Name -eq "stub_only") {
        $variant.profile.consumer_modes = @("stub")
    } elseif ($case.Name -eq "translated") {
        $variant.control_flow.default_boundary = "translated"
        $variant.control_flow.propagation.mode = "translated"
        $variant.control_flow.propagation | Add-Member `
            -NotePropertyName "translator_symbol" -NotePropertyValue "contract_translate_failure"
    } elseif ($case.Name -eq "shared_abi") {
        $variant.control_flow.default_boundary = "shared_abi"
        $variant.control_flow.propagation.mode = "shared_abi"
    } elseif ($case.Name -eq "hidden_inalloca") {
        $probe = @($variant.exports.symbols | Where-Object { $_.name -eq "contract_hidden_probe" })
        Assert-Test ($probe.Count -eq 1) "missing hidden-parameter probe symbol"
        $probe[0].abi | Add-Member -NotePropertyName "hidden_parameters" `
            -NotePropertyValue @([pscustomobject]@{ passing = "inalloca"; size = 8 })
        $variantSource = $hiddenParameterSource
    }
    $variantPath = Join-Path $cache ("direct_contract_" + $case.Name + ".dci")
    Write-JsonUtf8 -Document $variant -Path $variantPath
    # shared_abi-without-abi is deliberately schema-invalid; skip the standalone
    # validator so the Core Consumer's own fail-closed rejection is what gets
    # exercised (same pattern as the malformed bitfield/ABI variants below).
    if ($case.Name -ne "shared_abi") {
        Invoke-Checked -FilePath "python" `
            -ArgumentList @($validator, "--strict", $variantPath) `
            -Name ("validate_" + $case.Name)
    }
    Invoke-ExpectedFailure -FilePath $BootstrapCompiler `
        -ArgumentList @(
            "--src=file", $variantSource, "--emit=ir",
            "--dci", $variantPath,
            "-o", (Join-Path $cache ("unexpected_" + $case.Name + ".ll"))
        ) `
        -Name ("reject_" + $case.Name) `
        -ExpectedDiagnostic $case.Expected
}

$bitfieldVariantCases = @(
    @{ Name = "missing_signed"; Remove = "signed"; Expected = "incomplete or unsupported DCI bitfield Direct contract" },
    @{ Name = "missing_bit_order"; Remove = "bit_order"; Expected = "incomplete or unsupported DCI bitfield Direct contract" },
    @{ Name = "missing_read"; Remove = "read"; Expected = "incomplete or unsupported DCI bitfield Direct contract" },
    @{ Name = "missing_write"; Remove = "write"; Expected = "incomplete or unsupported DCI bitfield Direct contract" },
    @{ Name = "target_order"; Value = "target"; Expected = "has no Direct Profile interpretation" }
)

foreach ($case in $bitfieldVariantCases) {
    $variant = Get-Content -Raw -LiteralPath $dciFile | ConvertFrom-Json
    $layout = @($variant.exports.layouts | Where-Object { $_.type_name -eq "dci_rust.Flags" })
    Assert-Test ($layout.Count -eq 1) "missing Flags layout for bitfield Consumer fail-closed test"
    $field = @($layout[0].fields | Where-Object { $_.name -eq "priority" })
    Assert-Test ($field.Count -eq 1) "missing priority bitfield for Consumer fail-closed test"
    if ($case.ContainsKey("Remove")) {
        $field[0].bitfield.PSObject.Properties.Remove($case["Remove"])
    } else {
        $field[0].bitfield.bit_order = $case["Value"]
    }
    $variantPath = Join-Path $cache ("bitfield_" + $case.Name + ".dci")
    Write-JsonUtf8 -Document $variant -Path $variantPath
    # Deliberately skip dci_validate.py: malformed contracts must also be
    # rejected by the Core Consumer when no standalone validation is run.
    Invoke-ExpectedFailure -FilePath $BootstrapCompiler `
        -ArgumentList @(
            "--src=file", $bitfieldSource, "--emit=ir",
            "--dci", $variantPath,
            "-o", (Join-Path $cache ("unexpected_bitfield_" + $case.Name + ".ll"))
        ) `
        -Name ("reject_bitfield_" + $case.Name) `
        -ExpectedDiagnostic $case.Expected
}

$incompleteTable = Get-Content -Raw -LiteralPath $directDciFile | ConvertFrom-Json
$tableLayout = @($incompleteTable.exports.layouts | Where-Object {
    $_.type_name -eq "contract_table.Derived"
})
Assert-Test ($tableLayout.Count -eq 1) "missing table-adjustment layout for Consumer fail-closed test"
$tableLayout[0].bases[0].adjustment.PSObject.Properties.Remove("displacement_base_offset")
$incompleteTablePath = Join-Path $cache "incomplete_table_adjustment.dci"
Write-JsonUtf8 -Document $incompleteTable -Path $incompleteTablePath
# Deliberately skip dci_validate.py: the Core Consumer must reject malformed
# adjustment contracts even when callers omit the standalone validator.
Invoke-ExpectedFailure -FilePath $BootstrapCompiler `
    -ArgumentList @(
        "--src=file", $directSource, "--emit=ir",
        "--dci", $incompleteTablePath,
        "-o", (Join-Path $cache "unexpected_incomplete_table.ll")
    ) `
    -Name "reject_incomplete_table_adjustment_in_consumer" `
    -ExpectedDiagnostic "missing DCI base adjustment"

foreach ($case in @("missing", "constant")) {
    $variant = Get-Content -Raw -LiteralPath $directDciFile | ConvertFrom-Json
    $layout = @($variant.exports.layouts | Where-Object {
        $_.type_name -eq "contract_table.Derived"
    })
    Assert-Test ($layout.Count -eq 1) "missing virtual-base layout for Core rejection test"
    if ($case -eq "missing") {
        $layout[0].bases[0].PSObject.Properties.Remove("adjustment")
    } else {
        $layout[0].bases[0].adjustment = [pscustomobject]@{
            kind = "constant"
            offset = 40
            null_preserving = $true
        }
    }
    $variantPath = Join-Path $cache ("virtual_base_" + $case + "_adjustment.dci")
    Write-JsonUtf8 -Document $variant -Path $variantPath
    # Skip standalone validation to prove virtual bases cannot silently
    # degrade to a fixed offset inside the Core Consumer.
    Invoke-ExpectedFailure -FilePath $BootstrapCompiler `
        -ArgumentList @(
            "--src=file", $directSource, "--emit=ir",
            "--dci", $variantPath,
            "-o", (Join-Path $cache ("unexpected_virtual_base_" + $case + ".ll"))
        ) `
        -Name ("reject_virtual_base_" + $case + "_adjustment_in_consumer") `
        -ExpectedDiagnostic "missing DCI base adjustment"
}

$missingAbiParameter = Get-Content -Raw -LiteralPath $dciFile | ConvertFrom-Json
$missingAbiMixed = @($missingAbiParameter.exports.symbols | Where-Object {
    $_.name -eq "dci_rust_mixed_params"
})
Assert-Test ($missingAbiMixed.Count -eq 1) "missing mixed symbol for ABI completeness test"
$missingAbiMixed[0].abi.parameters = @($missingAbiMixed[0].abi.parameters | Where-Object {
    $_.index -ne 1
})
$missingAbiParameterPath = Join-Path $cache "missing_abi_parameter.dci"
Write-JsonUtf8 -Document $missingAbiParameter -Path $missingAbiParameterPath
# Skip the standalone validator to prove the Consumer has the same defense.
Invoke-ExpectedFailure -FilePath $BootstrapCompiler `
    -ArgumentList @(
        "--src=file", $src, "--emit=ir",
        "--dci", $missingAbiParameterPath,
        "-o", (Join-Path $cache "unexpected_missing_abi_parameter.ll")
    ) `
    -Name "reject_missing_abi_parameter_in_consumer" `
    -ExpectedDiagnostic "does not match 3 source parameter(s)"

$abiFailClosedCases = @(
    @{ Name = "invalid_alignment"; Expected = "must be a representable power of two" },
    @{ Name = "missing_indirect_alignment"; Expected = "indirect ABI requires alignment" },
    @{ Name = "missing_indirect_size"; Expected = "indirect ABI requires a positive byte size" },
    @{ Name = "mismatched_indirect_size"; Expected = "indirect ABI size mismatch" },
    @{ Name = "missing_sret_alignment"; Expected = "sret ABI requires alignment" },
    @{ Name = "mismatched_sret_size"; Expected = "sret ABI size mismatch" },
    @{ Name = "missing_hidden_sret_size"; Expected = "sret ABI requires a positive byte size" },
    @{ Name = "unknown_passing"; Expected = "unsupported DCI ABI passing" },
    @{ Name = "missing_passing"; Expected = "missing DCI ABI passing" },
    @{ Name = "fractional_index"; Expected = 'DCI JSON number `ABI parameter index` is not an integer' },
    @{ Name = "coerce_parameter_width"; Expected = "DCI coerce size mismatch" },
    @{ Name = "coerce_return_width"; Expected = "DCI coerce size mismatch" },
    @{ Name = "missing_coerce_size"; Expected = "coerce ABI requires a positive byte size" },
    @{ Name = "variadic"; Expected = "variadic DCI ABI is unsupported" },
    @{ Name = "unsupported_attribute"; Expected = "unsupported DCI ABI attribute" },
    @{ Name = "explicit_register"; Expected = "explicit DCI ABI register assignment is unsupported" },
    @{ Name = "shared_link_contract"; Expected = "conflicting function contracts for shared link symbol" }
)

foreach ($case in $abiFailClosedCases) {
    $variant = Get-Content -Raw -LiteralPath $dciFile | ConvertFrom-Json
    if ($case.Name -eq "invalid_alignment") {
        $symbol = @($variant.exports.symbols | Where-Object {
            $_.name -eq "dci_rust_aligned_fold"
        })
        Assert-Test ($symbol.Count -eq 1) "missing aligned fold symbol for ABI alignment test"
        $symbol[0].abi.parameters[0].alignment = 3
    } elseif ($case.Name -eq "missing_indirect_alignment") {
        $symbol = @($variant.exports.symbols | Where-Object {
            $_.name -eq "dci_rust_aligned_fold"
        })
        Assert-Test ($symbol.Count -eq 1) "missing aligned fold symbol for ABI shape test"
        $symbol[0].abi.parameters[0].PSObject.Properties.Remove("alignment")
    } elseif ($case.Name -eq "missing_indirect_size") {
        $symbol = @($variant.exports.symbols | Where-Object {
            $_.name -eq "dci_rust_aligned_fold"
        })
        Assert-Test ($symbol.Count -eq 1) "missing aligned fold symbol for ABI shape test"
        $symbol[0].abi.parameters[0].PSObject.Properties.Remove("size")
    } elseif ($case.Name -eq "mismatched_indirect_size") {
        $symbol = @($variant.exports.symbols | Where-Object {
            $_.name -eq "dci_rust_aligned_fold"
        })
        Assert-Test ($symbol.Count -eq 1) "missing aligned fold symbol for ABI shape test"
        $symbol[0].abi.parameters[0].size = 8
    } elseif ($case.Name -eq "missing_sret_alignment") {
        $symbol = @($variant.exports.symbols | Where-Object {
            $_.name -eq "dci_rust_aligned_make"
        })
        Assert-Test ($symbol.Count -eq 1) "missing aligned make symbol for sret shape test"
        $symbol[0].abi.return.PSObject.Properties.Remove("alignment")
    } elseif ($case.Name -eq "mismatched_sret_size") {
        $symbol = @($variant.exports.symbols | Where-Object {
            $_.name -eq "dci_rust_aligned_make"
        })
        Assert-Test ($symbol.Count -eq 1) "missing aligned make symbol for sret shape test"
        $symbol[0].abi.return.size = 8
    } elseif ($case.Name -eq "missing_hidden_sret_size") {
        $symbol = @($variant.exports.symbols | Where-Object {
            $_.name -eq "dci_rust_aligned_make"
        })
        Assert-Test ($symbol.Count -eq 1) "missing aligned make symbol for hidden sret test"
        $symbol[0].abi.hidden_parameters[0].PSObject.Properties.Remove("size")
    } elseif ($case.Name -eq "unknown_passing") {
        $symbol = @($variant.exports.symbols | Where-Object {
            $_.name -eq "dci_rust_flags_priority"
        })
        Assert-Test ($symbol.Count -eq 1) "missing Flags priority symbol for ABI passing test"
        $symbol[0].abi.parameters[0].passing = "contract-unknown"
    } elseif ($case.Name -eq "missing_passing") {
        $symbol = @($variant.exports.symbols | Where-Object {
            $_.name -eq "dci_rust_flags_priority"
        })
        Assert-Test ($symbol.Count -eq 1) "missing Flags priority symbol for ABI passing test"
        $symbol[0].abi.parameters[0].PSObject.Properties.Remove("passing")
    } elseif ($case.Name -eq "fractional_index") {
        $symbol = @($variant.exports.symbols | Where-Object {
            $_.name -eq "dci_rust_mixed_params"
        })
        Assert-Test ($symbol.Count -eq 1) "missing mixed symbol for fractional ABI index test"
        $symbol[0].abi.parameters[0].index = 0.5
    } elseif ($case.Name -eq "coerce_parameter_width") {
        $symbol = @($variant.exports.symbols | Where-Object {
            $_.name -eq "dci_rust_flags_priority"
        })
        Assert-Test ($symbol.Count -eq 1) "missing Flags priority symbol for coerce width test"
        $symbol[0].abi.parameters[0].coerce_to.name = "u64"
    } elseif ($case.Name -eq "coerce_return_width") {
        $symbol = @($variant.exports.symbols | Where-Object {
            $_.name -eq "dci_rust_flags_make"
        })
        Assert-Test ($symbol.Count -eq 1) "missing Flags make symbol for coerce width test"
        $symbol[0].abi.return.coerce_to.name = "u64"
    } elseif ($case.Name -eq "missing_coerce_size") {
        $symbol = @($variant.exports.symbols | Where-Object {
            $_.name -eq "dci_rust_flags_priority"
        })
        Assert-Test ($symbol.Count -eq 1) "missing Flags priority symbol for coerce shape test"
        $symbol[0].abi.parameters[0].PSObject.Properties.Remove("size")
    } elseif ($case.Name -eq "variadic") {
        $symbol = @($variant.exports.symbols | Where-Object {
            $_.name -eq "dci_rust_flags_priority"
        })
        Assert-Test ($symbol.Count -eq 1) "missing Flags priority symbol for variadic test"
        $symbol[0].abi.variadic = $true
    } elseif ($case.Name -eq "unsupported_attribute") {
        $symbol = @($variant.exports.symbols | Where-Object {
            $_.name -eq "dci_rust_flags_priority"
        })
        Assert-Test ($symbol.Count -eq 1) "missing Flags priority symbol for ABI attribute test"
        $symbol[0].abi.parameters[0] | Add-Member `
            -NotePropertyName "attributes" -NotePropertyValue @("nonnull")
    } elseif ($case.Name -eq "explicit_register") {
        # Register arrays are legal *only* for split/coerce lowerings now; put
        # them on an indirect parameter so the explicit-assignment lock still
        # fires.
        $symbol = @($variant.exports.symbols | Where-Object {
            $_.name -eq "dci_rust_aligned_fold"
        })
        Assert-Test ($symbol.Count -eq 1) "missing aligned fold symbol for ABI register test"
        $symbol[0].abi.parameters[0] | Add-Member `
            -NotePropertyName "registers" -NotePropertyValue @("rax")
    } else {
        $symbol = @($variant.exports.symbols | Where-Object {
            $_.name -eq "dci_rust_flags_enabled"
        })
        Assert-Test ($symbol.Count -eq 1) "missing Flags enabled symbol for link contract test"
        $symbol[0].link_name = "dci_rust_flags_priority"
    }
    $variantPath = Join-Path $cache ("abi_fail_closed_" + $case.Name + ".dci")
    Write-JsonUtf8 -Document $variant -Path $variantPath
    # Skip dci_validate.py deliberately: the Core Consumer must reject unsafe
    # ABI contracts even when embedded callers omit standalone validation.
    Invoke-ExpectedFailure -FilePath $BootstrapCompiler `
        -ArgumentList @(
            "--src=file", $src, "--emit=ir",
            "--dci", $variantPath,
            "-o", (Join-Path $cache ("unexpected_abi_" + $case.Name + ".ll"))
        ) `
        -Name ("reject_abi_" + $case.Name + "_in_consumer") `
        -ExpectedDiagnostic $case.Expected
}

$conflictingSymbol = Get-Content -Raw -LiteralPath $dciFile | ConvertFrom-Json
$flagsPriority = @($conflictingSymbol.exports.symbols | Where-Object {
    $_.name -eq "dci_rust_flags_priority"
})
Assert-Test ($flagsPriority.Count -eq 1) "missing Flags symbol for duplicate binding test"
$duplicateFlagsPriority = $flagsPriority[0] | ConvertTo-Json -Depth 100 | ConvertFrom-Json
# Two contract entries for the same native symbol (same link name / calling
# convention / kind / owner) with different *lowering* details are reconciled
# by the Consumer now; what must still fail closed is a conflicting NATIVE
# description, so the duplicate declares a different calling convention.
$duplicateFlagsPriority.abi.calling_convention = "stdcall"
$conflictingSymbol.exports.symbols = @($conflictingSymbol.exports.symbols) + @($duplicateFlagsPriority)
$conflictingSymbolPath = Join-Path $cache "conflicting_symbol_contract.dci"
Write-JsonUtf8 -Document $conflictingSymbol -Path $conflictingSymbolPath
Invoke-ExpectedFailure -FilePath $BootstrapCompiler `
    -ArgumentList @(
        "--src=file", $src, "--emit=ir",
        "--dci", $conflictingSymbolPath,
        "-o", (Join-Path $cache "unexpected_conflicting_symbol.ll")
    ) `
    -Name "reject_conflicting_symbol_contract_in_consumer" `
    -ExpectedDiagnostic "conflicting DCI symbol contracts"

Invoke-ExpectedFailure -FilePath $BootstrapCompiler `
    -ArgumentList @(
        "--src=file", $zstSource, "--emit=ir",
        "--dci", $zstDciFile,
        "-o", (Join-Path $cache "unexpected_zst.ll")
    ) `
    -Name "reject_zst" `
    -ExpectedDiagnostic "DCI zero-sized value layout requires an unsupported ignore ABI"

Invoke-ExpectedFailure -FilePath $BootstrapCompiler `
    -ArgumentList @(
        "--src=file", $classValueSource, "--emit=ir",
        "--dci", $classValueDciFile,
        "-o", (Join-Path $cache "unexpected_class_value.ll")
    ) `
    -Name "reject_class_value_without_materialization" `
    -ExpectedDiagnostic "DCI class-by-value indirect argument requires lifecycle materialization"

Invoke-ExpectedFailure -FilePath $BootstrapCompiler `
    -ArgumentList @(
        "--src=file", $classReturnSource, "--emit=ir",
        "--dci", $classValueDciFile,
        "-o", (Join-Path $cache "unexpected_class_return.ll")
    ) `
    -Name "reject_class_return_without_materialization" `
    -ExpectedDiagnostic "DCI class-by-value return"

$lifecycleValue = Get-Content -Raw -LiteralPath $dciFile | ConvertFrom-Json
$lifecycleFold = @($lifecycleValue.exports.symbols | Where-Object {
    $_.name -eq "dci_rust_packet_fold"
})
Assert-Test ($lifecycleFold.Count -eq 1) "missing Packet fold symbol for lifecycle materialization test"
$lifecycleFold[0].params[0].type.reference = "value"
$lifecycleFold[0].params[0].ownership = "copy"
$lifecycleFold[0].abi.parameters[0].passing = "indirect"
$lifecycleFold[0].abi.parameters[0].size = 16
$lifecycleFold[0].abi.parameters[0] | Add-Member -NotePropertyName "alignment" -NotePropertyValue 8
$lifecycleFold[0].abi.parameters[0] | Add-Member -NotePropertyName "attributes" -NotePropertyValue @("by_value")
$lifecycleValuePath = Join-Path $cache "unsupported_lifecycle_value_parameter.dci"
Write-JsonUtf8 -Document $lifecycleValue -Path $lifecycleValuePath
Invoke-Checked -FilePath "python" `
    -ArgumentList @($validator, "--strict", $lifecycleValuePath) `
    -Name "validate_unsupported_lifecycle_value_parameter"
Invoke-ExpectedFailure -FilePath $BootstrapCompiler `
    -ArgumentList @(
        "--src=file", $lifecycleValueSource, "--emit=ir",
        "--dci", $lifecycleValuePath,
        "-o", (Join-Path $cache "unexpected_lifecycle_value.ll")
    ) `
    -Name "reject_unsupported_lifecycle_value_parameter" `
    -ExpectedDiagnostic "requires lifecycle materialization"

$unprovenStableValue = Get-Content -Raw -LiteralPath $dciFile | ConvertFrom-Json
$unprovenAligned = @($unprovenStableValue.exports.layouts | Where-Object {
    $_.type_name -eq "dci_rust.AlignedPair"
})
Assert-Test ($unprovenAligned.Count -eq 1) "missing AlignedPair layout for triviality evidence test"
$unprovenAligned[0].PSObject.Properties.Remove("is_pod")
$unprovenAligned[0].PSObject.Properties.Remove("is_trivially_destructible")
$unprovenAligned[0].PSObject.Properties.Remove("lifecycle")
$unprovenStableValuePath = Join-Path $cache "unproven_stable_value.dci"
Write-JsonUtf8 -Document $unprovenStableValue -Path $unprovenStableValuePath
Invoke-ExpectedFailure -FilePath $BootstrapCompiler `
    -ArgumentList @(
        "--src=file", $src, "--emit=ir",
        "--dci", $unprovenStableValuePath,
        "-o", (Join-Path $cache "unexpected_unproven_stable_value.ll")
    ) `
    -Name "reject_unproven_stable_value_in_consumer" `
    -ExpectedDiagnostic "requires lifecycle materialization"

$lifecycleVariant = Get-Content -Raw -LiteralPath $dciFile | ConvertFrom-Json
$lifecyclePacket = @($lifecycleVariant.exports.layouts | Where-Object {
    $_.type_name -eq "dci_rust.Packet"
})
Assert-Test ($lifecyclePacket.Count -eq 1) "missing Packet lifecycle for fail-closed test"
$lifecyclePacket[0].lifecycle.destruction = "release"
$lifecyclePacket[0].lifecycle.operations | Add-Member `
    -NotePropertyName "release" `
    -NotePropertyValue $lifecyclePacket[0].lifecycle.operations.destroy
$unsupportedLifecycle = Join-Path $cache "unsupported_lifecycle_release.dci"
Write-JsonUtf8 -Document $lifecycleVariant -Path $unsupportedLifecycle
Invoke-Checked -FilePath "python" `
    -ArgumentList @($validator, "--strict", $unsupportedLifecycle) `
    -Name "validate_unsupported_lifecycle_release"
# `release` destruction is a supported contract now (shared ownership);
# release + unique ownership must still fail closed with the precise
# ownership diagnostic.
Invoke-ExpectedFailure -FilePath $BootstrapCompiler `
    -ArgumentList @(
        "--src=file", $src, "--emit=ir",
        "--dci", $unsupportedLifecycle,
        "-o", (Join-Path $cache "unexpected_lifecycle_release.ll")
    ) `
    -Name "reject_unsupported_lifecycle_release" `
    -ExpectedDiagnostic 'release-driven destruction for `dci_rust.Packet` currently requires shared ownership'

$document = Get-Content -Raw -LiteralPath $dciFile | ConvertFrom-Json
$documentText = Get-Content -Raw -LiteralPath $dciFile
$missingRuntimeCast = Get-Content -Raw -LiteralPath $dciFile | ConvertFrom-Json
$missingRuntimeCast.exports.runtime_type_operations = @(
    $missingRuntimeCast.exports.runtime_type_operations | Where-Object {
        $_.type_name -ne "dci_rust.RuntimeDerived"
    }
)
$missingRuntimeCastPath = Join-Path $cache "missing_runtime_cast.dci"
Write-JsonUtf8 -Document $missingRuntimeCast -Path $missingRuntimeCastPath
Invoke-Checked -FilePath "python" `
    -ArgumentList @($validator, "--strict", $missingRuntimeCastPath) `
    -Name "validate_missing_runtime_cast"
Invoke-ExpectedFailure -FilePath $BootstrapCompiler `
    -ArgumentList @(
        "--src=file", $src, "--emit=ir",
        "--dci", $missingRuntimeCastPath,
        "-o", (Join-Path $cache "unexpected_missing_runtime_cast.ll")
    ) `
    -Name "reject_missing_runtime_cast" `
    -ExpectedDiagnostic "has no Direct checked-downcast operation"

$ambiguousRuntimeCast = Get-Content -Raw -LiteralPath $dciFile | ConvertFrom-Json
$runtimeDerivedEntries = @($ambiguousRuntimeCast.exports.runtime_type_operations | Where-Object {
    $_.type_name -eq "dci_rust.RuntimeDerived"
})
Assert-Test ($runtimeDerivedEntries.Count -eq 1) "missing RuntimeDerived operations for ambiguity test"
$duplicateRuntimeDerived = $runtimeDerivedEntries[0] | ConvertTo-Json -Depth 100 | ConvertFrom-Json
$ambiguousRuntimeCast.exports.runtime_type_operations = `
    @($ambiguousRuntimeCast.exports.runtime_type_operations) + @($duplicateRuntimeDerived)
$ambiguousRuntimeCastPath = Join-Path $cache "ambiguous_runtime_cast.dci"
Write-JsonUtf8 -Document $ambiguousRuntimeCast -Path $ambiguousRuntimeCastPath
Invoke-ExpectedFailure -FilePath $BootstrapCompiler `
    -ArgumentList @(
        "--src=file", $src, "--emit=ir",
        "--dci", $ambiguousRuntimeCastPath,
        "-o", (Join-Path $cache "unexpected_ambiguous_runtime_cast.ll")
    ) `
    -Name "reject_ambiguous_runtime_cast_in_consumer" `
    -ExpectedDiagnostic "ambiguous DCI runtime type operations"

$consumingRuntimeCast = Get-Content -Raw -LiteralPath $dciFile | ConvertFrom-Json
$consumingRuntimeSymbol = @($consumingRuntimeCast.exports.symbols | Where-Object {
    $_.name -eq "dci_rust_runtime_checked_downcast"
})
Assert-Test ($consumingRuntimeSymbol.Count -eq 1) "missing runtime cast symbol for ownership test"
$consumingRuntimeSymbol[0].params[0].ownership = "move"
$consumingRuntimeSymbol[0].return.ownership = "owned"
$consumingRuntimeCastPath = Join-Path $cache "consuming_runtime_cast.dci"
Write-JsonUtf8 -Document $consumingRuntimeCast -Path $consumingRuntimeCastPath
Invoke-ExpectedFailure -FilePath $BootstrapCompiler `
    -ArgumentList @(
        "--src=file", $src, "--emit=ir",
        "--dci", $consumingRuntimeCastPath,
        "-o", (Join-Path $cache "unexpected_consuming_runtime_cast.ll")
    ) `
    -Name "reject_consuming_runtime_cast_in_consumer" `
    -ExpectedDiagnostic "invalid DCI checked-downcast symbol contract"

$conflictingOperationRef = Get-Content -Raw -LiteralPath $dciFile | ConvertFrom-Json
$operationRefSymbol = @($conflictingOperationRef.exports.symbols | Where-Object {
    $_.name -eq "dci_rust_runtime_checked_downcast"
})
Assert-Test ($operationRefSymbol.Count -eq 1) "missing runtime helper for operation-reference test"
$operationRefAlias = $operationRefSymbol[0] | ConvertTo-Json -Depth 100 | ConvertFrom-Json
$operationRefAlias.name = "dci_rust_runtime_checked_downcast_alias"
$operationRefAlias.abi.calling_convention = "vectorcall"
$conflictingOperationRef.exports.symbols = `
    @($conflictingOperationRef.exports.symbols) + @($operationRefAlias)
$conflictingOperationRefPath = Join-Path $cache "conflicting_operation_reference.dci"
Write-JsonUtf8 -Document $conflictingOperationRef -Path $conflictingOperationRefPath
Invoke-ExpectedFailure -FilePath $BootstrapCompiler `
    -ArgumentList @(
        "--src=file", $src, "--emit=ir",
        "--dci", $conflictingOperationRefPath,
        "-o", (Join-Path $cache "unexpected_conflicting_operation_reference.ll")
    ) `
    -Name "reject_conflicting_operation_reference_in_consumer" `
    -ExpectedDiagnostic "conflicting DCI operation symbol reference"

$runtimeDeclaredIr = Join-Path $cache "runtime_cast_declared_helper.ll"
Invoke-Checked -FilePath $BootstrapCompiler `
    -ArgumentList @(
        "--src=file", $runtimeDeclaredSource, "--emit=ir",
        "--dci", $dciFile,
        "-o", $runtimeDeclaredIr
    ) `
    -Name "emit_runtime_cast_declared_helper_ir"
$runtimeDeclaredIrText = Get-Content -Raw -LiteralPath $runtimeDeclaredIr
Assert-Test ($runtimeDeclaredIrText.IndexOf("@dci_rust_runtime_checked_downcast(", `
                                            [StringComparison]::Ordinal) -ge 0) `
    "runtime cast did not reuse the declared DCI helper symbol"
Assert-Test (-not [regex]::IsMatch($runtimeDeclaredIrText, `
                                  '@dci_rust_runtime_checked_downcast\.\d+\(')) `
    "runtime cast emitted a renamed duplicate of the declared DCI helper"

Assert-Test ($document.source.language -eq "rust") "DCI source language is not Rust"
Assert-Test ($document.profile.consumer_modes -contains "direct") "fixture is not a Direct consumer contract"
Assert-Test ($documentText.IndexOf('"cpp_type"', [StringComparison]::Ordinal) -lt 0) `
    "language-neutral fixture unexpectedly contains a C++ type annotation"
Assert-Test ($documentText.IndexOf('"mangled"', [StringComparison]::Ordinal) -lt 0) `
    "fixture must exercise link_name instead of the legacy mangled alias"

$languageNeutralDocument = $document | ConvertTo-Json -Depth 100 | ConvertFrom-Json
$languageNeutralDocument.source.language = "contract-fixture"
Write-JsonUtf8 -Document $languageNeutralDocument -Path $languageNeutralDci
Invoke-Checked -FilePath $BootstrapCompiler `
    -ArgumentList @(
        "--src=file", $src, "--emit=ir",
        "--dci", $languageNeutralDci,
        "-o", $languageNeutralIr
    ) `
    -Name "emit_language_neutral_ir"

$packetLayout = @($document.exports.layouts | Where-Object { $_.type_name -eq "dci_rust.Packet" })
Assert-Test ($packetLayout.Count -eq 1) "missing Packet layout"
Assert-Test ($packetLayout[0].lifecycle.operations.create.symbol -eq "dci_rust_packet_create") `
    "Packet create lifecycle operation is missing"
Assert-Test ($packetLayout[0].lifecycle.operations.copy.symbol -eq "dci_rust_packet_copy") `
    "Packet copy lifecycle operation is missing"
Assert-Test ($packetLayout[0].lifecycle.operations.move.symbol -eq "dci_rust_packet_move") `
    "Packet move lifecycle operation is missing"
Assert-Test ($packetLayout[0].lifecycle.operations.destroy.symbol -eq "dci_rust_packet_destroy") `
    "Packet destroy lifecycle operation is missing"
$copySymbol = @($document.exports.symbols | Where-Object { $_.name -eq "dci_rust_packet_copy" })
Assert-Test ($copySymbol.Count -eq 1) "missing Packet copy operation symbol"
Assert-Test ($copySymbol[0].params[0].type.reference -eq "pointer" -and `
             $copySymbol[0].params[0].ownership -eq "borrow") `
    "Packet copy must borrow an exact source pointer"
Assert-Test ($copySymbol[0].abi.parameters[0].passing -eq "direct") `
    "Packet copy source pointer must use direct ABI passing"
$foldSymbol = @($document.exports.symbols | Where-Object { $_.name -eq "dci_rust_packet_fold" })
Assert-Test ($foldSymbol.Count -eq 1 -and `
             $foldSymbol[0].params[0].type.reference -eq "pointer" -and `
             $foldSymbol[0].params[0].ownership -eq "borrow" -and `
             $foldSymbol[0].abi.parameters[0].passing -eq "direct") `
    "Packet fold must use a directly-passed borrowed pointer"

$flagsLayout = @($document.exports.layouts | Where-Object { $_.type_name -eq "dci_rust.Flags" })
$bitfields = @($flagsLayout[0].fields | Where-Object { $null -ne $_.bitfield })
Assert-Test ($bitfields.Count -eq 2) "Rust fixture does not describe both logical bitfields"
Assert-Test ($flagsLayout[0].lifecycle.destruction -eq "trivial") `
    "Flags must exercise the no-op trivial destruction contract"
Assert-Test (@($flagsLayout[0].lifecycle.operations.PSObject.Properties).Count -eq 0) `
    "trivial Flags unexpectedly declares lifecycle operations"

$mixedSymbol = @($document.exports.symbols | Where-Object { $_.name -eq "dci_rust_mixed_params" })
Assert-Test ($mixedSymbol.Count -eq 1) "missing reordered-parameter DCI symbol"
$mixedIndices = @($mixedSymbol[0].abi.parameters | ForEach-Object { $_.index }) -join ","
Assert-Test ($mixedIndices -eq "2,0,1") `
    "mixed-parameter ABI array is no longer deliberately reordered"
Assert-Test ($mixedSymbol[0].params[1].type.reference -eq "pointer" -and `
             $mixedSymbol[0].params[1].ownership -eq "borrow") `
    "mixed-parameter Packet input must be borrowed"
Assert-Test ($mixedSymbol[0].abi.parameters[2].index -eq 1 -and `
             $mixedSymbol[0].abi.parameters[2].passing -eq "direct") `
    "reordered Packet ABI entry must directly pass the borrowed pointer"

$alignedLayout = @($document.exports.layouts | Where-Object { $_.type_name -eq "dci_rust.AlignedPair" })
Assert-Test ($alignedLayout.Count -eq 1 -and $alignedLayout[0].alignment -eq 16) `
    "missing 16-byte-aligned aggregate layout"

$runtimeType = @($document.exports.runtime_type_operations | Where-Object {
    $_.type_name -eq "dci_rust.RuntimeDerived"
})
Assert-Test ($runtimeType.Count -eq 1) "missing RuntimeDerived runtime type operations"
$checkedDowncast = $runtimeType[0].operations.checked_downcast
Assert-Test ($checkedDowncast.symbol -eq "fixture.runtime.checked_downcast") `
    "RuntimeDerived checked-downcast does not reference the normalized operation symbol"
Assert-Test ($checkedDowncast.source_type -eq "dci_rust.RuntimeBase" -and `
             $checkedDowncast.target_type -eq "dci_rust.RuntimeDerived") `
    "RuntimeDerived checked-downcast does not declare its source and target types"
Assert-Test ($checkedDowncast.failure -eq "null" -and `
             $checkedDowncast.null_behavior -eq "return_null") `
    "RuntimeDerived checked-downcast does not define null failure semantics"
$sourceText = Get-Content -Raw -LiteralPath $src
Assert-Test ($sourceText.IndexOf("fn dci_rust_runtime_checked_downcast", `
                                 [StringComparison]::Ordinal) -lt 0) `
    "runtime cast helper must be consumed from the Descriptor, not declared in Vyx source"
$mainOffset = $sourceText.IndexOf("fn main()", [StringComparison]::Ordinal)
Assert-Test ($mainOffset -ge 0) "Rust DCI fixture has no Vyx main function"
$mainSource = $sourceText.Substring($mainOffset)
Assert-Test ($mainSource.IndexOf("dci_rust_packet_move(", [StringComparison]::Ordinal) -lt 0) `
    "fixture main must rely on Descriptor-driven move insertion"
Assert-Test ($mainSource.IndexOf("dci_rust_packet_destroy(", [StringComparison]::Ordinal) -lt 0) `
    "fixture main must rely on Descriptor-driven automatic destruction"

$irText = Get-Content -Raw -LiteralPath $ir
foreach ($symbol in @(
    "dci_rust_packet_create",
    "dci_rust_packet_copy",
    "dci_rust_packet_move",
    "dci_rust_packet_destroy",
    "dci_rust_destroy_calls",
    "dci_rust_packet_fold",
    "dci_rust_flags_make",
    "dci_rust_mixed_params",
    "dci_rust_aligned_make",
    "dci_rust_aligned_fold",
    "dci_rust_runtime_make",
    "dci_rust_runtime_checked_downcast",
    "dci_rust_runtime_inspect",
    "dci_rust_packet_type_id"
)) {
    Assert-Test ($irText.IndexOf($symbol, [StringComparison]::Ordinal) -ge 0) `
        "generated IR is missing language-neutral DCI link symbol: $symbol"
}
Assert-Test ($irText.IndexOf("sret", [StringComparison]::Ordinal) -ge 0) `
    "generated IR did not honor aggregate return lowering"
Assert-Test ([regex]::IsMatch(
    $irText,
    'dci_rust_packet_copy\s*\(ptr sret\([^\)]*\)[^\r\n]*,\s*ptr\s*\)'
)) "generated IR did not pass the Packet copy source as a borrowed pointer"
Assert-Test ([regex]::IsMatch($irText, 'dci_rust_packet_fold\s*\(ptr\s*\)')) `
    "generated IR did not pass Packet fold input as a borrowed pointer"
Assert-Test (-not [regex]::IsMatch(
    $irText,
    'dci_rust_packet_(?:copy|fold)\s*\([^\r\n]*byval'
)) "operation-owned Packet borrow unexpectedly used by-value ABI passing"
Assert-Test ([regex]::IsMatch($irText, 'call[^\r\n]*@dci_rust_packet_move\(')) `
    "generated IR is missing the Descriptor-inserted Packet move call"
Assert-Test ([regex]::IsMatch($irText, 'call[^\r\n]*@dci_rust_packet_destroy\(')) `
    "generated IR is missing automatic Packet destruction"
Assert-Test ([regex]::IsMatch($irText, 'dci_rust_mixed_params\s*\(i32[^\r\n]*ptr[^\r\n]*i64')) `
    "generated IR did not honor descriptor parameter indices after array reordering"
Assert-Test (-not [regex]::IsMatch(
    $irText,
    'dci_rust_mixed_params\s*\([^\r\n]*byval'
)) "mixed-parameter Packet borrow unexpectedly used by-value ABI passing"
Assert-Test ([regex]::IsMatch($irText, 'dci_rust_aligned_make\s*\(ptr sret\([^\)]*\) align 16')) `
    "generated IR did not preserve align(16) on the aggregate return slot"
Assert-Test ([regex]::IsMatch(
    $irText,
    '(?m)^declare\s+i64\s+@dci_rust_aligned_fold\s*\(ptr[^\r\n]*\balign 16\b'
)) "generated IR declaration did not preserve align(16) on the indirect aggregate parameter"
Assert-Test ([regex]::IsMatch(
    $irText,
    '(?m)^\s*(?:%[-A-Za-z$._0-9]+\s*=\s*)?(?:(?:musttail|tail|notail)\s+)?call\s+i64\s+@dci_rust_aligned_fold\s*\(ptr[^\r\n]*\balign 16\b'
)) "generated IR callsite did not preserve align(16) on the indirect aggregate parameter"
Assert-Test ([regex]::IsMatch(
    $irText,
    '(?m)^declare\s+i64\s+@dci_rust_aligned_byval_fold\s*\(ptr[^\r\n]*\bbyval\b[^\r\n]*\balign 16\b'
)) "generated IR declaration did not preserve byval align(16) aggregate passing"
Assert-Test ([regex]::IsMatch(
    $irText,
    '(?m)^\s*(?:%[-A-Za-z$._0-9]+\s*=\s*)?(?:(?:musttail|tail|notail)\s+)?call\s+i64\s+@dci_rust_aligned_byval_fold\s*\(ptr[^\r\n]*\bbyval\b[^\r\n]*\balign 16\b'
)) "generated IR callsite did not preserve byval align(16) aggregate passing"
Assert-Test ([regex]::IsMatch(
    $irText,
    'call\s+ptr\s+@dci_rust_runtime_checked_downcast\s*\(ptr'
)) "generated IR did not consume the Descriptor-driven checked-downcast operation"

# This is IR-only: the native Rust symbol uses its real coerce ABI, so the
# mutated indirect/by-value contract must never be linked or executed. It
# proves that an ABI descriptor may lower a direct alloca below the storage
# type's natural alignment without ordinary bridge stores claiming more.
$lowAlignmentContract = Get-Content -Raw -LiteralPath $dciFile | ConvertFrom-Json
$lowAlignmentSymbol = @($lowAlignmentContract.exports.symbols | Where-Object {
    $_.name -eq "dci_rust_flags_priority"
})
Assert-Test ($lowAlignmentSymbol.Count -eq 1) "missing Flags priority symbol for low-alignment IR test"
$lowAlignmentParameter = $lowAlignmentSymbol[0].abi.parameters[0]
$lowAlignmentParameter.passing = "indirect"
$lowAlignmentParameter.PSObject.Properties.Remove("coerce_to")
$lowAlignmentParameter | Add-Member -NotePropertyName "alignment" -NotePropertyValue 1 -Force
$lowAlignmentParameter | Add-Member -NotePropertyName "attributes" -NotePropertyValue @("by_value") -Force
$lowAlignmentDci = Join-Path $cache "alignment_indirect_flags.dci"
$lowAlignmentIr = Join-Path $cache "alignment_indirect_flags.ll"
Write-JsonUtf8 -Document $lowAlignmentContract -Path $lowAlignmentDci
Invoke-Checked -FilePath $BootstrapCompiler `
    -ArgumentList @("--src=file", $src, "--emit=ir", "--dci", $lowAlignmentDci, "-o", $lowAlignmentIr) `
    -Name "emit_low_alignment_indirect_ir"
$lowAlignmentIrText = Get-Content -Raw -LiteralPath $lowAlignmentIr
Assert-Test ([regex]::IsMatch(
    $lowAlignmentIrText,
    '(?m)^declare\s+i32\s+@dci_rust_flags_priority\s*\(ptr\s+align\s+1\)'
)) "low-alignment indirect declaration did not preserve align(1)"
$lowAlignmentMaterialization = [regex]::Match(
    $lowAlignmentIrText,
    '(?ms)(?<slot>%[-A-Za-z$._0-9]+)\s*=\s*alloca\s+%mir\.struct\.dci_rust\.Flags,\s*align\s+1.*?' +
    'store\s+%mir\.struct\.dci_rust\.Flags\s+[^\r\n]+,\s*ptr\s+\k<slot>,\s*align\s+1\s*\r?\n\s*' +
    '%[-A-Za-z$._0-9]+\s*=\s*call\s+i32\s+@dci_rust_flags_priority\s*\(ptr\s+align\s+1\s+\k<slot>\)'
)
Assert-Test ($lowAlignmentMaterialization.Success) `
    "low-alignment indirect materialization did not keep alloca/store/call at align(1)"

$languageNeutralIrText = Get-Content -Raw -LiteralPath $languageNeutralIr
Assert-Test ((Normalize-DirectIr $irText) -ceq (Normalize-DirectIr $languageNeutralIrText)) `
    "changing only source.language changed normalized Direct IR"

$directIrText = Get-Content -Raw -LiteralPath $directIr
$alphaBody = [regex]::Match(
    $directIrText,
    '(?s)define (?:ptr|%__VyxPointerHandle) @__vyx_F_alpha_5Fupcast[^\{]*\{(?<body>.*?)\r?\n\}'
)
$betaBody = [regex]::Match(
    $directIrText,
    '(?s)define (?:ptr|%__VyxPointerHandle) @__vyx_F_beta_5Fupcast[^\{]*\{(?<body>.*?)\r?\n\}'
)
$tableBody = [regex]::Match(
    $directIrText,
    '(?s)define (?:ptr|%__VyxPointerHandle) @__vyx_F_table_5Fupcast[^\{]*\{(?<body>.*?)\r?\n\}'
)
$tableAdjustment = [regex]::Match(
    $tableBody.Groups['body'].Value,
    '(?ms)(?<table_slot>%[-A-Za-z$._0-9]+)\s*=\s*getelementptr(?: inbounds)? i8,\s*ptr (?<object>%[-A-Za-z$._0-9]+),\s*i64 0\s*\r?\n' +
    '\s*(?<table>%[-A-Za-z$._0-9]+)\s*=\s*load ptr,\s*ptr \k<table_slot>[^\r\n]*\r?\n' +
    '\s*(?<entry>%[-A-Za-z$._0-9]+)\s*=\s*getelementptr(?: inbounds)? i8,\s*ptr \k<table>,\s*i64 4\s*\r?\n' +
    '\s*(?<displacement32>%[-A-Za-z$._0-9]+)\s*=\s*load i32,\s*ptr \k<entry>[^\r\n]*\r?\n' +
    '\s*(?<displacement64>%[-A-Za-z$._0-9]+)\s*=\s*sext i32 \k<displacement32> to i64\s*\r?\n' +
    '\s*%[-A-Za-z$._0-9]+\s*=\s*getelementptr(?: inbounds)? i8,\s*ptr \k<object>,\s*i64 \k<displacement64>'
)
Assert-Test ($alphaBody.Success -and $alphaBody.Groups['body'].Value -match 'i64 8') `
    "alpha.Base did not use its fully-qualified +8 adjustment"
Assert-Test ($betaBody.Success -and $betaBody.Groups['body'].Value -match 'i64 24') `
    "beta.Base did not use its fully-qualified +24 adjustment"
Assert-Test ($tableBody.Success -and $tableAdjustment.Success) `
    "table adjustment did not load and apply the descriptor-defined displacement"
Assert-Test ($alphaBody.Groups['body'].Value -match 'phi ptr') `
    "alpha null-preserving adjustment did not emit its pointer PHI"
Assert-Test ($betaBody.Groups['body'].Value -match 'phi ptr') `
    "beta null-preserving adjustment did not emit its pointer PHI"
Assert-Test ($tableBody.Groups['body'].Value -match 'phi ptr') `
    "table null-preserving adjustment did not emit its pointer PHI"

$oldBackendTool = $env:VYX_DCI_STUB_BACKEND_TOOL
$oldBackendToolArgs = $env:VYX_DCI_STUB_BACKEND_TOOL_ARGS
$oldBackendDependencies = $env:VYX_DCI_STUB_BACKEND_DEPENDENCIES
$oldBackendVersion = $env:VYX_DCI_STUB_BACKEND_VERSION
$oldBackendExtension = $env:VYX_DCI_STUB_SOURCE_EXTENSION
$oldBackendCapabilities = $env:VYX_DCI_STUB_BACKEND_CAPABILITIES
$oldRustc = $env:RUSTC
$cacheProbeId = [Guid]::NewGuid().ToString("N")
$cacheProbeBackend = Join-Path $cache ("rust_stub_backend_cache_probe_" + $cacheProbeId + ".py")
# Prior gate runs may have left a generated stub at a later probe revision: the
# emit cache keys on the probe backend's CONTENT digest, so a fresh run with an
# identical revision-1 probe would reuse the stale artifact and read as
# "already at revision 2".  Start the probe section from a clean slate.
Get-ChildItem -LiteralPath $cache -Filter "*dci_stubs*.rs" -File -ErrorAction SilentlyContinue |
    Remove-Item -Force -Confirm:$false -ErrorAction SilentlyContinue
Get-ChildItem -LiteralPath $cache -Filter "*dci_stubs*.obj" -File -ErrorAction SilentlyContinue |
    Remove-Item -Force -Confirm:$false -ErrorAction SilentlyContinue
Get-ChildItem -LiteralPath $cache -Filter "dci_stub_defs_*" -File -ErrorAction SilentlyContinue |
    Remove-Item -Force -Confirm:$false -ErrorAction SilentlyContinue
[IO.File]::Copy($stubBackendTool, $cacheProbeBackend, $true)
$cacheProbeOriginalInfo = Get-Item -LiteralPath $cacheProbeBackend
$cacheProbeOriginalLength = $cacheProbeOriginalInfo.Length
$cacheProbeOriginalWriteTime = $cacheProbeOriginalInfo.LastWriteTimeUtc
try {
    $env:VYX_DCI_STUB_BACKEND_TOOL = "python"
    $env:VYX_DCI_STUB_BACKEND_TOOL_ARGS = '"' + $cacheProbeBackend + '"'
    $env:VYX_DCI_STUB_BACKEND_DEPENDENCIES = $cacheProbeBackend
    $env:VYX_DCI_STUB_BACKEND_VERSION = "rust-stub-fixture-1"
    $env:VYX_DCI_STUB_SOURCE_EXTENSION = "rs"
    $env:VYX_DCI_STUB_BACKEND_CAPABILITIES = "emit-source,compile-object"
    $env:RUSTC = $Rustc
    Push-Location $projectRoot
    try {
        Invoke-Checked -FilePath $BootstrapCompiler `
            -ArgumentList @("build", "-j", "4") `
            -Name "external_stub_project_build"
    } finally {
        Pop-Location
    }

    # The stub artifact carries the backend flavor in its name now
    # (<stem>_dci_stubs_external.rs for an external backend).
    $generatedStubBefore = @(Get-ChildItem -LiteralPath $cache -Filter "*dci_stubs_external.rs" -File)
    Assert-Test ($generatedStubBefore.Count -eq 1) `
        "external DCI Stub backend must emit exactly one Rust artifact; found $($generatedStubBefore.Count)"
    $generatedStubPath = $generatedStubBefore[0].FullName
    $generatedStubTextBefore = Get-Content -Raw -LiteralPath $generatedStubPath
    Assert-Test ($generatedStubTextBefore.IndexOf("DCI_CACHE_PROBE: u8 = 1", [StringComparison]::Ordinal) -ge 0) `
        "external DCI Stub backend cache probe did not start at revision 1"
    $generatedStubObject = [IO.Path]::ChangeExtension($generatedStubPath, ".obj")
    Assert-Test (Test-Path -LiteralPath $generatedStubObject) `
        "external DCI Stub backend did not compile its generated Rust artifact"
    $generatedStubObjectHashBefore = Get-Sha256Hex -Path $generatedStubObject

    $cacheProbeText = Get-Content -Raw -LiteralPath $cacheProbeBackend
    $cacheProbeText2 = $cacheProbeText.Replace("CACHE_PROBE = 1", "CACHE_PROBE = 2")
    Assert-Test ($cacheProbeText2 -ne $cacheProbeText) `
        "external DCI Stub backend cache probe source did not contain its revision marker"
    [IO.File]::WriteAllText($cacheProbeBackend, $cacheProbeText2, [Text.UTF8Encoding]::new($false))
    [IO.File]::SetLastWriteTimeUtc($cacheProbeBackend, $cacheProbeOriginalWriteTime)
    $cacheProbeChangedInfo = Get-Item -LiteralPath $cacheProbeBackend
    Assert-Test ($cacheProbeChangedInfo.Length -eq $cacheProbeOriginalLength) `
        "external DCI Stub backend cache probe must preserve file size"
    Assert-Test ($cacheProbeChangedInfo.LastWriteTimeUtc.Ticks -eq $cacheProbeOriginalWriteTime.Ticks) `
        "external DCI Stub backend cache probe must preserve file mtime"

    Push-Location $projectRoot
    try {
        Invoke-Checked -FilePath $BootstrapCompiler `
            -ArgumentList @("build", "-j", "4") `
            -Name "external_stub_project_rebuild_after_dependency_change"
    } finally {
        Pop-Location
    }

    $generatedStubTextAfter = Get-Content -Raw -LiteralPath $generatedStubPath
    Assert-Test ($generatedStubTextAfter.IndexOf("DCI_CACHE_PROBE: u8 = 2", [StringComparison]::Ordinal) -ge 0) `
        "external DCI Stub backend dependency content change did not invalidate emit cache"
    $generatedStubObjectHashAfter = Get-Sha256Hex -Path $generatedStubObject
    Assert-Test ($generatedStubObjectHashAfter -ne $generatedStubObjectHashBefore) `
        "external DCI Stub backend dependency content change did not invalidate compile cache"
} finally {
    [Environment]::SetEnvironmentVariable("VYX_DCI_STUB_BACKEND_TOOL", $oldBackendTool, "Process")
    [Environment]::SetEnvironmentVariable("VYX_DCI_STUB_BACKEND_TOOL_ARGS", $oldBackendToolArgs, "Process")
    [Environment]::SetEnvironmentVariable("VYX_DCI_STUB_BACKEND_DEPENDENCIES", $oldBackendDependencies, "Process")
    [Environment]::SetEnvironmentVariable("VYX_DCI_STUB_BACKEND_VERSION", $oldBackendVersion, "Process")
    [Environment]::SetEnvironmentVariable("VYX_DCI_STUB_SOURCE_EXTENSION", $oldBackendExtension, "Process")
    [Environment]::SetEnvironmentVariable("VYX_DCI_STUB_BACKEND_CAPABILITIES", $oldBackendCapabilities, "Process")
    [Environment]::SetEnvironmentVariable("RUSTC", $oldRustc, "Process")
    Remove-Item -LiteralPath $cacheProbeBackend -Force -ErrorAction SilentlyContinue
}

Assert-Test (Test-Path -LiteralPath $stubExe) `
    "external Rust DCI Stub backend did not produce its executable"
$generatedStubText = Get-Content -Raw -LiteralPath $generatedStubPath
Assert-Test ($generatedStubText.IndexOf("dci_rust_stub_add", [StringComparison]::Ordinal) -ge 0) `
    "external Rust backend artifact does not implement the Descriptor symbol"

$stubRunOut = Join-Path $cache "run_external_stub.out.log"
$stubRunErr = Join-Path $cache "run_external_stub.err.log"
& $stubExe > $stubRunOut 2> $stubRunErr
if ($LASTEXITCODE -ne 0) {
    Write-Host "FAILED: run_external_stub (exit $LASTEXITCODE)"
    if (Test-Path $stubRunOut) { Get-Content $stubRunOut -Tail 100 }
    if (Test-Path $stubRunErr) { Get-Content $stubRunErr -Tail 100 }
    exit $LASTEXITCODE
}
$stubActual = ((Get-Content $stubRunOut) -join "`n").Trim()
Assert-Test ($stubActual -eq "dci_rust_stub_backend OK") `
    "unexpected external Rust Stub output: $stubActual"

Write-Host "dci_rust_generic: OK"
