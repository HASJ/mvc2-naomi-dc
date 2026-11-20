/*
 * dc_image.c  The disc's files, unpacked once from an image of the disc into the disc's folder: a CHD, a .gdi
 * with its tracks, a .cue with its .bins (Redump's), or an .iso of track 3
 *
 * The files are those of track 3, the GD-ROM's high-density area, which starts at sector 45000 and holds an
 * ISO 9660 file system whose sectors count from the disc's start.
 */

#include <ctype.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <strings.h>
#endif
#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#define make_dir(p) mkdir((p), 0777)
#endif

#include "../ext/libchdr/include/libchdr/chd.h"
#include "../ext/libchdr/include/libchdr/cdrom.h"

#include "naomi_host.h"

#define HD_FIRST 45000          /* track 3's first sector */
#define SECTOR 2048
#define CHUNK 128               /* sectors a read */
#define KEPT 8                  /* a CHD's hunks kept */
#define CHD_BUF (32 << 10)      /* the CHD file's buffer: its hunks are read a few KB at a time */

typedef struct {
    FILE* f;
    long long at;               /* the file offset of the track's first sector */
    chd_file* chd;
    FILE* chd_f;
    uint32_t frame0;            /* the CHD frame of the track's first sector */
    uint32_t per_hunk;          /* frames a hunk */
    uint32_t hunk_bytes;
    uint8_t* hunks;             /* KEPT hunks, the least recently used read over */
    uint32_t hunk_no[KEPT];     /* each one's number, or ~0 */
    uint32_t hunk_used[KEPT], uses;
    int size;                   /* bytes a sector in the image */
    int data;                   /* where in a sector its 2048 bytes start */
    uint8_t* raw;               /* CHUNK sectors of the image */
} TRACK;

static char why[300];

static const char* fail(const char* fmt, const char* what) {
    snprintf(why, sizeof why, fmt, what);
    host_log("disc image: %s", why);
    return why;
}

static int ends_with(const char* name, const char* end) {
    size_t n = strlen(name), e = strlen(end);
    size_t i;
    if (n < e) {
        return 0;
    }
    for (i = 0; i < e; i++) {
        if (tolower((unsigned char)name[n - e + i]) != end[i]) {
            return 0;
        }
    }
    return 1;
}

static int seek(FILE* f, long long at) {
#ifdef _WIN32
    return _fseeki64(f, at, SEEK_SET);
#else
    return fseeko(f, (off_t)at, SEEK_SET);
#endif
}

/* hunk h of the CHD, kept or read; NULL when it can't be */
static const uint8_t* chd_hunk(TRACK* t, uint32_t h) {
    int i, at = 0;
    for (i = 0; i < KEPT; i++) {
        if (t->hunk_no[i] == h) {
            t->hunk_used[i] = ++t->uses;
            return t->hunks + (size_t)i * t->hunk_bytes;
        }
        if (t->hunk_used[i] < t->hunk_used[at]) {
            at = i;
        }
    }
    t->hunk_no[at] = ~0u;
    if (chd_read(t->chd, h, t->hunks + (size_t)at * t->hunk_bytes) != CHDERR_NONE) {
        return NULL;
    }
    t->hunk_no[at] = h;
    t->hunk_used[at] = ++t->uses;
    return t->hunks + (size_t)at * t->hunk_bytes;
}

/* n sectors of track 3 from sector lba on, their 2048 bytes each */
static int read_sectors(TRACK* t, uint32_t lba, uint32_t n, uint8_t* out) {
    uint32_t i;
    if (lba < HD_FIRST) {
        return 0;
    }
    if (t->chd != NULL) {
        for (i = 0; i < n; i++) {
            uint32_t frame = t->frame0 + lba - HD_FIRST + i;
            const uint8_t* hunk = chd_hunk(t, frame / t->per_hunk);
            if (hunk == NULL) {
                return 0;
            }
            memcpy(out + i * SECTOR, hunk + (frame % t->per_hunk) * CD_FRAME_SIZE + t->data, SECTOR);
        }
        return 1;
    }
    if (t->size == SECTOR) {
        return seek(t->f, t->at + (long long)(lba - HD_FIRST) * SECTOR) == 0 && fread(out, SECTOR, n, t->f) == n;
    }
    while (n > 0) {
        uint32_t k = n < CHUNK ? n : CHUNK;
        if (seek(t->f, t->at + (long long)(lba - HD_FIRST) * t->size) != 0 || fread(t->raw, t->size, k, t->f) != k) {
            return 0;
        }
        for (i = 0; i < k; i++) {
            memcpy(out + i * SECTOR, t->raw + i * t->size + t->data, SECTOR);
        }
        out += k * SECTOR;
        lba += k;
        n -= k;
    }
    return 1;
}

