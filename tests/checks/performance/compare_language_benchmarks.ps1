param(
    [string]$BaselineCompiler = ".\build_yolo_vyxcg_release\vyxc.exe",
    [string]$BootstrapCompiler = ".\bootstrap_compiler\boot_b_current_recheck_o2.exe",
    [string]$ClangCompiler = "clang++",
    [string]$RuntimeDir = ".\build_yolo_vyxcg_release\vyx_codegen",
    [string]$CppStandard = "c++2c",
    [int]$Warmup = 1,
    [int]$Reps = 5,
    [int]$RunTimeoutSec = 20,
    [string]$OutDir = ".\tests\.cache\language_benchmarks"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-ExistingPath([string]$PathValue, [string]$Label) {
    if ([string]::IsNullOrWhiteSpace($PathValue)) { throw "$Label is empty." }
    if (!(Test-Path -LiteralPath $PathValue)) { throw "$Label not found: $PathValue" }
    return (Resolve-Path -LiteralPath $PathValue).Path
}

function Resolve-Executable([string]$PathValue, [string]$Label) {
    if ([string]::IsNullOrWhiteSpace($PathValue)) { throw "$Label is empty." }
    if (Test-Path -LiteralPath $PathValue) { return (Resolve-Path -LiteralPath $PathValue).Path }
    $cmd = Get-Command $PathValue -ErrorAction SilentlyContinue
    if ($null -ne $cmd) { return $cmd.Source }
    throw "$Label not found: $PathValue"
}

function Quote-Arg([string]$Arg) {
    if ($Arg -match '^[A-Za-z0-9_./:\\=-]+$') { return $Arg }
    return '"' + ($Arg -replace '"', '\"') + '"'
}

function Join-Arguments([object[]]$ArgList) {
    $parts = New-Object System.Collections.Generic.List[string]
    foreach ($arg in $ArgList) { [void]$parts.Add((Quote-Arg ([string]$arg))) }
    return ($parts -join " ")
}

function Join-CommandLine([string]$Exe, [object[]]$ArgList) {
    $parts = New-Object System.Collections.Generic.List[string]
    [void]$parts.Add((Quote-Arg $Exe))
    foreach ($arg in $ArgList) { [void]$parts.Add((Quote-Arg ([string]$arg))) }
    return ($parts -join " ")
}

function Normalize-Output([string]$Text) {
    if ($null -eq $Text) { return "" }
    return (($Text -replace "`r`n", "`n") -replace "`r", "`n").Trim()
}

function Write-Utf8NoBom([string]$PathValue, [string]$Text) {
    $encoding = [System.Text.UTF8Encoding]::new($false)
    [System.IO.File]::WriteAllText($PathValue, $Text, $encoding)
}

function Median([double[]]$Values) {
    if ($Values.Count -eq 0) { return 0.0 }
    $sorted = @($Values | Sort-Object)
    $mid = [int][Math]::Floor($sorted.Count / 2)
    if (($sorted.Count % 2) -eq 1) { return [double]$sorted[$mid] }
    return ([double]$sorted[$mid - 1] + [double]$sorted[$mid]) / 2.0
}

function Invoke-ProcessTimed(
    [string]$Exe,
    [object[]]$ArgList,
    [string]$StdoutPath,
    [string]$StderrPath,
    [string]$CommandPath,
    [string]$ExtraPath,
    [int]$TimeoutSec
) {
    $cmdLine = Join-CommandLine -Exe $Exe -ArgList $ArgList
    Set-Content -LiteralPath $CommandPath -Value $cmdLine -Encoding UTF8

    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $Exe
    $psi.Arguments = Join-Arguments -ArgList $ArgList
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    if (![string]::IsNullOrWhiteSpace($ExtraPath)) {
        $pathKey = "Path"
        if (!$psi.Environment.ContainsKey($pathKey) -and $psi.Environment.ContainsKey("PATH")) {
            $pathKey = "PATH"
        }
        $old = ""
        if ($psi.Environment.ContainsKey($pathKey)) { $old = $psi.Environment[$pathKey] }
        $psi.Environment[$pathKey] = $ExtraPath + [System.IO.Path]::PathSeparator + $old
    }

    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $proc = [System.Diagnostics.Process]::Start($psi)
    $stdoutTask = $proc.StandardOutput.ReadToEndAsync()
    $stderrTask = $proc.StandardError.ReadToEndAsync()
    $timeoutMs = [Math]::Max(1, $TimeoutSec) * 1000
    $finished = $proc.WaitForExit($timeoutMs)
    if (!$finished) {
        try { $proc.Kill() } catch {}
        $proc.WaitForExit()
    }
    $stdout = $stdoutTask.GetAwaiter().GetResult()
    $stderr = $stderrTask.GetAwaiter().GetResult()
    $sw.Stop()

    Set-Content -LiteralPath $StdoutPath -Value $stdout -Encoding UTF8
    Set-Content -LiteralPath $StderrPath -Value $stderr -Encoding UTF8

    $exitCode = -9999
    if ($finished) { $exitCode = $proc.ExitCode }
    return [pscustomobject]@{
        ExitCode = $exitCode
        TimedOut = -not $finished
        ElapsedMs = [Math]::Round($sw.Elapsed.TotalMilliseconds, 3)
        Stdout = $stdout
        Stderr = $stderr
        Command = $cmdLine
    }
}

function Write-BenchmarkFixtures([string]$GeneratedDir) {
    New-Item -ItemType Directory -Force -Path $GeneratedDir | Out-Null
    $items = New-Object System.Collections.Generic.List[object]

    $vyxArith = @'
module bench_arith_control;

fn mix(x: i64) -> i64 {
    return (x * 1664525 + 1013904223) & 2147483647;
}

fn main() -> i32 {
    var acc: i64 = 7;
    var i: i64 = 0;
    while (i < 20000) {
        acc = mix(acc + i);
        if ((i % 7) == 0) {
            acc = acc ^ (i * 13);
        } else {
            acc = acc + (i % 97);
        }
        i = i + 1;
    }
    print("arith_control result=${acc}");
    return 0;
}
'@
    $cppArith = @'
#include <cstdint>
#include <iostream>

static std::int64_t mix(std::int64_t x) {
    return (x * 1664525LL + 1013904223LL) & 2147483647LL;
}

int main() {
    std::int64_t acc = 7;
    for (std::int64_t i = 0; i < 20000; ++i) {
        acc = mix(acc + i);
        if ((i % 7) == 0) {
            acc = acc ^ (i * 13);
        } else {
            acc = acc + (i % 97);
        }
    }
    std::cout << "arith_control result=" << acc << "\n";
    return 0;
}
'@
    Add-BenchmarkFixture $items $GeneratedDir "arith_control" "loops+integer ops" $vyxArith $cppArith

    $vyxVec = @'
module bench_vec_generic;

use std.collections;

fn main() -> i32 {
    var v = Vec::<i64>.with_capacity(50000);
    var i: i64 = 0;
    while (i < 50000) {
        v.push_unchecked((i * 17 + 3) % 100000);
        i = i + 1;
    }
    var sum: i64 = 0;
    i = 0;
    while (i < v.size()) {
        let x = v.get_unchecked(i);
        sum = (sum + x * ((i % 11) + 1)) % 1000000007;
        i = i + 1;
    }
    print("vec_generic len=${v.size()} sum=${sum}");
    return 0;
}
'@
    $cppVec = @'
#include <cstdint>
#include <iostream>
#include <vector>

int main() {
    std::vector<std::int64_t> v;
    v.reserve(50000);
    for (std::int64_t i = 0; i < 50000; ++i) {
        v.push_back((i * 17 + 3) % 100000);
    }
    std::int64_t sum = 0;
    for (std::int64_t i = 0; i < static_cast<std::int64_t>(v.size()); ++i) {
        const auto x = v[static_cast<std::size_t>(i)];
        sum = (sum + x * ((i % 11) + 1)) % 1000000007LL;
    }
    std::cout << "vec_generic len=" << v.size() << " sum=" << sum << "\n";
    return 0;
}
'@
    Add-BenchmarkFixture $items $GeneratedDir "vec_generic" "generic Vec<T>" $vyxVec $cppVec

    $vyxString = @'
module bench_string_ops;

use std.string;

fn main() -> i32 {
    var s = "seed";
    var hits: i64 = 0;
    var i: i64 = 0;
    while (i < 4000) {
        let piece = "x${i}";
        s = s + piece;
        if (s.contains("x12")) { hits = hits + 1; }
        if (s.startsWith("seed")) { hits = hits + 2; }
        i = i + 1;
    }
    let head = s.substring(0, 4);
    print("string_ops len=${s.len} hits=${hits} head=${head}");
    return 0;
}
'@
    $cppString = @'
#include <iostream>
#include <string>

int main() {
    std::string s = "seed";
    long long hits = 0;
    for (long long i = 0; i < 4000; ++i) {
        const std::string piece = "x" + std::to_string(i);
        s += piece;
        if (s.find("x12") != std::string::npos) { hits += 1; }
        if (s.rfind("seed", 0) == 0) { hits += 2; }
    }
    const auto head = s.substr(0, 4);
    std::cout << "string_ops len=" << s.size() << " hits=" << hits << " head=" << head << "\n";
    return 0;
}
'@
    Add-BenchmarkFixture $items $GeneratedDir "string_ops" "strings+methods" $vyxString $cppString

    $vyxClosure = @'
module bench_closures;

fn apply(f: fn(i32) -> i32, x: i32) -> i32 {
    return f(x);
}

fn main() -> i32 {
    let base = 17;
    let scale = |x: i32| -> i32 { return x * base + 3; };
    var acc: i32 = 0;
    var i: i32 = 0;
    while (i < 20000) {
        acc = (acc + apply(scale, i % 97)) % 1000000007;
        i = i + 1;
    }
    print("closures result=${acc}");
    return 0;
}
'@
    $cppClosure = @'
#include <iostream>

template <class F>
static int apply(F f, int x) {
    return f(x);
}

int main() {
    const int base = 17;
    auto scale = [base](int x) { return x * base + 3; };
    int acc = 0;
    for (int i = 0; i < 20000; ++i) {
        acc = (acc + apply(scale, i % 97)) % 1000000007;
    }
    std::cout << "closures result=" << acc << "\n";
    return 0;
}
'@
    Add-BenchmarkFixture $items $GeneratedDir "closures" "closures+higher-order call" $vyxClosure $cppClosure

    $vyxArray = @'
module bench_generic_array;

use std.core;

fn fill_with<T, const N: i64>(v: T) -> [N]T {
    var a: [N]T = [v; N];
    return a;
}

fn sum_arr<T, const N: i64>(a: [N]T, zero: T) -> T {
    var s: T = zero;
    var i: i64 = 0;
    while (i < N) {
        s = s + a[i];
        i = i + 1;
    }
    return s;
}

fn main() -> i32 {
    let a = fill_with::<i64, 128>(7);
    let b = fill_with::<i32, 64>(5);
    let sa = sum_arr::<i64, 128>(a, 0);
    let sb = sum_arr::<i32, 64>(b, 0);
    print("generic_array sa=${sa} sb=${sb}");
    return 0;
}
'@
    $cppArray = @'
#include <array>
#include <iostream>

template <class T, long long N>
static std::array<T, static_cast<std::size_t>(N)> fill_with(T v) {
    std::array<T, static_cast<std::size_t>(N)> a{};
    a.fill(v);
    return a;
}

template <class T, long long N>
static T sum_arr(const std::array<T, static_cast<std::size_t>(N)>& a, T zero) {
    T s = zero;
    for (long long i = 0; i < N; ++i) {
        s = s + a[static_cast<std::size_t>(i)];
    }
    return s;
}

int main() {
    const auto a = fill_with<long long, 128>(7);
    const auto b = fill_with<int, 64>(5);
    const auto sa = sum_arr<long long, 128>(a, 0);
    const auto sb = sum_arr<int, 64>(b, 0);
    std::cout << "generic_array sa=" << sa << " sb=" << sb << "\n";
    return 0;
}
'@
    Add-BenchmarkFixture $items $GeneratedDir "generic_array" "const generics+arrays" $vyxArray $cppArray

    $vyxOption = @'
module bench_nested_option_vec;

use std.collections;
use std.core;

fn main() -> i32 {
    var v = Vec::<Option<i32>>.with_capacity(2000);
    var i: i32 = 0;
    while (i < 2000) {
        if ((i % 3) == 0) {
            v.push_unchecked(Option::<i32>.some(i));
        } else {
            v.push_unchecked(Option::<i32>.none());
        }
        i = i + 1;
    }
    var sum: i32 = 0;
    var somes: i32 = 0;
    i = 0;
    while (i < 2000) {
        let o = v.get_unchecked(i as i64);
        if (o.is_some()) {
            sum = sum + o.unwrap_or(0);
            somes = somes + 1;
        }
        i = i + 1;
    }
    print("nested_option_vec len=${v.size()} sum=${sum} somes=${somes}");
    return 0;
}
'@
    $cppOption = @'
#include <iostream>
#include <optional>
#include <vector>

int main() {
    std::vector<std::optional<int>> v;
    v.reserve(2000);
    for (int i = 0; i < 2000; ++i) {
        if ((i % 3) == 0) {
            v.push_back(i);
        } else {
            v.push_back(std::nullopt);
        }
    }
    int sum = 0;
    int somes = 0;
    for (int i = 0; i < 2000; ++i) {
        const auto& o = v[static_cast<std::size_t>(i)];
        if (o.has_value()) {
            sum += o.value_or(0);
            somes += 1;
        }
    }
    std::cout << "nested_option_vec len=" << v.size() << " sum=" << sum << " somes=" << somes << "\n";
    return 0;
}
'@
    Add-BenchmarkFixture $items $GeneratedDir "nested_option_vec" "nested generics+Option" $vyxOption $cppOption

    $vyxHashMap = @'
module bench_hashmap_i64;

use std.collections;

fn main() -> i32 {
    var m = HashMap::<i64, i64>.with_capacity(4096);
    var i: i64 = 0;
    while (i < 3500) {
        let key = (i * 1103515245 + 12345) % 8191;
        m.put(key, i * 3 + 7);
        i = i + 1;
    }

    var hits: i64 = 0;
    var sum: i64 = 0;
    i = 0;
    while (i < 3500) {
        let key = (i * 1103515245 + 12345) % 8191;
        if (m.contains(key)) {
            sum = (sum + m.get(key)) % 1000000007;
            hits = hits + 1;
        }
        i = i + 1;
    }

    var removed: i64 = 0;
    i = 0;
    while (i < 3500) {
        if ((i % 5) == 0) {
            let key = (i * 1103515245 + 12345) % 8191;
            if (m.remove(key)) { removed = removed + 1; }
        }
        i = i + 1;
    }

    print("hashmap_i64 size=${m.size()} hits=${hits} removed=${removed} sum=${sum}");
    return 0;
}
'@
    $cppHashMap = @'
#include <cstdint>
#include <iostream>
#include <unordered_map>

int main() {
    std::unordered_map<std::int64_t, std::int64_t> m;
    m.reserve(4096);
    for (std::int64_t i = 0; i < 3500; ++i) {
        const auto key = (i * 1103515245LL + 12345LL) % 8191LL;
        m[key] = i * 3 + 7;
    }

    std::int64_t hits = 0;
    std::int64_t sum = 0;
    for (std::int64_t i = 0; i < 3500; ++i) {
        const auto key = (i * 1103515245LL + 12345LL) % 8191LL;
        const auto it = m.find(key);
        if (it != m.end()) {
            sum = (sum + it->second) % 1000000007LL;
            hits += 1;
        }
    }

    std::int64_t removed = 0;
    for (std::int64_t i = 0; i < 3500; ++i) {
        if ((i % 5) == 0) {
            const auto key = (i * 1103515245LL + 12345LL) % 8191LL;
            if (m.erase(key) != 0) { removed += 1; }
        }
    }

    std::cout << "hashmap_i64 size=" << m.size() << " hits=" << hits
              << " removed=" << removed << " sum=" << sum << "\n";
    return 0;
}
'@
    Add-BenchmarkFixture $items $GeneratedDir "hashmap_i64" "HashMap insert+lookup+remove" $vyxHashMap $cppHashMap

    $vyxBTree = @'
module bench_btree_ordered_map;

use std.core;
use std.collections;

fn main() -> i32 {
    var m = BTreeMap::<i64, i64>.with_capacity(2048);
    var i: i64 = 0;
    while (i < 1800) {
        let key = (i * 37) % 2503;
        m.put(key, i * 11 - key);
        i = i + 1;
    }

    var sum: i64 = 0;
    var hits: i64 = 0;
    i = 0;
    while (i < 1800) {
        let key = (i * 29) % 2503;
        match (m.get(key)) {
            case Some(v) => {
                sum = (sum + v) % 1000000007;
                hits = hits + 1;
            }
            case None => {}
        }
        i = i + 1;
    }

    var removed: i64 = 0;
    i = 0;
    while (i < 1800) {
        if ((i % 7) == 0) {
            let key = (i * 37) % 2503;
            if (m.remove(key)) { removed = removed + 1; }
        }
        i = i + 1;
    }

    var first: i64 = -1;
    var last: i64 = -1;
    match (m.first_key()) {
        case Some(k) => { first = k; }
        case None => {}
    }
    match (m.last_key()) {
        case Some(k) => { last = k; }
        case None => {}
    }

    print("btree_ordered_map size=${m.size()} first=${first} last=${last} hits=${hits} removed=${removed} sum=${sum}");
    return 0;
}
'@
    $cppBTree = @'
#include <cstdint>
#include <iostream>
#include <map>

int main() {
    std::map<std::int64_t, std::int64_t> m;
    for (std::int64_t i = 0; i < 1800; ++i) {
        const auto key = (i * 37) % 2503;
        m[key] = i * 11 - key;
    }

    std::int64_t sum = 0;
    std::int64_t hits = 0;
    for (std::int64_t i = 0; i < 1800; ++i) {
        const auto key = (i * 29) % 2503;
        const auto it = m.find(key);
        if (it != m.end()) {
            sum = (sum + it->second) % 1000000007LL;
            hits += 1;
        }
    }

    std::int64_t removed = 0;
    for (std::int64_t i = 0; i < 1800; ++i) {
        if ((i % 7) == 0) {
            const auto key = (i * 37) % 2503;
            if (m.erase(key) != 0) { removed += 1; }
        }
    }

    const auto first = m.empty() ? -1LL : m.begin()->first;
    const auto last = m.empty() ? -1LL : m.rbegin()->first;
    std::cout << "btree_ordered_map size=" << m.size() << " first=" << first
              << " last=" << last << " hits=" << hits << " removed=" << removed
              << " sum=" << sum << "\n";
    return 0;
}
'@
    Add-BenchmarkFixture $items $GeneratedDir "btree_ordered_map" "BTreeMap ordered ops" $vyxBTree $cppBTree

    $vyxSortSearch = @'
module bench_vec_sort_search;

use std.core;
use std.collections;
use std.algorithm;

fn main() -> i32 {
    var v = Vec::<i64>.with_capacity(8000);
    var i: i64 = 0;
    while (i < 8000) {
        v.push_unchecked((i * 48271 + 17) % 100003);
        i = i + 1;
    }

    sort(v);

    var hits: i64 = 0;
    var checksum: i64 = 0;
    i = 0;
    while (i < 2000) {
        let needle = (i * 97) % 100003;
        let pos = binary_search::<i64>(v, needle);
        if (pos >= 0) {
            hits = hits + 1;
            checksum = (checksum + v.get_unchecked(pos)) % 1000000007;
        }
        i = i + 1;
    }

    print("vec_sort_search len=${v.size()} first=${v.get_unchecked(0)} last=${v.get_unchecked(v.size() - 1)} hits=${hits} checksum=${checksum}");
    return 0;
}
'@
    $cppSortSearch = @'
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <vector>

int main() {
    std::vector<std::int64_t> v;
    v.reserve(8000);
    for (std::int64_t i = 0; i < 8000; ++i) {
        v.push_back((i * 48271 + 17) % 100003);
    }

    std::sort(v.begin(), v.end());

    std::int64_t hits = 0;
    std::int64_t checksum = 0;
    for (std::int64_t i = 0; i < 2000; ++i) {
        const auto needle = (i * 97) % 100003;
        const auto it = std::lower_bound(v.begin(), v.end(), needle);
        if (it != v.end() && *it == needle) {
            hits += 1;
            checksum = (checksum + *it) % 1000000007LL;
        }
    }

    std::cout << "vec_sort_search len=" << v.size() << " first=" << v.front()
              << " last=" << v.back() << " hits=" << hits
              << " checksum=" << checksum << "\n";
    return 0;
}
'@
    Add-BenchmarkFixture $items $GeneratedDir "vec_sort_search" "Vec sort+binary_search" $vyxSortSearch $cppSortSearch

    $vyxResult = @'
module bench_result_pipeline;

use std.core;

fn parse_digits(s: string) -> Result<i32, string> {
    if (s.isEmpty()) { return Result::<i32, string>.err("empty"); }
    var v: i32 = 0;
    var i: i64 = 0;
    while (i < s.len) {
        let c = s.charAt(i);
        if (c < '0' || c > '9') {
            return Result::<i32, string>.err("nondigit");
        }
        v = v * 10 + (c as i32 - '0' as i32);
        i = i + 1;
    }
    return Result::<i32, string>.ok(v);
}

fn normalize(n: i32) -> Result<i32, string> {
    if ((n % 2) == 0) { return Result::<i32, string>.ok(n / 2); }
    return Result::<i32, string>.err("odd");
}

fn main() -> i32 {
    var sum: i32 = 0;
    var errs: i32 = 0;
    var i: i32 = 0;
    while (i < 3000) {
        let parsed = parse_digits("${i}");
        match (parsed) {
            case Ok(n) => {
                let normalized = normalize(n);
                match (normalized) {
                    case Ok(v) => { sum = (sum + v) % 1000000007; }
                    case Err(_) => { errs = errs + 1; }
                }
            }
            case Err(_) => { errs = errs + 1; }
        }
        i = i + 1;
    }

    print("result_pipeline sum=${sum} errs=${errs}");
    return 0;
}
'@
    $cppResult = @'
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>

static std::optional<int> parse_digits(const std::string& s) {
    if (s.empty()) { return std::nullopt; }
    int v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') { return std::nullopt; }
        v = v * 10 + (c - '0');
    }
    return v;
}

static std::optional<int> normalize(int n) {
    if ((n % 2) == 0) { return n / 2; }
    return std::nullopt;
}

int main() {
    int sum = 0;
    int errs = 0;
    for (int i = 0; i < 3000; ++i) {
        const auto parsed = parse_digits(std::to_string(i));
        if (parsed.has_value()) {
            const auto normalized = normalize(*parsed);
            if (normalized.has_value()) {
                sum = (sum + *normalized) % 1000000007;
            } else {
                errs += 1;
            }
        } else {
            errs += 1;
        }
    }

    std::cout << "result_pipeline sum=" << sum << " errs=" << errs << "\n";
    return 0;
}
'@
    Add-BenchmarkFixture $items $GeneratedDir "result_pipeline" "Result+match+string parse" $vyxResult $cppResult

    $vyxTrait = @'
module bench_trait_numeric_generic;

use std.core;
use std.collections;
use std.num;
use std.ops;

fn generic_sum<T>(xs: Vec<T>) -> T where T: Num {
    var s = T::zero();
    var i: i64 = 0;
    while (i < xs.size()) {
        s = s.operator_add(xs.get(i));
        i = i + 1;
    }
    return s;
}

fn main() -> i32 {
    var v = Vec::<i64>.with_capacity(12000);
    var i: i64 = 0;
    while (i < 12000) {
        v.push_unchecked((i * 13 + 5) % 1009);
        i = i + 1;
    }
    let n = v.size();
    let s = generic_sum::<i64>(v);
    print("trait_numeric_generic len=${n} sum=${s}");
    return 0;
}
'@
    $cppTrait = @'
#include <cstdint>
#include <iostream>
#include <vector>

template <class T>
static T generic_sum(const std::vector<T>& xs) {
    T s{};
    for (const auto& x : xs) {
        s = s + x;
    }
    return s;
}

int main() {
    std::vector<std::int64_t> v;
    v.reserve(12000);
    for (std::int64_t i = 0; i < 12000; ++i) {
        v.push_back((i * 13 + 5) % 1009);
    }
    const auto s = generic_sum(v);
    std::cout << "trait_numeric_generic len=" << v.size() << " sum=" << s << "\n";
    return 0;
}
'@
    Add-BenchmarkFixture $items $GeneratedDir "trait_numeric_generic" "trait-bound generic numeric" $vyxTrait $cppTrait

    $vyxStringTransform = @'
module bench_string_transform;

use std.string;

fn main() -> i32 {
    var hits: i64 = 0;
    var total: i64 = 0;
    var i: i64 = 0;
    while (i < 2500) {
        let raw = "AbC-${i}-zZ";
        let upper = raw.toUpper();
        let lower = upper.toLower();
        if (upper.contains("ABC")) { hits = hits + 1; }
        if (lower.endsWith("zz")) { hits = hits + 2; }
        let mid = raw.substring(4, raw.len - 3);
        total = total + mid.len;
        i = i + 1;
    }

    print("string_transform hits=${hits} total=${total}");
    return 0;
}
'@
    $cppStringTransform = @'
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iostream>
#include <string>

static std::string to_upper(std::string s) {
    for (char& c : s) { c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }
    return s;
}

static std::string to_lower(std::string s) {
    for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return s;
}

static bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

int main() {
    std::int64_t hits = 0;
    std::int64_t total = 0;
    for (std::int64_t i = 0; i < 2500; ++i) {
        const std::string raw = "AbC-" + std::to_string(i) + "-zZ";
        const auto upper = to_upper(raw);
        const auto lower = to_lower(upper);
        if (upper.find("ABC") != std::string::npos) { hits += 1; }
        if (ends_with(lower, "zz")) { hits += 2; }
        const auto mid = raw.substr(4, raw.size() - 7);
        total += static_cast<std::int64_t>(mid.size());
    }

    std::cout << "string_transform hits=" << hits << " total=" << total << "\n";
    return 0;
}
'@
    Add-BenchmarkFixture $items $GeneratedDir "string_transform" "string case+substring predicates" $vyxStringTransform $cppStringTransform

    $vyxMatrixSmallInt = @'
module bench_matrix_small_int;

fn main() -> i32 {
    var a00: i64 = 1;
    var a01: i64 = 2;
    var a10: i64 = 3;
    var a11: i64 = 5;
    var checksum: i64 = 0;
    var i: i64 = 0;
    while (i < 12000) {
        let b00 = (a00 + a01 + i) % 1000003;
        let b01 = (a00 * 2 + a11 + i * 3) % 1000003;
        let b10 = (a10 + a11 * 2 + i * 5) % 1000003;
        let b11 = (a01 + a10 + a11 + i * 7) % 1000003;
        a00 = b00;
        a01 = b01;
        a10 = b10;
        a11 = b11;
        checksum = (checksum + a00 * 3 + a01 * 5 + a10 * 7 + a11 * 11) % 1000000007;
        i = i + 1;
    }
    print("matrix_small_int a00=${a00} a11=${a11} checksum=${checksum}");
    return 0;
}
'@
    $cppMatrixSmallInt = @'
#include <cstdint>
#include <iostream>

int main() {
    std::int64_t a00 = 1;
    std::int64_t a01 = 2;
    std::int64_t a10 = 3;
    std::int64_t a11 = 5;
    std::int64_t checksum = 0;
    for (std::int64_t i = 0; i < 12000; ++i) {
        const auto b00 = (a00 + a01 + i) % 1000003LL;
        const auto b01 = (a00 * 2 + a11 + i * 3) % 1000003LL;
        const auto b10 = (a10 + a11 * 2 + i * 5) % 1000003LL;
        const auto b11 = (a01 + a10 + a11 + i * 7) % 1000003LL;
        a00 = b00;
        a01 = b01;
        a10 = b10;
        a11 = b11;
        checksum = (checksum + a00 * 3 + a01 * 5 + a10 * 7 + a11 * 11) % 1000000007LL;
    }
    std::cout << "matrix_small_int a00=" << a00 << " a11=" << a11
              << " checksum=" << checksum << "\n";
    return 0;
}
'@
    Add-BenchmarkFixture $items $GeneratedDir "matrix_small_int" "small integer matrix recurrence" $vyxMatrixSmallInt $cppMatrixSmallInt

    $vyxAsciiTokenScan = @'
module bench_ascii_token_scan;

use std.string;

fn main() -> i32 {
    var digits: i64 = 0;
    var alpha: i64 = 0;
    var separators: i64 = 0;
    var checksum: i64 = 0;
    var i: i64 = 0;
    while (i < 5000) {
        let s = "key-${i}-value-${(i * 37) % 1009}";
        var j: i64 = 0;
        while (j < s.len) {
            let c = s.charAt(j);
            if (c >= '0' && c <= '9') {
                digits = digits + 1;
                checksum = (checksum + (c as i64 - '0' as i64) * 3) % 1000000007;
            } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
                alpha = alpha + 1;
                checksum = (checksum + c as i64) % 1000000007;
            } else {
                separators = separators + 1;
                checksum = (checksum + 17) % 1000000007;
            }
            j = j + 1;
        }
        i = i + 1;
    }
    print("ascii_token_scan digits=${digits} alpha=${alpha} separators=${separators} checksum=${checksum}");
    return 0;
}
'@
    $cppAsciiTokenScan = @'
