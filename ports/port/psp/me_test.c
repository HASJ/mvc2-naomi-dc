/*
 * me_test.c  A test of the Media Engine worker (psp_me.c) on the PSP: what the list build there needs (its FPU, our
 * memory, its speed on our C, its effect on the main CPU's memory) and sleep and HOME with the ME ours. Its own
 * EBOOT; the results on the screen and in me.log beside it.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pspctrl.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspkernel.h>
#include <psputils.h>

#include "psp_me.h"

PSP_MODULE_INFO("mvc2me", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(4096);

#define CPU_UNCACHED(p) ((const volatile uint32_t*)((uintptr_t)(p) | 0x40000000u))

static void say(const char* fmt, ...) {
    char line[256];
    FILE* f;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    pspDebugScreenPrintf("%s\n", line);
    f = fopen("me.log", "a");
    if (f != NULL) {
        fprintf(f, "%s\n", line);
        fclose(f);
    }
}

static uint32_t now(void) {
    return sceKernelGetSystemTimeLow();
}

/* one job on the ME: its time from post to done in microseconds, 0 if it didn't answer within us */
static uint32_t on_me(void (*f)(void*), void* arg, uint32_t us) {
    uint32_t t0 = now();
    if (!psp_me_post(f, arg) || !psp_me_wait(us)) {
        return 0;
    }
    return now() - t0 ? now() - t0 : 1;
}

/* the ME back after a job that never answered */
static int again(void) {
    psp_me_stop(0);
    return psp_me_start();
}

static void nothing(void* p) {
    (void)p;
}

/* our memory, written through its own address */
static volatile uint32_t poke[16] __attribute__((aligned(64)));

static void set_poke(void* p) {
    (void)p;
    poke[0] = 0x600DF00Du;
}

/* cache 24 (create dirty exclusive): line 0 made and filled, line 1 made and half filled, line 2 filled without */
static uint32_t cde[3][16] __attribute__((aligned(64)));

static void cde_job(void* p) {
    uint32_t i, k = (uint32_t)(uintptr_t)p;
    __asm__ volatile("cache 24, 0(%0)" : : "r"(cde[0]) : "memory");
    __asm__ volatile("cache 24, 0(%0)" : : "r"(cde[1]) : "memory");
    for (i = 0; i < 16; i++) {
        cde[0][i] = k + i;
        cde[2][i] = k + 32 + i;
    }
    for (i = 0; i < 8; i++) {
        cde[1][i] = k + 16 + i;
    }
}

static void test_cde(const char* who, int me) {
    volatile uint32_t* u = (volatile uint32_t*)((uintptr_t)cde | 0x40000000u);
    uint32_t i, k = me ? 0x5E000000u : 0x3A000000u, bad0 = 0, bad2 = 0, half = 0, old = 0;
    memset(cde, 0xAA, sizeof cde);
    sceKernelDcacheWritebackInvalidateAll();
    if (me) {
        if (!on_me(cde_job, (void*)(uintptr_t)k, 100000)) {
            say("  cache 24 on the ME: the job didn't answer");
            again();
            return;
        }
    } else {
        cde_job((void*)(uintptr_t)k);
        sceKernelDcacheWritebackInvalidateAll();
    }
    for (i = 0; i < 16; i++) {
        bad0 += u[i] != k + i;
        bad2 += u[32 + i] != k + 32 + i;
    }
    for (i = 0; i < 8; i++) {
        half += u[16 + i] != k + 16 + i;
        old += u[24 + i] == 0xAAAAAAAAu;
    }
    say("  cache 24 %s: made and filled %u wrong; half filled %u wrong, %u of 8 unwritten words kept; plain %u wrong",
        who, bad0, half, old, bad2);
    say("    the unwritten half: %08X %08X %08X %08X", (unsigned)u[24], (unsigned)u[25], (unsigned)u[26],
        (unsigned)u[27]);
}

/* the FPU: each operation over the same inputs, the results kept as bits */
#define FN 512
#define FOPS 8
static float fin[FN];
static uint32_t fout_cpu[FOPS][FN], fout_me[FOPS][FN] __attribute__((aligned(64)));

