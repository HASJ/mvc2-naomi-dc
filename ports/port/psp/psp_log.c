/*
 * psp_log.c  The log, each line written and closed as it is made so that a crash keeps it, or held in memory while
 * the game plays and written at the end; the breadcrumbs that say how far the program got; and a watchdog that
 * ends a run that has stopped
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspintrman.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <pspthreadman.h>

#include "naomi_host.h"
#include "psp_front.h"

static char log_path[256];
static SceUID log_fd = -1;          /* held open with MVC2_LOG_SYNC=0, else opened for each line */
static char held[64 << 10];         /* psp_log_hold's lines, and how many were left out when it filled */
static int nheld, holding, held_over;
#define HELD_RESERVE (12 << 10)     /* the room kept for a crash's or a hang's report */
static int reserve_open;

/* the bytes written, or the kernel's error */
int psp_log_write(const char* s, int n) {
    SceUID fd;
    int r;
    if (log_path[0] == 0 || n <= 0) {
        return 0;
    }
    if (holding) {
        int k = sceKernelCpuSuspendIntr();
        if (nheld + n <= (int)sizeof held - (reserve_open ? 0 : HELD_RESERVE)) {
            memcpy(held + nheld, s, (size_t)n);
            nheld += n;
        } else {
            held_over++;
        }
        sceKernelCpuResumeIntr(k);
        return n;
    }
    if (log_fd >= 0) {
        return sceIoWrite(log_fd, s, n);
    }
    fd = sceIoOpen(log_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd < 0) {
        return fd;
    }
    r = sceIoWrite(fd, s, n);
    sceIoClose(fd);
    return r;
}

/* host_log's lines (naomi_board.c), formatted on the caller's stack: no lock, so any thread can */
void psp_log_line(const char* fmt, va_list ap) {
    char line[1024];
    int n = vsnprintf(line, sizeof line - 1, fmt, ap);
    if (n < 0) {
        return;
    }
    if (n > (int)sizeof line - 2) {
        n = sizeof line - 2;
    }
    line[n++] = '\n';
    psp_log_write(line, n);
}

static int stderr_write(void* cookie, const char* buf, int n) {
    (void)cookie;
    psp_log_write(buf, n);
    return n;
}

/* the log emptied, in the program's folder; stderr goes there too */
void psp_log_open(const char* name) {
    char cwd[200];
    SceUID fd;
    if (getcwd(cwd, sizeof cwd) != NULL) {
        snprintf(log_path, sizeof log_path, "%s/%s", cwd, name);
    } else {
        snprintf(log_path, sizeof log_path, "%s", name);
    }
    fd = sceIoOpen(log_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) {
        sceIoClose(fd);
    }
    stderr = funopen(NULL, NULL, stderr_write, NULL, NULL);
    if (stderr != NULL) {
        setvbuf(stderr, NULL, _IOLBF, 1024);
    }
}

/* MVC2_LOG_SYNC=0: the file held open, faster, but a crash may lose its last lines */
void psp_log_sync(int on) {
    if (!on && log_fd < 0) {
        log_fd = sceIoOpen(log_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    } else if (on && log_fd >= 0) {
        sceIoClose(log_fd);
        log_fd = -1;
    }
}

/* with on, the lines kept in memory, so nothing is written to the memory stick while the game plays; with off,
   those written out and each line written again as it comes */
void psp_log_hold(int on) {
    if (on) {
        holding = 1;
        return;
    }
    if (holding) {
        char note[64];
        holding = 0;
        psp_log_write(held, nheld);
        if (held_over != 0) {
            psp_log_write(note, snprintf(note, sizeof note, "(%d lines left out: the held log was full)\n", held_over));
        }
        nheld = held_over = 0;
    }
}

/* the held lines written out now and the file closed, the log still held: from any thread (the lines taken with
   interrupts off, as they are added) */
void psp_log_flush(void) {
    static char out[sizeof held];
    SceUID fd;
    int n, k;
    if (!holding || log_path[0] == 0) {
        return;
    }
    k = sceKernelCpuSuspendIntr();
    n = nheld;
    memcpy(out, held, (size_t)n);
    nheld = 0;
    sceKernelCpuResumeIntr(k);
    fd = sceIoOpen(log_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, out, n);
        sceIoClose(fd);
    }
}

/* text written and the file closed now, with the lines held before it: for an event the PSP may not come back
   from (a suspend); the lines after it are held again if they were */