#include <cstdint>
#include <iostream>
#include <string>

int main() {
    std::int64_t digits = 0;
    std::int64_t alpha = 0;
    std::int64_t separators = 0;
    std::int64_t checksum = 0;
    for (std::int64_t i = 0; i < 5000; ++i) {
        const std::string s = "key-" + std::to_string(i) + "-value-" + std::to_string((i * 37) % 1009);
        for (char c : s) {
            if (c >= '0' && c <= '9') {
                digits += 1;
                checksum = (checksum + (c - '0') * 3) % 1000000007LL;
            } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
                alpha += 1;
                checksum = (checksum + static_cast<unsigned char>(c)) % 1000000007LL;
            } else {
                separators += 1;
                checksum = (checksum + 17) % 1000000007LL;
            }
        }
    }
    std::cout << "ascii_token_scan digits=" << digits << " alpha=" << alpha
              << " separators=" << separators << " checksum=" << checksum << "\n";
    return 0;
}
'@
    Add-BenchmarkFixture $items $GeneratedDir "ascii_token_scan" "ASCII string token scanning" $vyxAsciiTokenScan $cppAsciiTokenScan

    $vyxBranchyStateMachine = @'
module bench_branchy_state_machine;

fn main() -> i32 {
    var state: i64 = 0;
    var accepted: i64 = 0;
    var checksum: i64 = 11;
    var i: i64 = 0;
    while (i < 30000) {
        let input = (i * 17 + state * 31 + 7) % 9;
        if (state == 0) {
            if (input < 3) { state = 1; } else if (input < 6) { state = 2; } else { state = 3; }
        } else if (state == 1) {
            if ((input % 2) == 0) { state = 4; } else { state = 2; }
        } else if (state == 2) {
            if (input == 5) { state = 0; } else { state = 3; }
        } else if (state == 3) {
            if (input > 6) { state = 4; } else { state = 1; }
        } else {
            accepted = accepted + 1;
            state = input % 4;
        }
        checksum = (checksum * 131 + state * 17 + input) % 1000000007;
        i = i + 1;
    }
    print("branchy_state_machine state=${state} accepted=${accepted} checksum=${checksum}");
    return 0;
}
'@
    $cppBranchyStateMachine = @'
