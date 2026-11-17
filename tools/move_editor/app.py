"""Functional Tkinter desktop GUI for editing UMvSC2 move properties, states, animations, and hitboxes."""

from __future__ import annotations

import copy
import math
import os
import shutil
import subprocess
import sys
import tempfile
import tkinter as tk
from dataclasses import dataclass
from pathlib import Path
from tkinter import messagebox, simpledialog, ttk
from typing import Callable, Dict, List, Optional, Set, Tuple

from .backup_git import BackupInfo, BackupManager, GitManager, rebuild_pc_port
from .c_data import (
    ActionEntry,
    BounceRule,
    CGFrame,
    CancelRule,
    CharSequence,
    CharSourceData,
    FrameBox,
    HitBox,
    HitData,
    WhiffGaugeEntry,
    allocate_hit_box,
    clone_frame_box,
    compile_char_bin,
    compile_frame_box_bin,
    compile_hit_box_bin,
    compile_hit_data_bin,
    count_frame_box_users,
    count_hit_box_users,
    load_character,
    parse_atck_gauge_tbl,
    parse_bounce_rules,
    parse_cancel_rules,
    parse_chain_tables,
    save_atck_gauge_tbl,
    save_bounce_rules,
    save_cancel_rules,
    save_char_c,
    save_frame_box_c,
    save_hit_c,
    save_hit_dt_c,
    save_whiff_gauge_entry,
)
from .constants import (
    ANIM_GROUP_MAP,
    ANIM_GROUPS,
    ATK,
    ATE,
    A_GUARD_MASK,
    A_GUARD_NG,
    A_HI,
    A_HYPER,
    A_LO,
    A_NOM,
    A_SPECIAL,
    AERIAL_FINISH,
    BLACK_COMBO,
    BOUNCE_PROP_NAMES,
    BOX_MASK,
    CG_ATCK_NG,
    CHAR_BY_ID,
    CHARACTERS,
    DM_NO_MASK,
    DM_POW_TBL,
    DM_TYPE_NAMES,
    DOT_X_SCALE,
    DOT_Y_SCALE,
    ELEMENT_NAMES,
    FLY_SCREEN,
    GUARD_TYPE_NAMES,
    HIT_MARK_NAMES,
    HIT_SE_NAMES,
    K_BACK_MAX,
    K_ELEM_MASK,
    K_LOW,
    K_LV_MASK,
    NO_CHIP,
    NO_COMBO,
    NO_KO,
    NO_SCALE,
    NO_VEC,
    NO_WEIGHT,
    NO_ZU,
    PUSH_BOX_BASE,
    STANDARD_ATCK_NAMES,
    STRENGTH_LEVEL_NAMES,
    BACK_SPD_TBL,
    compute_move_physics,
)
from .move_catalog import CharCatalog, MoveEntry, StateInfo, build_catalog
from .pl_dat import PlDatArchive, patch_pl_dat, rgba_to_png_bytes, scale_rgba_nearest
from .tooltip import FIELD_TOOLTIPS, attach_tooltip


def find_repo_root(start: Optional[Path] = None) -> Path:
    """Locate `mvc2-naomi-dc` root directory."""
    if start is not None and (start / "data").is_dir() and (start / "include" / "fighter.h").exists():
        return start
    here = Path(__file__).resolve()
    for p in [here.parent.parent.parent, Path.cwd(), Path.cwd() / "mvc2-naomi-dc"]:
        if (p / "data").is_dir() and (p / "include" / "fighter.h").exists():
            return p
    raise FileNotFoundError("Could not locate mvc2-naomi-dc repository root.")


# Colors for each hitbox slot
BOX_COLORS: Dict[str, Tuple[str, str]] = {
    "atck_box": ("#ff3b30", "Attack Box 1"),
    "atck_box2": ("#ff9500", "Attack Box 2 / Throw"),
    "body_box0": ("#3898ff", "Hurtbox 0 (Head)"),
    "body_box1": ("#00c7be", "Hurtbox 1 (Torso)"),
    "body_box2": ("#2b6cb0", "Hurtbox 2 (Legs)"),
    "body_box3": ("#5ac8fa", "Hurtbox 3 (Extra)"),
    "catch_box": ("#34c759", "Throw Hurtbox"),
    "push_box": ("#ffd60a", "Pushbox"),
}

BOX_SLOT_ORDER = [
    "atck_box",
    "atck_box2",
    "body_box0",
    "body_box1",
    "body_box2",
    "body_box3",
    "catch_box",
    "push_box",
]


class ScrollableFrame(ttk.Frame):
    """Reusable vertical scrollable container for property panels."""

    def __init__(self, parent: tk.Widget, **kwargs) -> None:
        super().__init__(parent, **kwargs)
        self.canvas = tk.Canvas(self, highlightthickness=0, borderwidth=0)
        self.vbar = ttk.Scrollbar(self, orient="vertical", command=self.canvas.yview)
        self.inner = ttk.Frame(self.canvas)
        self.inner.bind(
            "<Configure>",
            lambda _e: self.canvas.configure(scrollregion=self.canvas.bbox("all")),
        )
        self._win_id = self.canvas.create_window((0, 0), window=self.inner, anchor="nw")
        self.canvas.configure(yscrollcommand=self.vbar.set)
        self.canvas.bind(
            "<Configure>",
            lambda e: self.canvas.itemconfigure(self._win_id, width=e.width),
        )
        self.canvas.pack(side="left", fill="both", expand=True)
        self.vbar.pack(side="right", fill="y")

        # Mouse wheel scrolling when pointer is over the panel
        self.inner.bind("<Enter>", lambda _e: self._bind_wheel(True))
        self.inner.bind("<Leave>", lambda _e: self._bind_wheel(False))

    def _bind_wheel(self, bind: bool) -> None:
        if bind:
            self.canvas.bind_all("<MouseWheel>", self._on_mousewheel)
        else:
            self.canvas.unbind_all("<MouseWheel>")

    def _on_mousewheel(self, event) -> None:
        delta = -1 if event.delta > 0 else 1
        self.canvas.yview_scroll(delta * 3, "units")


@dataclass
class _EditorSnapshot:
    """Deep snapshot of mutable character + code-table data for Undo/Redo and Discard Action."""

    sequences: Dict[str, List]
    frame_boxes: List[FrameBox]
    hit_boxes: List[HitBox]
    hit_data: List[HitData]
    bounce_rules: List[BounceRule]
    cancel_rules: List[CancelRule]
    atck_gauge_tbl: List[int]
    edited_whiffs: Dict[Tuple[str, int], Tuple[WhiffGaugeEntry, int]]
    dirty_data: bool
    dirty_src_c: bool
    bounce_dirty: bool
    cancel_dirty: bool
    normal_whiff_dirty: bool
    action_ref: Optional[Tuple[int, int]]
    frame_idx: int
    atck_no: int
    no_active_hit_data: bool
    box_slot: str


