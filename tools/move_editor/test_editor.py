"""Automated verification suite for the UMvSC2 Move Property & Hitbox Editor."""

from __future__ import annotations

import shutil
import struct
import sys
import tempfile
import tkinter as tk
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from move_editor import backup_git, c_data, constants, pl_dat
from move_editor.app import MoveEditorApp, find_repo_root


class MoveEditorVerificationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.repo = find_repo_root()
        cls.disc = cls.repo / "ports" / "build-dist" / "disc"

    def test_01_all_59_characters_parse_and_compile(self) -> None:
        """All 59 characters must parse cleanly and match Part 5/7 byte-for-byte against PLXX_DAT.BIN."""
        # Note: pl0c, pl18, pl19, pl1a contain unreferenced Arcade ROM overread bytes at tail of C tables
        overread_chars = {0x0C, 0x18, 0x19, 0x1A}
        for cid in range(0x3B):
            d = c_data.load_character(self.repo, cid)
            self.assertGreater(len(d.actions), 0, f"pl{cid:02x} has no actions")
            self.assertEqual(len(d.hit_boxes), 256, f"pl{cid:02x} hit_boxes != 256")
            expected_prm = 32 if cid == 0x34 else 16
            self.assertEqual(len(d.spr_prm), expected_prm, f"pl{cid:02x} spr_prm count mismatch")

            dat_file = self.disc / f"PL{cid:02X}_DAT.BIN"
            if not dat_file.exists():
                continue
            arc = pl_dat.PlDatArchive(dat_file)
            p5 = pl_dat.pad32(c_data.compile_char_bin(d))
            p7 = pl_dat.pad32(c_data.compile_hit_box_bin(d.hit_boxes))
            self.assertEqual(p5, arc.parts[5], f"Part 5 (char) mismatch for pl{cid:02x}")
            self.assertEqual(p7, arc.parts[7], f"Part 7 (hit_box) mismatch for pl{cid:02x}")
            if cid not in overread_chars:
                p6 = pl_dat.pad32(c_data.compile_frame_box_bin(d.frame_boxes))
                p8 = pl_dat.pad32(c_data.compile_hit_data_bin(d.hit_data))
                self.assertEqual(p6, arc.parts[6], f"Part 6 (frame_box) mismatch for pl{cid:02x}")
                self.assertEqual(p8, arc.parts[8], f"Part 8 (hit_data) mismatch for pl{cid:02x}")

    def test_02_c_serializer_roundtrip_temp(self) -> None:
        """Serializing char/frame_box/hit/hit_dt back to C and reparsing must yield identical binary."""
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            (tmp / "data").mkdir()
            (tmp / "include").mkdir()
            (tmp / "src").mkdir()
            shutil.copy2(self.repo / "include" / "pl00_def.h", tmp / "include" / "pl00_def.h")
            shutil.copy2(self.repo / "src" / "s_pl00.c", tmp / "src" / "s_pl00.c")

            for cid in (0x00, 0x03, 0x1E, 0x34, 0x3A):
                orig = c_data.load_character(self.repo, cid)
                c_data.save_char_c(tmp, orig)
                c_data.save_frame_box_c(tmp, orig)
                c_data.save_hit_c(tmp, orig)
                c_data.save_hit_dt_c(tmp, orig)
                reloaded = c_data.load_character(tmp, cid)

                self.assertEqual(
                    c_data.compile_char_bin(orig),
                    c_data.compile_char_bin(reloaded),
                    f"C roundtrip Part 5 mismatch on pl{cid:02x}",
                )
                self.assertEqual(
                    c_data.compile_frame_box_bin(orig.frame_boxes),
                    c_data.compile_frame_box_bin(reloaded.frame_boxes),
                    f"C roundtrip Part 6 mismatch on pl{cid:02x}",
                )
                self.assertEqual(
                    c_data.compile_hit_box_bin(orig.hit_boxes),
                    c_data.compile_hit_box_bin(reloaded.hit_boxes),
                    f"C roundtrip Part 7 mismatch on pl{cid:02x}",
                )
                self.assertEqual(
                    c_data.compile_hit_data_bin(orig.hit_data),
                    c_data.compile_hit_data_bin(reloaded.hit_data),
                    f"C roundtrip Part 8 mismatch on pl{cid:02x}",
                )

    def test_03_sprite_decoding_and_png(self) -> None:
        """Sprites across multiple characters must decode, composite, and encode to valid RGBA PNGs."""
        for cid, spr_no in [(0x00, 61), (0x03, 1), (0x1E, 61), (0x2C, 1), (0x34, 1), (0x3A, 1)]:
            arc = pl_dat.PlDatArchive(self.disc / f"PL{cid:02X}_DAT.BIN")
            res = arc.render_sprite(spr_no, color_idx=0, flip_h=True)
            self.assertIsNotNone(res, f"Sprite {spr_no} failed on pl{cid:02x}")
            w, h, rgba, ox, oy = res  # type: ignore[misc]
            self.assertGreater(w, 8)
            self.assertGreater(h, 8)
            self.assertEqual(len(rgba), w * h * 4)
            self.assertTrue(any(rgba[3::4]), f"Sprite {spr_no} all transparent on pl{cid:02x}")
            png = pl_dat.rgba_to_png_bytes(w, h, rgba)
            self.assertTrue(png.startswith(b"\x89PNG\r\n\x1a\n"))

    def test_04_pl_dat_patch_and_grow(self) -> None:
        """Patching PL00_DAT.BIN with an added hitbox/frame_box must preserve all other parts cleanly."""
        with tempfile.TemporaryDirectory() as td:
            dst = Path(td) / "PL00_DAT.BIN"
            shutil.copy2(self.disc / "PL00_DAT.BIN", dst)
            before = pl_dat.PlDatArchive(dst)

            d = c_data.load_character(self.repo, 0x00)
            new_hb = c_data.allocate_hit_box(d, c_data.HitBox(-50, 40, -60, 25))
            new_fb = c_data.clone_frame_box(d, 1)
            d.frame_boxes[new_fb].atck_box = new_hb
            d.hit_data[0x02].damage = 25

            pl_dat.patch_pl_dat(
                dst,
                c_data.compile_char_bin(d),
                c_data.compile_frame_box_bin(d.frame_boxes),
                c_data.compile_hit_box_bin(d.hit_boxes),
                c_data.compile_hit_data_bin(d.hit_data),
            )
            after = pl_dat.PlDatArchive(dst)
            self.assertEqual(after.offsets[0] % 32, 0)
            for idx in (0, 1, 2, 3, 4, 9, 10, 11, 12, 13, 14, 15):
                self.assertEqual(before.parts[idx], after.parts[idx], f"Part {idx} corrupted by patch")
            self.assertEqual(len(after.parts[6]), len(before.parts[6]) + 32)  # +16 bytes rounded to 32
            self.assertEqual(after.parts[8][0x02 * 28], 25)

    def test_05_backup_restore_and_git_helpers(self) -> None:
        """BackupManager must keep untouched originals + timestamped snapshots and restore cleanly."""
        with tempfile.TemporaryDirectory() as td:
            fake_repo = Path(td)
            (fake_repo / "data").mkdir(parents=True)
            (fake_repo / "src").mkdir(parents=True)
            (fake_repo / "ports" / "build-dist" / "disc").mkdir(parents=True)

            target_c = fake_repo / "data" / "hit_dt00.c"
            target_bin = fake_repo / "ports" / "build-dist" / "disc" / "PL00_DAT.BIN"
            target_c.write_text("/* original */", encoding="utf-8")
            target_bin.write_bytes(b"ORIGBIN" * 16)

            bm = backup_git.BackupManager(fake_repo)
            bm.create_snapshot(0x00, label="edit1", summary="First edit", files=[target_c, target_bin])
            target_c.write_text("/* modified1 */", encoding="utf-8")
            target_bin.write_bytes(b"MOD1BIN" * 16)
            bm.create_snapshot(0x00, label="edit2", summary="Second edit", files=[target_c, target_bin])
            target_c.write_text("/* modified2 */", encoding="utf-8")

            backups = bm.list_backups(0x00)
            self.assertEqual(len(backups), 3)  # 1 original + 2 snapshots
            orig = next(b for b in backups if b.kind == "original")
            bm.restore_backup(orig)
            self.assertEqual(target_c.read_text(encoding="utf-8"), "/* original */")
            self.assertEqual(target_bin.read_bytes(), b"ORIGBIN" * 16)

        gm = backup_git.GitManager(self.repo)
        self.assertTrue(gm.is_repo())
        st = gm.get_status()
        groups = gm.suggest_commit_groups(st, active_char_id=0x00)
        self.assertIsInstance(groups, list)

    def test_06_physics_computation(self) -> None:
        """Physics helper must match known Ryu cr.HP (0x08) and Akuma hard KD properties."""
        # Ryu cr.HP: dmg=11, BU_O|NO_VEC, pow=11, vec=0, NO_WEIGHT|BLACK_COMBO, guard=0, K_LL
        p = constants.compute_move_physics(
            damage=11,
            damage_type=constants.BU_O | constants.NO_VEC,
            atck_kind=constants.A_NOM,
            damage_kind=constants.K_LL,
            vector=0,
            power=11,
            guard_time=0,
            shc=0,
            hit_flag=constants.NO_WEIGHT | constants.BLACK_COMBO,
            whiff_gauge=8,
        )
        self.assertEqual(p.chip_damage, 0)
        self.assertEqual(p.blockstun_frames, 18)
        self.assertAlmostEqual(p.launch_angle_deg, 90.0, places=2)
        self.assertGreater(p.launch_peak_height_dots, 300.0)
        self.assertIn("Launcher", p.knockdown_label)

        # Hard knockdown + special chip + meter test
        p_hkd = constants.compute_move_physics(
            damage=16,
            damage_type=21,
            atck_kind=constants.A_SPECIAL | constants.A_HI,
            damage_kind=constants.K_M,
            vector=0x1A,
            power=8,
            guard_time=20,
            shc=12,
            hit_flag=0,
        )
        self.assertEqual(p_hkd.chip_damage, 2)
        self.assertEqual(p_hkd.blockstun_frames, 20)
        self.assertEqual(p_hkd.hit_meter_attacker, 12)
        self.assertIn("HARD Knockdown", p_hkd.knockdown_label)

    def test_07_gui_smoke_and_hitbox_drag(self) -> None:
        """Instantiate MoveEditorApp headless, switch actions/chars, step frames, edit props & drag box."""
        tk_root = tk.Tk()
        tk_root.withdraw()
        try:
            app = MoveEditorApp(tk_root, self.repo)
            tk_root.update_idletasks()
            app.load_char(0x1E, prompt_dirty=False)  # Akuma
            self.assertEqual(app.char_id, 0x1E)

            # Switch to a special move action
            sp_act = next(a for a in app.filtered_actions if a.group == 21)
            app._select_action(sp_act)
            app.next_tick()
            app.next_frame()
            app.prev_frame()

            # Programmatically edit a hitbox and move property without saving to repo
            app.selected_box_slot = "atck_box"
            app._add_box_to_slot()
            app._redraw_canvas()
            rect = app.box_screen_rects.get("atck_box")
            self.assertIsNotNone(rect)

            class FakeEvt:
                def __init__(self, x: float, y: float) -> None:
                    self.x = x
                    self.y = y

            x0, y0, x1, y1 = rect  # type: ignore[misc]
            app._on_canvas_lpress(FakeEvt(x1, (y0 + y1) / 2.0))
            self.assertEqual(app._drag_mode, "e")
            app._on_canvas_ldrag(FakeEvt(x1 + 15.0, (y0 + y1) / 2.0))
            app._on_canvas_lrelease(FakeEvt(x1 + 15.0, (y0 + y1) / 2.0))

            app.var_hd_damage.set(22)
            app.var_hd_special.set(True)
            app._on_hit_data_changed()
            self.assertEqual(app._current_hit_data().damage, 22)  # type: ignore[union-attr]

            app.load_char(0x34, prompt_dirty=False)  # Sentinel (32 SPR_PRM)
            self.assertEqual(app.char_id, 0x34)
            app.stop_play()
        finally:
            tk_root.destroy()

    def test_08_strider_moves_sync_undo_discard(self) -> None:
        """Verify Strider move grouping, LP/HP version sync, Warp N/A hit data, Undo/Redo & Discard."""
        tk_root = tk.Tk()
        tk_root.withdraw()
        try:
            app = MoveEditorApp(tk_root, self.repo)
            app.load_char(0x05, prompt_dirty=False)  # Strider Hiryu
            self.assertIsNotNone(app.char_catalog)

            # 1. Ame no Murakumo: LP -> 0x30, HP -> 0x32
            mura = app.char_catalog.move_by_key("sp_murakumo")  # type: ignore[union-attr]
            self.assertIsNotNone(mura)
            idx_mura = app.char_catalog.moves.index(mura) + 1  # type: ignore[union-attr]
            app.cb_move.current(idx_mura)
            app._on_move_selected()
            self.assertEqual(app.current_atck_no, 0x30)
            hp_idx = next(i for i, v in enumerate(mura.versions) if v.key == "HP")  # type: ignore[union-attr]
            app.cb_version.current(hp_idx)
            app._on_version_selected()
            self.assertEqual(app.current_atck_no, 0x32)

            # 2. Warp: no attack hitbox -> no_active_hit_data=True
            warp = app.char_catalog.move_by_key("sp_warp")  # type: ignore[union-attr]
            app.cb_move.current(app.char_catalog.moves.index(warp) + 1)  # type: ignore[union-attr]
            app._on_move_selected()
            self.assertTrue(app.no_active_hit_data)
            self.assertIn("No Attack Hitbox", app.var_hd_summary.get())

            # 3. Formation A includes projectile 23:16 and 23:35
            fa = app.char_catalog.move_by_key("sp_formation_a")  # type: ignore[union-attr]
            refs = {s.ref for s in fa.states}  # type: ignore[union-attr]
            self.assertIn((21, 17), refs)
            self.assertIn((23, 16), refs)
            self.assertIn((23, 35), refs)

            # 4. Sync from Move Hit Data (0x4c Excalibur LP) jumps to Excalibur state
            app._select_atck_no(0x4C, from_sync=False)
            self.assertIsNotNone(app.current_move)
            self.assertEqual(app.current_move.key, "sp_jexcalibur")  # type: ignore[union-attr]
            self.assertEqual(app.current_action.group, 21)  # type: ignore[union-attr]

            # 5. Undo / Redo / Discard Action
            orig_dmg = app._current_hit_data().damage  # type: ignore[union-attr]
            app.var_hd_damage.set(orig_dmg + 5)
            app._on_hit_data_changed()
            self.assertEqual(app._current_hit_data().damage, orig_dmg + 5)  # type: ignore[union-attr]
            app.undo()
            self.assertEqual(app._current_hit_data().damage, orig_dmg)  # type: ignore[union-attr]
            app.redo()
            self.assertEqual(app._current_hit_data().damage, orig_dmg + 5)  # type: ignore[union-attr]

            app.discard_selected_action()
            self.assertEqual(app._current_hit_data().damage, orig_dmg)  # type: ignore[union-attr]
            app.undo()  # Discard itself is undoable
            self.assertEqual(app._current_hit_data().damage, orig_dmg + 5)  # type: ignore[union-attr]

            # 6. Temp test patch & restore leaves PL05_DAT.BIN identical afterward
            dat_path = self.disc / "PL05_DAT.BIN"
            if dat_path.exists():
                orig_bytes = dat_path.read_bytes()
                patched, err = app._prepare_temp_test_patch(include_src_c=False)
                self.assertIsNone(err)
                self.assertEqual(patched, dat_path)
                self.assertNotEqual(dat_path.read_bytes(), orig_bytes)
                self.assertTrue(app._restore_temp_test_patch())
                self.assertEqual(dat_path.read_bytes(), orig_bytes)

            app.stop_play()
        finally:
            tk_root.destroy()

    def test_09_cancel_options_and_overrides(self) -> None:
        """Verify cancel status display (chain/special/hyper/jump) and custom cancel rule toggling + C roundtrip."""
        # 1. C roundtrip in temp dir
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            (tmp / "src").mkdir()
            shutil.copy2(self.repo / "src" / "pl_com.c", tmp / "src" / "pl_com.c")
            rules = [c_data.CancelRule(0x05, 1, 5, 0x07, "Strider Gram")]
            self.assertTrue(c_data.save_cancel_rules(tmp, rules))
            reloaded = c_data.parse_cancel_rules(tmp)
            self.assertEqual(len(reloaded), 1)
            self.assertEqual(reloaded[0].pl_type, 0x05)
            self.assertEqual(reloaded[0].act_kind, 1)
            self.assertEqual(reloaded[0].move_id, 5)
            self.assertEqual(reloaded[0].flags, 0x07)

        # 2. GUI display & toggle
        tk_root = tk.Tk()
        tk_root.withdraw()
        try:
            app = MoveEditorApp(tk_root, self.repo)
            app.load_char(0x05, prompt_dirty=False)
            # Stand LP (7:0)
            app._jump_to_state(7, 0)
            st_txt = app.var_cancel_status.get()
            self.assertIn("Normal Chain:", st_txt)
            self.assertIn("HP", st_txt)

            # Gram (sp_gram)
            gram = app.char_catalog.move_by_key("sp_gram")  # type: ignore[union-attr]
            app.cb_move.current(app.char_catalog.moves.index(gram) + 1)  # type: ignore[union-attr]
            app._on_move_selected()
            self.assertIn("Special Move", app.var_cancel_status.get())

            app.var_cancel_jump.set(True)
            app.var_cancel_special.set(True)
            app._on_cancel_changed()
            self.assertTrue(app.cancel_dirty)
            self.assertIn("Jump: Yes (Override)", app.var_cancel_status.get())
            self.assertIn("Special: Yes (Override)", app.var_cancel_status.get())

            app.undo()
            self.assertFalse(app.var_cancel_jump.get())
            app.stop_play()
        finally:
            tk_root.destroy()


if __name__ == "__main__":
    unittest.main(verbosity=2)
