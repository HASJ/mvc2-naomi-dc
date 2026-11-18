/*
 * dc_gd.c  The Dreamcast's disc (Shinobi's gdFs) over a folder of its files, read at once in whole 2048-byte
 * sectors; a request is complete when gdFsGetStat next looks, and the drive's door never opens. On the 3DS and the
 * PSP with MVC2_GD_ASYNC=1, gdFsReqRd32's reads are done on another thread, and complete after as many frames as a
 * drive would take (MVC2_GD_SEEK, GD_RATE), waited for then if need be: the same frames on every run, whatever the
 * card does. With MVC2_GD_AHEAD=1 too, a ...POL.BIN file so read has its ...TEX.BIN read ahead, for the game's
 * gdFsRead.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "naomi_host.h"
#include "prof.h"
#include "shinobi.h"
#include "frame_log.h"
FRAME_LOG fl;

/* the 3DS and the PSP read the disc on a thread of their own (host_read_start and the rest) */
#if defined(__3DS__) || defined(__PSP__)
#define GD_READER 1
#endif

#define GD_MAX_OPEN 16
#define GDD_DRVSTAT_PAUSE 1

struct GDS_FS_HANDLE {
    DC_FILE* f;
    Sint32 size;
    long pos;           /* where the next sector is read from */
    Sint32 stat;
#ifdef GD_READER
    char name[32];
#endif
};

static char root[1024];
#if defined(__3DS__) || defined(__PSP__)
static void list_folder(void);
#endif
#if defined(__PSP__)
static void open_test(void);
#endif
#if defined(__3DS__) || defined(__PSP__)
/* MVC2_GD_NOBUF=1: the files read without stdio's buffer, each read one call to the SD card's service */
static int no_buf = -1;
#endif
static int trace;
static struct GDS_FS_HANDLE files[GD_MAX_OPEN];
static struct GDS_FS_HANDLE sys_handle = { NULL, 0, 0, GDD_STAT_COMPLETE };

/* the USA disc is told by its program's size and a few of the files it reads first */
static const struct {
    const char* name;
    long size;
} disc_files[] = {
    { "1ST_READ.BIN", 1809817 }, { "AICADRV.BIN", 59264 },   { "FONT.BIN", 73792 },     { "SELTEX.BIN", 1591990 },
    { "PL00_DAT.BIN", 557408 },  { "PL3A_DAT.BIN", 464448 }, { "STG00POL.BIN", 173976 }, { "ADX_SELC.BIN", 417792 },
};

/* the first of those files that is missing from dir or the wrong size, or NULL when all are there, or when
   there is an image of the disc to unpack them from */
const char* dc_disc_check(const char* dir) {
    const char* missing = dc_disc_missing(dir);
    return missing != NULL && dc_image_find(dir) != NULL ? NULL : missing;
}

const char* dc_disc_missing(const char* dir) {
    char path[1200];
    struct stat st;
    size_t i;
    if (dc_image_mounted()) {
        for (i = 0; i < sizeof disc_files / sizeof disc_files[0]; i++) {
            uint32_t lba, bytes;
            if (!dc_image_file(disc_files[i].name, &lba, &bytes) || (long)bytes != disc_files[i].size) {
                return disc_files[i].name;
            }
        }
        return NULL;
    }
    for (i = 0; i < sizeof disc_files / sizeof disc_files[0]; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, disc_files[i].name);
        if (stat(path, &st) != 0 || st.st_size != disc_files[i].size) {
            return disc_files[i].name;
        }
    }
    /* an unpack that didn't end */
    snprintf(path, sizeof path, "%s/%s", dir, dc_unpack_mark);
    if (stat(path, &st) == 0) {
        return dc_unpack_mark;
    }
    return NULL;
}

int dc_gd_open(const char* disc_path) {
    snprintf(root, sizeof root, "%s", disc_path);
#ifdef GD_READER
    trace = getenv("MVC2_TRACE_GD") != NULL;
#endif
#if defined(__3DS__) || defined(__PSP__)
    if (!dc_image_mounted() && (getenv("MVC2_GD_LATER") == NULL || atoi(getenv("MVC2_GD_LATER")) != 0)) {
        list_folder();
    }
#endif
#if defined(__PSP__)
    if (getenv("MVC2_GD_OPEN_TEST") != NULL && atoi(getenv("MVC2_GD_OPEN_TEST")) != 0) {
        open_test();
    }
#endif
    return 1;
}

#ifdef GD_READER
#ifdef __3DS__
#include <3ds.h>
#define ticks_now() svcGetSystemTick()
#define TICKS_MS (SYSCLOCK_ARM11 / 1000.0)
#else
#include <pspkernel.h>
#define ticks_now() ((uint64_t)sceKernelGetSystemTimeWide())
#define TICKS_MS 1000.0
#endif


static uint64_t read_begin(void) {
    return ticks_now();
}

/* with MVC2_TRACE_GD: a read's time */
static void read_end(const char* what, uint64_t t0, size_t bytes) {
    if (trace) {
        host_log("gd %s %u KB in %.1f ms (frame %u)", what, (unsigned)(bytes >> 10),
                 (double)(ticks_now() - t0) / TICKS_MS, host_frame());
    }
}