#include <cstdint>
#include <iostream>

int main() {
    std::int64_t state = 0;
    std::int64_t accepted = 0;
    std::int64_t checksum = 11;
    for (std::int64_t i = 0; i < 30000; ++i) {
        const auto input = (i * 17 + state * 31 + 7) % 9;
        if (state == 0) {
            if (input < 3) { state = 1; } else if (input < 6) { state = 2; } else { state = 3; }
        } else if (state == 1) {
            if ((input % 2) == 0) { state = 4; } else { state = 2; }
        } else if (state == 2) {
            if (input == 5) { state = 0; } else { state = 3; }
        } else if (state == 3) {
            if (input > 6) { state = 4; } else { state = 1; }
        } else {
            accepted += 1;
            state = input % 4;
        }
        checksum = (checksum * 131 + state * 17 + input) % 1000000007LL;
    }
    std::cout << "branchy_state_machine state=" << state << " accepted=" << accepted
              << " checksum=" << checksum << "\n";
    return 0;
}
'@
    Add-BenchmarkFixture $items $GeneratedDir "branchy_state_machine" "branch-heavy state machine" $vyxBranchyStateMachine $cppBranchyStateMachine

    $vyxGenericMinmaxPipeline = @'
module bench_generic_minmax_pipeline;

fn min_value<T>(a: T, b: T) -> T {
    if (a < b) { return a; }
    return b;
}