static int has_volume(TRACK* t) {
    uint8_t s[SECTOR];
    return read_sectors(t, HD_FIRST + 16, 1, s) && s[0] == 1 && memcmp(s + 1, "CD001", 5) == 0;
}

/* the track's start in the file at, or 150 sectors (a pregap) on */
static int file_track(TRACK* t, const char* path, long long at, int size) {
    static const int gaps[] = { 0, 150, -150 };
    size_t i;
    t->f = fopen(path, "rb");
    t->size = size;
    t->data = size == 2352 ? 16 : 0;
    if (t->f == NULL || (size != SECTOR && size != 2352)) {
        return 0;
    }
    for (i = 0; i < sizeof gaps / sizeof gaps[0]; i++) {
        t->at = at + (long long)gaps[i] * size;
        if (t->at >= 0 && has_volume(t)) {
            return 1;
        }
    }
    return 0;
}

/* the next field of a line of a .gdi or .cue: a word, or a quoted name */
static const char* field(const char** p, char* out, size_t size) {
    const char* s = *p;
    size_t n = 0;
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    if (*s == 0 || *s == '\r' || *s == '\n') {
        return NULL;
    }
    if (*s == '"') {
        for (s++; *s != 0 && *s != '"'; s++) {
            if (n + 1 < size) {
                out[n++] = *s;
            }
        }
        if (*s == '"') {
            s++;
        }
    } else {
        for (; *s != 0 && *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n'; s++) {
            if (n + 1 < size) {
                out[n++] = *s;
            }
        }
    }
    out[n] = 0;
    *p = s;
    return out;
}

/* track 3 as a .gdi names it: "3 45000 4 2352 track03.bin 0" */
static int gdi_track(TRACK* t, const char* dir, const char* sheet) {
    char line[1200], f[5][1100], path[2400];
    FILE* s;
    int ok = 0;
    snprintf(path, sizeof path, "%s/%s", dir, sheet);
    if ((s = fopen(path, "r")) == NULL) {
        return 0;
    }
    while (!ok && fgets(line, sizeof line, s) != NULL) {
        const char* p = line;
        int i;
        for (i = 0; i < 5 && field(&p, f[i], sizeof f[i]) != NULL; i++) {
        }
        if (i == 5 && atol(f[1]) == HD_FIRST) {
            snprintf(path, sizeof path, "%s/%s", dir, f[4]);
            ok = file_track(t, path, 0, atoi(f[3]));
        }
    }
    fclose(s);
    return ok;
}

/* track 3 as a .cue names it: its FILE, and INDEX 01's place in it */
static int cue_track(TRACK* t, const char* dir, const char* sheet) {
    char line[1200], word[1100], file[1100] = "", path[2400];
    FILE* s;
    int track = 0, size = 2352, ok = 0;
    snprintf(path, sizeof path, "%s/%s", dir, sheet);
    if ((s = fopen(path, "r")) == NULL) {
        return 0;
    }
    while (!ok && fgets(line, sizeof line, s) != NULL) {
        const char* p = line;
        if (field(&p, word, sizeof word) == NULL) {
            continue;
        }
        if (strcmp(word, "FILE") == 0 && field(&p, word, sizeof word) != NULL) {
            snprintf(file, sizeof file, "%s", word);
        } else if (strcmp(word, "TRACK") == 0 && field(&p, word, sizeof word) != NULL) {
            track = atoi(word);
            if (field(&p, word, sizeof word) != NULL) {
                size = strstr(word, "2048") != NULL ? SECTOR : 2352;
            }
        } else if (strcmp(word, "INDEX") == 0 && track == 3 && field(&p, word, sizeof word) != NULL &&
                   atoi(word) == 1 && field(&p, word, sizeof word) != NULL) {
            int m = 0, sec = 0, fr = 0;
            sscanf(word, "%d:%d:%d", &m, &sec, &fr);
            snprintf(path, sizeof path, "%s/%s", dir, file);
            ok = file_track(t, path, (long long)((m * 60 + sec) * 75 + fr) * size, size);
        }
    }
    fclose(s);
    return ok;
}

