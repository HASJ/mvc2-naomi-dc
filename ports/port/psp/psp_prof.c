/*
 * psp_prof.c  Where the PSP's time goes, for test runs: MVC2_PROF_HW=1 logs the CPU's own counters (cycles, stalls,
 * cache misses, uncached accesses) a frame with the times; MVC2_SAMPLE=from-to[,us] samples the interrupted address
 * every us (500) over those frames into samples.bin, with a mark every 100 frames (port/tools/psp_samples.py names
 * them); MVC2_SAMPLE_SLOW=ms keeps only the frames that took longer, each after a mark of its own. Both reach the
 * hardware through kubridge, in kernel mode.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>

#include <pspdmac.h>
#include <pspkernel.h>
#include <systemctrl.h>

#include "host_cache.h"
#include "naomi_host.h"
#include "psp_front.h"
#include "psp_me.h"

/* ---- the counters, 20 words at 0xBC400000 (pspdebug.h's PspDebugProfilerRegs) ---- */

#define HW_REGS ((volatile uint32_t*)0xBC400000u)
#define HW_N 20

enum { SYSTEMCK = 1, CPUCK, INTERNAL, MEMORY, COPZ, VFPU, SLEEP, BUS_ACCESS, UNCACHED_LOAD, UNCACHED_STORE,
       CACHED_LOAD, CACHED_STORE, I_MISS, D_MISS, D_WRITEBACK, COP0_INST, FPU_INST, VFPU_INST, LOCAL_BUS };

static int hw_on, hw_asked;
static uint32_t hw[HW_N];

/* run in kernel mode: the counters read, then cleared */
static int hw_take(uint32_t* out) {
    int i;
    for (i = 0; i < HW_N; i++) {
        out[i] = HW_REGS[i];
    }
    for (i = 1; i < HW_N; i++) {
        HW_REGS[i] = 0;
    }
    HW_REGS[0] = 1;
    return 0;
}

static int hw_stop(void) {
    HW_REGS[0] = 0;
    return 0;
}

/* ---- the sampler: an alarm set in kernel mode, whose handler reads the address the interrupt came at ---- */

#define SAMPLE_US 500u          /* by default; MVC2_SAMPLE=from-to,us sets it */
#define MAX_SAMPLES (64u << 10)
#define NID_SET_ALARM 0x6652B8CAu
#define NID_CANCEL_ALARM 0x7E65B999u

static uint32_t samples[MAX_SAMPLES];
static volatile unsigned nsamples, lost, stride = 1, ticks;
static volatile int sample_now, sample_run;
static unsigned sample_from, sample_to, sample_us = SAMPLE_US;
static unsigned slow_ms, frame_at;      /* MVC2_SAMPLE_SLOW: where the frame's samples begin */
static uint64_t frame_t0;
static SceUID alarm = -1;

static volatile unsigned user_mode;

/* a full buffer halved, the marks kept: every other sample dropped and every other tick taken from then on, so the
   samples stay spread over the whole range */
static void thin(void) {
    unsigned i, n = 0, odd = 0, at = 0;
    for (i = 0; i < nsamples; i++) {
        if (i == frame_at) {
            at = n;
        }
        if (samples[i] >= 0xFFFE0000u || (odd ^= 1)) {
            samples[n++] = samples[i];
        }
    }
    frame_at = frame_at < nsamples ? at : n;
    nsamples = n;
    stride *= 2;
}

static SceUInt sample_tick(void* common) {
    uint32_t epc, sp;
    (void)common;
    if (!sample_run) {
        return 0;                       /* the alarm ends */
    }
    __asm__ volatile("move %0, $sp" : "=r"(sp));
    if (sp < 0x80000000u) {
        user_mode++;                    /* on a user stack: COP0 can't be read */
    } else if (sample_now && ++ticks % stride == 0) {
        __asm__ volatile("mfc0 %0, $14" : "=r"(epc));
        if (nsamples == MAX_SAMPLES) {
            thin();
        }
        if (nsamples < MAX_SAMPLES) {
            samples[nsamples++] = epc;
        } else {
            lost++;
        }
    }
    return sample_us;
}

/* a routine of ours by its kernel-segment address, so the kernel runs it in kernel mode */
#define KERNEL_ADDRESS(f) ((uint32_t)(uintptr_t)(f) | 0x80000000u)