/* MVC2_GD_ASYNC=1: gdFsReqRd32's reads done on ctr_stream.c's reader, the handle READ for the frames a drive of
   GD_RATE bytes a second would take, so that the game's loader (am_load.c) keeps its frames going as a file is read */
#define GD_RATE (4u << 20)
static unsigned gd_seek;    /* frames before a read's first byte, MVC2_GD_SEEK (5) */
static int async_on = -1;
static GDFS async_gd;       /* the handle whose read is on the reader */
static unsigned async_due;  /* the game's frame it is complete in */
static int async_job;       /* 0: taken from the read-ahead at once */
static long async_pos;      /* the read on the reader: from where, into what, how many bytes */
static void* async_buf;
static uint32_t async_bytes;

static int gd_async(void) {
    if (async_on < 0) {
        const char* e = getenv("MVC2_GD_SEEK");
        async_on = getenv("MVC2_GD_ASYNC") != NULL && atoi(getenv("MVC2_GD_ASYNC")) != 0;
        gd_seek = e != NULL && atoi(e) >= 0 ? (unsigned)atoi(e) : 5;
        if (async_on) {
            host_log("gd: the game's requested reads done on another thread (MVC2_GD_ASYNC), a read's first byte "
                     "%u frames after it is asked for (MVC2_GD_SEEK), the rest at %u KB a second", gd_seek, GD_RATE >> 10);
        }
    }
    return async_on;
}

static void ahead_after(const char* name);
static void next_after(const char* name, int sync);

/* the reader's read finished, waited for if need be */
static void async_end(void) {
    uint64_t fe = fl_now();
    PROF_START(t0);
    float ms = 0.0f;
    int waited = async_job && host_read_busy();
    size_t got = async_job ? host_read_end(&ms) : 0;
    if (async_job && got < async_bytes) {
        /* never the game's data cleared for a read that failed: it is read here instead */
        size_t again = dc_file_read(async_gd->f, async_pos, async_buf, async_bytes);
        host_log("gd %s: the reader read %u of %u bytes at %ld; read here instead: %u (frame %u)", async_gd->name,
                 (unsigned)got, (unsigned)async_bytes, async_pos, (unsigned)again, host_frame());
    }
    host_fills++;
    async_gd->stat = GDD_STAT_COMPLETE;
    if (trace) {
        if (async_job) {
            host_log("gd read %u KB in %.1f ms on the reader (frame %u%s)", (unsigned)(got >> 10), (double)ms,
                     host_frame(), waited ? ", waited for" : "");
        }
    }
    ahead_after(async_gd->name);
    next_after(async_gd->name, 0);
    async_gd = NULL;
    PROF_STOP(PZ_GD_READ, t0);
    fl.end_ns += fl_now() - fe;
}

#define async_wait(gd) ((gd) == async_gd && (gd) != NULL ? async_end() : (void)0)

/* MVC2_GD_AHEAD=1: the files opened lately, and the texture file read ahead after its models' */
#define AHEAD_MOST 0x1A0000
static int ahead_on = -1;
static char opened[16][32];
static unsigned opened_n;

static int gd_ahead(void) {
    if (ahead_on < 0) {
        ahead_on = getenv("MVC2_GD_AHEAD") != NULL && atoi(getenv("MVC2_GD_AHEAD")) != 0;
        if (ahead_on) {
            host_log("gd: a models file's textures read ahead (MVC2_GD_AHEAD)");
        }
    }
    return ahead_on;
}

static void ahead_after(const char* name) {
    size_t n = strlen(name);
    char tex[32];
    unsigned i;
    if (!gd_ahead() || n < 7 || n >= sizeof tex || strcmp(name + n - 7, "POL.BIN") != 0) {
        return;
    }
    memcpy(tex, name, n - 7);
    strcpy(tex + n - 7, "TEX.BIN");
    for (i = 0; i < 16; i++) {
        if (strcmp(opened[i], tex) == 0) {
            return;                     /* read lately already */
        }
    }
    if (host_ahead_start(dc_gd_path(tex), AHEAD_MOST) && trace) {
        host_log("gd %s read ahead (frame %u)", tex, host_frame());
    }
}

/* MVC2_GD_NEXT=1: the textures the menus read whole with gdFsRead, in the frame they are wanted, read ahead after
   the file the game reads before them, when that one came from the reader or the read-ahead (there is time then)
   and nothing else is read ahead: the main menu's after the title's sounds, the mode select's after the main menu's.
   And the main menu's again once the opening's music is read, in place of anything read ahead: the title follows.
   On the PSP, the small files read in the same frame as those are read whole on the reader after any read (the main
   menu's sounds, read at the title and again at the menu, its models, the select's models and memory card screen) */
