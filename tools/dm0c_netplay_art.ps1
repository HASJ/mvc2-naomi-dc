<#
  Writes src/dc/dm0c_netplay_art.h: the art of the Dreamcast main menu's Netplay item and banner
  description (dm0c_menu.c), derived from DM0CPOL.BIN / DM0CTEX.BIN plus matching description text.

  Run with PowerShell 7 on Windows:
      pwsh -NoProfile -File tools/dm0c_netplay_art.ps1 [-DiscDir ports/build-dist/disc] [-Preview preview.png]
#>
param(
    [string]$DiscDir = '',
    [string]$Preview = ''
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$Root = Split-Path -Parent $PSScriptRoot
if (-not $DiscDir) {
    foreach ($cand in @(
        (Join-Path $Root 'ports/build-dist/disc'),
        (Join-Path $Root 'ports/dist/disc'),
        (Join-Path $Root 'disc')
    )) {
        if (Test-Path (Join-Path $cand 'DM0CPOL.BIN')) { $DiscDir = $cand; break }
    }
}
if (-not $DiscDir -or -not (Test-Path (Join-Path $DiscDir 'DM0CPOL.BIN')) -or -not (Test-Path (Join-Path $DiscDir 'DM0CTEX.BIN'))) {
    throw "Could not find DM0CPOL.BIN and DM0CTEX.BIN (checked '$DiscDir'). Pass -DiscDir <path>."
}

$Out = Join-Path $Root 'src/dc/dm0c_netplay_art.h'

$pol = [System.IO.File]::ReadAllBytes((Join-Path $DiscDir 'DM0CPOL.BIN'))
$tex = [System.IO.File]::ReadAllBytes((Join-Path $DiscDir 'DM0CTEX.BIN'))
$p0 = [BitConverter]::ToUInt32($pol, 0)
$p2 = [BitConverter]::ToUInt32($pol, 8)
$base = $p0 - 16
$thd6Off = ($p2 - $base) + 6 * 16
$thd6Ad = [BitConverter]::ToUInt32($pol, $thd6Off + 8)
$off6 = $thd6Ad - 0x0cc00000

function Spread([int]$v) {
    $r = 0
    for ($k = 0; ($v -shr $k) -gt 0; $k++) {
        $r = $r -bor ((($v -shr $k) -band 1) -shl (2 * $k))
    }
    return $r
}

$twCol = for ($x = 0; $x -lt 256; $x++) { (Spread $x) -shl 1 }
$twRow = for ($y = 0; $y -lt 256; $y++) { Spread $y }

function Get-Thd6([int]$x, [int]$uy) {
    $ty = 255 - $uy
    return [BitConverter]::ToUInt16($tex, $off6 + ($twCol[$x] + $twRow[$ty]) * 2)
}

# ---- Ring Label: NETPLAY (112 x 24), copied from NETWORK ('NET') + DISPLAY ('PLAY') -----------------
$RingW = 112
$RingH = 24
$ringRaw = New-Object uint16[] ($RingW * $RingH)

# 'NET' from NETWORK (uy=96..119, x=2..46 -> dx=2..46)
for ($dy = 0; $dy -lt $RingH; $dy++) {
    for ($dx = 2; $dx -le 46; $dx++) {
        $ringRaw[$dy * $RingW + $dx] = Get-Thd6 $dx (96 + $dy)
    }
}
# 'PLAY' from DISPLAY (uy=144..167, sx=37..97 -> dx=49..109)
for ($dy = 0; $dy -lt $RingH; $dy++) {
    for ($sx = 37; $sx -le 97; $sx++) {
        $dx = 49 + ($sx - 37)
        $ringRaw[$dy * $RingW + $dx] = Get-Thd6 $sx (144 + $dy)
    }
}
# Clean 2-column black outline seam between 'T' (dx=46) and 'P' (dx=49)
for ($dy = 0; $dy -lt $RingH; $dy++) {
    for ($dx = 47; $dx -le 48; $dx++) {
        $near1 = $false
        $near2 = $false
        for ($ny = $dy - 2; $ny -le $dy + 2; $ny++) {
            if ($ny -lt 0 -or $ny -ge $RingH) { continue }
            for ($nx = $dx - 2; $nx -le $dx + 2; $nx++) {
                if ($nx -eq 47 -or $nx -eq 48) { continue }
                $p = $ringRaw[$ny * $RingW + $nx]
                $a4 = ($p -shr 12) -band 15
                $r4 = ($p -shr 8) -band 15
                if ($r4 -ge 8 -and $a4 -ge 8) {
                    $d = [Math]::Abs($ny - $dy) + [Math]::Abs($nx - $dx)
                    if ($d -le 2) { $near1 = $true }
                    if ($d -le 3) { $near2 = $true }
                }
            }
        }
        if ($near1) { $ringRaw[$dy * $RingW + $dx] = 0xF000 }
        elseif ($near2) { $ringRaw[$dy * $RingW + $dx] = 0x9000 }
        else { $ringRaw[$dy * $RingW + $dx] = 0x0000 }
    }
}

$ringBytes = New-Object byte[] ($RingW * $RingH)
for ($i = 0; $i -lt $ringRaw.Length; $i++) {
    $p = $ringRaw[$i]
    $a4 = ($p -shr 12) -band 15
    $l4 = if ($a4 -eq 0) { 0 } else { ($p -shr 8) -band 15 }
    $ringBytes[$i] = [byte](($a4 -shl 4) -bor $l4)
}

# ---- Center Banner Description: two lines (240 x 22 each) -------------------------------------------
$DescW = 240
$DescH = 22
$Super = 4

function Render-DescLine([string]$text) {
    $bw = $DescW * $Super
    $bh = $DescH * $Super
    $b = New-Object System.Drawing.Bitmap $bw, $bh, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($b)
    $g.SmoothingMode = 'AntiAlias'
    $g.PixelOffsetMode = 'HighQuality'
    $g.Clear([System.Drawing.Color]::FromArgb(0, 255, 255, 255))

    $fam = New-Object System.Drawing.FontFamily 'Arial'
    $fmt = [System.Drawing.StringFormat]::GenericTypographic
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddString($text, $fam, [int][System.Drawing.FontStyle]::Bold, [float](15.5 * $Super), (New-Object System.Drawing.PointF ([float](3 * $Super)), ([float](1.5 * $Super))), $fmt)

    $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::White), ([float](4.2 * $Super))
    $pen.LineJoin = 'Round'
    $g.DrawPath($pen, $path)
    $g.FillPath([System.Drawing.Brushes]::White, $path)
    $g.FillPath([System.Drawing.Brushes]::Black, $path)
    $pen.Dispose(); $path.Dispose(); $fam.Dispose(); $g.Dispose()

    $sm = New-Object System.Drawing.Bitmap $DescW, $DescH, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $gs = [System.Drawing.Graphics]::FromImage($sm)
    $gs.InterpolationMode = 'HighQualityBicubic'
    $gs.PixelOffsetMode = 'HighQuality'
    $gs.DrawImage($b, 0, 0, $DescW, $DescH)
    $gs.Dispose(); $b.Dispose()

    $bytes = New-Object byte[] ($DescW * $DescH)
    for ($y = 0; $y -lt $DescH; $y++) {
        for ($x = 0; $x -lt $DescW; $x++) {
            $px = $sm.GetPixel($x, $y)
            $a4 = [int][Math]::Round($px.A / 17.0)
            if ($a4 -gt 15) { $a4 = 15 }
            if ($a4 -eq 0) {
                $l4 = 15   # 0x0F -> 0x0FFF transparent white to prevent dark bilinear halos
            } else {
                $lum = ($px.R + $px.G + $px.B) / 3.0
                $l4 = [int][Math]::Round($lum / 17.0)
                if ($l4 -gt 15) { $l4 = 15 }
            }
            $bytes[$y * $DescW + $x] = [byte](($a4 -shl 4) -bor $l4)
        }
    }
    return $bytes, $sm
}

