/*
 * ctr_stream.c  One file read ahead on a thread of its own, in pieces asked for in order: the Dreamcast's music
 * from the SD card (MVC2_ADX_AHEAD). Only the thread reads the file; the game's thread opens and closes it, with
 * the thread idle, and copies out of the pieces read. And one disc read at a time done on another thread
 * (ctr_read_*, MVC2_GD_ASYNC).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <3ds.h>

#include "naomi_host.h"
#include "prof.h"

#define PIECES 3

enum { P_FREE, P_QUEUED, P_READING, P_DONE };

static struct {
    uint32_t off, len, age;
    volatile uint32_t state;
} piece[PIECES];

static uint8_t* mem;
static uint32_t piece_max;
static DC_FILE* file;
static long file_size;
static LightLock lock;
static LightEvent wake;
static Thread thread;
static uint32_t queue_clock;
static volatile uint64_t read_ticks, read_most;     /* the thread's reads, since ctr_stream_times */
static volatile uint32_t reads, shorts;    /* shorts: pieces read short and dropped, so the caller reads them */

/* the readers' core: the game's (0), above the game, where a read's return isn't kept waiting behind the sound and the
   lists; a New 3DS's core 2 for a CHD, whose hunks take milliseconds each to unpack; MVC2_READER_CORE=0, 1 or 2 */
static int reader_core(void) {
    static int core = -1;
    if (core < 0) {
        const char* e = getenv("MVC2_READER_CORE");
        bool n3ds = false;
        APT_CheckNew3DS(&n3ds);
        core = e != NULL ? atoi(e) : dc_image_chd() && n3ds ? 2 : 0;
        core = core >= 0 && core <= 2 ? core : 0;
        host_log("disc readers: on core %d", core);
    }
    return core;
}

static void reader_main(void* arg) {
    (void)arg;
    for (;;) {
        int i, at = -1;
        LightLock_Lock(&lock);
        for (i = 0; i < PIECES; i++) {
            if (piece[i].state == P_QUEUED && (at < 0 || piece[i].age < piece[at].age)) {
                at = i;
            }
        }
        if (at >= 0) {
            piece[at].state = P_READING;
        }
        LightLock_Unlock(&lock);
        if (at < 0) {
            LightEvent_Wait(&wake);
            continue;
        }
        {
            uint8_t* to = mem + (size_t)at * piece_max;
            size_t got = 0;
            uint64_t t0 = svcGetSystemTick(), dt;
            size_t whole = (long)piece[at].off >= file_size ? 0
                           : (size_t)(file_size - (long)piece[at].off) < piece[at].len
                               ? (size_t)(file_size - (long)piece[at].off)
                               : piece[at].len;
            got = dc_file_read(file, (long)piece[at].off, to, piece[at].len);
            memset(to + got, 0, piece[at].len - got);
            dt = svcGetSystemTick() - t0;
            LightLock_Lock(&lock);
            read_ticks += dt;
            read_most = dt > read_most ? dt : read_most;
            reads++;
            shorts += got < whole;
            piece[at].state = got < whole ? P_FREE : P_DONE;   /* never handed over with bytes missing */
            LightLock_Unlock(&lock);
        }
    }
}

/* the thread's reads since the last call: how many, and their mean and longest in ms */
uint32_t ctr_stream_times(float* mean, float* most, uint32_t* dropped) {
    uint32_t n;
    LightLock_Lock(&lock);
    n = reads;
    *dropped = shorts;
    shorts = 0;
    *mean = n ? (float)read_ticks * 1000.0f / SYSCLOCK_ARM11 / (float)n : 0.0f;
    *most = (float)read_most * 1000.0f / SYSCLOCK_ARM11;
    read_ticks = read_most = 0;
    reads = 0;
    LightLock_Unlock(&lock);
    return n;
}

/* the file's pieces read from now on; 0 when it can't be (and the caller reads it itself) */
int ctr_stream_open(const char* path, uint32_t most) {
    s32 prio = 0x30;
    int i;
    ctr_stream_close();
    if (mem == NULL) {
        if ((mem = malloc((size_t)PIECES * most)) == NULL) {
            return 0;
        }
        piece_max = most;
        LightLock_Init(&lock);
        LightEvent_Init(&wake, RESET_ONESHOT);
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        thread = threadCreate(reader_main, NULL, 16 * 1024, prio - 1, reader_core(), true);
        if (thread == NULL) {
            thread = threadCreate(reader_main, NULL, 16 * 1024, prio - 1, -2, true);
        }
        if (thread == NULL) {
            free(mem);
            mem = NULL;
            return 0;
        }
    }
    {
        PROF_START(t0);
        long size = 0;
        file = most <= piece_max ? dc_file_open(path, &size) : NULL;
        file_size = size;
        PROF_STOP(PZ_GD_OPEN, t0);
    }
    if (file == NULL) {
        return 0;
    }
    for (i = 0; i < PIECES; i++) {
        piece[i].state = P_FREE;
    }
    return 1;
}

