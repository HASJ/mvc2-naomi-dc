/*
 * psp_audio.c  The PSP's sound out. Each frame's sound is made after the game's frame: on the Media Engine after the
 * frame's list while the lists are made there, otherwise by a thread below the game's; either way it is the PC's
 * sample for sample. It is played through sceAudio at 44.1 kHz by a thread of its own, stretched by up to 0.5% to
 * keep the queue near three frames against the drift between the screen's clock and the sound's.
 *
 * Where the game's frames fall behind the screen, the next frames' sound is made longer (MVC2_SNDPACE=0 not).
 *
 * MVC2_SNDME=0 keeps it off the ME; MVC2_SNDME_TIME=1 times its job there, watched by a thread above the game's; MVC2_SNDTHREAD=0 makes it on the game's thread instead of one below; MVC2_WAV=name
 * saves it as a WAV, made frame for frame as the PC's; MVC2_SNDLOG=1 logs its times in play too.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pspaudio.h>
#include <pspdisplay.h>
#include <pspintrman.h>
#include <pspkernel.h>
#include <pspthreadman.h>

#include "naomi_host.h"
#include "aica.h"
#include "arm7.h"
#include "snd.h"
#include "snd_drv.h"
#include "psp_front.h"
#include "psp_me.h"
#include "snd_lines.h"

#define BLOCK 512                   /* samples in each output */
#define RING 8192                   /* samples kept for it: about 11 frames */
#define FRAME 736                   /* samples in a frame, near enough */
#define TARGET (3 * FRAME)          /* samples kept queued */
#define MOST 2048                   /* samples a frame can be stretched to: two frames' and more */
#define REPORT 300                  /* frames between the log's lines */
#define FADE 128                    /* samples to silence and back where the queue runs dry */

enum { GO = 1, DONE = 2 };          /* the maker thread's */
enum { FREE, ARMED, ON_ME, HERE };  /* the frame for the ME: none, to be made there, being made there, taken back */

static int on, threaded, logged, me_use, paced, due;
static SceUID maker = -1, player = -1, flags, lock;
static volatile int quit;
static int channel = -1;
static struct {
    SND_WORK work;
    int a, b;
} pending[32];                      /* the game's calls while a frame is being made, run after it */
static int pending_n;

static volatile int me_frame = FREE;
static struct __attribute__((aligned(64))) {
    const int16_t* s;
    int n;
} me_out;                           /* the ME's, on a line of its own */

static int16_t ring[RING * 2] __attribute__((aligned(64)));
static volatile uint32_t head, tail;    /* samples put and played, from the start */

static int16_t in_last[2];          /* the last frame's last sample */
static uint32_t pos;                /* 16.16, in the frame, from in_last */
static float level = -1;            /* the queue, smoothed, in samples */
static float stretch, s_lo, s_hi;

static FILE* wav;
static uint32_t wav_samples;

/* the times, in us, over each REPORT frames: the threads' own, but for t_wall and the waits */
static uint32_t t_wall, t_arm, t_fiq, t_mix, t_frame, t_most, t_prepare, t_wait, t_call, t_chunk, t_mark, t_fiq_at;
static unsigned frames, made, dry, dropped, refilled, lo = RING, hi, voices, on_me, taken, calls_waited, longer;
static volatile unsigned ran_dry;   /* blocks played as silence, from the start */

static uint32_t now(void) {
    return sceKernelGetSystemTimeLow();
}

/* the calling thread's own time, in us: none of the threads' that preempt it */
static int run_clocks;

static uint32_t run_time(void) {
    SceKernelThreadRunStatus st;
    st.size = sizeof st;
    return sceKernelReferThreadRunStatus(0, &st) == 0 ? st.runClocks.low : 0;
}

static uint32_t cpu(void) {
    return run_clocks ? run_time() : now();
}

/* the run time used if it moves while the thread runs (PPSSPP's does not) */
static void clocks_check(void) {
    uint32_t r = run_time(), w = now();
    while (now() - w < 2000) {
    }
    run_clocks = run_time() != r;
    host_log("audio: timed by %s", run_clocks ? "the threads' own run time" : "the clock (the threads' run time stands still)");
}

