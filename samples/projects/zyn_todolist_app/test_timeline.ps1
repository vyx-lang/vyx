param(
    [switch]$SkipBuild,
    [switch]$Visual
)

$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
$sdkRoot = Join-Path $projectRoot '../../../Zyn'
$zyn = Join-Path $sdkRoot 'bin/zyn.ps1'
$app = Join-Path $projectRoot 'target/zyn_todolist_app.exe'
$timeline = Join-Path $projectRoot 'target/todo_self_test.zyns'

Push-Location -LiteralPath $projectRoot
try {
    if (-not $SkipBuild) {
        Push-Location -LiteralPath $sdkRoot
        try {
            & $zyn build
            if ($LASTEXITCODE -ne 0) { throw "Zyn framework build failed with exit code $LASTEXITCODE" }
        }
        finally {
            Pop-Location
        }
        & $zyn build
        if ($LASTEXITCODE -ne 0) { throw "Todo app build failed with exit code $LASTEXITCODE" }
    }
    if (-not (Test-Path -LiteralPath $app)) { throw "App executable not found: $app" }

    & $app --self-test
    if ($LASTEXITCODE -ne 0) { throw "Todo self-test failed with exit code $LASTEXITCODE" }
    if (-not (Test-Path -LiteralPath $timeline)) { throw "Timeline was not exported: $timeline" }

    & $app --reply 'target/todo_self_test.zyns' --headless --repeated 3
    if ($LASTEXITCODE -ne 0) { throw "Headless reply failed with exit code $LASTEXITCODE" }

    if ($Visual) {
        & $app --reply 'target/todo_self_test.zyns' --repeated 120
        if ($LASTEXITCODE -ne 0) { throw "Visual reply failed with exit code $LASTEXITCODE" }
    }
    Write-Host 'Todo recording/reply test: OK'
}
finally {
    Pop-Location
}
