"""Constants, bitmasks, lookup tables, and physics calculators for mvc2-naomi-dc."""

from __future__ import annotations

import math
from dataclasses import dataclass
from typing import Dict, List, Tuple

# 59 playable / boss characters from include/fighter.h
CHARACTERS: List[Tuple[int, str, str]] = [
    (0x00, "Ryu", "PL_RYU"),
    (0x01, "Zangief", "PL_ZANGIEF"),
    (0x02, "Guile", "PL_GUILE"),
    (0x03, "Morrigan", "PL_MORRIGAN"),
    (0x04, "Anakaris", "PL_ANAKARIS"),
    (0x05, "Strider Hiryu", "PL_STRIDER"),
    (0x06, "Cyclops", "PL_CYCLOPS"),
    (0x07, "Wolverine", "PL_WOLVERINE"),
    (0x08, "Psylocke", "PL_PSYLOCKE"),
    (0x09, "Iceman", "PL_ICEMAN"),
    (0x0A, "Rogue", "PL_ROGUE"),
    (0x0B, "Captain America", "PL_CAPTAIN_AMERICA"),
    (0x0C, "Spider-Man", "PL_SPIDERMAN"),
    (0x0D, "Hulk", "PL_HULK"),
    (0x0E, "Venom", "PL_VENOM"),
    (0x0F, "Doctor Doom", "PL_DOOM"),
    (0x10, "Tron Bonne", "PL_TRON"),
    (0x11, "Jill", "PL_JILL"),
    (0x12, "Hayato", "PL_HAYATO"),
    (0x13, "Ruby Heart", "PL_RUBY_HEART"),
    (0x14, "SonSon", "PL_SONSON"),
    (0x15, "Amingo", "PL_AMINGO"),
    (0x16, "Marrow", "PL_MARROW"),
    (0x17, "Cable", "PL_CABLE"),
    (0x18, "Abyss (Form 1)", "PL_ABYSS1"),
    (0x19, "Abyss (Form 2)", "PL_ABYSS2"),
    (0x1A, "Abyss (Form 3)", "PL_ABYSS3"),
    (0x1B, "Chun-Li", "PL_CHUN_LI"),
    (0x1C, "Mega Man", "PL_MEGAMAN"),
    (0x1D, "Roll", "PL_ROLL"),
    (0x1E, "Akuma", "PL_AKUMA"),
    (0x1F, "B.B. Hood", "PL_BB_HOOD"),
    (0x20, "Felicia", "PL_FELICIA"),
    (0x21, "Charlie", "PL_CHARLIE"),
    (0x22, "Sakura", "PL_SAKURA"),
    (0x23, "Dan", "PL_DAN"),
    (0x24, "Cammy", "PL_CAMMY"),
    (0x25, "Dhalsim", "PL_DHALSIM"),
    (0x26, "M. Bison", "PL_BISON"),
    (0x27, "Ken", "PL_KEN"),
    (0x28, "Gambit", "PL_GAMBIT"),
    (0x29, "Juggernaut", "PL_JUGGERNAUT"),
    (0x2A, "Storm", "PL_STORM"),
    (0x2B, "Sabretooth", "PL_SABRETOOTH"),
    (0x2C, "Magneto", "PL_MAGNETO"),
    (0x2D, "Shuma-Gorath", "PL_SHUMA_GORATH"),
    (0x2E, "War Machine", "PL_WAR_MACHINE"),
    (0x2F, "Silver Samurai", "PL_SILVER_SAMURAI"),
    (0x30, "Omega Red", "PL_OMEGA_RED"),
    (0x31, "Spiral", "PL_SPIRAL"),
    (0x32, "Colossus", "PL_COLOSSUS"),
    (0x33, "Iron Man", "PL_IRON_MAN"),
    (0x34, "Sentinel", "PL_SENTINEL"),
    (0x35, "Blackheart", "PL_BLACKHEART"),
    (0x36, "Thanos", "PL_THANOS"),
    (0x37, "Jin", "PL_JIN"),
    (0x38, "Captain Commando", "PL_CAPTAIN_COMMANDO"),
    (0x39, "Wolverine (Bone)", "PL_BONE_WOLVERINE"),
    (0x3A, "Servbot", "PL_SERVBOT"),
]