$desc1Bytes, $desc1Bmp = Render-DescLine 'Play online rollback matches,'
$desc2Bytes, $desc2Bmp = Render-DescLine 'lobbies, rankings, and replays.'

if ($Preview) {
    $prev = New-Object System.Drawing.Bitmap 320, 180, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $pg = [System.Drawing.Graphics]::FromImage($prev)
    $pg.Clear([System.Drawing.Color]::FromArgb(255, 165, 175, 195))
    for ($y = 0; $y -lt $RingH; $y++) {
        for ($x = 0; $x -lt $RingW; $x++) {
            $b = $ringBytes[$y * $RingW + $x]
            $a = (($b -shr 4) -band 15) * 17
            $l = ($b -band 15) * 17
            $t = $x / [double]$RingW
            $tr = 255; $tg = [int](100 + 50 * $t); $tb = 0
            $r = ($l * $tr) / 255; $g = ($l * $tg) / 255; $bl = ($l * $tb) / 255
            for ($zy = 0; $zy -lt 2; $zy++) {
                for ($zx = 0; $zx -lt 2; $zx++) {
                    $px = 48 + $x * 2 + $zx
                    $py = 16 + $y * 2 + $zy
                    $bg = $prev.GetPixel($px, $py)
                    $or = [int](($r * $a + $bg.R * (255 - $a)) / 255)
                    $og = [int](($g * $a + $bg.G * (255 - $a)) / 255)
                    $ob = [int](($bl * $a + $bg.B * (255 - $a)) / 255)
                    $prev.SetPixel($px, $py, [System.Drawing.Color]::FromArgb(255, $or, $og, $ob))
                }
            }
        }
    }
    $pg.DrawImage($desc1Bmp, 40, 96)
    $pg.DrawImage($desc2Bmp, 40, 124)
    $pg.Dispose()
    $prevDir = Split-Path -Parent $Preview
    if ($prevDir) { [void](New-Item -ItemType Directory -Force -Path $prevDir) }
    $prev.Save($Preview)
    $prev.Dispose()
}
$desc1Bmp.Dispose()
$desc2Bmp.Dispose()

