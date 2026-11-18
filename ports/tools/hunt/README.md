# hunt

Plays the same input script in the port and in Flycast (the arcade ROM), dumps the game's work every
game frame in both, and reports the first frame each field differs.

- `port/sdl/hunt.c` runs a script in the port (`MVC2_HUNT`), and `hunt.lua` runs it in Flycast 2.7.
  Flycast reads the Lua file only by a relative name from its own folder, with `LuaFileName = hunt.lua`
  in `emu.cfg`. `hunt.py` copies the file there.
- `menu.py` gets both sides to the fight with the same teams. When the entrances start it sets both
  random number generators (`nl_random_next`, `rnd_hi`/`rnd_lo`) and the frame counters to the same
  values, and presses nothing until the fight starts.
- `battery.py` builds a script that does one move after another; `cpu.py` builds a computer-against-
  computer fight; `roster.py` sweeps whole characters; `report.py` summarises a sweep; `idle.py` looks for
  a computer player that stands still.

```
python -I tools/hunt/battery.py run/script.txt 46,0,1 2,39,27 0 qcf+lp+hp
python -I tools/hunt/hunt.py run/script.txt run
python -I tools/hunt/roster.py OUT 0 46,13 --fc=build/fc,build/fc2
```

Environment variables:
- `HUNT_EXE`: the port executable.
- `HUNT_FC`: a folder holding a copy of Flycast. Each parallel run needs its own copy.
- `HUNT_WORKS=1`: also dumps the 256 effect works.

The port reads a script's keys a frame late, as Flycast does; `now KEYS1 [KEYS2]` (port only) gives
the game keys on the frame they are set.

A trace is about 70 MB per 8000 frames, so the sweep deletes each one after comparing it.