class MoveEditorApp:
    """Main Move Property, State/Animation & Interactive Hitbox Editor window."""

    def __init__(self, root: tk.Tk, repo_root: Path) -> None:
        self.root = root
        self.repo_root = repo_root
        self.backup_mgr = BackupManager(repo_root)
        self.git_mgr = GitManager(repo_root)

        self.root.title("UMvSC2 Move Property & Hitbox Editor (mvc2-naomi-dc)")
        self.root.geometry("1580x940")
        self.root.minsize(1200, 740)

        # Data state
        self.char_id: int = 0x00
        self.palette_idx: int = 0
        self.char_data: Optional[CharSourceData] = None
        self.char_catalog: Optional[CharCatalog] = None
        self.pl_archive: Optional[PlDatArchive] = None
        self.bounce_rules: List[BounceRule] = parse_bounce_rules(repo_root)
        self.cancel_rules: List[CancelRule] = parse_cancel_rules(repo_root)
        self.chain_tables: Dict[str, List[int]] = {}
        self.atck_gauge_tbl, _ = parse_atck_gauge_tbl(repo_root)

        self.filtered_actions: List[ActionEntry] = []
        self.current_action: Optional[ActionEntry] = None
        self.current_seq: Optional[CharSequence] = None
        self.current_move: Optional[MoveEntry] = None
        self.current_version_key: str = "*"
        self.current_frame_idx: int = 0
        self.current_tick_in_frame: int = 0
        self.current_atck_no: int = 0
        self.no_active_hit_data: bool = False

        # Dirty flags & Undo/Redo
        self.dirty_data: bool = False
        self.dirty_src_c: bool = False
        self.edited_whiffs: Dict[Tuple[str, int], Tuple[WhiffGaugeEntry, int]] = {}
        self.bounce_dirty: bool = False
        self.cancel_dirty: bool = False
        self.normal_whiff_dirty: bool = False
        self.undo_stack: List[_EditorSnapshot] = []
        self.redo_stack: List[_EditorSnapshot] = []
        self.saved_baseline: Optional[_EditorSnapshot] = None
        self._drag_pre_snapshot: Optional[_EditorSnapshot] = None
        self._drag_moved: bool = False
        self._test_proc: Optional[subprocess.Popen] = None
        self._test_backup: Optional[Tuple[List[Tuple[Path, Path]], tempfile.TemporaryDirectory]] = None

        # Playback state
        self.is_playing: bool = False
        self._play_after_id: Optional[str] = None
        self._tick_accum: float = 0.0
        self._last_play_time: float = 0.0

        # Canvas & interactive hitbox editing state
        self.sprite_cache: Dict[Tuple[int, int, bool, int, float, float], tk.PhotoImage] = {}
        self.origin_px_x: float = 280.0
        self.origin_px_y: float = 380.0
        self.selected_box_slot: str = "atck_box"
        self.selected_push_stance: int = 0  # 0=Stand, 1=Crouch, 2=Air, 3=Down
        self.box_screen_rects: Dict[str, Tuple[float, float, float, float]] = {}
        self._drag_mode: Optional[str] = None  # "move", "n", "s", "e", "w", "nw", "ne", "sw", "se", "pan"
        self._drag_start_px: Tuple[float, float] = (0.0, 0.0)
        self._drag_orig_dots: Tuple[float, float, float, float] = (0.0, 0.0, 0.0, 0.0)  # (left, top, right, bottom)
        self._drag_pan_origin: Tuple[float, float] = (0.0, 0.0)
        self._drag_cloned_done: bool = False

        # Tk variables (initialized before building UI)
        self._init_vars()
        self._build_ui()
        self._bind_shortcuts()
        self.load_char(0x00)
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

    # ------------------------------------------------------------------
    # Variable setup
    # ------------------------------------------------------------------

    def _init_vars(self) -> None:
        self.var_char = tk.StringVar()
        self.var_palette = tk.StringVar(value="Color 1 (LP)")
        self.var_category = tk.StringVar(value="All Categories")
        self.var_action = tk.StringVar()
        self.var_move = tk.StringVar(value="(All / None)")
        self.var_version = tk.StringVar(value="All versions")
        self.var_sync_action_hit = tk.BooleanVar(value=True)
        self.var_atck = tk.StringVar()
        self.var_move_timing = tk.StringVar(value="Select a Move to see chained states and code timers.")
        self.var_move_notes = tk.StringVar(value="")

        # Viewer options
        self.var_zoom = tk.IntVar(value=3)
        self.var_aspect_43 = tk.BooleanVar(value=True)
        self.var_face_right = tk.BooleanVar(value=True)
        self.var_show_traj = tk.BooleanVar(value=True)
        self.var_show_range = tk.BooleanVar(value=False)
        self.var_clone_shared = tk.BooleanVar(value=True)
        self.var_show_layer: Dict[str, tk.BooleanVar] = {
            slot: tk.BooleanVar(value=True) for slot in BOX_SLOT_ORDER
        }

        # Playback options
        self.var_loop = tk.BooleanVar(value=True)
        self.var_speed = tk.DoubleVar(value=1.0)
        self.var_frame_status = tk.StringVar(value="Frame 0/0 | Tick 0/0")
        self.var_timing_summary = tk.StringVar(value="Startup: - | Active: - | Recovery: - | Total: -")

        # Frame Inspector (CG_FRAME)
        self.var_fr_ctr = tk.IntVar(value=1)
        self.var_fr_spr = tk.IntVar(value=0)
        self.var_fr_box = tk.IntVar(value=0)
        self.var_fr_atk = tk.BooleanVar(value=False)
        self.var_fr_ate = tk.BooleanVar(value=False)
        self.var_fr_ng = tk.BooleanVar(value=False)
        self.var_fr_flag1 = tk.IntVar(value=0)
        self.var_fr_se = tk.IntVar(value=0)
        self.var_fr_zoom = tk.IntVar(value=0)
        self.var_fr_prm = tk.IntVar(value=0)
        self.var_fr_type = tk.IntVar(value=0)
        self.var_fr_flag2 = tk.IntVar(value=0)
        self.var_fr_rival = tk.IntVar(value=0)

        # Selected HitBox inspector
        self.var_hb_slot = tk.StringVar(value="atck_box")
        self.var_hb_id = tk.IntVar(value=0)
        self.var_hb_x = tk.IntVar(value=0)
        self.var_hb_w = tk.IntVar(value=0)
        self.var_hb_y = tk.IntVar(value=0)
        self.var_hb_h = tk.IntVar(value=0)
        self.var_hb_throw_bit = tk.BooleanVar(value=False)
        self.var_hb_body3_type = tk.StringVar(value="0x0000 (Type 4)")
        self.var_hb_push_stance = tk.StringVar(value="0: Stand (252)")
        self.var_hb_share_info = tk.StringVar(value="")

        # Move Properties (HIT_DATA + Bounce + Whiff)
        self.var_hd_comment = tk.StringVar(value="")
        self.var_hd_summary = tk.StringVar(value="")
        self.var_hd_damage = tk.IntVar(value=0)
        self.var_hd_chip_info = tk.StringVar(value="Chip: 0")
        self.var_hd_special = tk.BooleanVar(value=False)
        self.var_hd_hyper = tk.BooleanVar(value=False)
        self.var_hd_cls40 = tk.BooleanVar(value=False)
        self.var_hd_cls80 = tk.BooleanVar(value=False)

        self.var_hd_no_chip = tk.BooleanVar(value=False)
        self.var_hd_no_weight = tk.BooleanVar(value=False)
        self.var_hd_no_scale = tk.BooleanVar(value=False)
        self.var_hd_no_combo = tk.BooleanVar(value=False)
        self.var_hd_no_ko = tk.BooleanVar(value=False)
        self.var_hd_black_combo = tk.BooleanVar(value=False)
        self.var_hd_aerial_fin = tk.BooleanVar(value=False)

        self.var_hd_shc = tk.IntVar(value=0)
        self.var_hd_shc_nogauge = tk.BooleanVar(value=False)
        self.var_hd_meter_info = tk.StringVar(value="")
        self.var_whiff_normal = [tk.IntVar(value=v) for v in self.atck_gauge_tbl[:4]]
        self.var_whiff_sp_amt = tk.IntVar(value=0)
        self.var_whiff_sp_label = tk.StringVar(value="No special whiff call matched")

        self.var_hd_guard = tk.StringVar(value=GUARD_TYPE_NAMES[0])
        self.var_hd_strength = tk.StringVar(value=STRENGTH_LEVEL_NAMES[0x10])
        self.var_hd_k_low = tk.BooleanVar(value=False)
        self.var_hd_k_back_max = tk.BooleanVar(value=False)
        self.var_hd_element = tk.StringVar(value=ELEMENT_NAMES[0])

        self.var_hd_hit_stop = tk.IntVar(value=0)
        self.var_hd_guard_time = tk.IntVar(value=0)
        self.var_hd_guard_time_eff = tk.StringVar(value="")
        self.var_hd_sandbag_time = tk.IntVar(value=0)
        self.var_hd_sleep_time = tk.IntVar(value=0)
        self.var_hd_catch_ng = tk.IntVar(value=0)
        self.var_hd_piyo = tk.IntVar(value=0)
        self.var_hd_hit_cnt = tk.IntVar(value=0)
        self.var_hd_jug = tk.IntVar(value=0)
        self.var_hd_abyss_jug = tk.IntVar(value=0)

        self.var_hd_kd_preset = tk.StringVar(value="Select preset to apply...")
        self.var_hd_dm_no = tk.StringVar(value=DM_TYPE_NAMES[0][0])
        self.var_hd_dm_desc = tk.StringVar(value="")
        self.var_hd_fly_screen = tk.BooleanVar(value=False)
        self.var_hd_no_vec = tk.BooleanVar(value=False)
        self.var_hd_no_zu = tk.BooleanVar(value=False)
        self.var_hd_power = tk.IntVar(value=0)
        self.var_hd_vector = tk.IntVar(value=0)
        self.var_hd_physics_detail = tk.StringVar(value="")

        # Bounce variables
        self.var_bounce_prop = tk.StringVar(value=f"0: {BOUNCE_PROP_NAMES[0][1]} ({BOUNCE_PROP_NAMES[0][2]})")
        self.var_bounce_need_air = tk.BooleanVar(value=True)
        self.var_bounce_no_fly = tk.BooleanVar(value=True)
        self.var_bounce_height = tk.StringVar(value="0.0")

        # Cancel options variables
        self.var_cancel_status = tk.StringVar(value="Cancel Options: -")
        self.var_cancel_jump = tk.BooleanVar(value=False)
        self.var_cancel_special = tk.BooleanVar(value=False)
        self.var_cancel_hyper = tk.BooleanVar(value=False)

        # Presentation & AI
        self.var_hd_mark = tk.IntVar(value=0)
        self.var_hd_se = tk.IntVar(value=0)
        self.var_hd_score = tk.IntVar(value=4)
        self.var_hd_range = tk.IntVar(value=0x60)
        self.var_hd_cpu_ans = tk.IntVar(value=0)
        self.var_hd_rbox1 = tk.IntVar(value=0)
        self.var_hd_rbox2 = tk.IntVar(value=0)
        self.var_hd_unk13 = tk.IntVar(value=0)
        self.var_hd_unk15 = tk.IntVar(value=0)
        self.var_hd_unk1a0 = tk.IntVar(value=0)
        self.var_hd_unk1a1 = tk.IntVar(value=0)

        self.var_status = tk.StringVar(value="Ready.")
        self._suppress_callbacks = False

    # ------------------------------------------------------------------
    # UI Construction
    # ------------------------------------------------------------------

    def _bind_shortcuts(self) -> None:
        self.root.bind_all("<Control-z>", lambda _e: self.undo())
        self.root.bind_all("<Control-Z>", lambda _e: self.undo())
        self.root.bind_all("<Control-Shift-Z>", lambda _e: self.redo())
        self.root.bind_all("<Control-Shift-z>", lambda _e: self.redo())
        self.root.bind_all("<Control-y>", lambda _e: self.redo())
        self.root.bind_all("<Control-Y>", lambda _e: self.redo())

        def _step_combobox(event, delta: int):
            w = event.widget
            if not isinstance(w, ttk.Combobox):
                return None
            try:
                popdown = w.tk.call("ttk::combobox::PopdownWindow", w)
                if int(w.tk.call("winfo", "ismapped", popdown)):
                    return None  # let open dropdown navigate normally
            except Exception:
                pass
            vals = w.cget("values")
            if not vals:
                return "break"
            cur = w.current()
            if cur < 0:
                nxt = 0
            else:
                nxt = max(0, min(len(vals) - 1, cur + delta))
            if nxt != cur:
                w.current(nxt)
                w.event_generate("<<ComboboxSelected>>")
            return "break"

        self.root.bind_class("TCombobox", "<Up>", lambda e: _step_combobox(e, -1))
        self.root.bind_class("TCombobox", "<Down>", lambda e: _step_combobox(e, 1))

    def _build_ui(self) -> None:
        # Top Navigation & Persistence Bar
        top = ttk.Frame(self.root, padding=(6, 4))
        top.pack(fill="x")

        ttk.Label(top, text="Character:").pack(side="left")
        char_vals = [f"{cid:02X}: {name}" for cid, name, _ in CHARACTERS]
        self.cb_char = ttk.Combobox(top, textvariable=self.var_char, values=char_vals, state="readonly", width=20)
        self.cb_char.pack(side="left", padx=(2, 6))
        self.cb_char.bind("<<ComboboxSelected>>", self._on_char_selected)
        attach_tooltip(self.cb_char, FIELD_TOOLTIPS["char"])

        ttk.Label(top, text="Palette:").pack(side="left")
        self.cb_pal = ttk.Combobox(
            top,
            textvariable=self.var_palette,
            values=[f"Color {i+1}" for i in range(6)],
            state="readonly",
            width=9,
        )
        self.cb_pal.current(0)
        self.cb_pal.pack(side="left", padx=(2, 6))
        self.cb_pal.bind("<<ComboboxSelected>>", self._on_palette_selected)
        attach_tooltip(self.cb_pal, FIELD_TOOLTIPS["palette"])

        ttk.Label(top, text="Category:").pack(side="left")
        cat_vals = ["All Categories"] + [f"{g:02d}: {sym} ({desc})" for g, sym, desc in ANIM_GROUPS]
        self.cb_cat = ttk.Combobox(top, textvariable=self.var_category, values=cat_vals, state="readonly", width=24)
        self.cb_cat.current(0)
        self.cb_cat.pack(side="left", padx=(2, 6))
        self.cb_cat.bind("<<ComboboxSelected>>", lambda _e: self._refresh_action_list())
        attach_tooltip(self.cb_cat, FIELD_TOOLTIPS["category"])

        ttk.Label(top, text="Action / State:").pack(side="left")
        self.cb_action = ttk.Combobox(top, textvariable=self.var_action, state="readonly", width=36)
        self.cb_action.pack(side="left", padx=(2, 6), fill="x", expand=True)
        self.cb_action.bind("<<ComboboxSelected>>", self._on_action_selected)
        attach_tooltip(self.cb_action, FIELD_TOOLTIPS["action"])

        ttk.Label(top, text="Move:").pack(side="left")
        self.cb_move = ttk.Combobox(top, textvariable=self.var_move, state="readonly", width=26)
        self.cb_move.pack(side="left", padx=(2, 4))
        self.cb_move.bind("<<ComboboxSelected>>", self._on_move_selected)
        attach_tooltip(self.cb_move, FIELD_TOOLTIPS["move"])

        ttk.Label(top, text="Version:").pack(side="left")
        self.cb_version = ttk.Combobox(top, textvariable=self.var_version, state="readonly", width=18)
        self.cb_version.pack(side="left", padx=(2, 2))
        self.cb_version.bind("<<ComboboxSelected>>", self._on_version_selected)
        attach_tooltip(self.cb_version, FIELD_TOOLTIPS["version"])

        top2 = ttk.Frame(self.root, padding=(6, 2))
        top2.pack(fill="x")

        ttk.Label(top2, text="Move Hit Data (atck_no):").pack(side="left")
        self.cb_atck = ttk.Combobox(top2, textvariable=self.var_atck, state="readonly", width=62)
        self.cb_atck.pack(side="left", padx=(2, 6))
        self.cb_atck.bind("<<ComboboxSelected>>", self._on_atck_selected)
        attach_tooltip(self.cb_atck, FIELD_TOOLTIPS["atck"])

        ttk.Button(top2, text="+ New Attack", command=self._add_hit_data_entry).pack(side="left", padx=(0, 8))
        chk_sync = ttk.Checkbutton(
            top2,
            text="Sync Action <-> Hit Data",
            variable=self.var_sync_action_hit,
            command=self._on_sync_toggled,
        )
        chk_sync.pack(side="left", padx=(0, 10))
        attach_tooltip(chk_sync, FIELD_TOOLTIPS["sync_toggle"])

        self.btn_save = ttk.Button(top2, text="Save (C + BIN + Backup)", command=self.save_all)
        self.btn_save.pack(side="right", padx=2)
        self.btn_test_game = ttk.Button(top2, text="Test in Game", command=self.test_in_game)
        self.btn_test_game.pack(side="right", padx=2)
        attach_tooltip(self.btn_test_game, FIELD_TOOLTIPS["test_in_game"])
        ttk.Button(top2, text="Reload", command=lambda: self.load_char(self.char_id, prompt_dirty=True)).pack(side="right", padx=2)
        self.btn_discard_action = ttk.Button(top2, text="Discard Action", command=self.discard_selected_action)
        self.btn_discard_action.pack(side="right", padx=2)
        attach_tooltip(self.btn_discard_action, FIELD_TOOLTIPS["discard_action"])
        self.btn_redo = ttk.Button(top2, text="Redo", command=self.redo, state="disabled")
        self.btn_redo.pack(side="right", padx=2)
        attach_tooltip(self.btn_redo, FIELD_TOOLTIPS["redo"])
        self.btn_undo = ttk.Button(top2, text="Undo", command=self.undo, state="disabled")
        self.btn_undo.pack(side="right", padx=2)
        attach_tooltip(self.btn_undo, FIELD_TOOLTIPS["undo"])
        ttk.Button(top2, text="Rebuild EXE", command=self._on_rebuild_exe).pack(side="right", padx=2)
        ttk.Button(top2, text="Git Commit...", command=self._open_git_dialog).pack(side="right", padx=2)
        ttk.Button(top2, text="Backups / Restore...", command=self._open_backup_dialog).pack(side="right", padx=2)

        # Main Split Pane: Left (Hitbox & Animation Viewer) | Right (Move Properties)
        paned = ttk.PanedWindow(self.root, orient="horizontal")
        paned.pack(fill="both", expand=True, padx=6, pady=4)

        left_pane = ttk.Frame(paned)
        right_pane = ttk.Frame(paned, width=700)
        paned.add(left_pane, weight=3)
        paned.add(right_pane, weight=3)

        self._build_left_viewer(left_pane)
        self._build_right_properties(right_pane)

        # Bottom Status Bar
        status_bar = ttk.Frame(self.root, padding=(6, 2))
        status_bar.pack(fill="x", side="bottom")
        ttk.Label(status_bar, textvariable=self.var_status, relief="sunken", anchor="w").pack(fill="x")

    def _build_left_viewer(self, parent: ttk.Frame) -> None:
        # Canvas display options bar
        opts = ttk.Frame(parent)
        opts.pack(fill="x", pady=(0, 2))

        ttk.Label(opts, text="Zoom:").pack(side="left")
        sp_z = ttk.Spinbox(opts, from_=1, to=8, textvariable=self.var_zoom, width=3, command=self._redraw_canvas)
        sp_z.pack(side="left", padx=(2, 6))
        attach_tooltip(sp_z, FIELD_TOOLTIPS["zoom"])
        for txt, var, tip_key in [
            ("4:3 Aspect", self.var_aspect_43, "aspect_43"),
            ("Face Right (P1)", self.var_face_right, "face_right"),
            ("Trajectory Arc", self.var_show_traj, "traj"),
            ("AI Range", self.var_show_range, "ai_range"),
        ]:
            cb = ttk.Checkbutton(opts, text=txt, variable=var, command=self._redraw_canvas)
            cb.pack(side="left", padx=2)
            attach_tooltip(cb, FIELD_TOOLTIPS[tip_key])
        cb_cs = ttk.Checkbutton(opts, text="Clone Shared on Edit", variable=self.var_clone_shared)
        cb_cs.pack(side="left", padx=6)
        attach_tooltip(cb_cs, FIELD_TOOLTIPS["clone_shared"])
        ttk.Button(opts, text="Center View", command=self._center_canvas).pack(side="right")

        # Box layer toggles
        layers = ttk.Frame(parent)
        layers.pack(fill="x", pady=(0, 2))
        for slot in BOX_SLOT_ORDER:
            color, title = BOX_COLORS[slot]
            f = ttk.Frame(layers)
            f.pack(side="left", padx=3)
            swatch = tk.Label(f, text="  ", bg=color, relief="solid", borderwidth=1)
            swatch.pack(side="left", padx=(0, 2))
            ttk.Checkbutton(
                f,
                text=slot.replace("body_box", "body").replace("atck_box", "atk"),
                variable=self.var_show_layer[slot],
                command=self._redraw_canvas,
            ).pack(side="left")

        # Interactive Canvas
        cwrap = ttk.Frame(parent, relief="sunken", borderwidth=1)
        cwrap.pack(fill="both", expand=True)
        self.canvas = tk.Canvas(cwrap, bg="#181a20", highlightthickness=0)
        self.canvas.pack(fill="both", expand=True)
        self.canvas.bind("<ButtonPress-1>", self._on_canvas_lpress)
        self.canvas.bind("<B1-Motion>", self._on_canvas_ldrag)
        self.canvas.bind("<ButtonRelease-1>", self._on_canvas_lrelease)
        self.canvas.bind("<Motion>", self._on_canvas_hover)
        self.canvas.bind("<ButtonPress-3>", self._on_canvas_pan_start)
        self.canvas.bind("<B3-Motion>", self._on_canvas_pan_move)
        self.canvas.bind("<ButtonPress-2>", self._on_canvas_pan_start)
        self.canvas.bind("<B2-Motion>", self._on_canvas_pan_move)
        self.canvas.bind("<Configure>", lambda _e: self._redraw_canvas())

        # Hitbox Editor Bar directly under Canvas
        hb_frame = ttk.LabelFrame(parent, text="Hitbox Editor (Drag edges/corners on canvas or edit coordinates below)", padding=4)
        hb_frame.pack(fill="x", pady=3)

        hb_r1 = ttk.Frame(hb_frame)
        hb_r1.pack(fill="x")
        ttk.Label(hb_r1, text="Slot:").pack(side="left")
        self.cb_hb_slot = ttk.Combobox(hb_r1, textvariable=self.var_hb_slot, values=BOX_SLOT_ORDER, state="readonly", width=11)
        self.cb_hb_slot.pack(side="left", padx=(2, 6))
        self.cb_hb_slot.bind("<<ComboboxSelected>>", self._on_hb_slot_selected)

        ttk.Button(hb_r1, text="Add Box", command=self._add_box_to_slot).pack(side="left", padx=2)
        ttk.Button(hb_r1, text="Remove Box", command=self._remove_box_from_slot).pack(side="left", padx=2)
        ttk.Button(hb_r1, text="Clone FrameBox", command=self._clone_current_frame_box).pack(side="left", padx=(2, 8))

        ttk.Label(hb_r1, text="HitBox ID:").pack(side="left")
        sp_hbid = ttk.Spinbox(hb_r1, from_=0, to=8191, textvariable=self.var_hb_id, width=5, command=self._on_hb_id_changed)
        sp_hbid.pack(side="left", padx=(2, 6))
        sp_hbid.bind("<Return>", lambda _e: self._on_hb_id_changed())
        sp_hbid.bind("<FocusOut>", lambda _e: self._on_hb_id_changed())

        ttk.Checkbutton(hb_r1, text="Throw Box (0x8000)", variable=self.var_hb_throw_bit, command=self._on_hb_flags_changed).pack(side="left", padx=4)
        ttk.Label(hb_r1, text="Body3 Type:").pack(side="left")
        cb_b3 = ttk.Combobox(
            hb_r1,
            textvariable=self.var_hb_body3_type,
            values=["0x0000 (Type 4)", "0x2000 (Type 5)", "0x4000 (Type 6)", "0x6000 (Type 7)"],
            state="readonly",
            width=15,
        )
        cb_b3.pack(side="left", padx=2)
        cb_b3.bind("<<ComboboxSelected>>", lambda _e: self._on_hb_flags_changed())

        hb_r2 = ttk.Frame(hb_frame)
        hb_r2.pack(fill="x", pady=(3, 0))
        for lbl, var in [("X:", self.var_hb_x), ("Half-W:", self.var_hb_w), ("Y:", self.var_hb_y), ("Half-H:", self.var_hb_h)]:
            ttk.Label(hb_r2, text=lbl).pack(side="left")
            sp = ttk.Spinbox(hb_r2, from_=-2048, to=2048, textvariable=var, width=6, command=self._on_hb_coords_changed)
            sp.pack(side="left", padx=(2, 6))
            sp.bind("<Return>", lambda _e: self._on_hb_coords_changed())
            sp.bind("<FocusOut>", lambda _e: self._on_hb_coords_changed())

        ttk.Label(hb_r2, text="Push Stance:").pack(side="left")
        cb_ps = ttk.Combobox(
            hb_r2,
            textvariable=self.var_hb_push_stance,
            values=["0: Stand (252)", "1: Crouch (253)", "2: Air (254)", "3: Down (255)"],
            state="readonly",
            width=16,
        )
        cb_ps.pack(side="left", padx=(2, 6))
        cb_ps.bind("<<ComboboxSelected>>", self._on_push_stance_changed)
        ttk.Label(hb_r2, textvariable=self.var_hb_share_info, foreground="#555555").pack(side="left", padx=4)

        # Animation Playback Bar & Timeline
        anim_frame = ttk.LabelFrame(parent, text="Animation Player & Frame Data", padding=4)
        anim_frame.pack(fill="x", pady=(2, 0))

        ctrl = ttk.Frame(anim_frame)
        ctrl.pack(fill="x")
        ttk.Button(ctrl, text="|<", width=3, command=self.first_frame).pack(side="left", padx=1)
        ttk.Button(ctrl, text="< Frame", width=7, command=self.prev_frame).pack(side="left", padx=1)
        ttk.Button(ctrl, text="< Tick", width=6, command=self.prev_tick).pack(side="left", padx=1)
        self.btn_play = ttk.Button(ctrl, text="Play", width=7, command=self.toggle_play)
        self.btn_play.pack(side="left", padx=2)
        ttk.Button(ctrl, text="Tick >", width=6, command=self.next_tick).pack(side="left", padx=1)
        ttk.Button(ctrl, text="Frame >", width=7, command=self.next_frame).pack(side="left", padx=1)
        ttk.Button(ctrl, text=">|", width=3, command=self.last_frame).pack(side="left", padx=1)
        ttk.Checkbutton(ctrl, text="Loop", variable=self.var_loop).pack(side="left", padx=4)

        ttk.Label(ctrl, text="Speed:").pack(side="left", padx=(6, 2))
        for sp_val in (0.1, 0.25, 0.5, 1.0, 2.0):
            ttk.Button(ctrl, text=f"{sp_val}x", width=4, command=lambda v=sp_val: self.var_speed.set(v)).pack(side="left", padx=1)
        ttk.Spinbox(ctrl, from_=0.02, to=10.0, increment=0.05, textvariable=self.var_speed, width=5).pack(side="left", padx=2)
        ttk.Scale(ctrl, from_=0.05, to=4.0, variable=self.var_speed, orient="horizontal", length=90).pack(side="left", padx=2)

        # Frame counts & status
        stat_row = ttk.Frame(anim_frame)
        stat_row.pack(fill="x", pady=(2, 2))
        ttk.Label(stat_row, textvariable=self.var_frame_status, font=("Consolas", 9, "bold")).pack(side="left")
        ttk.Label(stat_row, textvariable=self.var_timing_summary, font=("Consolas", 9)).pack(side="right")

        # Visual Timeline Strip
        self.timeline = tk.Canvas(anim_frame, height=22, bg="#22252c", highlightthickness=1, highlightbackground="#444")
        self.timeline.pack(fill="x", pady=(0, 3))
        self.timeline.bind("<Button-1>", self._on_timeline_click)

        # CG_FRAME field editor row
        fr_row1 = ttk.Frame(anim_frame)
        fr_row1.pack(fill="x")
        for lbl, var, lo, hi, w in [
            ("Duration (ctr):", self.var_fr_ctr, -1, 255, 4),
            ("Sprite:", self.var_fr_spr, -1, 65535, 6),
            ("FrameBox:", self.var_fr_box, 0, 8191, 5),
            ("flag1:", self.var_fr_flag1, 0, 255, 4),
            ("se:", self.var_fr_se, 0, 255, 4),
            ("zoom:", self.var_fr_zoom, 0, 31, 3),
        ]:
            ttk.Label(fr_row1, text=lbl).pack(side="left")
            sp = ttk.Spinbox(fr_row1, from_=lo, to=hi, textvariable=var, width=w, command=self._on_frame_fields_changed)
            sp.pack(side="left", padx=(2, 5))
            sp.bind("<Return>", lambda _e: self._on_frame_fields_changed())

        ttk.Checkbutton(fr_row1, text="ATK", variable=self.var_fr_atk, command=self._on_frame_fields_changed).pack(side="left", padx=2)
        ttk.Checkbutton(fr_row1, text="ATE", variable=self.var_fr_ate, command=self._on_frame_fields_changed).pack(side="left", padx=2)
        ttk.Checkbutton(fr_row1, text="NG", variable=self.var_fr_ng, command=self._on_frame_fields_changed).pack(side="left", padx=2)

        fr_row2 = ttk.Frame(anim_frame)
        fr_row2.pack(fill="x", pady=(2, 0))
        for lbl, var, lo, hi, w in [
            ("prm:", self.var_fr_prm, -128, 127, 4),
            ("type:", self.var_fr_type, -128, 127, 4),
            ("flag2:", self.var_fr_flag2, 0, 255, 4),
            ("rival:", self.var_fr_rival, -32768, 65535, 6),
        ]:
            ttk.Label(fr_row2, text=lbl).pack(side="left")
            sp = ttk.Spinbox(fr_row2, from_=lo, to=hi, textvariable=var, width=w, command=self._on_frame_fields_changed)
            sp.pack(side="left", padx=(2, 5))
            sp.bind("<Return>", lambda _e: self._on_frame_fields_changed())

        ttk.Button(fr_row2, text="Duplicate Frame", command=self._duplicate_current_frame).pack(side="right", padx=2)
        ttk.Button(fr_row2, text="Delete Frame", command=self._delete_current_frame).pack(side="right", padx=2)

        # Move Timeline & Chained States panel
        mv_frame = ttk.LabelFrame(parent, text="Move Timeline & Chained States", padding=4)
        mv_frame.pack(fill="x", pady=(2, 0))
        mv_top = ttk.Frame(mv_frame)
        mv_top.pack(fill="x")
        ttk.Label(mv_top, textvariable=self.var_move_timing, font=("Consolas", 8, "bold"), foreground="#104e8b").pack(side="left")

        dur_bar = ttk.Frame(mv_top)
        dur_bar.pack(side="right")
        ttk.Label(dur_bar, text="State Speed:").pack(side="left", padx=(4, 2))
        for txt, mult, delta in [("-25%", 0.75, 0), ("-1F", 1.0, -1), ("+1F", 1.0, 1), ("+25%", 1.25, 0)]:
            b = ttk.Button(dur_bar, text=txt, width=5, command=lambda m=mult, d=delta: self._scale_current_state_duration(m, d))
            b.pack(side="left", padx=1)
            attach_tooltip(b, "Adjust all non-hold frame durations in the currently selected Action/State.")

        self.move_states_bar = ttk.Frame(mv_frame)
        self.move_states_bar.pack(fill="x", pady=(2, 0))
        ttk.Label(mv_frame, textvariable=self.var_move_notes, foreground="#555555", font=("Segoe UI", 8), wraplength=760, justify="left").pack(anchor="w", pady=(1, 0))

    def _build_right_properties(self, parent: ttk.Frame) -> None:
        # Live summary banner
        sum_box = ttk.LabelFrame(parent, text="Move Property Summary", padding=5)
        sum_box.pack(fill="x", pady=(0, 4))
        ttk.Label(sum_box, textvariable=self.var_hd_summary, font=("Consolas", 9), justify="left", wraplength=640).pack(fill="x")

        sc = ScrollableFrame(parent)
        sc.pack(fill="both", expand=True)
        p = sc.inner

        # Helper to trigger property writeback on spinbox/entry
        def bind_var_spin(parent_w, label: str, var: tk.Variable, lo: int, hi: int, width: int = 6, tip_key: str = ""):
            lbl_w = ttk.Label(parent_w, text=label)
            lbl_w.pack(side="left")
            sp = ttk.Spinbox(parent_w, from_=lo, to=hi, textvariable=var, width=width, command=self._on_hit_data_changed)
            sp.pack(side="left", padx=(2, 8))
            sp.bind("<Return>", lambda _e: self._on_hit_data_changed())
            sp.bind("<FocusOut>", lambda _e: self._on_hit_data_changed())
            if tip_key and tip_key in FIELD_TOOLTIPS:
                attach_tooltip(lbl_w, FIELD_TOOLTIPS[tip_key])
                attach_tooltip(sp, FIELD_TOOLTIPS[tip_key])
            return sp

        def add_chk(parent_w, text: str, var: tk.BooleanVar, cmd, tip_key: str = "", padx: int = 2):
            chk = ttk.Checkbutton(parent_w, text=text, variable=var, command=cmd)
            chk.pack(side="left", padx=padx)
            if tip_key and tip_key in FIELD_TOOLTIPS:
                attach_tooltip(chk, FIELD_TOOLTIPS[tip_key])
            return chk

        # Section 0: Comment / Name
        f_id = ttk.LabelFrame(p, text="1. Attack Identity", padding=5)
        f_id.pack(fill="x", padx=4, pady=3)
        r_id = ttk.Frame(f_id)
        r_id.pack(fill="x")
        ttk.Label(r_id, text="Name / Note:").pack(side="left")
        ent_c = ttk.Entry(r_id, textvariable=self.var_hd_comment)
        ent_c.pack(side="left", fill="x", expand=True, padx=4)
        ent_c.bind("<Return>", lambda _e: self._on_hit_data_changed())
        ent_c.bind("<FocusOut>", lambda _e: self._on_hit_data_changed())
        attach_tooltip(ent_c, FIELD_TOOLTIPS["hd_comment"])

        # Section 1: Damage & Scaling
        f_dmg = ttk.LabelFrame(p, text="2. Damage, Chip & Attack Class", padding=5)
        f_dmg.pack(fill="x", padx=4, pady=3)
        r1 = ttk.Frame(f_dmg)
        r1.pack(fill="x")
        bind_var_spin(r1, "Damage on Hit:", self.var_hd_damage, 0, 255, 6, "hd_damage")
        ttk.Label(r1, textvariable=self.var_hd_chip_info, foreground="#0a58ca").pack(side="left", padx=4)

        r2 = ttk.Frame(f_dmg)
        r2.pack(fill="x", pady=(3, 0))
        add_chk(r2, "Special Move (Chip + Meter)", self.var_hd_special, self._on_hit_data_changed, "hd_special")
        add_chk(r2, "Hyper Combo (Chip + Scaling)", self.var_hd_hyper, self._on_hit_data_changed, "hd_hyper")
        add_chk(r2, "Cmd/Throw (0x40)", self.var_hd_cls40, self._on_hit_data_changed, "hd_cls40")
        add_chk(r2, "Chip Bit (0x80)", self.var_hd_cls80, self._on_hit_data_changed, "hd_cls80")

        r3 = ttk.Frame(f_dmg)
        r3.pack(fill="x", pady=(3, 0))
        add_chk(r3, "No Chip Damage", self.var_hd_no_chip, self._on_hit_data_changed, "hd_no_chip")
        add_chk(r3, "No Combo Scaling", self.var_hd_no_scale, self._on_hit_data_changed, "hd_no_scale")
        add_chk(r3, "Don't Count Combo", self.var_hd_no_combo, self._on_hit_data_changed, "hd_no_combo")
        add_chk(r3, "Non-Lethal (No KO)", self.var_hd_no_ko, self._on_hit_data_changed, "hd_no_ko")

        # Section 2: Gauge Generation (On Hit, On Whiff, On Block)
        f_met = ttk.LabelFrame(p, text="3. Gauge Generation (On Hit / On Whiff / On Block) [144 = 1 Bar]", padding=5)
        f_met.pack(fill="x", padx=4, pady=3)

        rm1 = ttk.Frame(f_met)
        rm1.pack(fill="x")
        bind_var_spin(rm1, "On Hit Base (0..63):", self.var_hd_shc, 0, 63, 5, "hd_shc")
        add_chk(rm1, "No Attacker Meter on Hit", self.var_hd_shc_nogauge, self._on_hit_data_changed, "hd_shc_nogauge", padx=4)
        ttk.Label(f_met, textvariable=self.var_hd_meter_info, foreground="#333333").pack(anchor="w", pady=(2, 2))

        rm2 = ttk.Frame(f_met)
        rm2.pack(fill="x", pady=(2, 0))
        lbl_wn = ttk.Label(rm2, text="On Whiff (Normals - src/pl_act.c):")
        lbl_wn.pack(side="left")
        attach_tooltip(lbl_wn, FIELD_TOOLTIPS["whiff_normal"])
        for lbl, var in zip(["L:", "M:", "H:", "Ex:"], self.var_whiff_normal):
            ttk.Label(rm2, text=lbl).pack(side="left", padx=(4, 1))
            sp_wn = ttk.Spinbox(rm2, from_=0, to=144, textvariable=var, width=4)
            sp_wn.pack(side="left")
            attach_tooltip(sp_wn, FIELD_TOOLTIPS["whiff_normal"])
        ttk.Button(rm2, text="Apply Normals", command=self._apply_normal_whiff_table).pack(side="left", padx=6)

        rm3 = ttk.Frame(f_met)
        rm3.pack(fill="x", pady=(3, 0))
        lbl_ws = ttk.Label(rm3, text="On Whiff (Special Move):")
        lbl_ws.pack(side="left")
        attach_tooltip(lbl_ws, FIELD_TOOLTIPS["whiff_sp"])
        sp_ws = ttk.Spinbox(rm3, from_=-144, to=144, textvariable=self.var_whiff_sp_amt, width=5)
        sp_ws.pack(side="left", padx=4)
        attach_tooltip(sp_ws, FIELD_TOOLTIPS["whiff_sp"])
        ttk.Button(rm3, text="Update pl_gauge_add", command=self._apply_special_whiff).pack(side="left", padx=2)
        ttk.Label(rm3, textvariable=self.var_whiff_sp_label, foreground="#555555").pack(side="left", padx=6)

        ttk.Label(f_met, text="On Block Gain: +0 extra hit meter (block only grants startup whiff gauge if applicable; chip via Guard Flags).", foreground="#555555").pack(anchor="w", pady=(2, 0))

        # Section 3: Guard, Strength, Hitstop & Frame Timing
        f_grd = ttk.LabelFrame(p, text="4. Guard, Strength, Stun & Timing", padding=5)
        f_grd.pack(fill="x", padx=4, pady=3)

        rg1 = ttk.Frame(f_grd)
        rg1.pack(fill="x")
        ttk.Label(rg1, text="Guard:").pack(side="left")
        cb_g = ttk.Combobox(rg1, textvariable=self.var_hd_guard, values=list(GUARD_TYPE_NAMES.values()), state="readonly", width=27)
        cb_g.pack(side="left", padx=(2, 8))
        cb_g.bind("<<ComboboxSelected>>", lambda _e: self._on_hit_data_changed())
        attach_tooltip(cb_g, FIELD_TOOLTIPS["hd_guard"])

        ttk.Label(rg1, text="Strength:").pack(side="left")
        cb_s = ttk.Combobox(rg1, textvariable=self.var_hd_strength, values=list(STRENGTH_LEVEL_NAMES.values()), state="readonly", width=27)
        cb_s.pack(side="left", padx=2)
        cb_s.bind("<<ComboboxSelected>>", lambda _e: self._on_hit_data_changed())
        attach_tooltip(cb_s, FIELD_TOOLTIPS["hd_strength"])

        rg2 = ttk.Frame(f_grd)
        rg2.pack(fill="x", pady=(3, 0))
        add_chk(rg2, "Lower-Body Hit Anim (K_LOW)", self.var_hd_k_low, self._on_hit_data_changed, "hd_k_low")
        add_chk(rg2, "Max Block Push (K_BACK_MAX)", self.var_hd_k_back_max, self._on_hit_data_changed, "hd_k_back_max", padx=6)
        ttk.Label(rg2, text="Element:").pack(side="left", padx=(6, 2))
        cb_el = ttk.Combobox(rg2, textvariable=self.var_hd_element, values=list(ELEMENT_NAMES.values()), state="readonly", width=20)
        cb_el.pack(side="left")
        cb_el.bind("<<ComboboxSelected>>", lambda _e: self._on_hit_data_changed())
        attach_tooltip(cb_el, FIELD_TOOLTIPS["hd_element"])

        rg3 = ttk.Frame(f_grd)
        rg3.pack(fill="x", pady=(4, 0))
        bind_var_spin(rg3, "Hitstop:", self.var_hd_hit_stop, 0, 255, 5, "hd_hit_stop")
        bind_var_spin(rg3, "Blockstun (0=default):", self.var_hd_guard_time, 0, 255, 5, "hd_guard_time")
        ttk.Label(rg3, textvariable=self.var_hd_guard_time_eff, foreground="#0a58ca").pack(side="left")

        rg4 = ttk.Frame(f_grd)
        rg4.pack(fill="x", pady=(3, 0))
        bind_var_spin(rg4, "Air Tech Lockout:", self.var_hd_sandbag_time, 0, 255, 5, "hd_sandbag")
        bind_var_spin(rg4, "Down Time:", self.var_hd_sleep_time, 0, 255, 5, "hd_sleep")
        bind_var_spin(rg4, "Throw Invuln:", self.var_hd_catch_ng, 0, 63, 4, "hd_catch_ng")

        rg5 = ttk.Frame(f_grd)
        rg5.pack(fill="x", pady=(3, 0))
        bind_var_spin(rg5, "Dizzy/Stun:", self.var_hd_piyo, 0, 255, 4, "hd_piyo")
        bind_var_spin(rg5, "Hit Count:", self.var_hd_hit_cnt, 0, 255, 4, "hd_hit_cnt")
        bind_var_spin(rg5, "Armor Damage:", self.var_hd_jug, 0, 255, 4, "hd_jug")
        bind_var_spin(rg5, "Abyss Armor Dmg:", self.var_hd_abyss_jug, 0, 255, 4, "hd_abyss_jug")

        # Section 4: On-Hit / On-Block Launch, Trajectory, Knockback & Knockdown
        f_kb = ttk.LabelFrame(p, text="5. Knockback, Launch Trajectory & Knockdown (On Hit / On Block)", padding=5)
        f_kb.pack(fill="x", padx=4, pady=3)

        rk0 = ttk.Frame(f_kb)
        rk0.pack(fill="x")
        ttk.Label(rk0, text="Knockdown Preset:").pack(side="left")
        presets = [
            "Ground Hitstun (ZU = 0)",
            "Air Launch - Techable (BU_O = 1)",
            "Soft Knockdown - No Air Tech (dm_no = 2)",
            "Hard Knockdown - No Air/Ground Tech (dm_no = 21)",
            "Sweep Knockdown (AS = 4)",
            "Launcher - Super Jump Cancel (BLACK_COMBO)",
            "Air Combo Finisher (AERIAL_FINISH)",
            "Slam - Soft Knockdown (dm_no = 17)",
            "Dizzy Crumple (dm_no = 16)",
            "Snapback (dm_no = 12)",
        ]
        self.cb_kd_preset = ttk.Combobox(rk0, textvariable=self.var_hd_kd_preset, values=presets, state="readonly", width=40)
        self.cb_kd_preset.pack(side="left", padx=4)
        attach_tooltip(self.cb_kd_preset, FIELD_TOOLTIPS["hd_kd_preset"])
        ttk.Button(rk0, text="Apply Preset", command=self._apply_kd_preset).pack(side="left")

        rk1 = ttk.Frame(f_kb)
        rk1.pack(fill="x", pady=(4, 0))
        ttk.Label(rk1, text="Reaction Type:").pack(side="left")
        dm_vals = [pair[0] for pair in DM_TYPE_NAMES.values()]
        cb_dm = ttk.Combobox(rk1, textvariable=self.var_hd_dm_no, values=dm_vals, state="readonly", width=42)
        cb_dm.pack(side="left", padx=4, fill="x", expand=True)
        cb_dm.bind("<<ComboboxSelected>>", lambda _e: self._on_hit_data_changed())
        attach_tooltip(cb_dm, FIELD_TOOLTIPS["hd_dm_no"])
        ttk.Label(f_kb, textvariable=self.var_hd_dm_desc, foreground="#444444").pack(anchor="w", pady=(1, 2))

        rk2 = ttk.Frame(f_kb)
        rk2.pack(fill="x")
        add_chk(rk2, "Launcher (Super Jump Cancel)", self.var_hd_black_combo, self._on_hit_data_changed, "hd_black_combo")
        add_chk(rk2, "Aerial Finisher", self.var_hd_aerial_fin, self._on_hit_data_changed, "hd_aerial_fin")
        add_chk(rk2, "Camera Follow Offscreen", self.var_hd_fly_screen, self._on_hit_data_changed, "hd_fly_screen")

        rk3 = ttk.Frame(f_kb)
        rk3.pack(fill="x", pady=(2, 0))
        add_chk(rk3, "Fixed Angle (NO_VEC)", self.var_hd_no_vec, self._on_hit_data_changed, "hd_no_vec")
        add_chk(rk3, "No Corner Pushback (NO_ZU)", self.var_hd_no_zu, self._on_hit_data_changed, "hd_no_zu")
        add_chk(rk3, "Ignore Weight (NO_WEIGHT)", self.var_hd_no_weight, self._on_hit_data_changed, "hd_no_weight")

        rk4 = ttk.Frame(f_kb)
        rk4.pack(fill="x", pady=(4, 0))
        bind_var_spin(rk4, "Knockback Power (0..15):", self.var_hd_power, 0, 15, 5, "hd_power")
        bind_var_spin(rk4, "Launch Angle (0..31):", self.var_hd_vector, -128, 127, 5, "hd_vector")
        sc_vec = ttk.Scale(rk4, from_=0, to=31, variable=self.var_hd_vector, orient="horizontal", length=110, command=lambda _v: self._on_vector_slider())
        sc_vec.pack(side="left", padx=4)
        attach_tooltip(sc_vec, FIELD_TOOLTIPS["hd_vector"])

        ttk.Label(
            f_kb,
            textvariable=self.var_hd_physics_detail,
            font=("Consolas", 9),
            justify="left",
            foreground="#104e8b",
        ).pack(anchor="w", pady=(3, 0))

        # Section 5: UMvSC2 Custom Ground/Wall Bounce (src/pl_bounce.c)
        f_bnc = ttk.LabelFrame(p, text="6. Ground Bounce & Wall Bounce (src/pl_bounce.c)", padding=5)
        f_bnc.pack(fill="x", padx=4, pady=3)
        rb1 = ttk.Frame(f_bnc)
        rb1.pack(fill="x")
        ttk.Label(rb1, text="Bounce Property:").pack(side="left")
        b_vals = [f"{pid}: {sym} ({desc})" for pid, sym, desc in BOUNCE_PROP_NAMES]
        cb_b = ttk.Combobox(rb1, textvariable=self.var_bounce_prop, values=b_vals, state="readonly", width=40)
        cb_b.pack(side="left", padx=4)
        cb_b.bind("<<ComboboxSelected>>", lambda _e: self._on_bounce_changed())
        attach_tooltip(cb_b, FIELD_TOOLTIPS["hd_bounce_prop"])

        rb2 = ttk.Frame(f_bnc)
        rb2.pack(fill="x", pady=(3, 0))
        add_chk(rb2, "Require Airborne Target", self.var_bounce_need_air, self._on_bounce_changed, "hd_bounce_air")
        add_chk(rb2, "Suppress Offscreen Camera", self.var_bounce_no_fly, self._on_bounce_changed, "hd_bounce_nofly", padx=6)
        ttk.Label(rb2, text="Bounce Height:").pack(side="left", padx=(8, 2))
        e_bh = ttk.Entry(rb2, textvariable=self.var_bounce_height, width=6)
        e_bh.pack(side="left")
        e_bh.bind("<Return>", lambda _e: self._on_bounce_changed())
        e_bh.bind("<FocusOut>", lambda _e: self._on_bounce_changed())
        attach_tooltip(e_bh, FIELD_TOOLTIPS["hd_bounce_height"])

        # Section 7: Cancel Options (Current & Custom Overrides in src/pl_com.c)
        f_cnc = ttk.LabelFrame(p, text="7. Cancel Options (Current & Custom Overrides in src/pl_com.c)", padding=5)
        f_cnc.pack(fill="x", padx=4, pady=3)
        ttk.Label(f_cnc, textvariable=self.var_cancel_status, font=("Consolas", 8), justify="left", wraplength=620, foreground="#104e8b").pack(anchor="w", pady=(0, 3))
        rc1 = ttk.Frame(f_cnc)
        rc1.pack(fill="x")
        add_chk(rc1, "Grant Jump Cancel", self.var_cancel_jump, self._on_cancel_changed, "cancel_jump", padx=2)
        add_chk(rc1, "Grant Special Cancel", self.var_cancel_special, self._on_cancel_changed, "cancel_special", padx=8)
        add_chk(rc1, "Grant Hyper Cancel", self.var_cancel_hyper, self._on_cancel_changed, "cancel_hyper", padx=8)

        # Section 8: Presentation, AI & Unread Bytes
        f_ai = ttk.LabelFrame(p, text="8. Hit Spark, Sound, AI & Other Fields", padding=5)
        f_ai.pack(fill="x", padx=4, pady=3)
        ra1 = ttk.Frame(f_ai)
        ra1.pack(fill="x")
        bind_var_spin(ra1, "Hit Spark:", self.var_hd_mark, 0, 255, 5, "hd_mark")
        bind_var_spin(ra1, "Hit Sound:", self.var_hd_se, 0, 255, 5, "hd_se")
        bind_var_spin(ra1, "Score:", self.var_hd_score, 0, 255, 5, "hd_score")
        bind_var_spin(ra1, "Range Byte:", self.var_hd_range, 0, 255, 5, "hd_range")

        ra2 = ttk.Frame(f_ai)
        ra2.pack(fill="x", pady=(3, 0))
        bind_var_spin(ra2, "AI Answer:", self.var_hd_cpu_ans, 0, 255, 5, "hd_cpu_ans")
        bind_var_spin(ra2, "AI Range Box 1:", self.var_hd_rbox1, 0, 255, 5, "hd_rbox1")
        bind_var_spin(ra2, "AI Range Box 2:", self.var_hd_rbox2, 0, 255, 5, "hd_rbox2")

        ra3 = ttk.Frame(f_ai)
        ra3.pack(fill="x", pady=(3, 0))
        bind_var_spin(ra3, "Special Flag (unk_13):", self.var_hd_unk13, 0, 255, 4, "hd_unk13")
        bind_var_spin(ra3, "Hyper/Move ID (unk_15):", self.var_hd_unk15, 0, 255, 4, "hd_unk15")
        bind_var_spin(ra3, "Pad [0] (unk_1a[0]):", self.var_hd_unk1a0, 0, 255, 4, "hd_unk1a0")
        bind_var_spin(ra3, "Pad [1] (unk_1a[1]):", self.var_hd_unk1a1, 0, 255, 4, "hd_unk1a1")

    # ------------------------------------------------------------------
    # Undo / Redo & Discard Action
    # ------------------------------------------------------------------

    def _capture_snapshot(self) -> Optional[_EditorSnapshot]:
        if not self.char_data:
            return None
        seqs: Dict[str, List] = {}
        for k, seq in self.char_data.sequences.items():
            seqs[k] = [it.clone() if isinstance(it, CGFrame) else copy.copy(it) for it in seq.items]
        act_ref = (self.current_action.group, self.current_action.anim_idx) if self.current_action else None
        return _EditorSnapshot(
            sequences=seqs,
            frame_boxes=[fb.clone() for fb in self.char_data.frame_boxes],
            hit_boxes=[hb.clone() for hb in self.char_data.hit_boxes],
            hit_data=[copy.copy(hd) for hd in self.char_data.hit_data],
            bounce_rules=[copy.copy(r) for r in self.bounce_rules],
            cancel_rules=[copy.copy(r) for r in self.cancel_rules],
            atck_gauge_tbl=list(self.atck_gauge_tbl),
            edited_whiffs={k: (w, amt) for k, (w, amt) in self.edited_whiffs.items()},
            dirty_data=self.dirty_data,
            dirty_src_c=self.dirty_src_c,
            bounce_dirty=self.bounce_dirty,
            cancel_dirty=self.cancel_dirty,
            normal_whiff_dirty=self.normal_whiff_dirty,
            action_ref=act_ref,
            frame_idx=self.current_frame_idx,
            atck_no=self.current_atck_no,
            no_active_hit_data=self.no_active_hit_data,
            box_slot=self.selected_box_slot,
        )

    def _restore_snapshot(self, snap: _EditorSnapshot) -> None:
        if not self.char_data:
            return
        self.stop_play()
        for k, items in snap.sequences.items():
            if k in self.char_data.sequences:
                self.char_data.sequences[k].items = [
                    it.clone() if isinstance(it, CGFrame) else copy.copy(it) for it in items
                ]
        self.char_data.frame_boxes = [fb.clone() for fb in snap.frame_boxes]
        self.char_data.hit_boxes = [hb.clone() for hb in snap.hit_boxes]
        self.char_data.hit_data = [copy.copy(hd) for hd in snap.hit_data]
        self.bounce_rules = [copy.copy(r) for r in snap.bounce_rules]
        self.cancel_rules = [copy.copy(r) for r in snap.cancel_rules]
        self.atck_gauge_tbl = list(snap.atck_gauge_tbl)
        self.edited_whiffs = {k: (w, amt) for k, (w, amt) in snap.edited_whiffs.items()}
        self.dirty_data = snap.dirty_data
        self.dirty_src_c = snap.dirty_src_c
        self.bounce_dirty = snap.bounce_dirty
        self.cancel_dirty = snap.cancel_dirty
        self.normal_whiff_dirty = snap.normal_whiff_dirty
        self.selected_box_slot = snap.box_slot

        if snap.action_ref is not None:
            self._jump_to_state(snap.action_ref[0], snap.action_ref[1], sync_hit=False)
        self.current_frame_idx = snap.frame_idx
        self.current_tick_in_frame = 0
        self.no_active_hit_data = snap.no_active_hit_data
        self.current_atck_no = snap.atck_no
        self._refresh_atck_dropdown()
        self._sync_frame_to_ui()
        self._sync_hit_data_to_ui()
        self._refresh_timeline()
        self._refresh_move_timeline()
        self._redraw_canvas()
        self._update_undo_buttons()

    def _push_undo(self) -> None:
        snap = self._capture_snapshot()
        if snap is None:
            return
        self.undo_stack.append(snap)
        if len(self.undo_stack) > 60:
            self.undo_stack.pop(0)
        self.redo_stack.clear()
        self._update_undo_buttons()

    def _update_undo_buttons(self) -> None:
        if hasattr(self, "btn_undo"):
            self.btn_undo.configure(state="normal" if self.undo_stack else "disabled")
        if hasattr(self, "btn_redo"):
            self.btn_redo.configure(state="normal" if self.redo_stack else "disabled")

    def undo(self) -> None:
        if not self.undo_stack or not self.char_data:
            return
        cur = self._capture_snapshot()
        if cur is not None:
            self.redo_stack.append(cur)
        snap = self.undo_stack.pop()
        self._restore_snapshot(snap)
        self.set_status("Undid last change.")

    def redo(self) -> None:
        if not self.redo_stack or not self.char_data:
            return
        cur = self._capture_snapshot()
        if cur is not None:
            self.undo_stack.append(cur)
        snap = self.redo_stack.pop()
        self._restore_snapshot(snap)
        self.set_status("Redid change.")

    def discard_selected_action(self) -> None:
        if not self.char_data or not self.current_action or not self.saved_baseline:
            return
        act = self.current_action
        base = self.saved_baseline
        self._push_undo()

        # Revert sequence frames
        if act.seq_name in base.sequences and act.seq_name in self.char_data.sequences:
            self.char_data.sequences[act.seq_name].items = [
                it.clone() if isinstance(it, CGFrame) else copy.copy(it)
                for it in base.sequences[act.seq_name]
            ]

        # Revert FrameBoxes & HitBoxes referenced by the restored (and current) sequence
        fb_indices: Set[int] = set()
        for fr in self.char_data.sequences[act.seq_name].frames():
            fb_indices.add(fr.box)
        hb_indices: Set[int] = set()
        for fb_idx in fb_indices:
            if 0 <= fb_idx < len(base.frame_boxes) and fb_idx < len(self.char_data.frame_boxes):
                self.char_data.frame_boxes[fb_idx] = base.frame_boxes[fb_idx].clone()
                fb = self.char_data.frame_boxes[fb_idx]
                slots = [fb.atck_box, fb.atck_box2, fb.body_box0, fb.body_box1, fb.body_box2, fb.body_box3, fb.catch_box]
                if 0 < fb.catch_box2 < 0x8000:
                    slots.append(fb.catch_box2)
                for slot_val in slots:
                    hi = slot_val & BOX_MASK
                    if hi > 0:
                        hb_indices.add(hi)
        for hb_idx in hb_indices:
            if 0 <= hb_idx < len(base.hit_boxes) and hb_idx < len(self.char_data.hit_boxes):
                self.char_data.hit_boxes[hb_idx] = base.hit_boxes[hb_idx].clone()

        # Revert linked HitData and bounce rules
        atck_ids: Set[int] = set(act.atck_candidates)
        if not self.no_active_hit_data and 0 <= self.current_atck_no < len(self.char_data.hit_data):
            atck_ids.add(self.current_atck_no)
        for aid in atck_ids:
            if 0 <= aid < len(base.hit_data) and aid < len(self.char_data.hit_data):
                self.char_data.hit_data[aid] = copy.copy(base.hit_data[aid])
            self.bounce_rules = [r for r in self.bounce_rules if not (r.pl_type == self.char_id and r.atck_no == aid)]
            for br in base.bounce_rules:
                if br.pl_type == self.char_id and br.atck_no == aid:
                    self.bounce_rules.append(copy.copy(br))

        # Revert whiff edits tied to this action
        for w in act.whiff_entries:
            key = (str(w.file_path), w.line_no)
            self.edited_whiffs.pop(key, None)

        self.current_frame_idx = 0
        self.current_tick_in_frame = 0
        self._refresh_atck_dropdown()
        self._sync_frame_to_ui()
        self._sync_hit_data_to_ui()
        self._refresh_timeline()
        self._refresh_move_timeline()
        self._redraw_canvas()
        self.set_status(f"Discarded unsaved changes for [{act.group:02d}:{act.anim_idx:02d}] {act.seq_name} (Ctrl+Z to undo).")

    # ------------------------------------------------------------------
    # Loading & Navigation
    # ------------------------------------------------------------------

    def load_char(self, char_id: int, prompt_dirty: bool = True) -> None:
        if prompt_dirty and (self.dirty_data or self.dirty_src_c) and self.char_data is not None:
            if not messagebox.askyesno("Unsaved Changes", "You have unsaved changes. Discard and switch character?"):
                # Revert char combobox display
                name, _ = CHAR_BY_ID[self.char_id]
                self.var_char.set(f"{self.char_id:02X}: {name}")
                return

        self.stop_play()
        self.char_id = char_id
        name, _ = CHAR_BY_ID.get(char_id, (f"pl{char_id:02x}", ""))
        self.var_char.set(f"{char_id:02X}: {name}")

        self.char_data = load_character(self.repo_root, char_id)
        self.chain_tables = parse_chain_tables(self.repo_root, char_id)
        acts_meta = [
            (a.group, a.anim_idx, a.seq_name, self.char_data.sequences[a.seq_name].comment if a.seq_name in self.char_data.sequences else "")
            for a in self.char_data.actions
        ]
        self.char_catalog = build_catalog(self.repo_root, char_id, acts_meta)
        dat_path = self.repo_root / "ports" / "build-dist" / "disc" / f"PL{char_id:02X}_DAT.BIN"
        self.pl_archive = PlDatArchive(dat_path) if dat_path.exists() else None
        self.sprite_cache.clear()
        self.dirty_data = False
        self.dirty_src_c = False
        self.bounce_dirty = False
        self.cancel_dirty = False
        self.normal_whiff_dirty = False
        self.edited_whiffs.clear()
        self.undo_stack.clear()
        self.redo_stack.clear()
        self.current_move = None
        self.current_version_key = "*"
        self.no_active_hit_data = False

        self.var_category.set("All Categories")
        self._refresh_move_dropdown()
        self._refresh_action_list()
        self._refresh_atck_dropdown()
        self._center_canvas()
        self.saved_baseline = self._capture_snapshot()
        self._update_undo_buttons()
        self.set_status(f"Loaded {char_id:02X}: {name} ({len(self.char_data.actions)} animations/states, {len(self.char_catalog.moves)} moves)")

    def _on_char_selected(self, _evt=None) -> None:
        s = self.var_char.get()
        if not s:
            return
        cid = int(s.split(":", 1)[0], 16)
        if cid != self.char_id or self.char_data is None:
            self.load_char(cid)

    def _on_palette_selected(self, _evt=None) -> None:
        idx = self.cb_pal.current()
        self.palette_idx = max(0, min(5, idx))
        self.sprite_cache.clear()
        self._redraw_canvas()

    def _action_dropdown_label(self, act: ActionEntry) -> str:
        if self.char_catalog:
            st_lbl = self.char_catalog.state_label(act.group, act.anim_idx)
            if st_lbl:
                return f"[{act.group:02d}:{act.anim_idx:02d}] {st_lbl}"
        return act.display_label

    def _refresh_action_list(self, keep_current: bool = False) -> None:
        if not self.char_data:
            return
        cat_str = self.var_category.get()
        grp_filter: Optional[int] = None
        if cat_str != "All Categories" and ":" in cat_str:
            grp_filter = int(cat_str.split(":", 1)[0])
        self.filtered_actions = [
            act for act in self.char_data.actions if grp_filter is None or act.group == grp_filter
        ]
        vals = [self._action_dropdown_label(act) for act in self.filtered_actions]
        self.cb_action.configure(values=vals)
        if not self.filtered_actions:
            return
        if keep_current and self.current_action in self.filtered_actions:
            pick = self.filtered_actions.index(self.current_action)
            self.cb_action.current(pick)
            return
        # Prefer standing idle (0, 0) on fresh load if present
        pick = next((i for i, a in enumerate(self.filtered_actions) if (a.group, a.anim_idx) == (0, 0)), 0)
        self.cb_action.current(pick)
        self._select_action(self.filtered_actions[pick])

    def _on_action_selected(self, _evt=None) -> None:
        idx = self.cb_action.current()
        if 0 <= idx < len(self.filtered_actions):
            self._select_action(self.filtered_actions[idx])

    def _jump_to_state(self, group: int, anim_idx: int, sync_hit: bool = True) -> bool:
        if not self.char_data:
            return False
        target = next((a for a in self.char_data.actions if a.group == group and a.anim_idx == anim_idx), None)
        if target is None:
            return False
        if target not in self.filtered_actions:
            self.var_category.set("All Categories")
            self._refresh_action_list(keep_current=True)
        if target in self.filtered_actions:
            self.cb_action.current(self.filtered_actions.index(target))
        self._select_action(target, sync_hit=sync_hit)
        return True

    def _refresh_move_dropdown(self) -> None:
        if not self.char_catalog:
            self.cb_move.configure(values=["(None)"])
            self.var_move.set("(None)")
            return
        vals = ["(All / None)"] + [m.dropdown_label() for m in self.char_catalog.moves]
        self.cb_move.configure(values=vals)
        if self.current_move and self.current_move in self.char_catalog.moves:
            self.cb_move.current(self.char_catalog.moves.index(self.current_move) + 1)
        else:
            self.cb_move.current(0)

    def _refresh_version_dropdown(self) -> None:
        if not self.current_move or not self.current_move.versions:
            self.cb_version.configure(values=["All versions"])
            self.var_version.set("All versions")
            self.current_version_key = "*"
            return
        vals = [v.label for v in self.current_move.versions]
        self.cb_version.configure(values=vals)
        pick = next((i for i, v in enumerate(self.current_move.versions) if v.key == self.current_version_key), 0)
        self.cb_version.current(pick)
        self.current_version_key = self.current_move.versions[pick].key

    def _on_move_selected(self, _evt=None) -> None:
        if not self.char_catalog:
            return
        idx = self.cb_move.current()
        if idx <= 0:
            self.current_move = None
            self._refresh_version_dropdown()
            self._refresh_move_timeline()
            return
        mv = self.char_catalog.moves[idx - 1]
        self.current_move = mv
        self.current_version_key = mv.versions[0].key if mv.versions else "*"
        self._refresh_version_dropdown()
        if mv.states:
            # Prefer first non-effect state with attack frames, else first state
            v_states = mv.states_for(self.current_version_key) or mv.states
            prim = self._pick_primary_state(v_states)
            self._jump_to_state(prim.group, prim.anim, sync_hit=self.var_sync_action_hit.get())
        else:
            self._refresh_move_timeline()

    def _pick_primary_state(self, states: List[StateInfo]) -> StateInfo:
        if self.char_data:
            for st in states:
                if st.effect:
                    continue
                act = next((a for a in self.char_data.actions if a.group == st.group and a.anim_idx == st.anim), None)
                if act and act.seq_name in self.char_data.sequences:
                    for fr in self.char_data.sequences[act.seq_name].frames():
                        fb = self._get_frame_box(fr.box)
                        if (fr.atck & ATK) or (fb.atck_box & BOX_MASK) or (fb.atck_box2 & BOX_MASK):
                            return st
        return next((s for s in states if not s.effect), states[0])

    def _on_version_selected(self, _evt=None) -> None:
        if not self.current_move or not self.current_move.versions:
            return
        idx = self.cb_version.current()
        if 0 <= idx < len(self.current_move.versions):
            self.current_version_key = self.current_move.versions[idx].key
            # If current state isn't valid for this version, switch to matching state
            if self.current_action:
                cur_ref = (self.current_action.group, self.current_action.anim_idx)
                v_states = self.current_move.states_for(self.current_version_key)
                if v_states and not any(s.ref == cur_ref for s in v_states):
                    prim = self._pick_primary_state(v_states)
                    self._jump_to_state(prim.group, prim.anim, sync_hit=self.var_sync_action_hit.get())
                    return
            if self.var_sync_action_hit.get():
                hid = self.current_move.primary_hit(self.current_version_key)
                if hid is not None:
                    self._select_atck_no(hid, from_sync=True)
            self._refresh_move_timeline()

    def _on_sync_toggled(self) -> None:
        if self.var_sync_action_hit.get() and self.current_action:
            self._sync_hit_from_action(self.current_action)

    def _sync_hit_from_action(self, act: ActionEntry) -> None:
        chosen: Optional[int] = None
        # 1) Check move catalog state hits for current version
        if self.current_move:
            for st in self.current_move.states:
                if st.ref == (act.group, act.anim_idx):
                    v_hits = st.hits.get(self.current_version_key) or st.hits.get("*")
                    if not v_hits and st.hits:
                        v_hits = next(iter(st.hits.values()))
                    if v_hits:
                        chosen = v_hits[0]
                    break
        # 2) Fallback to ActionEntry's detected candidates
        if chosen is None and act.atck_candidates:
            chosen = act.atck_candidates[0]

        if chosen is not None and self.char_data and 0 <= chosen < len(self.char_data.hit_data):
            self.no_active_hit_data = False
            self._select_atck_no(chosen, from_sync=True)
        else:
            self.no_active_hit_data = True
            self._sync_whiff_special_ui()
            self._update_physics_readouts()

    def _select_action(self, act: ActionEntry, sync_hit: bool = True) -> None:
        self.stop_play()
        self.current_action = act
        self.current_seq = self.char_data.sequences.get(act.seq_name) if self.char_data else None
        self.current_frame_idx = 0
        self.current_tick_in_frame = 0

        # Update Move & Version dropdowns if this state belongs to a cataloged move
        if self.char_catalog:
            matches = self.char_catalog.state_info(act.group, act.anim_idx)
            if matches:
                if self.current_move and any(m is self.current_move for m, _ in matches):
                    mv, st = next((m, s) for m, s in matches if m is self.current_move)
                else:
                    mv, st = matches[0]
                    self.current_move = mv
                if st.versions and self.current_version_key not in st.versions:
                    self.current_version_key = next((v.key for v in mv.versions if v.key in st.versions), mv.versions[0].key if mv.versions else "*")
            else:
                self.current_move = None
                self.current_version_key = "*"
            self._refresh_move_dropdown()
            self._refresh_version_dropdown()

        self._sync_frame_to_ui()
        self._refresh_timeline()
        self._refresh_move_timeline()

        if sync_hit and self.var_sync_action_hit.get():
            self._sync_hit_from_action(act)
        else:
            self.no_active_hit_data = False
            self._sync_whiff_special_ui()
        self._sync_cancel_to_ui()
        self._redraw_canvas()

    def _refresh_move_timeline(self) -> None:
        for w in self.move_states_bar.winfo_children():
            w.destroy()
        mv = self.current_move
        if not mv or not self.char_data:
            self.var_move_timing.set("No Move selected (standalone state).")
            self.var_move_notes.set("")
            return

        # Sum timing across main (non-effect) states for current version
        states = mv.states_for(self.current_version_key) or mv.states
        total_f = 0
        main_count = 0
        for st in states:
            if st.effect:
                continue
            main_count += 1
            act = next((a for a in self.char_data.actions if a.group == st.group and a.anim_idx == st.anim), None)
            if act and act.seq_name in self.char_data.sequences:
                total_f += sum(self._frame_dur(fr) for fr in self.char_data.sequences[act.seq_name].frames())

        timer_bits: List[str] = []
        for t in mv.timers:
            val = t.values.get(self.current_version_key)
            if val is None and t.values:
                val = next(iter(t.values.values()))
            if val is not None:
                timer_bits.append(f"{t.func}:{t.line} {t.field_name}={val}F")
        timer_str = (" | Code timers: " + ", ".join(timer_bits)) if timer_bits else ""
        self.var_move_timing.set(f"{mv.name} [{self.current_version_key}]: {main_count} main states, {total_f}F anim total{timer_str}")

        notes = list(mv.notes)
        self.var_move_notes.set(" • ".join(notes[:3]) if notes else "")

        # Clickable state buttons
        cur_ref = (self.current_action.group, self.current_action.anim_idx) if self.current_action else None
        for idx, st in enumerate(states[:14]):
            short = st.label.split(":", 1)[-1].strip() if ":" in st.label else f"{st.group}:{st.anim:02d}"
            if len(short) > 22:
                short = short[:21] + "…"
            prefix = "▶ " if st.ref == cur_ref else ""
            btn = ttk.Button(
                self.move_states_bar,
                text=f"{prefix}{st.group}:{st.anim:02d} {short}",
                command=lambda g=st.group, a=st.anim: self._jump_to_state(g, a, sync_hit=self.var_sync_action_hit.get()),
            )
            btn.pack(side="left", padx=1, pady=1)
            attach_tooltip(btn, f"{st.label}\nFuncs: {', '.join(st.funcs)}")

    def _scale_current_state_duration(self, mult: float, delta: int) -> None:
        if not self.current_seq:
            return
        frs = self._current_frames()
        if not frs:
            return
        self._push_undo()
        changed = False
        for fr in frs:
            c = fr.ctr & 0xFF
            if fr.ctr < 0 or c == 255:
                continue  # preserve hold sentinel
            new_c = max(1, min(254, int(round(c * mult)) + delta))
            if new_c != c:
                fr.ctr = new_c
                changed = True
        if changed:
            self.dirty_data = True
            self._sync_frame_to_ui()
            self._refresh_timeline()
            self._refresh_move_timeline()
            self.set_status("Adjusted frame durations for current state (Ctrl+Z to undo).")

    def _refresh_atck_dropdown(self) -> None:
        if not self.char_data:
            return
        vals: List[str] = []
        for idx, hd in enumerate(self.char_data.hit_data):
            cat_lbl = self.char_catalog.hit_label(idx) if self.char_catalog else ""
            nm = cat_lbl or hd.comment or self.char_data.atck_names.get(idx, "")
            tag = "[empty]" if hd.is_empty() else f"dmg={hd.damage}"
            vals.append(f"0x{idx:02X} ({idx:3d}) {tag} — {nm[:56]}")
        self.cb_atck.configure(values=vals)
        if 0 <= self.current_atck_no < len(vals):
            self.cb_atck.current(self.current_atck_no)

    def _on_atck_selected(self, _evt=None) -> None:
        idx = self.cb_atck.current()
        if idx >= 0:
            self._select_atck_no(idx, from_sync=False)

    def _select_atck_no(self, atck_no: int, from_sync: bool = False) -> None:
        if not self.char_data:
            return
        if 0 <= atck_no < len(self.char_data.hit_data):
            self.no_active_hit_data = False
            self.current_atck_no = atck_no
            self._refresh_atck_dropdown()
            self.cb_atck.current(atck_no)
            self._sync_hit_data_to_ui()
            self._redraw_canvas()

            if not from_sync and self.var_sync_action_hit.get():
                # Jump to the move's primary state for this atck_no
                if self.char_catalog:
                    mvs = self.char_catalog.moves_for_hit(atck_no)
                    if mvs:
                        mv = mvs[0]
                        self.current_move = mv
                        # Pick a version that uses this atck_no
                        hi = next((h for h in mv.hits if h.atck_no == atck_no), None)
                        if hi and hi.versions:
                            self.current_version_key = next((v.key for v in mv.versions if v.key in hi.versions), self.current_version_key)
                        v_states = mv.states_for(self.current_version_key) or mv.states
                        if v_states:
                            prim = self._pick_primary_state(v_states)
                            self._jump_to_state(prim.group, prim.anim, sync_hit=False)
                            return
                # Fallback: first ActionEntry listing this atck_no
                cand = next((a for a in self.char_data.actions if atck_no in a.atck_candidates), None)
                if cand is not None:
                    self._jump_to_state(cand.group, cand.anim_idx, sync_hit=False)

    def _add_hit_data_entry(self) -> None:
        if not self.char_data:
            return
        self._push_undo()
        # Find first empty slot >= 0x12 or append
        slot = next((i for i in range(0x12, len(self.char_data.hit_data)) if self.char_data.hit_data[i].is_empty()), -1)
        if slot < 0:
            slot = len(self.char_data.hit_data)
            self.char_data.hit_data.append(HitData(damage=10, hit_stop=10, guard_time=0, damage_kind=0x20, hit_mark=1, score=8, hit_se=6, comment="Custom Attack"))
        else:
            self.char_data.hit_data[slot] = HitData(damage=10, hit_stop=10, guard_time=0, damage_kind=0x20, hit_mark=1, score=8, hit_se=6, comment="Custom Attack")
        self.dirty_data = True
        self._refresh_atck_dropdown()
        self._select_atck_no(slot, from_sync=True)
        self.set_status(f"Created new attack entry at atck_no 0x{slot:02X} ({slot}).")

    # ------------------------------------------------------------------
    # Frame Helpers & Animation Player
    # ------------------------------------------------------------------

    def _current_frames(self) -> List[CGFrame]:
        return self.current_seq.frames() if self.current_seq else []

    def _current_frame(self) -> Optional[CGFrame]:
        frs = self._current_frames()
        if not frs:
            return None
        if self.current_frame_idx >= len(frs):
            self.current_frame_idx = len(frs) - 1
        if self.current_frame_idx < 0:
            self.current_frame_idx = 0
        return frs[self.current_frame_idx]

    def _frame_dur(self, fr: CGFrame) -> int:
        c = fr.ctr & 0xFF
        if c == 0:
            return 1
        if fr.ctr < 0 or c == 255:
            return 30  # Hold frame sentinel (-1) preview duration
        return c

    def _compute_timing_summary(self) -> None:
        frs = self._current_frames()
        if not frs:
            self.var_timing_summary.set("No frames")
            return
        total = 0
        startup: Optional[int] = None
        active = 0
        recovery = 0
        seen_active = False
        cur_tick = 1
        for i, fr in enumerate(frs):
            dur = self._frame_dur(fr)
            fb = self._get_frame_box(fr.box)
            has_hitbox = bool((fb.atck_box & BOX_MASK) or (fb.atck_box2 & BOX_MASK) or (fr.atck & ATK))
            is_ate = bool(fr.atck & ATE)
            if has_hitbox and not is_ate:
                if startup is None:
                    startup = cur_tick
                seen_active = True
                active += dur
            elif seen_active or is_ate:
                recovery += dur
            total += dur
            cur_tick += dur
        if startup is not None:
            self.var_timing_summary.set(
                f"Startup: {startup}F | Active: {active}F | Recovery: {recovery}F | Total: {total}F"
            )
        else:
            self.var_timing_summary.set(f"Non-attack / Neutral | Frames: {len(frs)} | Total: {total}F")

    def _sync_frame_to_ui(self) -> None:
        frs = self._current_frames()
        fr = self._current_frame()
        self._compute_timing_summary()
        if not fr:
            self.var_frame_status.set("No animation frames")
            return

        total_ticks = sum(self._frame_dur(f) for f in frs)
        tick_pos = sum(self._frame_dur(frs[i]) for i in range(self.current_frame_idx)) + self.current_tick_in_frame + 1
        dur = self._frame_dur(fr)
        hold_note = " (HOLD)" if (fr.ctr & 0xFF) == 255 else ""
        self.var_frame_status.set(
            f"Step {self.current_frame_idx + 1}/{len(frs)} (step tick {self.current_tick_in_frame + 1}/{dur}{hold_note}) | Total {tick_pos}/{total_ticks}F"
        )

        # If this active frame carries a specific multi-hit atck_no in flag1, display hint/switch if appropriate
        self._suppress_callbacks = True
        try:
            self.var_fr_ctr.set(fr.ctr if fr.ctr < 128 else fr.ctr - 256)
            self.var_fr_spr.set(fr.spr)
            self.var_fr_box.set(fr.box)
            self.var_fr_atk.set(bool(fr.atck & ATK))
            self.var_fr_ate.set(bool(fr.atck & ATE))
            self.var_fr_ng.set(bool(fr.atck & CG_ATCK_NG))
            self.var_fr_flag1.set(fr.flag1)
            self.var_fr_se.set(fr.se)
            self.var_fr_zoom.set(fr.zoom)
            self.var_fr_prm.set(fr.prm if fr.prm < 128 else fr.prm - 256)
            self.var_fr_type.set(fr.ftype if fr.ftype < 128 else fr.ftype - 256)
            self.var_fr_flag2.set(fr.flag2)
            self.var_fr_rival.set(fr.rival)
        finally:
            self._suppress_callbacks = False

        # Auto-pick meaningful hitbox slot if current has 0
        fb = self._get_frame_box(fr.box)
        if self.selected_box_slot != "push_box" and (self._get_slot_raw(fb, self.selected_box_slot) & BOX_MASK) == 0:
            for s in BOX_SLOT_ORDER[:-1]:
                if (self._get_slot_raw(fb, s) & BOX_MASK) != 0:
                    self.selected_box_slot = s
                    break
        self.var_hb_slot.set(self.selected_box_slot)
        self._sync_hitbox_fields_to_ui()

    def _on_frame_fields_changed(self) -> None:
        if self._suppress_callbacks:
            return
        fr = self._current_frame()
        if not fr:
            return
        self._push_undo()
        fr.ctr = int(self.var_fr_ctr.get()) & 0xFF
        fr.spr = int(self.var_fr_spr.get())
        fr.box = max(0, int(self.var_fr_box.get()))
        if self.char_data:
            while fr.box >= len(self.char_data.frame_boxes):
                self.char_data.frame_boxes.append(FrameBox())
        atck = 0
        if self.var_fr_atk.get():
            atck |= ATK
        if self.var_fr_ate.get():
            atck |= ATE
        if self.var_fr_ng.get():
            atck |= CG_ATCK_NG
        fr.atck = atck
        fr.flag1 = int(self.var_fr_flag1.get()) & 0xFF
        fr.se = int(self.var_fr_se.get()) & 0xFF
        fr.zoom = int(self.var_fr_zoom.get()) & 0xFF
        fr.prm = int(self.var_fr_prm.get()) & 0xFF
        fr.ftype = int(self.var_fr_type.get()) & 0xFF
        fr.flag2 = int(self.var_fr_flag2.get()) & 0xFF
        fr.rival = int(self.var_fr_rival.get()) & 0xFFFF
        self.dirty_data = True
        self._sync_frame_to_ui()
        self._refresh_timeline()
        self._redraw_canvas()

    def _duplicate_current_frame(self) -> None:
        if not self.current_seq:
            return
        fr = self._current_frame()
        if not fr:
            return
        self._push_undo()
        # Find position in seq.items
        seen = -1
        for i, it in enumerate(self.current_seq.items):
            if isinstance(it, CGFrame):
                seen += 1
                if seen == self.current_frame_idx:
                    dup = fr.clone()
                    # Clear END_OF_DATA on earlier frame if inserting before last
                    if dup.end & 0x80:
                        dup.end &= ~0x80
                    self.current_seq.items.insert(i, dup)
                    self.dirty_data = True
                    self.current_frame_idx += 1
                    self._sync_frame_to_ui()
                    self._refresh_timeline()
                    self._redraw_canvas()
                    return

    def _delete_current_frame(self) -> None:
        if not self.current_seq:
            return
        frs = self._current_frames()
        if len(frs) <= 1:
            messagebox.showinfo("Delete Frame", "Cannot delete the only frame in a sequence.")
            return
        self._push_undo()
        seen = -1
        for i, it in enumerate(self.current_seq.items):
            if isinstance(it, CGFrame):
                seen += 1
                if seen == self.current_frame_idx:
                    was_end = bool(it.end & 0x80)
                    self.current_seq.items.pop(i)
                    rem = self.current_seq.frames()
                    if was_end and rem:
                        rem[-1].end |= 0x80
                    self.current_frame_idx = min(self.current_frame_idx, len(rem) - 1)
                    self.current_tick_in_frame = 0
                    self.dirty_data = True
                    self._sync_frame_to_ui()
                    self._refresh_timeline()
                    self._redraw_canvas()
                    return

    def _refresh_timeline(self) -> None:
        self.timeline.delete("all")
        frs = self._current_frames()
        if not frs:
            return
        w = max(200, self.timeline.winfo_width() or 600)
        h = max(18, self.timeline.winfo_height() or 22)
        durs = [self._frame_dur(fr) for fr in frs]
        total = max(1, sum(durs))
        x = 0.0
        for idx, (fr, dur) in enumerate(zip(frs, durs)):
            bw = (dur / total) * w
            fb = self._get_frame_box(fr.box)
            if (fr.atck & ATK) or (fb.atck_box & BOX_MASK) or (fb.atck_box2 & BOX_MASK):
                col = "#d9383a"
            elif fr.atck & ATE:
                col = "#2f9e44"
            elif fr.atck & CG_ATCK_NG:
                col = "#e67700"
            else:
                col = "#495057"
            outline = "#ffffff" if idx == self.current_frame_idx else "#111111"
            lw = 2 if idx == self.current_frame_idx else 1
            self.timeline.create_rectangle(x, 1, x + bw, h - 1, fill=col, outline=outline, width=lw)
            if bw >= 16:
                self.timeline.create_text(x + bw / 2, h / 2, text=f"{dur}", fill="#ffffff", font=("Consolas", 8))
            x += bw

    def _on_timeline_click(self, event) -> None:
        frs = self._current_frames()
        if not frs:
            return
        w = max(1, self.timeline.winfo_width())
        durs = [self._frame_dur(fr) for fr in frs]
        total = sum(durs)
        target_tick = (event.x / w) * total
        acc = 0
        for idx, dur in enumerate(durs):
            if target_tick <= acc + dur or idx == len(durs) - 1:
                self.current_frame_idx = idx
                self.current_tick_in_frame = max(0, min(dur - 1, int(target_tick - acc)))
                self._sync_frame_to_ui()
                self._refresh_timeline()
                self._redraw_canvas()
                return
            acc += dur

    def first_frame(self) -> None:
        self.stop_play()
        self.current_frame_idx = 0
        self.current_tick_in_frame = 0
        self._sync_frame_to_ui()
        self._refresh_timeline()
        self._redraw_canvas()

    def last_frame(self) -> None:
        self.stop_play()
        frs = self._current_frames()
        if not frs:
            return
        self.current_frame_idx = len(frs) - 1
        self.current_tick_in_frame = 0
        self._sync_frame_to_ui()
        self._refresh_timeline()
        self._redraw_canvas()

    def prev_frame(self) -> None:
        self.stop_play()
        frs = self._current_frames()
        if not frs:
            return
        self.current_frame_idx = (self.current_frame_idx - 1) % len(frs)
        self.current_tick_in_frame = 0
        self._sync_frame_to_ui()
        self._refresh_timeline()
        self._redraw_canvas()

    def next_frame(self) -> None:
        self.stop_play()
        frs = self._current_frames()
        if not frs:
            return
        self.current_frame_idx = (self.current_frame_idx + 1) % len(frs)
        self.current_tick_in_frame = 0
        self._sync_frame_to_ui()
        self._refresh_timeline()
        self._redraw_canvas()

    def prev_tick(self) -> None:
        self.stop_play()
        frs = self._current_frames()
        if not frs:
            return
        if self.current_tick_in_frame > 0:
            self.current_tick_in_frame -= 1
        else:
            self.current_frame_idx = (self.current_frame_idx - 1) % len(frs)
            self.current_tick_in_frame = max(0, self._frame_dur(frs[self.current_frame_idx]) - 1)
        self._sync_frame_to_ui()
        self._refresh_timeline()
        self._redraw_canvas()

    def next_tick(self) -> None:
        self.stop_play()
        self._advance_ticks(1)

    def _advance_ticks(self, ticks: int) -> None:
        frs = self._current_frames()
        if not frs or ticks <= 0:
            return
        changed_frame = False
        for _ in range(ticks):
            dur = self._frame_dur(frs[self.current_frame_idx])
            self.current_tick_in_frame += 1
            if self.current_tick_in_frame >= dur:
                self.current_tick_in_frame = 0
                if self.current_frame_idx + 1 < len(frs):
                    self.current_frame_idx += 1
                    changed_frame = True
                elif self.var_loop.get():
                    self.current_frame_idx = 0
                    changed_frame = True
                else:
                    self.current_tick_in_frame = dur - 1
                    self.stop_play()
                    break
        self._sync_frame_to_ui()
        self._refresh_timeline()
        if changed_frame or not self.is_playing:
            self._redraw_canvas()

    def toggle_play(self) -> None:
        if self.is_playing:
            self.stop_play()
        else:
            if not self._current_frames():
                return
            import time
            self.is_playing = True
            self.btn_play.configure(text="Pause")
            self._tick_accum = 0.0
            self._last_play_time = time.perf_counter()
            self._play_step()

    def stop_play(self) -> None:
        self.is_playing = False
        if self._play_after_id is not None:
            try:
                self.root.after_cancel(self._play_after_id)
            except Exception:
                pass
            self._play_after_id = None
        self.btn_play.configure(text="Play")

    def _play_step(self) -> None:
        if not self.is_playing:
            return
        import time
        now = time.perf_counter()
        dt = min(0.1, max(0.001, now - self._last_play_time))
        self._last_play_time = now
        speed = max(0.01, float(self.var_speed.get() or 1.0))
        self._tick_accum += dt * 60.0 * speed
        step = int(self._tick_accum)
        if step >= 1:
            self._tick_accum -= step
            self._advance_ticks(step)
        if self.is_playing:
            self._play_after_id = self.root.after(16, self._play_step)

    # ------------------------------------------------------------------
    # Canvas Rendering & Interactive Hitbox Drag/Drop
    # ------------------------------------------------------------------

    def _aspect_scales(self) -> Tuple[float, float]:
        z = max(1, int(self.var_zoom.get() or 3))
        if self.var_aspect_43.get():
            return z * 1.0, z * (DOT_Y_SCALE / DOT_X_SCALE)  # Relative aspect 1.2857x taller, preserving dot readability
        return float(z), float(z)

    def _center_canvas(self) -> None:
        cw = self.canvas.winfo_width() or 680
        ch = self.canvas.winfo_height() or 520
        self.origin_px_x = cw * 0.45
        self.origin_px_y = ch * 0.78
        self._redraw_canvas()

    def _get_frame_box(self, fb_idx: int) -> FrameBox:
        if not self.char_data or fb_idx < 0 or fb_idx >= len(self.char_data.frame_boxes):
            return FrameBox()
        return self.char_data.frame_boxes[fb_idx]

    def _get_slot_raw(self, fb: FrameBox, slot: str) -> int:
        if slot == "push_box":
            return PUSH_BOX_BASE + self.selected_push_stance
        return getattr(fb, slot, 0)

    def _set_slot_raw(self, fb: FrameBox, slot: str, value: int) -> None:
        if slot != "push_box":
            setattr(fb, slot, value & 0xFFFF)

    def _resolve_box_for_slot(self, fb: FrameBox, slot: str) -> Tuple[int, Optional[HitBox]]:
        if not self.char_data:
            return 0, None
        raw = self._get_slot_raw(fb, slot)
        hb_idx = raw if slot == "push_box" else (raw & BOX_MASK)
        if hb_idx <= 0 or hb_idx >= len(self.char_data.hit_boxes):
            return hb_idx, None
        return hb_idx, self.char_data.hit_boxes[hb_idx]

    def _dots_rect_for_box(self, hb: HitBox, is_push: bool) -> Tuple[float, float, float, float]:
        """Return (left, top, right, bottom) in viewer dot space around origin (0,0) (with Face-Right mirror if active)."""
        # Native coords: x negative = forward (left), y negative = up (pushbox y is positive up so negated like hit_box_disp)
        ny = -hb.y if is_push else hb.y
        nx = -hb.x if self.var_face_right.get() else hb.x
        return (float(nx - hb.w), float(ny - hb.h), float(nx + hb.w), float(ny + hb.h))

    def _redraw_canvas(self) -> None:
        self.canvas.delete("all")
        self.box_screen_rects.clear()
        cw = self.canvas.winfo_width() or 680
        ch = self.canvas.winfo_height() or 520
        sx, sy = self._aspect_scales()
        ox, oy = self.origin_px_x, self.origin_px_y

        # Background grid + ground line + origin crosshairs
        step_x = 32 * sx
        step_y = 32 * sy
        if step_x >= 16:
            gx = ox % step_x
            while gx < cw:
                self.canvas.create_line(gx, 0, gx, ch, fill="#22252e")
                gx += step_x
        if step_y >= 16:
            gy = oy % step_y
            while gy < ch:
                self.canvas.create_line(0, gy, cw, gy, fill="#22252e")
                gy += step_y

        self.canvas.create_line(0, oy, cw, oy, fill="#4c566a", width=2)
        self.canvas.create_line(ox, 0, ox, ch, fill="#3b4252", dash=(4, 4))

        fr = self._current_frame()
        if not fr or not self.char_data:
            return

        # Render sprite
        face_right = self.var_face_right.get()
        if self.pl_archive and fr.spr >= 0:
            prm = self.char_data.spr_prm[fr.zoom] if 0 <= fr.zoom < len(self.char_data.spr_prm) else None
            key = (fr.spr, self.palette_idx, face_right, fr.zoom, round(sx, 3), round(sy, 3))
            img = self.sprite_cache.get(key)
            if img is None:
                rendered = self.pl_archive.render_sprite(fr.spr, self.palette_idx, flip_h=face_right, spr_prm=prm)
                if rendered is not None:
                    rw, rh, rgba, rox, roy = rendered
                    rw_s, rh_s, rgba_s, _, _ = scale_rgba_nearest(rw, rh, rgba, rox, roy, sx, sy)
                    png = rgba_to_png_bytes(rw_s, rh_s, rgba_s)
                    img = tk.PhotoImage(data=png, format="png")
                    if len(self.sprite_cache) > 250:
                        self.sprite_cache.clear()
                    self.sprite_cache[key] = img
                    # Cache anchor offsets on image object
                    img._rox_s = rox * sx  # type: ignore[attr-defined]
                    img._roy_s = roy * sy  # type: ignore[attr-defined]
            if img is not None:
                rox_s = getattr(img, "_rox_s", 0.0)
                roy_s = getattr(img, "_roy_s", 0.0)
                self.canvas.create_image(ox - rox_s, oy - roy_s, image=img, anchor="nw")

        # Draw origin marker on top of sprite base
        self.canvas.create_oval(ox - 4, oy - 4, ox + 4, oy + 4, fill="#eceff4", outline="#181a20")

        # Draw collision boxes
        fb = self._get_frame_box(fr.box)
        draw_order = [
            "push_box",
            "catch_box",
            "body_box0",
            "body_box1",
            "body_box2",
            "body_box3",
            "atck_box2",
            "atck_box",
        ]
        # Draw selected box last so handles sit on top
        if self.selected_box_slot in draw_order:
            draw_order.remove(self.selected_box_slot)
            draw_order.append(self.selected_box_slot)

        atk_center: Optional[Tuple[float, float]] = None
        for slot in draw_order:
            if not self.var_show_layer[slot].get():
                continue
            hb_idx, hb = self._resolve_box_for_slot(fb, slot)
            if hb is None or (hb.w == 0 and hb.h == 0 and slot != self.selected_box_slot):
                continue
            dl, dt, dr, db = self._dots_rect_for_box(hb, is_push=(slot == "push_box"))
            x0, y0, x1, y1 = ox + dl * sx, oy + dt * sy, ox + dr * sx, oy + db * sy
            self.box_screen_rects[slot] = (x0, y0, x1, y1)
            if slot == "atck_box":
                atk_center = ((x0 + x1) / 2.0, (y0 + y1) / 2.0)

            base_col, _ = BOX_COLORS[slot]
            if slot == "atck_box2" and (fb.atck_box2 & 0x8000):
                base_col = "#e040fb"  # Purple for Throw Range Box
            is_sel = (slot == self.selected_box_slot)
            width = 3 if is_sel else 2
            dash = () if is_sel else ((4, 3) if slot == "push_box" else ())
            self.canvas.create_rectangle(x0, y0, x1, y1, outline=base_col, width=width, dash=dash)
            self.canvas.create_text(
                x0 + 3,
                y0 + 2,
                text=f"{slot}#{hb_idx}",
                anchor="nw",
                fill=base_col,
                font=("Consolas", 8, "bold" if is_sel else "normal"),
            )
            if is_sel:
                # Draw 8 resize handles
                for hx in (x0, (x0 + x1) / 2, x1):
                    for hy in (y0, (y0 + y1) / 2, y1):
                        if hx == (x0 + x1) / 2 and hy == (y0 + y1) / 2:
                            continue
                        self.canvas.create_rectangle(hx - 4, hy - 4, hx + 4, hy + 4, fill="#ffffff", outline=base_col)

        # Optional AI range boxes
        if self.var_show_range.get() and 0 <= self.current_atck_no < len(self.char_data.hit_data):
            hd = self.char_data.hit_data[self.current_atck_no]
            for rslot, ridx in [("AI_Range1", hd.range_box), ("AI_Range2", hd.range_box2)]:
                if 0 < ridx < len(self.char_data.hit_boxes):
                    rhb = self.char_data.hit_boxes[ridx]
                    dl, dt, dr, db = self._dots_rect_for_box(rhb, is_push=False)
                    self.canvas.create_rectangle(
                        ox + dl * sx,
                        oy + dt * sy,
                        ox + dr * sx,
                        oy + db * sy,
                        outline="#aaaaaa",
                        dash=(3, 4),
                        width=1,
                    )

        # Optional Knockback / Launch Trajectory Vector Arrow & Parabolic Arc
        if self.var_show_traj.get() and 0 <= self.current_atck_no < len(self.char_data.hit_data):
            hd = self.char_data.hit_data[self.current_atck_no]
            dm_no = hd.damage_type & DM_NO_MASK
            start_px, start_py = atk_center if atk_center is not None else (ox + (30 if face_right else -30) * sx, oy - 50 * sy)
            ang_idx = (40 - (hd.vector & 31)) & 31
            ang_rad = ang_idx * (2.0 * math.pi / 32.0)
            pow_val = DM_POW_TBL[hd.power & 15]
            vx_d = pow_val * math.cos(ang_rad) / 150.0  # -X is away when facing Left; negate X if face_right so knockback goes rightward
            vy_d = pow_val * math.sin(ang_rad) / 125.0  # +Y is UP in world, which is -Y on screen!
            dir_sign = -1.0 if face_right else 1.0  # In native sprite (facing Left), cos(180)=-1 is backward RIGHT; mirrored Face Right negates X
            disp_vx = vx_d * dir_sign
            disp_vy = -vy_d  # screen Y down

            if dm_no in (1, 2, 3, 7, 8, 10, 11, 12, 14, 17, 18, 21) or hd.damage_type == 0:
                arrow_end_x = start_px + disp_vx * 5.0 * sx
                arrow_end_y = start_py + disp_vy * 5.0 * sy
                self.canvas.create_line(
                    start_px, start_py, arrow_end_x, arrow_end_y,
                    fill="#ff6b6b", width=2, arrow=tk.LAST,
                )
                # Parabolic arc preview for 45 frames
                pts = []
                for t in range(0, 46, 3):
                    px_arc = start_px + disp_vx * t * sx
                    py_arc = start_py + (disp_vy * t + 0.5 * 0.375 * t * t) * sy
                    pts.extend([px_arc, py_arc])
                    if py_arc > oy + 20:
                        break
                if len(pts) >= 4:
                    self.canvas.create_line(*pts, fill="#ffa8a8", dash=(3, 3), width=1, smooth=True)

    # ------------------------------------------------------------------
    # Mouse Hitbox Resizing & Moving
    # ------------------------------------------------------------------

    def _hit_test_handles(self, px: float, py: float) -> Optional[str]:
        rect = self.box_screen_rects.get(self.selected_box_slot)
        if not rect:
            return None
        x0, y0, x1, y1 = rect
        tol = 7.0
        on_l = abs(px - x0) <= tol and (y0 - tol <= py <= y1 + tol)
        on_r = abs(px - x1) <= tol and (y0 - tol <= py <= y1 + tol)
        on_t = abs(py - y0) <= tol and (x0 - tol <= px <= x1 + tol)
        on_b = abs(py - y1) <= tol and (x0 - tol <= px <= x1 + tol)
        if on_t and on_l:
            return "nw"
        if on_t and on_r:
            return "ne"
        if on_b and on_l:
            return "sw"
        if on_b and on_r:
            return "se"
        if on_t:
            return "n"
        if on_b:
            return "s"
        if on_l:
            return "w"
        if on_r:
            return "e"
        if x0 <= px <= x1 and y0 <= py <= y1:
            return "move"
        return None

    def _find_box_at_point(self, px: float, py: float) -> Optional[str]:
        # Prefer smallest enclosing box, checking top layers first
        order = ["atck_box", "atck_box2", "body_box0", "body_box1", "body_box2", "body_box3", "catch_box", "push_box"]
        for slot in order:
            rect = self.box_screen_rects.get(slot)
            if not rect:
                continue
            x0, y0, x1, y1 = rect
            if x0 - 4 <= px <= x1 + 4 and y0 - 4 <= py <= y1 + 4:
                return slot
        return None

    def _on_canvas_hover(self, event) -> None:
        mode = self._hit_test_handles(event.x, event.y)
        cursor_map = {
            "nw": "size_nw_se",
            "se": "size_nw_se",
            "ne": "size_ne_sw",
            "sw": "size_ne_sw",
            "n": "sb_v_double_arrow",
            "s": "sb_v_double_arrow",
            "e": "sb_h_double_arrow",
            "w": "sb_h_double_arrow",
            "move": "fleur",
        }
        self.canvas.configure(cursor=cursor_map.get(mode or "", ""))

    def _on_canvas_lpress(self, event) -> None:
        mode = self._hit_test_handles(event.x, event.y)
        if mode is None:
            clicked = self._find_box_at_point(event.x, event.y)
            if clicked is not None:
                self.selected_box_slot = clicked
                self.var_hb_slot.set(clicked)
                self._sync_hitbox_fields_to_ui()
                self._redraw_canvas()
                mode = self._hit_test_handles(event.x, event.y)
        if mode is None:
            return
        fr = self._current_frame()
        if not fr or not self.char_data:
            return
        fb = self._get_frame_box(fr.box)
        hb_idx, hb = self._resolve_box_for_slot(fb, self.selected_box_slot)
        if hb is None or hb_idx <= 0:
            return
        self._drag_mode = mode
        self._drag_cloned_done = False
        self._drag_moved = False
        self._drag_pre_snapshot = self._capture_snapshot()
        self._drag_start_px = (float(event.x), float(event.y))
        self._drag_orig_dots = self._dots_rect_for_box(hb, is_push=(self.selected_box_slot == "push_box"))

    def _ensure_unshared_for_drag(self) -> None:
        if self._drag_cloned_done or not self.var_clone_shared.get() or not self.char_data:
            return
        self._drag_cloned_done = True
        if self.selected_box_slot == "push_box":
            return
        fr = self._current_frame()
        if not fr:
            return
        if len(count_frame_box_users(self.char_data, fr.box)) > 1:
            new_fb = clone_frame_box(self.char_data, fr.box)
            fr.box = new_fb
            self._suppress_callbacks = True
            self.var_fr_box.set(new_fb)
            self._suppress_callbacks = False
        fb = self.char_data.frame_boxes[fr.box]
        raw = self._get_slot_raw(fb, self.selected_box_slot)
        hb_idx = raw & BOX_MASK
        flags = raw & ~BOX_MASK
        if hb_idx > 0 and len(count_hit_box_users(self.char_data, hb_idx)) > 1:
            new_hb_idx = allocate_hit_box(self.char_data, self.char_data.hit_boxes[hb_idx].clone())
            self._set_slot_raw(fb, self.selected_box_slot, flags | new_hb_idx)

    def _on_canvas_ldrag(self, event) -> None:
        if not self._drag_mode or not self.char_data:
            return
        self._ensure_unshared_for_drag()
        fr = self._current_frame()
        if not fr:
            return
        fb = self._get_frame_box(fr.box)
        hb_idx, hb = self._resolve_box_for_slot(fb, self.selected_box_slot)
        if hb is None or hb_idx <= 0:
            return

        sx, sy = self._aspect_scales()
        dx = (event.x - self._drag_start_px[0]) / sx
        dy = (event.y - self._drag_start_px[1]) / sy
        l, t, r, b = self._drag_orig_dots
        m = self._drag_mode
        if m == "move":
            l, r = l + dx, r + dx
            t, b = t + dy, b + dy
        else:
            if "w" in m:
                l = min(r - 1.0, l + dx)
            if "e" in m:
                r = max(l + 1.0, r + dx)
            if "n" in m:
                t = min(b - 1.0, t + dy)
            if "s" in m:
                b = max(t + 1.0, b + dy)

        new_nx = int(round((l + r) / 2.0))
        new_w = max(1, int(round((r - l) / 2.0)))
        new_ny = int(round((t + b) / 2.0))
        new_h = max(1, int(round((b - t) / 2.0)))

        hb.x = -new_nx if self.var_face_right.get() else new_nx
        hb.w = new_w
        hb.y = -new_ny if (self.selected_box_slot == "push_box") else new_ny
        hb.h = new_h

        self._drag_moved = True
        self.dirty_data = True
        self._sync_hitbox_fields_to_ui()
        self._redraw_canvas()

    def _on_canvas_lrelease(self, _event) -> None:
        if self._drag_moved and self._drag_pre_snapshot is not None:
            self.undo_stack.append(self._drag_pre_snapshot)
            if len(self.undo_stack) > 60:
                self.undo_stack.pop(0)
            self.redo_stack.clear()
            self._update_undo_buttons()
        self._drag_mode = None
        self._drag_cloned_done = False
        self._drag_moved = False
        self._drag_pre_snapshot = None

    def _on_canvas_pan_start(self, event) -> None:
        self._drag_pan_origin = (event.x - self.origin_px_x, event.y - self.origin_px_y)

    def _on_canvas_pan_move(self, event) -> None:
        self.origin_px_x = event.x - self._drag_pan_origin[0]
        self.origin_px_y = event.y - self._drag_pan_origin[1]
        self._redraw_canvas()

    # ------------------------------------------------------------------
    # Hitbox Slot Controls (Add / Remove / Clone / Spinboxes)
    # ------------------------------------------------------------------

    def _on_hb_slot_selected(self, _evt=None) -> None:
        self.selected_box_slot = self.var_hb_slot.get()
        self._sync_hitbox_fields_to_ui()
        self._redraw_canvas()

    def _on_push_stance_changed(self, _evt=None) -> None:
        self.selected_push_stance = max(0, min(3, int(self.var_hb_push_stance.get().split(":")[0])))
        self.selected_box_slot = "push_box"
        self.var_hb_slot.set("push_box")
        self._sync_hitbox_fields_to_ui()
        self._redraw_canvas()

    def _sync_hitbox_fields_to_ui(self) -> None:
        fr = self._current_frame()
        if not fr or not self.char_data:
            return
        fb = self._get_frame_box(fr.box)
        hb_idx, hb = self._resolve_box_for_slot(fb, self.selected_box_slot)
        self._suppress_callbacks = True
        try:
            self.var_hb_id.set(hb_idx)
            if hb is not None:
                self.var_hb_x.set(hb.x)
                self.var_hb_w.set(hb.w)
                self.var_hb_y.set(hb.y)
                self.var_hb_h.set(hb.h)
            else:
                self.var_hb_x.set(0)
                self.var_hb_w.set(0)
                self.var_hb_y.set(0)
                self.var_hb_h.set(0)
            self.var_hb_throw_bit.set(bool(fb.atck_box2 & 0x8000))
            b3_hi = fb.body_box3 & 0x6000
            b3_map = {
                0x0000: "0x0000 (Type 4)",
                0x2000: "0x2000 (Type 5)",
                0x4000: "0x4000 (Type 6)",
                0x6000: "0x6000 (Type 7)",
            }
            self.var_hb_body3_type.set(b3_map.get(b3_hi, "0x0000 (Type 4)"))
            fb_users = len(count_frame_box_users(self.char_data, fr.box))
            hb_users = len(count_hit_box_users(self.char_data, hb_idx))
            self.var_hb_share_info.set(f"FrameBox#{fr.box} shared by {fb_users} seqs | HitBox#{hb_idx} by {hb_users} fboxes")
        finally:
            self._suppress_callbacks = False

    def _clone_current_frame_box(self) -> None:
        fr = self._current_frame()
        if not fr or not self.char_data:
            return
        self._push_undo()
        new_fb = clone_frame_box(self.char_data, fr.box)
        fr.box = new_fb
        self.dirty_data = True
        self._sync_frame_to_ui()
        self._redraw_canvas()
        self.set_status(f"Cloned FrameBox into new row #{new_fb} for current frame.")

    def _add_box_to_slot(self) -> None:
        fr = self._current_frame()
        if not fr or not self.char_data:
            return
        if self.selected_box_slot == "push_box":
            messagebox.showinfo("Pushbox", "Pushbox slots (252..255) always exist; edit its coordinates directly.")
            return
        self._push_undo()
        if self.var_clone_shared.get() and len(count_frame_box_users(self.char_data, fr.box)) > 1:
            fr.box = clone_frame_box(self.char_data, fr.box)
        fb = self.char_data.frame_boxes[fr.box]
        default_hb = HitBox(-40, 28, -55, 22) if "atck" in self.selected_box_slot else HitBox(0, 26, -50, 30)
        new_id = allocate_hit_box(self.char_data, default_hb)
        flags = self._get_slot_raw(fb, self.selected_box_slot) & ~BOX_MASK
        self._set_slot_raw(fb, self.selected_box_slot, flags | new_id)
        self.dirty_data = True
        self._sync_frame_to_ui()
        self._refresh_timeline()
        self._redraw_canvas()
        self.set_status(f"Added HitBox #{new_id} to {self.selected_box_slot} (FrameBox #{fr.box}).")

    def _remove_box_from_slot(self) -> None:
        fr = self._current_frame()
        if not fr or not self.char_data:
            return
        if self.selected_box_slot == "push_box":
            messagebox.showinfo("Pushbox", "Pushbox cannot be removed; set w=0, h=0 if needed.")
            return
        self._push_undo()
        if self.var_clone_shared.get() and len(count_frame_box_users(self.char_data, fr.box)) > 1:
            fr.box = clone_frame_box(self.char_data, fr.box)
        fb = self.char_data.frame_boxes[fr.box]
        self._set_slot_raw(fb, self.selected_box_slot, 0)
        self.dirty_data = True
        self._sync_frame_to_ui()
        self._refresh_timeline()
        self._redraw_canvas()
        self.set_status(f"Cleared {self.selected_box_slot} on FrameBox #{fr.box}.")

    def _on_hb_id_changed(self) -> None:
        if self._suppress_callbacks or not self.char_data:
            return
        fr = self._current_frame()
        if not fr or self.selected_box_slot == "push_box":
            return
        self._push_undo()
        new_id = max(0, int(self.var_hb_id.get())) & BOX_MASK
        while new_id >= len(self.char_data.hit_boxes):
            self.char_data.hit_boxes.append(HitBox())
        if self.var_clone_shared.get() and len(count_frame_box_users(self.char_data, fr.box)) > 1:
            fr.box = clone_frame_box(self.char_data, fr.box)
        fb = self.char_data.frame_boxes[fr.box]
        flags = self._get_slot_raw(fb, self.selected_box_slot) & ~BOX_MASK
        self._set_slot_raw(fb, self.selected_box_slot, flags | new_id)
        self.dirty_data = True
        self._sync_frame_to_ui()
        self._refresh_timeline()
        self._redraw_canvas()

    def _on_hb_flags_changed(self) -> None:
        if self._suppress_callbacks or not self.char_data:
            return
        fr = self._current_frame()
        if not fr:
            return
        self._push_undo()
        if self.var_clone_shared.get() and len(count_frame_box_users(self.char_data, fr.box)) > 1:
            fr.box = clone_frame_box(self.char_data, fr.box)
        fb = self.char_data.frame_boxes[fr.box]
        a2_lo = fb.atck_box2 & BOX_MASK
        fb.atck_box2 = (0x8000 | a2_lo) if self.var_hb_throw_bit.get() else a2_lo
        b3_lo = fb.body_box3 & BOX_MASK
        b3_hi = int(self.var_hb_body3_type.get().split()[0], 16)
        fb.body_box3 = b3_hi | b3_lo
        self.dirty_data = True
        self._sync_frame_to_ui()
        self._redraw_canvas()

    def _on_hb_coords_changed(self) -> None:
        if self._suppress_callbacks or not self.char_data:
            return
        fr = self._current_frame()
        if not fr:
            return
        self._push_undo()
        if self.selected_box_slot != "push_box":
            if self.var_clone_shared.get() and len(count_frame_box_users(self.char_data, fr.box)) > 1:
                fr.box = clone_frame_box(self.char_data, fr.box)
            fb = self.char_data.frame_boxes[fr.box]
            raw = self._get_slot_raw(fb, self.selected_box_slot)
            hb_idx = raw & BOX_MASK
            if hb_idx <= 0:
                return
            if self.var_clone_shared.get() and len(count_hit_box_users(self.char_data, hb_idx)) > 1:
                new_id = allocate_hit_box(self.char_data, self.char_data.hit_boxes[hb_idx].clone())
                self._set_slot_raw(fb, self.selected_box_slot, (raw & ~BOX_MASK) | new_id)
                hb_idx = new_id
        else:
            hb_idx = PUSH_BOX_BASE + self.selected_push_stance

        hb = self.char_data.hit_boxes[hb_idx]
        hb.x = int(self.var_hb_x.get())
        hb.w = max(0, int(self.var_hb_w.get()))
        hb.y = int(self.var_hb_y.get())
        hb.h = max(0, int(self.var_hb_h.get()))
        self.dirty_data = True
        self._sync_hitbox_fields_to_ui()
        self._redraw_canvas()

    # ------------------------------------------------------------------
    # HIT_DATA, Whiff Meter & Bounce Synchronization
    # ------------------------------------------------------------------

    def _current_hit_data(self) -> Optional[HitData]:
        if not self.char_data:
            return None
        if 0 <= self.current_atck_no < len(self.char_data.hit_data):
            return self.char_data.hit_data[self.current_atck_no]
        return None

    def _sync_whiff_special_ui(self) -> None:
        for i, v in enumerate(self.var_whiff_normal):
            v.set(self.atck_gauge_tbl[i] if i < len(self.atck_gauge_tbl) else 0)
        whiffs = self.current_action.whiff_entries if self.current_action else []
        if not whiffs and self.char_data:
            # Show first character special whiff if any
            whiffs = [w for w in self.char_data.whiff_gauges if w.group == (self.current_action.group if self.current_action else -1)]
        if whiffs:
            w = whiffs[0]
            key = (str(w.file_path), w.line_no)
            amt = self.edited_whiffs[key][1] if key in self.edited_whiffs else w.amount
            self.var_whiff_sp_amt.set(amt)
            self.var_whiff_sp_label.set(f"{w.func_name} (s_pl{self.char_id:02x}.c:{w.line_no})")
        else:
            self.var_whiff_sp_amt.set(0)
            if self.current_atck_no < 0x12 and self.current_atck_no in STANDARD_ATCK_NAMES:
                lvl = self.current_atck_no % 3
                self.var_whiff_sp_label.set(f"Normal uses atck_gauge_tbl[{lvl}] = {self.atck_gauge_tbl[lvl]}")
            else:
                self.var_whiff_sp_label.set("No pl_gauge_add call found on this animation")

    def _sync_bounce_to_ui(self) -> None:
        rule = next((r for r in self.bounce_rules if r.pl_type == self.char_id and r.atck_no == self.current_atck_no), None)
        self._suppress_callbacks = True
        try:
            if rule is None:
                self.var_bounce_prop.set(f"0: {BOUNCE_PROP_NAMES[0][1]} ({BOUNCE_PROP_NAMES[0][2]})")
                self.var_bounce_need_air.set(True)
                self.var_bounce_no_fly.set(True)
                self.var_bounce_height.set("0.0")
            else:
                _, sym, desc = BOUNCE_PROP_NAMES[rule.prop] if 0 <= rule.prop < len(BOUNCE_PROP_NAMES) else BOUNCE_PROP_NAMES[0]
                self.var_bounce_prop.set(f"{rule.prop}: {sym} ({desc})")
                self.var_bounce_need_air.set(bool(rule.need_air))
                self.var_bounce_no_fly.set(bool(rule.no_fly_screen))
                self.var_bounce_height.set(f"{rule.height:g}")
        finally:
            self._suppress_callbacks = False

    def _sync_hit_data_to_ui(self) -> None:
        hd = self._current_hit_data()
        self._sync_whiff_special_ui()
        self._sync_bounce_to_ui()
        self._sync_cancel_to_ui()
        if hd is None:
            return
        self._suppress_callbacks = True
        try:
            self.var_hd_comment.set(hd.comment)
            self.var_hd_damage.set(hd.damage)
            self.var_hd_special.set(bool(hd.atck_kind & A_SPECIAL))
            self.var_hd_hyper.set(bool(hd.atck_kind & A_HYPER))
            self.var_hd_cls40.set(bool(hd.atck_kind & 0x40))
            self.var_hd_cls80.set(bool(hd.atck_kind & 0x80))

            self.var_hd_no_chip.set(bool(hd.hit_flag & NO_CHIP))
            self.var_hd_no_weight.set(bool(hd.hit_flag & NO_WEIGHT))
            self.var_hd_no_scale.set(bool(hd.hit_flag & NO_SCALE))
            self.var_hd_no_combo.set(bool(hd.hit_flag & NO_COMBO))
            self.var_hd_no_ko.set(bool(hd.hit_flag & NO_KO))
            self.var_hd_black_combo.set(bool(hd.hit_flag & BLACK_COMBO))
            self.var_hd_aerial_fin.set(bool(hd.hit_flag & AERIAL_FINISH))

            self.var_hd_shc.set(hd.shc & 63)
            self.var_hd_shc_nogauge.set(bool(hd.shc & 0x40))

            g = hd.atck_kind & A_GUARD_MASK
            self.var_hd_guard.set(GUARD_TYPE_NAMES.get(g, GUARD_TYPE_NAMES[0]))

            lv = hd.damage_kind & K_LV_MASK
            self.var_hd_strength.set(STRENGTH_LEVEL_NAMES.get(lv, STRENGTH_LEVEL_NAMES[0]))
            self.var_hd_k_low.set(bool(hd.damage_kind & K_LOW))
            self.var_hd_k_back_max.set(bool(hd.damage_kind & K_BACK_MAX))
            elem = hd.damage_kind & K_ELEM_MASK
            self.var_hd_element.set(ELEMENT_NAMES.get(elem, ELEMENT_NAMES[0]))

            self.var_hd_hit_stop.set(hd.hit_stop)
            self.var_hd_guard_time.set(hd.guard_time)
            self.var_hd_sandbag_time.set(hd.sandbag_time)
            self.var_hd_sleep_time.set(hd.sleep_time)
            self.var_hd_catch_ng.set(hd.catch_ng_time & 0x3F)
            self.var_hd_piyo.set(hd.piyo)
            self.var_hd_hit_cnt.set(hd.hit_cnt)
            self.var_hd_jug.set(hd.jug)
            self.var_hd_abyss_jug.set(hd.abyss_jug)

            dm_no = hd.damage_type & DM_NO_MASK
            dm_pair = DM_TYPE_NAMES.get(dm_no, (f"{dm_no}: Unknown", ""))
            self.var_hd_dm_no.set(dm_pair[0])
            self.var_hd_dm_desc.set(dm_pair[1])
            self.var_hd_fly_screen.set(bool(hd.damage_type & FLY_SCREEN))
            self.var_hd_no_vec.set(bool(hd.damage_type & NO_VEC))
            self.var_hd_no_zu.set(bool(hd.damage_type & NO_ZU))

            self.var_hd_power.set(hd.power & 15)
            self.var_hd_vector.set(hd.vector)

            self.var_hd_mark.set(hd.hit_mark)
            self.var_hd_se.set(hd.hit_se)
            self.var_hd_score.set(hd.score)
            self.var_hd_range.set(hd.atck_range)
            self.var_hd_cpu_ans.set(hd.cpu_answer_no)
            self.var_hd_rbox1.set(hd.range_box)
            self.var_hd_rbox2.set(hd.range_box2)
            self.var_hd_unk13.set(hd.unk_13)
            self.var_hd_unk15.set(hd.unk_15)
            self.var_hd_unk1a0.set(hd.unk_1a_0)
            self.var_hd_unk1a1.set(hd.unk_1a_1)
        finally:
            self._suppress_callbacks = False
        self._update_physics_readouts()

    def _on_vector_slider(self) -> None:
        if self._suppress_callbacks:
            return
        self.var_hd_vector.set(int(round(float(self.var_hd_vector.get()))))
        self._on_hit_data_changed()

    def _on_hit_data_changed(self) -> None:
        if self._suppress_callbacks or not self.char_data or self.no_active_hit_data:
            return
        hd = self._current_hit_data()
        if hd is None:
            return
        self._push_undo()

        hd.comment = self.var_hd_comment.get().strip()
        hd.damage = max(0, min(255, int(self.var_hd_damage.get())))

        g_val = int(self.var_hd_guard.get().split(":", 1)[0])
        ak = g_val & A_GUARD_MASK
        if self.var_hd_special.get():
            ak |= A_SPECIAL
        if self.var_hd_hyper.get():
            ak |= A_HYPER
        if self.var_hd_cls40.get():
            ak |= 0x40
        if self.var_hd_cls80.get():
            ak |= 0x80
        hd.atck_kind = ak

        hf = 0
        if self.var_hd_no_chip.get():
            hf |= NO_CHIP
        if self.var_hd_no_weight.get():
            hf |= NO_WEIGHT
        if self.var_hd_no_scale.get():
            hf |= NO_SCALE
        if self.var_hd_no_combo.get():
            hf |= NO_COMBO
        if self.var_hd_no_ko.get():
            hf |= NO_KO
        if self.var_hd_black_combo.get():
            hf |= BLACK_COMBO
        if self.var_hd_aerial_fin.get():
            hf |= AERIAL_FINISH
        hd.hit_flag = hf

        hd.shc = (int(self.var_hd_shc.get()) & 63) | (0x40 if self.var_hd_shc_nogauge.get() else 0)

        lv = int(self.var_hd_strength.get().split(":", 1)[0], 0) & K_LV_MASK
        el = int(self.var_hd_element.get().split(":", 1)[0], 0) & K_ELEM_MASK
        dk = lv | el
        if self.var_hd_k_low.get():
            dk |= K_LOW
        if self.var_hd_k_back_max.get():
            dk |= K_BACK_MAX
        hd.damage_kind = dk

        hd.hit_stop = max(0, min(255, int(self.var_hd_hit_stop.get())))
        hd.guard_time = max(0, min(255, int(self.var_hd_guard_time.get())))
        hd.sandbag_time = max(0, min(255, int(self.var_hd_sandbag_time.get())))
        hd.sleep_time = max(0, min(255, int(self.var_hd_sleep_time.get())))
        hd.catch_ng_time = max(0, min(63, int(self.var_hd_catch_ng.get())))
        hd.piyo = max(0, min(255, int(self.var_hd_piyo.get())))
        hd.hit_cnt = max(0, min(255, int(self.var_hd_hit_cnt.get())))
        hd.jug = max(0, min(255, int(self.var_hd_jug.get())))
        hd.abyss_jug = max(0, min(255, int(self.var_hd_abyss_jug.get())))

        dm_no = int(self.var_hd_dm_no.get().split(":", 1)[0]) & DM_NO_MASK
        dt = dm_no
        if self.var_hd_fly_screen.get():
            dt |= FLY_SCREEN
        if self.var_hd_no_vec.get():
            dt |= NO_VEC
        if self.var_hd_no_zu.get():
            dt |= NO_ZU
        hd.damage_type = dt
        self.var_hd_dm_desc.set(DM_TYPE_NAMES.get(dm_no, ("", ""))[1])

        hd.power = max(0, min(15, int(self.var_hd_power.get())))
        vec = int(self.var_hd_vector.get())
        hd.vector = max(-128, min(127, vec))

        hd.hit_mark = max(0, min(255, int(self.var_hd_mark.get())))
        hd.hit_se = max(0, min(255, int(self.var_hd_se.get())))
        hd.score = max(0, min(255, int(self.var_hd_score.get())))
        hd.atck_range = max(0, min(255, int(self.var_hd_range.get())))
        hd.cpu_answer_no = max(0, min(255, int(self.var_hd_cpu_ans.get())))
        hd.range_box = max(0, min(255, int(self.var_hd_rbox1.get())))
        hd.range_box2 = max(0, min(255, int(self.var_hd_rbox2.get())))
        hd.unk_13 = max(0, min(255, int(self.var_hd_unk13.get())))
        hd.unk_15 = max(0, min(255, int(self.var_hd_unk15.get())))
        hd.unk_1a_0 = max(0, min(255, int(self.var_hd_unk1a0.get())))
        hd.unk_1a_1 = max(0, min(255, int(self.var_hd_unk1a1.get())))

        self.dirty_data = True
        self._update_physics_readouts()
        self._redraw_canvas()

    def _apply_kd_preset(self) -> None:
        preset = self.var_hd_kd_preset.get()
        hd = self._current_hit_data()
        if hd is None or "Select preset" in preset:
            return
        if "ZU = 0" in preset:
            self.var_hd_dm_no.set(DM_TYPE_NAMES[0][0])
            self.var_hd_black_combo.set(False)
            self.var_hd_aerial_fin.set(False)
        elif "BU_O = 1" in preset:
            self.var_hd_dm_no.set(DM_TYPE_NAMES[1][0])
            self.var_hd_black_combo.set(False)
        elif "dm_no = 2" in preset:
            self.var_hd_dm_no.set(DM_TYPE_NAMES[2][0])
        elif "dm_no = 21" in preset:
            self.var_hd_dm_no.set(DM_TYPE_NAMES[21][0])
        elif "AS = 4" in preset:
            self.var_hd_dm_no.set(DM_TYPE_NAMES[4][0])
        elif "BLACK_COMBO" in preset:
            self.var_hd_dm_no.set(DM_TYPE_NAMES[1][0])
            self.var_hd_no_vec.set(True)
            self.var_hd_no_weight.set(True)
            self.var_hd_black_combo.set(True)
            self.var_hd_vector.set(0)
            self.var_hd_power.set(11)
        elif "AERIAL_FINISH" in preset:
            self.var_hd_dm_no.set(DM_TYPE_NAMES[1][0])
            self.var_hd_no_vec.set(True)
            self.var_hd_aerial_fin.set(True)
        elif "dm_no = 17" in preset:
            self.var_hd_dm_no.set(DM_TYPE_NAMES[17][0])
        elif "dm_no = 16" in preset:
            self.var_hd_dm_no.set(DM_TYPE_NAMES[16][0])
        elif "dm_no = 12" in preset:
            self.var_hd_dm_no.set(DM_TYPE_NAMES[12][0])
        self._on_hit_data_changed()

    def _on_bounce_changed(self) -> None:
        if self._suppress_callbacks or not self.char_data or self.no_active_hit_data:
            return
        self._push_undo()
        prop = int(self.var_bounce_prop.get().split(":", 1)[0])
        need_air = 1 if self.var_bounce_need_air.get() else 0
        no_fly = 1 if self.var_bounce_no_fly.get() else 0
        try:
            height = max(0.0, float(self.var_bounce_height.get().strip()))
        except ValueError:
            height = 0.0

        existing = next((i for i, r in enumerate(self.bounce_rules) if r.pl_type == self.char_id and r.atck_no == self.current_atck_no), -1)
        if prop == 0:
            if existing >= 0:
                self.bounce_rules.pop(existing)
        else:
            hd = self._current_hit_data()
            cmt = f"{CHAR_BY_ID[self.char_id][0]} {hd.comment if hd and hd.comment else f'0x{self.current_atck_no:02x}'}"
            new_rule = BounceRule(self.char_id, self.current_atck_no, need_air, prop, no_fly, height, cmt[:48])
            if existing >= 0:
                self.bounce_rules[existing] = new_rule
            else:
                self.bounce_rules.append(new_rule)

        self.bounce_dirty = True
        self.dirty_src_c = True
        self._update_physics_readouts()

    def _current_cancel_key(self) -> Optional[Tuple[int, int, str]]:
        """Return `(act_kind, move_id, label)` for the active move/attack, or None."""
        act = self.current_action
        mv = self.current_move
        if act and act.group == 21 and mv and mv.sp_types:
            return (1, mv.sp_types[0], mv.name)
        if not self.no_active_hit_data and self.char_data and 0 <= self.current_atck_no < len(self.char_data.hit_data):
            hd = self.char_data.hit_data[self.current_atck_no]
            lbl = hd.comment or f"0x{self.current_atck_no:02x}"
            return (0, self.current_atck_no, lbl)
        if mv and mv.sp_types:
            return (1, mv.sp_types[0], mv.name)
        return None

    def _sync_cancel_to_ui(self) -> None:
        key = self._current_cancel_key()
        rule = (
            next((r for r in self.cancel_rules if r.pl_type == self.char_id and r.act_kind == key[0] and r.move_id == key[1]), None)
            if key
            else None
        )
        self._suppress_callbacks = True
        try:
            flags = rule.flags if rule else 0
            self.var_cancel_jump.set(bool(flags & 0x01))
            self.var_cancel_special.set(bool(flags & 0x02))
            self.var_cancel_hyper.set(bool(flags & 0x04))
        finally:
            self._suppress_callbacks = False

        # Compute human-readable summary of all current cancel options
        act = self.current_action
        frs = self._current_frames()
        has_atk = any((fr.atck & ATK) and not (fr.atck & (ATE | CG_ATCK_NG)) for fr in frs)
        all_ng = bool(frs) and all(bool(fr.atck & CG_ATCK_NG) for fr in frs)
        hd = None if self.no_active_hit_data else self._current_hit_data()

        lines: List[str] = []
        if act and act.group in (7, 8, 9, 10, 11, 12):
            # Normal attack: check chain_tbl
            tbl_map = {
                (7, 0): "lp_tbl", (7, 1): "mp_tbl", (7, 2): "hp_tbl",
                (8, 0): "lk_tbl", (8, 1): "mk_tbl", (8, 2): "hk_tbl",
                (9, 0): "lp_tbl", (9, 1): "mp_tbl", (9, 2): "hp_tbl",
                (10, 0): "lk_tbl", (10, 1): "mk_tbl", (10, 2): "hk_tbl",
                (11, 0): "j_lp_tbl", (11, 1): "j_mp_tbl", (11, 2): "j_hp_tbl",
                (11, 3): "sj_lp_tbl", (11, 4): "sj_mp_tbl", (11, 5): "sj_hp_tbl",
                (12, 0): "j_lk_tbl", (12, 1): "j_mk_tbl", (12, 2): "j_hk_tbl",
                (12, 3): "sj_lk_tbl", (12, 4): "sj_mk_tbl", (12, 5): "sj_hk_tbl",
            }
            tname = tbl_map.get((act.group, act.anim_idx))
            tbl = self.chain_tables.get(tname) if tname else None
            if tbl:
                btn_names = ["LP", "HP", "LK", "HK"]
                chains = [b for b, v in zip(btn_names, tbl) if v >= 0]
                lines.append(f"Normal Chain: {', '.join(chains) if chains else 'None'}")
            sp_ok = (not all_ng) or bool(flags & 0x02)
            hyp_ok = (not all_ng) or bool(flags & 0x04)
            is_launcher = bool(hd and (hd.hit_flag & BLACK_COMBO))
            sj_ok = is_launcher or bool(flags & 0x01)
            sj_reason = "Launcher (BLACK_COMBO)" if is_launcher else ("Custom Override" if (flags & 0x01) else "No")
            lines.append(
                f"Special: {'Yes' if sp_ok else 'No (CG_ATCK_NG)'} | "
                f"Hyper: {'Yes' if hyp_ok else 'No (CG_ATCK_NG)'} | "
                f"Super Jump: {sj_reason}"
            )
        elif act and act.group == 21:
            j_ok = bool(flags & 0x01)
            sp_ok = bool(flags & 0x02)
            if flags & 0x04:
                hyp_str = "Yes (Custom Override)"
            elif has_atk and not all_ng:
                hyp_str = "Yes (on ATK frames)"
            elif all_ng:
                hyp_str = "No (CG_ATCK_NG)"
            else:
                hyp_str = "No (needs ATK frame or Override)"
            lines.append(
                f"Special Move — Jump: {'Yes (Override)' if j_ok else 'No'} | "
                f"Special: {'Yes (Override)' if sp_ok else 'No'} | "
                f"Hyper: {hyp_str}"
            )
        elif act and act.group == 22:
            lines.append("Hyper Combo — DHC (Delayed Hyper Combo) into partner if meter >= 1; Jump/Special/Hyper: No")
        else:
            sp_ok = (not all_ng) or bool(flags & 0x02)
            hyp_ok = (not all_ng) or bool(flags & 0x04)
            j_ok = bool(flags & 0x01) or bool(hd and (hd.hit_flag & BLACK_COMBO))
            lines.append(
                f"Jump: {'Yes' if j_ok else 'No'} | Special: {'Yes' if sp_ok else 'No'} | Hyper: {'Yes' if hyp_ok else 'No'}"
            )
        self.var_cancel_status.set("\n".join(lines))

    def _on_cancel_changed(self) -> None:
        if self._suppress_callbacks or not self.char_data:
            return
        key = self._current_cancel_key()
        if key is None:
            return
        self._push_undo()
        act_kind, move_id, lbl = key
        flags = (
            (0x01 if self.var_cancel_jump.get() else 0)
            | (0x02 if self.var_cancel_special.get() else 0)
            | (0x04 if self.var_cancel_hyper.get() else 0)
        )
        existing = next(
            (i for i, r in enumerate(self.cancel_rules) if r.pl_type == self.char_id and r.act_kind == act_kind and r.move_id == move_id),
            -1,
        )
        if flags == 0:
            if existing >= 0:
                self.cancel_rules.pop(existing)
        else:
            cmt = f"{CHAR_BY_ID[self.char_id][0]} {lbl}"
            new_rule = CancelRule(self.char_id, act_kind, move_id, flags, cmt[:48])
            if existing >= 0:
                self.cancel_rules[existing] = new_rule
            else:
                self.cancel_rules.append(new_rule)

        self.cancel_dirty = True
        self.dirty_src_c = True
        self._sync_cancel_to_ui()

    def _apply_normal_whiff_table(self) -> None:
        self._push_undo()
        self.atck_gauge_tbl = [int(v.get()) for v in self.var_whiff_normal]
        self.normal_whiff_dirty = True
        self.dirty_src_c = True
        self._sync_whiff_special_ui()
        self._update_physics_readouts()
        self.set_status("Updated atck_gauge_tbl in memory (Save & Rebuild EXE to apply in game).")

    def _apply_special_whiff(self) -> None:
        whiffs = self.current_action.whiff_entries if self.current_action else []
        if not whiffs:
            messagebox.showinfo("Special Whiff Meter", "No pl_gauge_add(pl_w, N) call was matched directly to this animation.")
            return
        self._push_undo()
        amt = int(self.var_whiff_sp_amt.get())
        for w in whiffs:
            self.edited_whiffs[(str(w.file_path), w.line_no)] = (w, amt)
        self.dirty_src_c = True
        self._update_physics_readouts()
        self.set_status(f"Staged special whiff meter = {amt} for {whiffs[0].func_name} (Save & Rebuild EXE to apply).")

    def _update_physics_readouts(self) -> None:
        if self.no_active_hit_data:
            act_lbl = self._action_dropdown_label(self.current_action) if self.current_action else "this state"
            self.var_hd_summary.set(
                f"No Attack Hitbox on {act_lbl} (movement / teleport / visual clone).\n"
                "Move Hit Data is N/A. Select an attacking state or uncheck 'Sync Action <-> Hit Data' to inspect a HIT_DATA entry."
            )
            return
        hd = self._current_hit_data()
        if hd is None:
            self.var_hd_summary.set("No HIT_DATA selected.")
            return
        whiff_est = None
        if self.current_atck_no < 0x12 and self.current_atck_no in STANDARD_ATCK_NAMES:
            whiff_est = self.atck_gauge_tbl[self.current_atck_no % 3]
        elif self.current_action and self.current_action.whiff_entries:
            w = self.current_action.whiff_entries[0]
            key = (str(w.file_path), w.line_no)
            whiff_est = self.edited_whiffs[key][1] if key in self.edited_whiffs else w.amount

        phys = compute_move_physics(
            hd.damage,
            hd.damage_type,
            hd.atck_kind,
            hd.damage_kind,
            hd.vector,
            hd.power,
            hd.guard_time,
            hd.shc,
            hd.hit_flag,
            whiff_gauge=whiff_est,
        )
        self.var_hd_chip_info.set(f"Chip: {phys.chip_reason}")
        self.var_hd_guard_time_eff.set(
            f"Effective: {phys.blockstun_frames}F" + (" (default)" if phys.blockstun_is_default else " (custom)")
        )
        self.var_hd_meter_info.set(phys.hit_meter_note)

        pow_raw = DM_POW_TBL[hd.power & 15]
        spd_slide = BACK_SPD_TBL[hd.power & 15] / 256.0
        self.var_hd_physics_detail.set(
            f"Angle: {phys.launch_angle_deg:6.2f} deg | Speed: ({phys.launch_vx_dots:+.2f}, {phys.launch_vy_dots:+.2f}) dots/f | dm_pow={pow_raw}\n"
            f"Launch Peak Height: {phys.launch_peak_height_dots:5.1f} dots | Hang Time: {phys.launch_air_time_frames:4.1f}F | Horiz Dist: {phys.launch_horiz_distance_dots:5.1f} dots\n"
            f"On-Hit Ground Slide: {spd_slide:+.2f} dots/f (~{phys.ground_pushback_total_dots:.1f} dots) | On-Block Pushback: {phys.block_pushback_dots} dots (Tier {phys.block_pushback_tier})"
        )

        # Top summary banner
        b_prop = int(self.var_bounce_prop.get().split(":", 1)[0])
        b_str = BOUNCE_PROP_NAMES[b_prop][2] if b_prop > 0 else "No Bounce"
        guard_short = GUARD_TYPE_NAMES.get(hd.atck_kind & A_GUARD_MASK, "Mid")
        whiff_disp = f"+{whiff_est}" if whiff_est is not None else "N/A"
        self.var_hd_summary.set(
            f"Hit 0x{self.current_atck_no:02X}: Dmg {phys.raw_damage} (Chip {phys.chip_damage}) | Guard: {guard_short}\n"
            f"On Hit: {phys.knockdown_label} | Bounce: {b_str}\n"
            f"On Block: {phys.blockstun_frames}F stun, {phys.block_pushback_dots} dots push | "
            f"Meter (Hit/Whiff/Blk): +{phys.hit_meter_attacker} (def +{phys.hit_meter_defender}) / {whiff_disp} / +0"
        )

    # ------------------------------------------------------------------
    # Save (C source + PLXX_DAT.BIN + Backups), Restore, Git & Rebuild
    # ------------------------------------------------------------------

    def save_all(self) -> None:
        if not self.char_data:
            return
        if not self.no_active_hit_data:
            self._on_hit_data_changed()

        hx = f"{self.char_id:02x}"
        HX = f"{self.char_id:02X}"
        char_name, _ = CHAR_BY_ID.get(self.char_id, (f"pl{hx}", ""))
        dat_path = self.repo_root / "ports" / "build-dist" / "disc" / f"PL{HX}_DAT.BIN"

        files_to_backup = [
            p
            for p in [
                dat_path,
                self.repo_root / "data" / f"char_{hx}.c",
                self.repo_root / "data" / f"frame_box{hx}.c",
                self.repo_root / "data" / f"hit_{hx}.c",
                self.repo_root / "data" / f"hit_dt{hx}.c",
            ]
            if p.exists()
        ]
        for w, _amt in self.edited_whiffs.values():
            if w.file_path not in files_to_backup and w.file_path.exists():
                files_to_backup.append(w.file_path)
        if self.normal_whiff_dirty:
            files_to_backup.append(self.repo_root / "src" / "pl_act.c")
        if self.bounce_dirty:
            files_to_backup.append(self.repo_root / "src" / "pl_bounce.c")
        if self.cancel_dirty:
            files_to_backup.append(self.repo_root / "src" / "pl_com.c")

        # 1. Original baseline + timestamped snapshot BEFORE writing
        snap = self.backup_mgr.create_snapshot(
            self.char_id,
            label="pre_save",
            summary=f"Pre-save backup for {char_name} (atck 0x{self.current_atck_no:02x})",
            files=files_to_backup,
        )

        # 2. Write C source files
        save_hit_c(self.repo_root, self.char_data)
        save_frame_box_c(self.repo_root, self.char_data)
        save_hit_dt_c(self.repo_root, self.char_data)
        save_char_c(self.repo_root, self.char_data)

        code_changed = False
        if self.normal_whiff_dirty:
            save_atck_gauge_tbl(self.repo_root, self.atck_gauge_tbl)
            code_changed = True
            self.normal_whiff_dirty = False
        for (fp, ln), (w, amt) in list(self.edited_whiffs.items()):
            save_whiff_gauge_entry(w, amt)
            code_changed = True
        self.edited_whiffs.clear()
        if self.bounce_dirty:
            save_bounce_rules(self.repo_root, self.bounce_rules)
            code_changed = True
            self.bounce_dirty = False
        if self.cancel_dirty:
            save_cancel_rules(self.repo_root, self.cancel_rules)
            code_changed = True
            self.cancel_dirty = False

        # 3. Compile Parts 5..8 & patch PLXX_DAT.BIN
        if dat_path.exists():
            p5 = compile_char_bin(self.char_data)
            p6 = compile_frame_box_bin(self.char_data.frame_boxes)
            p7 = compile_hit_box_bin(self.char_data.hit_boxes)
            p8 = compile_hit_data_bin(self.char_data.hit_data)
            patch_pl_dat(dat_path, p5, p6, p7, p8)
            self.pl_archive = PlDatArchive(dat_path)

        self.dirty_data = False
        self.saved_baseline = self._capture_snapshot()
        if code_changed:
            self.dirty_src_c = True
            self.set_status(f"Saved C + PL{HX}_DAT.BIN + Backup ({snap.backup_dir.name}). Click 'Rebuild EXE' to compile src/*.c edits.")
            if messagebox.askyesno(
                "C Code Edited",
                "Changes were written to src/*.c (bounce rules or whiff meter). Rebuild mvc2.exe & mvc2dc.exe now so the PC port uses them?",
            ):
                self._on_rebuild_exe()
        else:
            self.set_status(f"Saved C sources, patched PL{HX}_DAT.BIN, and kept backup '{snap.backup_dir.name}'.")

    def _on_rebuild_exe(self) -> None:
        self.set_status("Rebuilding mvc2.exe and mvc2dc.exe...")
        self.root.update_idletasks()
        ok, msg = rebuild_pc_port(self.repo_root)
        if ok:
            self.dirty_src_c = False
            self.set_status(msg)
            messagebox.showinfo("Build Complete", msg)
        else:
            self.set_status("Build failed (see error dialog).")
            messagebox.showerror("Build Failed", msg)

    def _open_backup_dialog(self) -> None:
        dlg = tk.Toplevel(self.root)
        dlg.title("Backups & Restore (Originals + Snapshots)")
        dlg.geometry("760x460")
        dlg.transient(self.root)
        dlg.grab_set()

        filter_char = tk.BooleanVar(value=True)
        top = ttk.Frame(dlg, padding=6)
        top.pack(fill="x")
        ttk.Checkbutton(top, text=f"Show only backups for pl{self.char_id:02x}", variable=filter_char, command=lambda: populate()).pack(side="left")
        ttk.Button(
            top,
            text="Create Manual Backup Now",
            command=lambda: (
                self.backup_mgr.create_snapshot(
                    self.char_id,
                    label="manual",
                    summary=f"Manual backup for pl{self.char_id:02x}",
                ),
                populate(),
            ),
        ).pack(side="right")

        tree = ttk.Treeview(dlg, columns=("kind", "time", "char", "label", "summary"), show="headings")
        for col, txt, w in [
            ("kind", "Type", 75),
            ("time", "Timestamp", 135),
            ("char", "Char", 55),
            ("label", "Label", 150),
            ("summary", "Summary", 320),
        ]:
            tree.heading(col, text=txt)
            tree.column(col, width=w)
        tree.pack(fill="both", expand=True, padx=6, pady=4)

        backups_ref: List[BackupInfo] = []

        def populate() -> None:
            tree.delete(*tree.get_children())
            backups_ref.clear()
            cid = self.char_id if filter_char.get() else None
            for i, b in enumerate(self.backup_mgr.list_backups(cid)):
                backups_ref.append(b)
                cstr = f"pl{b.char_id:02x}" if b.char_id is not None else "all"
                tree.insert("", "end", iid=str(i), values=(b.kind.upper(), b.timestamp, cstr, b.label, b.summary))

        def do_restore() -> None:
            sel = tree.selection()
            if not sel:
                return
            info = backups_ref[int(sel[0])]
            if not messagebox.askyesno(
                "Confirm Restore",
                f"Restore {len(info.files)} file(s) from '{info.label}' ({info.timestamp})?\nA safety backup of the current files will be saved first.",
                parent=dlg,
            ):
                return
            restored, _ = self.backup_mgr.restore_backup(info)
            self.bounce_rules = parse_bounce_rules(self.repo_root)
            self.atck_gauge_tbl, _ = parse_atck_gauge_tbl(self.repo_root)
            self.load_char(self.char_id, prompt_dirty=False)
            self.set_status(f"Restored {len(restored)} files from backup '{info.backup_dir.name}'.")
            messagebox.showinfo("Restore Complete", f"Restored {len(restored)} files:\n" + "\n".join(restored), parent=dlg)
            populate()

        btns = ttk.Frame(dlg, padding=6)
        btns.pack(fill="x")
        ttk.Button(btns, text="Restore Selected Backup", command=do_restore).pack(side="left")
        ttk.Button(btns, text="Close", command=dlg.destroy).pack(side="right")
        populate()

    def _open_git_dialog(self) -> None:
        if not self.git_mgr.is_repo():
            messagebox.showerror("Git", f"Not a git repository: {self.repo_root}")
            return

        dlg = tk.Toplevel(self.root)
        dlg.title("Git Commit Changes (Single-Responsibility Commits)")
        dlg.geometry("860x580")
        dlg.transient(self.root)
        dlg.grab_set()

        status_items = self.git_mgr.get_status()
        groups = self.git_mgr.suggest_commit_groups(status_items, active_char_id=self.char_id)

        top = ttk.Frame(dlg, padding=6)
        top.pack(fill="x")
        ttk.Label(top, text="Logical Commit Group Preset:").pack(side="left")
        group_names = ["All Modified Files"] + [f"{g[0]} ({len(g[2])} files)" for g in groups]
        var_group = tk.StringVar(value=group_names[1] if len(group_names) > 1 else group_names[0])
        cb_grp = ttk.Combobox(top, textvariable=var_group, values=group_names, state="readonly", width=44)
        cb_grp.pack(side="left", padx=6)

        mid = ttk.PanedWindow(dlg, orient="horizontal")
        mid.pack(fill="both", expand=True, padx=6, pady=4)

        left_f = ttk.LabelFrame(mid, text="Files to Include", padding=4)
        right_f = ttk.LabelFrame(mid, text="Diff Preview", padding=4)
        mid.add(left_f, weight=1)
        mid.add(right_f, weight=2)

        file_vars: Dict[str, tk.BooleanVar] = {}
        sf = ScrollableFrame(left_f)
        sf.pack(fill="both", expand=True)
        for st, rel_p in status_items:
            v = tk.BooleanVar(value=False)
            file_vars[rel_p] = v
            ttk.Checkbutton(
                sf.inner,
                text=f"[{st.strip() or 'M'}] {rel_p}",
                variable=v,
                command=lambda: update_diff(),
            ).pack(anchor="w")

        diff_txt = tk.Text(right_f, font=("Consolas", 9), wrap="none")
        diff_txt.pack(fill="both", expand=True)

        msg_row = ttk.Frame(dlg, padding=6)
        msg_row.pack(fill="x")
        ttk.Label(msg_row, text="Commit Message:").pack(side="left")
        var_msg = tk.StringVar(value="")
        ttk.Entry(msg_row, textvariable=var_msg).pack(side="left", fill="x", expand=True, padx=6)

        def apply_group(_e=None) -> None:
            idx = cb_grp.current()
            if idx <= 0:
                for p, v in file_vars.items():
                    v.set(True)
                var_msg.set(f"pl{self.char_id:02x} ({CHAR_BY_ID[self.char_id][0]}): update move properties and hitboxes")
            else:
                _, smsg, paths = groups[idx - 1]
                pset = set(paths)
                for p, v in file_vars.items():
                    v.set(p in pset)
                var_msg.set(smsg)
            update_diff()

        def update_diff() -> None:
            sel = [p for p, v in file_vars.items() if v.get()]
            txt = self.git_mgr.get_diff(sel) if sel else "(Select files on the left to inspect diff)"
            diff_txt.delete("1.0", "end")
            diff_txt.insert("1.0", txt or "(New untracked files or binary changes)")

        def do_commit() -> None:
            sel = [p for p, v in file_vars.items() if v.get()]
            ok, out = self.git_mgr.commit_files(sel, var_msg.get())
            if ok:
                messagebox.showinfo("Git Commit", out, parent=dlg)
                self.set_status(f"Git commit created: {var_msg.get()}")
                dlg.destroy()
            else:
                messagebox.showerror("Git Commit Failed", out, parent=dlg)

        cb_grp.bind("<<ComboboxSelected>>", apply_group)
        if not status_items:
            var_msg.set("(Working tree clean - no changes to commit)")
        else:
            apply_group()

        bot = ttk.Frame(dlg, padding=6)
        bot.pack(fill="x")
        recent = "Recent: " + " | ".join(self.git_mgr.get_recent_commits(3))
        ttk.Label(bot, text=recent[:95], foreground="#555555").pack(side="left")
        ttk.Button(bot, text="Cancel", command=dlg.destroy).pack(side="right", padx=2)
        ttk.Button(bot, text="Commit Selected Files", command=do_commit).pack(side="right", padx=2)

    def _prepare_temp_test_patch(self, include_src_c: bool = True) -> Tuple[Optional[Path], Optional[str]]:
        """Back up PLXX_DAT.BIN (and optionally src/*.c + EXEs) to temp dir, apply in-memory edits, and rebuild if needed."""
        if not self.char_data:
            return None, "No character loaded."
        HX = f"{self.char_id:02X}"
        dat_path = self.repo_root / "ports" / "build-dist" / "disc" / f"PL{HX}_DAT.BIN"
        if not dat_path.exists():
            return None, f"Could not find {dat_path}."
        td = tempfile.TemporaryDirectory(prefix="mvc2_test_")
        bak_files: List[Tuple[Path, Path]] = []

        def _bak(p: Path, tag: str) -> None:
            if p.exists():
                dst = Path(td.name) / f"{tag}_{p.name}"
                shutil.copy2(p, dst)
                bak_files.append((p, dst))

        _bak(dat_path, "dat")

        p5 = compile_char_bin(self.char_data)
        p6 = compile_frame_box_bin(self.char_data.frame_boxes)
        p7 = compile_hit_box_bin(self.char_data.hit_boxes)
        p8 = compile_hit_data_bin(self.char_data.hit_data)
        patch_pl_dat(dat_path, p5, p6, p7, p8)

        if include_src_c and self.dirty_src_c:
            src_backups: List[Tuple[Path, Path]] = []

            def _bak_src(p: Path) -> None:
                if p.exists() and all(x[0] != p for x in src_backups):
                    dst = Path(td.name) / f"src_{p.name}"
                    shutil.copy2(p, dst)
                    src_backups.append((p, dst))

            dist_dir = self.repo_root / "ports" / "build-dist"
            _bak(dist_dir / "mvc2.exe", "exe")
            _bak(dist_dir / "mvc2dc.exe", "exe")

            try:
                if self.bounce_dirty:
                    p_bnc = self.repo_root / "src" / "pl_bounce.c"
                    _bak_src(p_bnc)
                    save_bounce_rules(self.repo_root, self.bounce_rules)
                if self.cancel_dirty:
                    p_com = self.repo_root / "src" / "pl_com.c"
                    _bak_src(p_com)
                    save_cancel_rules(self.repo_root, self.cancel_rules)
                if self.normal_whiff_dirty:
                    p_act = self.repo_root / "src" / "pl_act.c"
                    _bak_src(p_act)
                    save_atck_gauge_tbl(self.repo_root, self.atck_gauge_tbl)
                for (_fp, _ln), (w, amt) in self.edited_whiffs.items():
                    _bak_src(w.file_path)
                    orig_amt = w.amount
                    save_whiff_gauge_entry(w, amt)
                    w.amount = orig_amt  # keep in-memory object untouched since not saved

                self.set_status("Building temporary test mvc2.exe (bounce/whiff edits, C files will be restored)...")
                self.root.update_idletasks()
                ok, msg = rebuild_pc_port(self.repo_root)
            finally:
                # Immediately restore src/*.c so C source files remain unsaved
                for orig_p, bak_p in src_backups:
                    if bak_p.exists():
                        shutil.copy2(bak_p, orig_p)

            if not ok:
                self._test_backup = (bak_files, td)
                self._restore_temp_test_patch()
                return None, f"Temporary build failed:\n{msg}"

        self._test_backup = (bak_files, td)
        return dat_path, None

    def _restore_temp_test_patch(self) -> bool:
        """Restore PLXX_DAT.BIN and any temporary test EXEs from backup and clean up."""
        if self._test_backup is None:
            return False
        bak_files, td = self._test_backup
        self._test_backup = None
        try:
            for orig_p, bak_p in bak_files:
                if bak_p.exists():
                    shutil.copy2(bak_p, orig_p)
        finally:
            try:
                td.cleanup()
            except Exception:
                pass
        return True

    def test_in_game(self) -> None:
        if not self.char_data:
            return
        if self._test_proc is not None and self._test_proc.poll() is None:
            try:
                self._test_proc.terminate()
                self._test_proc.wait(timeout=3)
            except Exception:
                pass
            self._test_proc = None
            self._restore_temp_test_patch()
            self.btn_test_game.configure(text="Test in Game")
            self.set_status("Stopped test game and restored original files (unsaved changes remain in editor).")
            return

        dist_dir = self.repo_root / "ports" / "build-dist"
        exe_path = dist_dir / "mvc2.exe"
        if not exe_path.exists():
            messagebox.showerror("Test in Game", f"Could not find {exe_path}.\nBuild the PC port first.")
            return

        dat_path, err = self._prepare_temp_test_patch(include_src_c=True)
        if dat_path is None:
            messagebox.showerror("Test in Game", err or "Failed to prepare temporary test build.")
            return

        try:
            self._test_proc = subprocess.Popen([str(exe_path)], cwd=str(dist_dir))
        except Exception as exc:
            self._restore_temp_test_patch()
            messagebox.showerror("Test in Game", f"Failed to launch mvc2.exe:\n{exc}")
            return

        self.btn_test_game.configure(text="Stop Test & Restore")
        self.set_status(
            f"Testing unsaved changes in mvc2.exe (C files untouched; will auto-restore when game closes)."
        )
        self.root.after(250, self._poll_test_proc)

    def _poll_test_proc(self) -> None:
        if self._test_proc is None:
            return
        if self._test_proc.poll() is None:
            self.root.after(250, self._poll_test_proc)
            return
        self._test_proc = None
        restored = self._restore_temp_test_patch()
        self.btn_test_game.configure(text="Test in Game")
        if restored:
            self.set_status("Test game closed; restored original PLXX_DAT.BIN (unsaved edits kept in editor).")

    def set_status(self, msg: str) -> None:
        self.var_status.set(msg)

    def _on_close(self) -> None:
        if self._test_proc is not None and self._test_proc.poll() is None:
            try:
                self._test_proc.terminate()
                self._test_proc.wait(timeout=3)
            except Exception:
                pass
            self._test_proc = None
        self._restore_temp_test_patch()
        if (self.dirty_data or self.dirty_src_c):
            res = messagebox.askyesnocancel("Unsaved Changes", "Save changes before closing?")
            if res is None:
                return
            if res:
                self.save_all()
        self.stop_play()
        self.root.destroy()


def run_gui(repo_root: Optional[Path] = None, char_id: int = 0x00) -> None:
    root_path = find_repo_root(repo_root)
    tk_root = tk.Tk()
    app = MoveEditorApp(tk_root, root_path)
    if char_id != 0x00:
        app.load_char(char_id, prompt_dirty=False)
    tk_root.mainloop()
