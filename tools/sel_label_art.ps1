<#
  Writes src/sel_label_art.c: the art of the select screen's assist pillar labels.

  - A font of the printable ASCII characters (Arial Bold Italic), 4-bit coverage, for the move names.
  - The 13 assist type symbols: a hexagon badge with a glyph, 64 x 64, each texel one byte, coverage in the
    high nibble and a shade in the low one. The game tints the shades with the type's colour (sel_label.c):
    0 black, 8 the colour, 15 white.

  Run with PowerShell 7 on Windows (it draws with System.Drawing):
      pwsh -NoProfile -File tools/sel_label_art.ps1 [-Preview preview.png]
#>
param([string]$Preview = '')

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$Root = Split-Path -Parent $PSScriptRoot
$Out = Join-Path $Root 'src/sel_label_art.c'

$IconSize = 64
$Super = 4                                  # drawn this many times larger, then shrunk
$S = $IconSize * $Super

# the order of sel_glow_step_tbl's labels (sel_light.c)
$Types = 'Projectile', 'Ground', 'Expansion', 'Dash', 'Anti-air', 'Variety', 'Throw', 'Air Throw',
         'Heal', 'Enhance', 'Capture', 'Launcher', 'Balance'

function New-Canvas([int]$w, [int]$h) {
    $b = New-Object System.Drawing.Bitmap $w, $h, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($b)
    $g.SmoothingMode = 'AntiAlias'
    $g.PixelOffsetMode = 'HighQuality'
    $g.Clear([System.Drawing.Color]::Transparent)
    return $b, $g
}

function P([double]$x, [double]$y) { New-Object System.Drawing.PointF ([float]($x * $Super)), ([float]($y * $Super)) }

function Grey([int]$l, [int]$a = 255) { [System.Drawing.Color]::FromArgb($a, $l * 17, $l * 17, $l * 17) }

# a closed path of points in 64ths
function Poly([double[]]$xy) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $pts = for ($i = 0; $i -lt $xy.Count; $i += 2) { P $xy[$i] $xy[$i + 1] }
    $p.AddPolygon([System.Drawing.PointF[]]$pts)
    return $p
}

function Circle([double]$cx, [double]$cy, [double]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $p.AddEllipse([float](($cx - $r) * $Super), [float](($cy - $r) * $Super), [float](2 * $r * $Super), [float](2 * $r * $Super))
    return $p
}

function Rect([double]$x, [double]$y, [double]$w, [double]$h) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $p.AddRectangle((New-Object System.Drawing.RectangleF ([float]($x * $Super)), ([float]($y * $Super)), ([float]($w * $Super)), ([float]($h * $Super))))
    return $p
}

# a thick arc with an arrowhead at its end, angles in degrees (0 = right, clockwise)
function ArcArrow([double]$cx, [double]$cy, [double]$r, [double]$from, [double]$sweep, [double]$thick, [double]$head) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::White), ([float]($thick * $Super))
    $p.AddArc([float](($cx - $r) * $Super), [float](($cy - $r) * $Super), [float](2 * $r * $Super), [float](2 * $r * $Super), [float]$from, [float]$sweep)
    $p.Widen($pen)
    $end = ($from + $sweep) * [Math]::PI / 180
    $ex = $cx + $r * [Math]::Cos($end); $ey = $cy + $r * [Math]::Sin($end)
    $dir = $end + [Math]::Sign($sweep) * [Math]::PI / 2           # the way the arc runs at its end
    $nx = [Math]::Cos($dir); $ny = [Math]::Sin($dir)
    $tip = P ($ex + $nx * $head) ($ey + $ny * $head)
    $l = P ($ex - $ny * $head) ($ey + $nx * $head)
    $rr = P ($ex + $ny * $head) ($ey - $nx * $head)
    $p.AddPolygon([System.Drawing.PointF[]]@($tip, $l, $rr))
    return $p
}

# an arrow from (x0, y0) to (x1, y1)
function Arrow([double]$x0, [double]$y0, [double]$x1, [double]$y1, [double]$thick, [double]$head, [double]$headw) {
    $dx = $x1 - $x0; $dy = $y1 - $y0; $len = [Math]::Sqrt($dx * $dx + $dy * $dy)
    $ux = $dx / $len; $uy = $dy / $len; $nx = -$uy; $ny = $ux
    $bx = $x1 - $ux * $head; $by = $y1 - $uy * $head
    $t = $thick / 2
    Poly @(($x0 + $nx * $t), ($y0 + $ny * $t), ($bx + $nx * $t), ($by + $ny * $t), ($bx + $nx * $headw), ($by + $ny * $headw),
           $x1, $y1, ($bx - $nx * $headw), ($by - $ny * $headw), ($bx - $nx * $t), ($by - $ny * $t), ($x0 - $nx * $t), ($y0 - $ny * $t))
}