static void fpu(void* p) {
    uint32_t(*o)[FN] = p;
    int i;
    for (i = 0; i < FN; i++) {
        float a = fin[i], b = fin[(i * 7 + 3) % FN], r;
        int w;
        r = a + b;
        memcpy(&o[0][i], &r, 4);
        r = a * b;
        memcpy(&o[1][i], &r, 4);
        r = b > 1e-20f || b < -1e-20f ? a / b : 0.0f;
        memcpy(&o[2][i], &r, 4);
        r = a < 0.0f ? -a : a;
        __asm__("sqrt.s %0, %1" : "=f"(r) : "f"(r));
        memcpy(&o[3][i], &r, 4);
        w = a > -2e9f && a < 2e9f ? (int)a : 0;
        o[4][i] = (uint32_t)w;
        r = (float)(int)(i * 2654435761u);
        memcpy(&o[5][i], &r, 4);
        o[6][i] = (a < b) | (a == b) << 1 | (a <= b) << 2;
        r = a * 0.5f - b * 1.25f + 3.0f;
        memcpy(&o[7][i], &r, 4);
    }
}

/* our memory: a pattern written and read back through plain pointers */
typedef struct __attribute__((aligned(64))) {
    uint32_t* at[3];
    uint32_t words, seed, sum[3];
} PLACES;

static void places(void* p) {
    PLACES* w = p;
    int k;
    uint32_t i;
    for (k = 0; k < 3; k++) {
        uint32_t s = 0;
        if (w->at[k] == NULL) {
            continue;
        }
        for (i = 0; i < w->words; i++) {
            w->at[k][i] = w->seed ^ (i * 2654435761u) ^ (uint32_t)k;
        }
        for (i = 0; i < w->words; i++) {
            s += w->at[k][i];
        }
        w->sum[k] = s;
    }
}

/* a piece of real work: a sort through qsort (a call through a pointer) and a scattered gather */
#define SORTN 8192
#define GSRC (128 * 1024)
#define GOUT (64 * 1024)
static float sort_in[SORTN], sort_cpu[SORTN], sort_me[SORTN] __attribute__((aligned(64)));
static float gsrc[GSRC], gout_cpu[GOUT], gout_me[GOUT] __attribute__((aligned(64)));

typedef struct {
    float *sorted, *out;
} WORK;

static int fcmp(const void* a, const void* b) {
    float x = *(const float*)a, y = *(const float*)b;
    return x < y ? -1 : x > y;
}

static void work(void* p) {
    WORK* w = p;
    uint32_t i;
    memcpy(w->sorted, sort_in, sizeof sort_in);
    qsort(w->sorted, SORTN, sizeof(float), fcmp);
    for (i = 0; i < GOUT; i++) {
        uint32_t k = (i * 2654435761u >> 7) % GSRC;
        w->out[i] = gsrc[k] * 1.5f + gsrc[(k + 7) % GSRC] * gsrc[(k + 4093) % GSRC];
    }
}

/* the main CPU's copy, while the ME is in some state */
#define CBUF (256 * 1024)
static uint8_t csrc[CBUF] __attribute__((aligned(64))), cdst[CBUF] __attribute__((aligned(64)));

static uint32_t copy_time(int times) {
    uint32_t t0 = now();
    int k;
    for (k = 0; k < times; k++) {
        memcpy(cdst, csrc, CBUF);
        sceKernelDcacheWritebackRange(cdst, CBUF);
    }
    return now() - t0;
}

static void copies(const char* what) {
    uint32_t t = copy_time(8);
    say("  the main CPU's 8 copies of 256 KB, %s: %u us, %.1f MB/s", what, t, 8.0 * CBUF / t);
}

static uint32_t buttons(void) {
    SceCtrlData c;
    sceCtrlPeekBufferPositive(&c, 1);
    return c.Buttons;
}

static int x_pressed(void) {
    return (buttons() & PSP_CTRL_CROSS) != 0;
}

static void wait_x_up(void) {
    while (x_pressed()) {
        sceKernelDelayThread(10000);
    }
}

