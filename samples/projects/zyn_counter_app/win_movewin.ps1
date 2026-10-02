param(
  [int]$X = 60,
  [int]$Y = 60
)
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class MW {
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int ht, bool repaint);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    public struct RECT { public int L, T, R, B; }
}
"@
[MW]::SetProcessDPIAware() | Out-Null
$p = Get-Process zyn_counter_app -ErrorAction SilentlyContinue | Select-Object -First 1
if ($p -eq $null) { Write-Output "not-running"; exit 1 }
$h = $p.MainWindowHandle
$r = New-Object MW+RECT
[MW]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.R - $r.L; $ht = $r.B - $r.T
[MW]::MoveWindow($h, $X, $Y, $w, $ht, $true) | Out-Null
Start-Sleep -Milliseconds 500
$r2 = New-Object MW+RECT
[MW]::GetWindowRect($h, [ref]$r2) | Out-Null
Write-Output ("moved to L=" + $r2.L + " T=" + $r2.T + " size=" + ($r2.R - $r2.L) + "x" + ($r2.B - $r2.T))
