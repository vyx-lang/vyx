param(
    [string]$CacaoRoot = "E:\Dev\C++\Cacao",
    [string]$Clang = ""
)

$ErrorActionPreference = "Stop"
$projectRoot = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $projectRoot "..\..\..")).Path
$cacaoRoot = (Resolve-Path $CacaoRoot).Path
$bridgeRoot = Join-Path $projectRoot "bridge"
$buildRoot = Join-Path $bridgeRoot "build"

cmake -S $bridgeRoot -B $buildRoot -G Ninja -DCMAKE_BUILD_TYPE=Debug "-DCACAO_ROOT=$cacaoRoot"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmake --build $buildRoot --parallel 10
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Copy-Item -Force (Join-Path $buildRoot "bin\CacaoDciBridge.dll") (Join-Path $projectRoot "third_party\cacao\bin\CacaoDciBridge.dll")
Copy-Item -Force (Join-Path $buildRoot "lib\CacaoDciBridge.lib") (Join-Path $projectRoot "third_party\cacao\lib\CacaoDciBridge.lib")
Copy-Item -Force (Join-Path $cacaoRoot "demos\hello_triangle.slang") (Join-Path $projectRoot "third_party\cacao\bin\hello_triangle.slang")

if ([string]::IsNullOrWhiteSpace($Clang)) {
    $Clang = (Resolve-Path (Join-Path $repoRoot "clang\bin\clang++.exe")).Path
} else {
    $Clang = (Resolve-Path $Clang).Path
}

$adapter = Join-Path $repoRoot "tools\dci\dci_adapter_msvc.py"
$dcib = Join-Path $repoRoot "tools\dci\dcib.py"
$header = Join-Path $bridgeRoot "cacao_dci_bridge.h"
$descriptor = Join-Path $projectRoot "dci\cacao-cpp-bridge.dcib"
$debugJson = Join-Path $projectRoot "dci\cacao-cpp-bridge.dci"
& python $adapter --out $descriptor --debug-json-out $debugJson --include $header --clang $Clang --target x86_64-pc-windows-msvc --std c++20 -- -I $bridgeRoot -D_WIN32
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& python $dcib decode $descriptor $debugJson
exit $LASTEXITCODE