static int test_fpu(void) {
    int i, k, bad = 0;
    uint32_t t;
    for (i = 0; i < FN; i++) {
        /* exponents 2^-27 to 2^36 and every 16th a signed 0: nothing overflows or goes denormal, which the main
           CPU traps on */
        uint32_t b = (uint32_t)i * 2654435761u;
        b = i % 16 == 0 ? b & 0x80000000u : (b & 0x807FFFFFu) | (100u + (b >> 23 & 63u)) << 23;
        memcpy(&fin[i], &b, 4);
    }
    say("  on the main CPU...");
    fpu(fout_cpu);
    say("  on the ME...");
    memset(fout_me, 0, sizeof fout_me);
    t = on_me(fpu, fout_me, 1000000);
    if (t == 0) {
        say("  the FPU test didn't answer in 1 s");
        return again();
    }
    sceKernelDcacheInvalidateRange(fout_me, sizeof fout_me);
    for (k = 0; k < FOPS; k++) {
        int b = 0;
        for (i = 0; i < FN; i++) {
            b += fout_cpu[k][i] != fout_me[k][i];
        }
        if (b) {
            say("  FPU operation %d: %d of %d results differ (first at %d)", k, b, FN, i);
        }
        bad += b;
    }
    say("  the FPU: %d of %d results differ from the main CPU's, in %u us", bad, FN * FOPS, t);
    return 1;
}

static int test_places(void) {
    static uint32_t ours[4096] __attribute__((aligned(64)));
    static PLACES w __attribute__((aligned(64)));
    SceUID lo, hi;
    int k;
    memset(&w, 0, sizeof w);
    lo = sceKernelAllocPartitionMemory(2, "me lo", PSP_SMEM_Addr, 64 * 1024, (void*)0x0A000000);
    hi = sceKernelAllocPartitionMemory(2, "me hi", PSP_SMEM_High, 64 * 1024, NULL);
    w.at[0] = ours;
    w.at[1] = lo >= 0 ? sceKernelGetBlockHeadAddr(lo) : NULL;
    w.at[2] = hi >= 0 ? sceKernelGetBlockHeadAddr(hi) : NULL;
    w.words = 4096;
    w.seed = 0x13579BDFu;
    for (k = 0; k < 3; k++) {
        if (w.at[k] != NULL) {
            memset(w.at[k], 0, 4096 * 4);
        }
    }
    if (on_me(places, &w, 1000000) == 0) {
        say("  plain pointers: no answer in 1 s");
        return again();
    }
    sceKernelDcacheInvalidateRange(&w, sizeof w);
    for (k = 0; k < 3; k++) {
        uint32_t i, bad = 0, s = 0;
        const volatile uint32_t* u;
        if (w.at[k] == NULL) {
            say("  memory %d: none to be had", k);
            continue;
        }
        u = CPU_UNCACHED(w.at[k]);
        for (i = 0; i < w.words; i++) {
            uint32_t want = w.seed ^ (i * 2654435761u) ^ (uint32_t)k;
            bad += u[i] != want;
            s += want;
        }
        say("  plain pointer %08X: %u words wrong in RAM, the ME's sum %s", (unsigned)(uintptr_t)w.at[k], bad,
            w.sum[k] == s ? "right" : "wrong");
    }
    if (lo >= 0) {
        sceKernelFreePartitionMemory(lo);
    }
    if (hi >= 0) {
        sceKernelFreePartitionMemory(hi);
    }
    return 1;
}