CHAR_BY_ID: Dict[int, Tuple[str, str]] = {cid: (name, sym) for cid, name, sym in CHARACTERS}
CHAR_SYM_TO_ID: Dict[str, int] = {sym: cid for cid, _, sym in CHARACTERS}

# Character weights from src/chrdef.c:weight_tbl[59]
WEIGHT_TBL: List[int] = [
    3, 4, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 6, 3, 3, 3, 3, 3, 3,
    3, 3, 3, 3, 7, 7, 7, 2, 3, 3, 3, 3, 3, 3, 3, 3, 2, 3, 3, 3,
    3, 7, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 7, 7, 3, 3, 3, 3, 0,
]

# Animation groups matching include/pl_def.h (indices 0..26 in char_tbl_XX)
ANIM_GROUPS: List[Tuple[int, str, str]] = [
    (0, "Char_Stand", "Standing & Walking"),
    (1, "Char_Jump", "Jumping, Super Jump & Landing"),
    (2, "Char_Dash", "Dashing (Forward / Back / Air)"),
    (3, "Char_Crouch", "Crouching & Standing Up"),
    (4, "Char_Turn", "Turning Around"),
    (5, "Char_Guard", "Blocking (Stand / Crouch)"),
    (6, "Char_Air_Guard", "Air Blocking"),
    (7, "Char_Stand_P", "Standing Punches"),
    (8, "Char_Stand_K", "Standing Kicks"),
    (9, "Char_Crouch_P", "Crouching Punches"),
    (10, "Char_Crouch_K", "Crouching Kicks"),
    (11, "Char_Jump_P", "Jumping Punches"),
    (12, "Char_Jump_K", "Jumping Kicks"),
    (13, "Char_Damage", "Getting Hit, Knockdown & Spinning"),
    (14, "Char_Dizzy", "Dizzy / Stunned"),
    (15, "Char_Catch", "Throws (Attacker)"),
    (16, "Char_Caught", "Being Thrown (Victim)"),
    (17, "Char_Get_Up", "Wakeup & Tech Roll"),
    (18, "Char_Appear", "Intro / Entrance"),
    (19, "Char_Win", "Victory Poses"),
    (20, "Char_Extra", "Launcher, Snapback, Command Normals, Taunt"),
    (21, "Char_Special", "Special Moves"),
    (22, "Char_Super", "Hyper Combos"),
    (23, "Char_Effect", "Projectiles & Effects"),
    (24, "Char_Change", "Tag In / Out / Assist Entry"),
    (25, "Char_Grp25", "Assist / Group 25"),
    (26, "Char_Fly", "Flight Movement"),
]
ANIM_GROUP_MAP: Dict[int, Tuple[str, str]] = {g: (sym, desc) for g, sym, desc in ANIM_GROUPS}
ANIM_GROUP_SYM_TO_ID: Dict[str, int] = {sym: g for g, sym, _ in ANIM_GROUPS}

# Common table slug suffixes inside data/char_XX.c -> group index
TBL_SLUG_TO_GROUP: Dict[str, int] = {
    "stand": 0,
    "jump": 1,
    "dash": 2,
    "crouch": 3,
    "turn": 4,
    "guard": 5,
    "air_guard": 6,
    "stand_p": 7,
    "stand_k": 8,
    "crouch_p": 9,
    "crouch_k": 10,
    "jump_p": 11,
    "jump_k": 12,
    "damage": 13,
    "dizzy": 14,
    "catch": 15,
    "caught": 16,
    "get_up": 17,
    "appear": 18,
    "win": 19,
    "extra": 20,
    "special": 21,
    "super": 22,
    "effect": 23,
    "change": 24,
    "grp25": 25,
    "fly": 26,
}