/* track 3 of a CHD: its frames follow the earlier tracks' */
static int chd_track(TRACK* t, const char* path, const char** err) {
    char meta[512], type[64], sub[64], pgtype[64], pgsub[64];
    const chd_header* h;
    uint32_t phys = 0, i;
    if ((t->chd_f = fopen(path, "rb")) != NULL) {
        setvbuf(t->chd_f, NULL, _IOFBF, CHD_BUF);
    }
    if (t->chd_f == NULL || chd_open_file(t->chd_f, CHD_OPEN_READ, NULL, &t->chd) != CHDERR_NONE) {
        *err = "the CHD could not be opened";
        return 0;
    }
    h = chd_get_header(t->chd);
    t->per_hunk = h->hunkbytes / CD_FRAME_SIZE;
    t->hunk_bytes = h->hunkbytes;
    memset(t->hunk_no, 0xFF, sizeof t->hunk_no);
    if (h->unitbytes != CD_FRAME_SIZE || t->per_hunk == 0 || (t->hunks = malloc((size_t)KEPT * h->hunkbytes)) == NULL) {
        *err = "the CHD is not of a CD or GD-ROM";
        return 0;
    }
    for (i = 0; i < 99; i++) {
        int track = 0, frames = 0, pad = 0, pregap = 0, postgap = 0, n = 0;
        uint32_t len = 0;
        if (chd_get_metadata(t->chd, GDROM_TRACK_METADATA_TAG, i, meta, sizeof meta - 1, &len, NULL, NULL) ==
            CHDERR_NONE) {
            meta[len] = 0;
            n = sscanf(meta, GDROM_TRACK_METADATA_FORMAT, &track, type, sub, &frames, &pad, &pregap, pgtype, pgsub,
                       &postgap);
        } else if (chd_get_metadata(t->chd, CDROM_TRACK_METADATA2_TAG, i, meta, sizeof meta - 1, &len, NULL, NULL) ==
                   CHDERR_NONE) {
            meta[len] = 0;
            n = sscanf(meta, CDROM_TRACK_METADATA2_FORMAT, &track, type, sub, &frames, &pregap, pgtype, pgsub, &postgap);
        } else {
            break;
        }
        if (n < 4) {
            break;
        }
        if (track == 3) {
            static const int gaps[] = { 0, 150 };
            size_t g;
            t->data = strstr(type, "RAW") != NULL ? 16 : 0;
            if (strncmp(type, "MODE1", 5) != 0) {
                *err = "track 3 of the CHD is not a data track";
                return 0;
            }
            for (g = 0; g < sizeof gaps / sizeof gaps[0] + 1; g++) {
                t->frame0 = phys + (g < sizeof gaps / sizeof gaps[0] ? (uint32_t)gaps[g] : (uint32_t)pregap);
                if (has_volume(t)) {
                    return 1;
                }
            }
            break;
        }
        /* a GD-ROM's FRAMES take in PAD, the gap to the next track; every track is stored padded to 4 frames */
        phys += ((frames + CD_TRACK_PADDING - 1) / CD_TRACK_PADDING) * CD_TRACK_PADDING;
    }
    *err = "no ISO 9660 track 3 in the CHD (is it the Dreamcast disc?)";
    return 0;
}

static void track_close(TRACK* t) {
    if (t->f != NULL) {
        fclose(t->f);
    }
    if (t->chd != NULL) {
        chd_close(t->chd);
    }
    if (t->chd_f != NULL) {
        fclose(t->chd_f);
    }
    free(t->hunks);
    free(t->raw);
    memset(t, 0, sizeof *t);
}

/* a file in dir whose name ends so */
static int find_kind(const char* dir, const char* end, char* name, size_t size) {
    DIR* d = opendir(dir);
    struct dirent* e;
    int found = 0;
    while (d != NULL && !found && (e = readdir(d)) != NULL) {
        if (ends_with(e->d_name, end)) {
            snprintf(name, size, "%s", e->d_name);
            found = 1;
        }
    }
    if (d != NULL) {
        closedir(d);
    }
    return found;
}

