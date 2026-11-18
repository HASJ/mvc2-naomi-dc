Checklists marked with...

- `[/]` are insatisfactory and need further refinement
- `[]` are currently open
- `[x]` mean they are completed
- `[-]` mean they are not a priority right now

# Conventions

- This document uses numerical/numpad notations
- A1 = Assist 1
- A2 = Assist 2
- AS = Assist 1 or Assist 2

# Intent

- Buff rather than nerf
- Unique characteristics shouldn't be nerfed, such as Sentinel's lasers doing chip damage, unless they cause a game breaking bug
- Remove infinites, preference is for dynamic combos
  - Tentatively, loops shouldn't repeat more than three times, albeit there could be exceptions

# Game

- [] Color/palette editor
  - Check feasibility of editing special moves' effects
- [] Update the text textures
- [] Test texture filters
  - [] Bilinear
  - [] Cubic
  - [] xBR
  - [] NNEDI3
  - [] ArtCNN
- Higher resolution rendering of 3D elements
- 16:9 Aspect ratio support
- [] Classic/Ultimate ruleset toggle
  - Support for the unmodded game will be added eventually
  - [] Ranked and tournaments allow Classic, kept separate from Ultimate: separate queue, ratings, leaderboards and tournament records
- [] VS CPU with a chosen opponent and difficulty
- [] Controls follow the player
  - Button layout is saved to the account and loads on login with the 6-input arcade password, so tournament players get their layout on any setup
  - [] SOCD settings for leverless controllers

# Netplay

- [] Training mode waiting room
  - Places a Ranked or Casual queue once started, player selects