# Standard normals 0x00..0x11 from include/pl_work.h
STANDARD_ATCK_NAMES: Dict[int, str] = {
    0x00: "ATCK_STAND_LP",
    0x01: "ATCK_STAND_MP",
    0x02: "ATCK_STAND_HP",
    0x03: "ATCK_STAND_LK",
    0x04: "ATCK_STAND_MK",
    0x05: "ATCK_STAND_HK",
    0x06: "ATCK_CROUCH_LP",
    0x07: "ATCK_CROUCH_MP",
    0x08: "ATCK_CROUCH_HP",
    0x09: "ATCK_CROUCH_LK",
    0x0A: "ATCK_CROUCH_MK",
    0x0B: "ATCK_CROUCH_HK",
    0x0C: "ATCK_JUMP_LP",
    0x0D: "ATCK_JUMP_MP",
    0x0E: "ATCK_JUMP_HP",
    0x0F: "ATCK_JUMP_LK",
    0x10: "ATCK_JUMP_MK",
    0x11: "ATCK_JUMP_HK",
}

ATCK_NAME_TO_ID: Dict[str, int] = {name: idx for idx, name in STANDARD_ATCK_NAMES.items()}

# Mapping normal animation (group, anim_idx) -> default atck_no & display name
NORMAL_ANIM_TO_ATCK: Dict[Tuple[int, int], Tuple[int, str]] = {
    (7, 0): (0x00, "Stand LP"),
    (7, 1): (0x01, "Stand MP"),
    (7, 2): (0x02, "Stand HP"),
    (8, 0): (0x03, "Stand LK"),
    (8, 1): (0x04, "Stand MK"),
    (8, 2): (0x05, "Stand HK"),
    (9, 0): (0x06, "Crouch LP"),
    (9, 1): (0x07, "Crouch MP"),
    (9, 2): (0x08, "Crouch HP"),
    (10, 0): (0x09, "Crouch LK"),
    (10, 1): (0x0A, "Crouch MK"),
    (10, 2): (0x0B, "Crouch HK"),
    (11, 0): (0x0C, "Jump LP"),
    (11, 1): (0x0D, "Jump MP"),
    (11, 2): (0x0E, "Jump HP"),
    (11, 3): (0x0C, "Super Jump LP"),
    (11, 4): (0x0D, "Super Jump MP"),
    (11, 5): (0x0E, "Super Jump HP"),
    (12, 0): (0x0F, "Jump LK"),
    (12, 1): (0x10, "Jump MK"),
    (12, 2): (0x11, "Jump HK"),
    (12, 3): (0x0F, "Super Jump LK"),
    (12, 4): (0x10, "Super Jump MK"),
    (12, 5): (0x11, "Super Jump HK"),
}

# Screen dot scaling from include/player.h
DOT_X_SCALE = 5.0 / 3.0       # 1.6666666f
DOT_Y_SCALE = 15.0 / 7.0      # 2.142857f
PUSH_BOX_BASE = 0x7E0 // 8    # 252 (indices 252..255 for STAND/CROUCH/AIR/DOWN)
BOX_MASK = 0x1FFF

# HIT_DATA bitmasks (verified against include/pl_data.h & include/hit_equ.h)
ZU = 0
BU_O = 1
AS = 4
DM_NO_MASK = 0x1F
FLY_SCREEN = 0x20
NO_VEC = 0x40
NO_ZU = 0x80

A_GUARD_MASK = 0x0F
A_NOM = 0x00
A_HI = 0x02
A_LO = 0x04
A_GUARD_NG = 0x08
A_SPECIAL = 0x10
A_HYPER = 0x20
A_CLS_40 = 0x40
A_CLS_80 = 0x80

K_ELEM_MASK = 0x07
K_BACK_MAX = 0x08
K_LV_MASK = 0x70
K_LOW = 0x80
K_S = 0x10
K_M = 0x20
K_L = 0x30
K_LS = 0x90
K_LM = 0xA0
K_LL = 0xB0

NO_CHIP = 0x02
NO_WEIGHT = 0x04
NO_SCALE = 0x08
NO_COMBO = 0x10
NO_KO = 0x20
BLACK_COMBO = 0x40
AERIAL_FINISH = 0x80