fn max_value<T>(a: T, b: T) -> T {
    if (a > b) { return a; }
    return b;
}

fn fold_i64(n: i64) -> i64 {
    var low: i64 = 1000000;
    var high: i64 = -1000000;
    var checksum: i64 = 0;
    var i: i64 = 0;
    while (i < n) {
        let x = (i * 48271 + 13) % 200003 - 100001;
        low = min_value::<i64>(low, x);
        high = max_value::<i64>(high, x);
        checksum = (checksum + (high - low) + x) % 1000000007;
        i = i + 1;
    }
    return checksum + low + high;
}

fn fold_i32(n: i32) -> i32 {
    var low: i32 = 1000000;
    var high: i32 = -1000000;
    var checksum: i32 = 0;
    var i: i32 = 0;
    while (i < n) {
        let x = (i * 7919 + 23) % 100003 - 50001;
        low = min_value::<i32>(low, x);
        high = max_value::<i32>(high, x);
        checksum = (checksum + (high - low) + x) % 1000000007;
        i = i + 1;
    }
    return checksum + low + high;
}

fn main() -> i32 {
    let a = fold_i64(16000);
    let b = fold_i32(9000);
    print("generic_minmax_pipeline a=${a} b=${b}");
    return 0;
}
'@
    $cppGenericMinmaxPipeline = @'