/* aica_render's steps: the sound CPU from a chunk's start (its timer's interrupts among it), then the channels' mix */
static void perf(int at) {
    uint32_t t = cpu();
    switch (at) {
    case AICA_PERF_CHUNK:
        t_chunk = t;
        break;
    case AICA_PERF_ARM_END:
        t_arm += t - t_chunk;
        t_mark = t;
        break;
    case AICA_PERF_MIX_END:
        t_mix += t - t_mark;
        break;
    case AICA_PERF_FIQ:
        t_fiq_at = t;
        break;
    case AICA_PERF_FIQ_END:
        t_fiq += t - t_fiq_at;
        break;
    }
}

/* ---- the WAV ---- */

static void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void wav_header(void) {
    uint8_t h[44] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
                      0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a' };
    put32(h + 4, 36 + wav_samples * 4);
    put32(h + 24, SND_RATE);
    put32(h + 28, SND_RATE * 4);
    put32(h + 40, wav_samples * 4);
    fseek(wav, 0, SEEK_SET);
    fwrite(h, 1, 44, wav);
    fseek(wav, 0, SEEK_END);
}

static void wav_open(void) {
    if (getenv("MVC2_WAV") != NULL && (wav = fopen(getenv("MVC2_WAV"), "wb")) != NULL) {
        setvbuf(wav, NULL, _IOFBF, 64 << 10);
        wav_header();
        host_log("audio: the sound saved in %s", getenv("MVC2_WAV"));
    }
}

/* ---- the output ---- */

static void ring_put(const int16_t* s, uint32_t n) {
    uint32_t at = head % RING, first = RING - at < n ? RING - at : n;
    memcpy(ring + at * 2, s, first * 4);
    memcpy(ring, s + first * 2, (n - first) * 4);
    head += n;
}

/* the frame's samples at (1 + stretch) times their number, between each pair in a straight line */
static uint32_t resample(const int16_t* in, uint32_t n, int16_t* out, uint32_t step) {
    uint32_t o = 0;
    int c;
    while (o < MOST && pos >> 16 < n) {
        uint32_t i = pos >> 16;
        int32_t f = (int32_t)(pos & 0xFFFF) >> 1;
        for (c = 0; c < 2; c++) {
            int32_t a = i == 0 ? in_last[c] : in[(i - 1) * 2 + c], b = in[i * 2 + c];
            out[o * 2 + c] = (int16_t)(a + ((b - a) * f >> 15));
        }
        o++;
        pos += step;
    }
    pos = pos >> 16 < n ? 0 : pos - (n << 16);
    in_last[0] = in[(n - 1) * 2];
    in_last[1] = in[(n - 1) * 2 + 1];
    return o;
}

/* into the ring at a rate that holds the queue near TARGET: after it has run dry, silence is put ahead to fill it
   again; far too full, the frame is dropped */
static void queue(const int16_t* s, uint32_t n) {
    static const int16_t quiet[(TARGET - FRAME) * 2];
    static int16_t out[MOST * 2] __attribute__((aligned(64)));
    static unsigned dry_seen;
    int rest = sceAudioGetChannelRestLen(channel), fade = 0;
    uint32_t used = head - tail, queued = used + (rest > 0 ? (uint32_t)rest : 0), o, i;
    if (ran_dry != dry_seen) {
        dry_seen = ran_dry;
        if (queued < TARGET - FRAME) {
            ring_put(quiet, TARGET - FRAME - queued);
            refilled++;
            queued = TARGET - FRAME;
            fade = 1;
        }
        level = queued;
    } else if (used + MOST > RING) {
        level = TARGET;
        dropped++;
        return;
    }
    level = level < 0 ? queued : level + (queued - level) * 0.05f;
    stretch = (TARGET - level) * (0.0025f / FRAME);
    stretch = stretch > 0.005f ? 0.005f : stretch < -0.005f ? -0.005f : stretch;
    s_lo = stretch < s_lo ? stretch : s_lo;
    s_hi = stretch > s_hi ? stretch : s_hi;
    o = resample(s, n, out, (uint32_t)(65536.0f / (1.0f + stretch) + 0.5f));
    for (i = 0; fade && i < FADE && i < o; i++) {
        out[i * 2] = (int16_t)(out[i * 2] * (int32_t)i / FADE);     /* from the silence put ahead, brought up */
        out[i * 2 + 1] = (int16_t)(out[i * 2 + 1] * (int32_t)i / FADE);
    }
    ring_put(out, o);
}