/* the image in dir, the first of these kinds there */
const char* dc_image_find(const char* dir) {
    static const char* const kinds[] = { ".chd", ".gdi", ".cue", ".iso", "(track 3).bin", "track03.bin" };
    static char name[1100];
    size_t k;
    for (k = 0; k < sizeof kinds / sizeof kinds[0]; k++) {
        if (find_kind(dir, kinds[k], name, sizeof name)) {
            return name;
        }
    }
    return NULL;
}

typedef struct {
    char name[256];             /* its path in the disc, as A/B */
    uint32_t lba, bytes;
} ENTRY;

static ENTRY* entries;
static int n_entries, max_entries;

static int add_entry(const char* name, uint32_t lba, uint32_t bytes) {
    if (n_entries == max_entries) {
        int more = max_entries ? max_entries * 2 : 1024;
        ENTRY* e = realloc(entries, (size_t)more * sizeof *e);
        if (e == NULL) {
            return 0;
        }
        entries = e;
        max_entries = more;
    }
    snprintf(entries[n_entries].name, sizeof entries[n_entries].name, "%s", name);
    entries[n_entries].lba = lba;
    entries[n_entries].bytes = bytes;
    n_entries++;
    return 1;
}

/* a folder's files and those below it, as entries */
static int walk(TRACK* t, uint32_t lba, uint32_t bytes, const char* path, int depth) {
    uint32_t n = (bytes + SECTOR - 1) / SECTOR, i = 0;
    uint8_t* d;
    int ok = 1;
    if (depth > 8 || n == 0 || n > 4096 || (d = malloc((size_t)n * SECTOR)) == NULL) {
        return 0;
    }
    if (!read_sectors(t, lba, n, d)) {
        free(d);
        return 0;
    }
    while (ok && i + 33 < n * SECTOR) {
        uint8_t len = d[i], name_len = d[i + 32];
        char name[256], full[256];
        uint32_t at = d[i + 2] | d[i + 3] << 8 | d[i + 4] << 16 | (uint32_t)d[i + 5] << 24;
        uint32_t size = d[i + 10] | d[i + 11] << 8 | d[i + 12] << 16 | (uint32_t)d[i + 13] << 24;
        int k;
        if (len == 0) {
            i = (i / SECTOR + 1) * SECTOR;     /* records do not cross sectors */
            continue;
        }
        if (len < 34 || i + len > n * SECTOR || name_len == 0 || 33u + name_len > len) {
            break;
        }
        for (k = 0; k < name_len && d[i + 33 + k] != ';'; k++) {
            name[k] = (char)d[i + 33 + k];
        }
        name[k] = 0;
        if (name_len == 1 && (d[i + 33] == 0 || d[i + 33] == 1)) {
            /* the folder itself, and its parent */
        } else if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL || strcmp(name, "..") == 0 ||
                   strlen(path) + strlen(name) + 2 > sizeof full) {
            ok = 0;
        } else {
            size_t p = strlen(path);
            memcpy(full, path, p);
            if (p > 0) {
                full[p++] = '/';
            }
            memcpy(full + p, name, strlen(name) + 1);
            ok = (d[i + 25] & 2) ? walk(t, at, size, full, depth + 1) : add_entry(full, at, size);
        }
        i += len;
    }
    free(d);
    return ok;
}

static int by_lba(const void* a, const void* b) {
    const ENTRY* x = a;
    const ENTRY* y = b;
    /* 1ST_READ.BIN last: until it is there, the folder is not taken for a whole disc */
    int fx = strcmp(x->name, "1ST_READ.BIN") == 0, fy = strcmp(y->name, "1ST_READ.BIN") == 0;
    if (fx != fy) {
        return fx - fy;
    }
    return x->lba < y->lba ? -1 : x->lba > y->lba;
}

/* the folders a file's path needs, under dir */
static void make_dirs(const char* dir, const char* name) {
    char path[1400];
    const char* s;
    for (s = strchr(name, '/'); s != NULL; s = strchr(s + 1, '/')) {
        snprintf(path, sizeof path, "%s/%.*s", dir, (int)(s - name), name);
        make_dir(path);
    }
}

#if defined(__PSP__)
#include <pspkernel.h>
#define unpack_us() ((uint64_t)sceKernelGetSystemTimeWide())
#elif defined(__3DS__)
#include <3ds.h>
#define unpack_us() (svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000000))
#else
#include <time.h>
#define unpack_us() ((uint64_t)clock() * 1000000u / CLOCKS_PER_SEC)
#endif

