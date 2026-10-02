param(
    [Parameter(Mandatory)][string]$FixtureDir,
    [Parameter(Mandatory)][ValidateSet('LeafBody', 'PublicInterface')][string]$Kind
)

$ErrorActionPreference = 'Stop'
$fixture = (Resolve-Path -LiteralPath $FixtureDir).Path
$runsRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '.runs'))
if (-not $fixture.StartsWith($runsRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Only generated compiler_compare/.runs fixtures may be modified.'
}
$statePath = Join-Path $fixture 'fixture.json'
$state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
if ($Kind -in $state.applied_changes) { throw "$Kind already applied." }
$paths = @()
if ($Kind -eq 'LeafBody') {
    foreach ($relative in @('vyx/src/unit0000.vyx', 'cpp/src/unit0000.cpp')) {
        $path = Join-Path $fixture $relative
        $before = [IO.File]::ReadAllText($path)
        if (-not $before.Contains('value * 33 + bias')) { throw "Expected original recurrence absent: $path" }
        $after = $before.Replace('value * 33 + bias', 'value * 32 + value + bias')
        [IO.File]::WriteAllText($path, $after, [Text.UTF8Encoding]::new($false))
        $paths += $relative
    }
} else {
    $additions = [ordered]@{
        'vyx/src/shared.vyx' = "`npublic fn compare_api_revision() -> i64 {`n    return 2;`n}`n"
        'cpp/include/shared.hpp' = "`ni64 compare_api_revision();`n"
        'cpp/src/shared.cpp' = "`ni64 compare_api_revision() {`n    return 2;`n}`n"
    }
    foreach ($relative in $additions.Keys) {
        [IO.File]::AppendAllText((Join-Path $fixture $relative), $additions[$relative], [Text.UTF8Encoding]::new($false))
        $paths += $relative
    }
}
$state.applied_changes = @($state.applied_changes) + $Kind
$state | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $statePath -Encoding utf8NoBOM
[ordered]@{kind=$Kind; utc=[DateTime]::UtcNow.ToString('o'); modified_sources=@($paths | ForEach-Object {
    [ordered]@{path=$_; sha256=(Get-FileHash (Join-Path $fixture $_) -Algorithm SHA256).Hash}
})} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $fixture "change-$Kind.json") -Encoding utf8NoBOM
