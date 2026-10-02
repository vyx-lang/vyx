param(
    [string]$BaselineCompiler = ".\build_yolo_vyxcg_release\vyxc.exe",
    [string]$BootstrapCompiler = ".\bootstrap_compiler\boot_b_current_recheck_o2.exe",
    [string]$ClangCompiler = "clang++",
    [string]$RuntimeDir = ".\build_yolo_vyxcg_release\vyx_codegen",
    [string]$VyxInput = ".\bootstrap_compiler\src\main.vyx",
    [string[]]$BaselineVyxArgs = @("--emit-ir", "-O0"),
    [string[]]$BootstrapVyxArgs = @("--emit=ir", "-O0"),
    [string]$ClangInput = "",
    [string[]]$ClangArgs = @("-std=c++20", "-O0", "-S", "-emit-llvm"),
    [int]$Warmup = 2,
    [int]$Reps = 10,
    [string]$OutDir = ".\tests\.cache\compiler_perf",
    [switch]$SkipBaseline,
    [switch]$SkipBootstrap,
    [switch]$SkipClang
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-ExistingPath([string]$PathValue, [string]$Label) {
    if ([string]::IsNullOrWhiteSpace($PathValue)) {
        throw "$Label is empty."
    }
    if (!(Test-Path -LiteralPath $PathValue)) {
        throw "$Label not found: $PathValue"
    }
    return (Resolve-Path -LiteralPath $PathValue).Path
}

function Resolve-Executable([string]$PathValue, [string]$Label) {
    if ([string]::IsNullOrWhiteSpace($PathValue)) {
        throw "$Label is empty."
    }
    if (Test-Path -LiteralPath $PathValue) {
        return (Resolve-Path -LiteralPath $PathValue).Path
    }
    $cmd = Get-Command $PathValue -ErrorAction SilentlyContinue
    if ($null -ne $cmd) {
        return $cmd.Source
    }
    throw "$Label not found: $PathValue"
}

function Quote-Arg([string]$Arg) {
    if ($Arg -match '^[A-Za-z0-9_./:\\=-]+$') {
        return $Arg
    }
    return '"' + ($Arg -replace '"', '\"') + '"'
}

function Join-CommandLine([string]$Exe, [object[]]$ArgList) {
    $parts = New-Object System.Collections.Generic.List[string]
    [void]$parts.Add((Quote-Arg $Exe))
    foreach ($arg in $ArgList) {
        [void]$parts.Add((Quote-Arg $arg))
    }
    return ($parts -join " ")
}

function Join-Arguments([object[]]$ArgList) {
    $parts = New-Object System.Collections.Generic.List[string]
    foreach ($arg in $ArgList) {
        [void]$parts.Add((Quote-Arg $arg))
    }
    return ($parts -join " ")
}

function New-ClangFixture([string]$PathValue) {
    $dir = Split-Path -Parent $PathValue
    if (![string]::IsNullOrWhiteSpace($dir)) {
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
    }

    $sb = New-Object System.Text.StringBuilder
    [void]$sb.AppendLine("#include <array>")
    [void]$sb.AppendLine("#include <cstdint>")
    [void]$sb.AppendLine("#include <string>")
    [void]$sb.AppendLine("#include <vector>")
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine("template<int N>")
    [void]$sb.AppendLine("struct Node {")
    [void]$sb.AppendLine("    int value;")
    [void]$sb.AppendLine("    constexpr Node() : value(N) {}")
    [void]$sb.AppendLine("    int mix(int x) const { return (x * 33 + value) ^ (N << 1); }")
    [void]$sb.AppendLine("};")
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine("template<int N>")
    [void]$sb.AppendLine("int fold_value(int x) {")
    [void]$sb.AppendLine("    Node<N> n;")
    [void]$sb.AppendLine("    if constexpr (N == 0) {")
    [void]$sb.AppendLine("        return n.mix(x);")
    [void]$sb.AppendLine("    } else {")
    [void]$sb.AppendLine("        return n.mix(fold_value<N - 1>(x + N));")
    [void]$sb.AppendLine("    }")
    [void]$sb.AppendLine("}")
    [void]$sb.AppendLine("")

    for ($i = 0; $i -lt 256; $i++) {
        $depth = ($i % 32) + 8
        [void]$sb.AppendLine("int fixture_fn_$i(int x) {")
        [void]$sb.AppendLine("    std::array<int, 8> data = { x, $i, x + $i, x - $i, x ^ $i, x | $i, x & $i, $depth };")
        [void]$sb.AppendLine("    int acc = fold_value<$depth>(data[0]);")
        [void]$sb.AppendLine("    for (int v : data) { acc = (acc * 17) ^ (v + $i); }")
        [void]$sb.AppendLine("    return acc;")
        [void]$sb.AppendLine("}")
        [void]$sb.AppendLine("")
    }

    [void]$sb.AppendLine("int main() {")
    [void]$sb.AppendLine("    volatile int seed = 7;")
    [void]$sb.AppendLine("    int acc = seed;")
    for ($i = 0; $i -lt 256; $i++) {
        [void]$sb.AppendLine("    acc ^= fixture_fn_$i(acc + $i);")
    }
    [void]$sb.AppendLine("    return acc == 0x12345678 ? 1 : 0;")
    [void]$sb.AppendLine("}")

    Set-Content -LiteralPath $PathValue -Value $sb.ToString() -Encoding UTF8
}

function Median([double[]]$Values) {
    if ($Values.Count -eq 0) {
        return 0.0
    }
    $sorted = @($Values | Sort-Object)
    $mid = [int][Math]::Floor($sorted.Count / 2)
    if (($sorted.Count % 2) -eq 1) {
        return [double]$sorted[$mid]
    }
    return ([double]$sorted[$mid - 1] + [double]$sorted[$mid]) / 2.0
}

function Invoke-OneRun($Target, [int]$Iteration, [int]$Slot, [string]$Phase, [string]$OutRoot, [string]$RuntimePath) {
    $tag = "{0}_{1}_{2:00}_{3:00}" -f $Target.Name, $Phase, $Iteration, $Slot
    $stdoutPath = Join-Path $OutRoot ($tag + ".stdout.log")
    $stderrPath = Join-Path $OutRoot ($tag + ".stderr.log")
    $commandPath = Join-Path $OutRoot ($tag + ".command.txt")
    $outputPath = Join-Path $OutRoot ($tag + $Target.OutputExt)
    $procArgs = @()
    foreach ($arg in $Target.ArgsBeforeOutput) {
        $procArgs += $arg
    }
    $procArgs += $outputPath
    $procArgs += $Target.ArgsAfterOutput

    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $Target.Exe
    $psi.Arguments = Join-Arguments -ArgList $procArgs
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true

    if (![string]::IsNullOrWhiteSpace($RuntimePath)) {
        $pathKey = "Path"
        if (!$psi.Environment.ContainsKey($pathKey) -and $psi.Environment.ContainsKey("PATH")) {
            $pathKey = "PATH"
        }
        $existingPath = ""
        if ($psi.Environment.ContainsKey($pathKey)) {
            $existingPath = $psi.Environment[$pathKey]
        }
        $psi.Environment[$pathKey] = $RuntimePath + [System.IO.Path]::PathSeparator + $existingPath
    }

    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $proc = [System.Diagnostics.Process]::Start($psi)
    $stdoutTask = $proc.StandardOutput.ReadToEndAsync()
    $stderrTask = $proc.StandardError.ReadToEndAsync()
    $proc.WaitForExit()
    $stdout = $stdoutTask.GetAwaiter().GetResult()
    $stderr = $stderrTask.GetAwaiter().GetResult()
    $sw.Stop()

    Set-Content -LiteralPath $stdoutPath -Value $stdout -Encoding UTF8
    Set-Content -LiteralPath $stderrPath -Value $stderr -Encoding UTF8

    $cmdLine = Join-CommandLine -Exe $Target.Exe -ArgList $procArgs
    Set-Content -LiteralPath $commandPath -Value $cmdLine -Encoding UTF8
    return [pscustomobject]@{
        Phase = $Phase
        Iteration = $Iteration
        Slot = $Slot
        Target = $Target.Name
        Workload = $Target.Workload
        Input = $Target.Input
        Output = $outputPath
        ExitCode = $proc.ExitCode
        ElapsedMs = [Math]::Round($sw.Elapsed.TotalMilliseconds, 3)
        Command = $cmdLine
        CommandFile = $commandPath
        Stdout = $stdoutPath
        Stderr = $stderrPath
    }
}

if ($Warmup -lt 0) {
    throw "Warmup must be >= 0."
}
if ($Reps -lt 1) {
    throw "Reps must be >= 1."
}

$outRoot = $OutDir
New-Item -ItemType Directory -Force -Path $outRoot | Out-Null
$outRoot = (Resolve-Path -LiteralPath $outRoot).Path

$runtimePath = ""
if (!(($SkipBaseline.IsPresent) -and ($SkipBootstrap.IsPresent))) {
    $runtimePath = Resolve-ExistingPath $RuntimeDir "RuntimeDir"
}

$vyxInputPath = Resolve-ExistingPath $VyxInput "VyxInput"
$targets = New-Object System.Collections.Generic.List[object]

if (!$SkipBaseline.IsPresent) {
    $baselineExe = Resolve-Executable $BaselineCompiler "BaselineCompiler"
    [void]$targets.Add([pscustomobject]@{
        Name = "baseline"
        Workload = "vyx:$vyxInputPath"
        Exe = $baselineExe
        Input = $vyxInputPath
        OutputExt = ".ll"
        ArgsBeforeOutput = @($BaselineVyxArgs + @($vyxInputPath, "-o"))
        ArgsAfterOutput = @()
    })
}

if (!$SkipBootstrap.IsPresent) {
    $bootstrapExe = Resolve-Executable $BootstrapCompiler "BootstrapCompiler"
    [void]$targets.Add([pscustomobject]@{
        Name = "bootstrap"
        Workload = "vyx:$vyxInputPath"
        Exe = $bootstrapExe
        Input = $vyxInputPath
        OutputExt = ".ll"
        ArgsBeforeOutput = @("--src=file", $vyxInputPath) + @($BootstrapVyxArgs + @("-o"))
        ArgsAfterOutput = @()
    })
}

if (!$SkipClang.IsPresent) {
    $clangExe = Resolve-Executable $ClangCompiler "ClangCompiler"
    if ([string]::IsNullOrWhiteSpace($ClangInput)) {
        $ClangInput = Join-Path $outRoot "clang_fixture.cpp"
        New-ClangFixture $ClangInput
    } elseif (!(Test-Path -LiteralPath $ClangInput)) {
        New-ClangFixture $ClangInput
    }
    $clangInputPath = Resolve-ExistingPath $ClangInput "ClangInput"
    [void]$targets.Add([pscustomobject]@{
        Name = "clang++"
        Workload = "cpp-fixture:$clangInputPath"
        Exe = $clangExe
        Input = $clangInputPath
        OutputExt = ".ll"
        ArgsBeforeOutput = @($ClangArgs + @($clangInputPath, "-o"))
        ArgsAfterOutput = @()
    })
}

if ($targets.Count -eq 0) {
    throw "No targets selected."
}

$rows = New-Object System.Collections.Generic.List[object]
for ($i = 1; $i -le $Warmup; $i++) {
    $slot = 0
    foreach ($target in $targets) {
        $slot++
        $row = Invoke-OneRun $target $i $slot "warmup" $outRoot $runtimePath
        [void]$rows.Add($row)
        if ($row.ExitCode -ne 0) {
            throw "Warmup failed for $($row.Target), exit $($row.ExitCode). See $($row.Stderr)"
        }
    }
}

for ($i = 1; $i -le $Reps; $i++) {
    $orderedList = New-Object System.Collections.Generic.List[object]
    foreach ($target in $targets) {
        [void]$orderedList.Add($target)
    }
    $ordered = $orderedList.ToArray()
    if (($i % 2) -eq 0) {
        [Array]::Reverse($ordered)
    }
    $slot = 0
    foreach ($target in $ordered) {
        $slot++
        $row = Invoke-OneRun $target $i $slot "measure" $outRoot $runtimePath
        [void]$rows.Add($row)
        if ($row.ExitCode -ne 0) {
            throw "Measure run failed for $($row.Target), exit $($row.ExitCode). See $($row.Stderr)"
        }
    }
}

$rowsPath = Join-Path $outRoot "rows.csv"
$summaryPath = Join-Path $outRoot "summary.csv"
$summaryTextPath = Join-Path $outRoot "summary.txt"

$rows | Export-Csv -LiteralPath $rowsPath -NoTypeInformation -Encoding UTF8

$summary = New-Object System.Collections.Generic.List[object]
foreach ($target in $targets) {
    $values = @($rows | Where-Object { $_.Phase -eq "measure" -and $_.Target -eq $target.Name } | ForEach-Object { [double]$_.ElapsedMs })
    $count = $values.Count
    $min = ($values | Measure-Object -Minimum).Minimum
    $max = ($values | Measure-Object -Maximum).Maximum
    $avg = ($values | Measure-Object -Average).Average
    $median = Median $values
    [void]$summary.Add([pscustomobject]@{
        Target = $target.Name
        Workload = $target.Workload
        Count = $count
        MinMs = [Math]::Round([double]$min, 3)
        AvgMs = [Math]::Round([double]$avg, 3)
        MedianMs = [Math]::Round([double]$median, 3)
        MaxMs = [Math]::Round([double]$max, 3)
    })
}

$summary | Export-Csv -LiteralPath $summaryPath -NoTypeInformation -Encoding UTF8

$lines = New-Object System.Collections.Generic.List[string]
[void]$lines.Add("compiler performance comparison")
[void]$lines.Add("out_dir: $outRoot")
[void]$lines.Add("warmup: $Warmup")
[void]$lines.Add("reps: $Reps")
[void]$lines.Add("")
foreach ($item in $summary) {
    [void]$lines.Add(("target: {0}" -f $item.Target))
    [void]$lines.Add(("workload: {0}" -f $item.Workload))
    [void]$lines.Add(("count: {0}" -f $item.Count))
    [void]$lines.Add(("min_ms: {0}" -f $item.MinMs))
    [void]$lines.Add(("avg_ms: {0}" -f $item.AvgMs))
    [void]$lines.Add(("median_ms: {0}" -f $item.MedianMs))
    [void]$lines.Add(("max_ms: {0}" -f $item.MaxMs))
    [void]$lines.Add("")
}
[void]$lines.Add("rows_csv: $rowsPath")
[void]$lines.Add("summary_csv: $summaryPath")

Set-Content -LiteralPath $summaryTextPath -Value $lines -Encoding UTF8
Get-Content -LiteralPath $summaryTextPath
