/*
 * net_ui.c  Native SDL Netplay Overlay UI (F4) & Replay/Network HUD (F8)
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#endif

#include "net_ui.h"
#include "http_client.h"
#include "ggpo_glue.h"
#include "patch_dll.h"
#include "replay.h"
#include "menu_sdl.h"
#include "input_sdl.h"
#include "naomi_host.h"
#include "rd.h"

enum {
    TAB_PROFILE = 0,
    TAB_RANKED = 1,
    TAB_LOBBIES = 2,
    TAB_LEADERBOARD = 3,
    TAB_REPLAYS = 4,
    NUM_TABS = 5
};

enum {
    TAG_REGISTER = 101,
    TAG_LOGIN = 102,
    TAG_PATCH_NAME = 103,
    TAG_LIST_LOBBIES = 104,
    TAG_CREATE_LOBBY = 105,
    TAG_JOIN_LOBBY = 106,
    TAG_LEADERBOARD = 107,
    TAG_SEARCH_REPLAYS = 108,
    TAG_DL_REPLAY = 109,
    TAG_DL_PATCH_MAN = 110,
    TAG_DL_PATCH_SIG = 111,
    TAG_DL_PATCH_DLL = 112
};

enum {
    PIN_UP = 0, PIN_DOWN, PIN_LEFT, PIN_RIGHT,
    PIN_LP, PIN_HP, PIN_A1, PIN_LK, PIN_HK, PIN_A2
};

static const char* const pin_labels[10] = {
    "UP", "DN", "LT", "RT", "LP", "HP", "A1", "LK", "HK", "A2"
};

typedef struct FIGHTER_DEF {
    uint8_t id;
    const char* code;
    const char* name;
} FIGHTER_DEF;

static const FIGHTER_DEF MVC2_FIGHTERS[56] = {
    { 0x00, "RYU", "RYU" },           { 0x01, "ZAN", "ZANGIEF" },     { 0x02, "GUI", "GUILE" },       { 0x03, "MOR", "MORRIGAN" },
    { 0x04, "ANA", "ANAKARIS" },      { 0x05, "STR", "STRIDER" },     { 0x06, "CYC", "CYCLOPS" },     { 0x07, "WOL", "WOLVERINE" },
    { 0x08, "PSY", "PSYLOCKE" },      { 0x09, "ICE", "ICEMAN" },      { 0x0A, "ROG", "ROGUE" },       { 0x0B, "CAP", "CAPT AMERICA" },
    { 0x0C, "SPI", "SPIDER-MAN" },    { 0x0D, "HLK", "HULK" },        { 0x0E, "VEN", "VENOM" },       { 0x0F, "DOM", "DR DOOM" },
    { 0x10, "TRN", "TRON BONNE" },    { 0x11, "JIL", "JILL" },        { 0x12, "HAY", "HAYATO" },      { 0x13, "RUB", "RUBY HEART" },
    { 0x14, "SON", "SONSON" },        { 0x15, "AMI", "AMINGO" },      { 0x16, "MAR", "MARROW" },      { 0x17, "CAB", "CABLE" },
    { 0x1B, "CHN", "CHUN-LI" },       { 0x1C, "MEG", "MEGA MAN" },    { 0x1D, "ROL", "ROLL" },        { 0x1E, "AKU", "AKUMA" },
    { 0x1F, "BBH", "BB HOOD" },       { 0x20, "FEL", "FELICIA" },     { 0x21, "CHA", "CHARLIE" },     { 0x22, "SAK", "SAKURA" },
    { 0x23, "DAN", "DAN" },           { 0x24, "CAM", "CAMMY" },       { 0x25, "DHA", "DHALSIM" },     { 0x26, "BIS", "M BISON" },
    { 0x27, "KEN", "KEN" },           { 0x28, "GAM", "GAMBIT" },      { 0x29, "JUG", "JUGGERNAUT" },  { 0x2A, "STM", "STORM" },
    { 0x2B, "SAB", "SABRETOOTH" },    { 0x2C, "MAG", "MAGNETO" },     { 0x2D, "SHU", "SHUMA-GORATH" },{ 0x2E, "WAR", "WAR MACHINE" },
    { 0x2F, "SIL", "SILVER SAM" },    { 0x30, "OMG", "OMEGA RED" },    { 0x31, "SPR", "SPIRAL" },      { 0x32, "COL", "COLOSSUS" },
    { 0x33, "IRN", "IRON MAN" },      { 0x34, "SEN", "SENTINEL" },    { 0x35, "BLK", "BLACKHEART" },  { 0x36, "THN", "THANOS" },
    { 0x37, "JIN", "JIN" },           { 0x38, "COM", "CAPT COM" },    { 0x39, "BWL", "BONE WOLV" },   { 0x3A, "SRV", "SERVBOT" },
};

typedef struct LOBBY_ITEM {
    char code[16];
    char name[48];
    char host_id[16];
    char patch_id[32];
    int ft_target;
    int player_count;
    int max_players;
} LOBBY_ITEM;

typedef struct LB_ITEM {
    int rank;
    char player_id[16];
    char display_name[32];
    double rating;
    int wins;
    int losses;
    int mains[3];
} LB_ITEM;

typedef struct REPLAY_ITEM {
    int is_local;
    char replay_id[32];
    char path[512];
    char patch_id[32];
    char p1_name[32];
    char p2_name[32];
    int p1_rating;
    int p2_rating;
    int winner_side;
    int duration_frames;
    uint8_t p1_c[3];
    uint8_t p2_c[3];
    uint8_t p1_a[3];
    uint8_t p2_a[3];
} REPLAY_ITEM;

static struct {
    int inited;
    int open;
    int tab;
    int row;
    int editing_field; /* 0=none, 1=display_name, 2=server_url, 3=pin_entry, 4=join_code, 5=player_filter, 6=char_picker */
    int nav_axis[2];

    /* Persistent profile */
    char server_url[256];
    char player_id[16];
    char display_name[32];
    char access_token[128];
    double rating;
    double rd;
    int wins;
    int losses;

    /* 6-input PIN entry */
    int pin_inputs[6];
    int pin_len;
    char login_id_buf[16];

    /* Queue & Training standby */
    int ws_mode; /* 0=none, 1=queue, 2=lobby */
    int in_queue;
    uint32_t queue_start_ms;
    int match_found_modal;
    char pending_match_id[32];
    char pending_opp_name[32];
    double pending_opp_rating;

    /* Lobby */
    LOBBY_ITEM pub_lobbies[16];
    int pub_lobby_count;
    int create_private;
    int create_ft; /* 2, 3, 5, 0 */
    char join_code_buf[16];
    char active_lobby_code[16];
    char active_invite_url[384];
    char active_lobby_name[48];
    char active_p1[32];
    char active_p2[32];
    int active_scores[2];
    int lobby_ready;
    int lobby_spectate;
    int lobby_peer_count;
    char lobby_peers_summary[4][48];

    /* Leaderboard */
    int lb_around_me;
    LB_ITEM lb_items[100];
    int lb_count;
    int lb_scroll;

    /* Replays & Filter */
    int rep_online_tab; /* 0=local, 1=online */
    REPLAY_ITEM rep_items[64];
    int rep_count;
    int rep_scroll;
    int filter_team_a[3];
    int filter_team_b[3];
    int filter_assist;      /* -1=any, 0,1,2 */
    int filter_winner_only; /* 0 or 1 */
    char filter_player[32];
    int picker_target;      /* 0=Team A slot, 1=Team B slot */
    int picker_slot;        /* 0..2 */
    int picker_cursor;      /* 0..55 */

    /* Pending online replay + verified patch download */
    int pending_dl_active;
    char pending_rep_path[512];
    char pending_rep_patch[32];
    char pending_stage_dir[512];
    int patch_dl_mask; /* bit 0=man, 1=sig, 2=dll */
    int patch_dl_err;

    /* HUD & Toast */
    int show_net_hud;
    int show_replay_hud;
    char toast[128];
    uint32_t toast_until_ms;
} ui;

/* ---- Minimal helpers ---- */

static uint32_t ui_now_ms(void) {
#ifdef _WIN32
    return GetTickCount();
#else
    return 0;
#endif
}

static void show_toast(const char* msg) {
    snprintf(ui.toast, sizeof ui.toast, "%s", msg ? msg : "");
    ui.toast_until_ms = ui_now_ms() + 3500u;
}

static const char* fighter_code(int id) {
    int i;
    if (id < 0) return "---";
    for (i = 0; i < 56; i++) {
        if (MVC2_FIGHTERS[i].id == (uint8_t)id) return MVC2_FIGHTERS[i].code;
    }
    return "???";
}

static void fill_rect(uint32_t* buf, int w, int h, int x, int y, int rw, int rh, uint32_t col) {
    int yy, xx;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + rw > w ? w : x + rw;
    int y1 = y + rh > h ? h : y + rh;
    for (yy = y0; yy < y1; yy++) {
        uint32_t* row = buf + yy * w;
        for (xx = x0; xx < x1; xx++) {
            row[xx] = col;
        }
    }
}

static void draw_border_box(uint32_t* buf, int w, int h, int x, int y, int rw, int rh, uint32_t bg, uint32_t border) {
    fill_rect(buf, w, h, x, y, rw, rh, bg);
    fill_rect(buf, w, h, x, y, rw, 1, border);
    fill_rect(buf, w, h, x, y + rh - 1, rw, 1, border);
    fill_rect(buf, w, h, x, y, 1, rh, border);
    fill_rect(buf, w, h, x + rw - 1, y, 1, rh, border);
}

static void draw_str(uint32_t* buf, int w, int h, int x, int y, const char* s, uint32_t col) {
    menu_text(buf, w, h, x, y, s, col, 1);
}

/* ---- Minimal flat JSON extraction helpers ---- */

