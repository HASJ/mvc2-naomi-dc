<#
.SYNOPSIS
  Headless jump measurement for Super Hop: runs the PC port minimized and unfocused with MVC2_HOP_TRACE=1,
  then summarises the HOPTRACE lines in mvc2.log, one row a jump.

.DESCRIPTION
  Follow .agent/rules/headless-test.md (workspace root): the run is started with `cmd /c start /min /wait` from a
  hidden Start-Process, with SDL_WINDOW_ACTIVATE_WHEN_SHOWN=0, from ports/build-dist.
  Build and install first (cmake --build ports/build; cmake --install ports/build --prefix ports/build-dist).

  -Press   the MVC2_PRESS string: [n][+dur]:[p1:|p2:]key,...  (keys: start up down left right b1-b6 coin sjump dash)
  -Frames  MVC2_FRAMES, the run's length in frames
  -Dc      run the Dreamcast version (mvc2.exe --dc) instead of the arcade one
  -Shots   optional MVC2_SHOT frames, e.g. '600,900' (writes shot_n.bmp in build-dist)
  -Menu    put the recorded Dreamcast menu route ($MenuDc) in front of -Press: P1 Ruby Heart (pl 13) against
           Cable, the fight under way by frame 4500. Implies -Dc.
  -Point   with -Menu, P1's point character as a hex id (include/fighter.h), e.g. 05 for Strider: the cursor is
           walked there on the Dreamcast grid (ef05_home_grid_tbl, src/ef05_card.c) before the first pick; the
           other two picks are the cells to its right.

  Output per jump: first frame, frames to apex, apex height (units above the ground), spd.x at the apex, whether
  sjump_flag was set, and the largest hop_stop_cnt seen.

  Recorded recipes and results are kept below by the tasks that measured them (see "Recipes").

.NOTES
  Recipes (Tasks 1-4 of docs/superpowers/plans/2026-11-18-super-hop.md)
  - Superjump: '4900+3:down,4903+5:up' with -Menu (the 2, 8 command; the 'sjump' test key did not start one).
  - Baseline, before Super Hop: Ruby Heart (13) superjump apex 719.3, 40 frames to the apex, 90 frames in the air.
    Two runs gave identical numbers.
  - The game's getenv is a generated stub, so the trace reads MVC2_HOP_TRACE through the port's host_env_on().
  - Task 2, Super Hop on a ground superjump (Ruby Heart 13; launch '4900+3:down,4903+5:up', first air frame 4907):
      no 2                       719.3 (40 frames)      unchanged
      2 at 4906 (held or tapped)  130.4 (5 frames)       earliest hop: waits for HOP_MIN_RISE, then 5-frame ease-off
      2 at 4908                   161.4                  1 and 3 give the same
      2 at 4918                   467.3 (19 frames)
      forward superjump +/- hop   spd.x identical frame by frame
      b1 at 4915, no 2            720.5                  the air attack's own path (cnt stays 0); b1 + 2: 402.1
      normal jump '4900+5:up'     239.5 with or without 2, also right after landing from a hop
  - Task 3, air jumps (-Point 05 Strider, superjump apex 756.3; air jump '4950+3:up' from y 711.75):
      no 2                        1069.3 (+357.6)
      2 at 4953 / 4955            839.5 (+127.7) / 876.2 (+164.5)
      2 for 2 frames at 4912, up at 4914 (air jump mid-stop): the air jump rises fully (648.1), was cut at 313.7
      -Point 1b Chun-Li: second air jump at 4962, 2 at 4966: stops at 1069.8
#>
param(
    [Parameter(Mandatory = $true)][string]$Press,
    [int]$Frames = 1500,
    [switch]$Dc,
    [string]$Shots = '',
    [switch]$Menu,
    [string]$Point = '13'
)

