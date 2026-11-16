# Marvel vs. Capcom 2 on the 3DS

Both versions run at full speed, 60 fps, on every 3DS and 2DS, the original models too. A few screen changes
(the title, the select screen, the load before a fight) still pause for a moment while they read the SD card.

The arcade game and the Dreamcast version, built for the original 3DS and 2DS (a New 3DS also gets its faster
CPU). No game data comes with it: you need the MAME `mvsc2` set, the Dreamcast USA disc, or both.

## Installing

Copy the zip's `sd` folder to the root of the SD card, then install `mvc2.cia` with FBI, or start
`mvc2.3dsx` from the Homebrew Launcher. On an Old 3DS or 2DS use the CIA: the 3dsx has only the memory for the
Dreamcast version there.

Or install the latest release's CIA straight from FBI: Remote Install, Scan QR Code, and scan this. Then make
the folders below for the game's data.

![FBI QR code for the latest mvc2.cia](port/ctr/cia/install-qr.png)

    https://github.com/gibletto/mvc2-naomi-dc/releases/latest/download/mvc2.cia

Sound needs the usual `sdmc:/3ds/dspfirm.cdc`.

## The game's data

    sdmc:/3ds/mvc2/
        mvc2.3dsx
        roms/mvsc2/     MAME's mvsc2.zip, or its files
        disc/           the USA Dreamcast disc: an image of it, or its files
        mvc2.cfg        settings
        mvc2.log        the last run's log (mvc2_o3ds.log for the Old 3DS test build)

The first start unpacks the ROM set into one file, `roms/mvsc2/mvsc2.bin` (136 MB); the zip or the set's other
files can be deleted after.

For the Dreamcast, put any one of these in `disc/`:

| What                                   | Example                                                 |
|----------------------------------------|---------------------------------------------------------|
| a CHD                                  | `Marvel vs. Capcom 2 (USA).chd`                         |
| a GDI with its tracks                  | `disc.gdi`, `track01.bin`, `track02.raw`, `track03.bin` |
| a CUE with its BINs (Redump's set)     | `Marvel vs. Capcom 2 (USA).cue` and its three `.bin`s   |
| track 3 alone, raw or as an ISO        | `Marvel vs. Capcom 2 (USA) (Track 3).bin`, `mvc2.iso`   |
| the files already extracted            | `1ST_READ.BIN` and the rest                             |

The first start unpacks an image's files into `disc/` (210 MB, with a progress bar on the bottom screen).
That happens once, and the image can be deleted after; don't turn the 3DS off while it runs, though if it is,
the next start unpacks it again. Leave room on the SD card for both the image and the files while it unpacks.

With both versions there it asks which to play (touch NAOMI or Dreamcast, or choose with left, right and A) and
keeps the answer in `mvc2.cfg` as `mode =`.

## Controls

| Button            |                                   |
|-------------------|-----------------------------------|
| D-pad, Circle Pad | move                              |
| Y X               | light, heavy punch                |
| B A               | light, heavy kick                 |
| L (or ZL)         | assist 1                          |
| R (or ZR)         | assist 2                          |
| START             | start                             |
| SELECT            | coin                              |
| SELECT + START    | switch to the other version       |
| SELECT + B        | quit                              |

On the Dreamcast, Y X L B A R are its X Y L A B R.

The bottom screen's DIP button (the arcade only) opens the arcade's test menu: its game data, and its
settings (difficulty, damage, timer, continue, game speed and the rest) with free play added. The D-pad
chooses, A sets and B goes back; SAVE & EXIT keeps them, and the game starts again with them on leaving.

## Settings

`mvc2.cfg` has one `key = value` a line:

- `scale = stretch` fills the top screen (the default is 1:1, 320 by 240, between Marvel and Capcom pillars).
- `pillars = off` leaves the sides black.
- `render = exact` draws at 640 by 480 and halves it: sharper, slower.
- `freeplay = on` plays the arcade without coins (the DIP menu sets it).
- `arcade =` and `dc =` set other data folders.

## Building it

See the 3DS section of `ports/README.md` in the source. The Old 3DS test build runs on a New 3DS as an Old 3DS does.