# CG_FRAME flags from include/char_tbl.h & include/pl_work.h
END_OF_DATA = 0x80
ATK = 0x80
ATE = 0x40
CG_ATCK_NG = 0x20
CSE = 0x80

# Reaction handler table dm_tbl[4][22] from src/pl_dmg.c
DM_TYPE_NAMES: Dict[int, Tuple[str, str]] = {
    0: ("0: ZU - Ground Pushback", "Standing/crouching pushback; launches (techable) if opponent is airborne"),
    1: ("1: BU_O - Air Launch (Techable)", "Air launch; opponent can air-tech after sandbag_time & ground roll (Soft KD if missed)"),
    2: ("2: dm_buttobi_ng - Soft Knockdown (No Air Tech)", "Untechable in air; opponent can tech-roll upon landing (ukemi_flag=120)"),
    3: ("3: dm_piyo - Dizzy Launch", "High dizzy arc launch into stunned state"),
    4: ("4: AS - Sweep Knockdown", "Leg sweep to ground (or Soft KD vs airborne); opponent can ground roll"),
    5: ("5: dm_hold - Hold Stun", "Stun hold animation in place"),
    6: ("6: dm_hold_air - Air Hold Stun", "Stun hold animation (uses ground/air stance)"),
    7: ("7: dm_07 - Spiral / Spinning Launch (Techable)", "Spinning launch reaction; air techable"),
    8: ("8: dm_08 - Spiral / Spinning Launch (No Air Tech)", "Spinning launch reaction; untechable in air, ground roll allowed"),
    9: ("9: dm_freeze - Freeze / Ice Stun", "Frozen in ice (ground) or frozen falling block (air)"),
    10: ("10: dm_away - Knock Away / Wall Keep", "Long-range horizontal knockback across stage"),
    11: ("11: dm_bound - Ground Bound", "Builtin tumble / bound reaction"),
    12: ("12: dm_snap - Snapback", "Knocks victim off-screen and forces partner tag-in"),
    13: ("13: dm_13 - Ground Pushback (Alias)", "Ground pushback even against air targets"),
    14: ("14: dm_14 - Air Knockback Nop", "Airborne reaction alias to dm_buttobi_move"),
    15: ("15: dm_spark - Electric / Shock Stun", "Electrical shock stun animation"),
    16: ("16: dm_dizzy - Dizzy Crumple", "Crumples directly to floor then wakes up dizzy (stun=80)"),
    17: ("17: dm_slam - Slam (Soft Knockdown, Techable)", "Forces opponent flat onto ground (ukemi roll allowed)"),
    18: ("18: dm_slam_ng - Slam (No Air Tech, Soft KD)", "Forces opponent flat onto ground with recover_ng_flag=1"),
    19: ("19: dm_sandbag - Sandbag", "Freezes opponent in place for sandbag_time (multi-hit lock)"),
    20: ("20: dm_curse - Anakaris Curse", "Transforms victim into cursed puppet/animal"),
    21: ("21: dm_buttobi_ukemi_ng - HARD Knockdown", "Untechable in air AND ukemi_flag=0 (no ground tech roll)"),
}

ELEMENT_NAMES: Dict[int, str] = {
    0: "0: Normal / Physical",
    1: "1: Fire / Burn",
    2: "2: Ice / Freeze",
    3: "3: Electric / Shock",
    4: "4: Element 4",
    5: "5: Continuous Freeze",
    6: "6: Dark / Mystic",
    7: "7: Element 7",
}

GUARD_TYPE_NAMES: Dict[int, str] = {
    0x00: "0: A_NOM (Mid / All)",
    0x02: "2: A_HI (Overhead - Block Standing)",
    0x04: "4: A_LO (Low - Block Crouching)",
    0x08: "8: A_GUARD_NG (Unblockable)",
}

