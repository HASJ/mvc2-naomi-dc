/*
 * main_psp.c  The PSP front end: the frame loop, input, the log and the test switches (env.txt)
 *
 * The program's folder (ms0:/PSP/GAME/MVC2) holds mvc2.log, mvc2.cfg, env.txt and disc/, the Dreamcast's files.
 */

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pspctrl.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspkernel.h>
#include <psppower.h>
#include <systemctrl.h>
#include <kubridge.h>

#include "naomi_host.h"
#include "naomi_input.h"
#include "rd.h"
#include "../nl/nl_port.h"
#include "psp_front.h"
#include "psp_me.h"
#include "frame_log.h"

PSP_MODULE_INFO("MVC2", PSP_MODULE_USER, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_MAIN_THREAD_STACK_SIZE_KB(256);

#define LOG "mvc2.log"

extern char _ftext[], _etext[], _end[];

static volatile int quit_req;
static int test_run;            /* MVC2_FRAMES or MVC2_SHOT: no waits for a button */
static int feel;                /* MVC2_FEEL=1: a build to play: one short line a window, the log held in memory */
int psp_paced;                  /* one frame a vblank at most: not in a test run unless MVC2_PACE=1 */

/* ---- HOME's exit ---- */

/* the game's thread ends the program; if it hasn't moved on in 3 s (a wedge), this ends it, so HOME's quit never
   waits for ever */
static int exit_callback(int arg1, int arg2, void* common) {
    uint32_t t0 = sceKernelGetSystemTimeLow();
    (void)arg1;
    (void)arg2;
    (void)common;
    quit_req = 1;
    host_log("exit: HOME's quit at frame %u", host_frame());
    psp_log_flush();
    for (;;) {
        uint32_t step = psp_exit_step_us;
        if (sceKernelGetSystemTimeLow() - (step != 0 ? step : t0) > 3000000u) {
            psp_exit_now();
        }
        sceKernelDelayThread(50000);
    }
    return 0;
}

/* the memory stick away for a sleep: from the suspend's start until 3 s after the resume, or 20 s of waits for it
   should no resume come */
static volatile int stick_away, stick_waits;
static volatile uint32_t resumed_at;

int host_stick_wait(void) {
    if (resumed_at != 0 && sceKernelGetSystemTimeLow() - resumed_at > 3000000u) {
        resumed_at = 0;
    }
    if (stick_away && ++stick_waits > 100) {
        stick_away = 0;
        host_log("power: no resume after 20 s of waits for the memory stick: taken as back");
    }
    if (!stick_away && resumed_at == 0) {
        return 0;
    }
    sceKernelDelayThread(200000);
    return 1;
}

/* the power switch, a suspend and a resume, each written to the log at once */
static int power_callback(int count, int flags, void* common) {
    static int last;
    const int said = PSP_POWER_CB_POWER_SWITCH | PSP_POWER_CB_HOLD_SWITCH | PSP_POWER_CB_STANDBY |
                     PSP_POWER_CB_RESUME_COMPLETE | PSP_POWER_CB_RESUMING | PSP_POWER_CB_SUSPENDING;
    char text[160];
    (void)count;
    (void)common;
    if (flags & (PSP_POWER_CB_SUSPENDING | PSP_POWER_CB_STANDBY)) {
        stick_away = 1;
        stick_waits = 0;
    }
    if (flags & PSP_POWER_CB_RESUME_COMPLETE) {
        resumed_at = sceKernelGetSystemTimeLow() | 1u;
        stick_away = 0;
    }
    if ((flags & said) != (last & said)) {
        snprintf(text, sizeof text, "power: %08X%s%s%s%s%s%s at frame %u", (unsigned)flags,
                 flags & PSP_POWER_CB_POWER_SWITCH ? ", the power switch" : "",
                 flags & PSP_POWER_CB_HOLD_SWITCH ? ", hold" : "", flags & PSP_POWER_CB_SUSPENDING ? ", suspending" : "",
                 flags & PSP_POWER_CB_STANDBY ? ", standby" : "", flags & PSP_POWER_CB_RESUMING ? ", resuming" : "",
                 flags & PSP_POWER_CB_RESUME_COMPLETE ? ", resumed" : "", host_frame());
        if (feel) {
            host_log("%s", text);   /* held with the rest: the log isn't opened beside the game's files */
        } else {
            psp_log_now(text);
        }
    }
    last = flags;
    return 0;
}

/* MVC2_HOME_AT=frame (tests): HOME's quit after that frame, as from its callback (MVC2_CRASH=sleep at the next
   wedges the game) */
static int home_test(SceSize args, void* argp) {
    (void)args;
    (void)argp;
    return exit_callback(0, 0, NULL);
}

static int callback_thread(SceSize args, void* argp) {
    (void)args;
    (void)argp;
    sceKernelRegisterExitCallback(sceKernelCreateCallback("exit", exit_callback, NULL));
    scePowerRegisterCallback(-1, sceKernelCreateCallback("power", power_callback, NULL));
    sceKernelSleepThreadCB();
    return 0;
}

static void exit_callbacks(void) {
    SceUID th = sceKernelCreateThread("callbacks", callback_thread, 0x11, 0x4000, THREAD_ATTR_USER, NULL);
    if (th >= 0) {
        sceKernelStartThread(th, 0, NULL);
    }
}

/* ---- messages ---- */

static uint32_t buttons_down(void) {
    static uint32_t last;
    SceCtrlData pad;
    uint32_t down;
    sceCtrlPeekBufferPositive(&pad, 1);
    down = pad.Buttons & ~last;
    last = pad.Buttons;
    return down;
}

void psp_tell(const char* title, const char* text) {
    host_log("%s: %s", title, text);
    if (test_run) {
        return;
    }
    psp_video_text(title, text);
    pspDebugScreenPrintf("\nPress START.\n");
    while (!quit_req && !(buttons_down() & PSP_CTRL_START)) {
        sceDisplayWaitVblankStart();
        psp_crumb("a message shown", 0);    /* the watchdog's: waiting on the player */
    }
}

int psp_ask(const char* text) {
    uint32_t down = 0;
    psp_video_text("Marvel vs. Capcom 2", text);
    pspDebugScreenPrintf("\nCROSS: arcade, CIRCLE: Dreamcast, START: quit\n");
    while (!quit_req && !(down & (PSP_CTRL_CROSS | PSP_CTRL_CIRCLE | PSP_CTRL_START))) {
        sceDisplayWaitVblankStart();
        psp_crumb("a question shown", 0);
        down = buttons_down();
    }
    return down & PSP_CTRL_CROSS ? 0 : down & PSP_CTRL_CIRCLE ? 1 : -1;
}

/* a long unpack's screen: its text, then a bar, the time taken and the rate; the GE's screen back at its end. The
   unpack runs at 333 MHz */
static uint64_t unpack_t0;
static int unpack_row;
static void clock_set(int mhz);
static int clock_mhz(void);

void psp_unpack_screen(const char* text) {
    clock_set(text != NULL ? 333 : clock_mhz());
    if (text != NULL) {
        host_log("%s", text);
        psp_video_text("Marvel vs. Capcom 2", text);
        pspDebugScreenPrintf("\n");
        unpack_row = pspDebugScreenGetY();
        unpack_t0 = sceKernelGetSystemTimeWide();
        return;
    }
    host_log("unpacked in %.1f s", (sceKernelGetSystemTimeWide() - unpack_t0) / 1e6);
    psp_video_init(psp_video_bits16());
}

void psp_unpack_progress(uint32_t done, uint32_t total) {
    static uint32_t shown_pct = ~0u, shown_s = ~0u;
    uint32_t us = (uint32_t)(sceKernelGetSystemTimeWide() - unpack_t0), s = us / 1000000;
    uint32_t pct = total > 0 ? (uint32_t)((uint64_t)done * 100 / total) : 0;
    uint32_t kbs = us ? (uint32_t)((uint64_t)done * 1000000 / us) : 0;
    char bar[41];
    int i;
    HOST_CRUMB("unpacking the disc");
    scePowerTick(PSP_POWER_TICK_ALL);
    if (pct == shown_pct && s == shown_s) {
        return;
    }
    if (pct / 10 != shown_pct / 10) {
        host_log("unpacking: %u%% in %u s, %u.%u MB a second", (unsigned)pct, (unsigned)s, (unsigned)(kbs / 1024),
                 (unsigned)(kbs % 1024 * 10 / 1024));
    }
    shown_pct = pct;
    shown_s = s;
    for (i = 0; i < 40; i++) {
        bar[i] = i < (int)(pct * 40 / 100) ? '#' : '.';
    }
    bar[40] = 0;
    pspDebugScreenSetXY(0, unpack_row);
    pspDebugScreenPrintf("[%s] %3u%%\n%u:%02u, %u.%u MB a second    \n", bar, (unsigned)pct, (unsigned)(s / 60),
                         (unsigned)(s % 60), (unsigned)(kbs / 1024), (unsigned)(kbs % 1024 * 10 / 1024));
}

/* the reason on the screen until START, then the program ends */
static int fail(const char* text) {
    host_log("stopped: %s", text);
    psp_tell("Marvel vs. Capcom 2", text);
    psp_exit();
    return 1;
}

void* host_try_malloc(size_t bytes) {
    return malloc(bytes);
}

void* host_try_calloc(size_t n, size_t size) {
    return calloc(n, size);
}

void host_out_of_memory(const char* what, size_t bytes) {
    char text[200];
    snprintf(text, sizeof text, "Out of memory: %s of %lu bytes failed.", what, (unsigned long)bytes);
    fail(text);
}

/* ---- input ---- */

static HOST_INPUT input;
static unsigned presented;
static int switch_req, select_used, options_req;

const HOST_INPUT* host_input(void) {
    return &input;
}

/* MVC2_PRESS=n:key,..., as on the PC and 3DS: player 1 holds the key for 5 frames from frame n; coin and switch
   happen at n */
static int coin_req;

static uint32_t pressed(void) {
    static const struct {
        const char* name;
        uint32_t bit;
    } keys[] = {
        { "start", JVS_START }, { "up", JVS_UP }, { "down", JVS_DOWN }, { "left", JVS_LEFT }, { "right", JVS_RIGHT },
        { "b1", JVS_B1 }, { "b2", JVS_B2 }, { "b3", JVS_B3 }, { "b4", JVS_B4 }, { "b5", JVS_B5 }, { "b6", JVS_B6 },
    };
    enum { MAX_PRESSES = 1024, SWITCH = 1u << 31, COIN = 1u << 30 };
    static struct {
        unsigned at;
        uint32_t bits;
    } list[MAX_PRESSES];
    static int n = -1;
    uint32_t bits = 0;
    int k;
    if (n < 0) {
        const char* p = getenv("MVC2_PRESS");
        n = 0;
        while (p != NULL && *p != 0 && n < MAX_PRESSES) {
            char name[16];
            unsigned at;
            size_t i;
            if (sscanf(p, "%u:%15[a-z0-9]", &at, name) == 2) {
                list[n].at = at;
                list[n].bits = strcmp(name, "switch") == 0 ? SWITCH : strcmp(name, "coin") == 0 ? COIN : 0;
                for (i = 0; i < sizeof keys / sizeof keys[0]; i++) {
                    if (strcmp(name, keys[i].name) == 0) {
                        list[n].bits = keys[i].bit;
                    }
                }
                n++;
            }
            p = strchr(p, ',');
            p = p != NULL ? p + 1 : NULL;
        }
    }
    for (k = 0; k < n; k++) {
        if (list[k].bits == SWITCH || list[k].bits == COIN) {
            if (presented == list[k].at) {
                *(list[k].bits == SWITCH ? &switch_req : &coin_req) = 1;
            }
        } else if (presented >= list[k].at && presented < list[k].at + 5) {
            bits |= list[k].bits;
        }
    }
    return bits;
}

/* the buttons where the 3DS has them: SQUARE, TRIANGLE and L the punches and the first assist, CROSS, CIRCLE and R
   the kicks and the second; SELECT on its own is a coin, SELECT+START the other version, SELECT+TRIANGLE the
   options, SELECT+CROSS quits */
static void read_input(void) {
    static const struct {
        uint32_t key, bit;
    } keymap[] = {
        { PSP_CTRL_UP, JVS_UP }, { PSP_CTRL_DOWN, JVS_DOWN }, { PSP_CTRL_LEFT, JVS_LEFT },
        { PSP_CTRL_RIGHT, JVS_RIGHT }, { PSP_CTRL_START, JVS_START }, { PSP_CTRL_SQUARE, JVS_B1 },
        { PSP_CTRL_TRIANGLE, JVS_B2 }, { PSP_CTRL_LTRIGGER, JVS_B3 }, { PSP_CTRL_CROSS, JVS_B4 },
        { PSP_CTRL_CIRCLE, JVS_B5 }, { PSP_CTRL_RTRIGGER, JVS_B6 },
    };
    static uint32_t last;
    SceCtrlData pad;
    uint32_t held, down, up;
    size_t i;
    sceCtrlPeekBufferPositive(&pad, 1);
    held = pad.Buttons;
    down = held & ~last;
    up = last & ~held;
    last = held;
    input.player[0] = pressed();
    input.player[1] = 0;
    input.coin = 0;
    input.test = 0;
    if (coin_req) {
        input.coin = 1;
        coin_req = 0;
    }
    if (held & PSP_CTRL_SELECT) {
        if (down & PSP_CTRL_START) {
            switch_req = 1;
        }
        if (down & PSP_CTRL_CROSS) {
            quit_req = 1;
        }
        if (down & PSP_CTRL_TRIANGLE) {
            options_req = 1;
        }
        if (held & (PSP_CTRL_START | PSP_CTRL_CROSS | PSP_CTRL_TRIANGLE)) {
            select_used = 1;
        }
        held &= ~(uint32_t)(PSP_CTRL_START | PSP_CTRL_CROSS | PSP_CTRL_TRIANGLE);
    } else if (up & PSP_CTRL_SELECT) {
        input.coin = !select_used;
        select_used = 0;
    }
    /* the stick as the pad */
    held |= pad.Lx < 64 ? PSP_CTRL_LEFT : pad.Lx > 192 ? PSP_CTRL_RIGHT : 0;
    held |= pad.Ly < 64 ? PSP_CTRL_UP : pad.Ly > 192 ? PSP_CTRL_DOWN : 0;
    for (i = 0; i < sizeof keymap / sizeof keymap[0]; i++) {
        if (held & keymap[i].key) {
            input.player[0] |= keymap[i].bit;
        }
    }
}

/* ---- the frame ---- */

static uint32_t* frame;     /* rd_soft's */
static int stretch;         /* scale = stretch: the game across the screen's 480 */
static int use_ge;          /* renderer = ge, the default (soft: rd_soft) */

/* MVC2_SHOT=n,m,... */
static int want_shot(unsigned n) {
    const char* list = getenv("MVC2_SHOT");
    char want[16];
    if (list == NULL) {
        return 0;
    }
    snprintf(want, sizeof want, "%u", n);
    for (;;) {
        size_t k = strcspn(list, ",");
        if (k == strlen(want) && strncmp(list, want, k) == 0) {
            return 1;
        }
        if (list[k] == 0) {
            return 0;
        }
        list += k + 1;
    }
}

static void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* ABGR pixels as a 32-bit BMP, whose pixels are BGRA */
void psp_save_bmp(const char* name, const uint32_t* px, int w, int h, int pitch) {
    static uint32_t row[RD_WIDTH];
    uint8_t hdr[54] = { 'B', 'M' };
    FILE* f = fopen(name, "wb");
    int x, y;
    if (f == NULL) {
        return;
    }
    put32(hdr + 2, 54 + (uint32_t)(w * h * 4));
    put32(hdr + 10, 54);
    put32(hdr + 14, 40);
    put32(hdr + 18, (uint32_t)w);
    put32(hdr + 22, (uint32_t)h);
    hdr[26] = 1;
    hdr[28] = 32;
    fwrite(hdr, 1, sizeof hdr, f);
    for (y = h - 1; y >= 0; y--) {
        for (x = 0; x < w; x++) {
            uint32_t c = px[y * pitch + x];
            row[x] = (c & 0xFF00FF00u) | (c >> 16 & 0xFF) | (c & 0xFF) << 16;
        }
        fwrite(row, 4, (size_t)w, f);
    }
    fclose(f);
}

/* the times in the log every STATS_US or STATS_FRAMES: the game's (present to present), the drawing's and the
   present's; with the GE, the CPU's part of the drawing (its list made) and the GE's (the list drawn) */
#define STATS_US 5000000u
#define STATS_FRAMES 300u
static uint64_t t_end, t_window, us_game, us_draw, us_show;
static uint32_t us_game_most, fills_window;     /* the window's slowest game frame; host_fills at its start */
static unsigned window_frames, drawn, window_switches, window_missed, window_late;

static uint64_t now_us(void) {
    return (uint64_t)sceKernelGetSystemTimeWide();
}

#define KSEG0(f) ((uint32_t)(uintptr_t)(f) | 0x80000000u)

/* in kernel mode: the CPU's cycle counter */
static int cycle_count(void) {
    uint32_t c;
    __asm__ volatile("mfc0 %0, $9" : "=r"(c));
    return (int)c;
}

static int kernel_word(uint32_t at) {
    return *(const volatile int*)(uintptr_t)at;
}

/* in kernel mode: the routine scePower's set-clock wrappers all call, from the jal each keeps past its first two
   words (all a speed lock like ARK's defaultclock overwrites), if all three agree on one just below them */
static int clock_routine(uint32_t w0, uint32_t w1, uint32_t w2) {
    uint32_t w[3] = { w0, w1, w2 }, found = 0;
    int k, i;
    for (k = 0; k < 3; k++) {
        const volatile uint32_t* c = (const volatile uint32_t*)(uintptr_t)w[k];
        uint32_t t = 0;
        if (w[k] < 0x88000000u || w[k] >= 0x88800000u) {
            return 0;
        }
        for (i = 2; i < 8 && t == 0; i++) {
            if (c[i] >> 26 == 3) {
                t = ((w[k] + 4u * (uint32_t)i + 4u) & 0xF0000000u) | (c[i] & 0x03FFFFFFu) << 2;
            }
        }
        if (t == 0 || (found != 0 && t != found) || t >= w[k] || w[k] - t > 0x4000u) {
            return 0;
        }
        found = t;
    }
    return (int)found;
}

static int clock_call(uint32_t f, uint32_t mhz, uint32_t bus) {
    return ((int (*)(int, int, int))(uintptr_t)f)((int)mhz, (int)mhz, (int)bus);
}

/* the CPU's clock by its cycle counter over a busy spin of us microseconds (the counter stops while the CPU waits) */
static double counted_mhz(uint32_t us) {
    uint32_t c0 = 0, c1 = 0, t0, t1;
    psp_kernel_call(KSEG0(cycle_count), 0, 0, 0, &c0);
    t0 = sceKernelGetSystemTimeLow();
    while (sceKernelGetSystemTimeLow() - t0 < us) {
    }
    psp_kernel_call(KSEG0(cycle_count), 0, 0, 0, &c1);
    t1 = sceKernelGetSystemTimeLow();
    return (c1 - c0) / (double)(t1 - t0);
}

/* input to picture: when each frame's buttons were read and when its picture was flipped */
static uint64_t input_at[16], lat_us, pic_us;
static unsigned input_of[16], lat_n, lat_later;

static void input_read(void) {
    unsigned f = presented + 1;
    if (input_of[f & 15] != f) {
        input_of[f & 15] = f;
        input_at[f & 15] = now_us();
        psp_render_input();
    }
}

void psp_lat_shown(unsigned frame) {
    if (input_of[frame & 15] == frame) {
        lat_us += now_us() - input_at[frame & 15];
        pic_us += (uint32_t)(psp_video_shown_us() - (uint32_t)input_at[frame & 15]);
        lat_later += presented - frame;
        lat_n++;
    }
}

/* MVC2_FRAMELOG=FILE: a line for each frame over 17 ms or with disc work, kept in memory and written at exit */
typedef struct {
    uint32_t frame;
    uint16_t game, list, show, gd, sync, open, fopen, fclose, end, lz, conv, tex, ge_tex, adx;    /* 0.1 ms */
    uint16_t kb, opens, syncs, reqs, waits, convs;
} FL_REC;
#define FL_MOST 6000
static FL_REC* fl_recs;
static uint32_t fl_n;

static uint16_t fl_ms10(uint64_t ns) {
    return (uint16_t)(ns / 100000 > 65535 ? 65535 : ns / 100000);
}

static void fl_record(uint64_t t0, uint64_t t1, uint64_t t2) {
    static int on = -1;
    static uint32_t tex_last;
    uint32_t tex = psp_rstats.tex_us, ge_tex = tex >= tex_last ? tex - tex_last : tex;
    uint64_t game = t_end != 0 ? t0 - t_end : 0;
    FL_REC* r;
    tex_last = tex;
    if (on < 0) {
        on = getenv("MVC2_FRAMELOG") != NULL && (fl_recs = malloc(FL_MOST * sizeof *fl_recs)) != NULL;
    }
    if (on && fl_n < FL_MOST && (game + (t1 - t0) > 17000 || fl.gd_open || fl.gd_sync || fl.gd_req || fl.gd_ns)) {
        r = &fl_recs[fl_n++];
        r->frame = presented;
        r->game = fl_ms10(game * 1000);
        r->list = fl_ms10((t1 - t0) * 1000);
        r->show = fl_ms10((t2 - t1) * 1000);
        r->gd = fl_ms10(fl.gd_ns);
        r->sync = fl_ms10(fl.sync_ns);
        r->open = fl_ms10(fl.open_ns);
        r->fopen = fl_ms10(fl.fopen_ns);
        r->fclose = fl_ms10(fl.fclose_ns);
        r->end = fl_ms10(fl.end_ns);
        r->lz = fl_ms10(fl.lz_ns);
        r->conv = fl_ms10(fl.conv_ns);
        r->tex = fl_ms10(fl.tex_ns);
        r->ge_tex = fl_ms10((uint64_t)ge_tex * 1000);
        r->adx = fl_ms10(fl.adx_ns);
        r->kb = (uint16_t)(fl.gd_bytes >> 10 > 65535 ? 65535 : fl.gd_bytes >> 10);
        r->opens = (uint16_t)fl.gd_open;
        r->syncs = (uint16_t)fl.gd_sync;
        r->reqs = (uint16_t)fl.gd_req;
        r->waits = (uint16_t)fl.gd_wait;
        r->convs = (uint16_t)fl.tex_conv;
    }
    memset(&fl, 0, sizeof fl);
}

static void fl_write(void) {
    FILE* f;
    uint32_t i;
    if (fl_recs == NULL || (f = fopen(getenv("MVC2_FRAMELOG"), "w")) == NULL) {
        return;
    }
    fprintf(f, "frame game list show gd sync open fopen fclose end lz conv tex ge_tex adx kb opens syncs reqs waits convs\n");
    for (i = 0; i < fl_n; i++) {
        const FL_REC* r = &fl_recs[i];
        fprintf(f, "%u %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %u %u %u %u %u %u\n",
                (unsigned)r->frame, r->game / 10.0, r->list / 10.0, r->show / 10.0, r->gd / 10.0, r->sync / 10.0,
                r->open / 10.0, r->fopen / 10.0, r->fclose / 10.0, r->end / 10.0, r->lz / 10.0, r->conv / 10.0,
                r->tex / 10.0, r->ge_tex / 10.0, r->adx / 10.0, r->kb, r->opens, r->syncs, r->reqs, r->waits, r->convs);
    }
    fclose(f);
    host_log("frame log: %u frames written to %s", (unsigned)fl_n, getenv("MVC2_FRAMELOG"));
}

static void stats(uint64_t t0, uint64_t t1, uint64_t t2) {
    fl_record(t0, t1, t2);
    if (t_end != 0) {
        us_game += t0 - t_end;
        us_game_most = t0 - t_end > us_game_most ? (uint32_t)(t0 - t_end) : us_game_most;
        us_draw += t1 - t0;
        us_show += t2 - t1;
        window_frames++;
    }
    t_end = t2;
    if (t_window == 0) {
        t_window = t2;
    } else if (feel && (t2 - t_window >= STATS_US || window_frames >= STATS_FRAMES)) {
        char list[320] = "";
        double n = window_frames ? window_frames : 1;
        if (use_ge) {
            psp_render_window(list, sizeof list);
        }
        host_log("frames %u-%u: %.2f a second; %u vblanks missed, by %u frames; game %.2f ms, the most %.1f; %u disc "
                 "reads; %s; input to flip %.2f ms, %.2f frames; to the picture %.2f ms", presented - window_frames + 1,
                 presented,
                 window_frames * 1e6 / (double)(t2 - t_window), window_missed, window_late, us_game / n / 1000.0,
                 us_game_most / 1000.0, (unsigned)(host_fills - fills_window), list,
                 lat_n ? lat_us / (double)lat_n / 1000.0 : 0.0, lat_n ? lat_later / (double)lat_n : 0.0,
                 lat_n ? pic_us / (double)lat_n / 1000.0 : 0.0);
        us_game_most = 0;
        fills_window = host_fills;
        lat_us = pic_us = 0;
        lat_n = lat_later = 0;
        t_window = t2;
        us_game = us_draw = us_show = 0;
        window_frames = drawn = window_missed = window_late = 0;
    } else if (t2 - t_window >= STATS_US || window_frames >= STATS_FRAMES) {
        double n = window_frames ? window_frames : 1, d = drawn ? drawn : 1;
        if (psp_paced) {
            host_log("frames %u-%u: %u vblanks missed, by %u frames", presented - window_frames + 1, presented,
                     window_missed, window_late);
        }
        host_log("frames %u-%u: %.2f a second, %u drawn; game %.2f ms, %s %.2f ms a drawn frame, present %.2f ms; "
                 "heap in use %lu KB, free %lu KB; %.1f coroutine switches a frame", presented - window_frames + 1, presented,
                 window_frames * 1e6 / (double)(t2 - t_window), drawn, us_game / n / 1000.0, use_ge ? "the list" : "rd_soft",
                 us_draw / d / 1000.0, us_show / d / 1000.0, (unsigned long)psp_mem_in_use() >> 10,
                 (unsigned long)psp_mem_free() >> 10, (coro_psp_switches - window_switches) / n);
        window_switches = coro_psp_switches;
        {
            /* the clock timed, and the CPU's share of the window not waiting */
            static uint32_t c_last;
            static uint64_t t_last;
            uint32_t c = 0;
            double mhz = counted_mhz(2000);
            psp_kernel_call(KSEG0(cycle_count), 0, 0, 0, &c);
            host_log("  CPU %.0f MHz by its cycle counter (scePower says %d), busy %.0f%%; bus %d MHz", mhz,
                     scePowerGetCpuClockFrequencyInt(),
                     t_last != 0 && mhz > 0 ? 100.0 * (c - c_last) / (double)(t2 - t_last) / mhz : 0.0,
                     scePowerGetBusClockFrequencyInt());
            c_last = c;
            t_last = now_us();
        }
        if (lat_n != 0) {
            host_log("  input to flip %.2f ms, the picture flipped %.2f frames after its own and shown %.2f ms after the "
                     "input", lat_us / (double)lat_n / 1000.0, lat_later / (double)lat_n, pic_us / (double)lat_n / 1000.0);
        }
        lat_us = pic_us = 0;
        lat_n = lat_later = 0;
        psp_prof_window(window_frames);
        if (use_ge && psp_rstats.frames != 0) {
            PSP_RSTATS* r = &psp_rstats;
            double f = r->frames;
            host_log("  GE: CPU %.2f ms (set-up %.2f, opaque %.2f, punch-through %.2f, translucent %.2f, finish %.2f), "
                     "GE %.2f ms a frame (background %.2f, opaque %.2f, punch-through %.2f, translucent %.2f, after "
                     "%.2f), waited for %.2f ms", r->cpu_us / f / 1000.0, r->cpu_sec[0] / f / 1000.0,
                     r->cpu_sec[1] / f / 1000.0, r->cpu_sec[2] / f / 1000.0, r->cpu_sec[3] / f / 1000.0,
                     r->cpu_sec[4] / f / 1000.0, r->ge_us / f / 1000.0, r->sec_us[0] / f / 1000.0,
                     r->sec_us[1] / f / 1000.0, r->sec_us[2] / f / 1000.0, r->sec_us[3] / f / 1000.0,
                     r->sec_us[4] / f / 1000.0, r->wait_us / f / 1000.0);
            host_log("  %.0f triangles (%.0f vertices, %.0f cut, %.0f sprites) in %.0f "
                     "batches; %.0f 1/w steps%s%s%s%s", r->tris / f,
                     r->verts / f, r->clipped / f, r->sprites / f, r->batches / f, r->depth_values / f,
                     r->skipped ? "; some not drawable" : "", r->dropped ? "; some past a full list" : "",
                     r->offset_tris ? "; some without their offset colour" : "", r->mirrored ? "; some mirrored" : "");
            host_log("  models a frame: %.1f (%.0f triangles), meshes %u KB", r->groups / f, r->group_tris / f,
                     r->mesh_kb);
            host_log("  textures a frame: %.1f found, %.1f made (%.1f KB), %.2f let go; %u held in %u KB (%u KB in "
                     "VRAM), made in %.2f ms; the game's %lu KB; sprite tiles %.1f converted, %.1f copied to a cell, "
                     "%.1f not",
                     r->tex_hits / f, r->tex_made / f, r->tex_made_bytes / f / 1024.0, r->tex_evicted / f, r->tex_count,
                     r->tex_kb, r->tex_vram_kb, r->tex_us / f / 1000.0, (unsigned long)nl_tex_held() >> 10,
                     r->atlas_made / f, r->atlas_copies / f, r->atlas_full / f);
            memset(r, 0, sizeof *r);
        }
        t_window = t2;
        us_game = us_draw = us_show = 0;
        window_frames = drawn = window_missed = window_late = 0;
    }
}

/* the frame's end: with psp_paced, no sooner than the vblank after the last frame's end; one that ends later has
   missed that many vblanks and goes on at once (its picture shown a vblank late, the one before shown twice) */
static void pace(void) {
    static unsigned last_v;
    unsigned v = sceDisplayGetVcount();
    if (!psp_paced) {
        return;
    }
    if (v == last_v) {
        psp_video_wait_past(v);
        v = sceDisplayGetVcount();
    } else if (last_v != 0 && v - last_v > 1) {
        window_missed += v - last_v - 1;
        window_late++;
    }
    last_v = v;
    if (use_ge) {
        uint32_t w = psp_render_start_wait();
        if (w != 0) {
            sceKernelDelayThread(w);
        }
    }
}

/* MVC2_DRAW_EVERY=n: one frame in n drawn (0: none, the game alone); MVC2_SHOT's frames always */
static void decide_next(void) {
    static int every = -1;
    if (every < 0) {
        const char* e = getenv("MVC2_DRAW_EVERY");
        every = e != NULL ? atoi(e) : 1;
        if (every != 1) {
            host_log("drawing one frame in %d (MVC2_DRAW_EVERY)", every);
        }
    }
    rd_discard = !want_shot(presented + 1) && (every <= 0 || (presented + 1) % (unsigned)every != 0);
}

/* rd_soft's frame, made when first wanted */
static int soft_frame(void) {
    if (frame == NULL) {
        frame = memalign(64, RD_WIDTH * 512 * sizeof *frame);   /* the GE reads its textures 512 high */
        if (frame == NULL) {
            return 0;
        }
        memset(frame, 0, RD_WIDTH * 512 * sizeof *frame);
    }
    return 1;
}

/* MVC2_RAM_SUM=n: every n frames a hash of each megabyte of the game's RAM, to compare two builds' runs; at one of
   them MVC2_RAM_DUMP=frame writes it all to ram.bin */
static void ram_sums(unsigned frame) {
    static int every = -1;
    const uint32_t* w = (const uint32_t*)(uintptr_t)MVC2_RAM_BASE;
    char line[16 * 9 + 1];
    unsigned mb, i;
    if (every < 0) {
        every = getenv("MVC2_RAM_SUM") != NULL ? atoi(getenv("MVC2_RAM_SUM")) : 0;
    }
    if (every <= 0 || frame % (unsigned)every != 0) {
        return;
    }
    while (host_read_busy()) {
        sceKernelDelayThread(100);      /* a disc read lands in the game's RAM as the reader goes */
    }
    for (mb = 0; mb < MVC2_RAM_SIZE >> 20 && mb < 16; mb++) {
        uint32_t h = 2166136261u;
        for (i = 0; i < (1u << 20) / 4; i++) {
            uint32_t v = *w++;
            if (v >= 0x08800000u && v < 0x0C000000u && v - MVC2_RAM_BASE >= MVC2_RAM_SIZE) {
                v = 0;      /* the program's and its heaps' addresses, which move between builds */
            }
            h = (h ^ v) * 16777619u;
        }
        snprintf(line + mb * 9, 10, " %08lX", (unsigned long)h);
    }
    host_log("RAM at frame %u:%s", frame, line);
    if (getenv("MVC2_RAM_DUMP") != NULL && (unsigned)atoi(getenv("MVC2_RAM_DUMP")) == frame) {
        FILE* f = fopen("ram.bin", "wb");
        if (f != NULL) {
            fwrite((const void*)(uintptr_t)MVC2_RAM_BASE, 1, MVC2_RAM_SIZE, f);
            fclose(f);
        }
    }
}

void host_present(void) {
    uint64_t t0 = now_us(), t1, t2;
    char name[32];
    psp_audio_due();        /* made as the frame's list goes to the ME, which makes it after the list; else below */
    presented++;
    scePowerTick(PSP_POWER_TICK_ALL);   /* no auto-sleep or screen-off while the game runs, buttons or none */
    psp_crumb_frame(presented);
    psp_crash_test(presented);
    psp_prof_frame(presented);
    ram_sums(presented);
    if (presented == 300 && !feel) {
        host_log("CPU %.0f MHz by its cycle counter (scePower says %d), bus %d MHz at frame 300", counted_mhz(20000),
                 scePowerGetCpuClockFrequencyInt(), scePowerGetBusClockFrequencyInt());
    }
    if (rd_discard) {
        psp_audio_frame();
        pace();
        rd_begin_frame();
        stats(t0, t0, now_us());
        decide_next();
        return;
    }
    drawn++;
    snprintf(name, sizeof name, "shot_%u.bmp", presented);
    if (use_ge && !psp_render_frame(stretch, presented)) {
        use_ge = 0;
        host_log("drawn by rd_soft from now on");
    }
    if (!use_ge) {
        if (!soft_frame()) {
            host_out_of_memory("the picture", RD_WIDTH * 512 * sizeof *frame);
        }
        rd_end_frame(frame, RD_WIDTH);
    }
    rd_begin_frame();
    t1 = now_us();
    if (!use_ge) {
        if (want_shot(presented)) {
            psp_save_bmp(name, frame, RD_WIDTH, RD_HEIGHT, RD_WIDTH);
            t1 = now_us();
        }
        psp_video_show(frame, stretch);
        psp_video_flip();
        psp_lat_shown(presented);
    } else if (want_shot(presented)) {
        psp_render_idle();
        psp_video_shot(name);
        t1 = now_us();
    }
    psp_audio_frame();
    pace();
    t2 = now_us();
    stats(t0, t1, t2);
    decide_next();
}

/* ---- start-up ---- */

/* env.txt's NAME=value lines, for test runs */
static void read_env(void) {
    static char line[16 << 10];
    FILE* f = fopen("env.txt", "r");
    while (f != NULL && fgets(line, sizeof line, f) != NULL) {
        char* eq = strchr(line, '=');
        if (line[0] == '#' || eq == NULL) {
            continue;
        }
        *eq = 0;
        eq[1 + strcspn(eq + 1, "\r\n")] = 0;
        host_log("env: %s=%.48s%s", line, eq + 1, strlen(eq + 1) > 48 ? "..." : "");
        setenv(line, eq + 1, 1);
    }
    if (f != NULL) {
        fclose(f);
    }
}

/* the switches set by default, env.txt's own value kept; MVC2_FEEL=1's as well: nothing timed or written to the
   memory stick while the game plays (the memory card saved as the program ends) */
static void set_defaults(void) {
    static const char* const on[][2] = { { "MVC2_LZ_CACHE", "1" },    /* the sprite cells kept unpacked (nl_lz.c) */
                                         { "MVC2_GD_ASYNC", "1" },    /* the disc read on its own thread (dc_gd.c) */
                                         { "MVC2_GD_AHEAD", "1" }, { "MVC2_GD_NEXT", "1" }, { "MVC2_GD_NOBUF", "1" },
                                         { "MVC2_GD_KEEP", "1" }, { "MVC2_GD_PROGS", "1" },  /* fewer opens, as on the 3DS */
                                         { "MVC2_TEX_SKIP", "1" },    /* a texture loaded again in a frame not read again */
                                         { "MVC2_ME", "1" } };        /* the GE's lists made on the Media Engine */
    static const char* const feel_on[][2] = { { "MVC2_LOG_SYNC", "0" }, { "MVC2_VMU_LATER", "1" },
                                              { "MVC2_GE_SECTIONS", "0" }, { "MVC2_WATCHDOG", "15" } };
    int i, given = getenv("MVC2_FEEL") != NULL;
    for (i = 0; i < (int)(sizeof on / sizeof *on); i++) {
        if (getenv(on[i][0]) == NULL) {
            setenv(on[i][0], on[i][1], 0);
            host_log("default: %s=%s", on[i][0], on[i][1]);
        }
    }
    /* play settings unless a test run or MVC2_FEEL=0; the watchdog only when MVC2_FEEL is given (test packages) */
    feel = given ? atoi(getenv("MVC2_FEEL")) != 0 : !test_run;
    if (feel && !given) {
        setenv("MVC2_FEEL", "1", 0);
        host_log("default: MVC2_FEEL=1 (a run to play)");
    }
    for (i = 0; feel && i < (int)(sizeof feel_on / sizeof *feel_on); i++) {
        if (getenv(feel_on[i][0]) == NULL && (given || strcmp(feel_on[i][0], "MVC2_WATCHDOG") != 0)) {
            setenv(feel_on[i][0], feel_on[i][1], 0);
            host_log("MVC2_FEEL: %s=%s", feel_on[i][0], feel_on[i][1]);
        }
    }
}

/* the clock under a speed lock: scePower's own routine called past the wrappers the lock made return 0 */
static void clock_past_lock(int mhz) {
    static const uint32_t nids[3] = { 0x545A7F3Cu, 0xA4E93389u, 0xEBD177D6u };
    uint32_t w[3], f = 0, r = 0, head[2] = { 0, 0 };
    int k;
    for (k = 0; k < 3; k++) {
        w[k] = (uint32_t)sctrlHENFindFunction("scePower_Service", "scePower", nids[k]);
    }
    psp_kernel_call(KSEG0(kernel_word), w[0], 0, 0, &head[0]);
    psp_kernel_call(KSEG0(kernel_word), w[0] + 4, 0, 0, &head[1]);
    psp_kernel_call(KSEG0(clock_routine), w[0], w[1], w[2], &f);
    host_log("clock: scePower's wrappers at %08lX %08lX %08lX begin %08lX %08lX%s; their routine %08lX",
             (unsigned long)w[0], (unsigned long)w[1], (unsigned long)w[2], (unsigned long)head[0],
             (unsigned long)head[1], head[0] == 0x03E00008u && head[1] == 0x00001021u ? " (made to return 0)" : "",
             (unsigned long)f);
    if (f == 0) {
        return;
    }
    psp_kernel_call(KSEG0(clock_call), f, (uint32_t)mhz, (uint32_t)mhz / 2, &r);
    host_log("CPU %d MHz, bus %d MHz (scePower's routine gave %08lX)", scePowerGetCpuClockFrequencyInt(),
             scePowerGetBusClockFrequencyInt(), (unsigned long)r);
}

/* the bus's speed by uncached reads of main RAM, each waiting on the one before: ns a read and CPU cycles a read */
static double bus_read_ns(double* cycles) {
    static uint32_t ring[16384] __attribute__((aligned(64)));
    volatile uint32_t* u = (volatile uint32_t*)((uintptr_t)ring | 0x40000000u);
    uint32_t i, at = 0, c0 = 0, c1 = 0, t0, t1, n = 0;
    for (i = 0; i < 16384; i++) {
        ring[i] = (i + 16 * 97) % 16384;
    }
    sceKernelDcacheWritebackInvalidateAll();
    psp_kernel_call(KSEG0(cycle_count), 0, 0, 0, &c0);
    t0 = sceKernelGetSystemTimeLow();
    while (sceKernelGetSystemTimeLow() - t0 < 20000) {
        for (i = 0; i < 256; i++) {
            at = u[at];
        }
        n += 256;
    }
    t1 = sceKernelGetSystemTimeLow();
    psp_kernel_call(KSEG0(cycle_count), 0, 0, 0, &c1);
    *cycles = (c1 - c0) / (double)n;
    return (t1 - t0) * 1000.0 / n;
}

/* the clock: 222 MHz (the PSP's own), 266 or 333, from mvc2.cfg's clock (MVC2_CPU_MHZ overrides it), each rounded
   to the nearest of those, the steps scePower's routine takes past ARK's lock (300 gave 333). The bus at half */
static const int clock_steps[] = { 222, 266, 333 };
static double bus_ns0;      /* an uncached read at the start, at the bus scePower said then */
static int bus_mhz0, clock_now;

static int clock_round(int mhz) {
    int i, best = clock_steps[0];
    for (i = 1; i < (int)(sizeof clock_steps / sizeof *clock_steps); i++) {
        if (abs(clock_steps[i] - mhz) < abs(best - mhz)) {
            best = clock_steps[i];
        }
    }
    return best;
}

static int clock_mhz(void) {
    const char* v = getenv("MVC2_CPU_MHZ") != NULL ? getenv("MVC2_CPU_MHZ") : config_get("clock");
    return clock_round(v != NULL ? atoi(v) : 222);
}

/* the bus from an uncached read's time against the start's */
static double bus_mhz(void) {
    double c, ns = bus_read_ns(&c);
    return ns > 0 ? bus_mhz0 * bus_ns0 / ns : 0;
}

/* mhz set and the CPU and bus it gave measured: the CPU by its cycle counter (scePower's own figure where there is
   none, in PPSSPP), the bus by uncached reads */
static void clock_apply(int mhz, double* cpu, double* bus) {
    double c;
    clock_set(mhz);
    clock_now = mhz;
    c = counted_mhz(20000);
    *cpu = c > 0 ? c : scePowerGetCpuClockFrequencyInt();
    *bus = bus_mhz();
    host_log("clock: %d MHz set: CPU %.0f MHz by its cycle counter, bus about %.0f MHz by uncached reads (scePower says "
             "%d and %d)", mhz, c, *bus, scePowerGetCpuClockFrequencyInt(), scePowerGetBusClockFrequencyInt());
}

/* at the start, before mvc2.cfg is read: the bus measured as it was, then MVC2_CPU_MHZ's clock (mvc2.cfg's is set
   once pick_game has read it) */
static void clock_setup(void) {
    double c, cpu, bus;
    bus_mhz0 = scePowerGetBusClockFrequencyInt();
    bus_ns0 = bus_read_ns(&c);
    host_log("clock: CPU %d MHz, bus %d MHz at the start (an uncached read %.1f ns, %.1f cycles)",
             scePowerGetCpuClockFrequencyInt(), bus_mhz0, bus_ns0, c);
    if (getenv("MVC2_CPU_MHZ") != NULL) {
        clock_apply(clock_mhz(), &cpu, &bus);
    }
}

static void clock_from_config(void) {
    double cpu, bus;
    if (clock_mhz() != clock_now) {
        clock_apply(clock_mhz(), &cpu, &bus);
    }
}

/* the custom firmware's call when the kernel's does not take, and scePower's own routine when neither does */
static void clock_set(int mhz) {
    int r;
    if (psp_me_ours()) {
        host_log("clock: %d MHz not set: the ME is ours (a clock change while it runs hangs the PSP)", mhz);
        return;
    }
    host_log("CPU %d MHz, bus %d MHz before", scePowerGetCpuClockFrequencyInt(), scePowerGetBusClockFrequencyInt());
    r = scePowerSetClockFrequency(mhz, mhz, mhz / 2);
    host_log("CPU %d MHz, bus %d MHz (scePowerSetClockFrequency(%d) gave %08X)", scePowerGetCpuClockFrequencyInt(),
             scePowerGetBusClockFrequencyInt(), mhz, (unsigned)r);
    if (abs(scePowerGetCpuClockFrequencyInt() - mhz) > 2) {
        sctrlHENSetSpeed(mhz, mhz / 2);
        host_log("CPU %d MHz, bus %d MHz (sctrlHENSetSpeed)", scePowerGetCpuClockFrequencyInt(),
                 scePowerGetBusClockFrequencyInt());
    }
    if (abs(scePowerGetCpuClockFrequencyInt() - mhz) > 2) {
        clock_past_lock(mhz);
    }
}

/* round to nearest and denormals flushed, as the SH-4 runs (FPSCR 00040001) */
static void fpu_setup(void) {
    uint32_t fcr;
    __asm__ volatile("cfc1 %0, $31" : "=r"(fcr));
    host_log("FCR31 %08lX at the start", (unsigned long)fcr);
    fcr = (fcr & ~0x00000F83u) | 0x01000000u;
    __asm__ volatile("ctc1 %0, $31" : : "r"(fcr));
}

/* the PSP-1000's user memory is 24 MB whatever PARAM.SFO asks for; the 64 MB models give a MEMSIZE=1 program 52 */
static int memory_enough(void) {
    unsigned long have = (unsigned long)psp_mem.free_at_start + (unsigned long)(_end - _ftext);
    host_log("memory: model %d, user memory %lu KB", kuKernelGetModel(), have >> 10);
    return have >= (40ul << 20);
}

static void log_memory(void) {
    SceKernelThreadInfo th;
    int i;
    memset(&th, 0, sizeof th);
    th.size = sizeof th;
    sceKernelReferThreadStatus(sceKernelGetThreadId(), &th);
    host_log("memory: user partition free %lu KB at the start (largest block %lu KB)",
             (unsigned long)psp_mem.free_at_start >> 10, (unsigned long)psp_mem.max_at_start >> 10);
    host_log("  program %08lX-%08lX (%lu KB: code %lu KB)", (unsigned long)(uintptr_t)_ftext,
             (unsigned long)(uintptr_t)_end, (unsigned long)(_end - _ftext) >> 10,
             (unsigned long)(_etext - _ftext) >> 10);
    host_log("  main RAM %08lX-%08lX (%lu KB): %s", (unsigned long)NAOMI_RAM_BASE,
             (unsigned long)(NAOMI_RAM_BASE + NAOMI_RAM_SIZE), (unsigned long)NAOMI_RAM_SIZE >> 10,
             psp_mem.ram_mapped ? "taken" : "not had");
    if (!psp_mem.ram_mapped) {
        host_log("  (the kernel said %08X)", (unsigned)psp_mem.ram_error);
    }
    for (i = 0; i < psp_mem.nheaps; i++) {
        host_log("  heap %08lX-%08lX (%lu KB)", (unsigned long)(uintptr_t)psp_mem.heap[i].base,
                 (unsigned long)(uintptr_t)psp_mem.heap[i].end,
                 (unsigned long)(psp_mem.heap[i].end - psp_mem.heap[i].base) >> 10);
    }
    host_log("  main thread's stack %08lX-%08lX (%lu KB)", (unsigned long)(uintptr_t)th.stack,
             (unsigned long)(uintptr_t)th.stack + th.stackSize, (unsigned long)th.stackSize >> 10);
    host_log("  left free %lu KB (largest block %lu KB)", (unsigned long)sceKernelTotalFreeMemSize() >> 10,
             (unsigned long)sceKernelMaxFreeMemSize() >> 10);
}

/* SHC's division by 0 gives 0 (port/psp/divzero_as.py) */
static void divzero_check(void) {
    volatile int a = 7, z = 0;
    volatile unsigned ua = 7, uz = 0;
    int q = a / z, r = a % z;
    unsigned uq = ua / uz, ur = ua % uz;
    host_log("division by 0: %d %d %u %u%s", q, r, uq, ur, q != 0 || r != 0 || uq != 0 || ur != 0 ? ", not 0 as on the SH-4" : "");
}

/* ---- the options: SELECT+TRIANGLE stops the game, START (or SELECT) goes back to it ---- */

/* MVC2_OPTIONS=frame:keys (tests): the options opened at that frame, given the keys (up down left right start, comma
   between) one each 20 vblanks */
static const char* options_keys;

static uint32_t pad_buttons(void) {
    static const struct {
        const char* name;
        uint32_t key;
    } names[] = { { "up", PSP_CTRL_UP }, { "down", PSP_CTRL_DOWN }, { "left", PSP_CTRL_LEFT },
                  { "right", PSP_CTRL_RIGHT }, { "start", PSP_CTRL_START } };
    static unsigned v;
    SceCtrlData pad;
    size_t i, k;
    if (options_keys != NULL) {
        if (++v % 20 != 0 || *options_keys == 0) {
            return 0;
        }
        k = strcspn(options_keys, ",");
        for (i = 0; i < sizeof names / sizeof *names; i++) {
            if (strlen(names[i].name) == k && strncmp(names[i].name, options_keys, k) == 0) {
                break;
            }
        }
        options_keys += k + (options_keys[k] != 0);
        return i < sizeof names / sizeof *names ? names[i].key : 0;
    }
    sceCtrlPeekBufferPositive(&pad, 1);
    return pad.Buttons;
}

/* the clock set at once, the screen from the next frame; each kept in mvc2.cfg */
static void options(void) {
    enum { ROWS = 2, LINES = 10, W = 368, H = 8 + 12 * LINES + 4, X = (480 - W) / 2, Y = (272 - H) / 2 };
    char line[LINES][48];
    const char* lines[LINES];
    static const char* const screens[3] = { "4:3 with pillars", "4:3", "stretched" };
    int pillars = psp_art_wanted(), screen = stretch ? 2 : pillars ? 0 : 1, screen_was = screen;
    int row = 0, ci = 0, i, shown = 0, n = 0;
    uint32_t last = pad_buttons(), held, down;
    double cpu, bus;
    host_log("options: opened at frame %u", presented);
    psp_log_flush();
    psp_audio_idle();
    psp_render_me_after_wait();
    if (!psp_me_wait(500000)) {
        host_log("options: the ME's job not done in 0.5 s");
    }
    if (!psp_render_idle_within(500000)) {
        host_log("options: the kicker still busy after 0.5 s");
    }
    for (i = 0; i < 25 && sceGeDrawSync(1) > 0; i++) {
        sceKernelDelayThread(20000);    /* the GE's last list, 0.5 s at most */
    }
    if (i == 25) {
        host_log("options: the GE still drawing after 0.5 s");
    }
    psp_log_flush();
    for (i = 0; i < (int)(sizeof clock_steps / sizeof *clock_steps); i++) {
        if (clock_steps[i] == clock_mhz()) {
            ci = i;
        }
    }
    cpu = counted_mhz(20000);
    cpu = cpu > 0 ? cpu : scePowerGetCpuClockFrequencyInt();
    bus = bus_mhz();
    for (i = 0; i < LINES; i++) {
        lines[i] = line[i];
    }
    while (!quit_req) {
        if (!shown || ++n % 30 == 0) {      /* again now and then: a frame the GE was still drawing can cover it */
            snprintf(line[0], sizeof line[0], "Options");
            snprintf(line[1], sizeof line[1], " ");
            snprintf(line[2], sizeof line[2], "%c CPU clock    < %d MHz >", row == 0 ? '>' : ' ', clock_steps[ci]);
            snprintf(line[3], sizeof line[3], "%c Screen       < %s >", row == 1 ? '>' : ' ', screens[screen]);
            snprintf(line[4], sizeof line[4], " ");
            snprintf(line[5], sizeof line[5], "  now: CPU %.0f MHz, bus %.0f MHz", cpu, bus);
            snprintf(line[6], sizeof line[6], "  222: the PSP's own, the longest battery");
            snprintf(line[7], sizeof line[7], "  333: the smoothest, the most battery");
            snprintf(line[8], sizeof line[8], "%s",
                     clock_steps[ci] != clock_now ? "  the clock applies when you restart" : " ");
            snprintf(line[9], sizeof line[9], "UP DOWN LEFT RIGHT, START: back to the game");
            psp_video_panel(X, Y, W, H, lines, LINES, 2 + row);
            shown = 1;
        }
        sceDisplayWaitVblankStart();
        scePowerTick(PSP_POWER_TICK_ALL);
        psp_crumb("options", presented);
        held = pad_buttons();
        down = held & ~last;
        last = held;
        if (down & (PSP_CTRL_START | PSP_CTRL_SELECT)) {
            break;
        }
        if (down & (PSP_CTRL_UP | PSP_CTRL_DOWN)) {
            row = (row + 1) % ROWS;
            shown = 0;
        }
        if (down & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT)) {
            int step = down & PSP_CTRL_LEFT ? -1 : 1;
            if (row == 0) {
                int k = ci + step;
                if (k >= 0 && k < (int)(sizeof clock_steps / sizeof *clock_steps)) {
                    ci = k;     /* kept for the next start: the clock isn't changed while the ME is ours */
                }
            } else {
                screen = (screen + 3 + step) % 3;
                stretch = screen == 2;
                if (screen != 2) {
                    pillars = screen == 0;
                    psp_render_pillars(pillars);
                }
            }
            shown = 0;
        }
    }
    while (!quit_req && (pad_buttons() & (PSP_CTRL_START | PSP_CTRL_SELECT))) {
        sceDisplayWaitVblankStart();     /* let go before the game sees them */
    }
    if (clock_steps[ci] != clock_mhz()) {
        char v[8];
        snprintf(v, sizeof v, "%d", clock_steps[ci]);
        config_set("clock", v);
    }
    if (screen != screen_was) {
        config_set("scale", stretch ? "stretch" : "normal");
        if (!stretch) {
            config_set("pillars", pillars ? "on" : "off");
        }
    }
    if (use_ge && !stretch) {
        psp_render_pillars(pillars);    /* the bars again, where the panel was */
    }
    host_log("options: closed (clock %d MHz, %d from the next start; screen %s)", clock_now, clock_steps[ci],
             screens[screen]);
}

