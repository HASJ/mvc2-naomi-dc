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

  Output per jump: first frame, frames to apex, apex height (units above the ground), spd.x at the apex, whether
  sjump_flag was set, and the largest hop_stop_cnt seen.

  Recorded recipes and results are kept below by the tasks that measured them (see "Recipes").

.NOTES
  Recipes (Tasks 1-4 of docs/superpowers/plans/2026-11-18-super-hop.md)
  - Superjump: '4900+3:down,4903+5:up' with -Menu (the 2, 8 command; the 'sjump' test key did not start one).
  - Baseline, before Super Hop: Ruby Heart (13) superjump apex 719.3, 40 frames to the apex, 90 frames in the air.
    Two runs gave identical numbers.
  - The game's getenv is a generated stub, so the trace reads MVC2_HOP_TRACE through the port's host_env_on().
#>
param(
    [Parameter(Mandatory = $true)][string]$Press,
    [int]$Frames = 1500,
    [switch]$Dc,
    [string]$Shots = '',
    [switch]$Menu
)

# Dreamcast: title, mode, join, three picks a side (cursor moved right between them), assist types, then b1 on
# both sides through the later screens
$MenuDc = '300:start,500:start,700:start,900:start,1000:b1,1100:right,1200:b1,1300:right,1400:b1,1600:b1,1700:start,1800:p2:start,2000:p2:b1,2100:p2:right,2200:p2:b1,2300:p2:right,2400:p2:b1,2600:p2:b1,2700:p2:start,2900:b1,2900:p2:b1,3050:b1,3050:p2:b1,3200:b1,3200:p2:b1,3350:b1,3350:p2:b1,3500:b1,3500:p2:b1,3650:b1,3650:p2:b1,3800:b1,3800:p2:b1,3950:b1,3950:p2:b1,4100:b1,4100:p2:b1,4250:b1,4250:p2:b1,4400:b1,4400:p2:b1'
if ($Menu) { $Press = "$MenuDc,$Press"; $Dc = $true }

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

# split into jumps: a new jump starts after a gap in the trace's sequence (the fighter was on the ground) or when
# y falls back to the ground
$jumps = @()
$cur = @()
$prev = $null
foreach ($r in $rows) {
    if ($prev -and ($r.N -ne $prev.N + 1 -or ($prev.Y -gt 1.0 -and $r.Y -le 1.0 -and $prev.Sy -lt 0))) {
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