static uint32_t thread_call(uint32_t nid) {
    uint32_t f = sctrlHENFindFunction("sceThreadManager", "ThreadManForUser", nid);
    return f >= 0x88000000u && f < 0x8C000000u ? f : 0;
}

static void hog_start(void);

/* MVC2_MEM_BENCH=1: memcpy against the DMA controller's copy, 1 MB from memory not in the cache, logged */
static void mem_bench(void) {
    size_t n = 1u << 20;
    uint8_t* a = memalign(64, n);
    uint8_t* b = memalign(64, n);
    uint32_t t0, t1, t2, t3, i;
    int r;
    if (a != NULL && b != NULL) {
        for (i = 0; i < n; i++) {
            a[i] = (uint8_t)(i * 2654435761u >> 24);
        }
        sceKernelDcacheWritebackInvalidateAll();
        t0 = sceKernelGetSystemTimeLow();
        memcpy(b, a, n);
        t1 = sceKernelGetSystemTimeLow();
        memset(b, 0, n);
        sceKernelDcacheWritebackInvalidateAll();
        t2 = sceKernelGetSystemTimeLow();
        sceKernelDcacheWritebackRange(a, n);
        sceKernelDcacheWritebackInvalidateRange(b, n);
        r = sceDmacMemcpy(b, a, n);
        t3 = sceKernelGetSystemTimeLow();
        host_log("memory bench, 1 MB: memcpy %.2f ms; sceDmacMemcpy %.2f ms with the cache kept right (%d, %s)",
                 (double)(t1 - t0) / 1000.0, (double)(t3 - t2) / 1000.0, r, memcmp(a, b, n) == 0 ? "the same" : "differs");
        {
            /* psp_copy at offsets, its bytes and those either side checked */
            static const uint32_t offs[] = { 0, 4, 8, 16, 32, 60 };
            size_t m = n / 2 + 37;
            int so, dof, bad = 0, cases = 0;
            uint32_t tc = 0;
            for (so = 0; so < 6; so++) {
                for (dof = 0; dof < 6; dof++) {
                    uint32_t c0;
                    memset(b, 0xA5, n);
                    c0 = sceKernelGetSystemTimeLow();
                    psp_copy(b + 64 + offs[dof], a + offs[so], m);
                    tc += sceKernelGetSystemTimeLow() - c0;
                    cases++;
                    if (memcmp(b + 64 + offs[dof], a + offs[so], m) != 0 || b[63 + offs[dof]] != 0xA5 ||
                        b[64 + offs[dof] + m] != 0xA5) {
                        bad++;
                        host_log("psp_copy wrong from offset %u to offset %u", offs[so], offs[dof]);
                    }
                }
            }
            host_log("psp_copy: %d of %d copies of %u KB right, %.2f ms each", cases - bad, cases, (unsigned)(m >> 10),
                     (double)tc / cases / 1000.0);
        }
    }
    free(a);
    free(b);
}

void psp_prof_start(void) {
    const char* s = getenv("MVC2_SAMPLE");
    uint32_t ret;
    if (getenv("MVC2_MEM_BENCH") != NULL && atoi(getenv("MVC2_MEM_BENCH")) != 0) {
        mem_bench();
    }
    hog_start();
    if (getenv("MVC2_PROF_HW") != NULL && atoi(getenv("MVC2_PROF_HW")) != 0) {
        int r = psp_kernel_call(KERNEL_ADDRESS(hw_take), (uint32_t)(uintptr_t)hw, 0, 0, NULL);
        hw_asked = 1;
        hw_on = r >= 0;
        host_log("prof: the CPU's counters %s (%d)", hw_on ? "on" : "not had", r);
    }
    if (s != NULL && sscanf(s, "%u-%u,%u", &sample_from, &sample_to, &sample_us) >= 2) {
        sample_us = sample_us < 100 ? 100 : sample_us;
        uint32_t set = thread_call(NID_SET_ALARM);
        int r = -1;
        sample_run = 1;
        if (set != 0) {
            r = psp_kernel_call(set, sample_us, KERNEL_ADDRESS(sample_tick), 0, &ret);
            alarm = r >= 0 ? (SceUID)ret : -1;
        }
        host_log("prof: sampling every %u us over frames %u-%u: sceKernelSetAlarm at %08lX gave %d %08lX", sample_us,
                 sample_from, sample_to, (unsigned long)set, r, (unsigned long)ret);
        if (getenv("MVC2_SAMPLE_SLOW") != NULL && atoi(getenv("MVC2_SAMPLE_SLOW")) > 0) {
            slow_ms = (unsigned)atoi(getenv("MVC2_SAMPLE_SLOW"));
            host_log("prof: only the frames over %u ms kept", slow_ms);
        }
        if (alarm < 0) {
            sample_run = 0;
        }
    }
}