static int play_thread(SceSize args, void* argp) {
    static int16_t block[BLOCK * 2] __attribute__((aligned(64)));
    int i, c, was_dry = 0;
    (void)args;
    (void)argp;
    while (!quit) {
        uint32_t used = head - tail;
        lo = used < lo ? used : lo;
        hi = used > hi ? used : hi;
        if (used >= BLOCK) {
            uint32_t at = tail % RING, first = RING - at < BLOCK ? RING - at : BLOCK;
            memcpy(block, ring + at * 2, first * 4);
            memcpy(block + first * 2, ring, (BLOCK - first) * 4);
            tail += BLOCK;
            was_dry = 0;
        } else {
            /* run dry: the last sample brought down to silence, not cut */
            for (i = 0; i < BLOCK; i++) {
                for (c = 0; c < 2; c++) {
                    block[i * 2 + c] = was_dry || i >= FADE ? 0 : (int16_t)(block[(BLOCK - 1) * 2 + c] * (FADE - i) / FADE);
                }
            }
            was_dry = 1;
            dry++;
            ran_dry++;
        }
        sceAudioOutputPannedBlocking(channel, PSP_AUDIO_VOLUME_MAX, PSP_AUDIO_VOLUME_MAX, block);
    }
    return 0;
}

/* ---- making the frames ---- */

/* a frame's samples saved and queued */
static void publish(const int16_t* s, int n) {
    AICA_STATS st;
    aica_stats(&st);
    voices += st.active_channels;
    made++;
    if (n <= 0) {
        return;
    }
    if (wav != NULL) {
        fwrite(s, 4, (size_t)n, wav);
        wav_samples += (uint32_t)n;
    }
    if (channel >= 0) {
        queue(s, (uint32_t)n);
    }
}

static void make(void) {
    const int16_t* s;
    uint32_t t0 = cpu(), w0 = now();
    int n = snd_frame(&s);
    uint32_t t = cpu() - t0;
    t_wall += now() - w0;
    t_frame += t;
    t_most = t > t_most ? t : t_most;
    publish(s, n);
}

/* the calls kept while the frame was made, in their order; any sound call they make runs at once */
static void run_pending(void) {
    int i;
    sceKernelWaitSema(lock, 1, NULL);
    for (i = 0; i < pending_n; i++) {
        sceKernelSignalSema(lock, 1);
        pending[i].work(pending[i].a, pending[i].b);
        sceKernelWaitSema(lock, 1, NULL);
    }
    pending_n = 0;
    sceKernelSignalSema(lock, 1);
}

static int is_done(void) {
    u32 bits;
    return sceKernelPollEventFlag(flags, DONE, PSP_EVENT_WAITOR, &bits) == 0;
}

static int make_thread(SceSize args, void* argp) {
    uint32_t fcr;
    (void)args;
    (void)argp;
    /* the game thread's rounding (main_psp.c's fpu_setup) */
    __asm__ volatile("cfc1 %0, $31" : "=r"(fcr));
    fcr = (fcr & ~0x00000F83u) | 0x01000000u;
    __asm__ volatile("ctc1 %0, $31" : : "r"(fcr));
    for (;;) {
        sceKernelWaitEventFlag(flags, GO, PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR, NULL, NULL);
        if (quit) {
            break;
        }
        make();
        run_pending();
        sceKernelSetEventFlag(flags, DONE);
    }
    return 0;
}

/* the ME's job, after the frame's list: nothing logged, timed or called into the kernel */
static void me_job(void* arg) {
    (void)arg;
    me_out.n = snd_frame(&me_out.s);
}

static void frame(int me);

/* once the frame's list is given to the ME (psp_render.c), the last frame's sound made there ended */
static void before_list(int on_me) {
    if (due) {
        frame(on_me);
    }
}

/* MVC2_SNDME_TIME=1: the ME's job from its post to its end, as a thread above the game's sees it */
static volatile uint32_t me_at, me_ended;
static uint32_t me_sum, me_most, me_n;