static const struct {
    const char *after, *next;
    int first;          /* in place of what is read ahead, after a read here too */
    int small;          /* a small file, read whole */
} next_files[] = {
    { "SE_SYUK.BIN", "DM07TEX.BIN", 0, 0 }, { "DM07TEX.BIN", "DM0CTEX.BIN", 0, 0 },
    { "ADX_OPEN.BIN", "DM07TEX.BIN", 1, 0 },
#if defined(__PSP__)
    { "SE_SYUK.BIN", "SE_SYUK.BIN", 0, 1 }, { "SE_SYUK.BIN", "DM07POL.BIN", 0, 1 },
    { "DM07TEX.BIN", "DM0CPOL.BIN", 0, 1 }, { "DM01POL.BIN", "SELVMU.BIN", 0, 1 },
#endif
};
#define NEXT_SMALL_MOST (256 << 10)
static int next_on = -1;

/* after name is read: on the reader or from the read-ahead, else here (sync) */
static void next_after(const char* name, int sync) {
    unsigned i;
    if (next_on < 0) {
        next_on = gd_ahead() && getenv("MVC2_GD_NEXT") != NULL && atoi(getenv("MVC2_GD_NEXT")) != 0;
        if (next_on) {
            host_log("gd: the menus' textures read ahead after the files before them (MVC2_GD_NEXT)");
        }
    }
    if (!next_on) {
        return;
    }
    for (i = 0; i < sizeof next_files / sizeof next_files[0]; i++) {
        if (next_files[i].small) {
            if (strcmp(name, next_files[i].after) == 0 && host_small_start(dc_gd_path(next_files[i].next),
                                                                           NEXT_SMALL_MOST) && trace) {
                host_log("gd %s read whole after %s (frame %u)", next_files[i].next, name, host_frame());
            }
            continue;
        }
        if (strcmp(name, next_files[i].after) != 0 || (sync && !next_files[i].first) ||
            (!next_files[i].first && !host_ahead_idle()) || host_ahead_has(dc_gd_path(next_files[i].next))) {
            continue;
        }
        if (host_ahead_start(dc_gd_path(next_files[i].next), AHEAD_MOST) && trace) {
            host_log("gd %s read ahead after %s (frame %u)", next_files[i].next, name, host_frame());
        }
    }
}

/* a file read on the music's reader, as if read here: what is read ahead after it */
void dc_gd_read_elsewhere(const char* name) {
    next_after(name, 1);
}

/* the file's sectors from the read-ahead, if they are there (and read already, when done) */
static int ahead_take(GDFS gd, Sint32 nsct, void* buf, int done) {
    size_t want = (size_t)nsct * GDD_SCT_SIZE;
    float ms;
    uint64_t t0;
    if (ahead_on <= 0 || host_ahead_has(dc_gd_path(gd->name)) < 1 + done) {
        return 0;
    }
    t0 = ticks_now();
    if (!host_ahead_take((size_t)gd->pos, buf, want, &ms)) {
        PROF_ADD(PZ_GD_READ, ticks_now() - t0);
        return 0;
    }
    host_fills++;
    gd->pos += (long)want;
    gd->stat = GDD_STAT_COMPLETE;
    if (trace) {
        host_log("gd %u KB taken from the read-ahead (read in %.1f ms), %.1f ms here (frame %u)",
                 (unsigned)(want >> 10), (double)ms, (double)(ticks_now() - t0) / TICKS_MS,
                 host_frame());
    }
    next_after(gd->name, 0);
    PROF_ADD(PZ_GD_READ, ticks_now() - t0);
    return 1;
}

/* MVC2_GD_KEEP=n: the files the game closes kept open, the last n (1: KEEP_MOST), for when it opens one again; the
   one pushed out closed on the reader. A file read ahead is opened as the reader left it */
#define KEEP_MOST 8
#define GD_HANDLES 6            /* the PSP: the most of the folder's files kept open with KEEP */
static int keep_on = -1, keep_most = KEEP_MOST;
static struct {
    char name[32];
    DC_FILE* f;
    Sint32 size;
    unsigned used;
} kept[KEEP_MOST];
static unsigned keep_clock, keep_hits, keep_opens;

static DC_FILE* keep_take(const char* name, Sint32* size) {
    unsigned i;
    if (keep_on < 0) {
        keep_on = getenv("MVC2_GD_KEEP") != NULL && atoi(getenv("MVC2_GD_KEEP")) != 0;
        keep_most = keep_on && atoi(getenv("MVC2_GD_KEEP")) > 1 && atoi(getenv("MVC2_GD_KEEP")) < KEEP_MOST
                        ? atoi(getenv("MVC2_GD_KEEP"))
                        : KEEP_MOST;
        if (keep_on) {
            host_log("gd: the last %d files closed kept open for the game's next opens (MVC2_GD_KEEP)", keep_most);
        }
    }
    if (!keep_on) {
        return NULL;
    }
    keep_opens++;
    for (i = 0; i < (unsigned)keep_most; i++) {
        if (kept[i].f != NULL && strcmp(kept[i].name, name) == 0) {
            DC_FILE* f = kept[i].f;
            kept[i].f = NULL;
            *size = kept[i].size;
            keep_hits++;
            if (trace) {
                host_log("gd %s open still (%u of %u opens so)", name, keep_hits, keep_opens);
            }
            return f;
        }
    }
    return NULL;
}