void psp_log_now(const char* text) {
    int was = holding;
    psp_log_hold(0);
    psp_log_sync(1);
    host_log("%s", text);
    holding = was;
}

/* a crash's or a hang's lines from here held in memory, with the room kept for them */
void psp_log_report(void) {
    reserve_open = 1;
    holding = 1;
}

/* the held log and report written by who, each step on the screen, and the file closed: its length on the memory
   stick is set only by the close, and a program ended without it loses all it wrote through the open file */
static int report_write(const char* who) {
    char note[64];
    SceUID fd = log_fd;
    int r, c;
    log_fd = -1;
    if (fd < 0) {
        fd = sceIoOpen(log_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
        pspDebugScreenPrintf("%s: open %08X\n", who, (unsigned)fd);
        if (fd < 0) {
            return fd;
        }
    }
    r = nheld > 0 ? sceIoWrite(fd, held, nheld) : 0;
    if (r >= 0 && held_over != 0) {
        sceIoWrite(fd, note, snprintf(note, sizeof note, "(%d lines left out: the held log was full)\n", held_over));
    }
    c = sceIoClose(fd);
    pspDebugScreenPrintf("%s: %d bytes, write %08X, close %08X\n", who, nheld, (unsigned)r, (unsigned)c);
    if (r < 0 || c < 0) {
        return r < 0 ? r : c;
    }
    nheld = held_over = 0;
    return r;
}

/* the failing context may not reach the file system (an interrupt, interrupts off, the kernel inside a call), so a
   thread made at start-up, above all the others, waits to write the held log for it */
static SceUID rescue_sema = -1, rescued_sema = -1;
static volatile int rescued;
static int intr_on = 1;             /* the interrupt state sceKernelCpuSuspendIntr gives while they are on */

static int rescuer(SceSize args, void* argp) {
    (void)args;
    (void)argp;
    sceKernelWaitSema(rescue_sema, 1, NULL);
    pspDebugScreenPrintf("the log's writer running\n");
    rescued = report_write("the writer");
    sceKernelSignalSema(rescued_sema, 1);
    return 0;
}

void psp_log_rescuer_start(void) {
    SceUID th;
    intr_on = sceKernelCpuSuspendIntr();
    sceKernelCpuResumeIntr(intr_on);
    rescue_sema = sceKernelCreateSema("log rescue", 0, 0, 1, NULL);
    rescued_sema = sceKernelCreateSema("log rescued", 0, 0, 1, NULL);
    th = sceKernelCreateThread("log rescuer", rescuer, 0x0C, 0x4000, THREAD_ATTR_USER, NULL);
    if (rescue_sema < 0 || rescued_sema < 0 || th < 0 || sceKernelStartThread(th, 0, NULL) < 0) {
        host_log("log rescuer: not made (%08X %08X %08X)", (unsigned)rescue_sema, (unsigned)rescued_sema,
                 (unsigned)th);
        rescue_sema = -1;
    }
    host_log("log rescuer: interrupts on read as %08X", (unsigned)intr_on);
}

/* interrupts on again in a thread that failed with them off, so that the screen, the waits and the file system
   work; 1 when they were off. Inside an interrupt they stay as they are */
int psp_intr_back(void) {
    int was = sceKernelCpuSuspendIntr();
    sceKernelCpuResumeIntr(sceKernelGetThreadId() >= 0 ? intr_on : was);
    return was != intr_on;
}

/* the held log written, then then(the write's result or the kernel's error), which ends the program. Tried here
   first; where this context can't, by the rescuer, waited for 10 s at most. From an interrupt there is no way to
   the file system */
void psp_log_rescue(void (*then)(int r)) {
    SceUInt wait = 10 * 1000 * 1000;
    int r;
    if (sceKernelGetThreadId() < 0) {
        then((int)0x80020064);
        return;
    }
    pspDebugScreenPrintf("%s; writing the log here\n", psp_intr_back() ? "interrupts on again" : "interrupts on");
    r = report_write("here");
    if (r >= 0 || rescue_sema < 0) {
        then(r);
        return;
    }
    pspDebugScreenPrintf("waking the log's writer\n");
    r = sceKernelSignalSema(rescue_sema, 1);
    if (r >= 0) {
        r = sceKernelWaitSema(rescued_sema, 1, &wait);
        if (r < 0) {
            pspDebugScreenPrintf("the writer: no answer in 10 s (%08X)\n", (unsigned)r);
        }
    }
    then(r < 0 ? r : rescued);
}

void psp_log_close(void) {
    if (stderr != NULL) {
        fflush(stderr);
    }
    psp_log_hold(0);
    psp_log_sync(1);
}

/* ---- breadcrumbs ---- */

#define CRUMBS 32

static struct {
    const char* what;
    unsigned n;
    uint32_t ms;
} crumb[CRUMBS];
static volatile unsigned ncrumbs;
static int crumbs_logged;       /* MVC2_CRUMBS=1: each in the log as well */

static uint32_t ms_now(void) {
    return (uint32_t)(sceKernelGetSystemTimeWide() / 1000);
}

void psp_crumb(const char* what, unsigned n) {
    unsigned i = ncrumbs % CRUMBS;
    crumb[i].what = what;
    crumb[i].n = n;
    crumb[i].ms = ms_now();
    ncrumbs++;
    if (crumbs_logged) {
        host_log("at %s %u (%lu ms)", what, n, (unsigned long)crumb[i].ms);
    }
}

/* each frame in the ring; in the log the first 60 and then every 60th */
void psp_crumb_frame(unsigned frame) {
    int logged = crumbs_logged;
    crumbs_logged = logged && (frame <= 60 || frame % 60 == 0);
    psp_crumb("frame", frame);
    crumbs_logged = logged;
}

/* the last crumbs, oldest first, a line each */
void psp_crumbs_dump(void (*line)(const char* text), unsigned most) {
    unsigned i, from;
    most = most < CRUMBS ? most : CRUMBS;
    from = ncrumbs > most ? ncrumbs - most : 0;
    for (i = from; i < ncrumbs; i++) {
        char text[96];
        snprintf(text, sizeof text, "  %s %u at %lu ms", crumb[i % CRUMBS].what, crumb[i % CRUMBS].n,
                 (unsigned long)crumb[i % CRUMBS].ms);
        line(text);
    }
}

static void log_text(const char* text) {
    host_log("%s", text);
}

static void screen_text(const char* text) {
    pspDebugScreenPrintf("%s\n", text);
}

/* ---- the watchdog ---- */

extern uint32_t psp_crash_hits;

static int watch_seconds;

/* MVC2_BEAT=1: a box at the top right that changes colour twice a second from an alarm (stops only if interrupts
   do), and below it from the watchdog the frame, the seconds without a crumb and the reader's state */
static int beat_on;
static volatile uint32_t beats;

static SceUInt beat(void* p) {
    (void)p;
    beats++;
    psp_video_box(470, 0, 10, 10, beats & 1 ? 0xFF00FF00u : 0xFFFF0000u);
    return 500000;
}

static void beat_number(int* x, unsigned v, int digits) {
    static const uint16_t font[10] = { 075557, 026227, 071747, 071717, 055711, 074717, 074757, 071111, 075757, 075717 };
    int d, r, c;
    for (d = digits - 1; d >= 0; d--) {
        unsigned k = v;
        int n;
        for (n = 0; n < d; n++) {
            k /= 10;
        }
        for (r = 0; r < 5; r++) {
            for (c = 0; c < 3; c++) {
                if (font[k % 10] >> ((4 - r) * 3 + 2 - c) & 1) {
                    psp_video_box(*x + c * 2, 13 + r * 2, 2, 2, 0xFFFFFFFFu);
                }
            }
        }
        *x += 8;
    }
    *x += 6;
}

static void beat_say(unsigned still) {
    int x = 402;
    psp_video_box(x - 2, 11, 80, 14, 0xFF000000u);
    beat_number(&x, host_frame(), 5);
    beat_number(&x, still, 2);
    beat_number(&x, (unsigned)psp_reader_doing, 1);
}

static const char* wait_name(int type) {
    static const char* const names[] = { "nothing", "sleep", "delay", "semaphore", "event flag", "mailbox", "VPL",
                                         "FPL", "message pipe", "thread end", "event handler", "callback", "mutex",
                                         "lightweight mutex" };
    return type >= 0 && type < (int)(sizeof names / sizeof *names) ? names[type] : "?";
}

/* the screen for a report: the GE stopped first, as a list gone wrong may still be writing to memory */
void psp_screen_take(void) {
    PspGeBreakParam b;
    sceGeBreak(1, &b);
    pspDebugScreenInit();
    /* now, not at a vblank that interrupts left off would never bring */
    sceDisplaySetFrameBuf((void*)(0x40000000u | (uintptr_t)sceGeEdramGetAddr()), 512, PSP_DISPLAY_PIXEL_FORMAT_8888,
                          PSP_DISPLAY_SETBUF_IMMEDIATE);
}

/* each of the program's threads, a line each: what it waits for (the kernel's own left out) */
static void threads_said(void (*line)(const char* text)) {
    SceUID id[32];
    int n = 0, i;
    if (sceKernelGetThreadmanIdList(SCE_KERNEL_TMID_Thread, id, 32, &n) < 0) {
        return;
    }
    for (i = 0; i < n && i < 32; i++) {
        SceKernelThreadInfo th;
        char text[96];
        memset(&th, 0, sizeof th);
        th.size = sizeof th;
        if (sceKernelReferThreadStatus(id[i], &th) < 0 || !(th.attr & PSP_THREAD_ATTR_USER)) {
            continue;
        }
        if (th.status & PSP_THREAD_WAITING) {
            snprintf(text, sizeof text, "  %-16.16s waiting for %s (%08X)", th.name, wait_name(th.waitType),
                     (unsigned)th.waitId);
        } else {
            snprintf(text, sizeof text, "  %-16.16s %s (status %d)", th.name,
                     th.status & (PSP_THREAD_RUNNING | PSP_THREAD_READY) ? "running" : "stopped", th.status);
        }
        line(text);
    }
}

/* above the game's priority: wakes each second, and when no crumb has come for watch_seconds says what each
   thread is doing, then ends the run */
static int watchdog(SceSize args, void* argp) {
    unsigned seen = ncrumbs, still = 0;
    (void)args;
    (void)argp;
    for (;;) {
        sceKernelDelayThread(1000000);
        if (ncrumbs != seen) {
            seen = ncrumbs;
            still = 0;
        } else {
            still++;
        }
        if (beat_on) {
            beat_say(still);
        }
        if (still < (unsigned)watch_seconds) {
            continue;
        }
        {
            /* on the screen first: a hang in the file system would stop the log too */
            psp_screen_take();
            pspDebugScreenPrintf("Marvel vs. Capcom 2: stopped for %d s at frame %u\n", watch_seconds, host_frame());
            threads_said(screen_text);
            pspDebugScreenPrintf("the last crumbs:\n");
            psp_crumbs_dump(screen_text, 6);
            pspDebugScreenPrintf("writing mvc2.log...\n");
            psp_log_report();
            host_log("watchdog: nothing for %d s at frame %u; the threads:", watch_seconds, host_frame());
            threads_said(log_text);
            host_log("watchdog: the crash handler ran %u times", (unsigned)psp_crash_hits);
            host_log("watchdog: the last crumbs:");
            psp_crumbs_dump(log_text, CRUMBS);
            host_log("watchdog: the run ends (the memory card is not saved)");
            psp_log_rescue(psp_report_written);
        }
    }
    return 0;
}

/* MVC2_CRUMBS=1 logs the crumbs; MVC2_WATCHDOG=n (s, 0 for none) watches for them, by default in a test run */
static void host_crumb_psp(const char* what) {
    psp_crumb(what, host_frame());
}

void psp_crumbs_start(int test_run) {
    const char* w = getenv("MVC2_WATCHDOG");
    crumbs_logged = getenv("MVC2_CRUMBS") != NULL && atoi(getenv("MVC2_CRUMBS")) != 0;
    host_crumb = host_crumb_psp;
    watch_seconds = w != NULL ? atoi(w) : test_run ? 20 : 0;
    beat_on = getenv("MVC2_BEAT") != NULL && atoi(getenv("MVC2_BEAT")) != 0;
    if (beat_on) {
        host_log("beat: the alarm %08X", (unsigned)sceKernelSetAlarm(500000, beat, NULL));
        if (watch_seconds <= 0) {
            watch_seconds = 9999;
        }
    }
    if (watch_seconds > 0) {
        /* above the ME's threads (0x10) and the rest, below the log's rescuer */
        SceUID th = sceKernelCreateThread("watchdog", watchdog, 0x0E, 0x4000, THREAD_ATTR_USER, NULL);
        if (th >= 0) {
            th = sceKernelStartThread(th, 0, NULL);
        }
        host_log("watchdog: the run ends after %d s without a crumb (%08X)", watch_seconds, (unsigned)th);
    }
}