#include <cstdint>
#include <iostream>

template <class T>
static T min_value(T a, T b) {
    return a < b ? a : b;
}

template <class T>
static T max_value(T a, T b) {
    return a > b ? a : b;
}

static std::int64_t fold_i64(std::int64_t n) {
    std::int64_t low = 1000000;
    std::int64_t high = -1000000;
    std::int64_t checksum = 0;
    for (std::int64_t i = 0; i < n; ++i) {
        const auto x = (i * 48271 + 13) % 200003 - 100001;
        low = min_value<std::int64_t>(low, x);
        high = max_value<std::int64_t>(high, x);
        checksum = (checksum + (high - low) + x) % 1000000007LL;
    }
    return checksum + low + high;
}

static std::int32_t fold_i32(std::int32_t n) {
    std::int32_t low = 1000000;
    std::int32_t high = -1000000;
    std::int32_t checksum = 0;
    for (std::int32_t i = 0; i < n; ++i) {
        const auto x = (i * 7919 + 23) % 100003 - 50001;
        low = min_value<std::int32_t>(low, x);
        high = max_value<std::int32_t>(high, x);
        checksum = (checksum + (high - low) + x) % 1000000007;
    }
    return checksum + low + high;
}

int main() {
    const auto a = fold_i64(16000);
    const auto b = fold_i32(9000);
    std::cout << "generic_minmax_pipeline a=" << a << " b=" << b << "\n";
    return 0;
}
'@
    Add-BenchmarkFixture $items $GeneratedDir "generic_minmax_pipeline" "generic minmax pipeline" $vyxGenericMinmaxPipeline $cppGenericMinmaxPipeline

    return $items
}