#if defined(__PSP__)
/* MVC2_GD_HANDLES=n: with more than n of the folder's files open, the kept files' handles let go of, the oldest
   first (they are opened again at their next read): the memory stick lets few files be open at once */
static void keep_handles_cap(void) {
    static int most = -1;
    if (most < 0) {
        most = getenv("MVC2_GD_HANDLES") != NULL ? atoi(getenv("MVC2_GD_HANDLES")) : GD_HANDLES;
        host_log("gd: at most %d of the disc's files kept open (MVC2_GD_HANDLES)", most);
    }
    while (dc_files_open() > most) {
        unsigned i, at = KEEP_MOST;
        for (i = 0; i < (unsigned)keep_most; i++) {
            if (kept[i].f != NULL && dc_file_has_handle(kept[i].f) && (at == KEEP_MOST || kept[i].used < kept[at].used)) {
                at = i;
            }
        }
        if (at == KEEP_MOST) {
            return;
        }
        dc_file_let_go(kept[at].f);
    }
}
#endif

#if defined(__PSP__)
/* every kept file's handle let go of; how many */
static int keep_handles_let_go(void) {
    unsigned i;
    int n = 0;
    for (i = 0; i < (unsigned)keep_most; i++) {
        if (kept[i].f != NULL && dc_file_has_handle(kept[i].f)) {
            dc_file_let_go(kept[i].f);
            n++;
        }
    }
    return n;
}
#endif

/* 1 when gd's file is kept */
static int keep_put(GDFS gd) {
    unsigned i, at = 0;
    if (keep_on <= 0) {
        return 0;
    }
    for (i = 0; i < (unsigned)keep_most; i++) {
        if (kept[i].f == NULL) {
            at = i;
            break;
        }
        if (kept[i].used < kept[at].used) {
            at = i;
        }
    }
    if (kept[at].f != NULL) {
        host_close_later(kept[at].f);
    }
    snprintf(kept[at].name, sizeof kept[at].name, "%s", gd->name);
    kept[at].f = gd->f;
    kept[at].size = gd->size;
    kept[at].used = ++keep_clock;
#if defined(__PSP__)
    keep_handles_cap();
#endif
    return 1;
}

/* MVC2_GD_PROGS=1: the six players' programs (S_PLxxA to S_PLxxF), read at once as a fight begins, opened and read on
   the reader once their names are known. The players' voices (PLxx_VOI) come just before, a character's once, in the
   programs' order; the faces (PLxx_FAC) before those, each team's three in turn, and the programs take a character
   from each team in turn. So the faces name all six once the voices have named every character among them */
static int progs_on = -1;
static char voices[6][3], faces[6][3];
static unsigned nvoices, nfaces;

static int in_names(char (*names)[3], unsigned n, const char* who) {
    unsigned i;
    for (i = 0; i < n; i++) {
        if (memcmp(names[i], who, 2) == 0) {
            return 1;
        }
    }
    return 0;
}

static int is_pl(const char* name, const char* end) {
    return strncmp(name, "PL", 2) == 0 && strlen(name) == 12 && strcmp(name + 4, end) == 0;
}

static void progs_start(char (*who)[3]) {
    unsigned k;
    for (k = 0; k < 6; k++) {
        char prog[32];
        snprintf(prog, sizeof prog, "S_PL%s%c.BIN", who[k], (char)('A' + k));
        if (host_small_start(dc_gd_path(prog), 64 << 10) && trace) {
            host_log("gd %s read ahead (frame %u)", prog, host_frame());
        }
    }
}

static void progs_after_open(const char* name) {
    unsigned k, distinct = 0;
    if (progs_on < 0) {
        progs_on = getenv("MVC2_GD_PROGS") != NULL && atoi(getenv("MVC2_GD_PROGS")) != 0;
        if (progs_on) {
            host_log("gd: the players' programs read ahead once their voices are read (MVC2_GD_PROGS)");
        }
    }
    if (!progs_on) {
        return;
    }
    if (is_pl(name, "_FAC.BIN")) {
        if (nfaces == 6) {
            memmove(faces[0], faces[1], sizeof faces - sizeof faces[0]);
            nfaces = 5;
        }
        memcpy(faces[nfaces], name + 2, 2);
        faces[nfaces++][2] = 0;
        nvoices = 0;
        return;
    }
    if (!is_pl(name, "_VOI.BIN")) {
        nvoices = 0;
        return;
    }
    if (nvoices == 0) {
        host_small_drop();
    }
    if (nvoices < 6) {
        memcpy(voices[nvoices], name + 2, 2);
        voices[nvoices++][2] = 0;
    }
    for (k = 0; nfaces == 6 && k < 6; k++) {
        distinct += !in_names(faces, k, faces[k]);
        if (!in_names(voices, nvoices, faces[k])) {
            distinct = 99;
        }
    }
    if (nfaces == 6 && distinct == nvoices) {
        char who[6][3];
        for (k = 0; k < 6; k++) {
            memcpy(who[k], faces[k / 2 + (k & 1) * 3], 3);
        }
        progs_start(who);
        nvoices = nfaces = 0;
    } else if (nvoices == 6) {
        progs_start(voices);
        nvoices = 0;
    }
}