static int me_watch(SceSize args, void* argp) {
    (void)args;
    (void)argp;
    while (!quit) {
        if (me_frame == ON_ME && !me_ended && psp_me_wait(0)) {
            uint32_t d = now() - me_at;
            me_ended = 1;
            me_sum += d;
            me_most = d > me_most ? d : me_most;
            me_n++;
        }
        sceKernelDelayThread(200);
    }
    return 0;
}

/* on the main CPU just before the job is given to the ME (the kicker's thread, above the game's) */
static int me_ready(void* arg) {
    (void)arg;
    if (me_frame != ARMED) {
        return 0;
    }
    me_at = now();
    me_ended = 0;
    me_frame = ON_ME;
    return 1;
}

/* the job ended, on the game's thread: what the ME wrote read from RAM */
static void me_done(void* arg) {
    (void)arg;
    if (me_frame != ON_ME) {
        return;
    }
    sceKernelDcacheWritebackInvalidateAll();
    publish(me_out.s, me_out.n);
    on_me++;
    me_frame = FREE;
    run_pending();
}

/* the frame being made finished, and the calls kept meanwhile run: before the game's next call that reads or changes
   the sound, and before the next frame. A frame not yet given to the ME is made here */
static void settle(void) {
    uint32_t t0;
    if (sceKernelGetThreadId() == maker) {
        return;     /* the maker's own calls (pending work) wait for nothing */
    }
    if (threaded && !is_done()) {
        t0 = now();
        sceKernelWaitEventFlag(flags, DONE, PSP_EVENT_WAITOR, NULL, NULL);
        t_wait += now() - t0;
        return;
    }
    if (me_frame == FREE) {
        return;
    }
    t0 = now();
    {
        unsigned s = sceKernelCpuSuspendIntr();     /* the kicker can't give it to the ME meanwhile */
        if (me_frame == ARMED) {
            me_frame = HERE;
        }
        sceKernelCpuResumeIntr(s);
    }
    if (me_frame == ON_ME) {
        psp_render_me_after_wait();
    }
    if (me_frame != FREE) {
        me_frame = HERE;    /* not given to the ME after all */
        make();
        taken++;
        me_frame = FREE;
        run_pending();
    }
    t_wait += now() - t0;
}

/* a sound call from the game */
static void call_sync(void) {
    uint32_t t0 = now();
    int busy = (threaded && !is_done()) || me_frame != FREE;
    settle();
    if (busy && sceKernelGetThreadId() != maker) {
        calls_waited++;
        t_call += now() - t0;
    }
}

/* the game's calls at a frame's start, kept to run after the frame being made */
static int defer(SND_WORK work, int a, int b) {
    int taken_here = 0;
    sceKernelWaitSema(lock, 1, NULL);
    if (((threaded && !is_done()) || me_frame != FREE) && pending_n < 32) {
        pending[pending_n].work = work;
        pending[pending_n].a = a;
        pending[pending_n].b = b;
        pending_n++;
        taken_here = 1;
    }
    sceKernelSignalSema(lock, 1);
    return taken_here;
}

static double ms(uint32_t us) {
    return us / 1000.0 / REPORT;
}

static void report(void) {
    if (++frames % REPORT != 0 || !logged) {
        return;
    }
    host_log("audio: a frame %.2f ms, %.2f from start to end (sound CPU %.2f of which interrupts %.2f, channels %.2f with %.1f sounding; the "
             "most %.2f), ADX %.2f; %u of %u frames made on the ME, %u taken back; the game waited %.2f ms a frame, %.2f of it in %u calls; "
             "%u made longer; queue %u to %u samples, stretch %+.2f%% to %+.2f%%, %u blocks dry, %u refilled, %u frames dropped",
             ms(t_frame), ms(t_wall), ms(t_arm), ms(t_fiq), ms(t_mix), made ? (double)voices / made : 0.0, t_most / 1000.0,
             ms(t_prepare), on_me, made, taken, ms(t_wait), ms(t_call), calls_waited, longer, lo, hi, s_lo * 100.0, s_hi * 100.0, dry,
             refilled, dropped);
    if (me_n != 0) {
        host_log("audio: the ME's job %.2f ms from its post to its end, the most %.2f, %u seen", me_sum / 1000.0 / me_n,
                 me_most / 1000.0, me_n);
        me_sum = me_most = me_n = 0;
    }
    t_wall = t_arm = t_fiq = t_mix = t_frame = t_most = t_prepare = t_wait = t_call = 0;
    made = on_me = taken = calls_waited = longer = dry = dropped = refilled = hi = voices = 0;
    lo = RING;
    s_lo = s_hi = stretch;
}