/* MVC2_ME_HOG=KB,FROM: from frame FROM, every other 300-frame window, the ME copying KB over and over, for the cost
   of its memory traffic to the game */
typedef struct {
    uint8_t *src, *dst;
    uint32_t bytes, passes;
    volatile uint32_t* stop;
} HOG;

static HOG hog __attribute__((aligned(64)));
static uint32_t hog_stop[16] __attribute__((aligned(64)));
static unsigned hog_from;
static int hog_asked, hog_on, hog_was_on;

static void hog_job(void* p) {
    HOG* h = p;
    uint32_t i, passes = 0;
    while (*h->stop == 0) {
        for (i = 0; i < h->bytes; i += 16) {
            uint32_t a0 = *(uint32_t*)(h->src + i), a1 = *(uint32_t*)(h->src + i + 4);
            uint32_t a2 = *(uint32_t*)(h->src + i + 8), a3 = *(uint32_t*)(h->src + i + 12);
            *(uint32_t*)(h->dst + i) = a0;
            *(uint32_t*)(h->dst + i + 4) = a1;
            *(uint32_t*)(h->dst + i + 8) = a2;
            *(uint32_t*)(h->dst + i + 12) = a3;
        }
        passes++;
    }
    h->passes = passes;
}

static void hog_start(void) {
    const char* e = getenv("MVC2_ME_HOG");
    unsigned kb = 0;
    if (e == NULL || sscanf(e, "%u,%u", &kb, &hog_from) != 2 || kb == 0) {
        return;
    }
    hog.bytes = kb * 1024;
    hog.src = memalign(64, hog.bytes);
    hog.dst = memalign(64, hog.bytes);
    if (hog.src == NULL || hog.dst == NULL) {
        host_log("prof: no memory for the ME's copying");
        return;
    }
    /* the ME's addresses: main RAM cached, the stop word uncached */
    hog.src = (uint8_t*)((uintptr_t)hog.src | 0x80000000u);
    hog.dst = (uint8_t*)((uintptr_t)hog.dst | 0x80000000u);
    hog.stop = (volatile uint32_t*)((uintptr_t)hog_stop | 0xA0000000u);
    hog_asked = hog.bytes != 0 && psp_me_start();
    host_log("prof: the ME copying %u KB in every other window from frame %u: %s", kb, hog_from,
             hog_asked ? "the ME started" : "not had");
}

static void hog_frame(unsigned frame) {
    int want = hog_asked && frame >= hog_from && frame >= 2 && (frame - 2) / 300 % 2 == 1;
    volatile uint32_t* stop = (volatile uint32_t*)((uintptr_t)hog_stop | 0x40000000u);
    if (want && !hog_on) {
        *stop = 0;
        hog_on = psp_me_post(hog_job, (void*)((uintptr_t)&hog | 0x80000000u));
    } else if (!want && hog_on) {
        *stop = 1;
        hog_on = 0;
        if (!psp_me_wait(100000)) {
            host_log("prof: the ME's copying didn't stop");
            hog_asked = 0;
        }
    }
    hog_was_on |= hog_on;
}