function Join-Paths($list) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $p.FillMode = 'Winding'
    foreach ($q in $list) { $p.AddPath($q, $false) }
    return $p
}

# each type's glyph, inside the badge (about 14..50 across, 16..48 down)
function Glyph([int]$t) {
    switch ($t) {
        0 { Join-Paths @((Circle 40 32 10), (Poly @(18, 24, 34, 27, 34, 31, 18, 29)), (Poly @(13, 30.5, 32, 30.5, 32, 34.5, 13, 34.5)), (Poly @(18, 36, 34, 34, 34, 38, 18, 40))) }  # projectile: a shot and its trail
        1 { Join-Paths @((Arrow 14 34 51 34 7 11 10), (Rect 13 42 38 4)) }                  # ground: along the floor
        2 { $l = @(); for ($i = 0; $i -lt 8; $i++) { $a = $i * [Math]::PI / 4; $c = [Math]::Cos($a); $s = [Math]::Sin($a)
                $l += Poly @((32 + $c * 19), (32 + $s * 19), (32 - $s * 4.5), (32 + $c * 4.5), (32 + $s * 4.5), (32 - $c * 4.5)) }
            Join-Paths ($l + @(Circle 32 32 7)) }                                            # expansion: a burst
        3 { Join-Paths @((Poly @(14, 18, 25, 18, 37, 32, 25, 46, 14, 46, 26, 32)), (Poly @(29, 18, 40, 18, 52, 32, 40, 46, 29, 46, 41, 32))) }  # dash: two chevrons
        4 { Arrow 17 47 47 17 8 14 11 }                 # anti-air: up and forward
        5 { Join-Paths @((Circle 23 25 7.5), (Poly @(41, 15, 50, 31, 32, 31)), (Rect 17 35 13 13), (Poly @(41, 34, 49, 42, 41, 50, 33, 42))) }  # variety: mixed shapes
        6 { ArcArrow 32 33 13 200 280 7 8.5 }                                                # throw: a turn over
        7 { Join-Paths @((ArcArrow 32 36 11 200 280 6 7.5), (Poly @(32, 12, 40, 21, 24, 21))) }  # air throw: a turn, up high
        8 { Join-Paths @((Rect 27 15 10 34), (Rect 15 27 34 10)) }                         # heal: a cross
        9 { Poly @(37, 12, 21, 35, 31, 35, 26, 52, 44, 27, 34, 27, 41, 12) }                  # enhance: a bolt
        10 { $ring = New-Object System.Drawing.Drawing2D.GraphicsPath
             $ring.AddEllipse([float](17 * $Super), [float](17 * $Super), [float](30 * $Super), [float](30 * $Super))
             $ring.Widen((New-Object System.Drawing.Pen ([System.Drawing.Color]::White), ([float](5 * $Super))))
             Join-Paths @($ring, (Rect 30 10 4 11), (Rect 30 43 4 11), (Rect 10 30 11 4), (Rect 43 30 11 4), (Circle 32 32 4)) }  # capture: a sight
        11 { Join-Paths @((Arrow 32 46 32 11 9 14 12), (Rect 17 46 30 5)) }                  # launcher: straight up
        12 { $l = @((Poly @(32, 19, 39, 47, 25, 47)), (Rect 12 18 40 4.5), (Rect 15 21 2.5 11), (Rect 47 21 2.5 11))
             $l += Poly @(9, 31, 23, 31, 21, 36, 16, 38, 11, 36)
             $l += Poly @(41, 31, 55, 31, 53, 36, 48, 38, 43, 36)
             Join-Paths $l }  # balance: scales
    }
}

# the badge: a hexagon pointed at the sides, as the pillars are
function Badge([double]$inset) {
    $w = 30 - $inset; $h = 26 - $inset; $k = 13 - $inset * 0.5
    Poly @((32 - $w), 32, (32 - $w + $k), (32 - $h), (32 + $w - $k), (32 - $h), (32 + $w), 32, (32 + $w - $k), (32 + $h), (32 - $w + $k), (32 + $h))
}