/* the ME may make the sound: its data is on lines of its own, and nothing set logs or calls the kernel there */
static int me_allowed(void) {
    static const char* const off[] = { "MVC2_SNDDBG", "MVC2_SNDTRACE", "MVC2_DSPDUMP" };
    uintptr_t b = (uintptr_t)snd_lines_bss_begin, e = (uintptr_t)snd_lines_bss_end, a = (uintptr_t)&aica_ram;
    const char* native = getenv("MVC2_SNDNATIVE");
    int i;
    if (getenv("MVC2_SNDME") != NULL && atoi(getenv("MVC2_SNDME")) == 0) {
        return 0;
    }
    for (i = 0; i < (int)(sizeof off / sizeof *off); i++) {
        if (getenv(off[i]) != NULL) {
            host_log("audio: not on the ME with %s", off[i]);
            return 0;
        }
    }
    if ((native != NULL && (atoi(native) == 2 || atoi(native) == 4)) ||
        (getenv("MVC2_ARMJIT") != NULL && atoi(getenv("MVC2_ARMJIT")) != 0) ||
        (getenv("MVC2_ARMJIT2") != NULL && atoi(getenv("MVC2_ARMJIT2")) != 0)) {
        host_log("audio: not on the ME with the sound driver checked or translated");
        return 0;
    }
    if (a < b + 64 || a >= e || ((uintptr_t)snd_lines_data_begin & 63) || ((uintptr_t)snd_lines_data_end & 63) ||
        (b & 63) || (e & 63)) {
        host_log("audio: not on the ME: the sound's data isn't on lines of its own (%08X %08X %08X)", (unsigned)b,
                 (unsigned)a, (unsigned)e);
        return 0;
    }
    return 1;
}

/* ---- the front end's ---- */

void psp_audio_init(void) {
    int prio = sceKernelGetThreadCurrentPriority();
    static const char* const defaults[][2] = {
        { "MVC2_ARMFAST", "1" },        /* the same sound, at half the sound CPU's cost */
        { "MVC2_SNDNATIVE", "3" },      /* the sound driver as C, as on the 3DS: its sounds a little sooner, a third
                                           the cost */
        { "MVC2_SNDCHUNK", "64" },      /* the channels mixed 64 samples at a time: near the same, 40% less */
    };
    int i;
    if (on || !snd_enabled()) {
        return;
    }
    for (i = 0; i < (int)(sizeof defaults / sizeof *defaults); i++) {
        if (getenv(defaults[i][0]) == NULL) {
            setenv(defaults[i][0], defaults[i][1], 0);
            host_log("default: %s=%s", defaults[i][0], defaults[i][1]);
        }
    }
    wav_open();
    paced = wav == NULL && (getenv("MVC2_SNDPACE") == NULL || atoi(getenv("MVC2_SNDPACE")) != 0);
    /* the times in the log: in a test run, or with MVC2_SNDLOG=1 */
    logged = getenv("MVC2_SNDLOG") != NULL ? atoi(getenv("MVC2_SNDLOG")) != 0
                                           : getenv("MVC2_FRAMES") != NULL || getenv("MVC2_SHOT") != NULL;
    channel = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, BLOCK, PSP_AUDIO_FORMAT_STEREO);
    if (channel < 0) {
        host_log("audio: no channel (%08X): the sound made, not played", (unsigned)channel);
    } else {
        player = sceKernelCreateThread("sound out", play_thread, 0x10, 0x4000, PSP_THREAD_ATTR_USER, NULL);
        if (player >= 0) {
            sceKernelStartThread(player, 0, NULL);
        }
    }
    flags = sceKernelCreateEventFlag("sound", PSP_EVENT_WAITMULTIPLE, DONE, NULL);
    lock = sceKernelCreateSema("sound", 0, 1, 1, NULL);
    threaded = getenv("MVC2_SNDTHREAD") == NULL || atoi(getenv("MVC2_SNDTHREAD")) != 0;
    if (threaded && flags >= 0 && lock >= 0) {
        maker = sceKernelCreateThread("sound", make_thread, prio + 1, 0x10000, PSP_THREAD_ATTR_USER, NULL);
        threaded = maker >= 0 && sceKernelStartThread(maker, 0, NULL) >= 0;
    } else {
        threaded = 0;
    }
    me_use = threaded && me_allowed();
    if (me_use) {
        snd_drv_quiet(1);
        psp_render_me_after(me_job, me_ready, me_done, NULL);
        psp_render_me_before(before_list);
        if (getenv("MVC2_SNDME_TIME") != NULL && atoi(getenv("MVC2_SNDME_TIME")) != 0) {
            SceUID w = sceKernelCreateThread("sound ME watch", me_watch, prio - 2, 0x2000, PSP_THREAD_ATTR_USER, NULL);
            if (w >= 0) {
                sceKernelStartThread(w, 0, NULL);
            }
        }
    }
    if (logged) {
        clocks_check();
        if (!me_use) {
            aica_perf = perf;   /* the ME can't time it */
        }
    }
    host_log("audio: sceAudio channel %d, the sound made on %s", channel,
             me_use ? "the Media Engine after each list there, else a thread below the game's"
                    : threaded ? "a thread below the game's" : "the game's thread");
    on = 1;
}

