/*
 * test_mode.c  The arcade's test menu, as the test program on the cartridge lays it out (the decomp's src/test):
 * the game data and the game configuration, kept in the game's own backup memory and EEPROM. Its input test,
 * ROM board test and memory card settings are left out; free play, a boot ROM setting, is added to the
 * configuration.
 */

#include <stdio.h>
#include <string.h>

#include "naomi_host.h"
#include "test_mode.h"

#define WHITE 0xFFFFFF
#define RED 0xFF0000

#define BOOK_ID 0x42424730

/* the game's settings in the EEPROM (system.h's SYS_SET) */
typedef struct {
    uint32_t id;
    int8_t country, stereo, demo_sound_half, cont_enable, voice_type, difficulty, damage_level, time_speed;
    int8_t vms_mode, vm_write, speed_fix, event_flag, no_challenge_flag, open_lv;
} SYS_SET;

/* and its book-keeping in the backup memory (book.h's BOOK_WORK) */
typedef struct {
    int32_t id, coin_all, service_all, play_all, coin, service, play;
} BOOK;

/* the board is a USA one: its settings as the game starts them */
static const SYS_SET usa = { BOOK_ID, 1, 1, 0, 1, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0 };

enum { MAIN, DATA, CONFIG };
/* the configuration's lines, at the rows the arcade's menu has them */
enum { C_SOUND, C_DEMO, C_CONTINUE, C_DIFFICULTY, C_DAMAGE, C_TIMER, C_SPEED, C_EVENT, C_JOIN, C_FREEPLAY, C_DEFAULT,
       C_SAVE, C_LINES };
static const struct {
    int y;
    const char* name;
} lines[C_LINES] = {
    { 5, "SOUND MODE" }, { 6, "DEMO SOUND HALF" }, { 7, "CONTINUE" }, { 9, "DIFFICULTY" }, { 10, "DAMAGE LEVEL" },
    { 11, "TIMER SPEED" }, { 14, "GAME SPEED" }, { 15, "EVENT" }, { 16, "JOIN-IN" }, { 17, "FREE PLAY" },
    { 18, "DEFAULT" }, { 19, "SAVE & EXIT" },
};

static TEST_SCREEN screen;
static const char *ok_key, *back_key;
static int page, cursor, saved;
static SYS_SET set, now;
static int freeplay, freeplay_now;

static SYS_SET* game_set(void) {
    return host_user_eeprom();
}

static BOOK* game_book(void) {
    return host_user_backup();
}

static void clear(void) {
    memset(screen.ch, ' ', sizeof screen.ch);
    memset(screen.color, 0, sizeof screen.color);
}

static void put(int x, int y, const char* s, uint32_t color) {
    for (; *s != 0 && x < TM_COLS; s++, x++) {
        if (x >= 0 && y >= 0 && y < TM_ROWS) {
            screen.ch[y][x] = *s;
            screen.color[y][x] = color;
        }
    }
}

static void putf(int x, int y, uint32_t color, const char* fmt, int v) {
    char s[TM_COLS + 1];
    snprintf(s, sizeof s, fmt, v);
    put(x, y, s, color);
}

static void arrow(int x, int y) {
    put(x - 3, y, "->", WHITE);
}

static void help(const char* a, const char* b, const char* c) {
    char s[TM_COLS + 1];
    put(4, 25, "SELECT OPTION  = UP or DOWN", WHITE);
    snprintf(s, sizeof s, "%s%s", a, b);
    put(4, 26, s, WHITE);
    if (c != NULL) {
        snprintf(s, sizeof s, "               = %s", c);
        put(4, 27, s, WHITE);
    }
}

/* the settings differ from the country's own */
static int changed(const SYS_SET* s) {
    return s->stereo != usa.stereo || s->demo_sound_half != usa.demo_sound_half || s->cont_enable != usa.cont_enable ||
           s->difficulty != usa.difficulty || s->damage_level != usa.damage_level || s->time_speed != usa.time_speed ||
           s->speed_fix != usa.speed_fix || s->event_flag != usa.event_flag ||
           s->no_challenge_flag != usa.no_challenge_flag;
}

static void read_settings(void) {
    const char* f = config_get("freeplay");
    now = game_set() != NULL && game_set()->id == BOOK_ID ? *game_set() : usa;
    set = now;
    freeplay_now = freeplay = f != NULL && strcmp(f, "on") == 0;
}