int main(int argc, char** argv) {
    long frames_left;
    char cwd[256];
    char* args[3] = { argc > 0 ? argv[0] : "mvc2", NULL, NULL };

    exit_callbacks();
    psp_log_open(LOG);
    host_log("Marvel vs. Capcom 2, PSP: %s", getcwd(cwd, sizeof cwd) != NULL ? cwd : "?");
    read_env();
    test_run = getenv("MVC2_FRAMES") != NULL || getenv("MVC2_SHOT") != NULL;
    set_defaults();
    psp_paced = !test_run || (getenv("MVC2_PACE") != NULL && atoi(getenv("MVC2_PACE")) != 0);
    psp_log_sync(getenv("MVC2_LOG_SYNC") == NULL || atoi(getenv("MVC2_LOG_SYNC")) != 0);
    if (!memory_enough()) {
        return fail("MvC2 needs a PSP-2000, 3000 or Go (64 MB).");
    }
    psp_crumbs_start(test_run);
    if (test_run) {
        host_log("power: sleep refused in a test run (%08X)", (unsigned)scePowerLock(0));
    }
    psp_log_rescuer_start();
    psp_crash_install();
    psp_crash_test(0);
    psp_crumb("clock", 0);
    clock_setup();
    fpu_setup();
    divzero_check();
    log_memory();
    if (test_run) {
        host_log("coroutine switch: %.1f us (1000 round trips)", coro_psp_bench(1000));
    }
    psp_prof_start();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    if (!naomi_map_ram()) {
        return fail("The game's RAM could not be had at its address: see " LOG ".");
    }
    frames_left = getenv("MVC2_FRAMES") ? atol(getenv("MVC2_FRAMES")) : -1;
    host_divzero_install();
    if (getenv("MVC2_MODE") != NULL) {
        args[1] = strcmp(getenv("MVC2_MODE"), "dc") == 0 ? "--dc" : "--arcade";
    }
    psp_crumb("pick_game", 0);
    if (!pick_game(args[1] != NULL ? 2 : 1, args)) {
        host_log("stopped: no game data");      /* pick_game has said why */
        psp_exit();
        return 1;
    }
    clock_from_config();
    stretch = getenv("MVC2_STRETCH") != NULL ? strcmp(getenv("MVC2_STRETCH"), "0") != 0
                                             : config_get("scale") != NULL && strcmp(config_get("scale"), "stretch") == 0;
    use_ge = getenv("MVC2_RENDERER") != NULL ? strcmp(getenv("MVC2_RENDERER"), "soft") != 0
                                             : config_get("renderer") == NULL || strcmp(config_get("renderer"), "soft") != 0;
    host_log("drawn by %s", use_ge ? "the GE" : "rd_soft");
    nl_tex_sources = use_ge;    /* the GE's textures made from the PAL4 and VQ sources, kept as they are */
    nl_tex_sync = 1;
    if (use_ge) {
        psp_render_start();
    }
    /* rd_soft at half size by default: 320x240 is near the screen's size, and a quarter of the work (render = exact:
       640x480) */
    rd_quality(config_get("render") != NULL ? config_get("render") : "half");   /* MVC2_RDQUALITY overrides it */
    psp_crumb("video", 0);
    /* 16 bits a pixel, dithered, as the DC's own picture, unless MVC2_SCREEN16=0 */
    psp_video_init(getenv("MVC2_SCREEN16") == NULL || atoi(getenv("MVC2_SCREEN16")) != 0);
    host_keep_start();
    rd_begin_frame();
    decide_next();
    psp_crumb("start_game", 0);
    if (!start_game()) {
        return fail("The game could not start: see " LOG ".");
    }
    psp_audio_init();
    /* MVC2_LOG_HOLD=0 (tests): the lines written as they come, so a hang that stops the whole PSP loses none */
    if (feel && (getenv("MVC2_LOG_HOLD") == NULL || atoi(getenv("MVC2_LOG_HOLD")) != 0)) {
        host_log("playing: the log held in memory until the end");
        psp_log_hold(1);
    }
    while (!quit_req) {
        read_input();
        input_read();
        if (switch_req) {
            switch_req = 0;
            psp_audio_idle();
            if (!switch_game()) {
                break;
            }
            rd_begin_frame();
        }
        if (getenv("MVC2_OPTIONS") != NULL && presented == (unsigned)atoi(getenv("MVC2_OPTIONS"))) {
            options_keys = strchr(getenv("MVC2_OPTIONS"), ':') != NULL ? strchr(getenv("MVC2_OPTIONS"), ':') + 1 : "start";
            options_req = 1;
        }
        if (getenv("MVC2_HOME_AT") != NULL && presented == (unsigned)atoi(getenv("MVC2_HOME_AT"))) {
            SceUID th = sceKernelCreateThread("home test", home_test, 0x11, 0x4000, THREAD_ATTR_USER, NULL);
            unsetenv("MVC2_HOME_AT");
            if (th >= 0) {
                sceKernelStartThread(th, 0, NULL);
            }
        }
        if (options_req) {
            options_req = 0;
            options();
            options_keys = NULL;
            unsetenv("MVC2_OPTIONS");
        }
        naomi_run_frame();
        if (frames_left > 0 && --frames_left == 0) {
            break;
        }
    }
    /* the held lines written now, and each line from here written and closed: a hang on the way out keeps them */
    psp_exit_step_us = sceKernelGetSystemTimeLow() | 1u;
    psp_log_hold(0);
    psp_log_sync(1);
    host_log("exit: the game stopped at frame %u", presented);
    if (test_run) {
        scePowerUnlock(0);
    }
    psp_render_end();
    psp_exit_step_us = sceKernelGetSystemTimeLow() | 1u;
    psp_audio_exit();
    psp_exit_step_us = sceKernelGetSystemTimeLow() | 1u;
    host_stub_report();
    host_log("heap in use %lu KB, free %lu KB", (unsigned long)psp_mem_in_use() >> 10,
             (unsigned long)psp_mem_free() >> 10);
    fl_write();
    host_log("exit after %u frames", presented);
    psp_exit();
    return 0;
}