static void keep_close_all(void) {
    unsigned i;
    for (i = 0; i < KEEP_MOST; i++) {
        if (kept[i].f != NULL) {
            dc_file_close(kept[i].f);
            kept[i].f = NULL;
        }
    }
}
#else
#define read_begin() 0
#define read_end(what, t0, bytes) ((void)(t0))
#define async_wait(gd) ((void)0)
#endif

/* the disc read in place from its image, when there is one that can be. The handhelds read the folder's files,
   unpacked from an image at the first start, unless MVC2_DISC_IN_PLACE=1 (an image but a CHD) or MVC2_DISC_CHD=1
   (a CHD too); MVC2_DISC_CHD=0 refuses a CHD elsewhere, and MVC2_DISC_FILES=1 takes the folder's files */
int dc_gd_mount(const char* disc_path) {
    const char* files = getenv("MVC2_DISC_FILES");
    const char* chd = getenv("MVC2_DISC_CHD");
#if defined(__3DS__) || defined(__PSP__)
    const char* in_place = getenv("MVC2_DISC_IN_PLACE");
    int chd_ok = chd != NULL && atoi(chd) != 0;
    if (!chd_ok && (in_place == NULL || atoi(in_place) == 0)) {
        files = "1";
    }
#else
    int chd_ok = chd == NULL || atoi(chd) != 0;
#endif
    if (files != NULL && atoi(files) != 0) {
        dc_image_unmount();
        return 0;
    }
    snprintf(root, sizeof root, "%s", disc_path);
    if (!dc_image_mount(disc_path, chd_ok)) {
        return 0;
    }
    if (dc_disc_missing(disc_path) != NULL) {
        dc_image_unmount();
        return 0;
    }
    return 1;
}

struct DC_FILE {
    FILE* f;                    /* the folder's file, or NULL: the image's, or one not opened yet */
    uint32_t lba, size;
    char later[32];             /* a folder's file opened at its first read, on whichever thread reads it */
};

#if defined(__3DS__) || defined(__PSP__)
#include <strings.h>
/* the folder's files and their sizes, listed once: a file opened is opened at its first read, so that a read on the
   reader leaves the game's thread nothing to wait for (an open costs the memory stick 15-20 ms, and the 3DS's SD card
   up to 150 ms in a folder of 760 files) */
typedef struct {
    char name[32];
    long size;
} LISTED;
static LISTED* listed;
static int n_listed, max_listed;

static void list_add(const char* name, long size) {
    if (n_listed == max_listed) {
        LISTED* more = realloc(listed, (size_t)(max_listed + 256) * sizeof *listed);
        if (more == NULL) {
            return;
        }
        listed = more;
        max_listed += 256;
    }
    if (strlen(name) < sizeof listed[0].name) {
        strcpy(listed[n_listed].name, name);
        listed[n_listed++].size = size;
    }
}

static int by_listed_name(const void* a, const void* b) {
    return strcasecmp(((const LISTED*)a)->name, ((const LISTED*)b)->name);
}

static void list_folder(void) {
    free(listed);
    listed = NULL;
    n_listed = max_listed = 0;
    if (host_dir_list(root, list_add)) {
        qsort(listed, n_listed, sizeof *listed, by_listed_name);
        host_log("gd: %d files listed in %s, each opened at its first read", n_listed, root);
    } else {
        n_listed = 0;
    }
}

#if defined(__PSP__)
/* MVC2_GD_OPEN_TEST=1 (tests): how many of the folder's files can be open at once */
static void open_test(void) {
    static FILE* f[256];
    char path[1100];
    int n = 0, i, err = 0;
    for (n = 0; n < 256 && n < n_listed; n++) {
        snprintf(path, sizeof path, "%s/%s", root, listed[n].name);
        if ((f[n] = fopen(path, "rb")) == NULL) {
            err = errno;
            break;
        }
    }
    for (i = 0; i < n; i++) {
        fclose(f[i]);
    }
    host_log("gd: %d files opened at once%s (errno %d)", n, n < 256 && n < n_listed ? ", then no more" : "", err);
}
#endif

static const LISTED* list_find(const char* path) {
    size_t n = strlen(root);
    LISTED key;
    if (n_listed == 0 || strncmp(path, root, n) != 0 || path[n] != '/' || strchr(path + n + 1, '/') != NULL ||
        strlen(path + n + 1) >= sizeof key.name) {
        return NULL;
    }
    strcpy(key.name, path + n + 1);
    return bsearch(&key, listed, n_listed, sizeof *listed, by_listed_name);
}
#endif

static int file_now(DC_FILE* d, const char* path);

const char* dc_gd_path(const char* name) {
    static char path[1200];
    snprintf(path, sizeof path, "%s/%s", root, name);
    return path;
}

