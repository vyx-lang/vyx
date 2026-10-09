[CmdletBinding(PositionalBinding=$false)]
param(
    [Parameter(Position=0)][ValidateSet('setup','update','rollback','uninstall')][string]$Action = 'setup',
    [string]$BaseUrl = 'https://www.vyxlang.com',
    [string]$InstallDir,
    [string]$ConfigDir,
    [switch]$Yes,
    [switch]$NoPersistPath,
    [switch]$FromLauncher
)

$ErrorActionPreference = 'Stop'
$scriptSource = $MyInvocation.MyCommand.ScriptBlock.ToString()
& {
    param($Action, $BaseUrl, $InstallDir, $ConfigDir, $Yes, $NoPersistPath, $FromLauncher, $Source, $ScriptPath)
    $ErrorActionPreference = 'Stop'
    $ProgressPreference = 'SilentlyContinue'
    if ([Environment]::OSVersion.Platform -ne 'Win32NT') { throw 'This installer requires Windows.' }
    $architecture = if ($env:PROCESSOR_ARCHITEW6432) { $env:PROCESSOR_ARCHITEW6432 } else { $env:PROCESSOR_ARCHITECTURE }
    if ($architecture -ne 'AMD64') { throw "The Windows SDK requires x64; found $architecture." }
    function Read-Json($path) {
        if (Test-Path -LiteralPath $path -PathType Leaf) { return Get-Content -LiteralPath $path -Raw -Encoding UTF8 | ConvertFrom-Json }
        return $null
    }
    function Write-Atomic($path, $value) {
        if ((Test-Path -LiteralPath $path) -and [IO.File]::ReadAllText($path) -ceq $value) { return }
        $temp = $path + '.' + [Guid]::NewGuid().ToString('N')
        $backup = $temp + '.previous'
        try {
            [IO.File]::WriteAllText($temp, $value, (New-Object Text.UTF8Encoding($false)))
            if (Test-Path -LiteralPath $path) { [IO.File]::Replace($temp, $path, $backup) } else { [IO.File]::Move($temp, $path) }
        } finally { foreach ($file in @($temp, $backup)) { if (Test-Path -LiteralPath $file) { [IO.File]::Delete($file) } } }
    }
    function Require-Child($path, $root) {
        $absolute = [IO.Path]::GetFullPath($path)
        $prefix = [IO.Path]::GetFullPath($root).TrimEnd('\') + '\'
        if (!$absolute.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw "Path is outside the managed directory: $absolute" }
        return $absolute
    }
    # Delete links themselves, never their targets. Only called for verified managed SDKs and our staging directory.
    function Remove-ManagedTree($path, $root) {
        $path = Require-Child $path $root
        if (!(Test-Path -LiteralPath $path)) { return }
        $item = Get-Item -LiteralPath $path -Force
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            if ($item.PSIsContainer) { [IO.Directory]::Delete($path) } else { [IO.File]::Delete($path) }
        } elseif ($item.PSIsContainer) {
            foreach ($child in Get-ChildItem -LiteralPath $path -Force) { Remove-ManagedTree $child.FullName $root }
            [IO.Directory]::Delete($path)
        } else { $item.IsReadOnly = $false; [IO.File]::Delete($path) }
    }
    $explicitDir = !!$InstallDir
    if (!$InstallDir -and $ScriptPath -and [IO.Path]::GetFileName($ScriptPath) -eq 'vyxup.ps1') {
        $InstallDir = [IO.Path]::GetDirectoryName($ScriptPath)
        $stamp = Read-Json (Join-Path $InstallDir '.vyx-setup.json')
        if (!$ConfigDir -and $stamp) { $ConfigDir = $stamp.config }
    }
    if (!$ConfigDir) { $ConfigDir = Join-Path $env:APPDATA 'Vyx' }
    $ConfigDir = [IO.Path]::GetFullPath($ConfigDir)
    $pointerFile = Join-Path $ConfigDir 'install.json'
    $pointer = Read-Json $pointerFile
    if ($pointer -and $pointer.manager -ne 'vyx-sdk-setup') { throw "Unmanaged configuration file: $pointerFile" }
    if (!$InstallDir -and $pointer) { $InstallDir = $pointer.root }
    # Adopt installations created by the first installer, including a custom root already on PATH.
    if (!$InstallDir) {
        $command = Get-Command vyxc.exe -ErrorAction SilentlyContinue
        if ($command) {
            $candidate = $command.Source
            for ($i = 0; $i -lt 4; $i++) { $candidate = [IO.Path]::GetDirectoryName($candidate) }
            $legacy = Read-Json (Join-Path $candidate 'install.json')
            if ($legacy -and $legacy.sha256 -match '^[a-f0-9]{64}$' -and $legacy.bin -eq [IO.Path]::GetDirectoryName($command.Source)) { $InstallDir = $candidate }
        }
    }
    if (!$InstallDir) {
        if ($Action -ne 'setup') { Write-Host 'No managed Vyx SDK is installed.'; return }
        $InstallDir = Join-Path $env:LOCALAPPDATA 'Vyx'
    }
    $InstallDir = [IO.Path]::GetFullPath($InstallDir).TrimEnd('\')
    if ($InstallDir.Contains(';') -or $InstallDir -match '[\r\n]' -or $InstallDir -eq [IO.Path]::GetPathRoot($InstallDir).TrimEnd('\')) { throw 'Choose a directory below a drive root, without semicolons or newlines.' }
    if ($Action -eq 'setup' -and !$explicitDir -and !$Yes) {
        Write-Host "Vyx SDK setup`nDirectory: $InstallDir`n  1) Install / update here (default)`n  2) Choose a directory`n  3) Cancel"
        $choice = Read-Host 'Choose [1]'
        switch ($choice) {
            { $_ -eq '' -or $_ -eq '1' } { }
            '2' { $chosen = Read-Host 'Installation directory'; if (!$chosen.Trim()) { throw 'An installation directory is required.' }; $InstallDir = [IO.Path]::GetFullPath($chosen).TrimEnd('\') }
            '3' { Write-Host 'Cancelled.'; return }
            default { throw 'Choose 1, 2 or 3. Use -Yes for unattended installation.' }
        }
    }
    if ($InstallDir.Contains(';') -or $InstallDir -match '[\r\n]' -or $InstallDir -eq [IO.Path]::GetPathRoot($InstallDir).TrimEnd('\')) { throw 'Invalid installation directory.' }
    $releaseRoot = Join-Path $InstallDir 'sdk'
    $stateFile = Join-Path $InstallDir 'install.json'
    $stampFile = Join-Path $InstallDir '.vyx-setup.json'
    $current = Join-Path $InstallDir 'current'
    $bin = Join-Path $current 'bin'
    $managerBin = Join-Path $InstallDir 'bin'
    $state = Read-Json $stateFile
    $stamp = Read-Json $stampFile
    if ($stamp -and ($stamp.manager -ne 'vyx-sdk-setup' -or $stamp.root -ne $InstallDir)) { throw 'The installation directory is unmanaged.' }
    if ($state -and (($state.manager -and $state.manager -ne 'vyx-sdk-setup') -or $state.sha256 -notmatch '^[a-f0-9]{64}$')) { throw 'Invalid SDK state.' }
    function Require-Release($hash) {
        if ($hash -notmatch '^[a-f0-9]{64}$') { throw 'Invalid SDK revision.' }
        $path = Require-Child (Join-Path $releaseRoot $hash) $InstallDir
        $item = Get-Item -LiteralPath $path -Force
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "SDK directory must not be a link: $path" }
        if (!(Test-Path -LiteralPath (Join-Path $path 'SDK-MANIFEST.json')) -or !(Test-Path -LiteralPath (Join-Path $path 'bin/vyxc.exe')) -or (Get-Content -LiteralPath (Join-Path $path '.vyx-archive.sha256') -Raw).Trim() -ne $hash) { throw "Incomplete or unmanaged SDK: $path" }
        return $path
    }
    foreach ($dir in @($InstallDir, $releaseRoot, $managerBin)) {
        if ((Test-Path -LiteralPath $dir) -and ((Get-Item -LiteralPath $dir -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Managed directories must not be links: $dir" }
    }
    if ($state) { $null = Require-Release $state.sha256 }
    if (Test-Path -LiteralPath $current) {
        $link = Get-Item -LiteralPath $current -Force
        if (!$state -or $link.LinkType -ne 'Junction' -or @($link.Target).Count -ne 1 -or [IO.Path]::GetFullPath($link.Target[0]) -ne (Join-Path $releaseRoot $state.sha256)) { throw 'The current SDK junction is unmanaged.' }
    }
    $removeBins = @($bin, $managerBin)
    if ($state -and $state.bin) { $removeBins += $state.bin }
    if ($Action -ne 'uninstall' -and $pointer -and $pointer.root -ne $InstallDir) {
        $removeBins += (Join-Path $pointer.root 'current/bin'), (Join-Path $pointer.root 'bin')
    }
    function Without-VyxPath([string]$path) { return (@($path -split ';' | Where-Object { $_ -and $removeBins -notcontains $_.TrimEnd('\') }) -join ';') }
    function With-VyxPath([string]$path) { $rest = Without-VyxPath $path; return (@($bin, $managerBin) + @($rest -split ';' | Where-Object { $_ })) -join ';' }
    if ($Action -eq 'uninstall') {
        if (!$stamp -or !$state) { throw 'Uninstall requires a directory managed by Vyx setup. Run setup once to adopt an older installation.' }
        if (!$Yes -and (Read-Host "Remove managed SDKs from $InstallDir ? [y/N]") -notmatch '^(y|yes)$') { Write-Host 'Cancelled.'; return }
        $releases = @(Get-ChildItem -LiteralPath $releaseRoot -Directory -Force | Where-Object { $_.Name -match '^[a-f0-9]{64}$' } | ForEach-Object { Require-Release $_.Name })
        if (!$NoPersistPath) { [Environment]::SetEnvironmentVariable('Path', (Without-VyxPath ([Environment]::GetEnvironmentVariable('Path','User'))), 'User') }
        $env:Path = Without-VyxPath $env:Path
        if (Test-Path -LiteralPath $current) { [IO.Directory]::Delete($current) }
        foreach ($release in $releases) { Remove-ManagedTree $release $InstallDir }
        foreach ($file in @($stateFile, $stampFile, (Join-Path $InstallDir 'vyxup.ps1'))) { if (Test-Path -LiteralPath $file) { [IO.File]::Delete($file) } }
        $launcher = Require-Child (Join-Path $managerBin 'vyxup.cmd') $InstallDir
        if ($FromLauncher) {
            # CMD needs its file until it returns. A hidden helper waits for that specific CMD process.
            $parentId = (Get-CimInstance Win32_Process -Filter "ProcessId=$PID").ParentProcessId
            $parentProcess = Get-Process -Id $parentId -ErrorAction Stop
            if ($parentProcess.ProcessName -ne 'cmd') { throw 'The deferred cleanup caller is not the Vyx CMD launcher.' }
            $launcherLiteral = $launcher.Replace("'", "''")
            $rootLiteral = $InstallDir.Replace("'", "''")
            $binLiteral = $managerBin.Replace("'", "''")
            $cleanup = "Wait-Process -Id $parentId -ErrorAction SilentlyContinue; " +
                "[IO.File]::Delete('$launcherLiteral'); " +
                "foreach (`$dir in @('$binLiteral', '$rootLiteral')) { if ((Test-Path -LiteralPath `$dir) -and !(Get-ChildItem -LiteralPath `$dir -Force)) { [IO.Directory]::Delete(`$dir) } }"
            $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($cleanup))
            Start-Process -FilePath (Join-Path $PSHOME 'powershell.exe') -ArgumentList @('-NoProfile','-EncodedCommand',$encoded) -WindowStyle Hidden | Out-Null
        } elseif (Test-Path -LiteralPath $launcher) { [IO.File]::Delete($launcher) }
        if ($pointer -and $pointer.root -eq $InstallDir) { [IO.File]::Delete($pointerFile) }
        foreach ($dir in @($releaseRoot, $managerBin, $InstallDir, $ConfigDir)) { if ((Test-Path -LiteralPath $dir) -and !(Get-ChildItem -LiteralPath $dir -Force)) { [IO.Directory]::Delete($dir) } }
        Write-Host 'Vyx uninstalled. Project files, unrelated files and other tools were kept. Open a new terminal to refresh its inherited PATH.'
        return
    }
    if ($Action -eq 'rollback' -and (!$state -or !$state.previous)) { throw 'No previous SDK is available for rollback.' }
    if ($Action -eq 'update' -and !$state) { throw 'No managed SDK found. Run setup first.' }
    $BaseUrl = $BaseUrl.TrimEnd('/')
    $uri = [Uri]$BaseUrl
    if (!$uri.IsAbsoluteUri -or ($uri.Scheme -ne 'https' -and !($uri.Scheme -eq 'http' -and $uri.IsLoopback))) { throw 'Use an HTTPS download URL (HTTP is supported for localhost fixtures).' }
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    New-Item -ItemType Directory -Path $releaseRoot, $managerBin, $ConfigDir -Force | Out-Null
    foreach ($dir in @($InstallDir, $releaseRoot, $managerBin)) { if ((Get-Item -LiteralPath $dir -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Managed directories must not be links: $dir" } }
    $stage = Require-Child (Join-Path $InstallDir ('.staging-' + [Guid]::NewGuid().ToString('N'))) $InstallDir
    New-Item -ItemType Directory -Path $stage | Out-Null
    $oldProcessPath = $env:Path
    $oldUserPath = [Environment]::GetEnvironmentVariable('Path','User')
    $snapshots = @{}
    foreach ($file in @($stateFile, $pointerFile, $stampFile, (Join-Path $InstallDir 'vyxup.ps1'), (Join-Path $managerBin 'vyxup.cmd'))) { $snapshots[$file] = if (Test-Path -LiteralPath $file) { [IO.File]::ReadAllText($file) } else { $null } }
    $selected = $false
    try {
        if ($Action -eq 'rollback') { $expected = $state.previous; $release = Require-Release $expected } else {
            $archiveName = 'vyx-sdk-windows-x86_64-llvm22.zip'
            Write-Host 'Checking the Windows x64 SDK...'
            $checksum = (Invoke-RestMethod -Uri "$BaseUrl/sdk/$archiveName.sha256").ToString().Trim()
            if ($checksum -notmatch ('^([a-fA-F0-9]{64})\s+\*?' + [regex]::Escape($archiveName) + '$')) { throw 'Invalid SDK checksum file.' }
            $expected = $Matches[1].ToLowerInvariant()
            $release = Join-Path $releaseRoot $expected
            if (Test-Path -LiteralPath $release) { $release = Require-Release $expected; Write-Host 'This SDK is already installed.' } else {
                $archive = Join-Path $stage $archiveName
                Write-Host 'Downloading the SDK...'
                Invoke-WebRequest -Uri "$BaseUrl/sdk/$archiveName" -OutFile $archive -UseBasicParsing
                if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) { throw 'SDK checksum mismatch; installation stopped.' }
                Add-Type -AssemblyName System.IO.Compression.FileSystem
                [IO.Compression.ZipFile]::ExtractToDirectory($archive, (Join-Path $stage 'unpacked'))
                $payload = Join-Path $stage 'unpacked/vyx-sdk-windows-x86_64-llvm22'
                if (!(Test-Path -LiteralPath (Join-Path $payload 'bin/vyxc.exe')) -or !(Test-Path -LiteralPath (Join-Path $payload 'SDK-MANIFEST.json'))) { throw 'The archive does not contain a complete Windows SDK.' }
                & (Join-Path $payload 'bin/vyxc.exe') --version
                if ($LASTEXITCODE -ne 0) { throw "SDK startup failed (exit $LASTEXITCODE)." }
                Set-Content -LiteralPath (Join-Path $payload '.vyx-archive.sha256') -Value $expected -Encoding ASCII
                $null = Require-Child $payload $InstallDir
                $null = Require-Child $release $InstallDir
                Move-Item -LiteralPath $payload -Destination $release
            }
        }
        & (Join-Path $release 'bin/vyxc.exe') --version
        if ($LASTEXITCODE -ne 0) { throw "SDK startup failed (exit $LASTEXITCODE)." }
        $previous = if ($state -and $state.sha256 -ne $expected) { $state.sha256 } elseif ($state) { $state.previous } else { $null }
        $next = Join-Path $stage 'next'
        New-Item -ItemType Junction -Path $next -Target $release | Out-Null
        $backup = Join-Path $stage 'previous'
        if (Test-Path -LiteralPath $current) { $null = Require-Child $current $InstallDir; [IO.Directory]::Move($current, $backup) }
        try { [IO.Directory]::Move($next, $current); $selected = $true } catch { if (Test-Path -LiteralPath $backup) { [IO.Directory]::Move($backup, $current) }; throw }
        if (!$FromLauncher) {
            Write-Atomic (Join-Path $InstallDir 'vyxup.ps1') $Source
            Write-Atomic (Join-Path $managerBin 'vyxup.cmd') "@echo off`r`npowershell.exe -NoProfile -ExecutionPolicy Bypass -File `"%~dp0..\vyxup.ps1`" -FromLauncher %*`r`nexit /b %errorlevel%`r`n"
        }
        Write-Atomic $stateFile (@{ manager='vyx-sdk-setup'; schema=2; root=$InstallDir; bin=$bin; sha256=$expected; previous=$previous; source=$BaseUrl } | ConvertTo-Json)
        Write-Atomic $stampFile (@{ manager='vyx-sdk-setup'; root=$InstallDir; config=$ConfigDir } | ConvertTo-Json)
        Write-Atomic $pointerFile (@{ manager='vyx-sdk-setup'; root=$InstallDir } | ConvertTo-Json)
        if (!$NoPersistPath) { [Environment]::SetEnvironmentVariable('Path', (With-VyxPath $oldUserPath), 'User') }
        $env:Path = With-VyxPath $oldProcessPath
        Write-Host "SDK directory: $InstallDir`nReady: vyxc --version`nManage: vyxup update / vyxup rollback / vyxup uninstall"
    } catch {
        if ($selected) {
            [IO.Directory]::Delete($current)
            if (Test-Path -LiteralPath (Join-Path $stage 'previous')) { [IO.Directory]::Move((Join-Path $stage 'previous'), $current) }
            foreach ($file in $snapshots.Keys) { if ($null -eq $snapshots[$file]) { if (Test-Path -LiteralPath $file) { [IO.File]::Delete($file) } } else { Write-Atomic $file $snapshots[$file] } }
            if (!$NoPersistPath) { [Environment]::SetEnvironmentVariable('Path',$oldUserPath,'User') }
        }
        $env:Path = $oldProcessPath
        throw
    } finally { Remove-ManagedTree $stage $InstallDir }
} $Action $BaseUrl $InstallDir $ConfigDir $Yes $NoPersistPath $FromLauncher $scriptSource $PSCommandPath