/* the unpacked files made and written: on the PSP by a thread of their own while the next chunks are unpacked */
static uint64_t us_write, us_files;
static int w_failed;
static const char* w_dir;

#if defined(__PSP__)
#include <malloc.h>
#define WBUFS 4
#define WJOBS 32
enum { W_OPEN, W_WRITE, W_CLOSE, W_END };
static struct {
    int kind;
    const char* name;
    uint8_t* buf;
    uint32_t bytes;
} wjob[WJOBS];
static uint8_t* wbuf[WBUFS];
static unsigned w_put, w_next;
static SceUID w_jobs = -1, w_slots = -1, w_free = -1, w_thread = -1;

static int writer(SceSize n, void* p) {
    FILE* out = NULL;
    unsigned got = 0;
    (void)n;
    (void)p;
    for (;;) {
        char path[1400];
        int kind;
        uint64_t t0;
        sceKernelWaitSema(w_jobs, 1, NULL);
        t0 = unpack_us();
        kind = wjob[got % WJOBS].kind;
        if (kind == W_OPEN) {
            make_dirs(w_dir, wjob[got % WJOBS].name);
            snprintf(path, sizeof path, "%s/%s", w_dir, wjob[got % WJOBS].name);
            out = w_failed ? NULL : fopen(path, "wb");
            w_failed |= out == NULL;
            us_files += unpack_us() - t0;
        } else if (kind == W_WRITE) {
            w_failed |= out == NULL || fwrite(wjob[got % WJOBS].buf, 1, wjob[got % WJOBS].bytes, out) !=
                                           wjob[got % WJOBS].bytes;
            us_write += unpack_us() - t0;
            sceKernelSignalSema(w_free, 1);
        } else if (kind == W_CLOSE) {
            w_failed |= out != NULL && fclose(out) != 0;
            out = NULL;
            us_files += unpack_us() - t0;
        }
        got++;
        sceKernelSignalSema(w_slots, 1);
        if (kind == W_END) {
            return 0;
        }
    }
}

static void w_job(int kind, const char* name, uint8_t* buf, uint32_t bytes) {
    sceKernelWaitSema(w_slots, 1, NULL);
    wjob[w_put % WJOBS].kind = kind;
    wjob[w_put % WJOBS].name = name;
    wjob[w_put % WJOBS].buf = buf;
    wjob[w_put % WJOBS].bytes = bytes;
    w_put++;
    sceKernelSignalSema(w_jobs, 1);
}

static int w_start(const char* dir) {
    int i;
    w_dir = dir;
    w_failed = 0;
    w_put = w_next = 0;
    us_write = us_files = 0;
    for (i = 0; i < WBUFS; i++) {
        if ((wbuf[i] = memalign(64, (size_t)CHUNK * SECTOR)) == NULL) {
            return 0;
        }
    }
    w_jobs = sceKernelCreateSema("unpack jobs", 0, 0, WJOBS, NULL);
    w_slots = sceKernelCreateSema("unpack slots", 0, WJOBS, WJOBS, NULL);
    w_free = sceKernelCreateSema("unpack buffers", 0, WBUFS, WBUFS, NULL);
    w_thread = sceKernelCreateThread("unpack writer", writer, sceKernelGetThreadCurrentPriority() - 1, 0x4000,
                                     PSP_THREAD_ATTR_USER, NULL);
    if (w_thread >= 0 && sceKernelStartThread(w_thread, 0, NULL) < 0) {
        sceKernelDeleteThread(w_thread);
        w_thread = -1;
    }
    return w_jobs >= 0 && w_slots >= 0 && w_free >= 0 && w_thread >= 0;
}

/* a buffer to unpack the next chunk into, once the writer is done with it */
static uint8_t* w_buffer(void) {
    sceKernelWaitSema(w_free, 1, NULL);
    return wbuf[w_next++ % WBUFS];
}

static void w_open(const char* name) {
    w_job(W_OPEN, name, NULL, 0);
}

static void w_write(uint8_t* buf, uint32_t bytes) {
    w_job(W_WRITE, NULL, buf, bytes);
}

static void w_unused(uint8_t* buf) {
    (void)buf;
    sceKernelSignalSema(w_free, 1);
}

static void w_close(void) {
    w_job(W_CLOSE, NULL, NULL, 0);
}