static int test_work(void) {
    WORK c = { sort_cpu, gout_cpu }, m = { sort_me, gout_me };
    uint32_t i, t0, tc, tm, tb, bad = 0;
    for (i = 0; i < SORTN; i++) {
        sort_in[i] = (float)(int)(i * 2654435761u >> 8);
    }
    for (i = 0; i < GSRC; i++) {
        gsrc[i] = (float)(i % 1000) * 0.25f;
    }
    sceKernelDcacheWritebackInvalidateAll();
    t0 = now();
    work(&c);
    tc = now() - t0;
    sceKernelDcacheWritebackInvalidateAll();
    tm = on_me(work, &m, 2000000);
    if (tm == 0) {
        say("  the work (qsort and a gather) didn't answer in 2 s");
        return again();
    }
    sceKernelDcacheInvalidateRange(sort_me, sizeof sort_me);
    sceKernelDcacheInvalidateRange(gout_me, sizeof gout_me);
    bad = memcmp(sort_cpu, sort_me, sizeof sort_me) != 0;
    bad += memcmp(gout_cpu, gout_me, sizeof gout_me) != 0;
    say("  the work: the main CPU %u us, the ME %u us, results %s", tc, tm, bad ? "DIFFER" : "the same");
    /* both at once: the ME's work while the main CPU copies */
    t0 = now();
    psp_me_post(work, &m);
    tb = copy_time(8);
    if (!psp_me_wait(2000000)) {
        say("  the work beside copies didn't answer");
        return again();
    }
    tm = now() - t0;
    say("  at once: the ME's work done after %u us; the main CPU's copies %u us (%.1f MB/s)", tm, tb,
        8.0 * CBUF / tb);
    return 1;
}

static void test_pings(const char* how) {
    uint32_t i, t, total = 0, worst = 0;
    for (i = 0; i < 1000; i++) {
        t = on_me(nothing, NULL, 100000);
        if (t == 0) {
            say("  %s: ping %u unanswered", how, i);
            again();
            return;
        }
        total += t;
        worst = t > worst ? t : worst;
    }
    say("  %s: 1000 empty jobs, %.1f us each on average, %u us at worst", how, total / 1000.0, worst);
}

static int circle_pressed(void) {
    return (buttons() & PSP_CTRL_CIRCLE) != 0;
}

/* a step's instructions on a clear screen, held there until X */
static void prompt(const char* title, const char* what) {
    pspDebugScreenClear();
    pspDebugScreenSetXY(0, 8);
    say("%s", title);
    say(" ");
    say("%s", what);
    say(" ");
    say("Press X to begin.");
    wait_x_up();
    while (!x_pressed()) {
        sceKernelDelayThread(10000);
    }
    wait_x_up();
    pspDebugScreenClear();
    pspDebugScreenSetXY(0, 8);
}

static void test_home(void) {
    uint32_t t0 = now(), last = now(), n = 0, missed = 0;
    prompt("STEP 1 of 3: the HOME menu, with the ME ours.",
           "Press HOME, wait for its menu, choose to go back here, then press X.");
    say("Press HOME now. Back here, press X. (60 s at most)");
    while (now() - t0 < 60000000u && !x_pressed()) {
        if (on_me(nothing, NULL, 100000)) {
            n++;
        } else {
            missed++;
            again();
        }
        if (now() - last > 5000000u) {
            last = now();
            say("  %u jobs answered, %u missed", n, missed);
        }
        sceKernelDelayThread(1000);
    }
    say("  HOME done: %u jobs answered, %u missed", n, missed);
    wait_x_up();
}

static void test_sleep(void) {
    uint32_t t0, slept = 0;
    prompt("STEP 3 of 3: sleep, the ME given back for it.",
           "Slide the power switch down to sleep the PSP. Wait 5 s, then slide it again to wake it.");
    say("Slide the power switch down now. (O skips; 60 s at most)");
    t0 = now();
    while (now() - t0 < 60000000u && !circle_pressed()) {
        int was = psp_me_ours(), is = psp_me_frame();
        if (was && !is) {
            if (psp_me_given_back() < 0) {
                say("  the sleep asked for, but the firmware's ME didn't answer (%d): the PSP is kept awake",
                    psp_me_given_back());
                return;
            }
            say("  the sleep asked for: the ME given back to the firmware, which answered in %d us; sleeping",
                psp_me_given_back());
            slept = 1;
        }
        if (!was && is) {
            say("  woken: the ME ours again");
            test_pings("after waking");
            return;
        }
        sceKernelDelayThread(16000);
    }
    say(slept ? "  no wake seen" : "  skipped");
    if (!psp_me_ours()) {
        psp_me_start();
    }
}