DC_FILE* dc_file_open(const char* path, long* size) {
    DC_FILE* d = malloc(sizeof *d);
    size_t n = strlen(root);
    if (d == NULL) {
        return NULL;
    }
    d->f = NULL;
    d->later[0] = 0;
    if (dc_image_mounted()) {
        if (strncmp(path, root, n) == 0 && path[n] == '/' && dc_image_file(path + n + 1, &d->lba, &d->size)) {
            *size = (long)d->size;
            return d;
        }
        free(d);
        return NULL;
    }
#if defined(__3DS__) || defined(__PSP__)
    {
        const LISTED* l = list_find(path);
        if (l != NULL) {
            strcpy(d->later, l->name);
            *size = l->size;
            return d;
        }
    }
#endif
    if (!file_now(d, path)) {
        free(d);
        return NULL;
    }
    fseek(d->f, 0, SEEK_END);
    *size = ftell(d->f);
    return d;
}

static int files_open;      /* the folder's files open now, on any thread */

int dc_files_open(void) {
    return __atomic_load_n(&files_open, __ATOMIC_RELAXED);
}

/* the folder's file opened */
static int file_now(DC_FILE* d, const char* path) {
    uint64_t fo = fl_now();
    d->f = fopen(path, "rb");
    fl.fopen_ns += fl_now() - fo;
#if defined(__PSP__)
    if (trace) {
        host_log("gd fopen %s in %.1f ms on %s (frame %u)", d->later, (double)(fl_now() - fo) / 1e6,
                 host_on_reader() ? "the reader" : "the game's thread", host_frame());
    }
#endif
#if defined(__PSP__)
    if (d->f == NULL && !host_on_reader()) {
        /* the memory stick lets few files be open: the kept ones' handles let go of, and tried again */
        int err = errno, n = keep_handles_let_go();
        if (n > 0) {
            d->f = fopen(path, "rb");
            host_log("gd: %s could not be opened (errno %d), then %s with %d kept files let go of", path, err,
                     d->f != NULL ? "could" : "still not", n);
        }
    }
#endif
    if (d->f == NULL) {
        if (trace) {
            host_log("gd: %s could not be opened (errno %d, %d files open)", path, errno, dc_files_open());
        }
        return 0;
    }
    __atomic_add_fetch(&files_open, 1, __ATOMIC_RELAXED);
#if defined(__3DS__) || defined(__PSP__)
    if (no_buf < 0) {
        no_buf = getenv("MVC2_GD_NOBUF") != NULL && atoi(getenv("MVC2_GD_NOBUF")) != 0;
        if (no_buf) {
            host_log("gd: the files read without stdio's buffer (MVC2_GD_NOBUF)");
        }
    }
    if (no_buf) {
        setvbuf(d->f, NULL, _IONBF, 0);
    }
#endif
    return 1;
}

size_t dc_file_read(DC_FILE* d, long off, void* to, size_t want) {
    if (d->f == NULL && d->later[0] == 0) {
        return dc_image_read(d->lba, d->size, (uint32_t)off, to, want);
    }
    if (d->f == NULL) {
        char path[1100];
        snprintf(path, sizeof path, "%s/%s", root, d->later);
#if defined(__PSP__)
        file_now(d, path);      /* if it fails, opened again below once the memory stick is back */
#else
        if (!file_now(d, path)) {
            return 0;
        }
#endif
    }
#if defined(__PSP__)
    {
        /* a handle gone bad (the PSP's sleep ends every open file's) opened again; while the memory stick is away for
           the sleep, again each 0.2 s until it is back */
        size_t got = d->f != NULL && fseek(d->f, off, SEEK_SET) == 0 ? fread(to, 1, want, d->f) : 0;
        if (got < want && (got == 0 || d->f == NULL || ferror(d->f)) && d->later[0] != 0) {
            char path[1100];
            int err = errno, tries = 0;
            snprintf(path, sizeof path, "%s/%s", root, d->later);
            do {
                dc_file_let_go(d);
                got = file_now(d, path) && fseek(d->f, off, SEEK_SET) == 0 ? fread(to, 1, want, d->f) : 0;
            } while (got == 0 && tries < 100 && host_stick_wait() && ++tries);
            host_log("gd: %s read %u of %u bytes after opening it again (errno %d before; %d waits for the memory "
                     "stick)", d->later, (unsigned)got, (unsigned)want, err, tries);
        }
        return got;
    }
#else
    if (fseek(d->f, off, SEEK_SET) != 0) {
        return 0;
    }
    return fread(to, 1, want, d->f);
#endif
}

#if defined(__PSP__)
int dc_file_has_handle(const DC_FILE* d) {
    return d->f != NULL && d->later[0] != 0;     /* one that can be let go of */
}

/* a listed file's handle closed, to be opened again at its next read: the memory stick lets few be open at once */
void dc_file_let_go(DC_FILE* d) {
    if (d != NULL && d->f != NULL && d->later[0] != 0) {
        fclose(d->f);
        d->f = NULL;
        __atomic_sub_fetch(&files_open, 1, __ATOMIC_RELAXED);
    }
}
#endif

void dc_file_close(DC_FILE* d) {
    if (d != NULL) {
        if (d->f != NULL) {
            uint64_t fc = fl_now();
            fclose(d->f);
            fl.fclose_ns += fl_now() - fc;
            __atomic_sub_fetch(&files_open, 1, __ATOMIC_RELAXED);
        }
        free(d);
    }
}