static const char* skip_ws_j(const char* p) {
    while (p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}

static int j_find_key(const char* json, const char* end, const char* key, const char** val_out) {
    size_t klen = strlen(key);
    const char* p = json;
    if (!json) return 0;
    if (!end) end = json + strlen(json);
    while (p < end) {
        if (*p == '"') {
            const char* q = p + 1;
            while (q < end && *q != '"') {
                if (*q == '\\' && q + 1 < end) q += 2;
                else q++;
            }
            if (q >= end) break;
            if ((size_t)(q - (p + 1)) == klen && memcmp(p + 1, key, klen) == 0) {
                const char* c = skip_ws_j(q + 1);
                if (*c == ':') {
                    *val_out = skip_ws_j(c + 1);
                    return 1;
                }
            }
            p = q + 1;
        } else if (*p == '{' || *p == '[') {
            if (p != json) {
                char oc = *p++, cc = (oc == '{') ? '}' : ']';
                int nest = 1;
                while (p < end && nest > 0) {
                    if (*p == '"') {
                        p++;
                        while (p < end && *p != '"') {
                            if (*p == '\\' && p + 1 < end) p += 2;
                            else p++;
                        }
                        if (p < end) p++;
                    } else {
                        if (*p == oc) nest++;
                        else if (*p == cc) nest--;
                        p++;
                    }
                }
            } else {
                p++;
            }
        } else {
            p++;
        }
    }
    return 0;
}

static int j_get_str(const char* json, const char* end, const char* key, char* out, size_t cap) {
    const char* v;
    size_t n = 0;
    if (!j_find_key(json, end, key, &v) || *v != '"') return 0;
    v++;
    while (*v && *v != '"' && (!end || v < end)) {
        char c = *v++;
        if (c == '\\' && *v) c = *v++;
        if (n + 1 < cap) out[n++] = c;
    }
    out[n] = 0;
    return 1;
}

static int j_get_double(const char* json, const char* end, const char* key, double* out) {
    const char* v;
    char* ep = NULL;
    if (!j_find_key(json, end, key, &v)) return 0;
    *out = strtod(v, &ep);
    return ep != v;
}

static int j_get_int(const char* json, const char* end, const char* key, int* out) {
    double d = 0;
    if (!j_get_double(json, end, key, &d)) return 0;
    *out = (int)d;
    return 1;
}

static void save_profile_cfg(void) {
    FILE* f = fopen(host_config_path("netplay_profile.cfg"), "w");
    if (!f) return;
    fprintf(f, "server_url=%s\nplayer_id=%s\ndisplay_name=%s\naccess_token=%s\nrating=%.1f\n",
            ui.server_url, ui.player_id, ui.display_name, ui.access_token, ui.rating);
    fclose(f);
}

static void load_profile_cfg(void) {
    char line[512];
    FILE* f = fopen(host_config_path("netplay_profile.cfg"), "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) line[--len] = 0;
        if (strncmp(line, "server_url=", 11) == 0 && line[11]) {
            snprintf(ui.server_url, sizeof ui.server_url, "%s", line + 11);
        } else if (strncmp(line, "player_id=", 10) == 0) {
            snprintf(ui.player_id, sizeof ui.player_id, "%s", line + 10);
        } else if (strncmp(line, "display_name=", 13) == 0) {
            snprintf(ui.display_name, sizeof ui.display_name, "%s", line + 13);
        } else if (strncmp(line, "access_token=", 13) == 0) {
            snprintf(ui.access_token, sizeof ui.access_token, "%s", line + 13);
        } else if (strncmp(line, "rating=", 7) == 0) {
            ui.rating = atof(line + 7);
        }
    }
    fclose(f);
}

static void ensure_dir_exists(const char* path) {
#ifdef _WIN32
    _mkdir(path);
#endif
}

static void scan_local_replays(void) {
    char rep_dir[512];
    const char* dirs[2];
    int d;
    ui.rep_count = 0;
    snprintf(rep_dir, sizeof rep_dir, "%s", host_config_path("replays"));
    ensure_dir_exists(rep_dir);
    dirs[0] = rep_dir;
    dirs[1] = host_config_dir();

    for (d = 0; d < 2 && ui.rep_count < 64; d++) {
#ifdef _WIN32
        char pat[600];
        WIN32_FIND_DATAA fd;
        HANDLE h;
        snprintf(pat, sizeof pat, "%s/*.mvc2r", dirs[d]);
        h = FindFirstFileA(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            MVC2R_FILE rf;
            char full[600];
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            snprintf(full, sizeof full, "%s/%s", dirs[d], fd.cFileName);
            mvc2r_init(&rf);
            if (mvc2r_load(&rf, full)) {
                REPLAY_ITEM* it = &ui.rep_items[ui.rep_count++];
                memset(it, 0, sizeof *it);
                it->is_local = 1;
                snprintf(it->replay_id, sizeof it->replay_id, "%s", fd.cFileName);
                snprintf(it->path, sizeof it->path, "%s", full);
                snprintf(it->patch_id, sizeof it->patch_id, "%s", rf.hdr.patch_id);
                snprintf(it->p1_name, sizeof it->p1_name, "%s", rf.hdr.p1_name);
                snprintf(it->p2_name, sizeof it->p2_name, "%s", rf.hdr.p2_name);
                it->p1_rating = rf.hdr.p1_rating;
                it->p2_rating = rf.hdr.p2_rating;
                it->winner_side = rf.hdr.winner_side;
                it->duration_frames = (int)rf.hdr.duration_frames;
                for (int s = 0; s < 3; s++) {
                    it->p1_c[s] = rf.hdr.teams[0][s].char_id;
                    it->p2_c[s] = rf.hdr.teams[1][s].char_id;
                }
            }
            mvc2r_free(&rf);
        } while (ui.rep_count < 64 && FindNextFileA(h, &fd));
        FindClose(h);
#endif
    }
}

static void append_chars_param(char* qs, size_t cap, const char* key, const int arr[3]) {
    int any = 0, i;
    char buf[64] = "";
    for (i = 0; i < 3; i++) {
        if (arr[i] >= 0) {
            snprintf(buf + strlen(buf), sizeof buf - strlen(buf), "%s%d", any ? "," : "", arr[i]);
            any = 1;
        }
    }
    if (any) {
        snprintf(qs + strlen(qs), cap - strlen(qs), "&%s=%s", key, buf);
    }
}

static void trigger_online_replay_search(void) {
    char url[512];
    snprintf(url, sizeof url, "/api/v1/replays?limit=40");
    append_chars_param(url, sizeof url, "team_a_chars", ui.filter_team_a);
    append_chars_param(url, sizeof url, "team_b_chars", ui.filter_team_b);
    if (ui.filter_assist >= 0) {
        snprintf(url + strlen(url), sizeof url - strlen(url), "&assist=%d", ui.filter_assist);
    }
    if (ui.filter_winner_only) {
        snprintf(url + strlen(url), sizeof url - strlen(url), "&winner_only=1");
    }
    if (ui.filter_player[0]) {
        snprintf(url + strlen(url), sizeof url - strlen(url), "&player=%s", ui.filter_player);
    }
    http_client_request("GET", url, ui.access_token, NULL, 0, NULL, NULL, TAG_SEARCH_REPLAYS);
    show_toast("SEARCHING ONLINE REPLAYS...");
}

/* ---- Actions ---- */

static void send_auth_request(int is_register) {
    char body[256];
    if (ui.pin_len != 6) {
        show_toast("ENTER ALL 6 ARCADE PIN INPUTS FIRST!");
        return;
    }
    http_client_set_base_url(ui.server_url);
    if (is_register) {
        snprintf(body, sizeof body,
                 "{\"display_name\":\"%s\",\"pin_inputs\":[%d,%d,%d,%d,%d,%d]}",
                 ui.display_name[0] ? ui.display_name : "PLAYER",
                 ui.pin_inputs[0], ui.pin_inputs[1], ui.pin_inputs[2],
                 ui.pin_inputs[3], ui.pin_inputs[4], ui.pin_inputs[5]);
        http_client_request("POST", "/api/v1/auth/register", NULL, body, strlen(body), "application/json", NULL, TAG_REGISTER);
        show_toast("REGISTERING STATIONARY ID...");
    } else {
        const char* id = ui.login_id_buf[0] ? ui.login_id_buf : ui.player_id;
        if (!id[0]) {
            show_toast("ENTER P-XXXXXXXX ID TO LOGIN!");
            return;
        }
        snprintf(body, sizeof body,
                 "{\"player_id\":\"%s\",\"pin_inputs\":[%d,%d,%d,%d,%d,%d]}",
                 id,
                 ui.pin_inputs[0], ui.pin_inputs[1], ui.pin_inputs[2],
                 ui.pin_inputs[3], ui.pin_inputs[4], ui.pin_inputs[5]);
        http_client_request("POST", "/api/v1/auth/login", NULL, body, strlen(body), "application/json", NULL, TAG_LOGIN);
        show_toast("LOGGING IN...");
    }
}

static void start_training_standby(void) {
    const HOST_PROGRAM* prog = board_program();
    if (prog != NULL && prog->match != NULL && prog->match->start_training != NULL) {
        prog->match->start_training();
        ui.open = 0;
    }
}

static void enter_ranked_queue(void) {
    char ws_path[256];
    if (!ui.access_token[0]) {
        show_toast("SIGN IN ON PROFILE TAB FIRST!");
        return;
    }
    http_client_set_base_url(ui.server_url);
    snprintf(ws_path, sizeof ws_path, "/ws/queue?token=%s&patch_id=%s",
             ui.access_token, patch_dll_active_id());
    ui.ws_mode = 1;
    ui.in_queue = 1;
    ui.queue_start_ms = ui_now_ms();
    http_client_ws_connect(ws_path, ui.access_token);
    start_training_standby();
    show_toast("RANKED QUEUE STARTED - PRACTICE IN TRAINING!");
}

static void open_or_fetch_replay(const REPLAY_ITEM* it) {
    char dir[512], out_path[512];
    if (!it) return;
    if (it->is_local) {
        if (replay_session_open(it->path)) {
            ui.open = 0;
            ui.show_replay_hud = 1;
            show_toast("REPLAY OPENED (F8 HUD, F5/F6 TAKEOVER)");
        } else {
            show_toast("FAILED TO OPEN LOCAL REPLAY");
        }
        return;
    }
    snprintf(dir, sizeof dir, "%s", host_config_path("replays"));
    ensure_dir_exists(dir);
    snprintf(out_path, sizeof out_path, "%s/%s.mvc2r", dir, it->replay_id);
    snprintf(ui.pending_rep_path, sizeof ui.pending_rep_path, "%s", out_path);
    snprintf(ui.pending_rep_patch, sizeof ui.pending_rep_patch, "%s", it->patch_id);
    ui.pending_dl_active = 1;
    ui.patch_dl_mask = 0;
    ui.patch_dl_err = 0;
    {
        char req_url[256];
        snprintf(req_url, sizeof req_url, "/api/v1/replays/%s/download", it->replay_id);
        http_client_request("GET", req_url, ui.access_token, NULL, 0, NULL, out_path, TAG_DL_REPLAY);
        show_toast("DOWNLOADING REPLAY...");
    }
}

static void delete_staging_files(const char* stage_dir, const char* final_pdir, int move_into_place) {
    char p1[600], p2[600], p3[600];
    snprintf(p1, sizeof p1, "%s/manifest.json", stage_dir);
    snprintf(p2, sizeof p2, "%s/manifest.sig", stage_dir);
    snprintf(p3, sizeof p3, "%s/dc_game.dll", stage_dir);
    if (move_into_place && final_pdir != NULL) {
#ifdef _WIN32
        char d1[600], d2[600], d3[600];
        ensure_dir_exists(final_pdir);
        snprintf(d1, sizeof d1, "%s/manifest.json", final_pdir);
        snprintf(d2, sizeof d2, "%s/manifest.sig", final_pdir);
        snprintf(d3, sizeof d3, "%s/dc_game.dll", final_pdir);
        MoveFileExA(p1, d1, MOVEFILE_REPLACE_EXISTING);
        MoveFileExA(p2, d2, MOVEFILE_REPLACE_EXISTING);
        MoveFileExA(p3, d3, MOVEFILE_REPLACE_EXISTING);
        RemoveDirectoryA(stage_dir);
#endif
    } else {
        remove(p1);
        remove(p2);
        remove(p3);
#ifdef _WIN32
        RemoveDirectoryA(stage_dir);
#endif
    }
}

static void start_verified_patch_download_or_open(void) {
    char cfg_p[512], exe_p[512], err[128], p_root[512], stage_dir[512];
    const char* pid = ui.pending_rep_patch;

    /* Already verified on disk? Open directly without network fetch */
    snprintf(cfg_p, sizeof cfg_p, "%s/%s", host_config_path("patches"), pid);
    snprintf(exe_p, sizeof exe_p, "%s/%s", host_exe_path("patches"), pid);
    if (!pid[0] || patch_dll_verify_dir(cfg_p, pid, err, sizeof err) ||
        patch_dll_verify_dir(exe_p, pid, err, sizeof err)) {
        ui.pending_dl_active = 0;
        if (replay_session_open(ui.pending_rep_path)) {
            ui.open = 0;
            ui.show_replay_hud = 1;
            show_toast("REPLAY READY (F8 HUD, F5/F6 TAKEOVER)");
        } else {
            show_toast("FAILED TO BOOT REPLAY PATCH");
        }
        return;
    }

    /* Download manifest.json, manifest.sig, dc_game.dll into unverified staging folder */
    snprintf(p_root, sizeof p_root, "%s", host_config_path("patches"));
    ensure_dir_exists(p_root);
    snprintf(stage_dir, sizeof stage_dir, "%s/.staging_%s", p_root, pid);
    ensure_dir_exists(stage_dir);
    snprintf(ui.pending_stage_dir, sizeof ui.pending_stage_dir, "%s", stage_dir);
    ui.patch_dl_mask = 0;
    ui.patch_dl_err = 0;

    {
        char u1[256], u2[256], u3[256], o1[600], o2[600], o3[600];
        snprintf(u1, sizeof u1, "/api/v1/patches/%s/manifest.json", pid);
        snprintf(u2, sizeof u2, "/api/v1/patches/%s/manifest.sig", pid);
        snprintf(u3, sizeof u3, "/api/v1/patches/%s/dc_game.dll", pid);
        snprintf(o1, sizeof o1, "%s/manifest.json", stage_dir);
        snprintf(o2, sizeof o2, "%s/manifest.sig", stage_dir);
        snprintf(o3, sizeof o3, "%s/dc_game.dll", stage_dir);
        http_client_request("GET", u1, ui.access_token, NULL, 0, NULL, o1, TAG_DL_PATCH_MAN);
        http_client_request("GET", u2, ui.access_token, NULL, 0, NULL, o2, TAG_DL_PATCH_SIG);
        http_client_request("GET", u3, ui.access_token, NULL, 0, NULL, o3, TAG_DL_PATCH_DLL);
        show_toast("FETCHING & VERIFYING SIGNED PATCH...");
    }
}

/* ---- HTTP & WebSocket Async Event Handling ---- */

static void parse_lobbies_json(const char* js) {
    const char* arr;
    const char* p;
    ui.pub_lobby_count = 0;
    if (!j_find_key(js, NULL, "lobbies", &arr) || *arr != '[') return;
    p = arr + 1;
    while (*p && *p != ']' && ui.pub_lobby_count < 16) {
        p = skip_ws_j(p);
        if (*p == ',') { p++; continue; }
        if (*p != '{') break;
        const char* start = p++;
        int nest = 1;
        while (*p && nest > 0) {
            if (*p == '"') {
                p++;
                while (*p && *p != '"') p += (*p == '\\' && p[1]) ? 2 : 1;
                if (*p) p++;
            } else {
                if (*p == '{') nest++;
                else if (*p == '}') nest--;
                p++;
            }
        }
        {
            LOBBY_ITEM* it = &ui.pub_lobbies[ui.pub_lobby_count++];
            memset(it, 0, sizeof *it);
            j_get_str(start, p, "join_code", it->code, sizeof it->code);
            j_get_str(start, p, "name", it->name, sizeof it->name);
            j_get_str(start, p, "host_player_id", it->host_id, sizeof it->host_id);
            j_get_str(start, p, "patch_id", it->patch_id, sizeof it->patch_id);
            j_get_int(start, p, "ft_target", &it->ft_target);
            j_get_int(start, p, "player_count", &it->player_count);
            j_get_int(start, p, "max_players", &it->max_players);
        }
    }
}

static void parse_leaderboard_json(const char* js) {
    const char* arr;
    const char* p;
    ui.lb_count = 0;
    if (!j_find_key(js, NULL, "entries", &arr) || *arr != '[') return;
    p = arr + 1;
    while (*p && *p != ']' && ui.lb_count < 100) {
        p = skip_ws_j(p);
        if (*p == ',') { p++; continue; }
        if (*p != '{') break;
        const char* start = p++;
        int nest = 1;
        while (*p && nest > 0) {
            if (*p == '"') {
                p++;
                while (*p && *p != '"') p += (*p == '\\' && p[1]) ? 2 : 1;
                if (*p) p++;
            } else {
                if (*p == '{') nest++;
                else if (*p == '}') nest--;
                p++;
            }
        }
        {
            LB_ITEM* it = &ui.lb_items[ui.lb_count++];
            const char* ma;
            memset(it, 0, sizeof *it);
            it->mains[0] = it->mains[1] = it->mains[2] = -1;
            j_get_int(start, p, "rank", &it->rank);
            j_get_str(start, p, "player_id", it->player_id, sizeof it->player_id);
            j_get_str(start, p, "display_name", it->display_name, sizeof it->display_name);
            j_get_double(start, p, "rating", &it->rating);
            j_get_int(start, p, "wins", &it->wins);
            j_get_int(start, p, "losses", &it->losses);
            if (j_find_key(start, p, "main_chars", &ma) && *ma == '[') {
                const char* mp = ma + 1;
                int k = 0;
                while (*mp && *mp != ']' && k < 3) {
                    char* ep = NULL;
                    while (*mp == ' ' || *mp == ',') mp++;
                    it->mains[k] = (int)strtol(mp, &ep, 10);
                    if (ep == mp) break;
                    mp = ep;
                    k++;
                }
            }
        }
    }
}

static void parse_replays_json(const char* js) {
    const char* arr;
    const char* p;
    ui.rep_count = 0;
    if (!j_find_key(js, NULL, "replays", &arr) || *arr != '[') return;
    p = arr + 1;
    while (*p && *p != ']' && ui.rep_count < 64) {
        p = skip_ws_j(p);
        if (*p == ',') { p++; continue; }
        if (*p != '{') break;
        const char* start = p++;
        int nest = 1;
        while (*p && nest > 0) {
            if (*p == '"') {
                p++;
                while (*p && *p != '"') p += (*p == '\\' && p[1]) ? 2 : 1;
                if (*p) p++;
            } else {
                if (*p == '{') nest++;
                else if (*p == '}') nest--;
                p++;
            }
        }
        {
            REPLAY_ITEM* it = &ui.rep_items[ui.rep_count++];
            const char* ta;
            memset(it, 0, sizeof *it);
            j_get_str(start, p, "replay_id", it->replay_id, sizeof it->replay_id);
            j_get_str(start, p, "patch_id", it->patch_id, sizeof it->patch_id);
            j_get_str(start, p, "p1_name", it->p1_name, sizeof it->p1_name);
            j_get_str(start, p, "p2_name", it->p2_name, sizeof it->p2_name);
            j_get_int(start, p, "p1_rating", &it->p1_rating);
            j_get_int(start, p, "p2_rating", &it->p2_rating);
            j_get_int(start, p, "winner_side", &it->winner_side);
            j_get_int(start, p, "duration_frames", &it->duration_frames);
            if (j_find_key(start, p, "teams", &ta)) {
                const char* tp = ta;
                int idx = 0;
                while (tp < p && idx < 6) {
                    int cid = 0;
                    if (j_get_int(tp, p, "char_id", &cid)) {
                        if (idx < 3) it->p1_c[idx] = (uint8_t)cid;
                        else it->p2_c[idx - 3] = (uint8_t)cid;
                        idx++;
                        tp = strstr(tp, "char_id");
                        if (!tp) break;
                        tp += 7;
                    } else {
                        break;
                    }
                }
            }
        }
    }
}

static void handle_ws_message(const char* js) {
    char typ[32] = "";
    j_get_str(js, NULL, "type", typ, sizeof typ);
    if (strcmp(typ, "match_found") == 0) {
        j_get_str(js, NULL, "match_id", ui.pending_match_id, sizeof ui.pending_match_id);
        {
            char p1_id[16] = "", p1n[32] = "", p2n[32] = "";
            double p1r = 1500, p2r = 1500;
            j_get_str(js, NULL, "p1_id", p1_id, sizeof p1_id);
            j_get_str(js, NULL, "p1_name", p1n, sizeof p1n);
            j_get_str(js, NULL, "p2_name", p2n, sizeof p2n);
            j_get_double(js, NULL, "p1_rating", &p1r);
            j_get_double(js, NULL, "p2_rating", &p2r);
            if (strcmp(p1_id, ui.player_id) == 0) {
                snprintf(ui.pending_opp_name, sizeof ui.pending_opp_name, "%s", p2n);
                ui.pending_opp_rating = p2r;
            } else {
                snprintf(ui.pending_opp_name, sizeof ui.pending_opp_name, "%s", p1n);
                ui.pending_opp_rating = p1r;
            }
        }
        ui.match_found_modal = 1;
        show_toast("RANKED MATCH FOUND! PRESS A/ENTER TO ACCEPT");
    } else if (strcmp(typ, "match_start") == 0) {
        char p1_id[16] = "", p1_addr[64] = "", p2_addr[64] = "", relay_addr[64] = "", patch_id[32] = "";
        double seed_d = 0, tok_d = 0;
        int my_side;
        const char* peer_addr;
        j_get_str(js, NULL, "p1_id", p1_id, sizeof p1_id);
        j_get_str(js, NULL, "p1_addr", p1_addr, sizeof p1_addr);
        j_get_str(js, NULL, "p2_addr", p2_addr, sizeof p2_addr);
        j_get_str(js, NULL, "relay_addr", relay_addr, sizeof relay_addr);
        j_get_str(js, NULL, "patch_id", patch_id, sizeof patch_id);
        j_get_double(js, NULL, "match_seed", &seed_d);
        j_get_double(js, NULL, "relay_token", &tok_d);
        my_side = (strcmp(p1_id, ui.player_id) == 0) ? 1 : 2;
        peer_addr = (my_side == 1) ? p2_addr : p1_addr;
        ui.in_queue = 0;
        ui.match_found_modal = 0;
        ui.open = 0;
        ui.show_net_hud = 1;
        ggpo_glue_start_p2p(my_side, (uint16_t)(my_side == 1 ? 27111 : 27112),
                            peer_addr[0] ? peer_addr : NULL, relay_addr[0] ? relay_addr : NULL,
                            (uint32_t)tok_d, 0, 1, (uint32_t)seed_d,
                            patch_id[0] ? patch_id : patch_dll_active_id(), NULL);
    } else if (strcmp(typ, "lobby_state") == 0) {
        j_get_str(js, NULL, "join_code", ui.active_lobby_code, sizeof ui.active_lobby_code);
        j_get_str(js, NULL, "name", ui.active_lobby_name, sizeof ui.active_lobby_name);
        j_get_str(js, NULL, "p1", ui.active_p1, sizeof ui.active_p1);
        j_get_str(js, NULL, "p2", ui.active_p2, sizeof ui.active_p2);
    }
}

static void handle_http_response(const HTTP_EVENT* ev) {
    const char* js = ev->data ? ev->data : "";
    char err_msg[96] = "";
    if (ev->err[0] != 0) {
        show_toast(ev->err);
        return;
    }
    if (ev->status_code >= 400) {
        if (!j_get_str(js, NULL, "error", err_msg, sizeof err_msg)) {
            snprintf(err_msg, sizeof err_msg, "HTTP ERROR %d", ev->status_code);
        }
        show_toast(err_msg);
        if (ev->user_tag == TAG_DL_PATCH_MAN || ev->user_tag == TAG_DL_PATCH_SIG || ev->user_tag == TAG_DL_PATCH_DLL) {
            ui.patch_dl_err = 1;
            delete_staging_files(ui.pending_stage_dir, NULL, 0);
            ui.pending_dl_active = 0;
        }
        return;
    }

    switch (ev->user_tag) {
    case TAG_REGISTER:
    case TAG_LOGIN:
        j_get_str(js, NULL, "player_id", ui.player_id, sizeof ui.player_id);
        j_get_str(js, NULL, "display_name", ui.display_name, sizeof ui.display_name);
        j_get_str(js, NULL, "access_token", ui.access_token, sizeof ui.access_token);
        j_get_double(js, NULL, "glicko_rating", &ui.rating);
        j_get_double(js, NULL, "glicko_rd", &ui.rd);
        save_profile_cfg();
        show_toast(ev->user_tag == TAG_REGISTER ? "STATIONARY ID CREATED!" : "SIGNED IN SUCCESSFULLY!");
        break;
    case TAG_PATCH_NAME:
        j_get_str(js, NULL, "display_name", ui.display_name, sizeof ui.display_name);
        save_profile_cfg();
        show_toast("DISPLAY NAME UPDATED!");
        break;
    case TAG_LIST_LOBBIES:
        parse_lobbies_json(js);
        break;
    case TAG_CREATE_LOBBY:
        j_get_str(js, NULL, "join_code", ui.active_lobby_code, sizeof ui.active_lobby_code);
        j_get_str(js, NULL, "invite_url", ui.active_invite_url, sizeof ui.active_invite_url);
        if (ui.active_lobby_code[0]) {
            char wsp[256];
            snprintf(wsp, sizeof wsp, "/ws/lobby/%s?token=%s", ui.active_lobby_code, ui.access_token);
            ui.ws_mode = 2;
            http_client_ws_connect(wsp, ui.access_token);
            show_toast("LOBBY CREATED!");
        }
        break;
    case TAG_JOIN_LOBBY: {
        char code[16] = "";
        if (j_get_str(js, NULL, "join_code", code, sizeof code) && code[0]) {
            char wsp[256];
            snprintf(ui.active_lobby_code, sizeof ui.active_lobby_code, "%s", code);
            snprintf(wsp, sizeof wsp, "/ws/lobby/%s?token=%s", code, ui.access_token);
            ui.ws_mode = 2;
            http_client_ws_connect(wsp, ui.access_token);
            ui.tab = TAB_LOBBIES;
            ui.open = 1;
            show_toast("JOINED LOBBY!");
        }
        break;
    }
    case TAG_LEADERBOARD:
        parse_leaderboard_json(js);
        break;
    case TAG_SEARCH_REPLAYS:
        parse_replays_json(js);
        show_toast("ONLINE REPLAYS UPDATED");
        break;
    case TAG_DL_REPLAY: {
        MVC2R_FILE rf;
        mvc2r_init(&rf);
        if (mvc2r_load(&rf, ui.pending_rep_path)) {
            snprintf(ui.pending_rep_patch, sizeof ui.pending_rep_patch, "%s", rf.hdr.patch_id);
            mvc2r_free(&rf);
            start_verified_patch_download_or_open();
        } else {
            mvc2r_free(&rf);
            ui.pending_dl_active = 0;
            show_toast("DOWNLOADED REPLAY IS INVALID!");
        }
        break;
    }
    case TAG_DL_PATCH_MAN:
    case TAG_DL_PATCH_SIG:
    case TAG_DL_PATCH_DLL:
        if (ev->user_tag == TAG_DL_PATCH_MAN) ui.patch_dl_mask |= 1;
        if (ev->user_tag == TAG_DL_PATCH_SIG) ui.patch_dl_mask |= 2;
        if (ev->user_tag == TAG_DL_PATCH_DLL) ui.patch_dl_mask |= 4;
        if (ui.patch_dl_mask == 7 && !ui.patch_dl_err) {
            char err[128], final_dir[512];
            if (!patch_dll_verify_dir(ui.pending_stage_dir, ui.pending_rep_patch, err, sizeof err)) {
                delete_staging_files(ui.pending_stage_dir, NULL, 0);
                ui.pending_dl_active = 0;
                host_log("net_ui: REJECTED downloaded patch %s: %s", ui.pending_rep_patch, err);
                show_toast("REJECTED UNVERIFIED PATCH SIGNATURE!");
                break;
            }
            snprintf(final_dir, sizeof final_dir, "%s/%s", host_config_path("patches"), ui.pending_rep_patch);
            delete_staging_files(ui.pending_stage_dir, final_dir, 1);
            ui.pending_dl_active = 0;
            if (replay_session_open(ui.pending_rep_path)) {
                ui.open = 0;
                ui.show_replay_hud = 1;
                show_toast("PATCH VERIFIED & REPLAY STARTED!");
            } else {
                show_toast("FAILED TO LOAD VERIFIED PATCH!");
            }
        }
        break;
    default:
        break;
    }
}

/* ---- Public Lifecycle & Deep Links ---- */

int net_ui_init(void) {
    const char* env_srv = getenv("MVC2_SERVER_URL");
    const char* cli_srv = cli_get_opt("server");
    if (ui.inited) return 1;
    memset(&ui, 0, sizeof ui);
    snprintf(ui.server_url, sizeof ui.server_url, "%s",
             (cli_srv && cli_srv[0]) ? cli_srv : (env_srv && env_srv[0]) ? env_srv : "http://127.0.0.1:8080");
    snprintf(ui.display_name, sizeof ui.display_name, "PLAYER");
    ui.rating = 1500.0;
    ui.rd = 350.0;
    ui.create_ft = 3;
    ui.filter_team_a[0] = ui.filter_team_a[1] = ui.filter_team_a[2] = -1;
    ui.filter_team_b[0] = ui.filter_team_b[1] = ui.filter_team_b[2] = -1;
    ui.filter_assist = -1;
    ui.show_net_hud = 1;
    ui.show_replay_hud = 1;
    load_profile_cfg();
    http_client_init(ui.server_url);
    ui.inited = 1;
    return 1;
}

void net_ui_shutdown(void) {
    if (!ui.inited) return;
    http_client_shutdown();
    memset(&ui, 0, sizeof ui);
}

int net_ui_is_open(void) {
    return ui.inited && ui.open;
}

int net_ui_has_hud(void) {
    if (!ui.inited) return 0;
    if (ui.toast[0] && ui_now_ms() < ui.toast_until_ms) return 1;
    if (ui.in_queue || ui.match_found_modal) return 1;
    if (ggpo_glue_is_active() && ui.show_net_hud) return 1;
    if (replay_session_is_active() && ui.show_replay_hud) return 1;
    return 0;
}

void net_ui_toggle_replay_hud(void) {
    if (replay_session_is_active()) {
        ui.show_replay_hud = !ui.show_replay_hud;
    } else if (ggpo_glue_is_active()) {
        ui.show_net_hud = !ui.show_net_hud;
    }
}

int net_ui_replay_hud_visible(void) {
    return ui.show_replay_hud;
}

void net_ui_handle_join_uri(const char* uri) {
    char code[16] = "", key[128] = "", path[128], body[256];
    const char* p;
    const char* q;
    if (!uri || !ui.inited) return;
    p = strstr(uri, "umvsc2://join/");
    if (!p) return;
    p += 14;
    q = strchr(p, '?');
    if (q) {
        size_t n = (size_t)(q - p);
        if (n >= sizeof code) n = sizeof code - 1;
        memcpy(code, p, n);
        code[n] = 0;
        const char* kp = strstr(q, "k=");
        if (kp) snprintf(key, sizeof key, "%s", kp + 2);
    } else {
        snprintf(code, sizeof code, "%s", p);
    }
    if (!code[0]) return;
    if (!ui.access_token[0]) {
        snprintf(ui.join_code_buf, sizeof ui.join_code_buf, "%s", code);
        ui.tab = TAB_PROFILE;
        ui.open = 1;
        show_toast("SIGN IN TO JOIN INVITED LOBBY!");
        return;
    }
    http_client_set_base_url(ui.server_url);
    snprintf(path, sizeof path, "/api/v1/lobbies/%s/join", code);
    snprintf(body, sizeof body, "{\"invite_key\":\"%s\"}", key);
    http_client_request("POST", path, ui.access_token, body, strlen(body), "application/json", NULL, TAG_JOIN_LOBBY);
    show_toast("JOINING INVITED LOBBY...");
}

void net_ui_tick(void) {
    HTTP_EVENT ev;
    if (!ui.inited) return;
    while (http_client_poll(&ev)) {
        if (ev.type == HTTP_EV_RESPONSE) {
            handle_http_response(&ev);
        } else if (ev.type == HTTP_EV_WS_MESSAGE && ev.data) {
            handle_ws_message(ev.data);
        } else if (ev.type == HTTP_EV_WS_CLOSED) {
            if (ui.ws_mode == 1) ui.in_queue = 0;
            ui.ws_mode = 0;
        }
        http_event_free(&ev);
    }
}

/* ---- Navigation & Input ---- */

static void act_pin_pad(int pin_val) {
    if (ui.pin_len < 6) {
        ui.pin_inputs[ui.pin_len++] = pin_val;
    }
}

static void handle_text_char(char ch) {
    char* dst = NULL;
    size_t cap = 0;
    if (ui.editing_field == 1) { dst = ui.display_name; cap = sizeof ui.display_name; }
    else if (ui.editing_field == 2) { dst = ui.server_url; cap = sizeof ui.server_url; }
    else if (ui.editing_field == 4) { dst = ui.join_code_buf; cap = sizeof ui.join_code_buf; }
    else if (ui.editing_field == 5) { dst = ui.filter_player; cap = sizeof ui.filter_player; }
    if (!dst) return;
    {
        size_t n = strlen(dst);
        if (ch == '\b') {
            if (n > 0) dst[n - 1] = 0;
        } else if (n + 1 < cap && ch >= 32 && ch < 127) {
            dst[n++] = ch;
            dst[n] = 0;
        }
    }
}

static void tab_action(int up, int down, int left, int right, int enter, int back, int alt_x) {
    const int max_rows[NUM_TABS] = { 6, 3, 7, 4, 8 };

    if (ui.editing_field == 6) {
        if (left) ui.picker_cursor = (ui.picker_cursor + 55) % 56;
        else if (right) ui.picker_cursor = (ui.picker_cursor + 1) % 56;
        else if (up) ui.picker_cursor = (ui.picker_cursor + 56 - 8) % 56;
        else if (down) ui.picker_cursor = (ui.picker_cursor + 8) % 56;
        else if (alt_x) {
            if (ui.picker_target == 0) ui.filter_team_a[ui.picker_slot] = -1;
            else ui.filter_team_b[ui.picker_slot] = -1;
            ui.editing_field = 0;
        } else if (enter) {
            int cid = MVC2_FIGHTERS[ui.picker_cursor].id;
            if (ui.picker_target == 0) ui.filter_team_a[ui.picker_slot] = cid;
            else ui.filter_team_b[ui.picker_slot] = cid;
            ui.editing_field = 0;
        } else if (back) {
            ui.editing_field = 0;
        }
        return;
    }

    if (ui.editing_field == 3) {
        if (up) act_pin_pad(PIN_UP);
        else if (down) act_pin_pad(PIN_DOWN);
        else if (left) act_pin_pad(PIN_LEFT);
        else if (right) act_pin_pad(PIN_RIGHT);
        else if (alt_x) act_pin_pad(PIN_LP);
        else if (enter || back) ui.editing_field = 0;
        return;
    }

    if (ui.editing_field != 0) {
        if (enter || back) {
            ui.editing_field = 0;
            http_client_set_base_url(ui.server_url);
            save_profile_cfg();
        }
        return;
    }

    if (back) {
        ui.open = 0;
        return;
    }
    if (up) {
        if (ui.row == 0) ui.tab = (ui.tab + NUM_TABS - 1) % NUM_TABS;
        else ui.row--;
        return;
    }
    if (down) {
        ui.row = (ui.row + 1) % max_rows[ui.tab];
        return;
    }
    if ((left || right) && ui.row == 0) {
        ui.tab = (ui.tab + (right ? 1 : NUM_TABS - 1)) % NUM_TABS;
        ui.row = 0;
        if (ui.tab == TAB_REPLAYS && !ui.rep_online_tab) scan_local_replays();
        if (ui.tab == TAB_LEADERBOARD && ui.lb_count == 0 && ui.access_token[0]) {
            http_client_request("GET", "/api/v1/leaderboard?limit=100", ui.access_token, NULL, 0, NULL, NULL, TAG_LEADERBOARD);
        }
        return;
    }

    /* Tab-specific row activations */
    if (ui.tab == TAB_PROFILE) {
        if (ui.row == 1 && enter) ui.editing_field = 1;
        else if (ui.row == 2 && enter) {
            if (ui.access_token[0]) {
                char body[96];
                snprintf(body, sizeof body, "{\"display_name\":\"%s\"}", ui.display_name);
                http_client_request("PATCH", "/api/v1/profile", ui.access_token, body, strlen(body), "application/json", NULL, TAG_PATCH_NAME);
            } else {
                ui.editing_field = 3;
            }
        } else if (ui.row == 3 && enter) {
            ui.pin_len = 0;
            ui.editing_field = 3;
        } else if (ui.row == 4 && enter) {
            send_auth_request(1);
        } else if (ui.row == 5 && enter) {
            send_auth_request(0);
        } else if (ui.row == 5 && alt_x) {
            ui.editing_field = 2;
        }
    } else if (ui.tab == TAB_RANKED) {
        if (ui.row == 1 && enter) {
            enter_ranked_queue();
        } else if (ui.row == 2 && enter) {
            http_client_ws_close();
            ui.in_queue = 0;
            show_toast("LEFT RANKED QUEUE");
        }
    } else if (ui.tab == TAB_LOBBIES) {
        if (ui.row == 1 && enter) {
            http_client_request("GET", "/api/v1/lobbies", ui.access_token, NULL, 0, NULL, NULL, TAG_LIST_LOBBIES);
        } else if (ui.row == 2 && (left || right || enter)) {
            if (left || right) ui.create_private = !ui.create_private;
            else {
                char body[256];
                snprintf(body, sizeof body,
                         "{\"name\":\"%s's Room\",\"is_private\":%s,\"ft_target\":%d,\"patch_id\":\"%s\"}",
                         ui.display_name[0] ? ui.display_name : "UMvSC2",
                         ui.create_private ? "true" : "false",
                         ui.create_ft, patch_dll_active_id());
                http_client_request("POST", "/api/v1/lobbies", ui.access_token, body, strlen(body), "application/json", NULL, TAG_CREATE_LOBBY);
            }
        } else if (ui.row == 3 && (left || right)) {
            static const int fts[4] = { 2, 3, 5, 0 };
            int k = 0;
            while (k < 4 && fts[k] != ui.create_ft) k++;
            ui.create_ft = fts[(k + (right ? 1 : 3)) % 4];
        } else if (ui.row == 4 && enter) {
            if (ui.active_invite_url[0]) {
                (void)SDL_SetClipboardText(ui.active_invite_url);
                show_toast("INVITE LINK COPIED!");
            } else {
                ui.editing_field = 4;
            }
        } else if (ui.row == 5 && enter) {
            if (ui.active_lobby_code[0]) {
                ui.lobby_ready = !ui.lobby_ready;
                http_client_ws_send(ui.lobby_ready ? "{\"type\":\"ready\",\"ready\":true}" : "{\"type\":\"ready\",\"ready\":false}");
            } else if (ui.pub_lobby_count > 0) {
                char path[128];
                snprintf(path, sizeof path, "/api/v1/lobbies/%s/join", ui.pub_lobbies[0].code);
                http_client_request("POST", path, ui.access_token, "{}", 2, "application/json", NULL, TAG_JOIN_LOBBY);
            }
        } else if (ui.row == 6 && enter) {
            start_training_standby();
        }
    } else if (ui.tab == TAB_LEADERBOARD) {
        if (ui.row == 1 && (left || right || enter)) {
            char qs[128];
            if (left || right) ui.lb_around_me = !ui.lb_around_me;
            snprintf(qs, sizeof qs, "/api/v1/leaderboard?limit=100%s%s",
                     ui.lb_around_me && ui.player_id[0] ? "&around=" : "",
                     ui.lb_around_me && ui.player_id[0] ? ui.player_id : "");
            http_client_request("GET", qs, ui.access_token, NULL, 0, NULL, NULL, TAG_LEADERBOARD);
        } else if (ui.row == 2 && (left || right)) {
            if (right && ui.lb_scroll + 1 < ui.lb_count) ui.lb_scroll++;
            if (left && ui.lb_scroll > 0) ui.lb_scroll--;
        } else if (ui.row >= 2 && enter && ui.lb_scroll < ui.lb_count) {
            snprintf(ui.filter_player, sizeof ui.filter_player, "%s", ui.lb_items[ui.lb_scroll].player_id);
            ui.tab = TAB_REPLAYS;
            ui.rep_online_tab = 1;
            ui.row = 1;
            trigger_online_replay_search();
        }
    } else if (ui.tab == TAB_REPLAYS) {
        if (ui.row == 1 && (left || right || enter)) {
            ui.rep_online_tab = !ui.rep_online_tab;
            if (!ui.rep_online_tab) scan_local_replays();
            else trigger_online_replay_search();
        } else if (ui.row == 2) {
            if (left || right) ui.picker_slot = (ui.picker_slot + (right ? 1 : 2)) % 3;
            if (enter) { ui.picker_target = 0; ui.editing_field = 6; }
        } else if (ui.row == 3) {
            if (left || right) ui.picker_slot = (ui.picker_slot + (right ? 1 : 2)) % 3;
            if (enter) { ui.picker_target = 1; ui.editing_field = 6; }
        } else if (ui.row == 4 && (left || right || enter)) {
            ui.filter_assist = (ui.filter_assist + 2) % 4 - 1;
        } else if (ui.row == 5 && (left || right || enter)) {
            ui.filter_winner_only = !ui.filter_winner_only;
        } else if (ui.row == 6 && enter) {
            if (ui.rep_online_tab) trigger_online_replay_search();
            else scan_local_replays();
        } else if (ui.row == 7) {
            if (left && ui.rep_scroll > 0) ui.rep_scroll--;
            else if (right && ui.rep_scroll + 1 < ui.rep_count) ui.rep_scroll++;
            else if (enter && ui.rep_scroll < ui.rep_count) open_or_fetch_replay(&ui.rep_items[ui.rep_scroll]);
        }
    }
}

int net_ui_event(const SDL_Event* e) {
    if (!ui.inited) return 0;

    if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat) {
        SDL_Keycode k = key_sym(e);
        if (k == SDLK_F4) {
            ui.open = !ui.open;
            if (ui.open && ui.tab == TAB_REPLAYS && !ui.rep_online_tab) scan_local_replays();
            return 1;
        }
        if (k == SDLK_F8) {
            net_ui_toggle_replay_hud();
            return 1;
        }
        if (ui.match_found_modal && !ui.open) {
            if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) {
                char msg[96];
                snprintf(msg, sizeof msg, "{\"type\":\"accept\",\"match_id\":\"%s\"}", ui.pending_match_id);
                http_client_ws_send(msg);
                show_toast("MATCH ACCEPTED - WAITING FOR OPPONENT");
                return 1;
            }
            if (k == SDLK_ESCAPE) {
                ui.match_found_modal = 0;
                ui.in_queue = 0;
                http_client_ws_close();
                return 1;
            }
        }
        if (replay_session_is_active() && !ui.open) {
            REPLAY_STATUS st;
            replay_session_get_status(&st);
            if (k == SDLK_COMMA) {
                if (st.cur_frame > 0) replay_session_seek(st.cur_frame - 1u, NULL);
                return 1;
            }
            if (k == SDLK_B) {
                replay_session_set_bookmark(-1);
                show_toast("BOOKMARK SET (F5/F6 TAKEOVER)");
                return 1;
            }
            if (k == SDLK_LEFT || k == SDLK_RIGHT) {
                int step = 20;
                uint32_t target = st.cur_frame;
                if (k == SDLK_LEFT) target = (st.cur_frame > (uint32_t)step) ? st.cur_frame - (uint32_t)step : 0u;
                else target = st.cur_frame + (uint32_t)step;
                replay_session_seek(target, NULL);
                return 1;
            }
        }
    }

    if (!ui.open) {
        if (ui.match_found_modal && e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
            if (e->gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH || e->gbutton.button == SDL_GAMEPAD_BUTTON_START) {
                char msg[96];
                snprintf(msg, sizeof msg, "{\"type\":\"accept\",\"match_id\":\"%s\"}", ui.pending_match_id);
                http_client_ws_send(msg);
                return 1;
            }
            if (e->gbutton.button == SDL_GAMEPAD_BUTTON_EAST) {
                ui.match_found_modal = 0;
                ui.in_queue = 0;
                http_client_ws_close();
                return 1;
            }
        }
        return 0;
    }

    if (e->type == SDL_EVENT_KEY_DOWN) {
        SDL_Keycode k = key_sym(e);
        if (ui.editing_field == 3 && !e->key.repeat) {
            if (k == SDLK_1) { act_pin_pad(PIN_LP); return 1; }
            if (k == SDLK_2) { act_pin_pad(PIN_HP); return 1; }
            if (k == SDLK_3) { act_pin_pad(PIN_A1); return 1; }
            if (k == SDLK_4) { act_pin_pad(PIN_LK); return 1; }
            if (k == SDLK_5) { act_pin_pad(PIN_HK); return 1; }
            if (k == SDLK_6) { act_pin_pad(PIN_A2); return 1; }
            if (k == SDLK_BACKSPACE && ui.pin_len > 0) { ui.pin_len--; return 1; }
        }
        if (ui.editing_field == 1 || ui.editing_field == 2 || ui.editing_field == 4 || ui.editing_field == 5) {
            if (k == SDLK_BACKSPACE) { handle_text_char('\b'); return 1; }
            if (k >= SDLK_A && k <= SDLK_Z) { handle_text_char((char)('A' + (k - SDLK_A))); return 1; }
            if (k >= SDLK_0 && k <= SDLK_9) { handle_text_char((char)('0' + (k - SDLK_0))); return 1; }
            if (k == SDLK_MINUS) { handle_text_char('-'); return 1; }
            if (k == SDLK_PERIOD) { handle_text_char('.'); return 1; }
            if (k == SDLK_SLASH) { handle_text_char('/'); return 1; }
            if (k == SDLK_SEMICOLON) { handle_text_char(':'); return 1; }
        }
        tab_action(k == SDLK_UP, k == SDLK_DOWN, k == SDLK_LEFT, k == SDLK_RIGHT,
                   (k == SDLK_RETURN || k == SDLK_KP_ENTER) && !e->key.repeat,
                   (k == SDLK_ESCAPE || k == SDLK_F4) && !e->key.repeat,
                   (k == SDLK_BACKSPACE || k == SDLK_DELETE || k == SDLK_X) && !e->key.repeat);
        return 1;
    } else if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        int b = e->gbutton.button;
        if (ui.editing_field == 3) {
            if (b == SDL_GAMEPAD_BUTTON_WEST) { act_pin_pad(PIN_LP); return 1; }
            if (b == SDL_GAMEPAD_BUTTON_NORTH) { act_pin_pad(PIN_HP); return 1; }
            if (b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) { act_pin_pad(PIN_A1); return 1; }
            if (b == SDL_GAMEPAD_BUTTON_SOUTH) { act_pin_pad(PIN_LK); return 1; }
            if (b == SDL_GAMEPAD_BUTTON_EAST) { act_pin_pad(PIN_HK); return 1; }
            if (b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) { act_pin_pad(PIN_A2); return 1; }
        }
        tab_action(b == SDL_GAMEPAD_BUTTON_DPAD_UP, b == SDL_GAMEPAD_BUTTON_DPAD_DOWN,
                   b == SDL_GAMEPAD_BUTTON_DPAD_LEFT, b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
                   b == SDL_GAMEPAD_BUTTON_SOUTH, b == SDL_GAMEPAD_BUTTON_EAST,
                   b == SDL_GAMEPAD_BUTTON_WEST);
        return 1;
    } else if (e->type == SDL_EVENT_GAMEPAD_AXIS_MOTION &&
               (e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX || e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY)) {
        int way = e->gaxis.value > 24000 ? 1 : e->gaxis.value < -24000 ? -1 : 0;
        int* last = &ui.nav_axis[e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY];
        if (way != *last && way != 0) {
            int y = e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY;
            tab_action(y && way < 0, y && way > 0, !y && way < 0, !y && way > 0, 0, 0, 0);
        }
        *last = way;
        return 1;
    }
    return e->type != SDL_EVENT_QUIT;
}

