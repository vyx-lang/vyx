Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class WC {
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    public struct RECT { public int L, T, R, B; }
}
"@
# Become DPI-aware first, otherwise GetWindowRect / PrintWindow report
# virtualized (scaled-down) coordinates and every click misses the target.
[WC]::SetProcessDPIAware() | Out-Null
$p = Get-Process zyn_counter_app -ErrorAction SilentlyContinue | Select-Object -First 1
if ($p -eq $null) { Write-Output "not-running"; exit 1 }
$h = $p.MainWindowHandle
if ($h -eq [IntPtr]::Zero) { Write-Output "no-window"; exit 1 }
$r = New-Object WC+RECT
[WC]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.R - $r.L; $ht = $r.B - $r.T
Write-Output ("rect w=" + $w + " h=" + $ht)
if ($w -le 0 -or $ht -le 0) { exit 1 }
$bmp = New-Object System.Drawing.Bitmap($w, $ht)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$dc = $g.GetHdc()
[WC]::PrintWindow($h, $dc, 2) | Out-Null
$g.ReleaseHdc($dc)
$bmp.Save("$PWD\window_capture.png", [System.Drawing.Imaging.ImageFormat]::Png)
Write-Output "captured"