function Add-BenchmarkFixture(
    [System.Collections.Generic.List[object]]$Items,
    [string]$GeneratedDir,
    [string]$Name,
    [string]$Category,
    [string]$VyxSource,
    [string]$CppSource
) {
    $vyxPath = Join-Path $GeneratedDir ($Name + ".vyx")
    $cppPath = Join-Path $GeneratedDir ($Name + ".cpp")
    Write-Utf8NoBom $vyxPath $VyxSource
    Write-Utf8NoBom $cppPath $CppSource
    [void]$Items.Add([pscustomobject]@{
        Name = $Name
        Category = $Category
        VyxPath = (Resolve-Path -LiteralPath $vyxPath).Path
        CppPath = (Resolve-Path -LiteralPath $cppPath).Path
    })
}

if ($Warmup -lt 0) { throw "Warmup must be >= 0." }
if ($Reps -lt 1) { throw "Reps must be >= 1." }

$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..\..")).Path
$outRoot = $OutDir
if (![System.IO.Path]::IsPathRooted($outRoot)) { $outRoot = Join-Path $repoRoot $outRoot }
New-Item -ItemType Directory -Force -Path $outRoot | Out-Null
$outRoot = (Resolve-Path -LiteralPath $outRoot).Path

$genDir = Join-Path $outRoot "generated"
$logDir = Join-Path $outRoot "logs"
$binDir = Join-Path $outRoot "bin"
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
New-Item -ItemType Directory -Force -Path $binDir | Out-Null

$baselineExe = Resolve-Executable $BaselineCompiler "BaselineCompiler"
$bootstrapExe = Resolve-Executable $BootstrapCompiler "BootstrapCompiler"
$clangExe = Resolve-Executable $ClangCompiler "ClangCompiler"
$runtimePath = Resolve-ExistingPath $RuntimeDir "RuntimeDir"

$benchmarks = Write-BenchmarkFixtures $genDir
$targets = @(
    [pscustomobject]@{ Name = "baseline"; Kind = "vyx"; Exe = $baselineExe },
    [pscustomobject]@{ Name = "bootstrap"; Kind = "vyx"; Exe = $bootstrapExe },
    [pscustomobject]@{ Name = "cpp26"; Kind = "cpp"; Exe = $clangExe }
)

