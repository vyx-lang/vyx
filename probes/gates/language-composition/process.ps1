# One persistent JSON-lines bridge: Job-capped compiler/runtime launches without
# rebuilding the Windows native process helper for every generated batch.
param([Parameter(Mandatory)][string]$Repo)
$ErrorActionPreference = 'Stop'
. (Join-Path $Repo 'bootstrap_compiler/scripts/VyxTestProcess.ps1')
while ($null -ne ($line = [Console]::ReadLine())) {
    try {
        $request = $line | ConvertFrom-Json
        foreach ($property in $request.environment.PSObject.Properties) {
            [Environment]::SetEnvironmentVariable($property.Name, $property.Value, 'Process')
        }
        $result = Invoke-VyxProcess -FilePath $request.exe -ArgumentList @($request.arguments) `
            -WorkingDirectory $request.cwd -TimeoutSec $request.timeout -MemoryLimitMB $request.memory_mb `
            -StdoutLog $request.stdout -StderrLog $request.stderr -DialogLog $request.dialog
        [Console]::WriteLine(($result | ConvertTo-Json -Depth 8 -Compress))
    } catch {
        [Console]::WriteLine((@{ BridgeError = $_.Exception.Message } | ConvertTo-Json -Compress))
    }
}