void test_mode_open(const char* ok, const char* back) {
    ok_key = ok;
    back_key = back;
    page = MAIN;
    cursor = 0;
    saved = 0;
    read_settings();
}

/* the menu: GAME DATA, GAME CONFIGURATION, EXIT */
static void draw_main(void) {
    static const char* const items[] = { "GAME DATA", "GAME CONFIGURATION", "EXIT" };
    char s[TM_COLS + 1];
    int i;
    put(13, 2, "GAME TEST MODE", WHITE);
    for (i = 0; i < 3; i++) {
        put(15, 6 + i * 2, items[i], i == 1 && changed(&now) ? RED : WHITE);
    }
    arrow(15, 6 + cursor * 2);
    snprintf(s, sizeof s, "%s", cursor < 2 ? "START          = " : "RETURN TO GAME = ");
    help(s, ok_key, NULL);
}

static void draw_data(void) {
    static const struct {
        int x, y;
        const char* s;
    } text[] = {
        { 11, 2, "G A M E   D A T A" }, { 11, 4, "- ACCUMULATAED DATA -" }, { 11, 6, "COIN COUNTER" },
        { 11, 7, "SERVICE COUNTER" }, { 11, 8, "FREEPLAY COUNTER" }, { 11, 11, "- RESETABLE DATA -" },
        { 11, 13, "COIN COUNTER" }, { 11, 14, "SERVICE COUNTER" }, { 11, 15, "FREEPLAY COUNTER" },
        { 10, 17, "RESETABLE DATA CLEAR" }, { 19, 19, "YES" }, { 19, 20, "NO" },
    };
    const BOOK* bk = game_book();
    char s[TM_COLS + 1];
    size_t i;
    for (i = 0; i < sizeof text / sizeof text[0]; i++) {
        put(text[i].x, text[i].y, text[i].s, WHITE);
    }
    if (bk != NULL) {
        putf(27, 6, WHITE, " %d", bk->coin_all);
        putf(27, 7, WHITE, " %d", bk->service_all);
        putf(27, 8, WHITE, " %d", bk->play_all);
        putf(27, 13, WHITE, " %d", bk->coin);
        putf(27, 14, WHITE, " %d", bk->service);
        putf(27, 15, WHITE, " %d", bk->play);
    }
    arrow(19, cursor ? 19 : 20);
    put(8, 23, "SELECT WITH UP or DOWN", WHITE);
    snprintf(s, sizeof s, "     AND PRESS %s", ok_key);
    put(8, 24, s, WHITE);
    snprintf(s, sizeof s, "EXIT  = %s", back_key);
    put(8, 26, s, WHITE);
}

/* a level as stars, its number in its place */
static void level(int x, int y, const char* low, const char* high, int lv, int was, int max) {
    int i;
    put(x, y, low, WHITE);
    x += (int)strlen(low);
    for (i = 0; i <= max; i++, x++) {
        if (i == lv) {
            putf(x, y, lv != was ? RED : WHITE, "%d", lv + 1);
        } else {
            put(x, y, "*", WHITE);
        }
    }
    put(x, y, high, WHITE);
}

static void value(int line, const char* s, int differs) {
    put(20, lines[line].y, s, differs ? RED : WHITE);
}

static void draw_config(void) {
    static const char* const off_on[] = { "OFF", "ON" };
    static const char* const speeds[] = { "FREE SELECT", "NORMAL", "TURBO" };
    char s[TM_COLS + 1];
    int i;
    put(11, 2, "GAME CONFIGRATION", WHITE);
    for (i = 0; i < C_LINES; i++) {
        put(4, lines[i].y, lines[i].name, i == C_DEFAULT && changed(&set) ? RED : WHITE);
    }
    value(C_SOUND, set.stereo ? "STEREO" : "MONAURAL", set.stereo != now.stereo);
    value(C_DEMO, off_on[set.demo_sound_half & 1], set.demo_sound_half != now.demo_sound_half);
    value(C_CONTINUE, off_on[set.cont_enable & 1], set.cont_enable != now.cont_enable);
    level(20, lines[C_DIFFICULTY].y, "EASY[", "]HARD", set.difficulty, now.difficulty, 7);
    level(20, lines[C_DAMAGE].y, "LOW [", "]HIGH", set.damage_level, now.damage_level, 3);
    level(20, lines[C_TIMER].y, "LOW [", "]HIGH", set.time_speed, now.time_speed, 3);
    value(C_SPEED, speeds[set.speed_fix % 3], set.speed_fix != now.speed_fix);
    value(C_EVENT, off_on[set.event_flag & 1], set.event_flag != now.event_flag);
    value(C_JOIN, set.no_challenge_flag ? "OFF" : "ON", set.no_challenge_flag != now.no_challenge_flag);
    value(C_FREEPLAY, off_on[freeplay], freeplay != freeplay_now);
    arrow(4, lines[cursor].y);
    if (cursor == C_DEFAULT) {
        snprintf(s, sizeof s, "RESET          = ");
        help(s, ok_key, NULL);
    } else if (cursor == C_SAVE) {
        help("RETURN TO GAME TEST MODE", "", ok_key);
    } else {
        snprintf(s, sizeof s, "%s or LEFT or RIGHT", ok_key);
        help("MODIFY SETTING = ", s, NULL);
    }
}