/* the fallback: the ME kept and the power switch held off, so a sleep asked for is refused and the game goes on */
static void test_refuse(void) {
    uint32_t t0, n = 0, missed = 0, last;
    psp_me_refuse_sleep(1);
    prompt("STEP 2 of 3: sleep refused, with the ME ours.",
           "Slide the power switch down as if to sleep. The PSP should stay awake and this keep counting. Then press X.");
    say("Slide the power switch down now, then press X. (60 s at most)");
    t0 = now();
    last = t0;
    while (now() - t0 < 60000000u && !x_pressed()) {
        psp_me_frame();
        if (on_me(nothing, NULL, 100000)) {
            n++;
        } else {
            missed++;
            again();
        }
        if (now() - last > 2000000u) {
            last = now();
            say("  awake: %u jobs answered, %u missed, %d sleeps refused", n, missed, psp_me_sleeps_refused());
        }
        sceKernelDelayThread(1000);
    }
    say("  refusing done: %u jobs answered, %u missed, %d sleeps refused", n, missed, psp_me_sleeps_refused());
    psp_me_refuse_sleep(0);
    wait_x_up();
}

static void say_words(const char* when) {
    uint32_t w[24];
    psp_me_fw_words(w);
    say("  the firmware's words %s: 600: %08X %08X %08X %08X %08X .. %08X %08X", when, (unsigned)w[0],
        (unsigned)w[1], (unsigned)w[2], (unsigned)w[3], (unsigned)w[4], (unsigned)w[10], (unsigned)w[11]);
    say("    700: %08X %08X %08X %08X %08X %08X %08X %08X", (unsigned)w[12], (unsigned)w[13], (unsigned)w[14],
        (unsigned)w[15], (unsigned)w[16], (unsigned)w[17], (unsigned)w[18], (unsigned)w[19]);
}

static const char* halt_said(int r) {
    return r == 0 ? "saved and halted, as before a sleep" : r == -1 ? "busy, so taken cold" :
           r == -2 ? "asked to halt and never said so" : "?";
}

static void say_fw(void) {
    const PSP_ME_FW* f = psp_me_fw();
    static const char* names[4] = {"save address (pspsdk's name)", "save address (ARK's name)",
                                   "save size (pspsdk's name)", "save size (ARK's name)"};
    int i;
    say("  the firmware's ME sleep: interrupt to the ME at %08X, DDR flush at %08X", (unsigned)f->to_other,
        (unsigned)f->ddr_flush);
    for (i = 0; i < 4; i++) {
        say("    %s at %08X: %08X %08X %08X %08X, %s %08X", names[i], (unsigned)f->getter_at[i],
            (unsigned)f->getter_code[i][0], (unsigned)f->getter_code[i][1], (unsigned)f->getter_code[i][2],
            (unsigned)f->getter_code[i][3], f->getter_ok[i] ? "gave" : "not called", (unsigned)f->getter_gave[i]);
    }
    say("    the ME's memory saved at %08X, %u KB (names %X); start code %08X %08X, its image at %08X begins %08X%s",
        (unsigned)f->save_at, (unsigned)(f->save_size >> 10), (unsigned)f->pair, (unsigned)f->boot_hi,
        (unsigned)f->boot_lo, (unsigned)f->image_at, (unsigned)f->image_word,
        f->image_word == 0x45344C4Bu ? " (KL4E: whole)" : " (not KL4E: overwritten)");
    say("    AVC bits %08X; %s", (unsigned)f->avc,
        f->can_halt ? halt_said(f->halt) : "the halt left out (something not found), so taken cold");
}

static const char* call_said(int r) {
    return r == 1 ? "answered" : r == -2 ? "the call not found" : r == -3 ? "no thread for it" :
           r == -4 ? "no answer in 0.5 s" : r == -1 ? "its code didn't start" : "?";
}

static int exit_cb(int a, int b, void* c) {
    (void)a;
    (void)b;
    (void)c;
    psp_me_stop(1);
    sceKernelExitGame();
    return 0;
}