/* all written, and the writer ended; 1 when all went well */
static int w_end(void) {
    int i;
    if (w_thread >= 0) {
        w_job(W_END, NULL, NULL, 0);
        sceKernelWaitThreadEnd(w_thread, NULL);
        sceKernelDeleteThread(w_thread);
    }
    if (w_jobs >= 0) {
        sceKernelDeleteSema(w_jobs);
    }
    if (w_slots >= 0) {
        sceKernelDeleteSema(w_slots);
    }
    if (w_free >= 0) {
        sceKernelDeleteSema(w_free);
    }
    w_thread = w_jobs = w_slots = w_free = -1;
    for (i = 0; i < WBUFS; i++) {
        free(wbuf[i]);
        wbuf[i] = NULL;
    }
    return !w_failed;
}
#else
static FILE* w_out;
static uint8_t* w_buf;

static int w_start(const char* dir) {
    w_dir = dir;
    w_failed = 0;
    us_write = us_files = 0;
    return (w_buf = malloc((size_t)CHUNK * SECTOR)) != NULL;
}

static uint8_t* w_buffer(void) {
    return w_buf;
}

static void w_open(const char* name) {
    char path[1400];
    uint64_t t0 = unpack_us();
    make_dirs(w_dir, name);
    snprintf(path, sizeof path, "%s/%s", w_dir, name);
    w_out = w_failed ? NULL : fopen(path, "wb");
    w_failed |= w_out == NULL;
    us_files += unpack_us() - t0;
}

static void w_write(uint8_t* buf, uint32_t bytes) {
    uint64_t t0 = unpack_us();
    w_failed |= w_out == NULL || fwrite(buf, 1, bytes, w_out) != bytes;
    us_write += unpack_us() - t0;
}

static void w_unused(uint8_t* buf) {
    (void)buf;
}

static void w_close(void) {
    uint64_t t0 = unpack_us();
    w_failed |= w_out != NULL && fclose(w_out) != 0;
    w_out = NULL;
    us_files += unpack_us() - t0;
}

static int w_end(void) {
    free(w_buf);
    w_buf = NULL;
    return !w_failed;
}
#endif

/* each file written under its own name, a mark beside them until the last is done (no name made and changed for
   each one: a FAT folder of 700 files is searched through at every create, remove and rename) */
const char* const dc_unpack_mark = "UNPACK.TMP";

const char* dc_image_unpack(const char* dir, void (*progress)(uint32_t done, uint32_t total)) {
    const char* image = dc_image_find(dir);
    const char* err = NULL;
    char path[1400], mark[1400];
    uint8_t vol[SECTOR];
    uint64_t total = 0, done = 0, us_read = 0, wall, t0;
    TRACK t;
    int ok = 0, i;
    memset(&t, 0, sizeof t);
    if (image == NULL) {
        return fail("no disc image in %s", dir);
    }
    host_log("disc image: unpacking %s/%s", dir, image);
    snprintf(path, sizeof path, "%s/%s", dir, image);
    t.raw = malloc((size_t)CHUNK * 2352);
    if (t.raw == NULL) {
        return fail("%s", "out of memory");
    }
    if (ends_with(image, ".chd")) {
        ok = chd_track(&t, path, &err);
    } else if (ends_with(image, ".gdi")) {
        ok = gdi_track(&t, dir, image);
    } else if (ends_with(image, ".cue")) {
        ok = cue_track(&t, dir, image);
    } else {
        ok = file_track(&t, path, 0, ends_with(image, ".iso") ? SECTOR : 2352);
    }
    if (!ok) {
        track_close(&t);
        return fail("%s", err != NULL ? err : "no ISO 9660 track 3 found in the image (is it the Dreamcast disc?)");
    }
    n_entries = 0;
    if (!read_sectors(&t, HD_FIRST + 16, 1, vol) ||
        !walk(&t, vol[158] | vol[159] << 8 | vol[160] << 16 | (uint32_t)vol[161] << 24,
              vol[166] | vol[167] << 8 | vol[168] << 16 | (uint32_t)vol[169] << 24, "", 0) ||
        n_entries == 0) {
        track_close(&t);
        return fail("%s", "the image's file system could not be read");
    }
    qsort(entries, n_entries, sizeof *entries, by_lba);
    for (i = 0; i < n_entries; i++) {
        total += entries[i].bytes;
    }
    snprintf(mark, sizeof mark, "%s/%s", dir, dc_unpack_mark);
    {
        FILE* m = fopen(mark, "wb");
        ok = m != NULL;
        if (m != NULL) {
            fclose(m);
        }
    }
    ok = w_start(dir) && ok;
    wall = unpack_us();
    for (i = 0; ok && i < n_entries; i++) {
        const ENTRY* e = &entries[i];
        uint32_t left = e->bytes, lba = e->lba;
        w_open(e->name);
        while (ok && left > 0 && !w_failed) {
            uint32_t k = (left + SECTOR - 1) / SECTOR, bytes;
            uint8_t* buf = w_buffer();
            k = k < CHUNK ? k : CHUNK;
            bytes = k * SECTOR < left ? k * SECTOR : left;
            t0 = unpack_us();
            if (!read_sectors(&t, lba, k, buf)) {
                err = "the image could not be read";
                ok = 0;
                w_unused(buf);
            } else {
                w_write(buf, bytes);
            }
            us_read += unpack_us() - t0;
            lba += k;
            left -= bytes;
            done += bytes;
            if (progress != NULL) {
                progress((uint32_t)(done >> 10), (uint32_t)(total >> 10));
            }
        }
        w_close();
    }
    if (!w_end() && ok) {
        err = "a file could not be written (is the card full?)";
        ok = 0;
    }
    track_close(&t);
    if (!ok) {
        return fail("%s", err != NULL ? err : "the disc's files could not be written");
    }
    remove(mark);
    host_log("disc image: %d files, %lu KB in %.1f s; reading and unpacking %.1f s, writing %.1f s, making and closing "
             "the files %.1f s", n_entries, (unsigned long)(total >> 10), (unpack_us() - wall) / 1e6, us_read / 1e6,
             us_write / 1e6, us_files / 1e6);
    return NULL;
}