function Draw-Icon([int]$t) {
    $b, $g = New-Canvas $S $S
    # the badge: a black rim, a white rim, then the face shaded top to bottom
    $g.FillPath((New-Object System.Drawing.SolidBrush (Grey 0)), (Badge 0))
    $g.FillPath((New-Object System.Drawing.SolidBrush (Grey 14)), (Badge 2.5))
    $face = Badge 4
    $bounds = $face.GetBounds()
    $grad = New-Object System.Drawing.Drawing2D.LinearGradientBrush $bounds, (Grey 11), (Grey 6), ([System.Drawing.Drawing2D.LinearGradientMode]::Vertical)
    $blend = New-Object System.Drawing.Drawing2D.ColorBlend 4
    $blend.Colors = [System.Drawing.Color[]]@((Grey 11), (Grey 9), (Grey 7), (Grey 6))
    $blend.Positions = [single[]]@(0, 0.48, 0.52, 1)
    $grad.InterpolationColors = $blend
    $g.FillPath($grad, $face)
    # the glyph: a black edge round a white shape
    $glyph = Glyph $t
    $edge = $glyph.Clone()
    $pen = New-Object System.Drawing.Pen (Grey 0), ([float](5 * $Super))
    $pen.LineJoin = 'Round'
    $g.DrawPath($pen, $edge)
    $g.FillPath((New-Object System.Drawing.SolidBrush (Grey 0)), $edge)
    $g.FillPath((New-Object System.Drawing.SolidBrush (Grey 15)), $glyph)
    $g.Dispose()
    $small = New-Object System.Drawing.Bitmap $IconSize, $IconSize, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $gs = [System.Drawing.Graphics]::FromImage($small)
    $gs.InterpolationMode = 'HighQualityBicubic'
    $gs.PixelOffsetMode = 'HighQuality'
    $gs.DrawImage($b, 0, 0, $IconSize, $IconSize)
    $gs.Dispose(); $b.Dispose()
    return $small
}

# ---- the font -------------------------------------------------------------------------------------
$FontPx = 40                                   # the em, in texels
$Font = New-Object System.Drawing.Font 'Arial', ([float]$FontPx), ([System.Drawing.FontStyle]'Bold, Italic'), ([System.Drawing.GraphicsUnit]::Pixel)
$Family = $Font.FontFamily
$Style = [int][System.Drawing.FontStyle]'Bold, Italic'
$Em = $Family.GetEmHeight($Style)
$Ascent = [Math]::Ceiling($FontPx * $Family.GetCellAscent($Style) / $Em)
$Descent = [Math]::Ceiling($FontPx * $Family.GetCellDescent($Style) / $Em)
$GlyphH = $Ascent + $Descent
$fmt = [System.Drawing.StringFormat]::GenericTypographic
$fmt.FormatFlags = $fmt.FormatFlags -bor [System.Drawing.StringFormatFlags]::MeasureTrailingSpaces
$tmp, $tg = New-Canvas 8 8
$tg.TextRenderingHint = 'AntiAlias'

$Glyphs = @()
for ($c = 32; $c -lt 127; $c++) {
    $ch = [string][char]$c
    $adv = $tg.MeasureString($ch, $Font, 1000, $fmt).Width
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddString($ch, $Family, $Style, [float]$FontPx, (New-Object System.Drawing.PointF 0, 0), $fmt)
    $bd = $path.GetBounds()
    if ($bd.Width -le 0) { $left = 0; $w = 0 } else { $left = [Math]::Floor($bd.Left) - 1; $w = [Math]::Ceiling($bd.Right) + 1 - $left }
    $bits = New-Object byte[] ([Math]::Max(1, $w * $GlyphH))
    if ($w -gt 0) {
        $gb, $gg = New-Canvas $w $GlyphH
        $gg.TranslateTransform([float](-$left), 0)
        $gg.FillPath([System.Drawing.Brushes]::White, $path)
        $gg.Dispose()
        for ($y = 0; $y -lt $GlyphH; $y++) { for ($x = 0; $x -lt $w; $x++) { $bits[$y * $w + $x] = [byte][Math]::Round($gb.GetPixel($x, $y).A / 17) } }
        $gb.Dispose()
    }
    $Glyphs += [pscustomobject]@{ Code = $c; Left = [int]$left; Width = [int]$w; Advance = [Math]::Round($adv * 4); Bits = $bits }
}

# ---- the icons ------------------------------------------------------------------------------------
$Icons = @()
for ($t = 0; $t -lt $Types.Count; $t++) {
    $bmp = Draw-Icon $t
    $px = New-Object byte[] ($IconSize * $IconSize)
    for ($y = 0; $y -lt $IconSize; $y++) {
        for ($x = 0; $x -lt $IconSize; $x++) {
            $p = $bmp.GetPixel($x, $y)
            $a = [Math]::Round($p.A / 17)
            $l = if ($p.A -gt 0) { [Math]::Round($p.R / 17) } else { 0 }
            $px[$y * $IconSize + $x] = [byte](($a -shl 4) -bor $l)
        }
    }
    $Icons += , $px
    if ($Preview) { $bmp.Save((Join-Path (Split-Path $Preview) ("icon_$t.png"))) }
    $bmp.Dispose()
}

