# Capcom

## Ruby Heart

- [] Gamma assist
  - [] Make it buff the characteristic that the active character lacks the most, which can be buffed up to three times and lasts 10 seconds but the timer does not restart once the buff reaches level 3, having to start at level 0 once its timer is up
    - [] Speed boost
      - Zangief, Hulk, Juggernaut, Colossus and other slow chars
      - Level 1 buff = 0f startup + -1f recovery + 15% movement speed
      - Level 2 buff = -1f startup + -2f recovery + 25% movement speed
      - Level 3 buff = -2f startup + -3f recovery + 35% movement speed
    - [] Defense boost
      - Zoners or low health characters, such as Blackheart, Strider, Magneto, Dhalsim, etc.
    - [] Damage boost
      - Everyone else
      - Level 1 = +10% damage
      - Level 2 = +25% damage
      - Level 3 = +40% damage
- [] Beta assist
  - [] Make it function more consistently in the corner

## Ken

- [x] Make 2LK 2HP more consistent by reducing 2LK's pushback
- [x] 6-way air dash
  - Directions: 4, 1, 2, 3, 6, 9
  - Neutral dashes forward; 7 and 8 don't dash
- [x] Can cancel specials and hypers into dashes and air dashes
  - Only after the move has touched (hit or blocked)
  - [x] Cancelling blocked specials into dashes costs 50% gauge units
  - [x] Cancelling hypers cost 100% gauge units
  - [x] At most three dash cancels per combo (or block string)
- [x] 623LP launches higher and soft knockdowns
- [x] Hadouken (ground and air) is cancellable into specials, hypers and dashes from the moment the shot is out
- [x] 623HP causes a hard knockdown
- [x] Air tatsu (j214K) has no landing recovery
- [x] Air dashes show a flame behind Ken, opposite the dash direction
  - Uses his Shinryuuken fire, turned to point against his motion
- [x] 5HK launches into a soft knockdown, and wall bounces if the opponent reaches the wall

## Akuma

- [x] 6HP
  - [x] Make it cancellable
- [/] 6HK
  - [x] Decrease recovery
  - [x] Normal cancellable
  - [ ] Fix its infinite
    - [ ] Allow only once per combo?
- [/] j236LK
  - [x] Make it jump cancellable
    - [ ] Actions can be taken afterwards even during a normal jump
  - [x] Allow player to control its angle of descent with 4 or 6 after pressing LK
    - [x] j236LK4 goes down in a shallow angle, almost directly below Akuma
    - [x] j236LK6 goes in a wide angle, hitting almost fullscreen
    - [x] Any other direction results in its default trajectory

* [/] j236HK
  - [/] Lower its recovery to make comboing off of it more consistent
  - [x] Allow player to control its angle of descent with 4 or 6 after pressing HK
    - [x] j236HK4 goes down in a shallow angle, almost directly below but further than j236LK4
    - [x] j236HK6 goes in a wide angle, hitting fullscreen
      - [/] Passes through enemies
    - [x] Any other direction results in its default trajectory
* [/] j236PP super
  - [/] Lower its recovery
* [x] 623PP
  - [x] Make it cancellable into supers

## Strider

- [x] j236x
  - Allow Strider to move after the special move
- 236LP
  - Reduce startup
  - Reduce recovery
- 236HP
  - Reduce startup
  - Ground bounce on hit
  - Reduce recovery
- 623LP
  - Reduce startup
- 214LK
  - Massively reduce startup
  - Jump cancellable on block
  - Tracks/homing but does not follow to superjump height
- 214HK
  - Massively reduce recovery
  - Tracks/homing everywhere and as an assist too
- 623HP
  - Full screen
  - Wall bounce on hit
- 421x
  - Reduce recovery
  - LK/HK versions
    - Allow move and attack
    - Turn towards the opponent's direction

# Marvel

## Sentinel

- Fly/unfly
  - Assist calls during unfly mode costs an additional 25% gauge units
  - Fix the unfly glitch into a move that costs meter to maintain
  - Unfly mode is entered by the command 22+KK
  - Add a badge to signify that unfly is currently on
- HP/2HP
  - Fix the last active frame of the lasers being unblockable

## Magneto

- Hyper-grav
  - It shouldn't be techable or the projectiles should be much faster to balance it out
- Magnetic Blast
  - Add 3f airdash cancellable window 2f after the projectile has been launched

## Rogue

- New super that lets her choose which buff she has active: damage, speed