/* the thread idle and the pieces all free */
void ctr_stream_drop(void) {
    int i, busy;
    if (file == NULL) {
        return;
    }
    LightLock_Lock(&lock);
    for (i = 0; i < PIECES; i++) {
        if (piece[i].state == P_QUEUED) {
            piece[i].state = P_FREE;
        }
    }
    LightLock_Unlock(&lock);
    do {
        busy = 0;
        for (i = 0; i < PIECES; i++) {
            busy |= piece[i].state == P_READING;
        }
        if (busy) {
            svcSleepThread(100000);
        }
    } while (busy);
    for (i = 0; i < PIECES; i++) {
        piece[i].state = P_FREE;
    }
}

/* and the file closed */
void ctr_stream_close(void) {
    if (file != NULL) {
        PROF_START(t0);
        ctr_stream_drop();
        dc_file_close(file);
        file = NULL;
        PROF_STOP(PZ_GD_OPEN, t0);
    }
}

/* len bytes from off asked for; the piece, or -1 when all are taken */
int ctr_stream_queue(uint32_t off, uint32_t len) {
    int i;
    if (file == NULL || len > piece_max) {
        return -1;
    }
    LightLock_Lock(&lock);
    for (i = 0; i < PIECES && piece[i].state != P_FREE; i++) {
    }
    if (i < PIECES) {
        piece[i].off = off;
        piece[i].len = len;
        piece[i].age = ++queue_clock;
        piece[i].state = P_QUEUED;
    }
    LightLock_Unlock(&lock);
    if (i == PIECES) {
        return -1;
    }
    LightEvent_Signal(&wake);
    return i;
}

/* len bytes from off that the caller has read already, as a piece read; the piece, or -1 when all are taken */
int ctr_stream_put(uint32_t off, uint32_t len, const void* bytes) {
    int i;
    if (file == NULL || len > piece_max) {
        return -1;
    }
    for (i = 0; i < PIECES && piece[i].state != P_FREE; i++) {
    }
    if (i == PIECES) {
        return -1;
    }
    memcpy(mem + (size_t)i * piece_max, bytes, len);
    LightLock_Lock(&lock);
    piece[i].off = off;
    piece[i].len = len;
    piece[i].age = ++queue_clock;
    piece[i].state = P_DONE;
    LightLock_Unlock(&lock);
    return i;
}

/* the piece asked for that holds n bytes from off, or -1 */
int ctr_stream_find(uint32_t off, uint32_t n) {
    int i, at = -1;
    for (i = 0; i < PIECES; i++) {
        if (piece[i].state != P_FREE && off >= piece[i].off && off + n <= piece[i].off + piece[i].len &&
            (at < 0 || piece[i].age < piece[at].age)) {
            at = i;
        }
    }
    return at;
}

/* the piece's bytes, NULL while they are still being read */
const uint8_t* ctr_stream_take(int i, uint32_t* off) {
    if (piece[i].state != P_DONE) {
        return NULL;
    }
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    *off = piece[i].off;
    return mem + (size_t)i * piece_max;
}

void ctr_stream_free(int i) {
    LightLock_Lock(&lock);
    piece[i].state = P_FREE;
    LightLock_Unlock(&lock);
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
static LightEvent job_wake;
static Thread job_thread;

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

static void job_read(void) {
    size_t got = 0;
    uint64_t t0 = svcGetSystemTick();
    while (got < job.want) {
        size_t step = job.want - got < job_step() ? job.want - got : job_step(), n;
        n = dc_file_read(job.f, job.off + (long)got, job.to + got, step);
        got += n;
        if (n < step) {
            break;
        }
    }
    memset(job.to + got, 0, job.want - got);
    job.got = got;
    job.ticks = svcGetSystemTick() - t0;
    __atomic_store_n(&job_busy, 0, __ATOMIC_RELEASE);
}

/* the file read ahead: opened, or a step of it read; done or let go of at its end, or when asked to stop */
static void ahead_step(void) {
    int from = A_READING;
    uint64_t t0 = svcGetSystemTick();
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
    } else {
        size_t step = ahead.size - ahead.got < job_step() ? ahead.size - ahead.got : job_step();
        size_t n = dc_file_read(ahead.f, (long)ahead.got, ahead.mem + ahead.got, step);
        ahead.got += n;
        ok = n == step;
    }
    ahead.ticks += svcGetSystemTick() - t0;
    end = !ok || ahead.got == ahead.size;
    if (end) {
        static int keep = -1;
        if (keep < 0) {
            keep = getenv("MVC2_GD_KEEP") != NULL && atoi(getenv("MVC2_GD_KEEP")) != 0;
        }
        ahead.done_f = ok && keep ? ahead.f : NULL;
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
                if (f != NULL) {
                    dc_file_close(f);
                }
                free(small[i].mem);
                small[i].mem = NULL;
                f = NULL;
            }
            small[i].f = f;
            small[i].size = ok ? (size_t)n : 0;
            __atomic_store_n(&small[i].state, ok ? S_DONE : S_GONE, __ATOMIC_RELEASE);
            return 1;
        }
    }
    return 0;
}