$rows = New-Object System.Collections.Generic.List[object]
$totalIterations = $Warmup + $Reps
for ($iter = 1; $iter -le $totalIterations; $iter++) {
    $phase = "measure"
    $measureIter = $iter - $Warmup
    if ($iter -le $Warmup) {
        $phase = "warmup"
        $measureIter = $iter
    }

    foreach ($bench in $benchmarks) {
        $orderedTargets = @($targets)
        if (($iter % 2) -eq 0) { [Array]::Reverse($orderedTargets) }
        $slot = 0
        foreach ($target in $orderedTargets) {
            $slot += 1
            $tag = "{0}_{1}_{2:00}_{3}_{4}" -f $bench.Name, $phase, $measureIter, $target.Name, $slot
            $compileStdout = Join-Path $logDir ($tag + ".compile.stdout.log")
            $compileStderr = Join-Path $logDir ($tag + ".compile.stderr.log")
            $compileCommand = Join-Path $logDir ($tag + ".compile.command.txt")
            $runStdout = Join-Path $logDir ($tag + ".run.stdout.log")
            $runStderr = Join-Path $logDir ($tag + ".run.stderr.log")
            $runCommand = Join-Path $logDir ($tag + ".run.command.txt")
            $exeOut = Join-Path $binDir ($tag + ".exe")

            $compileArgs = @()
            $sourcePath = ""
            if ($target.Kind -eq "vyx") {
                $sourcePath = $bench.VyxPath
                if ($target.Name -eq "bootstrap") {
                    $compileArgs = @("--src=file", $sourcePath, "--emit=exe", "-O2", "-o", $exeOut, "-L", $runtimePath, "-l", "vyx_codegen")
                } else {
                    $compileArgs = @($sourcePath, "-O2", "-o", $exeOut, "-L", $runtimePath, "-l", "vyx_codegen")
                }
            } else {
                $sourcePath = $bench.CppPath
                $compileArgs = @("-std=$CppStandard", "-O2", $sourcePath, "-o", $exeOut)
            }

            $compile = Invoke-ProcessTimed -Exe $target.Exe `
                                           -ArgList $compileArgs `
                                           -StdoutPath $compileStdout `
                                           -StderrPath $compileStderr `
                                           -CommandPath $compileCommand `
                                           -ExtraPath $runtimePath `
                                           -TimeoutSec ([Math]::Max(60, $RunTimeoutSec))

            $runExit = -9999
            $runTimedOut = $false
            $runMs = 0.0
            $runOutput = ""
            if ($compile.ExitCode -eq 0 -and (Test-Path -LiteralPath $exeOut)) {
                $run = Invoke-ProcessTimed -Exe $exeOut `
                                           -ArgList @() `
                                           -StdoutPath $runStdout `
                                           -StderrPath $runStderr `
                                           -CommandPath $runCommand `
                                           -ExtraPath $runtimePath `
                                           -TimeoutSec $RunTimeoutSec
                $runExit = $run.ExitCode
                $runTimedOut = $run.TimedOut
                $runMs = $run.ElapsedMs
                $runOutput = Normalize-Output $run.Stdout
            } else {
                Set-Content -LiteralPath $runStdout -Value "" -Encoding UTF8
                Set-Content -LiteralPath $runStderr -Value "compile failed; run skipped" -Encoding UTF8
                Set-Content -LiteralPath $runCommand -Value "" -Encoding UTF8
            }

            [void]$rows.Add([pscustomobject]@{
                Phase = $phase
                Iteration = $measureIter
                Slot = $slot
                Benchmark = $bench.Name
                Category = $bench.Category
                Target = $target.Name
                Source = $sourcePath
                CompileExitCode = $compile.ExitCode
                CompileTimedOut = $compile.TimedOut
                CompileMs = $compile.ElapsedMs
                RunExitCode = $runExit
                RunTimedOut = $runTimedOut
                RunMs = [Math]::Round([double]$runMs, 3)
                Output = $runOutput
                Exe = $exeOut
                CompileCommand = $compileCommand
                CompileStdout = $compileStdout
                CompileStderr = $compileStderr
                RunCommand = $runCommand
                RunStdout = $runStdout
                RunStderr = $runStderr
            })
        }
    }
}

$rowsPath = Join-Path $outRoot "rows.csv"
$perfSummaryPath = Join-Path $outRoot "performance_summary.csv"
$targetSummaryPath = Join-Path $outRoot "target_summary.csv"
$comparisonSummaryPath = Join-Path $outRoot "comparison_summary.csv"
$semanticSummaryPath = Join-Path $outRoot "semantic_summary.csv"
$summaryTextPath = Join-Path $outRoot "summary.txt"

$rows | Export-Csv -LiteralPath $rowsPath -NoTypeInformation -Encoding UTF8

$perf = New-Object System.Collections.Generic.List[object]
foreach ($bench in $benchmarks) {
    foreach ($target in $targets) {
        $vals = @($rows | Where-Object { $_.Phase -eq "measure" -and $_.Benchmark -eq $bench.Name -and $_.Target -eq $target.Name })
        $compileVals = @($vals | ForEach-Object { [double]$_.CompileMs })
        $runVals = @($vals | ForEach-Object { [double]$_.RunMs })
        [void]$perf.Add([pscustomobject]@{
            Benchmark = $bench.Name
            Category = $bench.Category
            Target = $target.Name
            Count = $vals.Count
            CompileMinMs = [Math]::Round([double](($compileVals | Measure-Object -Minimum).Minimum), 3)
            CompileAvgMs = [Math]::Round([double](($compileVals | Measure-Object -Average).Average), 3)
            CompileMedianMs = [Math]::Round([double](Median $compileVals), 3)
            CompileMaxMs = [Math]::Round([double](($compileVals | Measure-Object -Maximum).Maximum), 3)
            RunMinMs = [Math]::Round([double](($runVals | Measure-Object -Minimum).Minimum), 3)
            RunAvgMs = [Math]::Round([double](($runVals | Measure-Object -Average).Average), 3)
            RunMedianMs = [Math]::Round([double](Median $runVals), 3)
            RunMaxMs = [Math]::Round([double](($runVals | Measure-Object -Maximum).Maximum), 3)
        })
    }
}
$perf | Export-Csv -LiteralPath $perfSummaryPath -NoTypeInformation -Encoding UTF8