/* ---- the disc read in place from its image: its files found in a directory kept in memory, read under a lock
   (the game's thread and the readers' share the image's one handle) ---- */

#if defined(__3DS__)
#include <3ds.h>
/* a reader that lets go and takes it again at once would keep the game's thread waiting for a whole file, so a
   reader (any thread but the one that mounted the image, the game's) that lets go while another waits stands aside
   until that one has it */
static LightLock disc_lock;
static int disc_waiting;
static u32 game_thread;
static void disc_lock_init(void) {
    LightLock_Init(&disc_lock);
    disc_waiting = 0;
    svcGetThreadId(&game_thread, CUR_THREAD_HANDLE);
}
static void disc_take(void) {
    __atomic_add_fetch(&disc_waiting, 1, __ATOMIC_ACQ_REL);
    LightLock_Lock(&disc_lock);
    __atomic_sub_fetch(&disc_waiting, 1, __ATOMIC_ACQ_REL);
}
static void disc_give(void) {
    u32 me;
    int i;
    LightLock_Unlock(&disc_lock);
    svcGetThreadId(&me, CUR_THREAD_HANDLE);
    for (i = 0; me != game_thread && i < 200 && __atomic_load_n(&disc_waiting, __ATOMIC_ACQUIRE) > 0; i++) {
        svcSleepThread(50000);
    }
}
#define DISC_LOCK() disc_take()
#define DISC_UNLOCK() disc_give()
#elif defined(__PSP__) || defined(MVC2_MISTER)
void host_disc_lock(int on);    /* psp_reader.c, mister_reader.c */
static void disc_lock_init(void) {
}
#define DISC_LOCK() host_disc_lock(1)
#define DISC_UNLOCK() host_disc_lock(0)
#else
static void disc_lock_init(void) {
}
#define DISC_LOCK()
#define DISC_UNLOCK()
#endif

#define MOUNT_STEP 32           /* sectors read under the lock at a time */

static TRACK mounted;
static ENTRY* names;            /* by name */
static int n_names;
static uint8_t* bounce;         /* MOUNT_STEP sectors */

static int by_name(const void* a, const void* b) {
    const ENTRY* x = a;
    const ENTRY* y = b;
#ifdef _WIN32
    return _stricmp(x->name, y->name);
#else
    return strcasecmp(x->name, y->name);
#endif
}

