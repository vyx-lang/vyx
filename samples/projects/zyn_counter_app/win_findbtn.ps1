param(
  [string]$Path = "dpi0.png"
)
Add-Type -AssemblyName System.Drawing
$bmp = New-Object System.Drawing.Bitmap((Resolve-Path $Path).Path)
$w = $bmp.Width; $h = $bmp.Height
Write-Output "image ${w}x${h}"

# Scan for the saturated "filled" button: strong purple, mid brightness.
$cols = @{}
$rows = @{}
$minX = $w; $maxX = -1; $minY = $h; $maxY = -1
for ($y = 0; $y -lt $h; $y++) {
  for ($x = 0; $x -lt $w; $x++) {
    $c = $bmp.GetPixel($x, $y)
    $r = [int]$c.R; $g = [int]$c.G; $b = [int]$c.B
    # purple: blue dominates green, red between; not near-white
    if (($b - $g) -gt 45 -and ($r - $g) -gt 15 -and $b -gt 120 -and $b -lt 210 -and $g -lt 140) {
      if ($x -lt $minX) { $minX = $x }
      if ($x -gt $maxX) { $maxX = $x }
      if ($y -lt $minY) { $minY = $y }
      if ($y -gt $maxY) { $maxY = $y }
      if ($cols.ContainsKey($x)) { $cols[$x]++ } else { $cols[$x] = 1 }
      if ($rows.ContainsKey($y)) { $rows[$y]++ } else { $rows[$y] = 1 }
    }
  }
}
if ($maxX -lt 0) { Write-Output "no-match"; exit 1 }
Write-Output ("bbox x=" + $minX + ".." + $maxX + " y=" + $minY + ".." + $maxY)
Write-Output ("center=" + [int](($minX + $maxX) / 2) + "," + [int](($minY + $maxY) / 2))
$cx = [int](($minX + $maxX) / 2)
$cy = [int](($minY + $maxY) / 2)
$c = $bmp.GetPixel($cx, $cy)
Write-Output ("centerPixel=" + $c.R + "," + $c.G + "," + $c.B)
# column histogram summary: print distinct horizontal runs
$xs = $cols.Keys | Sort-Object
Write-Output ("firstX=" + $xs[0] + " lastX=" + ($xs[-1]) + " distinctCols=" + $xs.Count)
$bmp.Dispose()
