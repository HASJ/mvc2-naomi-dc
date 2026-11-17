"""Lightweight hover tooltips for Tkinter / ttk widgets."""

from __future__ import annotations

import tkinter as tk
from typing import Dict, Optional


class ToolTip:
    """Shows a delayed, readable tooltip popup when hovering over a widget."""

    def __init__(self, widget: tk.Widget, text: str, delay_ms: int = 400, wrap_px: int = 340) -> None:
        self.widget = widget
        self.text = text
        self.delay_ms = delay_ms
        self.wrap_px = wrap_px
        self._after_id: Optional[str] = None
        self._win: Optional[tk.Toplevel] = None
        widget.bind("<Enter>", self._schedule, add="+")
        widget.bind("<Leave>", self._hide, add="+")
        widget.bind("<ButtonPress>", self._hide, add="+")

    def set_text(self, text: str) -> None:
        self.text = text

    def _schedule(self, _evt=None) -> None:
        self._cancel()
        if not self.text:
            return
        self._after_id = self.widget.after(self.delay_ms, self._show)

    def _cancel(self) -> None:
        if self._after_id is not None:
            try:
                self.widget.after_cancel(self._after_id)
            except Exception:
                pass
            self._after_id = None

    def _show(self) -> None:
        if self._win is not None or not self.text:
            return
        try:
            x = self.widget.winfo_rootx() + 14
            y = self.widget.winfo_rooty() + self.widget.winfo_height() + 4
        except Exception:
            return
        win = tk.Toplevel(self.widget)
        win.wm_overrideredirect(True)
        win.wm_geometry(f"+{x}+{y}")
        try:
            win.attributes("-topmost", True)
        except Exception:
            pass
        lbl = tk.Label(
            win,
            text=self.text,
            justify="left",
            wraplength=self.wrap_px,
            background="#ffffe6",
            foreground="#1a1a1a",
            relief="solid",
            borderwidth=1,
            padx=6,
            pady=4,
            font=("Segoe UI", 9),
        )
        lbl.pack()
        self._win = win

    def _hide(self, _evt=None) -> None:
        self._cancel()
        if self._win is not None:
            try:
                self._win.destroy()
            except Exception:
                pass
            self._win = None


def attach_tooltip(widget: tk.Widget, text: str) -> ToolTip:
    return ToolTip(widget, text)