static int cb_thread(SceSize n, void* p) {
    (void)n;
    (void)p;
    sceKernelRegisterExitCallback(sceKernelCreateCallback("exit", exit_cb, NULL));
    sceKernelSleepThreadCB();
    return 0;
}

int main(void) {
    uint32_t t0;
    int t, fw_ok, fw_control;
    SceUID th;
    pspDebugScreenInit();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);
    remove("me.log");
    th = sceKernelCreateThread("callbacks", cb_thread, 0x11, 0x1000, THREAD_ATTR_USER, NULL);
    if (th >= 0) {
        sceKernelStartThread(th, 0, NULL);
    }
    say("Media Engine test 7: the firmware's ME halted and resumed, cache 24, HOME, sleep refused, sleep");
    copies("the ME the firmware's");
    say("the firmware's own call on its own ME, before we take it...");
    say_words("before");
    t0 = now();
    t = psp_me_fw_call();
    say("  the call at %08X: %s (%d) after %u us", (unsigned)psp_me_fw_call_at(), call_said(t), t, now() - t0);
    fw_control = t;
    say_words("after it");
    say("starting the ME...");
    t0 = now();
    if (!psp_me_start()) {
        say("the ME didn't start: nothing more to test");
        goto out;
    }
    say_fw();
    say("the ME is ours after %u us; its data cache %u KB, its FP control word was %08X", now() - t0,
        psp_me_dcache() >> 10, (unsigned)psp_me_fcsr_was());
    {
        uint32_t f;
        __asm__ volatile("cfc1 %0, $31" : "=r"(f));
        say("  the main CPU's FP control word %08X", (unsigned)f);
    }
    poke[0] = 0;
    sceKernelDcacheWritebackInvalidateAll();
    if (on_me(set_poke, NULL, 100000) == 0) {
        say("  a job writing our memory (%08X) didn't answer", (unsigned)(uintptr_t)poke);
        again();
    } else {
        sceKernelDcacheInvalidateRange((void*)poke, sizeof poke);
        say("  a job writing our memory: %s", poke[0] == 0x600DF00Du ? "right" : "WRONG");
    }
    test_pings("waiting on main RAM");
    copies("the ME idle on main RAM");
    say("waiting in its own memory:");
    psp_me_poll_ram(0);
    test_pings("waiting in its own memory");
    copies("the ME idle in its own memory");
    psp_me_poll_ram(1);
    say("cache 24:");
    test_cde("on the main CPU", 0);
    test_cde("on the ME", 1);
    say("FPU:");
    test_fpu();
    say("our memory:");
    test_places();
    say("our C:");
    test_work();
    say("giving the ME back to the firmware...");
    t = psp_me_stop(1);
    fw_ok = t >= 0;
    say("the ME given back (%s): %s (%d)", psp_me_fw()->can_halt && psp_me_fw()->halt == 0 ?
        (psp_me_fw()->resume == 0 ? "resumed" : "not resumed") : "started cold",
        t >= 0 ? "its code answered the firmware's own call, in us" : call_said(t), t);
    say_words("after giving back");
    if (fw_control == -4) {
        say("  (the first call is still waiting and holds the firmware's ME lock, so no later call can answer)");
    }
    if (!fw_ok) {
        say("so the sleep step is left out: the PSP stays awake while this runs");
    }
    copies("the ME the firmware's again");
    say("taking it again...");
    if (!psp_me_start()) {
        say("the ME didn't start a second time");
        goto out;
    }
    say("the ME ours again: %s", psp_me_fw()->can_halt ? halt_said(psp_me_fw()->halt) : "taken cold");
    test_home();
    test_refuse();
    if (fw_ok) {
        test_sleep();
    }
    if (psp_me_ours()) {
        t = psp_me_stop(1);
        say("the ME given back to the firmware: %s (%d)", t >= 0 ? "answered" : call_said(t), t);
    }
out:
    pspDebugScreenClear();
    pspDebugScreenSetXY(0, 8);
    say("done. Quit with HOME, or this closes in 20 seconds.");
    sceKernelDelayThread(20 * 1000 * 1000);
    sceKernelExitGame();
    return 0;
}