static void job_main(void* arg) {
    (void)arg;
    for (;;) {
        int a;
        uint32_t c;
        if (__atomic_load_n(&job_busy, __ATOMIC_ACQUIRE)) {
            job_read();
            continue;
        }
        if ((c = __atomic_load_n(&close_got, __ATOMIC_RELAXED)) != __atomic_load_n(&close_put, __ATOMIC_ACQUIRE)) {
            dc_file_close(closes[c % CLOSES]);
            __atomic_store_n(&close_got, c + 1, __ATOMIC_RELEASE);
            continue;
        }
        if (small_step()) {
            continue;
        }
        a = __atomic_load_n(&ahead.state, __ATOMIC_ACQUIRE);
        if (a == A_READING || a == A_STOP) {
            ahead_step();
            continue;
        }
        LightEvent_Wait(&job_wake);
    }
}

static int job_make(void) {
    if (job_thread == NULL) {
        s32 prio = 0x30;
        LightEvent_Init(&job_wake, RESET_ONESHOT);
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        job_thread = threadCreate(job_main, NULL, 16 * 1024, prio - 1, reader_core(), true);
        if (job_thread == NULL) {
            job_thread = threadCreate(job_main, NULL, 16 * 1024, prio - 1, -2, true);
        }
    }
    return job_thread != NULL;
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
    LightEvent_Signal(&job_wake);
    return 1;
}

int host_read_busy(void) {
    return __atomic_load_n(&job_busy, __ATOMIC_ACQUIRE);
}

/* the read finished, waited for if need be: the bytes it found, and its time in ms */
size_t host_read_end(float* ms) {
    while (host_read_busy()) {
        svcSleepThread(100000);
    }
    *ms = (float)job.ticks * 1000.0f / SYSCLOCK_ARM11;
    return job.got;
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
    LightEvent_Signal(&job_wake);
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
    LightEvent_Signal(&job_wake);
    return 1;
}

static int small_find(const char* path) {
    int i;
    for (i = 0; i < SMALLS; i++) {
        int s = __atomic_load_n(&small[i].state, __ATOMIC_ACQUIRE);
        if (s != S_FREE && strcmp(small[i].path, path) == 0) {
            while (__atomic_load_n(&small[i].state, __ATOMIC_ACQUIRE) == S_QUEUED) {
                svcSleepThread(100000);
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
    memcpy(to, small[i].mem + off, n);
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
        svcSleepThread(100000);
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
    LightEvent_Signal(&job_wake);
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
        svcSleepThread(100000);
    }
    if (__atomic_load_n(&ahead.state, __ATOMIC_ACQUIRE) != A_DONE) {
        return 0;
    }
    n = off < ahead.size ? (ahead.size - off < want ? ahead.size - off : want) : 0;
    memcpy(to, ahead.mem + off, n);
    memset((uint8_t*)to + n, 0, want - n);
    *ms = (float)ahead.ticks * 1000.0f / SYSCLOCK_ARM11;
    if (ahead.done_f != NULL) {
        host_close_later(ahead.done_f);
        ahead.done_f = NULL;
    }
    __atomic_store_n(&ahead.state, A_FREE, __ATOMIC_RELEASE);
    return 1;
}

/* the files directly in dir (on the SD card, or from the working folder) and their sizes; 0 if it can't be listed */
int host_dir_list(const char* dir, void (*add)(const char* name, long size)) {
    FS_Archive arch;
    Handle h;
    FS_DirectoryEntry* e;
    u32 n, i;
    char full[1024], cwd[512];
    if (strncmp(dir, "sdmc:/", 6) == 0) {
        snprintf(full, sizeof full, "%s", dir + 5);
    } else if (strchr(dir, ':') == NULL && dir[0] != '/' && getcwd(cwd, sizeof cwd) != NULL &&
               strncmp(cwd, "sdmc:/", 6) == 0) {
        snprintf(full, sizeof full, "%s%s%s", cwd + 5, cwd[strlen(cwd) - 1] == '/' ? "" : "/", dir);
    } else {
        return 0;
    }
    if ((e = malloc(sizeof *e * 256)) == NULL) {
        return 0;
    }
    if (R_FAILED(FSUSER_OpenArchive(&arch, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))) {
        free(e);
        return 0;
    }
    if (R_FAILED(FSUSER_OpenDirectory(&h, arch, fsMakePath(PATH_ASCII, full)))) {
        FSUSER_CloseArchive(arch);
        free(e);
        return 0;
    }
    while (R_SUCCEEDED(FSDIR_Read(h, &n, 256, e)) && n > 0) {
        for (i = 0; i < n; i++) {
            char name[64];
            ssize_t len = utf16_to_utf8((uint8_t*)name, e[i].name, sizeof name - 1);
            if (!(e[i].attributes & FS_ATTRIBUTE_DIRECTORY) && len > 0 && len < (ssize_t)sizeof name) {
                name[len] = '\0';
                add(name, (long)e[i].fileSize);
            }
        }
    }
    FSDIR_Close(h);
    FSUSER_CloseArchive(arch);
    free(e);
    return 1;
}