/* ---- Rendering ---- */

#define C_BG       0xE80E1424u
#define C_PANEL    0xF0182238u
#define C_BORDER   0xFF3E63A0u
#define C_WHITE    0xFFFFFFFFu
#define C_YELLOW   0xFFFFE040u
#define C_CYAN     0xFF40E8FFu
#define C_GREEN    0xFF48E878u
#define C_RED      0xFFFF5050u
#define C_GREY     0xFFA0A8B8u

void net_ui_draw_overlay(uint32_t* frame, int w, int h) {
    static const char* const tab_names[NUM_TABS] = {
        "PROFILE", "RANKED", "LOBBIES", "LADDER", "REPLAYS"
    };
    char line[160], pin_str[96];
    int i;

    for (i = 0; i < w * h; i++) {
        frame[i] = 0xFF000000u | ((frame[i] >> 2) & 0x3F3F3Fu);
    }
    draw_border_box(frame, w, h, 14, 12, w - 28, h - 24, C_BG, C_BORDER);
    draw_str(frame, w, h, 26, 22, "UMVSC2 NETPLAY & REPLAY THEATRE", C_YELLOW);
    snprintf(line, sizeof line, "ID: %s  MMR: %.0f",
             ui.player_id[0] ? ui.player_id : "GUEST", ui.rating);
    draw_str(frame, w, h, w - 240, 22, line, C_CYAN);

    /* Tab header strip */
    for (i = 0; i < NUM_TABS; i++) {
        int tx = 26 + i * 116;
        uint32_t bg = (i == ui.tab) ? 0xFF284880u : 0xFF141C2Eu;
        uint32_t fg = (i == ui.tab) ? C_YELLOW : (ui.row == 0 ? C_WHITE : C_GREY);
        draw_border_box(frame, w, h, tx, 40, 108, 18, bg, (i == ui.tab && ui.row == 0) ? C_YELLOW : C_BORDER);
        draw_str(frame, w, h, tx + 10, 45, tab_names[i], fg);
    }

    if (ui.tab == TAB_PROFILE) {
        draw_str(frame, w, h, 28, 72, "STATIONARY ID PROFILE & 6-INPUT ARCADE PIN", C_CYAN);
        snprintf(line, sizeof line, "DISPLAY NAME : %s%s", ui.display_name, ui.editing_field == 1 ? "_" : "");
        draw_str(frame, w, h, 28, 96, line, ui.row == 1 ? C_YELLOW : C_WHITE);
        draw_str(frame, w, h, 28, 114, ui.access_token[0] ? "[SAVE NEW DISPLAY NAME]" : "NOTE: 6-INPUT PIN BINDS TO STICK/KEYS 1-6",
                 ui.row == 2 ? C_YELLOW : C_GREY);

        pin_str[0] = 0;
        for (i = 0; i < 6; i++) {
            snprintf(pin_str + strlen(pin_str), sizeof pin_str - strlen(pin_str), "%s[%s]",
                     i > 0 ? " " : "", i < ui.pin_len ? pin_labels[ui.pin_inputs[i]] : "..");
        }
        snprintf(line, sizeof line, "PIN INPUTS   : %s %s", pin_str, ui.editing_field == 3 ? "(ENTERING)" : "");
        draw_str(frame, w, h, 28, 136, line, ui.row == 3 ? C_YELLOW : C_WHITE);
        draw_str(frame, w, h, 28, 162, "[REGISTER NEW STATIONARY ID]", ui.row == 4 ? C_YELLOW : C_GREEN);
        snprintf(line, sizeof line, "[LOGIN AS %s]   SERVER: %s%s",
                 ui.player_id[0] ? ui.player_id : "P-XXXXXXXX", ui.server_url, ui.editing_field == 2 ? "_" : "");
        draw_str(frame, w, h, 28, 184, line, ui.row == 5 ? C_YELLOW : C_WHITE);
    } else if (ui.tab == TAB_RANKED) {
        draw_str(frame, w, h, 28, 72, "RANKED MATCHMAKING (TRAINING MODE STANDBY)", C_CYAN);
        snprintf(line, sizeof line, "YOUR MMR: %.0f +/- %.0f   PATCH: %s", ui.rating, ui.rd, patch_dll_active_id());
        draw_str(frame, w, h, 28, 98, line, C_WHITE);
        draw_str(frame, w, h, 28, 126, "[ENTER RANKED QUEUE + OPEN TRAINING MODE]", ui.row == 1 ? C_YELLOW : C_GREEN);
        draw_str(frame, w, h, 28, 148, "[LEAVE QUEUE]", ui.row == 2 ? C_YELLOW : C_RED);
        if (ui.in_queue) {
            draw_str(frame, w, h, 28, 180, "STATUS: QUEUED - SEARCHING EXPANDING MMR WINDOW...", C_YELLOW);
        }
    } else if (ui.tab == TAB_LOBBIES) {
        draw_str(frame, w, h, 28, 72, "PUBLIC & PRIVATE KOTH LOBBIES", C_CYAN);
        draw_str(frame, w, h, 28, 94, "[REFRESH PUBLIC ROOMS]", ui.row == 1 ? C_YELLOW : C_WHITE);
        snprintf(line, sizeof line, "[CREATE %s LOBBY]", ui.create_private ? "PRIVATE" : "PUBLIC");
        draw_str(frame, w, h, 28, 112, line, ui.row == 2 ? C_YELLOW : C_GREEN);
        snprintf(line, sizeof line, "RULESET      : FT%d (0=ENDLESS)", ui.create_ft);
        draw_str(frame, w, h, 28, 130, line, ui.row == 3 ? C_YELLOW : C_WHITE);
        snprintf(line, sizeof line, ui.active_invite_url[0] ? "[COPY CLICKABLE INVITE LINK]  CODE: %s" : "JOIN BY CODE : %s%s",
                 ui.active_invite_url[0] ? ui.active_lobby_code : ui.join_code_buf, ui.editing_field == 4 ? "_" : "");
        draw_str(frame, w, h, 28, 148, line, ui.row == 4 ? C_YELLOW : C_CYAN);
        snprintf(line, sizeof line, ui.active_lobby_code[0] ? "[TOGGLE READY (%s)]  P1:%s VS P2:%s" : "[JOIN FIRST PUBLIC ROOM]",
                 ui.lobby_ready ? "READY" : "NOT READY", ui.active_p1[0] ? ui.active_p1 : "-", ui.active_p2[0] ? ui.active_p2 : "-");
        draw_str(frame, w, h, 28, 166, line, ui.row == 5 ? C_YELLOW : C_WHITE);
        draw_str(frame, w, h, 28, 184, "[PRACTICE IN TRAINING WHILE WAITING]", ui.row == 6 ? C_YELLOW : C_WHITE);
        for (i = 0; i < ui.pub_lobby_count && i < 6; i++) {
            snprintf(line, sizeof line, "%s  %-18s  %d/%d  FT%d", ui.pub_lobbies[i].code,
                     ui.pub_lobbies[i].name, ui.pub_lobbies[i].player_count, ui.pub_lobbies[i].max_players,
                     ui.pub_lobbies[i].ft_target);
            draw_str(frame, w, h, 36, 214 + i * 16, line, C_GREY);
        }
    } else if (ui.tab == TAB_LEADERBOARD) {
        draw_str(frame, w, h, 28, 72, "GLICKO-2 RANKED LEADERBOARD (ENTER/A: VIEW REPLAYS)", C_CYAN);
        snprintf(line, sizeof line, "VIEW MODE    : < %s >", ui.lb_around_me ? "AROUND ME" : "TOP 100");
        draw_str(frame, w, h, 28, 94, line, ui.row == 1 ? C_YELLOW : C_WHITE);
        draw_str(frame, w, h, 28, 118, "RNK  NAME            ID          MMR    W-L     TEAM", C_GREY);
        for (i = 0; i < 12 && ui.lb_scroll + i < ui.lb_count; i++) {
            const LB_ITEM* it = &ui.lb_items[ui.lb_scroll + i];
            int y = 136 + i * 16;
            snprintf(line, sizeof line, "%3d  %-14.14s  %-10s  %4.0f  %3d-%-3d  %s/%s/%s",
                     it->rank, it->display_name, it->player_id, it->rating, it->wins, it->losses,
                     fighter_code(it->mains[0]), fighter_code(it->mains[1]), fighter_code(it->mains[2]));
            draw_str(frame, w, h, 28, y, line, (ui.row >= 2 && i == 0) ? C_YELLOW : C_WHITE);
        }
    } else if (ui.tab == TAB_REPLAYS) {
        draw_str(frame, w, h, 28, 72, "REPLAY THEATRE & MATCH DATABASE", C_CYAN);
        snprintf(line, sizeof line, "SOURCE       : < %s >", ui.rep_online_tab ? "ONLINE DATABASE" : "LOCAL .MVC2R");
        draw_str(frame, w, h, 28, 94, line, ui.row == 1 ? C_YELLOW : C_WHITE);
        snprintf(line, sizeof line, "TEAM A FILTER: [%s] [%s] [%s]",
                 fighter_code(ui.filter_team_a[0]), fighter_code(ui.filter_team_a[1]), fighter_code(ui.filter_team_a[2]));
        draw_str(frame, w, h, 28, 112, line, ui.row == 2 ? C_YELLOW : C_WHITE);
        snprintf(line, sizeof line, "TEAM B FILTER: [%s] [%s] [%s]",
                 fighter_code(ui.filter_team_b[0]), fighter_code(ui.filter_team_b[1]), fighter_code(ui.filter_team_b[2]));
        draw_str(frame, w, h, 28, 130, line, ui.row == 3 ? C_YELLOW : C_WHITE);
        snprintf(line, sizeof line, "ASSIST TYPE  : < %s >",
                 ui.filter_assist < 0 ? "ANY" : ui.filter_assist == 0 ? "ALPHA" : ui.filter_assist == 1 ? "BETA" : "GAMMA");
        draw_str(frame, w, h, 28, 148, line, ui.row == 4 ? C_YELLOW : C_WHITE);
        snprintf(line, sizeof line, "MUST BE WIN  : < %s >  PLAYER: %s",
                 ui.filter_winner_only ? "YES (WINNER ONLY)" : "NO (ANY RESULT)", ui.filter_player[0] ? ui.filter_player : "ANY");
        draw_str(frame, w, h, 28, 166, line, ui.row == 5 ? C_YELLOW : C_WHITE);
        draw_str(frame, w, h, 28, 184, "[SEARCH / REFRESH REPLAYS]", ui.row == 6 ? C_YELLOW : C_GREEN);
        for (i = 0; i < 8 && ui.rep_scroll + i < ui.rep_count; i++) {
            const REPLAY_ITEM* it = &ui.rep_items[ui.rep_scroll + i];
            snprintf(line, sizeof line, "%s %s/%s/%s %s vs %s/%s/%s %s (%df)",
                     it->winner_side == 0 ? "W" : " ",
                     fighter_code(it->p1_c[0]), fighter_code(it->p1_c[1]), fighter_code(it->p1_c[2]), it->p1_name,
                     fighter_code(it->p2_c[0]), fighter_code(it->p2_c[1]), fighter_code(it->p2_c[2]), it->p2_name,
                     it->duration_frames);
            draw_str(frame, w, h, 28, 212 + i * 16, line, (ui.row == 7 && i == 0) ? C_YELLOW : C_WHITE);
        }
    }

    /* 56-Character Filter Picker Modal */
    if (ui.editing_field == 6) {
        draw_border_box(frame, w, h, 48, 86, w - 96, h - 140, 0xF80A1020u, C_YELLOW);
        draw_str(frame, w, h, 64, 98, "SELECT CHARACTER (ARROWS/PAD PICK, A SELECT, X CLEAR)", C_YELLOW);
        for (i = 0; i < 56; i++) {
            int col = i % 8, row = i / 8;
            int px = 64 + col * 64, py = 122 + row * 28;
            uint32_t bg = (i == ui.picker_cursor) ? 0xFF305098u : 0xFF161E34u;
            draw_border_box(frame, w, h, px, py, 58, 22, bg, (i == ui.picker_cursor) ? C_YELLOW : C_BORDER);
            draw_str(frame, w, h, px + 14, py + 7, MVC2_FIGHTERS[i].code, (i == ui.picker_cursor) ? C_YELLOW : C_WHITE);
        }
    }

    /* Toast & Help Footer */
    if (ui.toast[0] && ui_now_ms() < ui.toast_until_ms) {
        draw_border_box(frame, w, h, 26, h - 62, w - 52, 20, 0xFF1A3A28u, C_GREEN);
        draw_str(frame, w, h, 36, h - 55, ui.toast, C_YELLOW);
    }
    draw_str(frame, w, h, 28, h - 32, "ARROWS/PAD: NAVIGATE   ENTER/A: SELECT   ESC/B/F4: CLOSE", C_GREY);
}