# Dreamcast: title, mode, join, three picks a side (cursor moved right between them), assist types, then b1 on
# both sides through the later screens
$MenuDc = '300:start,500:start,700:start,900:start,1000:b1,1100:right,1200:b1,1300:right,1400:b1,1600:b1,1700:start,1800:p2:start,2000:p2:b1,2100:p2:right,2200:p2:b1,2300:p2:right,2400:p2:b1,2600:p2:b1,2700:p2:start,2900:b1,2900:p2:b1,3050:b1,3050:p2:b1,3200:b1,3200:p2:b1,3350:b1,3350:p2:b1,3500:b1,3500:p2:b1,3650:b1,3650:p2:b1,3800:b1,3800:p2:b1,3950:b1,3950:p2:b1,4100:b1,4100:p2:b1,4250:b1,4250:p2:b1,4400:b1,4400:p2:b1'
# the Dreamcast select grid, ef05_home_grid_tbl in src/ef05_card.c (-1: no cell); P1's cursor starts on 0x13 (3, 3)
$Grid = @(
    @(-1, 0x20, 0x1c, 0x38, 0x07, 0x2d, 0x36, -1),
    @(-1, 0x23, 0x02, 0x15, 0x06, 0x39, 0x2e, -1),
    @(0x27, 0x1f, 0x00, 0x14, 0x16, 0x08, 0x2f, 0x2b),
    @(0x21, 0x1e, 0x01, 0x13, 0x17, 0x09, 0x28, 0x35),
    @(0x25, 0x37, 0x04, 0x12, 0x0b, 0x0c, 0x29, 0x31),
    @(0x26, 0x03, 0x22, 0x10, 0x0f, 0x0e, 0x2c, 0x32),
    @(-1, 0x24, 0x05, 0x11, 0x0a, 0x0d, 0x33, -1),
    @(-1, 0x3a, 0x1d, 0x1b, 0x30, 0x2a, 0x34, -1)
)
if ($Menu) {
    $want = [Convert]::ToInt32($Point, 16)
    $moves = @()
    for ($r = 0; $r -lt 8; $r++) {
        for ($c = 0; $c -lt 8; $c++) {
            if ($Grid[$r][$c] -eq $want) {
                $keys = @()
                if ($r -gt 3) { $keys += @('down') * ($r - 3) } elseif ($r -lt 3) { $keys += @('up') * (3 - $r) }
                if ($c -gt 3) { $keys += @('right') * ($c - 3) } elseif ($c -lt 3) { $keys += @('left') * (3 - $c) }
                $t = 940
                foreach ($k in $keys) { $moves += "${t}:$k"; $t += 12 }
            }
        }
    }
    if ($want -ne 0x13 -and $moves.Count -eq 0) { Write-Error "no grid cell for point $Point"; exit 1 }
    $route = $MenuDc
    if ($moves.Count) { $route = $MenuDc -replace '900:start,', ('900:start,' + ($moves -join ',') + ',') }
    $Press = "$route,$Press"
    $Dc = $true
}

$ErrorActionPreference = 'Stop'
$dist = Join-Path $PSScriptRoot '..\build-dist' | Resolve-Path
$exe = 'mvc2.exe'
$log = Join-Path $dist 'mvc2.log'
if (Test-Path $log) { Remove-Item $log -Force }

$env:MVC2_FRAMES = "$Frames"
$env:MVC2_PRESS = $Press
$env:MVC2_HOP_TRACE = '1'
$env:MVC2_UNLOCK = '1'
$env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN = '0'
if ($Shots -ne '') { $env:MVC2_SHOT = $Shots } else { Remove-Item Env:\MVC2_SHOT -ErrorAction SilentlyContinue }

$argList = @('/c', 'start', '""', '/min', '/wait', $exe)
if ($Dc) { $argList += '--dc' }
Push-Location $dist
try {
    Start-Process cmd.exe -ArgumentList $argList -WindowStyle Hidden -Wait
} finally {
    Pop-Location
}

if (-not (Test-Path $log)) { Write-Error "no mvc2.log after the run in $dist"; exit 1 }

$rows = Select-String -Path $log -Pattern '^.*HOPTRACE n=(\d+) f=(\d+) pl=([0-9a-f]+) act=(-?\d+) sj=(\d+) y=(-?[\d.]+) sx=(-?[\d.]+) sy=(-?[\d.]+) cnt=(\d+)' |
    ForEach-Object {
        $m = $_.Matches[0].Groups
        [pscustomobject]@{
            N = [int]$m[1].Value; F = [int]$m[2].Value; Pl = $m[3].Value; Act = [int]$m[4].Value; Sj = [int]$m[5].Value
            Y = [double]$m[6].Value; Sx = [double]$m[7].Value; Sy = [double]$m[8].Value; Cnt = [int]$m[9].Value
        }
    }

if (-not $rows) { Write-Output 'no HOPTRACE lines (is the build installed, and did the run reach a fight?)'; exit 2 }

# split into rises: a new one starts after a gap in frames (the fighter was on the ground), when the speed turns
# upward again (a new jump, an air jump) or when y falls back to the ground
$jumps = @()
$cur = @()
$prev = $null
foreach ($r in $rows) {
    if ($prev -and ($r.F -gt $prev.F + 2 -or ($r.Sy -gt 0 -and $prev.Sy -le 0) -or ($prev.Y -gt 1.0 -and $r.Y -le 1.0 -and $prev.Sy -lt 0))) {
        if ($cur.Count) { $jumps += , $cur }
        $cur = @()
    }
    $cur += $r
    $prev = $r
}
if ($cur.Count) { $jumps += , $cur }

$i = 0
foreach ($j in $jumps) {
    $apex = $j | Sort-Object Y -Descending | Select-Object -First 1
    if ($apex.Y -lt 2.0) { continue }    # the last frames of a landing, split off on their own
    $i++
    $maxCnt = ($j | Measure-Object Cnt -Maximum).Maximum
    [string]::Format([cultureinfo]::InvariantCulture,
        '{0,2}: pl={8} f={1} frames-to-apex={2} apex-y={3:F1} sx@apex={4:F3} sj-seen={5} max-cnt={6} frames={7}',
        $i, $j[0].F, ($apex.N - $j[0].N), $apex.Y, $apex.Sx, (($j | Where-Object Sj -gt 0).Count -gt 0), $maxCnt, $j.Count, $j[0].Pl)
}