static DC_FILE* open_file(const char* name, Sint32* size) {
    long n = 0;
    DC_FILE* d = dc_file_open(dc_gd_path(name), &n);
    *size = (Sint32)n;
    return d;
}

long dc_gd_read_file(const char* name, void* buf, long max) {
    Sint32 size;
    DC_FILE* f = open_file(name, &size);
    uint64_t t0 = read_begin();
    long n;
    if (f == NULL) {
        return -1;
    }
    n = (long)dc_file_read(f, 0, buf, (size_t)max);
    read_end(name, t0, (size_t)n);
    dc_file_close(f);
    return n;
}

Sint32 gdFsInit(Sint32 max_open, void* work, Sint32 max_dirent, void* dirbuf) {
    (void)max_open;
    (void)work;
    (void)max_dirent;
    (void)dirbuf;
    return GDD_ERR_OK;
}

void gdFsFinish(void) {
    int i;
    async_wait(async_gd);
#ifdef GD_READER
    host_ahead_drop(1);
    host_small_drop();
    keep_close_all();
#endif
    for (i = 0; i < GD_MAX_OPEN; i++) {
        if (files[i].f != NULL) {
            dc_file_close(files[i].f);
            files[i].f = NULL;
        }
    }
}

static GDFS gdFsOpen_body(const char* fname, void* dirrec);
GDFS gdFsOpen(const char* fname, void* dirrec) {
    uint64_t t = fl_now();
    GDFS r = gdFsOpen_body(fname, dirrec);
    fl.open_ns += fl_now() - t;
    return r;
}

static GDFS gdFsOpen_body(const char* fname, void* dirrec) {
    int i;
    (void)dirrec;
    fl.gd_open++;
#if defined(__PSP__)
    if (trace) {
        const char* why = host_reader_why();
        if (why != NULL) {
            host_log("%s (frame %u)", why, host_frame());
        }
    }
#endif
#ifndef __3DS__
    if (getenv("MVC2_FRAMELOG")) {
        host_log("fl open %s (frame %u)", fname, host_frame());
    }
#endif
    for (i = 0; i < GD_MAX_OPEN; i++) {
        if (files[i].f == NULL) {
            GDFS gd = &files[i];
#ifdef GD_READER
            PROF_START(t0);
            long n = 0;
            if ((gd->f = host_small_open(dc_gd_path(fname), &n)) != NULL ||
                (keep_on > 0 && (gd->f = host_ahead_file(dc_gd_path(fname), &n)) != NULL)) {
                gd->size = (Sint32)n;
            } else if ((gd->f = keep_take(fname, &gd->size)) == NULL) {
                gd->f = open_file(fname, &gd->size);
            }
            PROF_STOP(PZ_GD_OPEN, t0);
            progs_after_open(fname);
            snprintf(gd->name, sizeof gd->name, "%s", fname);
            snprintf(opened[opened_n++ % 16], sizeof opened[0], "%s", fname);
#else
            gd->f = open_file(fname, &gd->size);
#endif
            if (gd->f == NULL) {
                host_log("gdFsOpen: no %s on the disc", fname);
                return NULL;
            }
            if (getenv("MVC2_TRACE_GD")) {
                host_log("gd %s %X (frame %u, %d files open)", fname, gd->size, host_frame(), dc_files_open());
            }
            gd->pos = 0;
            gd->stat = GDD_STAT_IDLE;
            return gd;
        }
    }
    host_log("gdFsOpen: more than %d files open", GD_MAX_OPEN);
    return NULL;
}

static void gdFsClose_body(GDFS gd);
void gdFsClose(GDFS gd) {
    uint64_t t = fl_now();
    gdFsClose_body(gd);
    fl.open_ns += fl_now() - t;
}

static void gdFsClose_body(GDFS gd) {
    async_wait(gd);
    if (gd != NULL && gd->f != NULL) {
        PROF_START(t0);
#ifdef GD_READER
        if (!keep_put(gd)) {
            dc_file_close(gd->f);
        }
#else
        dc_file_close(gd->f);
#endif
        gd->f = NULL;
        PROF_STOP(PZ_GD_OPEN, t0);
    }
}

Sint32 gdFsGetFileSize(GDFS gd, Sint32* fsize) {
    if (gd == NULL || gd->f == NULL) {
        return 0;
    }
    *fsize = gd->size;
    return 1;
}

static void read_sectors(GDFS gd, Sint32 nsct, void* buf) {
    size_t want = (size_t)nsct * GDD_SCT_SIZE, got = 0;
    uint64_t fl0 = fl_now();
    PROF_START(t1);
    host_fills++;
    if (gd->pos < gd->size) {
        uint64_t t0 = read_begin();
#ifdef GD_READER
        /* not past the file's end: a read that reaches it costs the SD card another access */
        size_t n = (size_t)(gd->size - gd->pos) < want ? (size_t)(gd->size - gd->pos) : want;
#else
        size_t n = want;
#endif
        got = dc_file_read(gd->f, gd->pos, buf, n);
        read_end("read", t0, got);
    }
    memset((char*)buf + got, 0, want - got);
    gd->pos += (long)want;
    gd->stat = GDD_STAT_COMPLETE;
    fl.gd_ns += fl_now() - fl0;
    PROF_STOP(PZ_GD_READ, t1);
}

