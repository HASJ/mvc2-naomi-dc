# Super Hop: design (short)

## Intent
Let the player control superjump height by pressing a downward direction (`1`, `2` or `3`) after launching it. This
works as in Marvel vs. Capcom Infinite's super hop: it is not a separate jump type but a cancel of the rise.
The player decides the height by choosing when to stop.

## Behaviour
- While a superjump is rising, pressing `1`, `2` or `3` stops the rise. The stop is not instant: the rise speed
  eases to 0 over about 5 frames (`HOP_STOP_FRAMES`), so it does not feel jarring. The fighter then falls with the
  superjump's normal fall rate. The later the press, the higher the hop. Once started, the stop cannot be undone by
  releasing the direction (as in MVCI, the momentum is cancelled).
- **Floor.** The lowest hop is about half a normal jump. The 5-frame ease-off itself provides most of it: a press
  on the first frames still adds about 100 units of rise while the speed ramps down. So no separate wait for a
  floor height is needed, and `HOP_MIN_RISE` starts at about 0. The builder tunes `HOP_STOP_FRAMES` and
  `HOP_MIN_RISE` so the earliest possible press lands at about half a normal jump (about 120 units).
- Superjump maneuverability is unchanged: `pl_sjump_ctrl` steering, the sideways limits, air actions, the fall
  rate. Only vertical speed is zeroed.
- Air jumps (double/triple jump) and wall jumps get the same control, but only in a chain that began with a ground
  superjump. Each extra jump has its own floor, measured from where that jump started.
- There is no time window. The stop is available from the floor height until the apex.

## Goals / non-goals / anti-goals
- Goal: one universal rule for the whole cast. No per-character tables.
- Non-goals: the Classic/Ultimate ruleset flag (G1 in the ideas doc), fast-falling, CPU use of the hop, tag-in and
  assist hops (`act_change_out*` sets `sjump_flag` but is not a ground superjump).
- Known cost, accepted by following MvC Infinite: an air special or hyper whose motion starts with `2` stops the
  rise if it is input while rising. The player picks the height by when they start the motion.

## Constraints
- The change is unconditional in shared `src/`, like the other mod commits (no `HOST_PORT` guard).
- New state goes in `PLAYER_WORK` (`include/pl_work.h`) so savestates and netplay rollback stay deterministic.
- Verify with the PC port build and a headless run, then install into `ports/build-dist/`.

## Approaches
1. **Zero the rise speed on the first frame a down direction is held and the floor is reached. Chosen.**
   It is MVCI's rule, it is one shared function, and it needs one small field (the launch height).
2. Decay the rise speed over a short held window. This was the earlier design. Rejected by the user in favour of
   MVCI's rule.
3. Heavier gravity while held. Rejected: it changes the fall arc and needs restoring.

## Decisions
- Universal constants, no per-character tables: `HOP_STOP_FRAMES = 5` and a small `HOP_MIN_RISE` (height gained
  since launch before a press counts, starting at about 0). The target is a lowest hop of about half the mean
  normal-jump apex (mean about 240, range about 135 to 281), so about 120 units. Playtest and tune.
- The ease-off is a linear ramp of the current rise speed to 0 over `HOP_STOP_FRAMES`, kept in a small
  `PLAYER_WORK` counter. While it runs, gravity still applies and further presses are ignored.
- Hand-model facts (not run in the game): all characters launch from one of 16 near-identical tables, so the
  floor is reached about 4 frames in. A full superjump apex is about 605-811 units, 29-54 frames.
- Applies only while `sjump_flag == 1` and `spd.y > 0`. After `spd.y` is zeroed, the existing `pl_sjump_top_ck`
  (`src/pl_sub.c`) moves the jump to `sjump_flag = 2` with the character's own fall rate.
- Hook: in or next to `pl_sjump_ctrl` (`src/pl_sub.c`), which every character's superjump-air routine already
  calls, before `pl_jump_move` runs.
- A chain flag set in the ground `act_sjump` (`src/pl_act.c`) and cleared on landing and on hit covers wall jumps
  and air jumps. The launch height is stored when each jump starts.