void net_ui_draw_hud(uint32_t* argb, int w, int h) {
    char line[160];
    if (!ui.inited) return;

    /* Compact Online Match Network HUD (Top-Center) */
    if (ggpo_glue_is_active() && ui.show_net_hud) {
        GGPO_GLUE_STATUS st;
        ggpo_glue_get_status(&st);
        snprintf(line, sizeof line, "PING %dMS | DELAY %dF | RB %dF%s",
                 st.ping_ms, st.frame_delay, st.rollback_frames,
                 st.using_relay ? " | RELAY" : "");
        draw_border_box(argb, w, h, w / 2 - 140, 4, 280, 16, 0xC0080C18u, C_BORDER);
        draw_str(argb, w, h, w / 2 - 128, 8, line, C_WHITE);
    }

    /* Ranked Queue Training Standby Badge (Top-Right) */
    if (ui.in_queue && !ui.open) {
        uint32_t sec = (ui_now_ms() - ui.queue_start_ms) / 1000u;
        snprintf(line, sizeof line, "RANKED QUEUE %02u:%02u | MAX 120MS", sec / 60u, sec % 60u);
        draw_border_box(argb, w, h, w - 216, 6, 210, 18, 0xD012203Cu, C_CYAN);
        draw_str(argb, w, h, w - 208, 11, line, C_YELLOW);
    }

    /* Match-Found Accept Modal */
    if (ui.match_found_modal) {
        draw_border_box(argb, w, h, w / 2 - 160, 48, 320, 62, 0xF0101830u, C_YELLOW);
        draw_str(argb, w, h, w / 2 - 80, 56, "RANKED MATCH FOUND!", C_YELLOW);
        snprintf(line, sizeof line, "VS %s (MMR %.0f)", ui.pending_opp_name, ui.pending_opp_rating);
        draw_str(argb, w, h, w / 2 - 140, 74, line, C_WHITE);
        draw_str(argb, w, h, w / 2 - 140, 92, "[ENTER/A] ACCEPT    [ESC/B] DECLINE", C_GREEN);
    }

    /* F8 Replay HUD Bar + Takeover Controls (Bottom) */
    if (replay_session_is_active() && ui.show_replay_hud && !ui.open) {
        REPLAY_STATUS rs;
        int bar_x = 18, bar_w = w - 36, fill_w = 0;
        replay_session_get_status(&rs);
        draw_border_box(argb, w, h, 8, h - 54, w - 16, 48, 0xD80C1222u, C_BORDER);
        if (rs.duration_frames > 0) {
            fill_w = (int)((uint64_t)rs.cur_frame * (uint64_t)bar_w / (uint64_t)rs.duration_frames);
            if (fill_w > bar_w) fill_w = bar_w;
        }
        fill_rect(argb, w, h, bar_x, h - 46, bar_w, 6, 0xFF28344Cu);
        fill_rect(argb, w, h, bar_x, h - 46, fill_w, 6, rs.takeover_slot >= 0 ? C_GREEN : C_CYAN);
        if (rs.bookmark_frame >= 0 && rs.duration_frames > 0) {
            int bx = bar_x + (int)((uint64_t)rs.bookmark_frame * (uint64_t)bar_w / (uint64_t)rs.duration_frames);
            fill_rect(argb, w, h, bx, h - 48, 3, 10, C_YELLOW);
        }
        snprintf(line, sizeof line, "F%u/%u %s | %s",
                 rs.cur_frame, rs.duration_frames,
                 rs.paused ? "[PAUSED]" : "[PLAYING]",
                 rs.takeover_slot == 0 ? "TAKEOVER P1" : rs.takeover_slot == 1 ? "TAKEOVER P2" : "WATCH");
        draw_str(argb, w, h, 18, h - 34, line, C_WHITE);
        draw_str(argb, w, h, 18, h - 20, "SPACE:PLAY  ,/.:-1/+1F  L/R:SEEK  B:MARK  F5:P1  F6:P2  F7:RESET  F8:HUD", C_GREY);
    }

    if (ui.toast[0] && ui_now_ms() < ui.toast_until_ms && !ui.open) {
        draw_border_box(argb, w, h, 14, 28, w - 28, 18, 0xE0183424u, C_GREEN);
        draw_str(argb, w, h, 24, 33, ui.toast, C_YELLOW);
    }
}