/* a track 3 found in the image named image in dir */
static int open_track(TRACK* t, const char* dir, const char* image, const char** err) {
    char path[1400];
    memset(t, 0, sizeof *t);
    snprintf(path, sizeof path, "%s/%s", dir, image);
    if ((t->raw = malloc((size_t)CHUNK * 2352)) == NULL) {
        *err = "out of memory";
        return 0;
    }
    if (ends_with(image, ".chd")) {
        return chd_track(t, path, err);
    }
    if (ends_with(image, ".gdi")) {
        return gdi_track(t, dir, image);
    }
    if (ends_with(image, ".cue")) {
        return cue_track(t, dir, image);
    }
    return file_track(t, path, 0, ends_with(image, ".iso") ? SECTOR : 2352);
}

/* the files of track 3, in entries */
static int read_directory(TRACK* t) {
    uint8_t vol[SECTOR];
    n_entries = 0;
    return read_sectors(t, HD_FIRST + 16, 1, vol) &&
           walk(t, vol[158] | vol[159] << 8 | vol[160] << 16 | (uint32_t)vol[161] << 24,
                vol[166] | vol[167] << 8 | vol[168] << 16 | (uint32_t)vol[169] << 24, "", 0) &&
           n_entries > 0;
}

int dc_image_mount(const char* dir, int chd_ok) {
    const char* image = dc_image_find(dir);
    const char* err = NULL;
    TRACK t;
    dc_image_unmount();
    if (image == NULL || (!chd_ok && ends_with(image, ".chd"))) {
        return 0;
    }
    if (!open_track(&t, dir, image, &err) || !read_directory(&t) || (bounce = malloc(MOUNT_STEP * SECTOR)) == NULL ||
        (names = malloc((size_t)n_entries * sizeof *names)) == NULL) {
        host_log("disc image: %s/%s not read in place: %s", dir, image,
                 err != NULL ? err : "no ISO 9660 track 3 in it, or no memory");
        track_close(&t);
        free(bounce);
        bounce = NULL;
        return 0;
    }
    memcpy(names, entries, (size_t)n_entries * sizeof *names);
    n_names = n_entries;
    qsort(names, n_names, sizeof *names, by_name);
    free(entries);
    entries = NULL;
    n_entries = max_entries = 0;
    mounted = t;
    disc_lock_init();
    host_log("disc image: %s/%s read in place, %d files", dir, image, n_names);
    return 1;
}

void dc_image_unmount(void) {
    track_close(&mounted);
    free(names);
    free(bounce);
    names = NULL;
    bounce = NULL;
    n_names = 0;
}

int dc_image_mounted(void) {
    return names != NULL;
}

int dc_image_chd(void) {
    return names != NULL && mounted.chd != NULL;
}

int dc_image_file(const char* name, uint32_t* lba, uint32_t* bytes) {
    ENTRY key;
    const ENTRY* e;
    if (names == NULL || strlen(name) >= sizeof key.name) {
        return 0;
    }
    strcpy(key.name, name);
    if ((e = bsearch(&key, names, n_names, sizeof *names, by_name)) == NULL) {
        return 0;
    }
    *lba = e->lba;
    *bytes = e->bytes;
    return 1;
}

size_t dc_image_read(uint32_t lba, uint32_t bytes, uint32_t off, void* to, size_t want) {
    uint8_t* out = to;
    size_t got = 0;
    if (off >= bytes) {
        return 0;
    }
    want = want < bytes - off ? want : bytes - off;
    while (got < want) {
        uint32_t at = off + (uint32_t)got, skip = at % SECTOR, k;
        size_t n;
        int ok;
        DISC_LOCK();
        if (mounted.chd == NULL && mounted.size == SECTOR) {
            /* whole runs of a 2048-byte image read straight in */
            n = want - got < (size_t)MOUNT_STEP * SECTOR ? want - got : (size_t)MOUNT_STEP * SECTOR;
            ok = seek(mounted.f, mounted.at + (long long)(lba - HD_FIRST) * SECTOR + at) == 0 &&
                 fread(out + got, 1, n, mounted.f) == n;
        } else {
            k = (uint32_t)((skip + (want - got) + SECTOR - 1) / SECTOR);
            k = k < MOUNT_STEP ? k : MOUNT_STEP;
            n = (size_t)k * SECTOR - skip;
            n = n < want - got ? n : want - got;
            ok = read_sectors(&mounted, lba + at / SECTOR, k, bounce);
            if (ok) {
                memcpy(out + got, bounce + skip, n);
            }
        }
        DISC_UNLOCK();
        if (!ok) {
            break;
        }
        got += n;
    }
    return got;
}