## Corrections found while planning (code facts that refine the above)
- **Any superjump state can hop** (user decision). The gate is `sjump_flag != 0` and a rising `spd.y`, so ground
  superjumps, launcher-chase superjumps, air recovery and tag-in superjumps all qualify, and a normal jump does not.
  No separate chain flag is needed. If the wall-jump hand-off clears `sjump_flag`, a small flag is added for it.
- **Air attacks never halt the rise on their own** (user decision). Only `1`/`2`/`3` starts a stop. Because attacks
  in a superjump run the same steering routine, a downward input during an air attack still stops the rise.
- **Air jumps use the normal-jump launch table**, not the superjump table (`pl_air_jump_ck` clears `sjump_flag`
  around `pl_jump_spd_set`). Their launch is about 20 units a frame, so the earliest air-jump hop is about 40-60
  units, lower than "half a jump". This is accepted and measured, not tuned.
- **Wall jumps never reach `pl_sjump_ctrl`.** `pl_wall_jump_ck` sets fixed speeds, `act_wall_jump` plays an
  animation, then the fighter is in `N_JUMP` and `act_jump_air` (`src/pl_main.c`). It needs its own hook there,
  gated on the chain flag, and "rising" means `spd.y > 0` (`sjump_flag` may already be 2). Felicia's wall jump
  goes through `TBL_TJUMP` and is excluded.
- `pl_sjump_ctrl` returns early before steering (`sjump_cnt < 7`, and no forward/back held). The hop goes before
  both returns, so a pure `2` and the first 7 frames work.
- `pl_sjump_ctrl` is also run by each character's air-attack move (`sjump_move`), so the stop also applies while
  attacking in a superjump.
- The computer is excluded (`cpu_flag`): `cpu_crouch_flag` holds `KEY_DOWN` for it.
- The ramp only lowers a positive `spd.y`, so it never fights `pl_sjump_top_ck`'s fall rate or `pl_jump_hit_ck`.
- `HOP_MIN_RISE` is dropped for now. The ramp alone should give about 100 units; add a floor only if the trace
  shows the earliest hop too low.

## Measured results (`ports/tools/hop_trace.ps1`, Dreamcast `mvc2dc.exe`)
- **Ground superjump** (Ruby Heart `13`): baseline apex `719.3` (40 frames to apex). Earliest `2` (`HOP_MIN_RISE = 60.0f`
  + `HOP_STOP_FRAMES = 5`) lands at `130.4` (5 frames); `2` at `4908` gives `161.4`; `2` at `4918` gives `467.3` (19 frames).
  `1` and `3` match `2`. Forward steering `spd.x` is identical frame-by-frame with or without a hop. Air attack (`b1`)
  alone rises to `720.5` (`cnt=0`), and `b1 + 2` stops at `402.1`. Normal jump is `239.5` with or without `2` (including
  right after landing from a hop).
- **Air jumps** (Strider `05`, air jump from `y = 711.75`): no `2` reaches `1069.3` (`+357.6`); `2` at `4953` / `4955`
  stops at `839.5` (`+127.7`) / `876.2` (`+164.5`). An air jump during an active superjump stop (`2` at `4912`, `up` at
  `4914`) resets `hop_stop_cnt` and rises cleanly (`648.1` after SJ cut at `313.7`). Chun-Li (`1b`) second air jump
  stops with `2` at `1069.8`.
- **Wall jumps** (Spider-Man `0c`, superjump clinging at `y = 331.1`, wall jump at `f = 4924`): no `2` reaches `425.8`
  (`+94.7`, 13 frames); earliest `2` (`4916`) stops at `354.0` (`+22.9`, 3 frames); late `2` (`4930`) stops at `415.6`
  (`+84.5`, 11 frames). A wall jump after a mid-SJ stop (`298.9`) starts fresh (`+94.7` to `393.6`, `cnt = 0`). Strider
  (`05`) normal jump + air jump into a wall jump (`sj-seen = False`) reaches `491.1` (`cnt = 0`) with or without `2`.

## Left to the builder
- The exact field names and places in `PLAYER_WORK`. Check the offset comments before adding a field.
- Whether wall jumps keep `sjump_flag`, how high their floor should be (their launch speed differs from a
  superjump's), and where each wall-jump character's own routine needs the hook. Some characters have their own
  superjump-air routines (`s_plXX.c`).
- The headless verification method, using `MVC2_PRESS` and an apex readout from the log.
