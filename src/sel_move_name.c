/* The name of each character's special moves (its pl_sp_type), which the select screen's assist
   pillars show. Written by tools/sel_move_names.py from the comments of include/plXX_def.h: run it
   again after adding, renumbering or renaming a move. (The headers can't all be included at once.) */
#include "player.h"

typedef struct SEL_MOVE_NAME {
	Sint16	sp;		/* the move's pl_sp_type, -1 at the end */
	const char	*name;
} SEL_MOVE_NAME;

static const SEL_MOVE_NAME sel_move_name_00[] = {
	{ 0, "Hadouken" },	/* pl00_sp_hadou */
	{ 1, "Shoryuken" },	/* pl00_sp_shouryu */
	{ 2, "Tatsumaki Senpuukyaku" },	/* pl00_sp_tatsu */
	{ 3, "Air Tatsumaki Senpuukyaku" },	/* pl00_sp_jtatsu */
	{ 4, "Air Hadouken" },	/* pl00_sp_jhadou */
	{ 5, "Shinkuu Hadouken" },	/* pl00_sp_shinkuu */
	{ 6, "Shinkuu Tatsumaki Senpuukyaku" },	/* pl00_sp_shinkuu_tatsu */
	{ 8, "Taunt" },	/* pl00_sp_chouhatsu */
	{ 11, "Shin Shoryuken" },	/* pl00_sp_shin_shouryu */
	{ 13, "Snapback" },	/* pl00_sp_snapback */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_01[] = {
	{ 0, "Double Lariat" },	/* pl01_sp_lariat */
	{ 1, "Quick Double Lariat" },	/* pl01_sp_qlariat */
	{ 2, "Banishing Flat" },	/* pl01_sp_banish */
	{ 3, "Spinning Pile Driver" },	/* pl01_sp_screw */
	{ 4, "Flying Powerbomb" },	/* pl01_sp_powerbomb */
	{ 5, "Final Atomic Buster" },	/* pl01_sp_fab */
	{ 6, "Aerial Russian Slam" },	/* pl01_sp_russian */
	{ 8, "His part in a team hyper combo" },	/* pl01_sp_team */
	{ 9, "Taunt" },	/* pl01_sp_chouhatsu */
	{ 17, "Ultra Final Atomic Buster" },	/* pl01_sp_uab */
	{ 18, "Iron Body" },	/* pl01_sp_iron */
	{ 19, "Vodka Fire" },	/* pl01_sp_vodka */
	{ 20, "Siberian Blizzard" },	/* pl01_sp_blizzard */
	{ 21, "Snapback" },	/* pl01_sp_snapback */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_02[] = {
	{ 0, "Sonic Boom" },	/* pl02_sp_sonic */
	{ 1, "Somersault Kick" },	/* pl02_sp_smr */
	{ 2, "Air Somersault Kick" },	/* pl02_sp_jsmr */
	{ 3, "Somersault Strike" },	/* pl02_sp_strike */
	{ 4, "Sonic Hurricane" },	/* pl02_sp_hurricane */
	{ 5, "Snapback" },	/* pl02_sp_snapback */
	{ 6, "Taunt" },	/* pl02_sp_chouhatsu */
	{ 7, "Crossfire Assault (hyper" },	/* pl02_sp_crossfire */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_03[] = {
	{ 1, "Soul Fist" },	/* pl03_sp_soul */
	{ 2, "Shadow Blade" },	/* pl03_sp_blade */
	{ 3, "Vector Drain" },	/* pl03_sp_vector */
	{ 6, "Darkness Illusion" },	/* pl03_sp_illusion */
	{ 7, "Finishing Shower" },	/* pl03_sp_shower */
	{ 8, "Taunt" },	/* pl03_sp_chouhatsu */
	{ 9, "Silhouette Blade" },	/* pl03_sp_silhouette */
	{ 14, "Snapback" },	/* pl03_sp_snapback */
	{ 15, "Shell Kick" },	/* pl03_sp_shell */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_04[] = {
	{ 0, "Coffin Dance" },	/* pl04_sp_coffin */
	{ 1, "Royal Judgement" },	/* pl04_sp_judgement */
	{ 2, "Cobra Blow" },	/* pl04_sp_cobra */
	{ 3, "Mummy Drop" },	/* pl04_sp_mummy */
	{ 4, "Pharaoh Magic" },	/* pl04_sp_magic */
	{ 5, "Pharaoh Cobra Throw" },	/* pl04_sp_cobras */
	{ 6, "Pharaoh Illusion" },	/* pl04_sp_illusion */
	{ 7, "Coffin Fall" },	/* pl04_sp_coffins */
	{ 8, "Snapback" },	/* pl04_sp_snapback */
	{ 9, "Taunt" },	/* pl04_sp_chouhatsu */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_05[] = {
	{ 0, "Ame no Murakumo" },	/* pl05_sp_murakumo */
	{ 3, "Warp" },	/* pl05_sp_warp */
	{ 4, "Ouroboros" },	/* pl05_sp_ouroboros */
	{ 5, "Gram" },	/* pl05_sp_gram */
	{ 6, "Low Gram" },	/* pl05_sp_lgram */
	{ 7, "Gram" },	/* pl05_sp_jgram */
	{ 8, "Kabe Hari Tsuki" },	/* pl05_sp_kabe */
	{ 9, "Formation A" },	/* pl05_sp_formation_a */
	{ 10, "Legion" },	/* pl05_sp_legion */
	{ 11, "Excalibur" },	/* pl05_sp_jexcalibur */
	{ 12, "Ragnarok" },	/* pl05_sp_ragnarok */
	{ 13, "Vajra" },	/* pl05_sp_vajra */
	{ 14, "Formation C" },	/* pl05_sp_formation_c */
	{ 15, "Formation B" },	/* pl05_sp_formation_b */
	{ 16, "Formation B" },	/* pl05_sp_formation_b_shot */
	{ 17, "Taunt" },	/* pl05_sp_chouhatsu */
	{ 22, "Snapback" },	/* pl05_sp_snapback */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_06[] = {
	{ 0, "Optic Blast" },	/* pl06_sp_blast */
	{ 1, "Super Optic Blast" },	/* pl06_sp_super */
	{ 2, "Mega Optic Blast" },	/* pl06_sp_mega */
	{ 3, "Gene Splice" },	/* pl06_sp_gene */
	{ 4, "Running Neckbreaker" },	/* pl06_sp_neckbreaker */
	{ 7, "Cyclone Kick" },	/* pl06_sp_cyclone */
	{ 8, "Optic Sweep" },	/* pl06_sp_sweep */
	{ 9, "Rapid Punches" },	/* pl06_sp_rapid */
	{ 11, "Taunt" },	/* pl06_sp_chouhatsu */
	{ 12, "Snapback" },	/* pl06_sp_snapback */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_07[] = {
	{ 0, "Tornado Claw" },	/* pl07_sp_tornado */
	{ 1, "Drill Claw" },	/* pl07_sp_drill */
	{ 2, "Berserker Barrage" },	/* pl07_sp_barrage */
	{ 3, "Berserker Slash" },	/* pl07_sp_slash */
	{ 4, "Weapon X" },	/* pl07_sp_weapon_x */
	{ 5, "Berserker Barrage X" },	/* pl07_sp_barrage_x */
	{ 6, "Fatal Claw" },	/* pl07_sp_fatal */
	{ 7, "Berserker Charge" },	/* pl07_sp_charge */
	{ 8, "Snapback" },	/* pl07_sp_snapback */
	{ 9, "Taunt" },	/* pl07_sp_chouhatsu */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_08[] = {
	{ 0, "Psy-Blast" },	/* pl08_sp_blast */
	{ 1, "Psy-Blade" },	/* pl08_sp_spin */
	{ 2, "Kocho-Gakure" },	/* pl08_sp_butterfly */
	{ 3, "Psy-Thrust" },	/* pl08_sp_thrust */
	{ 4, "Ninjutsu" },	/* pl08_sp_ninjitsu */
	{ 5, "Taunt" },	/* pl08_sp_chouhatsu */
	{ 6, "Psy-Maelstrom" },	/* pl08_sp_maelstrom */
	{ 7, "Leaping Slash" },	/* pl08_sp_leap */
	{ 8, "Snapback" },	/* pl08_sp_snapback */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_09[] = {
	{ 0, "Ice Avalanche" },	/* pl09_sp_avalanche */
	{ 1, "Ice Beam" },	/* pl09_sp_beam */
	{ 2, "Arctic Attack" },	/* pl09_sp_arctic */
	{ 3, "Ice Fist" },	/* pl09_sp_fist */
	{ 4, "Ice Beam" },	/* pl09_sp_jbeam */
	{ 5, "Taunt" },	/* pl09_sp_chouhatsu */
	{ 6, "Snapback" },	/* pl09_sp_snapback */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_0a[] = {
	{ 3, "Repeating Punches" },	/* pl0a_sp_repeat */
	{ 8, "Power Leap" },	/* pl0a_sp_leap */
	{ 11, "Power Drain" },	/* pl0a_sp_drain */
	{ 12, "Screw Piledriver" },	/* pl0a_sp_screw */
	{ 13, "Rising Repeating Punches" },	/* pl0a_sp_rising */
	{ 17, "Cannon Spike" },	/* pl0a_sp_spike */
	{ 18, "Berserker Barrage" },	/* pl0a_sp_barrage */
	{ 19, "Taunt" },	/* pl0a_sp_chouhatsu */
	{ 20, "Goodnight Sugah" },	/* pl0a_sp_sugah */
	{ 22, "Dive Punch" },	/* pl0a_sp_dive */
	{ 23, "Shun Goku Satsu" },	/* pl0a_sp_shungoku */
	{ 24, "Team Hyper" },	/* pl0a_sp_team */
	{ 26, "Snapback" },	/* pl0a_sp_snapback */
	{ 27, "Berserker Barrage X" },	/* pl0a_sp_barragex */
	{ 28, "Shoryu-Reppa" },	/* pl0a_sp_reppa */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_0b[] = {
	{ 0, "Shield Slash" },	/* pl0b_sp_slash */
	{ 1, "Stars & Stripes" },	/* pl0b_sp_stars */
	{ 2, "Roll" },	/* pl0b_sp_cartwheel */
	{ 3, "Charging Star" },	/* pl0b_sp_charge */
	{ 4, "Taunt" },	/* pl0b_sp_chouhatsu */
	{ 5, "Hyper Stars & Stripes" },	/* pl0b_sp_hstars */
	{ 6, "Final Justice" },	/* pl0b_sp_justice */
	{ 8, "Hyper Charging Star" },	/* pl0b_sp_hcharge */
	{ 14, "Snapback" },	/* pl0b_sp_snapback */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_0c[] = {
	{ 0, "Spider Sting" },	/* pl0c_sp_sting */
	{ 1, "Web Ball" },	/* pl0c_sp_webball */
	{ 2, "Web Swing" },	/* pl0c_sp_swing */
	{ 3, "Web Throw" },	/* pl0c_sp_webthrow */
	{ 4, "Maximum Spider" },	/* pl0c_sp_maximum */
	{ 5, "Crawler Assault" },	/* pl0c_sp_crawler */
	{ 6, "Ultimate Web Throw" },	/* pl0c_sp_hwebthrow */
	{ 7, "Taunt" },	/* pl0c_sp_chouhatsu */
	{ 8, "Snapback" },	/* pl0c_sp_snapback */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_0d[] = {
	{ 0, "Gamma Charge" },	/* pl0d_sp_charge */
	{ 2, "Gamma Slam" },	/* pl0d_sp_slam */
	{ 3, "Gamma Crush" },	/* pl0d_sp_crush */
	{ 4, "Gamma Tornado" },	/* pl0d_sp_tornado */
	{ 5, "Gamma Charge" },	/* pl0d_sp_ucharge */
	{ 6, "Gamma Wave" },	/* pl0d_sp_wave */
	{ 9, "Taunt" },	/* pl0d_sp_chouhatsu */
	{ 10, "Gamma Quake" },	/* pl0d_sp_quake */
	{ 11, "Snapback" },	/* pl0d_sp_snapback */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_0e[] = {
	{ 0, "Venom Fang" },	/* pl0e_sp_fang */
	{ 1, "Venom Rush" },	/* pl0e_sp_rush */
	{ 2, "Web Throw" },	/* pl0e_sp_webthrow */
	{ 3, "Venom Web" },	/* pl0e_sp_web */
	{ 4, "Death Bite" },	/* pl0e_sp_bite */
	{ 5, "Taunt" },	/* pl0e_sp_chouhatsu */
	{ 6, "Launcher" },	/* pl0e_sp_gamma */
	{ 7, "Snapback" },	/* pl0e_sp_snapback */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_0f[] = {
	{ 0, "Plasma Beam" },	/* pl0f_sp_plasma */
	{ 1, "Photon Shot" },	/* pl0f_sp_photon */
	{ 2, "Molecular Shield" },	/* pl0f_sp_shield */
	{ 3, "Photon Array" },	/* pl0f_sp_array */
	{ 4, "Electric Cage" },	/* pl0f_sp_cage */
	{ 5, "Sphere Flame" },	/* pl0f_sp_flame */
	{ 6, "Taunt" },	/* pl0f_sp_chouhatsu */
	{ 7, "Snapback" },	/* pl0f_sp_snapback */
	{ 8, "Flight" },	/* pl0f_sp_fly */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_10[] = {
	{ 0, "Bonne Strike" },	/* pl10_sp_strike */
	{ 1, "Beacon Bomb" },	/* pl10_sp_beacon */
	{ 2, "Kobun Launch" },	/* pl10_sp_kobun */
	{ 3, "Taunt" },	/* pl10_sp_chouhatsu */
	{ 4, "King Kobun" },	/* pl10_sp_king */
	{ 5, "Lunch Rush" },	/* pl10_sp_lunch */
	{ 6, "Snapback" },	/* pl10_sp_snapback */
	{ 7, "Throw" },	/* pl10_sp_throw */
	{ 8, "Kick" },	/* pl10_sp_kick */
	{ 9, "Gustaff Fire" },	/* pl10_sp_fire */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_11[] = {
	{ 0, "Cerberus Escape" },	/* pl11_sp_cerberus */
	{ 1, "Crow Escape" },	/* pl11_sp_crow */
	{ 2, "Zombie Escape" },	/* pl11_sp_zombie */
	{ 3, "Return Fire" },	/* pl11_sp_return */
	{ 4, "Charging Stars" },	/* pl11_sp_stars */
	{ 5, "Grenade Launcher" },	/* pl11_sp_grenade */
	{ 6, "Hyper Charging Stars" },	/* pl11_sp_hstars */
	{ 7, "Tyrant Escape" },	/* pl11_sp_tyrant */
	{ 8, "Rocket Launcher" },	/* pl11_sp_rocket */
	{ 9, "Heal" },	/* pl11_sp_heal */
	{ 10, "Taunt" },	/* pl11_sp_chouhatsu */
	{ 11, "Snapback" },	/* pl11_sp_snapback */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_12[] = {
	{ 0, "Shiden" },	/* pl12_sp_shiden */
	{ 1, "Guren" },	/* pl12_sp_guren */
	{ 2, "Byakkou Hou" },	/* pl12_sp_byakko */
	{ 3, "Engetsu" },	/* pl12_sp_engetsu */
	{ 4, "Rasetsu Zan" },	/* pl12_sp_rasetsu */
	{ 5, "Plasma Field" },	/* pl12_sp_field */
	{ 6, "Snapback" },	/* pl12_sp_snapback */
	{ 7, "Guren" },	/* pl12_sp_counter */
	{ 8, "Black Hayato" },	/* pl12_sp_black */
	{ 9, "Taunt" },	/* pl12_sp_chouhatsu */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_13[] = {
	{ 0, "Schwarzaile" },	/* pl13_sp_schwarz */
	{ 1, "Fantome" },	/* pl13_sp_fantome */
	{ 2, "Sublimation" },	/* pl13_sp_sublimation */
	{ 3, "Flammeile" },	/* pl13_sp_flammeile */
	{ 4, "Mille Fantomes" },	/* pl13_sp_mille */
	{ 5, "Pare-tonn're" },	/* pl13_sp_paretonnre */
	{ 6, "Rafale Cannon" },	/* pl13_sp_rafale */
	{ 7, "Carry Throw" },	/* pl13_sp_throw */
	{ 8, "Power Up" },	/* pl13_sp_power */
	{ 9, "Tour De Magie" },	/* pl13_sp_magie */
	{ 10, "Snapback" },	/* pl13_sp_snapback */
	{ 11, "Taunt" },	/* pl13_sp_chouhatsu */
	{ 12, "Flammeile as her part of a team hyper" },	/* pl13_sp_flammeile_team */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_14[] = {
	{ 0, "Shienbu" },	/* pl14_sp_shienbu */
	{ 1, "Fuusetsu Zan" },	/* pl14_sp_fuusetsu */
	{ 2, "Seiten Rengeki" },	/* pl14_sp_seiten */
	{ 3, "Hofuku Zenshin" },	/* pl14_sp_hofuku */
	{ 4, "Tenchi Tsuukan" },	/* pl14_sp_tenchi */
	{ 5, "Kingin no Hisago" },	/* pl14_sp_hisago */
	{ 6, "Pow" },	/* pl14_sp_pow */
	{ 7, "En'Ou" },	/* pl14_sp_enou */
	{ 8, "Taunt" },	/* pl14_sp_chouhatsu */
	{ 9, "Peach" },	/* pl14_sp_peach */
	{ 10, "Snapback" },	/* pl14_sp_snapback */
	{ 11, "Wall Walk" },	/* pl14_sp_wall */
	{ 12, "En'Ou" },	/* pl14_sp_enou_auto */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_15[] = {
	{ 0, "Daichi no Kodomo" },	/* pl15_sp_daichi_catch */
	{ 1, "Midori no Kanki" },	/* pl15_sp_midori */
	{ 2, "Kaze no Sakebi" },	/* pl15_sp_kaze */
	{ 3, "Taiyou no Megumi" },	/* pl15_sp_taiyou */
	{ 4, "Bite" },	/* pl15_sp_bite */
	{ 5, "Shokubutsu no Okite" },	/* pl15_sp_okite */
	{ 6, "Taunt" },	/* pl15_sp_chouhatsu */
	{ 7, "Snapback" },	/* pl15_sp_snapback */
	{ 8, "Daichi no Kodomo" },	/* pl15_sp_daichi */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_16[] = {
	{ 0, "Bone-merang" },	/* pl16_sp_bone */
	{ 1, "Towering Spine" },	/* pl16_sp_spine */
	{ 2, "Ricochet Slash" },	/* pl16_sp_ricochet */
	{ 3, "Ride and Slash" },	/* pl16_sp_ride */
	{ 4, "Stinger Bones" },	/* pl16_sp_stinger */
	{ 5, "Bone Burst" },	/* pl16_sp_burst */
	{ 6, "Snapback" },	/* pl16_sp_snapback */
	{ 7, "Taunt" },	/* pl16_sp_chouhatsu */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_17[] = {
	{ 0, "Hyper Viper Beam" },	/* pl17_sp_hviper */
	{ 1, "Time Flip" },	/* pl17_sp_timeflip */
	{ 2, "Psy-Charge" },	/* pl17_sp_psycharge */
	{ 3, "Psimitar" },	/* pl17_sp_psimitar */
	{ 4, "Viper Beam" },	/* pl17_sp_viper */
	{ 5, "Crackdown" },	/* pl17_sp_crackdown */
	{ 6, "Electrap" },	/* pl17_sp_electrap */
	{ 7, "Taunt" },	/* pl17_sp_chouhatsu */
	{ 8, "Snapback" },	/* pl17_sp_snapback */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_18[] = {
	{ 0, "Dark Fist" },	/* pl18_sp_punch */
	{ 1, "Dark Thunder" },	/* pl18_sp_thunder */
	{ 2, "Dark Balls" },	/* pl18_sp_ball */
	{ 3, "Dark Pillars" },	/* pl18_sp_pillar */
	{ 5, "Sweeping Blow" },	/* pl18_sp_sweep */
	{ 6, "Charging Rush" },	/* pl18_sp_rush */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_19[] = {
	{ 0, "Bubbles" },	/* pl19_sp_bubble */
	{ 1, "Shot Storm" },	/* pl19_sp_storm */
	{ 2, "Beam" },	/* pl19_sp_beam */
	{ 3, "Side Blast" },	/* pl19_sp_blast */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_1a[] = {
	{ 0, "Shot Storm" },	/* pl1a_sp_storm */
	{ 1, "Stomp" },	/* pl1a_sp_stomp */
	{ 2, "Jumping Fire" },	/* pl1a_sp_jump */
	{ 3, "Floor Wave" },	/* pl1a_sp_wave */
	{ 4, "Shot Spray" },	/* pl1a_sp_spray */
	{ 5, "Blast" },	/* pl1a_sp_blast */
	{ 8, "Leaping Throw" },	/* pl1a_sp_throw */
	{ 11, "Dark Flames" },	/* pl1a_sp_flame */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_1b[] = {
	{ 0, "Kikou Ken" },	/* pl1b_sp_kikou */
	{ 1, "Tenshoukyaku" },	/* pl1b_sp_tenshou */
	{ 2, "Sen'en Shuu" },	/* pl1b_sp_senen */
	{ 3, "Kikoushou" },	/* pl1b_sp_kikoushou */
	{ 4, "Senretsukyaku" },	/* pl1b_sp_senretsu */
	{ 5, "Hazan Tenshoukyaku" },	/* pl1b_sp_hazan */
	{ 6, "Hyakuretsukyaku" },	/* pl1b_sp_hyakuretsu */
	{ 8, "Taunt" },	/* pl1b_sp_chouhatsu */
	{ 11, "Shichisei Ranka" },	/* pl1b_sp_shichisei */
	{ 15, "Snap Back" },	/* pl1b_sp_snapback */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_1c[] = {
	{ 0, "Mega Buster" },	/* pl1c_sp_buster */
	{ 2, "Mega Uppercut" },	/* pl1c_sp_upper */
	{ 3, "Taunt" },	/* pl1c_sp_chouhatsu */
	{ 4, "Rush Drill" },	/* pl1c_sp_drill */
	{ 5, "Beat Plane" },	/* pl1c_sp_beat */
	{ 6, "Weapon Activation" },	/* pl1c_sp_item */
	{ 7, "Hyper Mega Man" },	/* pl1c_sp_hyper_rock */
	{ 8, "Snap Back" },	/* pl1c_sp_snapback */
	{ 12, "Summon power-up (Rockball" },	/* pl1c_sp_item_sel */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_1d[] = {
	{ 0, "Roll Buster" },	/* pl1d_sp_buster */
	{ 2, "Flowers" },	/* pl1d_sp_flower */
	{ 3, "Taunt" },	/* pl1d_sp_chouhatsu */
	{ 4, "Rush Drill" },	/* pl1d_sp_drill */
	{ 5, "Beat Plane" },	/* pl1d_sp_beat */
	{ 6, "Weapon Activation" },	/* pl1d_sp_item */
	{ 7, "Hyper Roll" },	/* pl1d_sp_hyper_roll */
	{ 8, "Snap Back" },	/* pl1d_sp_snapback */
	{ 12, "Summon power-up (Rockball" },	/* pl1d_sp_item_sel */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_1e[] = {
	{ 0, "Gou Hadouken" },	/* pl1e_sp_hadou */
	{ 1, "Gou Shouryuuken" },	/* pl1e_sp_shouryuu */
	{ 2, "Tatsumaki Zankuukyaku" },	/* pl1e_sp_tatsumaki */
	{ 3, "Air Tatsumaki Zankuukyaku" },	/* pl1e_sp_jtatsumaki */
	{ 4, "Zankuu Hadouken" },	/* pl1e_sp_zankuu */
	{ 5, "Tenma Gou Zankuu" },	/* pl1e_sp_tenma */
	{ 6, "Ashura Senkuu forward" },	/* pl1e_sp_ashura */
	{ 7, "Ashura Senkuu back" },	/* pl1e_sp_ashura_b */
	{ 8, "Shun Goku Satsu" },	/* pl1e_sp_shungoku */
	{ 11, "Messatsu Gou Shouryuu" },	/* pl1e_sp_gou_shouryuu */
	{ 13, "Messatsu Gou Hadou" },	/* pl1e_sp_gou_hadou */
	{ 14, "Tenma Kuujinkyaku" },	/* pl1e_sp_kuujin */
	{ 15, "Taunt" },	/* pl1e_sp_chouhatsu */
	{ 16, "Snap Back" },	/* pl1e_sp_snapback */
	{ 17, "Messatsu Gou Rasen" },	/* pl1e_sp_gou_rasen */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_1f[] = {
	{ 0, "Beautiful Memory" },	/* pl1f_sp_memory */
	{ 1, "Cool Hunting" },	/* pl1f_sp_hunting */
	{ 3, "Apple for You" },	/* pl1f_sp_apple */
	{ 4, "Shyness & Strike" },	/* pl1f_sp_shyness */
	{ 5, "Cheer & Fire" },	/* pl1f_sp_cheer */
	{ 6, "Smile & Missile" },	/* pl1f_sp_missile */
	{ 7, "Smile & Missile" },	/* pl1f_sp_happy */
	{ 8, "Smile & Missile" },	/* pl1f_sp_missile_j */
	{ 9, "Tell Me Why" },	/* pl1f_sp_tellme */
	{ 10, "Snap Back" },	/* pl1f_sp_snapback */
	{ 11, "Taunt" },	/* pl1f_sp_chouhatsu */
	{ 12, "Cheer & Fire (horizontal" },	/* pl1f_sp_cheer_k */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_20[] = {
	{ 0, "Delta Kick" },	/* pl20_sp_delta */
	{ 1, "Rolling Buckler" },	/* pl20_sp_rolling */
	{ 2, "Sand Splash" },	/* pl20_sp_sand */
	{ 3, "Cat Spike" },	/* pl20_sp_spike */
	{ 4, "Hell Cat" },	/* pl20_sp_hellcat */
	{ 6, "Dancing Flash" },	/* pl20_sp_dancing */
	{ 7, "Please Help Me" },	/* pl20_sp_help */
	{ 8, "Super Sand Splash" },	/* pl20_sp_ssand */
	{ 9, "Snapback" },	/* pl20_sp_snapback */
	{ 10, "Taunt" },	/* pl20_sp_chouhatsu */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_21[] = {
	{ 0, "Sonic Boom" },	/* pl21_sp_sonic */
	{ 1, "Somersault Shell" },	/* pl21_sp_smr */
	{ 2, "Sonic Break" },	/* pl21_sp_break */
	{ 3, "Somersault Justice" },	/* pl21_sp_justice */
	{ 4, "Crossfire Blitz" },	/* pl21_sp_blitz */
	{ 6, "Moonsault Slash" },	/* pl21_sp_moonsault */
	{ 7, "Taunt" },	/* pl21_sp_chouhatsu */
	{ 11, "Snapback" },	/* pl21_sp_snapback */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_22[] = {
	{ 0, "Shououken" },	/* pl22_sp_shouou */
	{ 1, "Shunpukyaku" },	/* pl22_sp_shunpu */
	{ 2, "Taunt" },	/* pl22_sp_chouhatsu */
	{ 5, "Hadouken" },	/* pl22_sp_hadou */
	{ 6, "Midare Zakura" },	/* pl22_sp_midare */
	{ 7, "Air Hadouken" },	/* pl22_sp_jhadou */
	{ 8, "Shinkuu Hadouken" },	/* pl22_sp_shinkuu */
	{ 11, "Shinkuu Hadouken" },	/* pl22_sp_dshinkuu */
	{ 12, "Air Shunpukyaku" },	/* pl22_sp_jshunpu */
	{ 13, "Air Shououken" },	/* pl22_sp_jshouou */
	{ 14, "Haru Ichiban" },	/* pl22_sp_haru */
	{ 16, "Ashura Senkuu forward" },	/* pl22_sp_ashura_f */
	{ 17, "Ashura Senkuu back" },	/* pl22_sp_ashura_b */
	{ 18, "Shun Goku Satsu" },	/* pl22_sp_shungoku */
	{ 19, "Snapback" },	/* pl22_sp_snapback */
	{ 20, "The change to and from the dark form" },	/* pl22_sp_dark */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_23[] = {
	{ 0, "Gadouken" },	/* pl23_sp_gadou */
	{ 1, "Kouryuuken" },	/* pl23_sp_kouryuu */
	{ 2, "Dankuukyaku" },	/* pl23_sp_danku */
	{ 3, "Shinkuu Gadouken" },	/* pl23_sp_shinkuu */
	{ 4, "Kouryuu Rekka" },	/* pl23_sp_rekka */
	{ 5, "Hisshou Buraiken" },	/* pl23_sp_buraiken */
	{ 8, "Taunt" },	/* pl23_sp_chouhatsu */
	{ 9, "Zenten Chouhatsu" },	/* pl23_sp_zenten */
	{ 10, "Kouten Chouhatsu" },	/* pl23_sp_kouten */
	{ 11, "Chouhatsu Densetsu" },	/* pl23_sp_densetsu */
	{ 12, "Hisshou Buraiken in the air (hyper" },	/* pl23_sp_jburaiken */
	{ 13, "Air Dankuukyaku" },	/* pl23_sp_jdanku */
	{ 14, "Premium Sign" },	/* pl23_sp_sign */
	{ 16, "Otoko Michi" },	/* pl23_sp_otoko */
	{ 17, "Snapback" },	/* pl23_sp_snapback */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_24[] = {
	{ 0, "Cannon Spike" },	/* pl24_sp_spike */
	{ 1, "Spiral Arrow" },	/* pl24_sp_arrow */
	{ 2, "Axle Spin Knuckle" },	/* pl24_sp_knuckle */
	{ 3, "Hooligan Combination" },	/* pl24_sp_hooligan */
	{ 5, "Taunt" },	/* pl24_sp_chouhatsu */
	{ 6, "Cannon Revenge" },	/* pl24_sp_revenge */
	{ 7, "Spin Drive Smasher" },	/* pl24_sp_smasher */
	{ 8, "Cannon Strike" },	/* pl24_sp_strike */
	{ 9, "Cannon Drill" },	/* pl24_sp_drill */
	{ 11, "Killer Bee Assault" },	/* pl24_sp_killerbee */
	{ 13, "Snapback" },	/* pl24_sp_snapback */
	{ 14, "Reverse Shaft Breaker" },	/* pl24_sp_shaft */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_25[] = {
	{ 0, "Yoga Fire" },	/* pl25_sp_fire */
	{ 1, "Yoga Flame" },	/* pl25_sp_frei */
	{ 2, "Yoga Blast" },	/* pl25_sp_brast */
	{ 3, "Yoga Teleport" },	/* pl25_sp_teleport */
	{ 4, "Yoga Float" },	/* pl25_sp_float */
	{ 5, "Yoga Inferno" },	/* pl25_sp_inferno */
	{ 6, "Yoga Strike" },	/* pl25_sp_strike */
	{ 9, "Taunt" },	/* pl25_sp_chouhatsu */
	{ 12, "Air Yoga Inferno" },	/* pl25_sp_jinferno */
	{ 13, "Snapback" },	/* pl25_sp_snapback */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_26[] = {
	{ 0, "Psycho Shot" },	/* pl26_sp_shot */
	{ 1, "Double Knee Press" },	/* pl26_sp_knee */
	{ 2, "Bison Warp" },	/* pl26_sp_warp */
	{ 3, "Head Press" },	/* pl26_sp_head */
	{ 4, "Somersault Skull Diver" },	/* pl26_sp_skull */
	{ 5, "Knee Press Nightmare" },	/* pl26_sp_nightmare */
	{ 6, "Psycho Crusher" },	/* pl26_sp_crusher */
	{ 7, "Psycho Field" },	/* pl26_sp_field */
	{ 9, "Flight" },	/* pl26_sp_fly */
	{ 11, "Taunt" },	/* pl26_sp_chouhatsu */
	{ 14, "Snapback" },	/* pl26_sp_snapback */
	{ 15, "Psycho Explosion" },	/* pl26_sp_explosion */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_27[] = {
	{ 0, "Hadouken" },	/* pl27_sp_hadou */
	{ 1, "Shouryuuken" },	/* pl27_sp_shouryuu */
	{ 2, "Tatsumaki Senpuukyaku" },	/* pl27_sp_tatsumaki */
	{ 3, "Air Tatsumaki Senpuukyaku" },	/* pl27_sp_jtatsumaki */
	{ 4, "Air Hadouken" },	/* pl27_sp_jhadou */
	{ 5, "Zenpou Tenshin" },	/* pl27_sp_zenpou */
	{ 6, "Shouryuu Reppa" },	/* pl27_sp_reppa */
	{ 7, "Shinryuuken" },	/* pl27_sp_shinryuu */
	{ 9, "Taunt" },	/* pl27_sp_chouhatsu */
	{ 11, "Air Shouryuuken" },	/* pl27_sp_jshouryuu */
	{ 14, "Shippuu Jinraikyaku" },	/* pl27_sp_shippu */
	{ 16, "Snapback" },	/* pl27_sp_snapback */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_28[] = {
	{ 1, "Kinetic Card" },	/* pl28_sp_card */
	{ 2, "Trick Card" },	/* pl28_sp_trick */
	{ 3, "Cajun Slash" },	/* pl28_sp_slash */
	{ 4, "Cajun Strike" },	/* pl28_sp_strike */
	{ 5, "Royal Flush" },	/* pl28_sp_royal */
	{ 6, "Taunt" },	/* pl28_sp_chouhatsu */
	{ 7, "Cajun Escape" },	/* pl28_sp_escape */
	{ 8, "Cajun Slash" },	/* pl28_sp_slash2 */
	{ 9, "Kinetic Card" },	/* pl28_sp_jcard */
	{ 10, "Cajun Explosion" },	/* pl28_sp_explosion */
	{ 14, "Cajun Explosion" },	/* pl28_sp_explosion_f */
	{ 15, "Crouching HP" },	/* pl28_sp_crouch_hp */
	{ 16, "Snapback" },	/* pl28_sp_snapback */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_29[] = {
	{ 0, "Earthquake Punch" },	/* pl29_sp_quake */
	{ 1, "Juggernaut Headcrush" },	/* pl29_sp_headcrush */
	{ 2, "Cytorrak Power-up" },	/* pl29_sp_powerup */
	{ 3, "Juggernaut Punch" },	/* pl29_sp_punch */
	{ 4, "Taunt" },	/* pl29_sp_chouhatsu */
	{ 7, "Juggernaut Body Press" },	/* pl29_sp_press */
	{ 9, "Snapback" },	/* pl29_sp_snapback */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_2a[] = {
	{ 0, "Typhoon" },	/* pl2a_sp_typhoon */
	{ 1, "Lightning Storm" },	/* pl2a_sp_lstorm */
	{ 2, "Lightning Attack" },	/* pl2a_sp_lattack_a */
	{ 3, "Flight" },	/* pl2a_sp_fly */
	{ 4, "Lightning Attack" },	/* pl2a_sp_lattack */
	{ 5, "Hail Storm" },	/* pl2a_sp_hail */
	{ 6, "Double Typhoon" },	/* pl2a_sp_dtyphoon */
	{ 8, "Taunt" },	/* pl2a_sp_chouhatsu */
	{ 11, "Snapback" },	/* pl2a_sp_snapback */
	{ 12, "Lightning Sphere" },	/* pl2a_sp_jsphere */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_2b[] = {
	{ 0, "Berserker Claw" },	/* pl2b_sp_berserk */
	{ 1, "Birdie" },	/* pl2b_sp_birdie */
	{ 2, "Wild Fang" },	/* pl2b_sp_fang */
	{ 3, "Berserker Claw X" },	/* pl2b_sp_clawx */
	{ 4, "Hyper Birdie" },	/* pl2b_sp_hbirdie */
	{ 5, "Weapon X Rush" },	/* pl2b_sp_weapon */
	{ 6, "Berserker Claw" },	/* pl2b_sp_06 */
	{ 7, "Crouching HP" },	/* pl2b_sp_crouch_hp */
	{ 8, "Snapback" },	/* pl2b_sp_snapback */
	{ 9, "Taunt" },	/* pl2b_sp_chouhatsu */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_2c[] = {
	{ 0, "EM Disruptor" },	/* pl2c_sp_disrupt */
	{ 1, "Hyper Grav" },	/* pl2c_sp_grav */
	{ 2, "Magnetic Shockwave" },	/* pl2c_sp_shock */
	{ 3, "Magnetic Tempest" },	/* pl2c_sp_tempest */
	{ 4, "Flight" },	/* pl2c_sp_fly */
	{ 6, "Taunt" },	/* pl2c_sp_chouhatsu */
	{ 7, "Magnetic Blast" },	/* pl2c_sp_jblast */
	{ 8, "Magnetic Forcefield" },	/* pl2c_sp_field */
	{ 12, "Snapback" },	/* pl2c_sp_snapback */
	{ 13, "Crouching HP" },	/* pl2c_sp_crouch_hp */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_2d[] = {
	{ 0, "Mystic Stare" },	/* pl2d_sp_stare */
	{ 1, "Mystic Smash" },	/* pl2d_sp_smash */
	{ 2, "Devitalization" },	/* pl2d_sp_devital */
	{ 3, "Chaos Dimension" },	/* pl2d_sp_chaos */
	{ 5, "Hyper Mystic Smash" },	/* pl2d_sp_hsmash */
	{ 6, "Taunt" },	/* pl2d_sp_chouhatsu */
	{ 7, "Snapback" },	/* pl2d_sp_snapback */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_2e[] = {
	{ 0, "Repulsor Blast" },	/* pl2e_sp_repulsor */
	{ 1, "Proton Cannon" },	/* pl2e_sp_proton */
	{ 2, "Shoulder Cannon" },	/* pl2e_sp_cannon */
	{ 3, "Smart Bomb" },	/* pl2e_sp_bomb */
	{ 4, "Taunt" },	/* pl2e_sp_chouhatsu */
	{ 5, "Flight" },	/* pl2e_sp_fly */
	{ 6, "War Destroyer" },	/* pl2e_sp_destroyer */
	{ 8, "Snapback" },	/* pl2e_sp_snapback */
	{ 12, "Shoulder Cannon" },	/* pl2e_sp_ccannon */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_2f[] = {
	{ 0, "Shuriken" },	/* pl2f_sp_shuriken */
	{ 1, "Shuriken" },	/* pl2f_sp_jshuriken */
	{ 2, "Raimeiken" },	/* pl2f_sp_raimei */
	{ 3, "Hyakuretsutou" },	/* pl2f_sp_rapid */
	{ 4, "Bushin" },	/* pl2f_sp_bushin */
	{ 5, "Hyougaken (hyper" },	/* pl2f_sp_hyouga */
	{ 6, "Running Hyakuretsutou" },	/* pl2f_sp_dash */
	{ 7, "Hyper Hyakuretsutou" },	/* pl2f_sp_hrapid */
	{ 8, "Cho-Shuriken" },	/* pl2f_sp_hshuriken */
	{ 10, "Toki-Hyo (LK" },	/* pl2f_sp_elem_k */
	{ 11, "Toki-Rai (lightning element" },	/* pl2f_sp_elem_p */
	{ 13, "Taunt" },	/* pl2f_sp_chouhatsu */
	{ 14, "Snapback" },	/* pl2f_sp_snapback */
	{ 15, "Crouching HP" },	/* pl2f_sp_crouch_hp */
	{ 16, "Homuraken (hyper" },	/* pl2f_sp_homura */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_30[] = {
	{ 0, "Omega Destroyer" },	/* pl30_sp_destroyer */
	{ 2, "Carbonadium Coil" },	/* pl30_sp_coil */
	{ 3, "Kuuchuu Carbonadium Coil (air" },	/* pl30_sp_jcoil */
	{ 4, "Omega Strike" },	/* pl30_sp_strike */
	{ 6, "Taunt" },	/* pl30_sp_chouhatsu */
	{ 7, "Snapback" },	/* pl30_sp_snapback */
	{ 9, "Burrowing Coil" },	/* pl30_sp_burrow */
	{ 10, "Air Omega Destroyer" },	/* pl30_sp_jdestroyer */
	{ 11, "Variable Counter" },	/* pl30_sp_counter_a */
	{ 12, "Variable Counter" },	/* pl30_sp_counter_g */
	{ 13, "Carbonadium Coil" },	/* pl30_sp_coil2 */
	{ 14, "Kuuchuu Carbonadium Coil" },	/* pl30_sp_jcoil2 */
	{ 15, "Omega Strike" },	/* pl30_sp_strike2 */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_31[] = {
	{ 0, "Dancing Sword" },	/* pl31_sp_sword */
	{ 1, "Tsurugi Tobashi" },	/* pl31_sp_tobashi */
	{ 2, "Teleport Dance" },	/* pl31_sp_tele */
	{ 3, "Six-hand Grapple" },	/* pl31_sp_grapple */
	{ 4, "Metamorphose" },	/* pl31_sp_metamor */
	{ 5, "Snapback" },	/* pl31_sp_snapback */
	{ 6, "Taunt" },	/* pl31_sp_chouhatsu */
	{ 7, "Dancing Sword" },	/* pl31_sp_swords */
	{ 8, "Standing HP" },	/* pl31_sp_stand_hp */
	{ 9, "Crouching HP" },	/* pl31_sp_crouch_hp */
	{ 10, "Power Dance" },	/* pl31_sp_power */
	{ 11, "Speed Dance" },	/* pl31_sp_speed */
	{ 12, "Hyper Metamorphose" },	/* pl31_sp_hmetamor */
	{ 13, "Stampede Sword (hyper" },	/* pl31_sp_stampede */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_32[] = {
	{ 0, "Power Tackle" },	/* pl32_sp_tackle */
	{ 1, "Giant Swing" },	/* pl32_sp_swing */
	{ 2, "Super Armor" },	/* pl32_sp_armor */
	{ 3, "Super Dive" },	/* pl32_sp_dive */
	{ 4, "Taunt" },	/* pl32_sp_chouhatsu */
	{ 5, "Snapback" },	/* pl32_sp_snapback */
	{ 6, "Standing HK" },	/* pl32_sp_stand_hk */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_33[] = {
	{ 0, "Repulsor Blast" },	/* pl33_sp_repulsor */
	{ 1, "Proton Cannon" },	/* pl33_sp_proton */
	{ 2, "Unibeam" },	/* pl33_sp_unibeam */
	{ 3, "Smart Bomb" },	/* pl33_sp_bomb */
	{ 4, "Taunt" },	/* pl33_sp_chouhatsu */
	{ 5, "Flight" },	/* pl33_sp_fly */
	{ 8, "Snapback" },	/* pl33_sp_snapback */
	{ 9, "Standing HK" },	/* pl33_sp_stand_hk */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_34[] = {
	{ 0, "Rocket Punch" },	/* pl34_sp_rocket */
	{ 1, "Plasma Storm" },	/* pl34_sp_plasma */
	{ 2, "Sentinel Force" },	/* pl34_sp_force */
	{ 3, "Snapback" },	/* pl34_sp_snapback */
	{ 4, "Flight" },	/* pl34_sp_fly */
	{ 5, "Standing HK" },	/* pl34_sp_stand_hk */
	{ 6, "Hyper Sentinel Force" },	/* pl34_sp_hforce */
	{ 7, "Taunt" },	/* pl34_sp_chouhatsu */
	{ 8, "Hard Drive" },	/* pl34_sp_hdrive */
	{ 9, "Rocket Punch" },	/* pl34_sp_rocket2 */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_35[] = {
	{ 0, "Heart of Darkness" },	/* pl35_sp_heart */
	{ 1, "Armageddon" },	/* pl35_sp_armageddon */
	{ 2, "Dark Thunder" },	/* pl35_sp_thunder */
	{ 3, "Inferno" },	/* pl35_sp_inferno */
	{ 5, "Taunt" },	/* pl35_sp_chouhatsu */
	{ 8, "Judgement Day" },	/* pl35_sp_judgement */
	{ 9, "Snapback" },	/* pl35_sp_snapback */
	{ 10, "Launcher" },	/* pl35_sp_gamma */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_36[] = {
	{ 0, "Titan Crush" },	/* pl36_sp_titan */
	{ 1, "Death Sphere" },	/* pl36_sp_sphere */
	{ 2, "Gauntlet Reality" },	/* pl36_sp_reality */
	{ 3, "Gauntlet Space" },	/* pl36_sp_space */
	{ 4, "Power Gem" },	/* pl36_sp_power */
	{ 6, "Soul Gem" },	/* pl36_sp_soul */
	{ 8, "Taunt" },	/* pl36_sp_chouhatsu */
	{ 9, "Uppercut" },	/* pl36_sp_upper */
	{ 10, "Snapback" },	/* pl36_sp_snapback */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_37[] = {
	{ 0, "Saotome Typhoon" },	/* pl37_sp_typhoon */
	{ 1, "Saotome Dynamite" },	/* pl37_sp_dynamite */
	{ 2, "Blodia Vulcan" },	/* pl37_sp_vulcan */
	{ 3, "Blodia Punch" },	/* pl37_sp_blodia */
	{ 4, "Saotome Cyclone" },	/* pl37_sp_cyclone */
	{ 5, "Saotome Crash" },	/* pl37_sp_crush */
	{ 7, "Saotome Shine" },	/* pl37_sp_shine */
	{ 8, "Taunt" },	/* pl37_sp_chouhatsu */
	{ 9, "Snapback" },	/* pl37_sp_snapback */
	{ 10, "Standing MP" },	/* pl37_sp_punch */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_38[] = {
	{ 0, "Captain Sword" },	/* pl38_sp_sword */
	{ 1, "Captain Fire" },	/* pl38_sp_fire */
	{ 2, "Captain Corridor" },	/* pl38_sp_corridor */
	{ 4, "Air Captain Fire" },	/* pl38_sp_jfire */
	{ 6, "Captain Storm" },	/* pl38_sp_storm */
	{ 7, "Taunt" },	/* pl38_sp_chouhatsu */
	{ 8, "Commando Strike" },	/* pl38_sp_strike */
	{ 12, "Captain Kick" },	/* pl38_sp_kick */
	{ 13, "Snapback" },	/* pl38_sp_snapback */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_39[] = {
	{ 0, "Tornado Claw" },	/* pl39_sp_tornado */
	{ 1, "Drill Claw" },	/* pl39_sp_drill */
	{ 2, "Berserker Barrage" },	/* pl39_sp_berserker */
	{ 3, "Berserker Slash" },	/* pl39_sp_slash */
	{ 4, "Weapon X" },	/* pl39_sp_weapon_x */
	{ 5, "Berserker Barrage X" },	/* pl39_sp_berserker_x */
	{ 6, "Fatal Claw" },	/* pl39_sp_fatal */
	{ 7, "Berserker Charge" },	/* pl39_sp_charge */
	{ 8, "Snapback" },	/* pl39_sp_snapback */
	{ 9, "Taunt" },	/* pl39_sp_chouhatsu */
	{ 10, "Standing HP" },	/* pl39_sp_stand_hp */
	{ 11, "Standing HK" },	/* pl39_sp_stand_hk */
	{ 12, "Sliding Claw" },	/* pl39_sp_slide */
	{ -1, 0 }
} ;
static const SEL_MOVE_NAME sel_move_name_3a[] = {
	{ 0, "Kobun Fire" },	/* pl3a_sp_fire */
	{ 1, "Engo Kougeki" },	/* pl3a_sp_engo */
	{ 2, "Snapback" },	/* pl3a_sp_snapback */
	{ 3, "Lunch Rush" },	/* pl3a_sp_lunch */
	{ 4, "Taunt" },	/* pl3a_sp_chouhatsu */
	{ 5, "King Kobun" },	/* pl3a_sp_king */
	{ 6, "Rushing King Kobun (team hyper" },	/* pl3a_sp_rking */
	{ 7, "Engo Kougeki" },	/* pl3a_sp_engo_k */
	{ 8, "Paper Planes" },	/* pl3a_sp_plane */
	{ 9, "Bull Mover" },	/* pl3a_sp_bull */
	{ 10, "A dash through the opponent" },	/* pl3a_sp_tackle */
	{ -1, 0 }
} ;

static const SEL_MOVE_NAME *const sel_move_name_tbl[59] = {
	sel_move_name_00, sel_move_name_01, sel_move_name_02, sel_move_name_03, sel_move_name_04, sel_move_name_05,
	sel_move_name_06, sel_move_name_07, sel_move_name_08, sel_move_name_09, sel_move_name_0a, sel_move_name_0b,
	sel_move_name_0c, sel_move_name_0d, sel_move_name_0e, sel_move_name_0f, sel_move_name_10, sel_move_name_11,
	sel_move_name_12, sel_move_name_13, sel_move_name_14, sel_move_name_15, sel_move_name_16, sel_move_name_17,
	sel_move_name_18, sel_move_name_19, sel_move_name_1a, sel_move_name_1b, sel_move_name_1c, sel_move_name_1d,
	sel_move_name_1e, sel_move_name_1f, sel_move_name_20, sel_move_name_21, sel_move_name_22, sel_move_name_23,
	sel_move_name_24, sel_move_name_25, sel_move_name_26, sel_move_name_27, sel_move_name_28, sel_move_name_29,
	sel_move_name_2a, sel_move_name_2b, sel_move_name_2c, sel_move_name_2d, sel_move_name_2e, sel_move_name_2f,
	sel_move_name_30, sel_move_name_31, sel_move_name_32, sel_move_name_33, sel_move_name_34, sel_move_name_35,
	sel_move_name_36, sel_move_name_37, sel_move_name_38, sel_move_name_39, sel_move_name_3a,
} ;

/****************************************************************/
/*			The name of character type's move sp, or 0			*/
/****************************************************************/
const char *sel_move_name( Sint32 type, Sint32 sp )
{
	const SEL_MOVE_NAME *n ;

	if( type < 0 || type >= 59 ) return 0 ;
	for( n = sel_move_name_tbl[type] ; n->sp >= 0 ; n ++ ){
		if( n->sp == sp ) return n->name ;
	}
	return 0 ;
}

/*==============================================================*/
/*	End of File.												*/
/*==============================================================*/
/* "sel_move_name.c" End of File	*/