static Sint32 gdFsRead_body(GDFS gd, Sint32 nsct, void* buf) {
    if (gd == NULL || gd->f == NULL) {
        return -1;
    }
    fl.gd_sync++;
    async_wait(gd);
#ifdef GD_READER
    if (ahead_take(gd, nsct, buf, 0)) {
        return GDD_ERR_OK;
    }
    {
        PROF_START(t0);
        if (host_small_take(dc_gd_path(gd->name), (size_t)gd->pos, buf, (size_t)nsct * GDD_SCT_SIZE)) {
            host_fills++;
            gd->pos += (long)nsct * GDD_SCT_SIZE;
            gd->stat = GDD_STAT_COMPLETE;
            PROF_STOP(PZ_GD_READ, t0);
            if (trace) {
                host_log("gd %s taken as read ahead (frame %u)", gd->name, host_frame());
            }
#if defined(__PSP__)
            next_after(gd->name, 0);
#endif
            return GDD_ERR_OK;
        }
    }
    read_sectors(gd, nsct, buf);
    next_after(gd->name, 1);
#else
    read_sectors(gd, nsct, buf);
#endif
    return GDD_ERR_OK;
}

Sint32 gdFsRead(GDFS gd, Sint32 nsct, void* buf) {
    uint64_t t = fl_now();
    Sint32 r = gdFsRead_body(gd, nsct, buf);
    fl.sync_ns += fl_now() - t;
    if (gd != NULL) {
        fl.gd_bytes += (uint32_t)nsct * GDD_SCT_SIZE;
    }
    return r;
}

Sint32 gdFsReqRd32(GDFS gd, Sint32 nsct, void* buf) {
    if (gd == NULL || gd->f == NULL) {
        return -1;
    }
    fl.gd_req++;
    if (gd != NULL) {
        fl.gd_bytes += (uint32_t)nsct * GDD_SCT_SIZE;
    }
#ifdef GD_READER
    if (gd_async() && gd->pos < gd->size) {
        size_t want = (size_t)nsct * GDD_SCT_SIZE;
        if (async_gd != NULL) {
            return 0;                   /* the drive busy: asked again */
        }
        uint32_t bytes = (uint32_t)(gd->size - gd->pos) < want ? (uint32_t)(gd->size - gd->pos) : (uint32_t)want;
        unsigned due = host_frame() + gd_seek + (unsigned)((bytes * 60ull + GD_RATE - 1) / GD_RATE);
        if (ahead_take(gd, nsct, buf, 1)) {
            async_job = 0;              /* in already, but complete when the drive would be */
        } else {
            if (host_ahead_has(dc_gd_path(gd->name))) {
                host_ahead_drop(0);      /* still being read: read here instead */
            }
            memset((char*)buf + bytes, 0, want - bytes);
            async_pos = gd->pos;
            async_buf = buf;
            async_bytes = bytes;
            if (!host_read_start(gd->f, gd->pos, buf, bytes)) {
                read_sectors(gd, nsct, buf);
                return nsct;
            }
            async_job = 1;
            gd->pos += (long)want;
        }
        async_due = due;
        gd->stat = GDD_STAT_READ;
        async_gd = gd;
        return nsct;
    }
#endif
    read_sectors(gd, nsct, buf);
    return nsct;
}

/* the handle's file and its size, for a read on another thread (the ADX player's read-ahead) */
DC_FILE* dc_gd_file(GDFS gd, long* size) {
    if (gd == NULL || gd->f == NULL) {
        return NULL;
    }
    async_wait(gd);
    *size = gd->size;
    return gd->f;
}

/* moves to a sector: from the file's start (origin 0), from where it is (1) or from its end (2) (the ADX player) */
Sint32 gdFsSeek(GDFS gd, Sint32 offset, Sint32 origin) {
    long base;
    if (gd == NULL || gd->f == NULL) {
        return -1;
    }
    async_wait(gd);
    base = origin == 1 ? gd->pos : origin == 2 ? (gd->size + GDD_SCT_SIZE - 1) / GDD_SCT_SIZE * GDD_SCT_SIZE : 0;
    gd->pos = base + (long)offset * GDD_SCT_SIZE;
    return GDD_ERR_OK;
}

Sint32 gdFsGetStat(GDFS gd) {
#ifdef GD_READER
    if (gd != NULL && gd == async_gd && (int)(host_frame() - async_due) >= 0) {
        async_end();
    }
#endif
    if (gd != NULL && gd->stat == GDD_STAT_READ) {
        fl.gd_wait++;
    }
    return gd != NULL ? gd->stat : GDD_STAT_IDLE;
}

GDFS gdFsGetSysHn(void) {
    return &sys_handle;
}

Sint32 gdFsGetDrvStat(void) {
    return GDD_DRVSTAT_PAUSE;
}

Sint32 gdFsReqDrvStat(void) {
    return GDD_ERR_OK;
}

void gdFsEntryErrFuncAll(GDFS_ERRFUNC func, void* obj) {
    (void)func;
    (void)obj;
}