# Human-readable explanations for all editor fields and controls
FIELD_TOOLTIPS: Dict[str, str] = {
    # Top navigation
    "char": "Select the character (00..3A) to load its animations, hitboxes, and move hit data.",
    "palette": "Preview palette (Color 1-6) used when rendering character sprites on the canvas.",
    "category": "Filter the Action / State dropdown by animation group (e.g. Standing Punches, Special Moves, Projectiles).",
    "action": "Select a specific animation sequence / state ([Group:Index] Name). Use the Move dropdown to see all states that belong to the same special/hyper move.",
    "move": "Group special moves, hypers, and their projectiles/shadow clones into a single named move (e.g. Vajra, Warp, Formation A).",
    "version": "Select button strength (Light LP/LK vs Heavy HP/HK, or Punch vs Kick). Updates the linked Move Hit Data and highlights which states/timers belong to that version.",
    "sync_toggle": "When ON, selecting an Action/State automatically selects its matching Move Hit Data, and selecting Move Hit Data jumps to its primary Action/State.",
    "atck": "Select the HIT_DATA entry (damage, stun, knockback, guard type, meter gain, etc.) to inspect or edit.",
    "undo": "Undo the last edit to animations, hitboxes, move hit data, bounce rules, or whiff meter (Ctrl+Z).",
    "redo": "Redo the last undone edit (Ctrl+Shift+Z).",
    "discard_action": "Revert all unsaved changes made to the currently selected Action/State (its frames, its used FrameBoxes/HitBoxes, and its linked Hit Data) back to the last saved state. This action itself can be undone with Ctrl+Z.",
    "test_in_game": "Temporarily patch PLXX_DAT.BIN in-memory/temp and launch mvc2.exe to test your unsaved animation, hitbox, and Move Hit Data changes without saving to C files. Automatically restores PLXX_DAT.BIN when the game closes.",

    # Viewer & Hitbox editor
    "zoom": "Canvas zoom multiplier (1x..8x).",
    "aspect_43": "Scale vertically by MvC2's native 4:3 arcade pixel aspect ratio (DOT_Y_SCALE / DOT_X_SCALE).",
    "face_right": "Mirror sprite and boxes to face right (Player 1 side).",
    "traj": "Show predicted launch/knockback velocity vector and parabolic trajectory arc.",
    "ai_range": "Show AI range detection boxes (range_box / range_box2) if defined on the current Hit Data.",
    "clone_shared": "When ON, editing a FrameBox or HitBox that is shared by multiple animations automatically clones it first so other moves are not accidentally modified.",
    "hb_slot": "Collision box slot on the current frame: atck_box (red), atck_box2 (orange/purple throw), body_box0..3 (blue hurtboxes), catch_box (green throw hurtbox), push_box (yellow).",
    "hb_id": "Index into the character's HIT_BOX table (0 = none, 252..255 = pushboxes).",
    "hb_throw": "Bit 0x8000 on atck_box2: marks this box as a Throw Range Box instead of a normal secondary attack box.",
    "hb_body3": "Collision type bits (0x6000) on body_box3 (Type 4..7, used for special hurtbox flags like projectile invulnerability).",
    "hb_x": "Box center X offset in dots from character origin (negative = in front of character).",
    "hb_w": "Box half-width in dots (total width = 2 * Half-W).",
    "hb_y": "Box center Y offset in dots from ground origin (negative = above ground, except push_box).",
    "hb_h": "Box half-height in dots (total height = 2 * Half-H).",
    "hb_push": "Select which global pushbox stance (252 Stand, 253 Crouch, 254 Air, 255 Down) to view/edit.",

    # Frame editor
    "fr_ctr": "Duration of this animation step in game frames (ticks at 60fps). -1 (255) = hold frame indefinitely until code advances.",
    "fr_spr": "Sprite index inside PLXX_DAT.BIN (-1 = invisible / no sprite).",
    "fr_box": "FrameBox index defining all hitboxes/hurtboxes active during this frame.",
    "fr_atk": "ATK flag (0x80): marks this frame as an active attack frame in animation data.",
    "fr_ate": "ATE flag (0x40): marks recovery / end of active attack window.",
    "fr_ng": "CG_ATCK_NG (0x20): disables chain/cancel or attack continuation on this frame.",
    "fr_flag1": "Per-frame attack sub-index / multi-hit HIT_DATA selector (used by some multi-hit moves).",
    "fr_se": "Sound effect ID triggered when this animation step begins (0 = none).",
    "fr_zoom": "Sprite scale/palette parameter index (into spr_prm_XX).",
    "fr_prm": "Animation script parameter byte (movement/physics cue read by move code).",
    "fr_type": "Animation step type byte (e.g. triggers cg_type events in s_plXX.c).",
    "fr_flag2": "Secondary per-frame flag byte.",
    "fr_rival": "Rival/throw alignment index or extra 16-bit frame parameter.",

    # Hit Data properties
    "hd_comment": "Human-readable name or note for this HIT_DATA entry (saved as a comment in hit_dtXX.c).",
    "hd_damage": "Raw damage dealt on a clean hit (before combo scaling and guts/defense). Score points = damage / 5.",
    "hd_special": "A_SPECIAL (0x10): marks move as a Special Move (grants chip damage on block and special meter scaling).",
    "hd_hyper": "A_HYPER (0x20): marks move as a Hyper Combo (uses hyper chip/scaling rules, breaks super armor).",
    "hd_cls40": "Attack class bit 0x40: used for command grabs / throws (grants 8 meter on hit).",
    "hd_cls80": "Attack class bit 0x80: enables chip damage even without A_SPECIAL/A_HYPER.",
    "hd_no_chip": "NO_CHIP (0x02): guarded hits deal 0 chip damage.",
    "hd_no_scale": "NO_SCALE (0x08): damage is not reduced by combo hit-count scaling.",
    "hd_no_combo": "NO_COMBO (0x10): hit does not increment the combo counter.",
    "hd_no_ko": "NO_KO (0x20): hit cannot KO the opponent (leaves at least 1 HP).",
    "hd_shc": "Base Hyper Combo gauge gained by the attacker on hit (144 = 1 full bar).",
    "hd_shc_nogauge": "Bit 0x40 on shc: attacker gains 0 meter on hit.",
    "whiff_normal": "Global normal-move whiff meter table (atck_gauge_tbl in src/pl_act.c) for Light, Medium, Heavy, and Extra normals.",
    "whiff_sp": "Meter added when starting this special move (pl_gauge_add call in src/s_plXX.c).",
    "hd_guard": "How the opponent must block this attack: Mid (stand or crouch), Overhead (must stand-block), Low (must crouch-block), or Unblockable.",
    "hd_strength": "Hit strength level (Light/Medium/Heavy): sets default blockstun (11F/14F/18F), hitstop, and pushback tier.",
    "hd_k_low": "K_LOW (0x80): forces standing opponents into the lower-body hitstun animation.",
    "hd_k_back_max": "K_BACK_MAX (0x08): uses maximum block pushback regardless of strength level.",
    "hd_element": "Elemental hit effect (Normal, Fire burn, Ice freeze, Electric shock, Dark, etc.).",
    "hd_hit_stop": "Freeze frames (hitstop) applied to both attacker and defender on hit/block.",
    "hd_guard_time": "Custom blockstun duration in frames. 0 = use default for Strength (11F Light, 14F Medium, 18F Heavy).",
    "hd_sandbag": "Frames before an airborne hit victim is allowed to air-tech (recover).",
    "hd_sleep": "Extra frames the victim stays lying down on the ground after knockdown.",
    "hd_catch_ng": "Frames of throw invulnerability granted to the victim after being hit.",
    "hd_piyo": "Dizzy / stun gauge damage dealt to the victim.",
    "hd_hit_cnt": "Number of combo hits this attack counts for (0 = counts as 1 hit).",
    "hd_jug": "How many hits this attack subtracts from opponent Super Armor.",
    "hd_abyss_jug": "How many hits this attack subtracts from Abyss's special armor (0 = uses Armor Jug).",
    "hd_kd_preset": "Quickly configure Reaction Type and flags for common hit/knockdown behaviors.",
    "hd_dm_no": "Reaction handler (dm_no 0..21) determining hitstun, air launch, soft knockdown, hard knockdown, sweep, snapback, etc.",
    "hd_black_combo": "BLACK_COMBO (0x40): launcher flash; allows attacker to super-jump cancel on hit.",
    "hd_aerial_fin": "AERIAL_FINISH (0x80): air combo finisher camera/knockback behavior.",
    "hd_fly_screen": "FLY_SCREEN (0x20): victim flies off-screen and camera follows.",
    "hd_no_vec": "NO_VEC (0x40): uses fixed knockback speed instead of the Vector angle.",
    "hd_no_zu": "NO_ZU (0x80): disables corner pushback on the attacker when hitting near the wall.",
    "hd_no_weight": "NO_WEIGHT (0x04): ignores defender character weight when calculating launch/knockback.",
    "hd_power": "Knockback speed magnitude index (0..15, looked up in DM_POW_TBL / BACK_SPD_TBL).",
    "hd_vector": "Launch angle in 32 steps (0..31) when NO_VEC is unchecked.",
    "hd_bounce_prop": "Custom UMvSC2 ground/wall bounce rule in src/pl_bounce.c for this move.",
    "hd_bounce_air": "If checked, the bounce only triggers if the victim was airborne when hit.",
    "hd_bounce_nofly": "If checked, suppresses FLY_SCREEN camera behavior during the bounce.",
    "hd_bounce_height": "Rebound vertical velocity in DOT_Y units (0 = default: 8.0 ground, 5.5/7.5 wall).",
    "cancel_jump": "Grant Jump Cancel on hit (Super Jump on ground, Air Jump in air if character has one). Stored in src/pl_com.c cancel_rules[].",
    "cancel_special": "Grant Special Cancel (ignores CG_ATCK_NG on normals, and allows cancelling this special into another special on hit). Stored in src/pl_com.c cancel_rules[].",
    "cancel_hyper": "Grant Hyper Cancel (ignores CG_ATCK_NG and cancel_timer restrictions on hit/block). Stored in src/pl_com.c cancel_rules[].",
    "hd_mark": "Visual hit spark graphic ID (0=Small, 1=Medium, 2=Heavy, 4/5=Slash, 128/255=None).",
    "hd_se": "Hit sound effect ID (played via hit_se_tbl).",
    "hd_score": "Legacy score index (not used; actual score is damage / 5).",
    "hd_range": "Legacy attack reach byte (not read by engine).",
    "hd_cpu_ans": "AI response index when the CPU is hit/blocked by this attack.",
    "hd_rbox1": "Primary HIT_BOX index used by the CPU AI to measure this attack's reach (0 = none).",
    "hd_rbox2": "Secondary HIT_BOX index used by the CPU AI to measure this attack's reach (0 = none).",
    "hd_unk13": "Legacy CPS2 special/hyper hit flag (0 or 1). Checked by an empty branch in dm_data_set() (src/pl_dmg.c); has no effect at runtime.",
    "hd_unk15": "Unread legacy Hyper Combo / signature-grab ID (1..55 in data tables; never read by the engine).",
    "hd_unk1a0": "Unused 4-byte alignment padding byte 0 (0 on all clean rows; never read by the engine).",
    "hd_unk1a1": "Unused 4-byte alignment padding byte 1 (0 on all clean rows; never read by the engine).",
}
