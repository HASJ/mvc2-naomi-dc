# Marvel vs. Capcom 2 on the PSP

The Dreamcast version, drawn by the GE. The arcade version needs 32 MB of game RAM, more than the PSP has to spare,
so the PSP plays the Dreamcast version only, for now.

It runs on a PSP-2000, PSP-3000 or PSP Go (all 64 MB) with custom firmware (ARK-4 tested). The PSP-1000 has 32 MB,
too little for the game: it says so and stops. In PPSSPP, set Settings, System, PSP model to PSP-2000/3000.

## The game's data

    ms0:/PSP/GAME/MVC2/
        EBOOT.PBP
        disc/           the USA Dreamcast disc: an image of it, or its files (1ST_READ.BIN and the rest)
        mvc2.cfg        settings
        mvc2.log        the last run's log
        env.txt         test switches, NAME=value a line (MVC2_FRAMES=n, MVC2_SHOT=n,m, MVC2_DRAW_EVERY=n,
                        MVC2_RENDERER=soft, MVC2_WAV=name: the sound saved; MVC2_SOUND=1: sound in a test run)

The disc goes in `disc/`: a CHD, a GDI with its tracks, a CUE with its BINs, an ISO (or BIN) of track 3, or the
files already extracted. The first start unpacks an image into the folder, once; a CHD made with zstd
(`chdman createcd -c cdzs,cdfl`) unpacks fastest.

## Sound

The sound CPU's driver runs as C (`MVC2_SNDNATIVE=3`, as on the 3DS) and the channels are mixed 64 samples at a time
(`MVC2_SNDCHUNK=64`): its sounds come a little sooner than the PC's. `MVC2_SNDNATIVE=1` and `MVC2_SNDCHUNK=16` in
env.txt give the PC's sound sample for sample, at about three times the cost. Each frame's sound is made on the Media
Engine after the frame's list while the lists are made there (`MVC2_ME=1`; `MVC2_SNDME=0` keeps it on a thread below
the game's), and the same either way. It is played about three frames behind, stretched by up to 0.5% to stay there;
the WAV is saved before the stretch. Where the game's frames fall behind the screen, the next frames' sound is made
longer to keep up (`MVC2_SNDPACE=0` not); where it stalls longer than that (a load), the sound fades out and back in.
The music's next piece is read ahead on the disc reader (`MVC2_ADX_AHEAD=0` reads it in the frame it is wanted). A test run's log has the sound's time a frame and the queue every 300 frames (`MVC2_SNDLOG=1` in play too).

## Controls

| Button               |                                   |
|----------------------|-----------------------------------|
| D-pad, analog stick  | move                              |
| Square Triangle      | light, heavy punch                |
| Cross Circle         | light, heavy kick                 |
| L                    | assist 1                          |
| R                    | assist 2                          |
| START                | start                             |
| SELECT               | coin                              |
| SELECT + TRIANGLE    | options (the game stops)          |
| SELECT + CROSS       | quit                              |

## Settings

SELECT + TRIANGLE opens the options over the game: the CPU clock and the screen (4:3 with pillars, 4:3 or
stretched). START goes back to the game. They are kept in `mvc2.cfg`, one `key = value` a line:

- `clock = 222`, `266` or `333`: the CPU's MHz, the bus at half. 222, the PSP's own speed, is the default and lasts
  longest on a battery; 333 plays the smoothest and uses the most. Other values are rounded to the nearest of the
  three. A new clock takes effect when the game next starts. The log has the CPU and bus speeds they gave, measured.
- `scale = stretch` fills the screen (the default keeps 4:3), and `pillars = off` leaves the bars beside a 4:3
  picture black instead of the 3DS's MARVEL and CAPCOM pillars.
- `renderer = soft` draws in software instead of with the GE, at 320 by 240 (`render = exact`: 640 by 480).

The power switch puts the PSP to sleep as usual: the Media Engine, which makes the GE's lists, is given back to the
firmware first and taken again on waking, and the disc's files are opened again. `MVC2_ME_SLEEP=0` in env.txt holds
the switch off while the game runs instead.

## Differences from the PC

The sprites' small tiles are drawn from cells of one texture (`MVC2_GE_ATLAS=0`: a texture each). Where a scaled
sprite's sample falls exactly on a texel's edge, it can take the next row or column, so a few hundred pixels of a
frame differ from the PC's.

A 3D scene's big textures are drawn from half-size copies where they are shrunk to under half size, and from the
full size above that, as sharp as the PC's picture scaled down. When a frame has no room for that (most fights at
222 MHz), they are drawn from the half-size copies wherever they are shrunk at all, which saves the GE's reads of main
RAM; the HUD and menus stay sharp. `MVC2_GE_MIP_AUTO=0` in env.txt keeps the scenes sharp.

## Building it

With pspdev (the decompilation is the folder above, or `-DMVC2_DECOMP=path`):

    psp-cmake -G Ninja -B build-psp
    ninja -C build-psp

makes `build-psp/EBOOT.PBP`, with the game's RAM moved to where the PSP has memory (0x0A000000, the Dreamcast's
16 MB).