# ---- Write src/dc/dm0c_netplay_art.h --------------------------------------------------------------
$sb = New-Object System.Text.StringBuilder
function L([string]$s) { [void]$sb.Append($s + "`n") }
function Write-ByteArray([byte[]]$arr, [int]$per = 24) {
    for ($i = 0; $i -lt $arr.Length; $i += $per) {
        $end = [Math]::Min($i + $per, $arr.Length) - 1
        L ("`t" + (($arr[$i..$end] | ForEach-Object { '0x{0:x2},' -f $_ }) -join ' '))
    }
}

L '/* The art of the Dreamcast main menu''s Netplay item and banner description (dm0c_menu.c).'
L '   Written by tools/dm0c_netplay_art.ps1: change that script and run it again rather than editing this file. */'
L '#ifndef DM0C_NETPLAY_ART_H'
L '#define DM0C_NETPLAY_ART_H'
L ''
L "#define DM0C_NP_RING_W`t$RingW"
L "#define DM0C_NP_RING_H`t$RingH"
L "#define DM0C_NP_DESC_W`t$DescW"
L "#define DM0C_NP_DESC_H`t$DescH"
L ''
L '/* 1 byte per texel: (a4 << 4) | l4 (0x00 transparent black outside outline) */'
L "static const Uint8 dm0c_netplay_ring[DM0C_NP_RING_W * DM0C_NP_RING_H] = {"
Write-ByteArray $ringBytes
L '} ;'
L ''
L '/* 1 byte per texel: (a4 << 4) | l4 (0x0f transparent white outside outline) */'
L "static const Uint8 dm0c_netplay_desc1[DM0C_NP_DESC_W * DM0C_NP_DESC_H] = {"
Write-ByteArray $desc1Bytes
L '} ;'
L ''
L "static const Uint8 dm0c_netplay_desc2[DM0C_NP_DESC_W * DM0C_NP_DESC_H] = {"
Write-ByteArray $desc2Bytes
L '} ;'
L ''
L '#endif'
[System.IO.File]::WriteAllText($Out, $sb.ToString().Replace("`r`n", "`n"))
"wrote src/dc/dm0c_netplay_art.h (ring ${RingW}x${RingH}, desc ${DescW}x${DescH} x2)"
