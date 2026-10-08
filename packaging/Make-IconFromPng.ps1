<#
    Builds a multi-size Windows .ico from a square PNG artwork file.
    Each size (256, 128, 64, 48, 32, 24, 16) is resampled with high-quality
    bicubic filtering and stored as a PNG entry, which Windows Vista and later
    read directly. Transparency in the source is kept.

      -Source <png>   the artwork
      -Output <ico>   the icon to write
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string]$Source,
    [Parameter(Mandatory = $true)] [string]$Output
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$sizes = @(256, 128, 64, 48, 32, 24, 16)
$art = [System.Drawing.Image]::FromFile((Resolve-Path -LiteralPath $Source))
$entries = New-Object System.Collections.Generic.List[byte[]]
try {
    foreach ($size in $sizes) {
        $bmp = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.Clear([System.Drawing.Color]::Transparent)
        $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
        $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
        $g.DrawImage($art, 0, 0, $size, $size)
        $g.Dispose()
        $ms = New-Object System.IO.MemoryStream
        $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
        $bmp.Dispose()
        $entries.Add($ms.ToArray())
        $ms.Dispose()
    }
}
finally {
    $art.Dispose()
}

$out = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter($out)
$w.Write([uint16]0)                 # reserved
$w.Write([uint16]1)                 # type: icon
$w.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $size = $sizes[$i]
    $dim = if ($size -ge 256) { 0 } else { $size }   # 0 means 256
    $w.Write([byte]$dim)
    $w.Write([byte]$dim)
    $w.Write([byte]0)               # palette colours
    $w.Write([byte]0)               # reserved
    $w.Write([uint16]1)             # colour planes
    $w.Write([uint16]32)            # bits per pixel
    $w.Write([uint32]$entries[$i].Length)
    $w.Write([uint32]$offset)
    $offset += $entries[$i].Length
}
foreach ($entry in $entries) { $w.Write($entry) }
$w.Flush()
[System.IO.File]::WriteAllBytes($Output, $out.ToArray())
$w.Dispose()
Write-Output ('Wrote {0} ({1:N0} bytes, {2} sizes)' -f $Output, (Get-Item -LiteralPath $Output).Length, $sizes.Count)