- [] Matchmaking
  - Goal: fast matches with a small player base. One queue, few hard filters, and a search that widens quickly
  - [] Fix: a pending match that one player never accepts stays pending forever (`tryMatchQueueLocked` in `netplay-server/internal/lobby/lobby.go`)
    - Pending matches time out after 5 seconds, and the player who accepted goes back into the queue with their original join time
  - [] Replace the accept button with a ping probe
    - Both clients hole-punch and ping each other for about a second
    - If the ping is within both players' filters, the match starts with no button press
    - If not, both players requeue and that pair is skipped for a few minutes
  - [] One central matcher every 250 ms instead of one per connected client
    - Scores every candidate pair on rating gap, ping and wait time, and pairs the best first
    - Can form several matches per tick
  - [] Search widens over time on rating and ping
  - [] Matchmaking uses ping, not region. Regional divisions are only for points and leaderboards
  - [] Rematches reuse the open connection instead of searching again
  - [] Found player info
    - [] If Ranked match, player name displayed only after entering the Versus/Loading screen
      - The server's `match_found` message currently sends both names right away
    - [] Ethernet / Wi-Fi detector
    - [] Ping display
      - 5 tiers
        - Ping displayed in colors from green to red
        1. 0-64 ms (Optimal)
        2. 65-120 ms (Great)
        3. 121-180 ms (Good)
        4. 181-230 ms (Risky/Depends)
        5. +231 ms (More often than not, it'll be a bad experience)
      - [] Colorblind-safe: show the ping number or a bar count next to the color
- [] Ranked match format
  - First to 3 wins
  - [] Between games, players choose:
    - Rematch
    - The loser can change characters
    - The winner can only change their selected assist types
  - [] Once the set has a winner, if both players select rematch, they are moved to a casual lobby
  - [] Disconnects and rage-quits
    - [] The player who stopped responding gets the loss
    - [] Repeat quitters get increasing timeouts
      | Tier | Timeout | Disconnects allowed before the next tier | Clean time per stacked disconnect to reset to no timeout (7x the timeout) |
      |---|---|---|---|
      | 1 | 5 minutes | 4 | 35 minutes |
      | 2 | 10 minutes | 3 | 70 minutes |
      | 3 | 30 minutes | 2 | 3.5 hours |
      | 4 | 1 hour | 1 | 7 hours |
      | 5 | 3 hours | 1 | 21 hours |
      | 6 | 6 hours | 1 | 42 hours |
      | 7 | 12 hours | 1 | 3.5 days |
      | 8 | 24 hours | - | 7 days |
      - Disconnects stack inside a tier and reset the clean-time counter: with 1 minute left to erase one 5-minute disconnect, a second disconnect means 2 x 35 = 70 minutes of clean time are now needed
      - Once the clean time is reached, the player goes straight back to no timeout, not down one tier
      - Reaching the 24-hour tier takes 7 days to go back to no timeout
      - [] Badges while on a timeout tier
        - Tier 2: Unstable Connection
        - Tier 3 and above: Ragequitter
    - [] Connection broken between the players while both still reach the server
      - No result and no penalty, unless it repeats
      - After 3 such breaks within a rolling 2-day window, the player who is the common denominator across the different opponents is assumed to be doing it on purpose, and the breaks count as disconnects for that player
    - [] Desyncs
      - [] Detected by comparing the game state hash on both sides
      - [] Both players are asked whether they want a rematch
        - If both say yes, the set continues from where it stopped with the same score (for example, game 2 of a ranked first to 3)
        - If not, both players return to the netplay menu and the set counts as a no contest, with no effect on either player's rating
- [] Patch distribution
  - The server only matches players on the same patch, so every patch splits the queue until everyone updates
  - [] Auto-updater, or at least a "new patch available" prompt
  - [] In-game patch notes
- [] Blocking and reporting
  - [] Players can mute, block and report other players
  - [] Blocked players are never matched with each other
- [x] Lobbies
  - [] Ranked and Casual
  - [] Should show all public available lobbies
  - [] Split between Ranked and Casual
  - [] Sorted by ping (ascending order, lower ping shows first)
  - [] Can be filtered by Rank
  - [] Player Rank is only displayed once the match has already started
  - [x] Private lobbies
- [/] Leaderboards/ladder
  - [] Top 10 challenges (ranks 14-17, per regional division)
    - Players in ranks 14-17 must defend their position at least once every 7 days, otherwise they are moved to rank 13
      - A week with no challenges counts as defended
    - Rank 13 players can challenge a rank 14 player once a week
    - Rank 14 players can challenge a player in ranks 15, 16 or 17 once a week
    - Players in ranks 14-17 can decline a given player's challenge only once per month
    - If the rank 17 player loses to a rank 14 challenger, they are knocked down to rank 16, and so on down the ladder
    - When a rank 13 challenger beats a rank 14 player, the two players swap places
    - A challenger who loses gets a 💢 next to their player name for two days
  - [] Placement for new players
    - Rating uses Glicko-2 as the server already does (start at 1500, rating deviation 350)
    - The rank is hidden for the first 10 sets, shown as "Placement 1/10" and so on
    - The first rank shown is capped at rank 9
  - Regional divisions
    - Players can change regions
      - [] One region change every 30 days, so players can't move to a weak region to take ranks 14-17
    - Regions have separate points
    - North America
      - United States
        - East Coast
        - Middle America
        - West Coast
    - Central America
    - South America
      - Brazil
        - South
        - Southeast
        - North
        - Northeast
        - Northwest
    - North Africa
    - South Africa
    - Europe
    - Asia
    - Russia
    - China
    - South-east Asia
    - Middle-east
    - Oceania
  - [] 17 ranks
    - Every rank has a Capcom name and a Marvel name, and the player chooses which franchise's names their rank shows
      - Everyone, including opponents, sees a player's rank under the franchise that player chose
    - Names climb from street level to cosmic
    - Rank 14 is reserved for places 4-10 of each regional division
    - Ranks 15-17 are reserved for the top three players of each regional division and tell the series' origin story
    1. Rank 1
       - Capcom: Saikyo-ryu Student (Dan's style)
       - Marvel: Great Lakes Avenger (Marvel's joke team)
    2. Rank 2
       - Capcom: Servbot (Mega Man Legends)
       - Marvel: Doombot (Doctor Doom)
    3. Rank 3
       - Capcom: Shadaloo Soldier (Street Fighter)
       - Marvel: Hand Ninja (Daredevil, Wolverine)
    4. Rank 4
       - Capcom: Mad Gear Brawler (Final Fight)
       - Marvel: Hell's Kitchen Brawler (Daredevil)
    5. Rank 5
       - Capcom: Bushinryu Disciple (Guy's school)
       - Marvel: Danger Room Recruit (X-Men)
    6. Rank 6
       - Capcom: Street Fighter (Street Fighter)
       - Marvel: Hero for Hire (Luke Cage and Iron Fist)
    7. Rank 7
       - Capcom: Tyrant (Resident Evil)
       - Marvel: Weapon X (Wolverine)
    8. Rank 8
       - Capcom: Darkhunter (Darkstalkers, Donovan's title)
       - Marvel: Midnight Son (Blade, Ghost Rider)
    9. Rank 9
       - Capcom: Commando (Captain Commando)
       - Marvel: Avenger (Avengers)
    10. Rank 10
        - Capcom: Special A-Class Strider (Strider)
        - Marvel: Level 10 S.H.I.E.L.D. Agent (Nick Fury's clearance)
    11. Rank 11
        - Capcom: Dark Messiah (Darkstalkers, Jedah's title)
        - Marvel: Herald of Galactus (Silver Surfer)
    12. Rank 12
        - Capcom: Raging Demon (Street Fighter)
        - Marvel: Sorcerer Supreme (Doctor Strange)
    13. Rank 13
        - Capcom: Abyss (MvC2's final boss)
        - Marvel: Phoenix Force (X-Men)
    14. Rank 14, places 4-13 of the region
        - Capcom: New Age Hero (Marvel vs. Capcom 2: New Age of Heroes)
        - Marvel: Punisher (Capcom's 1993 The Punisher beat 'em up)
    15. Rank 15, 3rd place of the region
        - Capcom: Night Warrior (Night Warriors: Darkstalkers' Revenge)
        - Marvel: Child of the Atom (X-Men: Children of the Atom)
    16. Rank 16, 2nd place of the region
        - Capcom: World Warrior (Street Fighter II: The World Warrior)
        - Marvel: Super Hero (Marvel Super Heroes)
    17. Rank 17, 1st place of the region
        - Capcom: Ansatsuken Master (Street Fighter)
        - Marvel: X-Man (X-Men vs. Street Fighter)

      - Each rank has 5 subdivisions (except ranks 14-17)
        1. Entry
        2. Mainstay
        3. Going Up
        4. No Looking Back
        5. Promotion (invisible, this is used to more frequently match up players in the same subdivision but about to be promoted)
- [x] Rollback
  - [x] Replays
    - [x] Patch retrocompatibility
    - [x] Replay takeover
    - [x] Replay database
      - [x] Match search
        - [x] Filter by characters, who won, player, rating, assist, patch

# Character Select

- [x] Faster start
- [x] Faster menus
- [x] Faster transitions
- [x] Faster assist select menu animation
- [x] After selecting a character holding a button automatically selects the assist
  - LP = Alpha assist
  - LK = Beta assist
  - HP = Gamma assist
- [/] Display the highlighted character standing animation instead of a frozen sprite
- [x] Display chosen character palette
- [x] Ability to undo a character select with the LK button from the assist select menu
- [-] Team presets

# System

- [/] Add ground and wall bounces
  - [x] Bounces are triggered by specific moves or specific hits in certain conditions
  - Needs refinement
    - Define what "refinement" is with a proper plan of action
      - [/] Animation and sound effects are lacking
- [-] Combos cannot kill a character if the combo started with the opponent at +33% health
  - Only Heavy characters should be able to bypass this rule
    - Juggernaut
    - Hulk
    - Zangief
    - Blackheart
    - Colossus
    - Thanos

- [x] Allow superjump to be executed with LK+HK
  - [x] It should share the new dash leniency
- [x] Make airdashes (LP+HP) more lenient and consistent
- [x] Make the `F` key into a dash button. Not a LP+HP macro but straight Dash button.
- [x] Make the `V` key into a super jump button. Not a LK+HK macro but straight Super Jump button
- [x] Super Hop
  - [x] Control superjump height by pressing or holding `1`, `2` or `3` while rising, easing the rise to a stop over 5 frames while keeping superjump maneuverability
  - [x] Works on air jumps and wall jumps chained from a superjump
- [x] Allow calling both assists in one combo
- [x] Allow calling both assists at the same time but increase their cooldown
- [x] Allow calling assists after a superjump during a combo
- [] Remove unblockable incoming setups/guard breaks
  - Cause is assumed to be that the player is only allowed to block once per normal jump, so if the blockstun ends they cannot block again until touching the ground
- [-] Increase damage scaling
  - Touches of Death should not happen unless the player spends a lot of resources
- [/] Active tag
  - [x] Pressing an assist button while the assist character is on the screen cancels whatever that character is doing, cosmetically the guard cancel effect appears on the character that is being tagged in and switches control to that character
  - [x] Once the previously on point character goes into neutral state, it will be vulnerable and stay on the screen for 2 seconds then leave
  - [x] Characters that were actively tagged out will remain on lockout for 2 more seconds, they can be called one more time as assists if the opponent is still in a combo, but cannot be actively tagged back in
  - [x] If both assists were called, only one can be actively tagged in
  - [x] If still in a combo and the unused assist is called, they can be actively tagged into
  - [x] Costs 50% gauge units
- Assists
  - [x] Allow assist calls during special moves
    - Costs 25% gauge units
  - [x] Allow assist calls during super jump
    - Costs 25% gauge units
  - [x] Allow assist calls during flying screen
  - [x] Extra assists
    - [x] Characters can use all their assists besides the one selected
    - [x] Up+Assist or Down+Assist determine which extra assist is getting called
    - [x] Extra assists costs 25% gauge units
    - Any other input is just the selected assist that do not cost gauge units
- [x] Counter-tag
  - Costs 250% gauge units
  - [x] Activated by tagging while the point character is getting hit
  - [x] The point character is still vulnerable and it is up to the player to protect them
  - [x] Each team member has one counter-tag available per match
  - [x] On activation, the game's colors are inverted and return to normal once the tagged partner is controllable
  - [x] Characters can come from three positions:
    - Ground (DownBack + Tag)
    - Incoming height (Back + Tag)
    - Superjump height (UpBack + Tag)
- [-] Pushblock
  - Changes deferred

# Training Mode

Main purpose: balance tuning for the mod. Learning tools for players come later.

- Native options (Dreamcast training menu, `src/pl_sw.c`): dummy mode, stance, guard, air recovery, dummy change, damage display, delayed hyper free, mash level, damage level, game speed and auto gauge
- [] Game speed
  - 10% to 100% in 10% steps, alongside the native Normal, Turbo and Turbo 2
  - Native speeds are set in `vs_rule_set` (`src/vs_sys.c`)
  - [] Frame advance: pause and step one frame at a time
- [] Save states
  - [] Multiple slots
  - [] Position reset: left corner, center, right corner and swap sides
  - [] Load a save state from any replay frame
- [] Recordings
  - [] 6+ slots
  - [] Playback in a loop, at random or weighted
  - [] Playback triggers: on block, on hit, on wakeup and on tech
  - [] Record assist calls, active tags and DHCs
  - [] Export and import recording files
- [] Dummy
  - [] Guard like a human: hold one direction, so unblockable setups can be tested
  - [] Air and ground tech direction
  - [] Incoming character option: block, roll or attack
  - [] Pushblock and Variable Counter
  - [] Call assists: which partner, which type (α/β/γ), and on block, on hit or at random
- [] Displays
  - [] Hitbox and hurtbox viewer, also in replays
  - [] Frame meter, with one row per on-screen assist and super freezes marked
  - [] Input display with a frame count per input
  - [] Readouts: combo damage, damage scaling, red health, assist cooldowns and gauge costs
    - [] Mod rules: anti-TOD floor active, bounces used, active-tag and counter-tag lockouts, counter-tags left
- [] Character-specific settings
  - Ruby Heart's γ buff: type, level and timer
  - Sentinel's unfly mode on or off
  - Rogue's stolen buff
  - Strider's Ouroboros
  - Wolverine's Berserker Charge
  - Juggernaut's Power Up
  - Silver Samurai's install hyper
  - Zangief's Mech mode
  - Colossus's Armored mode
  - Spiral's install hyper
- [] Simulated input delay, matching the ping tiers
- [] Online co-op training in private lobbies
- [-] Combo trials, lessons and missions
  - Stored as data files, so the community can write, record from training mode and share them
  - To be uploaded, trials have to be validated by manual execution before being accepted.
- [-] Move list with frame data
- Not planned: AI ghost opponents, fully produced tutorials and Modern controls

# Telemetry

Purpose: understand trends and catch bugs and exploits that escape testing. The data must make sense on its own or be readable by an AI model.

- Covers online matches and local versus
- [] Telemetry comes from replays, not from logging during play
  - A replay analyzer re-runs each `.mvc2r` replay headless under its patch's game DLL and writes the events
  - New detectors can be run on old matches
- [] Local versus matches save a replay and upload it
  - Local versus has no logged-in players, so these matches feed trends but not ratings or match history
  - [] Opt-out: uploads are on by default and can be turned off in the options menu
  - [] Without an internet connection, replays are kept and uploaded silently once a connection is available, with no prompts or error messages
- [] Client-only reports: crashes, desyncs (state hash mismatch), stub hits from `mvc2.log`, ping and rollback counts, input device type
- [] Event format
  - One JSON object per line
  - Names instead of IDs (`Sentinel 2HP`, not `0x1a/0x23`)
  - Every event is stamped with its patch
  - [] Data dictionary describing every field
- [] Events: hits, combos, damage, assist calls, active tags, counter-tags, bounces, anti-TOD triggers, deaths and timeouts
- [] Exploit flags
  - The same move sequence repeating (infinites)
  - A hit landing while the defender held the correct guard direction (unblockables)
  - Very high damage for very few resources
  - A character stuck in one state too long, or out of bounds

# Tournament Mode

Purpose: keep track of tournament matches for posterity.

- [] Before each match, the game asks for:
  - Tournament name
  - Stage of the match: pools, top 128, top 64, top 32, top 24, top 16, top 8, winner's quarters, loser's quarters, winner's semis, loser's semis, winner's finals, loser's finals, grand finals
  - Player names, for offline matches
    - Players can log in with their 6-input arcade password, so placements reach their account
    - Players who don't log in play as guests under their typed name and can't earn badges
- [] Date and time are assigned automatically
- [] Stream output: player names, scores and stage written to text files that OBS can read
- [] Every match saves a replay tagged with the tournament, stage and players
  - [] Replays are stored and uploaded once a connection is available, since many venues have no reliable internet
  - [] Replay database search can filter by tournament
- [] Placements are worked out from the stages (double elimination)
  - 1st: grand finals winner
  - 2nd: grand finals loser
  - 3rd: loser's finals loser
  - Top 8: anyone who played a winner's/loser's quarters or later stage
- [] Badges
  - Awarded for top 8, 3rd, 2nd and 1st placements, and last 5 days
  - Other players can see which tournament a badge came from and find its matches
  - The badge shown is the player's highest placement
  - If a player has several concurrent placements, their profile lists the other tournaments
- [] Badge trust checks
  - [] Replay check: suspicious matches are flagged and don't count
    - Players losing without fighting, near-zero or apathetic inputs, never blocking, walking into attacks, rounds ending too fast to be real
    - Uses the telemetry replay analyzer
  - [] Size and consistency check
    - A tournament awards 1st, 2nd and 3rd badges only if at least 8 distinct accounts played in it
    - Top 8 badges need at least 12 distinct accounts, so not every entrant of a small tournament gets one
    - Those accounts need some ranked match history, so fresh alt accounts don't count
    - Stages must follow a valid bracket order: no grand finals without a loser's finals before it
  - [] Optional start.gg or Challonge link
    - Both have public APIs (start.gg GraphQL, Challonge REST)
    - Fills in the stage and player names from the bracket and reads official placements
    - Badges from linked tournaments get a "verified" mark
