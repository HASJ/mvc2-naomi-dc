/*
 * psp_reader.c  The PSP's disc reader for dc_gd.c (MVC2_GD_ASYNC, MVC2_GD_AHEAD): one read at a time on a thread
 * above the game's, so the game's frames go on while the memory stick reads; the 3DS's reader, ported.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <malloc.h>
#include <pspkernel.h>

#include "naomi_host.h"

static SceUID job_wake = -1;

static uint64_t now_us(void) {
    return (uint64_t)sceKernelGetSystemTimeWide();
}

/* the disc image's one handle, taken by a thread at a time and handed on in turn (dc_image.c): the semaphore's
   waiters are woken first come, first served, so a reader that lets go can't take it straight back while the game's
   thread waits. Made at the image's mount, on the game's thread, before any reader runs */
void host_disc_lock(int on) {
    static SceUID lock = -1;
    if (lock < 0) {
        lock = sceKernelCreateSema("disc image", 0, 1, 1, NULL);
    }
    if (on) {
        sceKernelWaitSema(lock, 1, NULL);
    } else {
        sceKernelSignalSema(lock, 1);
    }
}

int host_dir_list(const char* dir, void (*add)(const char* name, long size)) {
    SceIoDirent e;
    SceUID d = sceIoDopen(dir);
    if (d < 0) {
        return 0;
    }
    memset(&e, 0, sizeof e);
    while (sceIoDread(d, &e) > 0) {
        if (FIO_S_ISREG(e.d_stat.st_mode)) {
            add(e.d_name, (long)e.d_stat.st_size);
        } else if (strcmp(e.d_name, ".") != 0 && strcmp(e.d_name, "..") != 0) {
            host_log("gd: %s/%s is not a file (mode %04X), not listed", dir, e.d_name, (unsigned)e.d_stat.st_mode);
        }
        memset(&e, 0, sizeof e);
    }
    sceIoDclose(d);
    return 1;
}

/* the reader woken: at most one wake held */
static void wake(void) {
    sceKernelSignalSema(job_wake, 1);
}

/* the disc reader: one read at a time, from a file open already, in steps the music's reads can come between; and
   when it has nothing asked of it, one whole file read ahead (MVC2_GD_AHEAD), a step at a time */
/* a step's bytes, MVC2_GD_STEP_KB (128): each costs the SD card a request, and a requested read waits for one of the
   read-ahead's at most */
static size_t job_step(void) {
    static size_t step;
    if (step == 0) {
        const char* e = getenv("MVC2_GD_STEP_KB");
        step = (e != NULL && atoi(e) >= 16 ? (size_t)atoi(e) : 128) << 10;
    }
    return step;
}

static struct {
    DC_FILE* f;
    long off;
    uint8_t* to;
    size_t want, got;
    uint64_t ticks;
} job;
static volatile int job_busy;
volatile int psp_reader_doing;     /* 1 a job, 2 a close, 3 the small reads, 4 a read-ahead, 0 waiting */
static SceUID job_thread = -1;

enum { A_FREE, A_READING, A_DONE, A_STOP };

static struct {
    char path[1200];
    DC_FILE* f;
    DC_FILE* done_f;                /* MVC2_GD_KEEP=1: once read, still open for the game's open of it */
    uint8_t* mem;
    size_t cap, size, got;
    uint64_t ticks;
    volatile int state;
} ahead;

static void reader_failed(const char* what, const char* path, long n, size_t got);

/* a piece of the music read ahead (dc_adx.c), first of all the reader's work and between a read's steps */
static struct {
    DC_FILE* f;
    long off;
    uint8_t* to;
    size_t want;
} music;
static volatile int music_busy;

static int music_read(void) {
    size_t got;
    if (!__atomic_load_n(&music_busy, __ATOMIC_ACQUIRE)) {
        return 0;
    }
    got = dc_file_read(music.f, music.off, music.to, music.want);
    memset(music.to + got, 0, music.want - got);
    __atomic_store_n(&music_busy, 0, __ATOMIC_RELEASE);
    return 1;
}

static void job_read(void) {
    size_t got = 0;
    uint64_t t0 = now_us();
    while (got < job.want) {
        music_read();
        size_t step = job.want - got < job_step() ? job.want - got : job_step(), n;
        n = dc_file_read(job.f, job.off + (long)got, job.to + got, step);
        got += n;
        if (n < step) {
            break;
        }
    }
    if (got == 0 && job.want != 0) {
        reader_failed("the game's read", "asked for", -1, 0);
    }
    memset(job.to + got, 0, job.want - got);
    job.got = got;
    job.ticks = now_us() - t0;
    __atomic_store_n(&job_busy, 0, __ATOMIC_RELEASE);
}