/* every 100th frame a mark among the samples: 0xFFFF0000 + the frame / 100 */
void psp_prof_frame(unsigned frame) {
    hog_frame(frame);
    sample_now = sample_run && frame >= sample_from && frame <= sample_to;
    if (slow_ms != 0) {
        /* the frame just shown dropped unless it was slow; a mark 0xFFFE0000 + the next frame for the next */
        uint64_t t = (uint64_t)sceKernelGetSystemTimeWide();
        int intr = sceKernelCpuSuspendIntr();
        if (frame_t0 == 0 || t - frame_t0 < (uint64_t)slow_ms * 1000u) {
            nsamples = frame_at < nsamples ? frame_at : nsamples;
        }
        frame_at = nsamples;
        if (sample_now && nsamples < MAX_SAMPLES) {
            samples[nsamples++] = 0xFFFE0000u + ((frame + 1) & 0xFFFFu);
        }
        sceKernelCpuResumeIntr(intr);
        frame_t0 = t;
        return;
    }
    if (sample_now && frame % 100 == 0) {
        int intr = sceKernelCpuSuspendIntr();
        if (nsamples < MAX_SAMPLES) {
            samples[nsamples++] = 0xFFFF0000u + frame / 100;
        }
        sceKernelCpuResumeIntr(intr);
    }
}

uint32_t host_written[HOST_W_N];

void psp_prof_window(unsigned frames) {
    uint32_t c[HW_N];
    double n = frames ? frames : 1;
    if (hog_asked) {
        host_log("  the ME %s", hog_was_on ? "copying in this window" : "idle in this window");
        hog_was_on = hog_on;
    }
    if (!hw_asked) {
        return;
    }
    host_log("  written a frame, KB: lz %.1f, sources kept %.1f, texels copied %.1f, triangles %.1f, vertex cache %.1f, "
             "game models drawn here %.1f, GE list %.1f, tiles and textures made %.1f",
             host_written[HOST_W_LZ] / n / 1024, host_written[HOST_W_KEEP] / n / 1024, host_written[HOST_W_TEX] / n / 1024,
             host_written[HOST_W_TRIS] / n / 1024, host_written[HOST_W_VCACHE] / n / 1024,
             host_written[HOST_W_MODELS] / n / 1024, host_written[HOST_W_LIST] / n / 1024,
             host_written[HOST_W_ATLAS] / n / 1024);
    memset(host_written, 0, sizeof host_written);
    if (!hw_on) {
        return;
    }
    psp_kernel_call(KERNEL_ADDRESS(hw_take), (uint32_t)(uintptr_t)c, 0, 0, NULL);
    host_log("  a frame: %.2f M CPU cycles (%.2f M system), stalls %.2f M internal %.2f M memory, sleep %.2f M; "
             "I-cache misses %.0f, D-cache misses %.0f, writebacks %.0f; cached loads %.0f stores %.0f, uncached loads "
             "%.0f stores %.0f; bus %.0f, local bus %.0f; FPU %.0f, VFPU %.0f, COP0 %.0f", c[CPUCK] / n / 1e6,
             c[SYSTEMCK] / n / 1e6, c[INTERNAL] / n / 1e6, c[MEMORY] / n / 1e6, c[SLEEP] / n / 1e6, c[I_MISS] / n,
             c[D_MISS] / n, c[D_WRITEBACK] / n, c[CACHED_LOAD] / n, c[CACHED_STORE] / n, c[UNCACHED_LOAD] / n,
             c[UNCACHED_STORE] / n, c[BUS_ACCESS] / n, c[LOCAL_BUS] / n, c[FPU_INST] / n, c[VFPU_INST] / n,
             c[COP0_INST] / n);
}

extern char _ftext[], _etext[];

void psp_prof_stop(void) {
    static int stopped;
    if (stopped++) {
        return;
    }
    if (hw_on) {
        psp_kernel_call(KERNEL_ADDRESS(hw_stop), 0, 0, 0, NULL);
    }
    if (alarm >= 0) {
        uint32_t cancel = thread_call(NID_CANCEL_ALARM);
        sample_run = 0;
        sceKernelDelayThread(2 * sample_us);
        if (cancel != 0) {
            psp_kernel_call(cancel, (uint32_t)alarm, 0, 0, NULL);
        }
        if (nsamples > 0) {
            FILE* f = fopen("samples.bin", "wb");
            uint32_t head[4] = { 0x53325643u, (uint32_t)(uintptr_t)_ftext, (uint32_t)(uintptr_t)_etext, nsamples };
            if (f != NULL) {
                fwrite(head, 4, 4, f);
                fwrite(samples, 4, nsamples, f);
                fclose(f);
            }
        }
        host_log("prof: %u samples in samples.bin, one in %u ticks (%u lost, %u ticks in user mode)", nsamples, stride,
                 lost, user_mode);
    }
}