$targetSummary = New-Object System.Collections.Generic.List[object]
foreach ($target in $targets) {
    $vals = @($perf | Where-Object { $_.Target -eq $target.Name })
    [void]$targetSummary.Add([pscustomobject]@{
        Target = $target.Name
        BenchmarkCount = $vals.Count
        CompileAvgMeanMs = [Math]::Round([double](($vals | Measure-Object CompileAvgMs -Average).Average), 3)
        CompileMedianMeanMs = [Math]::Round([double](($vals | Measure-Object CompileMedianMs -Average).Average), 3)
        RunAvgMeanMs = [Math]::Round([double](($vals | Measure-Object RunAvgMs -Average).Average), 3)
        RunMedianMeanMs = [Math]::Round([double](($vals | Measure-Object RunMedianMs -Average).Average), 3)
    })
}
$targetSummary | Export-Csv -LiteralPath $targetSummaryPath -NoTypeInformation -Encoding UTF8

$comparison = New-Object System.Collections.Generic.List[object]
foreach ($bench in $benchmarks) {
    $baselinePerf = $perf | Where-Object { $_.Benchmark -eq $bench.Name -and $_.Target -eq "baseline" } | Select-Object -First 1
    $bootstrapPerf = $perf | Where-Object { $_.Benchmark -eq $bench.Name -and $_.Target -eq "bootstrap" } | Select-Object -First 1
    $cppPerf = $perf | Where-Object { $_.Benchmark -eq $bench.Name -and $_.Target -eq "cpp26" } | Select-Object -First 1
    [void]$comparison.Add([pscustomobject]@{
        Benchmark = $bench.Name
        Category = $bench.Category
        BootstrapCompileVsBaseline = [Math]::Round([double]$bootstrapPerf.CompileAvgMs / [double]$baselinePerf.CompileAvgMs, 3)
        BootstrapRunVsBaseline = [Math]::Round([double]$bootstrapPerf.RunAvgMs / [double]$baselinePerf.RunAvgMs, 3)
        Cpp26CompileVsBaseline = [Math]::Round([double]$cppPerf.CompileAvgMs / [double]$baselinePerf.CompileAvgMs, 3)
        Cpp26RunVsBaseline = [Math]::Round([double]$cppPerf.RunAvgMs / [double]$baselinePerf.RunAvgMs, 3)
    })
}
$comparison | Export-Csv -LiteralPath $comparisonSummaryPath -NoTypeInformation -Encoding UTF8

$semantic = New-Object System.Collections.Generic.List[object]
foreach ($bench in $benchmarks) {
    $measureRows = @($rows | Where-Object { $_.Phase -eq "measure" -and $_.Benchmark -eq $bench.Name })
    $baselineOutputs = @($measureRows | Where-Object { $_.Target -eq "baseline" -and $_.CompileExitCode -eq 0 -and $_.RunExitCode -eq 0 } | Select-Object -ExpandProperty Output -Unique)
    $expected = ""
    if ($baselineOutputs.Count -gt 0) { $expected = [string]$baselineOutputs[0] }
    foreach ($target in $targets) {
        $targetRows = @($measureRows | Where-Object { $_.Target -eq $target.Name })
        $okExit = @($targetRows | Where-Object { $_.CompileExitCode -ne 0 -or $_.RunExitCode -ne 0 -or $_.CompileTimedOut -or $_.RunTimedOut }).Count -eq 0
        $outputs = @($targetRows | Select-Object -ExpandProperty Output -Unique)
        $outputStable = $outputs.Count -eq 1
        $matchesBaseline = $false
        if ($expected.Length -gt 0 -and $outputStable) {
            $matchesBaseline = ([string]$outputs[0]) -eq $expected
        }
        [void]$semantic.Add([pscustomobject]@{
            Benchmark = $bench.Name
            Category = $bench.Category
            Target = $target.Name
            ExitOk = $okExit
            OutputStable = $outputStable
            MatchesBaseline = $matchesBaseline
            ExpectedOutput = $expected
            ObservedOutput = (($outputs | ForEach-Object { [string]$_ }) -join " | ")
        })
    }
}
$semantic | Export-Csv -LiteralPath $semanticSummaryPath -NoTypeInformation -Encoding UTF8

$lines = New-Object System.Collections.Generic.List[string]
[void]$lines.Add("language feature and performance benchmark")
[void]$lines.Add("out_dir: $outRoot")
[void]$lines.Add("warmup: $Warmup")
[void]$lines.Add("reps: $Reps")
[void]$lines.Add("cpp_standard: $CppStandard")
[void]$lines.Add("run timings are process elapsed time and include executable startup")
[void]$lines.Add("")
[void]$lines.Add("semantic summary")
foreach ($item in $semantic) {
    [void]$lines.Add(("{0} {1} exit_ok={2} stable={3} matches_baseline={4}" -f $item.Benchmark, $item.Target, $item.ExitOk, $item.OutputStable, $item.MatchesBaseline))
}
[void]$lines.Add("")
[void]$lines.Add("performance summary")
foreach ($item in $perf) {
    [void]$lines.Add(("{0} {1} compile_avg_ms={2} compile_median_ms={3} run_avg_ms={4} run_median_ms={5}" -f $item.Benchmark, $item.Target, $item.CompileAvgMs, $item.CompileMedianMs, $item.RunAvgMs, $item.RunMedianMs))
}
[void]$lines.Add("")
[void]$lines.Add("target summary")
foreach ($item in $targetSummary) {
    [void]$lines.Add(("{0} compile_avg_mean_ms={1} compile_median_mean_ms={2} run_avg_mean_ms={3} run_median_mean_ms={4}" -f $item.Target, $item.CompileAvgMeanMs, $item.CompileMedianMeanMs, $item.RunAvgMeanMs, $item.RunMedianMeanMs))
}
[void]$lines.Add("")
[void]$lines.Add("baseline ratio summary")
foreach ($item in $comparison) {
    [void]$lines.Add(("{0} bootstrap_compile_x={1} bootstrap_run_x={2} cpp26_compile_x={3} cpp26_run_x={4}" -f $item.Benchmark, $item.BootstrapCompileVsBaseline, $item.BootstrapRunVsBaseline, $item.Cpp26CompileVsBaseline, $item.Cpp26RunVsBaseline))
}
[void]$lines.Add("")
[void]$lines.Add("rows_csv: $rowsPath")
[void]$lines.Add("performance_csv: $perfSummaryPath")
[void]$lines.Add("target_csv: $targetSummaryPath")
[void]$lines.Add("comparison_csv: $comparisonSummaryPath")
[void]$lines.Add("semantic_csv: $semanticSummaryPath")

Set-Content -LiteralPath $summaryTextPath -Value $lines -Encoding UTF8
Get-Content -LiteralPath $summaryTextPath