/* why the reader's last small file or read-ahead failed, for the game's thread to log (MVC2_TRACE_GD) */
static char why[200];
static volatile int why_new;

static void reader_failed(const char* what, const char* path, long n, size_t got) {
    const char* name = strrchr(path, '/');
    if (__atomic_load_n(&why_new, __ATOMIC_ACQUIRE)) {
        return;
    }
    snprintf(why, sizeof why, "gd reader: %s %s failed: size %ld, %lu read, errno %d, %d files open, %lu KB free in the "
             "heap", what, name != NULL ? name + 1 : path, n, (unsigned long)got, errno, dc_files_open(),
             (unsigned long)(mallinfo().fordblks >> 10));
    __atomic_store_n(&why_new, 1, __ATOMIC_RELEASE);
}

int host_on_reader(void) {
    return job_thread >= 0 && sceKernelGetThreadId() == job_thread;
}

const char* host_reader_why(void) {
    static char out[200];
    if (!__atomic_load_n(&why_new, __ATOMIC_ACQUIRE)) {
        return NULL;
    }
    strcpy(out, why);
    __atomic_store_n(&why_new, 0, __ATOMIC_RELEASE);
    return out;
}

/* the file read ahead: opened, or a step of it read; done or let go of at its end, or when asked to stop */
static void ahead_step(void) {
    int from = A_READING;
    uint64_t t0 = now_us();
    int end, ok = 1;
    if (__atomic_load_n(&ahead.state, __ATOMIC_ACQUIRE) == A_STOP) {
        ok = 0;
    } else if (ahead.f == NULL) {
        long n = 0;
        ahead.got = 0;
        ahead.ticks = 0;
        ok = (ahead.f = dc_file_open(ahead.path, &n)) != NULL;
        if (ok) {
            ok = n >= 0 && (size_t)n <= ahead.cap;
            ahead.size = ok ? (size_t)n : 0;
        }
        if (!ok) {
            reader_failed(ahead.f == NULL ? "read-ahead's open of" : "read-ahead (too big) of", ahead.path, n, 0);
        }
    } else {
        size_t step = ahead.size - ahead.got < job_step() ? ahead.size - ahead.got : job_step();
        size_t n = dc_file_read(ahead.f, (long)ahead.got, ahead.mem + ahead.got, step);
        ahead.got += n;
        ok = n == step;
        if (!ok) {
            reader_failed("read-ahead's read of", ahead.path, (long)ahead.size, ahead.got);
        }
    }
    ahead.ticks += now_us() - t0;
    end = !ok || ahead.got == ahead.size;
    if (end) {
        static int keep = -1;
        if (keep < 0) {
            keep = getenv("MVC2_GD_KEEP") != NULL && atoi(getenv("MVC2_GD_KEEP")) != 0;
        }
        ahead.done_f = ok && keep ? ahead.f : NULL;
        dc_file_let_go(ahead.done_f);
        if (ahead.done_f == NULL && ahead.f != NULL) {
            dc_file_close(ahead.f);
        }
        ahead.f = NULL;
        if (!ok || !__atomic_compare_exchange_n(&ahead.state, &from, A_DONE, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
            if (ahead.done_f != NULL) {
                dc_file_close(ahead.done_f);
                ahead.done_f = NULL;
            }
            __atomic_store_n(&ahead.state, A_FREE, __ATOMIC_RELEASE);
        }
    }
}

/* files to close, put by the game's thread and closed by the reader's */
#define CLOSES 16
static DC_FILE* closes[CLOSES];
static volatile uint32_t close_put, close_got;

/* small files opened and read whole ahead, after any read asked for and before the read-ahead's steps; each left
   open for the game's open of it */
#define SMALLS 8
enum { S_FREE, S_QUEUED, S_DONE, S_GONE };
static struct {
    char path[256];
    DC_FILE* f;
    uint8_t* mem;
    size_t most, size;
    volatile int state;
} small[SMALLS];

/* the first one queued opened and read, or failed; 0 when none is queued */
static int small_step(void) {
    int i;
    for (i = 0; i < SMALLS; i++) {
        if (__atomic_load_n(&small[i].state, __ATOMIC_ACQUIRE) == S_QUEUED) {
            long n = -1;
            DC_FILE* f = dc_file_open(small[i].path, &n);
            int ok = 0;
            if (f != NULL) {
                if (n >= 0 && (size_t)n <= small[i].most && (small[i].mem = malloc(n > 0 ? (size_t)n : 1)) != NULL) {
                    ok = dc_file_read(f, 0, small[i].mem, (size_t)n) == (size_t)n;
                }
            }
            if (!ok) {
                reader_failed(f == NULL ? "small file's open of" : small[i].mem == NULL ? "small file (no memory or too big)"
                                                                                       : "small file's read of",
                              small[i].path, n, 0);
            }
            if (!ok) {
                if (f != NULL) {
                    dc_file_close(f);
                }
                free(small[i].mem);
                small[i].mem = NULL;
                f = NULL;
            }
            dc_file_let_go(f);
            small[i].f = f;
            small[i].size = ok ? (size_t)n : 0;
            __atomic_store_n(&small[i].state, ok ? S_DONE : S_GONE, __ATOMIC_RELEASE);
            return 1;
        }
    }
    return 0;
}

static int job_main(SceSize args, void* argp) {
    (void)args;
    (void)argp;
    for (;;) {
        int a;
        uint32_t c;
        if (music_read()) {
            continue;
        }
        if (__atomic_load_n(&job_busy, __ATOMIC_ACQUIRE)) {
            psp_reader_doing = 1;
            job_read();
            continue;
        }
        if ((c = __atomic_load_n(&close_got, __ATOMIC_RELAXED)) != __atomic_load_n(&close_put, __ATOMIC_ACQUIRE)) {
            psp_reader_doing = 2;
            dc_file_close(closes[c % CLOSES]);
            __atomic_store_n(&close_got, c + 1, __ATOMIC_RELEASE);
            continue;
        }
        psp_reader_doing = 3;
        if (small_step()) {
            continue;
        }
        a = __atomic_load_n(&ahead.state, __ATOMIC_ACQUIRE);
        if (a == A_READING || a == A_STOP) {
            psp_reader_doing = 4;
            ahead_step();
            continue;
        }
        psp_reader_doing = 0;
        sceKernelWaitSema(job_wake, 1, NULL);
    }
    return 0;
}

/* MVC2_GD_BENCH=1: the memory stick's speed into a buffer aligned for its DMA and into one that isn't, logged */
static void bench(void) {
    static const size_t len = 1u << 20;
    uint8_t* mem = memalign(64, len + 64);
    const char* path = dc_gd_path("1ST_READ.BIN");
    int k;
    if (mem == NULL || getenv("MVC2_GD_BENCH") == NULL || atoi(getenv("MVC2_GD_BENCH")) == 0) {
        free(mem);
        return;
    }
    for (k = 0; k < 4; k++) {
        size_t skew = k & 1 ? 4 : 0, got = 0;
        FILE* f = fopen(path, "rb");
        uint64_t t0 = now_us(), dt;
        if (f != NULL) {
            setvbuf(f, NULL, _IONBF, 0);
            got = fread(mem + skew, 1, len, f);
            fclose(f);
        }
        dt = now_us() - t0;
        host_log("gd bench: %u KB to %s in %.1f ms, %.2f MB a second", (unsigned)(got >> 10),
                 skew ? "an address 4 past 64" : "a 64-byte boundary", dt / 1000.0, dt ? got / (double)dt : 0.0);
    }
    free(mem);
}

static int job_make(void) {
    if (job_thread == -1) {
        bench();
        /* a CHD's hunks unpacked below the game's thread, in the time its frames leave */
        int prio = sceKernelGetThreadCurrentPriority() + (dc_image_chd() ? 1 : -1);
        job_wake = sceKernelCreateSema("gd wake", 0, 0, 1, NULL);
        job_thread = sceKernelCreateThread("gd reader", job_main, prio, 64 * 1024, PSP_THREAD_ATTR_USER, NULL);
        if (job_wake < 0 || job_thread < 0 || sceKernelStartThread(job_thread, 0, NULL) < 0) {
            host_log("gd reader: not made (%08X %08X)", (unsigned)job_wake, (unsigned)job_thread);
            job_thread = -2;
        }
    }
    return job_thread >= 0;
}

/* want bytes from off in the open file f read to to on the reader, what is past the file's end cleared; 0 when it
   can't be (the caller reads them itself). Only the reader uses f until the read ends */
int host_read_start(DC_FILE* f, long off, void* to, size_t want) {
    if (!job_make() || __atomic_load_n(&job_busy, __ATOMIC_ACQUIRE)) {
        return 0;
    }
    job.f = f;
    job.off = off;
    job.to = to;
    job.want = want;
    __atomic_store_n(&job_busy, 1, __ATOMIC_RELEASE);
    wake();
    return 1;
}

int host_read_busy(void) {
    return __atomic_load_n(&job_busy, __ATOMIC_ACQUIRE);
}

/* the read finished, waited for if need be: the bytes it found, and its time in ms */
size_t host_read_end(float* ms) {
    while (host_read_busy()) {
        sceKernelDelayThread(100);
    }
    *ms = (float)job.ticks / 1000.0f;
    return job.got;
}

/* want bytes from off in the open file f, what is past its end cleared, read to to on the reader before its other
   work; 0 when it can't be. Only the reader uses f until host_music_busy is 0 */
int host_music_start(DC_FILE* f, long off, void* to, size_t want) {
    if (!job_make() || __atomic_load_n(&music_busy, __ATOMIC_ACQUIRE)) {
        return 0;
    }
    music.f = f;
    music.off = off;
    music.to = to;
    music.want = want;
    __atomic_store_n(&music_busy, 1, __ATOMIC_RELEASE);
    wake();
    return 1;
}

int host_music_busy(void) {
    return __atomic_load_n(&music_busy, __ATOMIC_ACQUIRE);
}

void host_music_wait(void) {
    while (host_music_busy()) {
        sceKernelDelayThread(100);
    }
}

/* f closed on the reader, between its reads; here when it can't be */
void host_close_later(DC_FILE* f) {
    uint32_t p = close_put;
    if (!job_make() || p - __atomic_load_n(&close_got, __ATOMIC_ACQUIRE) == CLOSES) {
        dc_file_close(f);
        return;
    }
    closes[p % CLOSES] = f;
    __atomic_store_n(&close_put, p + 1, __ATOMIC_RELEASE);
    wake();
}

/* the whole file at path, up to most bytes, opened and read on the reader; 0 when it can't be */
int host_small_start(const char* path, size_t most) {
    int i, at = -1;
    if (!job_make() || strlen(path) >= sizeof small[0].path) {
        return 0;
    }
    for (i = 0; i < SMALLS; i++) {
        int s = __atomic_load_n(&small[i].state, __ATOMIC_ACQUIRE);
        if (s != S_FREE && strcmp(small[i].path, path) == 0) {
            return 1;
        }
        if (s == S_FREE && at < 0) {
            at = i;
        }
    }
    if (at < 0) {
        return 0;
    }
    strcpy(small[at].path, path);
    small[at].most = most;
    small[at].f = NULL;
    small[at].mem = NULL;
    __atomic_store_n(&small[at].state, S_QUEUED, __ATOMIC_RELEASE);
    wake();
    return 1;
}

static int small_find(const char* path) {
    int i;
    for (i = 0; i < SMALLS; i++) {
        int s = __atomic_load_n(&small[i].state, __ATOMIC_ACQUIRE);
        if (s != S_FREE && strcmp(small[i].path, path) == 0) {
            while (__atomic_load_n(&small[i].state, __ATOMIC_ACQUIRE) == S_QUEUED) {
                sceKernelDelayThread(100);
            }
            return i;
        }
    }
    return -1;
}

/* the file at path as the reader opened it, its size in size, if it was read ahead; the caller closes it */
DC_FILE* host_small_open(const char* path, long* size) {
    int i = small_find(path);
    DC_FILE* f;
    if (i < 0 || __atomic_load_n(&small[i].state, __ATOMIC_ACQUIRE) != S_DONE || small[i].f == NULL) {
        return NULL;
    }
    f = small[i].f;
    small[i].f = NULL;
    *size = (long)small[i].size;
    return f;
}

/* want bytes from off of the file read ahead, what is past its end cleared, and the file let go of; 0 when it
   wasn't read ahead */
int host_small_take(const char* path, size_t off, void* to, size_t want) {
    int i = small_find(path);
    size_t n;
    if (i < 0) {
        return 0;
    }
    if (__atomic_load_n(&small[i].state, __ATOMIC_ACQUIRE) != S_DONE) {
        __atomic_store_n(&small[i].state, S_FREE, __ATOMIC_RELEASE);
        return 0;
    }
    n = off < small[i].size ? (small[i].size - off < want ? small[i].size - off : want) : 0;
    psp_copy(to, small[i].mem + off, n);
    memset((uint8_t*)to + n, 0, want - n);
    free(small[i].mem);
    small[i].mem = NULL;
    if (small[i].f != NULL) {
        host_close_later(small[i].f);
        small[i].f = NULL;
    }
    __atomic_store_n(&small[i].state, S_FREE, __ATOMIC_RELEASE);
    return 1;
}

/* all of them let go of */
void host_small_drop(void) {
    int i;
    for (i = 0; i < SMALLS; i++) {
        if (__atomic_load_n(&small[i].state, __ATOMIC_ACQUIRE) != S_FREE) {
            small_find(small[i].path);
            if (small[i].f != NULL) {
                dc_file_close(small[i].f);
                small[i].f = NULL;
            }
            free(small[i].mem);
            small[i].mem = NULL;
            __atomic_store_n(&small[i].state, S_FREE, __ATOMIC_RELEASE);
        }
    }
}

/* the file read ahead let go of, its memory too when all */
void host_ahead_drop(int all) {
    int from = A_READING;
    if (!__atomic_compare_exchange_n(&ahead.state, &from, A_STOP, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) &&
        from == A_DONE) {
        if (ahead.done_f != NULL) {
            host_close_later(ahead.done_f);
            ahead.done_f = NULL;
        }
        __atomic_store_n(&ahead.state, A_FREE, __ATOMIC_RELEASE);
    }
    while (__atomic_load_n(&ahead.state, __ATOMIC_ACQUIRE) != A_FREE) {
        sceKernelDelayThread(100);
    }
    if (all) {
        free(ahead.mem);
        ahead.mem = NULL;
        ahead.cap = 0;
    }
}

/* the whole file at path, up to most bytes, read ahead when the reader has nothing else to do; 0 when it can't be */
int host_ahead_start(const char* path, size_t most) {
    host_ahead_drop(0);
    if (!job_make() || strlen(path) >= sizeof ahead.path) {
        return 0;
    }
    if (ahead.cap < most) {
        free(ahead.mem);
        ahead.cap = (ahead.mem = malloc(most)) != NULL ? most : 0;
        if (ahead.mem == NULL) {
            return 0;
        }
    }
    strcpy(ahead.path, path);
    __atomic_store_n(&ahead.state, A_READING, __ATOMIC_RELEASE);
    wake();
    return 1;
}

/* when the file at path is the one read ahead: 1 while it is still being read, 2 once it is read; else 0 */
int host_ahead_has(const char* path) {
    int a = __atomic_load_n(&ahead.state, __ATOMIC_ACQUIRE);
    return (a == A_READING || a == A_DONE) && strcmp(ahead.path, path) == 0 ? (a == A_DONE) + 1 : 0;
}

/* the file at path as the reader opened it, its size in size, once it is read ahead; the caller closes it */
DC_FILE* host_ahead_file(const char* path, long* size) {
    DC_FILE* f;
    if (host_ahead_has(path) != 2 || ahead.done_f == NULL) {
        return NULL;
    }
    f = ahead.done_f;
    ahead.done_f = NULL;
    *size = (long)ahead.size;
    return f;
}

/* no file being read ahead or waiting to be taken */
int host_ahead_idle(void) {
    return __atomic_load_n(&ahead.state, __ATOMIC_ACQUIRE) == A_FREE;
}

/* want bytes from off of the file read ahead, waited for if need be, what is past its end cleared, and the file let
   go of; 0 when it was not read whole after all (the caller reads them itself). ms: how long it was read for */
int host_ahead_take(size_t off, void* to, size_t want, float* ms) {
    size_t n;
    while (__atomic_load_n(&ahead.state, __ATOMIC_ACQUIRE) == A_READING) {
        sceKernelDelayThread(100);
    }
    if (__atomic_load_n(&ahead.state, __ATOMIC_ACQUIRE) != A_DONE) {
        return 0;
    }
    n = off < ahead.size ? (ahead.size - off < want ? ahead.size - off : want) : 0;
    psp_copy(to, ahead.mem + off, n);
    memset((uint8_t*)to + n, 0, want - n);
    *ms = (float)ahead.ticks / 1000.0f;
    if (ahead.done_f != NULL) {
        host_close_later(ahead.done_f);
        ahead.done_f = NULL;
    }
    __atomic_store_n(&ahead.state, A_FREE, __ATOMIC_RELEASE);
    return 1;
}