STRENGTH_LEVEL_NAMES: Dict[int, str] = {
    0x00: "0x00: None / Power-based",
    0x10: "0x10: K_S (Light - 11f default blockstun)",
    0x20: "0x20: K_M (Medium - 14f default blockstun)",
    0x30: "0x30: K_L (Heavy - 18f default blockstun)",
}

HIT_MARK_NAMES: Dict[int, str] = {
    0: "0: MARK_N1 (Small Hit Spark)",
    1: "1: MARK_N2 (Medium Hit Spark)",
    2: "2: MARK_N3 (Heavy Hit Spark)",
    3: "3: Special / Heavy Flash Spark",
    4: "4: Slash / Cut Small",
    5: "5: Slash / Cut Large",
    128: "128 (0x80): None",
    255: "255 (0xff): None",
}

HIT_SE_NAMES: Dict[int, str] = {
    0: "0: Default / Silent",
    5: "5: SD_NOR_S (Light Punch Hit)",
    6: "6: SD_NOR_M (Medium Punch Hit)",
    7: "7: SD_NOR_L (Heavy Punch Hit)",
    8: "8: Special / Beam Hit",
    9: "9: SD_KIK_S (Light Kick Hit)",
    10: "10: SD_KIK_M (Medium Kick Hit)",
    11: "11: SD_KIK_L (Heavy Kick Hit)",
    12: "12: Hard Knockdown / Explosion",
}

BOUNCE_PROP_NAMES: List[Tuple[int, str, str]] = [
    (0, "BOUNCE_PROP_NONE", "None (Vanilla MvC2)"),
    (1, "BOUNCE_PROP_GROUND", "Ground Bounce (1 per combo)"),
    (2, "BOUNCE_PROP_GROUND_FORCED", "Ground Bounce (Forced / Always)"),
    (3, "BOUNCE_PROP_WALL", "Wall Bounce (1 per combo)"),
    (4, "BOUNCE_PROP_WALL_FORCED", "Wall Bounce (Forced / Always)"),
]

# Physics tables from src/chrdef.c
DM_POW_TBL: List[int] = [
    400, 500, 600, 700, 800, 900, 1000, 1150,
    1300, 1500, 1700, 2000, 2500, 3000, 3500, 4000,
]

BACK_SPD_TBL: List[int] = [
    -1280, -1536, -1792, -2048, -2304, -2560, -2816, -3072,
    -3328, -3584, -3840, -4096, -4352, -4608, -4864, -5120,
]

BACK_STEP_TBL: List[List[int]] = [
    [6, 5, 4, 4, 3, 2, 2, 1, 1, 1, 1, -1],
    [6, 6, 6, 5, 5, 5, 4, 4, 3, 3, 2, 1, 1, 1, 1, 1, -1, -1],
    [8, 8, 8, 7, 7, 7, 6, 5, 5, 4, 3, 2, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1],
    [10, 10, 10, 9, 9, 9, 8, 7, 7, 6, 5, 4, 3, 3, 3, 3, 1, 1, 1, 1, -1, -1],
    [11, 11, 9, 9, 7, 7, 5, 5, 2, 2, 0, 0, -1, -1],
]


@dataclass
class MovePhysicsSummary:
    raw_damage: int
    chip_damage: int
    chip_reason: str
    blockstun_frames: int
    blockstun_is_default: bool
    block_pushback_dots: int
    block_pushback_tier: int
    ground_pushback_speed_dots: float
    ground_pushback_total_dots: float
    launch_angle_deg: float
    launch_vx_dots: float
    launch_vy_dots: float
    launch_peak_height_dots: float
    launch_air_time_frames: float
    launch_horiz_distance_dots: float
    knockdown_label: str
    hit_meter_attacker: int
    hit_meter_defender: int
    hit_meter_note: str