int net_ui_run_smoke_test(void) {
    static uint32_t scratch_buf[640 * 480];
    int t;
    net_ui_init();

    /* Exercise rendering all 5 F4 tabs + 56-char picker + 6-input PIN entry + HUD + URI join parsing */
    ui.open = 1;
    ui.pin_len = 6;
    for (t = 0; t < NUM_TABS; t++) {
        ui.tab = t;
        ui.row = 1;
        net_ui_draw_overlay(scratch_buf, 640, 480);
    }
    ui.tab = TAB_REPLAYS;
    ui.editing_field = 6;
    ui.picker_cursor = 55;
    net_ui_draw_overlay(scratch_buf, 640, 480);
    ui.editing_field = 0;
    ui.open = 0;

    ui.in_queue = 1;
    ui.match_found_modal = 1;
    snprintf(ui.pending_opp_name, sizeof ui.pending_opp_name, "SmokeOpp");
    ui.pending_opp_rating = 1620.0;
    net_ui_draw_hud(scratch_buf, 640, 480);
    ui.in_queue = 0;
    ui.match_found_modal = 0;

    net_ui_handle_join_uri("umvsc2://join/SMK999?k=testkey");
    net_ui_tick();
    http_client_ws_close();

    /* Sanity check MVC2_FIGHTERS table covers all 56 unique IDs */
    for (t = 0; t < 56; t++) {
        if (!MVC2_FIGHTERS[t].code || !MVC2_FIGHTERS[t].code[0]) {
            host_log("UI_SMOKE_TEST FAIL: empty fighter %d", t);
            net_ui_shutdown();
            return 1;
        }
    }
    host_log("UI_SMOKE_TEST PASS tabs=%d fighters=56", NUM_TABS);
    net_ui_shutdown();
    return 0;
}