# ---- the C file -----------------------------------------------------------------------------------
$sb = New-Object System.Text.StringBuilder
function L([string]$s) { [void]$sb.Append($s + "`n") }
function Bytes([byte[]]$b, [int]$per = 24, [switch]$Nibbles) {
    $vals = if ($Nibbles) {
        for ($i = 0; $i -lt $b.Count; $i += 2) { $lo = $b[$i]; $hi = if ($i + 1 -lt $b.Count) { $b[$i + 1] } else { 0 }; ($hi -shl 4) -bor $lo }
    } else { $b }
    for ($i = 0; $i -lt $vals.Count; $i += $per) {
        L ("`t" + (($vals[$i..([Math]::Min($i + $per, $vals.Count) - 1)] | ForEach-Object { '0x{0:x2},' -f $_ }) -join ' '))
    }
}

L '/* The art of the select screen''s assist pillar labels (sel_label.c). Written by tools/sel_label_art.ps1:'
L '   change that script and run it again rather than editing this file. */'
L '#include "player.h"'
L '#include "sel_label.h"'
L ''
L "/* the move names' font: Arial Bold Italic at $FontPx texels, each glyph's coverage four bits a texel,"
L '   two to a byte with the left one low, rows of the glyph''s width; its top is the line''s top */'
L 'const SEL_GLYPH sel_font_glyph[SEL_FONT_CHARS] = {'
$ofs = 0
foreach ($gl in $Glyphs) {
    L ("`t{{ {0}, {1}, {2}, {3} }},`t/* '{4}' */" -f $gl.Left, $gl.Width, $gl.Advance, $ofs, $(if ($gl.Code -eq 92) { 'backslash' } elseif ($gl.Code -eq 42) { 'star' } else { [char]$gl.Code }))
    $ofs += [Math]::Ceiling($gl.Width * $GlyphH / 2)
}
L '} ;'
L ''
L "const Uint8 sel_font_bits[$ofs] = {"
$all = New-Object System.Collections.Generic.List[byte]
foreach ($gl in $Glyphs) {
    $n = $gl.Width * $GlyphH
    for ($i = 0; $i -lt $n; $i += 2) { $lo = $gl.Bits[$i]; $hi = if ($i + 1 -lt $n) { $gl.Bits[$i + 1] } else { 0 }; $all.Add([byte](($hi -shl 4) -bor $lo)) }
}
Bytes $all.ToArray()
L '} ;'
L ''
L '/* the assist type symbols, by sel_glow_step_tbl''s label: coverage in the high nibble, shade in the low */'
L "const Uint8 sel_icon_bits[SEL_ICON_TYPES][SEL_ICON_SIZE * SEL_ICON_SIZE] = {"
for ($t = 0; $t -lt $Icons.Count; $t++) {
    L "`t{`t/* $($Types[$t]) */"
    Bytes $Icons[$t]
    L "`t},"
}
L '} ;'
L ''
L '/*==============================================================*/'
L '/*	End of File.												*/'
L '/*==============================================================*/'
L '/* "sel_label_art.c" End of File	*/'
[System.IO.File]::WriteAllText($Out, $sb.ToString().Replace("`r`n", "`n"))

$hdr = @"
/* The select screen's assist pillar labels: the move each assist type does, by name, with a symbol of its type. */
#ifndef SEL_LABEL_H
#define SEL_LABEL_H

#define SEL_FONT_FIRST	32		/* the font's first character */
#define SEL_FONT_CHARS	95		/* and how many */
#define SEL_FONT_H		$GlyphH		/* each glyph's rows */
#define SEL_FONT_ASCENT	$Ascent		/* the baseline, from the top */
#define SEL_ICON_TYPES	$($Types.Count)		/* the assist types: sel_glow_step_tbl's labels */
#define SEL_ICON_SIZE	$IconSize		/* each symbol's texels across and down */

typedef struct SEL_GLYPH {
	Sint8	left;		/* its first column, from the pen */
	Uint8	width;		/* its columns */
	Uint8	advance;	/* the pen's move after it, in quarter texels */
	Uint32	ofs;		/* where its bits start in sel_font_bits */
} SEL_GLYPH;

extern const SEL_GLYPH sel_font_glyph[SEL_FONT_CHARS];
extern const Uint8 sel_font_bits[];
extern const Uint8 sel_icon_bits[SEL_ICON_TYPES][SEL_ICON_SIZE * SEL_ICON_SIZE];

#endif
"@
[System.IO.File]::WriteAllText((Join-Path $Root 'include/sel_label.h'), $hdr.Replace("`r`n", "`n"))
"wrote src/sel_label_art.c ($ofs font bytes, $($Icons.Count) icons) and include/sel_label.h"