def compute_move_physics(
    damage: int,
    damage_type: int,
    atck_kind: int,
    damage_kind: int,
    vector: int,
    power: int,
    guard_time: int,
    shc: int,
    hit_flag: int,
    whiff_gauge: int | None = None,
) -> MovePhysicsSummary:
    """Compute human-readable hit/block physics, chip, meter, and KD summary from HIT_DATA fields."""
    # Chip damage calculation (src/pl_dmg.c:guard_ef_set lines 1040-1051)
    can_chip_by_class = bool(atck_kind & 0xB0)  # A_SPECIAL (0x10), A_HYPER (0x20), or 0x80
    no_chip_flag = bool(hit_flag & NO_CHIP)
    if damage <= 0:
        chip_damage = 0
        chip_reason = "0 (no base damage)"
    elif no_chip_flag:
        chip_damage = 0
        chip_reason = "0 (NO_CHIP set)"
    elif can_chip_by_class:
        chip_damage = max(1, int(damage / 8.0))
        chip_reason = f"{chip_damage} (1/8 of {damage}; ATR_CHIP N/A)"
    else:
        chip_damage = 0
        chip_reason = f"0 (needs A_SPECIAL/A_HYPER or ATR_CHIP; would be {max(1, int(damage / 8.0))})"

    # Blockstun calculation (src/pl_dmg.c:guard_char_set lines 549-556)
    if guard_time > 0:
        blockstun = guard_time
        blockstun_is_default = False
    else:
        blockstun_is_default = True
        if not (atck_kind & 0xF0) and (damage_kind & K_LV_MASK):
            lv = damage_kind & K_LV_MASK
            if lv == 32:
                blockstun = 14
            elif lv == 16:
                blockstun = 11
            else:
                blockstun = 18
        else:
            blockstun = 18

    # Block / ground pushback step table (src/pl_dmg.c:guard_back_set lines 723-731)
    if damage_kind & (K_BACK_MAX | K_LV_MASK):
        if damage_kind & K_BACK_MAX:
            step_no = 4
        else:
            step_no = min(4, max(0, ((damage_kind & K_LV_MASK) >> 4) - 1))
    else:
        step_no = 0
        if power >= 5:
            step_no += 1
        if power >= 12:
            step_no += 1
    block_pushback_dots = sum(v for v in BACK_STEP_TBL[step_no] if v > 0)

    # Slide pushback speed/distance from back_spd_tbl[power] (decel = 1.9791666f / DOT_X = 1.1875 dots/f^2)
    pow_idx = power & 15
    v0_slide = abs(BACK_SPD_TBL[pow_idx]) / 256.0
    slide_decel = 1.9791666 / DOT_X_SCALE
    t_slide = v0_slide / slide_decel
    ground_pushback_total = max(float(block_pushback_dots), 0.5 * v0_slide * t_slide)

    # Launch vector & speed from dm_spd_get (src/pl_dmg.c lines 1866-1873)
    dir_step = vector & 31
    ang_idx = (40 - dir_step) & 31
    ang_rad = ang_idx * (2.0 * math.pi / 32.0)
    ang_deg = ang_idx * 11.25
    raw_pow = DM_POW_TBL[pow_idx]
    vx_dots = -(raw_pow * math.cos(ang_rad) / 150.0)  # Away from attacker (positive = away horizontally here for magnitude display)
    # Wait: let's keep exact signed convention: in dm_spd_get, spd.x = cos(ang), where 180 deg (-X) is pushback when P1 faces right.
    signed_vx_dots = raw_pow * math.cos(ang_rad) / 150.0
    vy_dots = raw_pow * math.sin(ang_rad) / 125.0  # +Y is UP in world space
    g_dots = 0.375  # -DOT_Y(0.375f) in world units -> 0.375 CPS2 dots/frame^2
    if vy_dots > 0:
        t_peak = vy_dots / g_dots
        peak_h = (vy_dots * vy_dots) / (2.0 * g_dots)
        air_time = 2.0 * t_peak
        horiz_dist = abs(signed_vx_dots) * air_time
    else:
        peak_h = 0.0
        air_time = 0.0
        horiz_dist = 0.0

    # Knockdown & reaction summary
    dm_no = damage_type & DM_NO_MASK
    kd_parts: List[str] = []
    if dm_no == 0:
        kd_parts.append("Ground Hitstun (Launches if Airborne)")
    elif dm_no == 1:
        kd_parts.append("Air Launch (Air-Techable)")
    elif dm_no == 2:
        kd_parts.append("Soft Knockdown (No Air Tech, Ground Roll Allowed)")
    elif dm_no == 3:
        kd_parts.append("Dizzy Launch")
    elif dm_no == 4:
        kd_parts.append("Sweep Knockdown (Soft KD, Ground Roll Allowed)")
    elif dm_no in (5, 6):
        kd_parts.append("Hold Stun")
    elif dm_no == 7:
        kd_parts.append("Spiral Launch (Air-Techable)")
    elif dm_no == 8:
        kd_parts.append("Spiral Launch (Soft KD, No Air Tech)")
    elif dm_no == 9:
        kd_parts.append("Ice Freeze Stun")
    elif dm_no == 10:
        kd_parts.append("Knock Away (Wall Carry)")
    elif dm_no == 11:
        kd_parts.append("Builtin Ground Bound")
    elif dm_no == 12:
        kd_parts.append("Snapback (Forces Tag-Out)")
    elif dm_no == 15:
        kd_parts.append("Electric / Spark Stun")
    elif dm_no == 16:
        kd_parts.append("Dizzy Crumple")
    elif dm_no == 17:
        kd_parts.append("Slam (Soft Knockdown, Ground Roll Allowed)")
    elif dm_no == 18:
        kd_parts.append("Slam (No Air Tech, Soft KD)")
    elif dm_no == 19:
        kd_parts.append("Sandbag Freeze")
    elif dm_no == 20:
        kd_parts.append("Anakaris Curse")
    elif dm_no == 21:
        kd_parts.append("HARD Knockdown (No Air Tech, No Ground Roll)")
    else:
        kd_parts.append(f"Reaction dm_no={dm_no}")

    if hit_flag & BLACK_COMBO:
        kd_parts.append("Launcher (SJ Cancel)")
    if hit_flag & AERIAL_FINISH:
        kd_parts.append("Air Finisher")
    if damage_type & FLY_SCREEN:
        kd_parts.append("Fly Screen")
    if damage_type & NO_VEC:
        kd_parts.append("Fixed Trajectory (NO_VEC)")
    if damage_type & NO_ZU:
        kd_parts.append("No Corner Pushback (NO_ZU)")

    # Gauge generation (src/pl_dmg.c:dm_gauge_add lines 674-706; 144 = 1 bar)
    if atck_kind & A_HYPER:
        hit_def = 1
        hit_atk = 0
        meter_note = "Hyper: +0 attacker (+1 defender); 144 = 1 bar"
    else:
        base_g = shc & 63
        if base_g == 0:
            if (atck_kind & 0x40) or (atck_kind & A_SPECIAL):
                base_g = 8
            elif whiff_gauge is not None:
                base_g = whiff_gauge
            else:
                base_g = 2  # fallback L normal estimate
        hit_def = base_g
        if shc & 0x40:
            hit_atk = 0
            meter_note = f"shc 0x40 set: +0 attacker (+{hit_def} defender)"
        else:
            hit_atk = base_g
            meter_note = f"Base +{hit_atk}/144 bar (scaled by combo_cnt & gauge_rate_tbl)"

    return MovePhysicsSummary(
        raw_damage=damage,
        chip_damage=chip_damage,
        chip_reason=chip_reason,
        blockstun_frames=blockstun,
        blockstun_is_default=blockstun_is_default,
        block_pushback_dots=block_pushback_dots,
        block_pushback_tier=step_no,
        ground_pushback_speed_dots=v0_slide,
        ground_pushback_total_dots=ground_pushback_total,
        launch_angle_deg=ang_deg,
        launch_vx_dots=signed_vx_dots,
        launch_vy_dots=vy_dots,
        launch_peak_height_dots=peak_h,
        launch_air_time_frames=air_time,
        launch_horiz_distance_dots=horiz_dist,
        knockdown_label=" | ".join(kd_parts),
        hit_meter_attacker=hit_atk,
        hit_meter_defender=hit_def,
        hit_meter_note=meter_note,
    )
