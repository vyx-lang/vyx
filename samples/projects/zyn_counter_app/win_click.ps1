param(
  [int]$X = 0,
  [int]$Y = 0,
  [int]$Repeat = 1,
  [int]$X2 = -1,
  [int]$Y2 = -1,
  [int]$ActivateX = 700,
  [int]$ActivateY = 15
)
# X,Y: window-relative image coordinates; Repeat: number of clicks in place.
# X2,Y2: optional second click target after an activation click on the title bar.
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class MZ {
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    public struct RECT { public int L, T, R, B; }
}
"@
# Must be DPI-aware before GetWindowRect/SetCursorPos, otherwise the
# virtualized coordinates do not match the physical cursor positions and
# every synthetic click lands in the wrong place on scaled displays.
[MZ]::SetProcessDPIAware() | Out-Null
$p = Get-Process zyn_counter_app -ErrorAction SilentlyContinue | Select-Object -First 1
if ($p -eq $null) { Write-Output "not-running"; exit 1 }
$h = $p.MainWindowHandle
if ($h -eq [IntPtr]::Zero) { Write-Output "no-window"; exit 1 }
$r = New-Object MZ+RECT
[MZ]::GetWindowRect($h, [ref]$r) | Out-Null
Write-Output ("window L=" + $r.L + " T=" + $r.T + " R=" + $r.R + " B=" + $r.B)
[MZ]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 500
function Invoke-Click([int]$px, [int]$py) {
  $sx = $r.L + $px
  $sy = $r.T + $py
  [MZ]::SetCursorPos($sx, $sy) | Out-Null
  Start-Sleep -Milliseconds 220
  [MZ]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
  Start-Sleep -Milliseconds 90
  [MZ]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
  Start-Sleep -Milliseconds 320
}
# A real user first activates the window; SDL only reports pointer buttons for
# a focused window, so a bare client-area click can be swallowed.
Invoke-Click $ActivateX $ActivateY
Start-Sleep -Milliseconds 300
for ($rep = 0; $rep -lt $Repeat; $rep++) {
  Invoke-Click $X $Y
  if ($X2 -ge 0) { Invoke-Click $X2 $Y2 }
}
Write-Output "clicked"