/* MVC2_SNDPACE (on in play, unless 0; never with MVC2_WAV): the sound kept to the screen's time, as the sound CPU
   keeps its own: where the game's frames fall behind the vblanks, the next frames' sound made longer, up to twice */
static void pace(void) {
    static unsigned last_v;
    static int32_t owed;
    unsigned v = sceDisplayGetVcount();
    int32_t more = 0;
    if (last_v != 0) {
        owed += ((int32_t)(v - last_v) - 1) * SND_FRAME_US;
        owed = owed < -SND_FRAME_US ? -SND_FRAME_US : owed > 4 * SND_FRAME_US ? 4 * SND_FRAME_US : owed;
        if (level > TARGET + FRAME) {
            owed = owed < 0 ? owed : 0;
        } else if (owed > SND_FRAME_US / 4) {
            more = owed < SND_FRAME_US ? owed : SND_FRAME_US;
            owed -= more;
            longer++;
        }
    }
    last_v = v;
    snd_set_frame_us(SND_FRAME_US + (uint32_t)more);
}

/* the game's frame's end: the frame before's sound finished, this one's begun, on the ME after the frame's list when
   me says so */
static void frame(int me) {
    uint32_t t0;
    due = 0;
    settle();
    if (paced) {
        pace();
    }
    t0 = cpu();
    snd_prepare();
    t_prepare += cpu() - t0;
    report();
    snd_set_sync(call_sync);    /* again after a switch, which puts the sound's state back */
    snd_set_defer(defer);
    if (!threaded) {
        make();
    } else if (me) {
        me_frame = ARMED;
    } else {
        sceKernelClearEventFlag(flags, ~(u32)DONE);
        sceKernelSetEventFlag(flags, GO);
    }
}

void psp_audio_due(void) {
    due = on;
}

void psp_audio_frame(void) {
    if (due) {
        frame(0);
    }
}

void psp_audio_idle(void) {
    if (on) {
        settle();
    }
}

void psp_audio_exit(void) {
    if (!on) {
        return;
    }
    settle();
    if (me_use) {
        psp_render_me_after(NULL, NULL, NULL, NULL);
        psp_render_me_before(NULL);
        me_use = 0;
    }
    if (threaded) {
        quit = 1;
        sceKernelSetEventFlag(flags, GO);
        sceKernelWaitThreadEnd(maker, NULL);
        sceKernelDeleteThread(maker);
        threaded = 0;
    }
    snd_set_sync(NULL);
    snd_set_defer(NULL);
    quit = 1;
    if (player >= 0 && channel >= 0) {
        sceKernelWaitThreadEnd(player, NULL);
        sceKernelDeleteThread(player);
        sceAudioChRelease(channel);
    }
    aica_perf = NULL;
    snd_close();
    if (wav != NULL) {
        wav_header();
        fclose(wav);
        wav = NULL;
    }
    on = 0;
}