static void step(int8_t* v, int way, int max) {
    *v = (int8_t)(*v + way > max ? 0 : *v + way < 0 ? max : *v + way);
}

/* one setting a step up or down, around its range */
static void modify(int way) {
    int8_t f = (int8_t)freeplay;
    switch (cursor) {
    case C_SOUND: step(&set.stereo, way, 1); break;
    case C_DEMO: step(&set.demo_sound_half, way, 1); break;
    case C_CONTINUE: step(&set.cont_enable, way, 1); break;
    case C_DIFFICULTY: step(&set.difficulty, way, 7); break;
    case C_DAMAGE: step(&set.damage_level, way, 3); break;
    case C_TIMER: step(&set.time_speed, way, 3); break;
    case C_SPEED: step(&set.speed_fix, way, 2); break;
    case C_EVENT: step(&set.event_flag, way, 1); break;
    case C_JOIN: step(&set.no_challenge_flag, way, 1); break;
    case C_FREEPLAY: step(&f, way, 1); freeplay = f; break;
    }
    if (set.event_flag) {
        set.cont_enable = 0;    /* no continues in an event */
    }
}

/* written to the EEPROM as the arcade's SAVE & EXIT does, and free play to mvc2.cfg */
static void save(void) {
    SYS_SET* g = game_set();
    if (g != NULL) {
        *g = set;
        g->id = BOOK_ID;
        g->country = usa.country;
        nlaUpdateUserEEPROM();
    }
    if (freeplay != freeplay_now) {
        config_set("freeplay", freeplay ? "on" : "off");
    }
    saved = 1;
    read_settings();
}

int test_mode_keys(int keys) {
    int lines_n = C_LINES;
    if (page == MAIN) {
        if (keys & TM_UP) {
            cursor = (cursor + 2) % 3;
        } else if (keys & TM_DOWN) {
            cursor = (cursor + 1) % 3;
        } else if ((keys & TM_BACK) || ((keys & TM_OK) && cursor == 2)) {
            return saved ? TM_RESTART : TM_CLOSED;
        } else if (keys & TM_OK) {
            page = cursor == 0 ? DATA : CONFIG;
            cursor = 0;
            read_settings();
        }
    } else if (page == DATA) {
        if (keys & (TM_UP | TM_DOWN)) {
            cursor ^= 1;
        } else if ((keys & TM_OK) && cursor && game_book() != NULL) {
            BOOK* bk = game_book();
            bk->coin = bk->service = bk->play = 0;
            nlaUpdateUserBackup();
        } else if (keys & TM_BACK) {
            page = MAIN;
            cursor = 0;
        }
    } else {
        do {
            if (keys & TM_UP) {
                cursor = (cursor + lines_n - 1) % lines_n;
            } else if (keys & TM_DOWN) {
                cursor = (cursor + 1) % lines_n;
            }
        } while ((keys & (TM_UP | TM_DOWN)) && cursor == C_CONTINUE && set.event_flag);
        if (keys & TM_BACK) {
            page = MAIN;
            cursor = 1;
            read_settings();
        } else if ((keys & TM_OK) && cursor == C_SAVE) {
            save();
            page = MAIN;
            cursor = 1;
        } else if ((keys & TM_OK) && cursor == C_DEFAULT) {
            set = usa;
            freeplay = 0;
        } else if (keys & (TM_LEFT | TM_RIGHT | TM_OK)) {
            modify(keys & TM_LEFT ? -1 : 1);
        }
    }
    return TM_OPEN;
}

const TEST_SCREEN* test_mode_screen(void) {
    clear();
    if (page == MAIN) {
        draw_main();
    } else if (page == DATA) {
        draw_data();
    } else {
        draw_config();
    }
    return &screen;
}
