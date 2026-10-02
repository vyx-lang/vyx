param([string]$Path = "accept_0.png", [int]$Thresh = 250, [int]$ScanY0 = 60)
# Measure the corner radius of the light panel: for each row of the panel we
# record the leftmost "panel-white" pixel, then check how many rows are inset
# from the widest row.  A square panel insets 0 rows; a rounded one insets
# roughly `radius` rows.
# ScanY0 skips the (near-white) title bar, which would otherwise be mistaken
# for panel pixels.
Add-Type -AssemblyName System.Drawing
$bmp = New-Object System.Drawing.Bitmap((Resolve-Path $Path).Path)
$w = $bmp.Width; $h = $bmp.Height
$minX = $w; $maxX = -1; $minY = $h; $maxY = -1
for ($y = $ScanY0; $y -lt $h; $y++) {
  for ($x = 0; $x -lt $w; $x++) {
    $c = $bmp.GetPixel($x, $y)
    if ($c.R -ge $Thresh -and $c.G -ge $Thresh -and $c.B -ge 235) {
      if ($x -lt $minX) { $minX = $x }
      if ($x -gt $maxX) { $maxX = $x }
      if ($y -lt $minY) { $minY = $y }
      if ($y -gt $maxY) { $maxY = $y }
    }
  }
}
Write-Output ("panel bbox x=" + $minX + ".." + $maxX + " y=" + $minY + ".." + $maxY + " (" + ($maxX - $minX + 1) + "x" + ($maxY - $minY + 1) + ")")
function LeftmostInRow([int]$y) {
  for ($x = $minX; $x -le $maxX; $x++) {
    $c = $bmp.GetPixel($x, $y)
    if ($c.R -ge $Thresh -and $c.G -ge $Thresh -and $c.B -ge 235) { return $x }
  }
  return -1
}
# Print the top-edge profile: a rounded rectangle insets row 0 by ~radius and
# returns to 0 after ~radius rows; a square one is 0 from the first row.
$profile = @()
$maxInset = 0
for ($dy = 0; $dy -lt 14; $dy++) {
  $lx = LeftmostInRow ($minY + $dy)
  $inset = if ($lx -lt 0) { -1 } else { $lx - $minX }
  $profile += $inset
  if ($inset -gt $maxInset) { $maxInset = $inset }
}
Write-Output ("top-edge inset profile (dy=0..13): " + ($profile -join " "))
$insetRows = ($profile | Where-Object { $_ -gt 0 }).Count
Write-Output ("rows with inset>0 = " + $insetRows + "  max inset = " + $maxInset)
if ($maxInset -ge 4 -and $insetRows -ge 4) {
  Write-Output "VERDICT: rounded"
} else {
  Write-Output "VERDICT: square"
}
$bmp.Dispose()
