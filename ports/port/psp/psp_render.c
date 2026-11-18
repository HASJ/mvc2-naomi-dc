/*
 * psp_render.c  The frame's lists drawn by the GE, straight onto the 480x272 screen
 *
 * A vertex goes in as x * w, y * w, w: the GE's own divide by w gives back the game's pixel, scaled to the screen,
 * and its textures and colours are perspective-correct. Depth: the viewport's z scale and centre lay the frame's
 * 1/w values out on the 16-bit depth buffer, the 3D triangles' range evenly and each 1/w of a flat triangle (a
 * sprite) outside it on a step of its own. Textures are converted once and read from main RAM: 16-bit ones with
 * red and blue swapped, the palette ones as 4 or 8-bit indices with the palette loaded as a CLUT.
 */

#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pspgu.h>
#include <psppower.h>
#include <pspkernel.h>
#include <pspthreadman.h>

#include "../nl/nl_port.h"
#include "psp_front.h"
#include "host_cache.h"
#include "psp_ge.h"
#include "../tools/rd_frame_file.h"
#include "psp_me.h"

void host_log(const char* fmt, ...);

#define SLOTS 49152                 /* depths a float24 centre holds exactly: each below 32768, every other one above */
#define SLOT_MARGIN 8
#define MAX_TEX 3072
#define HASH_SIZE 8192

PSP_RSTATS psp_rstats;

/* The GE draws a frame while the CPU makes the next: each frame's list, vertices and palette are one of two, and a
   texture or mesh the frame before drew is kept until it is drawn. */
static unsigned int* clists[2];     /* the frame's list, called from a short direct one */
unsigned int* clist;

static unsigned int __attribute__((aligned(64))) dlists[2][256];
static VTX* verts_buf[2];
VTX* verts;
static VTX* over_buf[2];
VTX* over;                          /* the fog over the opaque list: its colour, the table's value the alpha */
uint32_t frame_no;
static int ready, failed;
int screen16;                       /* the screen's buffers at 16 bits a pixel, dithered, as the DC's */
static int no_mips;                 /* MVC2_GE_MIPS=0: no levels made here */

/* ---- textures ---- */

/* The GE's copies of the game's textures, in a fixed budget: small ones (a sprite's tiles) in a block of main RAM of
   their own, in slots of 512 bytes to 4 KB, the others in the VRAM the screen leaves, then a block of main RAM. A copy
   is kept by its texels' serial, which the same texels loaded again have again; for room the least recently drawn of
   its size are let go, never one the frame being made draws. */
#define SLOT_SIZES 4                /* 512 << k bytes */
#define BIG SLOT_SIZES              /* the big ones' kind */
#define SMALL_TEX (512u << (SLOT_SIZES - 1))
#define SMALL_POOL (1u << 20)
#define BIG_POOL (3u << 20)


static GTEX gtex[MAX_TEX];
static int16_t lru_first[BIG + 1], lru_last[BIG + 1], free_first;
static uint8_t* slot_free[SLOT_SIZES];  /* each slot let go points to the next */
static uint32_t slot_bytes;
static uint8_t *slot_new, *slot_end;    /* the small ones' block not yet used */
static int ngtex;
static int16_t hash[HASH_SIZE];     /* gtex's indices, -1 empty */
static uint32_t pal_buf[2][1024] __attribute__((aligned(64)));    /* rd_palette as ABGR */
uint32_t* pal = pal_buf[0];
static uint32_t pal_serial[2] = { 0xFFFFFFFFu, 0xFFFFFFFFu };

/* a pool's free spans, in address order, 16-byte granules */
#define MAX_SPANS (MAX_TEX + 2)

typedef struct {
    uint8_t* base;
    uint32_t size, used;
    uint32_t at[MAX_SPANS], len[MAX_SPANS];
    int n;
} POOL;

static POOL pools[2];               /* VRAM and main RAM */

static void pool_init(POOL* p, void* base, uint32_t size) {
    p->base = base;
    p->size = size & ~15u;
    p->used = 0;
    p->at[0] = 0;
    p->len[0] = p->size;
    p->n = p->size != 0;
}

static void drop_span(POOL* p, int i) {
    memmove(&p->at[i], &p->at[i + 1], (size_t)(p->n - i - 1) * sizeof *p->at);
    memmove(&p->len[i], &p->len[i + 1], (size_t)(p->n - i - 1) * sizeof *p->len);
    p->n--;
}

/* the smallest span n fits in; NULL when none */
static uint8_t* pool_take(POOL* p, uint32_t n) {
    int i, best = -1;
    uint32_t at;
    for (i = 0; i < p->n; i++) {
        if (p->len[i] >= n && (best < 0 || p->len[i] < p->len[best])) {
            best = i;
            if (p->len[i] == n) {
                break;
            }
        }
    }
    if (best < 0) {
        return NULL;
    }
    at = p->at[best];
    p->at[best] += n;
    p->len[best] -= n;
    if (p->len[best] == 0) {
        drop_span(p, best);
    }
    p->used += n;
    return p->base + at;
}

/* n bytes at mem given back, joined to the spans either side */
static void pool_give(POOL* p, uint8_t* mem, uint32_t n) {
    uint32_t at;
    int i;
    if (n == 0) {
        return;
    }
    at = (uint32_t)(mem - p->base);
    p->used -= n;
    for (i = 0; i < p->n && p->at[i] < at; i++) {
    }
    if (i > 0 && p->at[i - 1] + p->len[i - 1] == at) {
        p->len[i - 1] += n;
        if (i < p->n && at + n == p->at[i]) {
            p->len[i - 1] += p->len[i];
            drop_span(p, i);
        }
    } else if (i < p->n && at + n == p->at[i]) {
        p->at[i] = at;
        p->len[i] += n;
    } else if (p->n < MAX_SPANS) {
        memmove(&p->at[i + 1], &p->at[i], (size_t)(p->n - i) * sizeof *p->at);
        memmove(&p->len[i + 1], &p->len[i], (size_t)(p->n - i) * sizeof *p->len);
        p->at[i] = at;
        p->len[i] = n;
        p->n++;
    }
}

static unsigned hash_of(uint32_t serial) {
    return serial * 2654435761u >> 19;
}

static GTEX* find(uint32_t serial) {
    unsigned h = hash_of(serial);
    while (hash[h] >= 0) {
        if (gtex[hash[h]].serial == serial) {
            return &gtex[hash[h]];
        }
        h = (h + 1) & (HASH_SIZE - 1);
    }
    return NULL;
}

static void put_hash(int i) {
    unsigned h = hash_of(gtex[i].serial);
    while (hash[h] >= 0) {
        h = (h + 1) & (HASH_SIZE - 1);
    }
    hash[h] = (int16_t)i;
}

/* entry i out of the hash, those after it in its run moved up to where a search finds them */
static void unhash(int i) {
    unsigned h = hash_of(gtex[i].serial), j;
    while (hash[h] != i) {
        h = (h + 1) & (HASH_SIZE - 1);
    }
    for (j = h;;) {
        unsigned k;
        j = (j + 1) & (HASH_SIZE - 1);
        if (hash[j] < 0) {
            break;
        }
        k = hash_of(gtex[hash[j]].serial);
        if (j > h ? k <= h || k > j : k <= h && k > j) {
            hash[h] = hash[j];
            h = j;
        }
    }
    hash[h] = -1;
}

static void unlink_lru(GTEX* e) {
    if (e->prev >= 0) {
        gtex[e->prev].next = e->next;
    } else {
        lru_first[e->kind] = e->next;
    }
    if (e->next >= 0) {
        gtex[e->next].prev = e->prev;
    } else {
        lru_last[e->kind] = e->prev;
    }
}

static void append_lru(GTEX* e) {
    int16_t i = (int16_t)(e - gtex);
    e->prev = lru_last[e->kind];
    e->next = -1;
    if (e->prev >= 0) {
        gtex[e->prev].next = i;
    } else {
        lru_first[e->kind] = i;
    }
    lru_last[e->kind] = i;
}

static void cache_init(void) {
    int i;
    memset(hash, 0xFF, sizeof hash);
    for (i = 0; i < MAX_TEX; i++) {
        gtex[i].next = (int16_t)(i + 1 < MAX_TEX ? i + 1 : -1);
    }
    free_first = 0;
    for (i = 0; i <= BIG; i++) {
        lru_first[i] = lru_last[i] = -1;
    }
}

static POOL* pool_of(const uint8_t* mem) {
    return mem >= pools[0].base && mem < pools[0].base + pools[0].size ? &pools[0] : &pools[1];
}

static void let_go(GTEX* e) {
    if (e->ext != NULL) {
        pool_give(pool_of(e->ext), e->ext, e->ext_bytes);
        e->ext = NULL;
    }
    if (e->kind == BIG) {
        pool_give(pool_of(e->mem), e->mem, e->bytes);
    } else {
        memcpy(e->mem, &slot_free[e->kind], sizeof(uint8_t*));
        slot_free[e->kind] = e->mem;
        slot_bytes -= 512u << e->kind;
    }
    unhash((int)(e - gtex));
    unlink_lru(e);
    e->serial = 0;
    e->mem = NULL;
    e->next = free_first;
    free_first = (int16_t)(e - gtex);
    ngtex--;
}

static int slot_kind(uint32_t n) {
    int k = 0;
    while (k < BIG && n > 512u << k) {
        k++;
    }
    return k;
}

/* n bytes of kind k: a slot when it is small, or from the big ones' pools (for a small one where spill), main RAM's
   first where ram_first */
static uint8_t* take(int* k, uint32_t n, int ram_first, int spill) {
    uint8_t* mem = NULL;
    int i;
    if (*k < BIG) {
        uint32_t size = 512u << *k;
        if (slot_free[*k] != NULL) {
            mem = slot_free[*k];
            memcpy(&slot_free[*k], mem, sizeof(uint8_t*));
        } else if (slot_end - slot_new >= (ptrdiff_t)size) {
            mem = slot_new;
            slot_new += size;
        }
        if (mem != NULL) {
            slot_bytes += size;
            return mem;
        }
        if (!spill) {
            return NULL;
        }
    }
    for (i = 0; i < 2 && mem == NULL; i++) {
        mem = pool_take(&pools[i ^ ram_first], n);
    }
    *k = BIG;
    return mem;
}

static uint32_t flight_from;        /* the first frame the GE may not have drawn: the one before the frame made, or it */

/* drawn by the frame being made or the one the GE may be drawing */
static int in_flight(uint32_t used) {
    return used >= flight_from;
}

/* n bytes for a new copy and an entry for it, the least recently drawn of its kind (then of the big ones) let go until
   both are had; NULL when even that can't make room. A small one takes from the big ones' pools only when none of its
   kind can be let go, so that tiles don't break up the room big ones need */
static GTEX* room_for(uint32_t n, uint8_t** mem, int ram_first) {
    int kind = n <= SMALL_TEX ? slot_kind(n) : BIG, k, got, own;
    GTEX* e;
    for (;;) {
        got = kind;
        own = lru_first[kind] >= 0 && !in_flight(gtex[lru_first[kind]].used);
        if (free_first >= 0 && (*mem = take(&got, n, ram_first, !own)) != NULL) {
            break;
        }
        k = own ? lru_first[kind] : lru_first[BIG];
        if (k < 0 || in_flight(gtex[k].used)) {
            return NULL;
        }
        let_go(&gtex[k]);
        psp_rstats.tex_evicted++;
    }
    e = &gtex[free_first];
    free_first = e->next;
    e->ext = NULL;
    e->kind = (uint8_t)got;
    ngtex++;
    return e;
}

static int psm_of(int format) {
    switch (format) {
    case RD_ARGB1555: return GU_PSM_5551;
    case RD_RGB565: return GU_PSM_5650;
    case RD_ARGB4444: return GU_PSM_4444;
    case RD_PAL4: return GU_PSM_T4;
    case RD_PAL8: return GU_PSM_T8;
    default: return -1;
    }
}

static int min_tbw(int psm) {
    return psm == GU_PSM_T4 ? 32 : psm == GU_PSM_T8 ? 16 : 8;
}

static uint32_t row_bytes(int psm, int tbw) {
    return psm == GU_PSM_T4 ? (uint32_t)tbw / 2 : psm == GU_PSM_T8 ? (uint32_t)tbw : (uint32_t)tbw * 2;
}

/* a level's rows: 8 at least where it is swizzled */
static int level_rows(const GTEX* e, int h) {
    return e->swizzled ? (h + 7) & ~7 : h;
}

/* row y of a level rb bytes a row, swizzled: each 16 bytes in the 16 by 8 block it is in */
static void put_swizzled(uint8_t* level, const uint32_t* row, int y, uint32_t rb) {
    uint32_t* o = (uint32_t*)(level + (y >> 3) * rb * 8 + (y & 7) * 16);
    uint32_t x;
    for (x = 0; x < rb / 4; x += 4, o += 32) {
        o[0] = row[x];
        o[1] = row[x + 1];
        o[2] = row[x + 2];
        o[3] = row[x + 3];
    }
}

/* a 16-bit level w by h, w a multiple of 8, from rows sw wide straight into the GE's 16-byte by 8-row blocks, two
   texels a word with red and blue swapped: the word's bits in keep stay, those in lo swap with those sh above */
static void convert16_swizzled(int format, const uint16_t* src, int sw, uint8_t* out, int w, int h, uint32_t rb) {
    uint32_t keep = format == RD_RGB565 ? 0x07E007E0u : format == RD_ARGB1555 ? 0x83E083E0u : 0xF0F0F0F0u;
    uint32_t lo = format == RD_ARGB4444 ? 0x000F000Fu : 0x001F001Fu;
    int sh = format == RD_RGB565 ? 11 : format == RD_ARGB1555 ? 10 : 8, x, y;
    host_lines_new(out, rb * (uint32_t)h, out + rb * (uint32_t)h);
    for (y = 0; y < h; y++) {
        const uint16_t* s = src + (size_t)y * sw;
        uint32_t* o = (uint32_t*)(out + (y >> 3) * rb * 8 + (y & 7) * 16);
        for (x = 0; x < w; x += 8, s += 8, o += 32) {
            uint32_t a, b, c, d;
            if (((uintptr_t)s & 3) == 0) {
                const uint32_t* s4 = __builtin_assume_aligned(s, 4);
                a = s4[0];
                b = s4[1];
                c = s4[2];
                d = s4[3];
            } else {
                a = s[0] | (uint32_t)s[1] << 16;
                b = s[2] | (uint32_t)s[3] << 16;
                c = s[4] | (uint32_t)s[5] << 16;
                d = s[6] | (uint32_t)s[7] << 16;
            }
            o[0] = (a & keep) | (a >> sh & lo) | (a & lo) << sh;
            o[1] = (b & keep) | (b >> sh & lo) | (b & lo) << sh;
            o[2] = (c & keep) | (c >> sh & lo) | (c & lo) << sh;
            o[3] = (d & keep) | (d >> sh & lo) | (d & lo) << sh;
        }
    }
}

/* w by h texels from a level sw wide, every step'th, into rows tbw wide, swizzled or not */
static void convert(const RD_TEXTURE* t, const void* src, int sw, int step, uint8_t* out, int w, int h, int tbw,
                    int swizzled) {
    static uint32_t row[256];       /* a row being swizzled: 512 16-bit texels */
    uint32_t rb = row_bytes(psm_of(t->format), tbw);
    int x, y;
    if (swizzled && step == 1 && t->format <= RD_ARGB4444 && w % 8 == 0 && tbw == w && h % 8 == 0) {
        convert16_swizzled(t->format, src, sw, out, w, h, rb);
        return;
    }
    for (y = 0; y < h; y++) {
        uint8_t* line = swizzled ? (uint8_t*)row : out + (size_t)y * rb;
        if (t->format == RD_PAL4 || t->format == RD_PAL8) {
            const uint8_t* s = (const uint8_t*)src + (size_t)y * step * sw;
            uint8_t* o = line;
            if (t->format == RD_PAL8) {
                for (x = 0; x < w; x++) {
                    o[x] = s[x * step];
                }
            } else {
                for (x = 0; x < w; x += 2) {
                    o[x / 2] = (uint8_t)((s[x * step] & 15) | (x + 1 < w ? (s[(x + 1) * step] & 15) << 4 : 0));
                }
            }
        } else {
            const uint16_t* s = (const uint16_t*)src + (size_t)y * step * sw;
            uint16_t* o = (uint16_t*)line;
            for (x = 0; x < w; x++) {
                uint32_t p = s[x * step];
                switch (t->format) {
                case RD_RGB565: p = (p & 0x07E0u) | p >> 11 | (p & 0x1Fu) << 11; break;
                case RD_ARGB1555: p = (p & 0x83E0u) | (p >> 10 & 0x1Fu) | (p & 0x1Fu) << 10; break;
                default: p = (p & 0xF0F0u) | (p >> 8 & 0xFu) | (p & 0xFu) << 8; break;
                }
                o[x] = (uint16_t)p;
            }
        }
        if (swizzled) {
            put_swizzled(out, row, y, rb);
        }
    }
}

/* v's bits spread to the even bits */
static uint32_t spread_bits(uint32_t v) {
    uint32_t r = 0;
    int k;
    for (k = 0; v >> k; k++) {
        r |= (v >> k & 1) << (2 * k);
    }
    return r;
}

/* A PAL4 square's twiddled source straight to the GE's swizzled 4-bit texels. Its 16-bit words are its 2x2 blocks,
   twiddled at half the size: texels (x, y), (x, y + 1), (x + 1, y), (x + 1, y + 1) from the low nibble up. */
static void pal4_from_twiddled(const uint16_t* src, int n, uint8_t* out, uint32_t rb) {
    static uint32_t twid[256], col[256];
    static int made_n;
    int i, j, m = n / 2;
    if (made_n != n) {
        made_n = n;
        for (i = 0; i < m; i++) {
            twid[i] = spread_bits((uint32_t)i);
            col[i] = (uint32_t)(i >> 4) * 128 + (uint32_t)(i & 15);
        }
    }
    /* four words at a time: blocks (i, j), (i, j + 1), (i + 1, j), (i + 1, j + 1), two bytes of four rows; with i and
       j even they are 8 bytes from a multiple of 8 */
    for (j = 0; j < m; j += 2) {
        uint32_t y = (uint32_t)j * 2, at = (y >> 3) * rb * 8 + (y & 7) * 16, tj = twid[j];
        for (i = 0; i < m; i += 2) {
            const uint16_t* q = src + (twid[i] << 1 | tj);
            uint32_t w0, w1;
            if (((uintptr_t)src & 3) == 0) {
                const void* q4 = __builtin_assume_aligned(q, 4);
                memcpy(&w0, q4, 4);
                memcpy(&w1, (const uint8_t*)q4 + 4, 4);
            } else {
                w0 = q[0] | (uint32_t)q[1] << 16;
                w1 = q[2] | (uint32_t)q[3] << 16;
            }
            /* t: even rows' bytes (row y in the low half, y + 2 in the high), u: odd rows' */
            uint32_t t0 = (w0 & 0x000F000Fu) | (w0 >> 4 & 0x00F000F0u);
            uint32_t u0 = (w0 >> 4 & 0x000F000Fu) | (w0 >> 8 & 0x00F000F0u);
            uint32_t t1 = (w1 & 0x000F000Fu) | (w1 >> 4 & 0x00F000F0u);
            uint32_t u1 = (w1 >> 4 & 0x000F000Fu) | (w1 >> 8 & 0x00F000F0u);
            uint16_t* o = (uint16_t*)(out + at + col[i]);
            o[0] = (uint16_t)((t0 & 0xFF) | (t1 & 0xFF) << 8);
            o[8] = (uint16_t)((u0 & 0xFF) | (u1 & 0xFF) << 8);
            o[16] = (uint16_t)((t0 >> 16 & 0xFF) | (t1 >> 8 & 0xFF00));
            o[24] = (uint16_t)((u0 >> 16 & 0xFF) | (u1 >> 8 & 0xFF00));
        }
    }
}

/* t is a PAL4 square kept as its twiddled source, at most 512 and with no mip levels, made at 8 or more */
static int pal4_square(const RD_TEXTURE* t) {
    return t->format == RD_PAL4 && t->unpack != NULL && t->width == t->height && t->width >= 8 && t->width <= 512 &&
           t->levels == 0;
}

static void log_once(int* done, const char* what) {
    if (!*done) {
        *done = 1;
        host_log("GE: %s", what);
    }
}

/* The sprites' small PAL4 tiles drawn in through mode, clamped, in cells of one 4-bit texture in VRAM, so that a run
   of them in one look is one batch. A tile is converted once into main RAM with a border of its edge texels, which
   filtering reads as the clamp would, and copied into a cell by the GE as the list asks for it. MVC2_GE_ATLAS=0: each
   tile a texture of its own. */
#define CELL_MAP 1024
#define KEPT 256
#define KEPT_MAP 512

typedef struct {
    uint32_t serial, used;
} CELL;

static CELL cells[CELLS], kept[KEPT];
/* the frame's copies into cells, a list of their own called before the frame's */
#define UP_WORDS (CELLS * UP_COPY_WORDS + 1)
#define UP_COPY_WORDS 10
static uint32_t up_lists[2][UP_WORDS] __attribute__((aligned(64)));
static uint32_t *up_list, *up_cur;

static int16_t cell_of[CELL_MAP];   /* by serial, -1 none: an entry whose serial is another's is not this one's */
static int16_t kept_of[KEPT_MAP];
static int cell_hand, kept_hand;
static int kept_lo = KEPT, kept_hi;    /* those made for the frame, written back with its list */
static uint8_t* atlas;              /* NULL: none */
static uint8_t* kept_rows;
GTEX atlas_tex;

static int atlas_tile(const RD_TEXTURE* t) {
    return t->format == RD_PAL4 && t->width == t->height && (t->width == 8 || t->width == 16 || t->width == 32) &&
           t->levels == 0 && t->texels != NULL && t->serial != 0;
}

/* the next of n entries from *hand not drawn by a frame not yet drawn; -1 if none */
static int take_entry(CELL* e, int n, int* hand) {
    int i, k;
    for (i = 0; i < n && in_flight(e[*hand].used); i++) {
        *hand = *hand + 1 < n ? *hand + 1 : 0;
    }
    if (i == n) {
        return -1;
    }
    k = *hand;
    *hand = *hand + 1 < n ? *hand + 1 : 0;
    return k;
}


/* the texels on a side of tile t its cell holds: those drawn, with the border past them */
static int tile_side(const RD_TEXTURE* t) {
    return t->corner != 0 ? t->corner : t->width;
}

/* tile t's bordered rows in main RAM, made if need be; NULL if there is no room */
static const uint8_t* kept_tile(const RD_TEXTURE* t) {
    uint8_t tile[32 * 16] __attribute__((aligned(4)));
    int k = kept_of[t->serial & (KEPT_MAP - 1)], n = tile_side(t), nb = n / 2, w = t->width, x, y;
    uint8_t* o;
    if (k >= 0 && kept[k].serial == t->serial) {
        kept[k].used = frame_no;
        return kept_rows + k * KEPT_BYTES;
    }
    if ((k = take_entry(kept, KEPT, &kept_hand)) < 0) {
        return NULL;
    }
    kept[k].serial = t->serial;
    kept[k].used = frame_no;
    kept_of[t->serial & (KEPT_MAP - 1)] = (int16_t)k;
    /* the source's lines asked for at once; the entry's taken without a read, the GE reading only what is written */
    for (x = 0; x < (pal4_square(t) ? n * n / 2 : n * w); x += 64) {
        host_prefetch((const uint8_t*)t->texels + x);
    }
    host_lines_new(kept_rows + k * KEPT_BYTES, KEPT_BYTES, kept_rows + KEPT * KEPT_BYTES);
    /* the tile's rows, 16 bytes apart */
    if (pal4_square(t)) {
        pal4_from_twiddled(t->texels, n, tile, 16);
    } else {
        const uint8_t* src = t->texels;
        for (y = 0; y < n; y++) {
            for (x = 0; x < n; x += 2) {
                tile[y * 16 + x / 2] = (uint8_t)((src[y * w + x] & 15) | (src[y * w + x + 1] & 15) << 4);
            }
        }
    }
    /* each row with its border, the first and last twice: a texel of the border, the row a byte on, a texel of the
       border; in words, the two bytes after unused */
    o = kept_rows + k * KEPT_BYTES;
    for (y = -1; y <= n; y++, o += KEPT_ROW) {
        const uint8_t* r = tile + 16 * (y < 0 ? 0 : y < n ? y : n - 1);
        uint32_t* w = (uint32_t*)o;
        uint32_t carry = (r[0] & 15u) << 4, v;
        for (x = 0; x < nb / 4; x++) {
            memcpy(&v, r + 4 * x, 4);
            w[x] = carry | v << 8;
            carry = v >> 24;
        }
        w[x] = carry | (carry >> 4) << 8;
    }
    kept_lo = k < kept_lo ? k : kept_lo;
    kept_hi = k + 1 > kept_hi ? k + 1 : kept_hi;
    HOST_WRITTEN(HOST_W_ATLAS, (n + 2) * (nb / 4 + 1) * 4);
    psp_rstats.atlas_made++;
    return kept_rows + k * KEPT_BYTES;
}

/* tile t's cell, its copy there sent to the GE if need be; -1 when it can't have one */
static __attribute__((noinline)) int atlas_cell_new(const RD_TEXTURE* t) {
    int c, n = tile_side(t);
    const uint8_t* rows;
    if (up_cur + UP_COPY_WORDS > up_list + UP_WORDS || (rows = kept_tile(t)) == NULL ||
        (c = take_entry(cells, CELLS, &cell_hand)) < 0) {
        psp_rstats.atlas_full++;
        return -1;
    }
    cells[c].serial = t->serial;
    cells[c].used = frame_no;
    cell_of[t->serial & (CELL_MAP - 1)] = (int16_t)c;
    /* as 16-bit pixels, a row of the atlas 128 of them */
    up_cur = ge_copy(up_cur, GU_PSM_4444, n / 4 + 1, n + 2, KEPT_ROW / 2, rows, c % CELLS_X * (CELL_W / 4),
                     c / CELLS_X * CELL_H, ATLAS_SIDE / 4, atlas);
    psp_rstats.atlas_copies++;
    return c;
}

static int atlas_cell(const RD_TEXTURE* t) {
    int c = cell_of[t->serial & (CELL_MAP - 1)];
    if (c >= 0 && cells[c].serial == t->serial) {
        cells[c].used = frame_no;
        return c;
    }
    return atlas_cell_new(t);
}

static void atlas_init(void) {
    const char* e = getenv("MVC2_GE_ATLAS");
    if ((e != NULL && atoi(e) == 0) || (kept_rows = memalign(64, KEPT * KEPT_BYTES)) == NULL ||
        (atlas = pool_take(&pools[0], ATLAS_SIDE * ATLAS_SIDE / 2)) == NULL) {
        free(kept_rows);
        kept_rows = NULL;
        atlas = NULL;
        return;
    }
    memset(cell_of, 0xFF, sizeof cell_of);
    memset(kept_of, 0xFF, sizeof kept_of);
    memset(atlas, 0, ATLAS_SIDE * ATLAS_SIDE / 2);
    sceKernelDcacheWritebackInvalidateRange(atlas, ATLAS_SIDE * ATLAS_SIDE / 2);
    atlas_tex.psm = GU_PSM_T4;
    atlas_tex.w = atlas_tex.h = ATLAS_SIDE;
    atlas_tex.tbw[0] = ATLAS_SIDE;
    atlas_tex.lv[0] = atlas;
    host_log("GE: sprite tiles in %d cells of a %dx%d texture in VRAM, %d kept converted in %lu KB", CELLS, ATLAS_SIDE,
             ATLAS_SIDE, KEPT, (unsigned long)(KEPT * KEPT_BYTES) >> 10);
}

/* VRAM for n bytes of levels made here: the least recently drawn copies holding VRAM let go for it, a few at most;
   main RAM when that doesn't make room */
static uint8_t* ext_take(uint32_t n) {
    uint8_t* mem;
    int tries, k;
    for (tries = 0; (mem = pool_take(&pools[0], n)) == NULL && tries < 8; tries++) {
        for (k = lru_first[BIG]; k >= 0; k = gtex[k].next) {
            const GTEX* e = &gtex[k];
            if (!in_flight(e->used) && (pool_of(e->mem) == &pools[0] || (e->ext != NULL && pool_of(e->ext) == &pools[0]))) {
                break;
            }
        }
        if (k < 0) {
            break;
        }
        let_go(&gtex[k]);
        psp_rstats.tex_evicted++;
    }
    return mem != NULL ? mem : pool_take(&pools[1], n);
}

/* the mean of four 16-bit texels, rounded half up: two fields a sum, each with room for its carries above it, and the
   sum's bits that are not the field's after the shift masked off */
static uint32_t mean_565(uint32_t p0, uint32_t p1, uint32_t p2, uint32_t p3) {
    uint32_t rb = (p0 & 0xF81Fu) + (p1 & 0xF81Fu) + (p2 & 0xF81Fu) + (p3 & 0xF81Fu) + 0x1002u;
    uint32_t g = (p0 & 0x07E0u) + (p1 & 0x07E0u) + (p2 & 0x07E0u) + (p3 & 0x07E0u) + 0x0040u;
    return (rb >> 2 & 0xF81Fu) | (g >> 2 & 0x07E0u);
}

static uint32_t mean_4444(uint32_t p0, uint32_t p1, uint32_t p2, uint32_t p3) {
    uint32_t rb = (p0 & 0x0F0Fu) + (p1 & 0x0F0Fu) + (p2 & 0x0F0Fu) + (p3 & 0x0F0Fu) + 0x0202u;
    uint32_t ag = (p0 & 0xF0F0u) + (p1 & 0xF0F0u) + (p2 & 0xF0F0u) + (p3 & 0xF0F0u) + 0x2020u;
    return (rb >> 2 & 0x0F0Fu) | (ag >> 2 & 0xF0F0u);
}

/* 1555: the colour the mean of those with alpha (all four where none has), the alpha set where two or more have */
static uint32_t mean_1555(const uint32_t* p) {
    static const uint32_t inv[5] = { 0, 65536, 32768, 21846, 16384 };    /* 65536 / n, rounded up: exact here */
    uint32_t r = 0, g = 0, b = 0, n = 0;
    int k;
    for (k = 0; k < 4; k++) {
        n += p[k] >> 15;
    }
    for (k = 0; k < 4; k++) {
        if (n == 0 || p[k] >> 15) {
            r += p[k] >> 10 & 31;
            g += p[k] >> 5 & 31;
            b += p[k] & 31;
        }
    }
    {
        uint32_t m = n > 0 ? n : 4, half = m / 2;
        return (n >= 2 ? 0x8000u : 0) | ((r + half) * inv[m] >> 16) << 10 | ((g + half) * inv[m] >> 16) << 5 |
               ((b + half) * inv[m] >> 16);
    }
}

/* the plain mean where the four have the same alpha */
static uint32_t mean_1555_4(uint32_t p0, uint32_t p1, uint32_t p2, uint32_t p3) {
    uint32_t all = p0 & p1 & p2 & p3, any = p0 | p1 | p2 | p3;
    if (((all ^ any) & 0x8000u) == 0) {
        uint32_t rb = (p0 & 0x7C1Fu) + (p1 & 0x7C1Fu) + (p2 & 0x7C1Fu) + (p3 & 0x7C1Fu) + 0x0802u;
        uint32_t g = (p0 & 0x03E0u) + (p1 & 0x03E0u) + (p2 & 0x03E0u) + (p3 & 0x03E0u) + 0x0040u;
        return (all & 0x8000u) | (rb >> 2 & 0x7C1Fu) | (g >> 2 & 0x03E0u);
    }
    {
        uint32_t p[4] = { p0 & 0xFFFFu, p1 & 0xFFFFu, p2 & 0xFFFFu, p3 & 0xFFFFu };
        return mean_1555(p);
    }
}

static inline __attribute__((always_inline)) uint32_t mean16(int psm, uint32_t p0, uint32_t p1, uint32_t p2,
                                                             uint32_t p3) {
    return psm == GU_PSM_5650 ? mean_565(p0, p1, p2, p3)
           : psm == GU_PSM_4444 ? mean_4444(p0, p1, p2, p3)
                                : mean_1555_4(p0, p1, p2, p3);
}

/* A GE level w by h (each a multiple of 8) at half the size from the one before, both in its 16-byte by 8-row
   blocks: each texel the mean of four. The means take the fields alike, so the swapped red and blue don't matter. A
   word of a block's row is two texels side by side; rows 2y and 2y + 1 are in the same block, 16 bytes apart. A word
   of the output, two texels, from the input's words at q and q + 1 in the two rows */
#define HALF_WORD(q) (mean16(psm, (q)[0], (q)[0] >> 16, (q)[4], (q)[4] >> 16) | \
                      mean16(psm, (q)[1], (q)[1] >> 16, (q)[5], (q)[5] >> 16) << 16)

/* its output rows from y0 on, 8 of them: a row of blocks */
static inline __attribute__((always_inline)) void half_body(int psm, const uint8_t* in, int w, int y0, uint8_t* out) {
    uint32_t rbi = 4 * (uint32_t)w, rbo = 2 * (uint32_t)w;
    uint8_t* band = out + (uint32_t)(y0 >> 3) * rbo * 8;
    int x, y;
    host_lines_new(band, rbo * 8, band + rbo * 8);
    for (y = y0; y < y0 + 8; y++) {
        const uint32_t* a = (const uint32_t*)(in + (uint32_t)(y >> 2) * rbi * 8 + (uint32_t)(2 * y & 7) * 16);
        uint32_t* o = (uint32_t*)(band + (uint32_t)(y & 7) * 16);
        for (x = 0; x < w; x += 8, a += 64, o += 32) {
            o[0] = HALF_WORD(a);
            o[1] = HALF_WORD(a + 2);
            o[2] = HALF_WORD(a + 32);
            o[3] = HALF_WORD(a + 34);
        }
    }
}

static void half_swizzled(int psm, const uint8_t* in, int w, int y0, uint8_t* out) {
    if (psm == GU_PSM_5650) {
        half_body(GU_PSM_5650, in, w, y0, out);
    } else if (psm == GU_PSM_4444) {
        half_body(GU_PSM_4444, in, w, y0, out);
    } else {
        half_body(GU_PSM_5551, in, w, y0, out);
    }
}

/* The levels made here, made from the texture's first after it is first drawn, a level at a time in the frames'
   time for them (MVC2_GE_MIP_US), so that a screen's new textures don't take one frame long. Until they are made it
   is drawn from its first alone */
#define MIP_QUEUE 128
static struct {
    int16_t i;
    uint8_t gen, done;              /* levels to make, made */
    uint16_t row;                   /* the next level's next row */
    uint32_t serial;
} mipq[MIP_QUEUE];
static int nmipq;
static uint32_t mip_us = 1500;

static uint32_t level_bytes(const GTEX* e, int j) {
    return (row_bytes(e->psm, e->tbw[j]) * (uint32_t)level_rows(e, e->h >> j) + 15) & ~15u;
}

static void mipq_pop(void) {
    memmove(&mipq[0], &mipq[1], (size_t)(nmipq - 1) * sizeof mipq[0]);
    nmipq--;
}

/* the first queued texture's next row of blocks made; 0 when none is queued */
static int mip_step(void) {
    while (nmipq > 0) {
        GTEX* e = &gtex[mipq[0].i];
        int j;
        if (e->serial != mipq[0].serial || (mipq[0].done > 0 && e->ext == NULL)) {
            mipq_pop();     /* let go, or its levels were */
            continue;
        }
        if (e->ext == NULL) {
            uint32_t n = 0, at = 0;
            uint8_t* ext;
            for (j = 1; j <= mipq[0].gen; j++) {
                e->tbw[j] = (uint16_t)(e->w >> j);
                n += level_bytes(e, j);
            }
            ext = ext_take(n);
            if (ext != NULL && e->serial != mipq[0].serial) {
                pool_give(pool_of(ext), ext, n);    /* let go for the room */
                ext = NULL;
            }
            if (ext == NULL) {
                mipq_pop();
                continue;
            }
            e->ext = ext;
            e->ext_bytes = n;
            for (j = 1; j <= mipq[0].gen; j++) {
                e->lv[j] = ext + at;
                at += level_bytes(e, j);
            }
        }
        j = mipq[0].done + 1;
        half_swizzled(e->psm, e->lv[j - 1], e->w >> j, mipq[0].row, e->lv[j]);
        mipq[0].row += 8;
        if (mipq[0].row < e->h >> j) {
            return 1;
        }
        sceKernelDcacheWritebackRange(e->lv[j], level_bytes(e, j));
        HOST_WRITTEN(HOST_W_ATLAS, level_bytes(e, j));
        e->levels = (uint8_t)(e->own + j);
        mipq[0].done = (uint8_t)j;
        mipq[0].row = 0;
        if (j == mipq[0].gen) {
            mipq_pop();
        }
        return 1;
    }
    return 0;
}

/* gen levels to make for e, a 16-bit texture just made; none when the queue is full */
static void mips_later(GTEX* e, int gen) {
    if (nmipq == MIP_QUEUE) {
        return;
    }
    mipq[nmipq].i = (int16_t)(e - gtex);
    mipq[nmipq].serial = e->serial;
    mipq[nmipq].gen = (uint8_t)gen;
    mipq[nmipq].done = 0;
    mipq[nmipq].row = 0;
    nmipq++;
}

/* levels made for a 16-bit texture without its own, 128 or more each way: down to 16, three at most */
static int levels_to_make(const GTEX* e, const RD_TEXTURE* t, int step) {
    int n = 0;
    if (e->levels != 0 || step != 1 || t->format > RD_ARGB4444 || e->w < 128 || e->h < 128 || no_mips) {
        return 0;
    }
    while (n < 3 && (e->w >> (n + 1)) >= 16 && (e->h >> (n + 1)) >= 16) {
        n++;
    }
    return n;
}

/* t's levels as the GE will have them, in e, shrink halvings below what it can have; their bytes. k0: t's level drawn
   as the first, step: of its texels, every step'th */
static uint32_t plan(GTEX* e, const RD_TEXTURE* t, int shrink, int* k0, int* step) {
    uint32_t total = 0;
    int j;
    e->psm = psm_of(t->format);
    for (*k0 = 0; (t->width >> *k0) > 512 || (t->height >> *k0) > 512; ++*k0) {
    }
    *k0 += shrink;
    *step = *k0 > t->levels ? 1 << *k0 : 1;     /* no level small enough: every step'th texel of the first */
    e->levels = *k0 > t->levels ? 0 : t->levels - *k0;
    e->levels = e->levels > 7 ? 7 : e->levels;
    e->w = t->width >> *k0;
    e->h = t->height >> *k0;
    e->swizzled = e->h >= 8;        /* the GE's texture cache reads a block at a time */
    for (j = 0; j <= e->levels; j++) {
        int w = e->w >> j, h = e->h >> j;
        w = w < 1 ? 1 : w;
        h = h < 1 ? 1 : h;
        e->tbw[j] = w < min_tbw(e->psm) ? min_tbw(e->psm) : w;
        total += (row_bytes(e->psm, e->tbw[j]) * (uint32_t)level_rows(e, h) + 15) & ~15u;
    }
    return total;
}

/* the texels of a texture kept as its source, unpacked into a buffer kept while it is small */
#define UNPACK_KEEP (256u << 10)
static uint8_t* unpack_buf;
static uint32_t unpack_room;

static const RD_TEXTURE* unpacked(const RD_TEXTURE* t, RD_TEXTURE* out) {
    if (t->unpack == NULL) {
        return t;
    }
    if (t->unpacked > unpack_room) {
        free(unpack_buf);
        unpack_buf = memalign(16, t->unpacked);
        unpack_room = unpack_buf != NULL ? t->unpacked : 0;
        if (unpack_buf == NULL) {
            return NULL;
        }
    }
    *out = *t;
    out->texels = unpack_buf;
    t->unpack(t, out);
    return out;
}

static void unpacked_done(void) {
    if (unpack_room > UNPACK_KEEP) {
        free(unpack_buf);
        unpack_buf = NULL;
        unpack_room = 0;
    }
}

/* a corner tile's texels (twiddled: its PAL4 source) with those past its corner as the corner's edge, the texels it
   draws and nothing else */
static const void* corner_texels(const RD_TEXTURE* t, const uint8_t* from, int twiddled) {
    static uint8_t buf[32 * 32] __attribute__((aligned(4)));
    uint32_t sp[32];
    int n = t->width, c = t->corner, x, y;
    if (c == 0 || n > 32) {
        return from;
    }
    for (x = 0; x < n; x++) {
        sp[x] = spread_bits((uint32_t)x);
    }
    if (twiddled) {
        memset(buf, 0, (size_t)n * n / 2);
    }
    for (y = 0; y < n; y++) {
        int cy = y < c ? y : c - 1;
        for (x = 0; x < n; x++) {
            int cx = x < c ? x : c - 1;
            if (twiddled) {
                uint32_t i = sp[cx] << 1 | sp[cy], o = sp[x] << 1 | sp[y];
                buf[o >> 1] |= (uint8_t)((from[i >> 1] >> (i & 1) * 4 & 15) << (o & 1) * 4);
            } else {
                buf[y * n + x] = from[cy * n + cx];
            }
        }
    }
    return buf;
}

static void all_drawn(void);

/* why a texture the frame draws had no room at first, the first few times */
static void room_log(const RD_TEXTURE* t, uint32_t n, const char* then) {
    static int said;
    uint32_t big = 0, flying = 0, longest[2] = { 0, 0 };
    int i, p;
    if (said >= 12) {
        return;
    }
    said++;
    for (p = 0; p < 2; p++) {
        for (i = 0; i < pools[p].n; i++) {
            longest[p] = pools[p].len[i] > longest[p] ? pools[p].len[i] : longest[p];
        }
    }
    for (i = 0; i < MAX_TEX; i++) {
        if (gtex[i].serial != 0 && gtex[i].kind == BIG) {
            big++;
            flying += in_flight(gtex[i].used) ? gtex[i].bytes + gtex[i].ext_bytes : 0;
        }
    }
    host_log("GE: frame %u: no room for a %dx%d texture (format %d, %u bytes): VRAM %u of %u KB used, longest free %u; "
             "RAM %u of %u KB, longest free %u; small %u KB; %d held, %u big, %u KB of them drawn by frames in flight; "
             "%s", (unsigned)frame_no, t->width, t->height, t->format, (unsigned)n, (unsigned)pools[0].used >> 10,
             (unsigned)pools[0].size >> 10, (unsigned)longest[0], (unsigned)pools[1].used >> 10,
             (unsigned)pools[1].size >> 10, (unsigned)longest[1], (unsigned)slot_bytes >> 10, ngtex, (unsigned)big,
             (unsigned)flying >> 10, then);
}

/* the levels made here let go, every texture's: the GE draws them no longer */
static void drop_made_levels(void) {
    int i;
    for (i = 0; i < MAX_TEX; i++) {
        GTEX* g = &gtex[i];
        if (g->serial != 0 && g->ext != NULL) {
            pool_give(pool_of(g->ext), g->ext, g->ext_bytes);
            g->ext = NULL;
            g->ext_bytes = 0;
            g->levels = g->own;
        }
    }
    nmipq = 0;
}

/* t's texels as the GE reads them; NULL when they can't be had */
static GTEX* texture(const RD_TEXTURE* t) {
    static int big_said, unpack_said, room_said;
    GTEX* e = find(t->serial);
    GTEX made;
    RD_TEXTURE ut;
    const RD_TEXTURE* src;
    const void* texels;
    uint32_t total, at = 0, t0;
    uint8_t* mem;
    int k0, step, j, gen, shrink;
    if (e != NULL) {
        if (e->used != frame_no) {
            e->used = frame_no;
            unlink_lru(e);
            append_lru(e);
        }
        psp_rstats.tex_hits++;
        return e;
    }
    if (t->serial == 0 || psm_of(t->format) < 0 || t->texels == NULL || (t->width & (t->width - 1)) != 0 ||
        (t->height & (t->height - 1)) != 0) {
        return NULL;
    }
    total = plan(&made, t, 0, &k0, &step);
    made.own = made.levels;
    gen = levels_to_make(&made, t, step);
    if (step > 1) {
        log_once(&big_said, "a texture over 512 texels drawn from every other texel");
    }
    t0 = sceKernelGetSystemTimeLow();
    src = pal4_square(t) ? t : unpacked(t, &ut);
    if (src == NULL) {
        log_once(&unpack_said, "no memory to unpack a texture in");
        return NULL;
    }
    e = room_for(total, &mem, gen > 0);
    /* no room among those not in flight: the GE's frame waited for, then the levels made here let go, then the
       texture drawn from fewer texels */
    for (shrink = 0; e == NULL && shrink <= 3; shrink++) {
        room_log(t, total, shrink == 0 ? "the GE's frame waited for" : shrink == 1 ? "the made levels let go"
                                                                                    : "drawn at half the size");
        if (shrink == 0) {
            all_drawn();
        } else if (shrink == 1) {
            drop_made_levels();
        } else {
            if (src == t && t->unpack != NULL && (src = unpacked(t, &ut)) == NULL) {
                break;
            }
            total = plan(&made, t, shrink - 1, &k0, &step);
            made.own = made.levels;
            if (made.w < 8 || made.h < 8) {
                break;
            }
        }
        gen = 0;
        e = room_for(total, &mem, 0);
    }
    if (e == NULL) {
        log_once(&room_said, "a texture not drawn: those the frame draws fill the budget");
        unpacked_done();
        return NULL;
    }
    texels = corner_texels(t, src->texels, src == t && t->unpack != NULL);
    for (j = 0; j <= made.levels; j++) {
        int k = step > 1 ? 0 : k0 + j, w = made.w >> j, h = made.h >> j, sw = t->width >> k;
        w = w < 1 ? 1 : w;
        h = h < 1 ? 1 : h;
        made.lv[j] = mem + at;
        if (src == t && t->unpack != NULL) {
            pal4_from_twiddled(texels, t->width, made.lv[0], row_bytes(made.psm, made.tbw[0]));
        } else {
            convert(t, k == 0 ? texels : src->mip[k], sw < 1 ? 1 : sw, step, made.lv[j], w, h, made.tbw[j],
                    made.swizzled);
        }
        at += (row_bytes(made.psm, made.tbw[j]) * (uint32_t)level_rows(&made, h) + 15) & ~15u;
    }
    unpacked_done();
    psp_rstats.tex_us += sceKernelGetSystemTimeLow() - t0;
    HOST_WRITTEN(HOST_W_ATLAS, total);
    sceKernelDcacheWritebackRange(mem, total);
    made.serial = t->serial;
    made.used = frame_no;
    made.mem = mem;
    made.bytes = total;
    made.ext = NULL;
    made.ext_bytes = 0;
    made.kind = e->kind;
    *e = made;
    append_lru(e);
    put_hash((int)(e - gtex));
    if (gen > 0) {
        mips_later(e, gen);
    }
    psp_rstats.tex_made++;
    psp_rstats.tex_made_bytes += total + made.ext_bytes;
    return e;
}

/* ---- the depth layout ---- */

static float* flat_z;               /* the flat triangles' 1/w seen in the frame being made */
static int nflat, maxflat;
/* the layout the list takes: the flat 1/w outside the 3D range, sorted, each once; nbelow below it. The game's next
   frame gathers into the other */
static float* lay_z;
static int nlay, maxlay, nbelow;
float lo3, hi3;              /* the 3D triangles' 1/w */
int has3d;
float zk;                    /* the GE's depth z is zk * (1 / w - aff_lo) */
float aff_scale, aff_centre; /* the 3D range's viewport z */
float aff_slope, aff_lo;     /* its depth is aff_centre + aff_slope * (1 / w - aff_lo): the GE's centre stays small */
static int spread;                  /* steps a flat 1/w */

static float slot_depth(int r) {
    return r < 32768 ? (float)r : 32768.0f + 2.0f * (float)(r - 32768);
}

static void add_flat(float z) {
    if (nflat == maxflat) {
        int n = maxflat ? maxflat * 2 : 4096;
        float* p = realloc(flat_z, (size_t)n * sizeof *p);
        if (p == NULL) {
            return;
        }
        flat_z = p;
        maxflat = n;
    }
    flat_z[nflat++] = z;
}

/* the flat 1/w values seen this frame, each kept once: a hash of their bits, an entry free when its stamp is old */
#define SEEN_SIZE 4096
static uint32_t seen_bits[SEEN_SIZE], seen_stamp[SEEN_SIZE], stamp;
static int nseen;

static void add_flat_once(float z) {
    uint32_t bits, h;
    if (nseen >= SEEN_SIZE * 3 / 4) {
        add_flat(z);        /* the sort after takes out those seen twice */
        return;
    }
    memcpy(&bits, &z, 4);
    for (h = bits * 2654435761u >> 20; seen_stamp[h] == stamp; h = (h + 1) & (SEEN_SIZE - 1)) {
        if (seen_bits[h] == bits) {
            return;
        }
    }
    seen_stamp[h] = stamp;
    seen_bits[h] = bits;
    nseen++;
    add_flat(z);
}

static int float_cmp(const void* a, const void* b) {
    float x = *(const float*)a, y = *(const float*)b;
    return x < y ? -1 : x > y;
}

/* each list's groups in order, found once a frame (layout_depth) */
uint16_t list_groups[RD_LISTS][RD_GPU_GROUPS];
int nlist_groups[RD_LISTS];

RD_ZRANGE rd_zrange;
static float flat_max;

/* the 1/w range of a triangle's part on the screen (and RD_ZCUT about it): 1/w is a plane over the screen, so
   it is within both the corners' range and the plane's at the screen's corners */
void rd_tri_z_cut(const RD_VTX* a, const RD_VTX* b, const RD_VTX* c) {
    float bx = b->x - a->x, by = b->y - a->y, cx = c->x - a->x, cy = c->y - a->y, bz = b->z - a->z, cz = c->z - a->z;
    float d = bx * cy - cx * by, lo, hi, kx, ky, x0, x1, y0, y1, z0, z;
    lo = a->z < b->z ? a->z : b->z;
    lo = c->z < lo ? c->z : lo;
    hi = a->z > b->z ? a->z : b->z;
    hi = c->z > hi ? c->z : hi;
    if (fabsf(d) > 1e-3f) {
        kx = (bz * cy - cz * by) / d;
        ky = (cz * bx - bz * cx) / d;
        z0 = a->z - kx * a->x - ky * a->y;
        x0 = kx * -RD_ZCUT;         /* the plane's least and most over the screen */
        x1 = kx * (RD_WIDTH + RD_ZCUT);
        y0 = ky * -RD_ZCUT;
        y1 = ky * (RD_HEIGHT + RD_ZCUT);
        z = z0 + (kx < 0.0f ? x1 : x0) + (ky < 0.0f ? y1 : y0);
        lo = z > lo ? z : lo;
        z = z0 + (kx < 0.0f ? x0 : x1) + (ky < 0.0f ? y0 : y1);
        hi = z < hi ? z : hi;
        if (!(lo <= hi)) {
            return;     /* none of it on the screen */
        }
    }
    rd_z_take(lo);
    rd_z_take(hi);
}

void rd_zrange_begin(void) {
    int i;
    rd_zrange.lo = 1e30f;
    rd_zrange.hi = 0.0f;
    rd_zrange.has3d = 0;
    for (i = 0; i < RD_LISTS; i++) {
        rd_zrange.last[i] = -1.0f;
    }
    flat_max = 1e-30f;
    nflat = nseen = 0;
    stamp++;
}

void rd_flat_z(float z) {
    if (usable_z(z)) {
        flat_max = z > flat_max ? z : flat_max;
        add_flat_once(z);
    }
}

static void layout_depth(void) {
    float zmax = flat_max, d_lo, d_hi, whi = 0.0f, wlo = 1e30f;
    int i, n, top;
    const RD_VTX* bg = rd_frame->background;
    has3d = rd_zrange.has3d;
    lo3 = rd_zrange.lo;
    hi3 = rd_zrange.hi;
    memset(nlist_groups, 0, sizeof nlist_groups);
    for (i = 0; i < rd_frame->ngroups; i++) {
        const RD_GPU_DRAW* d = &rd_frame->groups[i];
        int list = rd_frame->states[d->state].list;
        list_groups[list][nlist_groups[list]++] = (uint16_t)i;
        whi = d->g.whi > whi ? d->g.whi : whi;
        wlo = d->g.wlo < wlo ? d->g.wlo : wlo;
    }
    if (rd_frame->ngroups > 0) {
        float glo = 1.0f / whi, ghi = 1.0f / wlo;
        lo3 = glo < lo3 ? glo : lo3;
        hi3 = ghi > hi3 ? ghi : hi3;
        has3d = 1;
    }
    if (!(bg[0].z == bg[1].z && bg[1].z == bg[2].z)) {
        for (i = 0; i < 3; i++) {
            if (usable_z(bg[i].z)) {
                lo3 = bg[i].z < lo3 ? bg[i].z : lo3;
                hi3 = bg[i].z > hi3 ? bg[i].z : hi3;
                has3d = 1;
            }
        }
    }
    if (rd_frame->ngroups > 0 && !(hi3 > lo3)) {
        /* models all at one 1/w, as on a page of them: a range about it, for their depth */
        lo3 *= 0.5f;
        hi3 *= 2.0f;
    }
    has3d = has3d && hi3 > lo3;
    zmax = has3d && hi3 > zmax ? hi3 : zmax;
    add_flat(usable_z(bg[0].z) ? bg[0].z : 1e-30f);
    for (i = n = 0; i < nflat; i++) {
        float z = flat_z[i];
        if (i == nflat - 1 || !has3d || z < lo3 || z > hi3) {
            flat_z[n++] = z;
        }
    }
    qsort(flat_z, (size_t)n, sizeof *flat_z, float_cmp);
    for (i = nflat = 0; i < n; i++) {
        if (nflat == 0 || flat_z[i] != flat_z[nflat - 1]) {
            flat_z[nflat++] = flat_z[i];
        }
    }
    for (nbelow = 0; nbelow < nflat && has3d && flat_z[nbelow] < lo3; nbelow++) {
    }
    if (!has3d) {
        nbelow = nflat;
    }
    {
        float* z = lay_z;
        int m = maxlay;
        lay_z = flat_z;
        maxlay = maxflat;
        nlay = nflat;
        flat_z = z;
        maxflat = m;
        nflat = 0;
    }
    /* each flat 1/w a step, or more where there is room; the rest the 3D range's */
    top = SLOTS - 1;
    spread = 1;
    if (!has3d && nlay > 0) {
        spread = (top - 1) / nlay;
        spread = spread > 64 ? 64 : spread < 1 ? 1 : spread;
    }
    zk = 1.0f / zmax;
    d_lo = slot_depth(nbelow + SLOT_MARGIN);
    d_hi = slot_depth(top - (nlay - nbelow) - SLOT_MARGIN);
    if (has3d) {
        float slope = (d_hi - d_lo) / (hi3 - lo3);
        aff_scale = slope / zk;
        aff_slope = slope;
        aff_centre = d_lo;
        aff_lo = lo3;
    }
    psp_rstats.depth_values += (uint32_t)nlay;
}

/* a flat 1/w's depth: its step */
float flat_depth(float z) {
    int lo = 0, hi = nlay - 1, top = SLOTS - 1;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (lay_z[mid] < z) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (nlay + 2 * SLOT_MARGIN + 2 > top) {
        return slot_depth(1 + (int)((int64_t)lo * (top - 2) / nlay));     /* more than the steps: shared */
    }
    return slot_depth(lo < nbelow ? 1 + lo * spread : top - (nlay - 1 - lo));
}

/* ---- probes ---- */

static const char* const probe_name[PROBES] = { "as it is", "no textures", "nearest texels", "no blending",
                                                "no models", "models only", "no made levels", "no fog pass",
                                                "no depth test", "level bias +1", "list uncached",
                                                "no atlas" };
int probe;
static int probe_start, probe_frames, probe_cycles;

STATE_TEX state_tex[RD_MAX_STATES];

static int tile_state(const RD_STATE* s) {
    return s->tex != NULL && atlas != NULL && s->fog != RD_FOG_TABLE2 && probe != P_NO_TEX && probe != P_NO_ATLAS &&
           atlas_tile(s->tex);
}

static void make_textures(void) {
    int i;
    for (i = 0; i < rd_frame->nstates; i++) {
        const RD_STATE* s = &rd_frame->states[i];
        const RD_TEXTURE* t = s->tex;
        STATE_TEX* e = &state_tex[i];
        int c = tile_state(s) ? atlas_cell(t) : -1;
        e->cell = (int16_t)c;
        e->rows = c >= 0 ? kept_tile(t) : NULL;
        e->tex = t != NULL && probe != P_NO_TEX && e->rows == NULL ? texture(t) : NULL;
        e->side = c >= 0 ? (uint16_t)tile_side(t) : 0;
        e->width = c >= 0 ? (uint16_t)t->width : 0;
        e->no_texels = t != NULL && t->texels == NULL;
        e->plain = t == NULL || t->levels == 0;
    }
}

/* MVC2_ME: where the list is made. 0 here, before its frame is given to the GE; 1 on the Media Engine, beside the
   game's next frame, and given to the GE a frame behind; late here, at the game's next frame's end, as the Media
   Engine's would be at its latest: a check that the game's next frame leaves alone what the list is made from */
enum { LIST_HERE, LIST_LATE, LIST_ME };
static int list_where;

/* ---- models the GE turns and lights ---- */

/* A mesh is the model's vertices as the GE reads them, its triangles' indices and its runs of them by how they are
   culled and shaded, in a pool of main RAM. One let go is kept until the frame that may draw it is drawn. */
#define MESH_POOL (2u << 20)
#define MAX_MESHES 1024

GMESH gmesh[MAX_MESHES];
static POOL mesh_pool;
static int mesh_next;
static uint32_t mesh_freed;         /* those let go and not yet back in the pool */

static void mesh_release(void) {
    int i;
    for (i = 0; i < MAX_MESHES && mesh_freed != 0; i++) {
        GMESH* e = &gmesh[i];
        if (e->mem != NULL && e->freed != 0 && e->freed <= frame_no) {
            pool_give(&mesh_pool, e->mem, e->bytes);
            e->mem = NULL;
            mesh_freed--;
        }
    }
}

static uint32_t align16(uint32_t n) {
    return (n + 15) & ~15u;
}

int rd_gpu_mesh_make(const NL_MESH* m) {
    static const int vtype[3] = { GU_TEXTURE_32BITF | GU_VERTEX_32BITF,
                                  GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF,
                                  GU_TEXTURE_32BITF | GU_NORMAL_32BITF | GU_VERTEX_32BITF };
    static const int stride[3] = { 5, 6, 8 };   /* words */
    uint32_t vbytes, ibytes, rbytes;
    int i, nruns = 0;
    GMESH* e;
    float* d;
    RUN* run;
    if (mesh_pool.base == NULL || m->kind < RD_GPU_UNLIT || m->kind > RD_GPU_LIT || m->nv <= 0 || m->nv > 65535 ||
        m->ntri <= 0 || m->ntri > 65535 || m->nspan <= 0) {
        return -1;
    }
    for (i = 0; i < m->ntri * 3; i++) {
        if (m->idx[i] >= m->nv) {
            return -1;
        }
    }
    for (i = 0; i < m->nspan; i++) {
        if (m->span[i].first + m->span[i].count > m->ntri) {
            return -1;
        }
    }
    mesh_release();
    for (i = 0; i < MAX_MESHES && gmesh[mesh_next].mem != NULL; i++) {
        mesh_next = (mesh_next + 1) % MAX_MESHES;
    }
    vbytes = align16((uint32_t)m->nv * (uint32_t)stride[m->kind] * 4);
    ibytes = align16((uint32_t)m->ntri * 6);
    rbytes = align16((uint32_t)m->nspan * sizeof(RUN));
    if (i == MAX_MESHES) {
        return -1;
    }
    e = &gmesh[mesh_next];
    e->bytes = vbytes + ibytes + rbytes;
    if ((e->mem = pool_take(&mesh_pool, e->bytes)) == NULL) {
        return -1;
    }
    d = (float*)e->mem;
    e->opaque = 1;
    for (i = 0; i < m->nv; i++) {
        const NL_MVTX* v = &m->v[i];
        *d++ = v->u;
        *d++ = v->v;
        if (m->kind == RD_GPU_VTXCOL) {
            uint32_t c = abgr(v->argb);
            memcpy(d++, &c, 4);
            e->opaque &= c >> 24 == 255;
        } else if (m->kind == RD_GPU_LIT) {
            *d++ = v->nx;
            *d++ = v->ny;
            *d++ = v->nz;
        }
        *d++ = v->x;
        *d++ = v->y;
        *d++ = v->z;
    }
    memcpy(e->mem + vbytes, m->idx, (size_t)m->ntri * 6);
    run = (RUN*)(e->mem + vbytes + ibytes);
    for (i = 0; i < m->nspan; i++) {
        const NL_MSPAN* s = &m->span[i];
        if (s->count == 0) {
            continue;
        }
        if (nruns > 0 && run[nruns - 1].cull == s->cull && run[nruns - 1].flat == s->flat &&
            run[nruns - 1].first + run[nruns - 1].count == s->first) {
            run[nruns - 1].count = (uint16_t)(run[nruns - 1].count + s->count);
        } else {
            run[nruns].first = s->first;
            run[nruns].count = s->count;
            run[nruns].cull = s->cull;
            run[nruns].flat = s->flat;
            nruns++;
        }
    }
    sceKernelDcacheWritebackRange(e->mem, e->bytes);
    e->freed = 0;
    e->kind = m->kind;
    e->vtype = vtype[m->kind] | GU_INDEX_16BIT | GU_TRANSFORM_3D;
    e->nruns = nruns;
    e->verts = e->mem;
    e->idx = (const uint16_t*)(e->mem + vbytes);
    e->runs = run;
    for (i = 0; i < 3; i++) {
        e->lo[i] = m->lo[i];
        e->hi[i] = m->hi[i];
    }
    return mesh_next;
}

void rd_gpu_mesh_free(int id) {
    if (id >= 0 && id < MAX_MESHES && gmesh[id].mem != NULL && gmesh[id].freed == 0) {
        /* the frame being made may draw it, and the GE draws it after, a frame later where lists are made behind */
        gmesh[id].freed = frame_no + 2 + (list_where != LIST_HERE);
        mesh_freed++;
    }
}

uint32_t rd_gpu_mesh_room(void) {
    uint32_t n = 0;
    int i;
    mesh_release();
    for (i = 0; i < mesh_pool.n; i++) {
        n += mesh_pool.len[i];
    }
    return n;
}

/* a group the GE can draw as the CPU's path would: its box in front and inside the guard band, and outside the opaque
   list, its alpha as the CPU's would be (the vertices' colours times the face's, or 1 where the state takes none) */
static int take_group(const RD_STATE* s, const RD_GPU_GROUP* g, const float m[16]) {
    const GMESH* e = g->mesh >= 0 && g->mesh < MAX_MESHES ? &gmesh[g->mesh] : NULL;
    float f[5];
    int k, j;
    if (e == NULL || e->mem == NULL || s->fog == RD_FOG_TABLE2 || !(g->wlo > 0.0f)) {
        return 0;
    }
    if (s->list != RD_OPAQUE && g->kind == RD_GPU_VTXCOL && opaque_alpha_of(s) && !(e->opaque && g->face[0] == 1.0f)) {
        return 0;
    }
    /* w, and the corner's pixel's margins inside the guard band times w, are each linear over the box: their least
       over its corners, from each axis's low or high, all above 0 */
    f[0] = m[12];
    f[1] = m[13] + GUARD * m[12];
    f[2] = (RD_WIDTH + GUARD) * m[12] - m[13];
    f[3] = m[14] + GUARD * m[12];
    f[4] = (RD_HEIGHT + GUARD) * m[12] - m[14];
    for (k = 0; k < 3; k++) {
        const float* r = m + 4 * k;
        float c[5];
        c[0] = r[0];
        c[1] = r[1] + GUARD * r[0];
        c[2] = (RD_WIDTH + GUARD) * r[0] - r[1];
        c[3] = r[2] + GUARD * r[0];
        c[4] = (RD_HEIGHT + GUARD) * r[0] - r[2];
        for (j = 0; j < 5; j++) {
            float lo = e->lo[k] * c[j], hi = e->hi[k] * c[j];
            f[j] += lo < hi ? lo : hi;
        }
    }
    return f[0] > 0.0f && f[1] > 0.0f && f[2] > 0.0f && f[3] > 0.0f && f[4] > 0.0f;
}

static const uint32_t* pillar_tex;  /* psp_art.c's, or NULL for none */
static int pillars_left, clear_again;

/* the pillars beside a 4:3 picture on or off (an options menu's), the bars cleared and drawn again in both buffers */
void psp_render_pillars(int on) {
    static const uint32_t* made;
    if (on && made == NULL) {
        made = psp_art_pillars();
    }
    pillar_tex = on ? made : NULL;
    pillars_left = 2;
    clear_again = 2;
}

void psp_render_start(void) {
    const char* e = getenv("MVC2_GPU_TL");
    void* mem;
    psp_render_pillars(psp_art_wanted());
    if ((e != NULL && strcmp(e, "0") == 0) || (mem = memalign(64, MESH_POOL)) == NULL) {
        return;
    }
    pool_init(&mesh_pool, mem, MESH_POOL);
    rd_gpu_tl = 1;
    rd_gpu_tl_trans = 1;
    rd_gpu_tl_trans_deep = 1.25f;   /* a deeper translucent one is taken only as a shell, its back faces then its front */
    rd_gpu_tl_two_pass = 1;
    rd_gpu_tl_nearclip = 1;
    rd_gpu_take = take_group;
    host_log("GE: models turned and lit by the GE, their meshes in %lu KB", (unsigned long)MESH_POOL >> 10);
}

/* the GE's times: when each list finished and each section of one ended, by its interrupts */
#define SECTIONS 5                  /* the background, the three lists, and what follows */
static volatile uint32_t fin_at[8], sig_at[32];
static volatile unsigned nfin, nsig;
int sections;                /* the sections timed: MVC2_GE_SECTIONS, on unless 0 */
static int list_uncached;           /* MVC2_GE_LIST_UNCACHED=1: the list written past the cache */
static int list_crc;                /* MVC2_GE_CRC=1: each 300 frames' lists hashed, addresses left out */
static float fixed_bias;            /* MVC2_GE_MIP_BIAS, -0.5 (the first level up to two texels a pixel) */
static int auto_on = 1;             /* MVC2_GE_MIP_AUTO=HI,LO, 0 off: the scene's made levels by the frame's time */
static uint32_t auto_hi = 15500, auto_lo = 13000;
static uint32_t crc_at = 2166136261u;

/* the words the GE reads, from start to end, past the words a jump skips; an address's bits not hashed */
static void hash_list(const uint32_t* w, const uint32_t* end) {
    uint32_t h = crc_at;
    while (w < end) {
        uint32_t c = *w >> 24, x = *w++;
        if (c == 0x08) {
            w += (int32_t)((x & 0xFFFFFFu) - ((uint32_t)w & 0xFFFFFFu)) / 4;
        }
        if (c <= 0x02 || c == 0x08 || c == 0x0A || c == 0x10 || (c >= 0xA0 && c <= 0xB5)) {
            x = c << 24;
        }
        h = (h ^ x) * 16777619u;
    }
    crc_at = h;
}

/* MVC2_GE_HASHLOG=path: each frame's hashes to path, a line a frame: the uploads, the list's words by kind (matrices,
   textures, primitives, the rest) and its vertices. MVC2_GE_REF=path: such a log made elsewhere, each frame checked
   against it; the first that differs written whole to path.bad. The lines are kept in memory and added to the file
   each 60 frames by the game's thread, closed each time (the memory stick sets a file's length at its close) */
enum { FH_UP, FH_MATRIX, FH_TEX, FH_PRIM, FH_REST, FH_VERTS, FH_N };
#define HASH_TEXT (8000u * 80u)
static char* hash_text;
static volatile size_t hash_len;
static size_t hash_written;
static const char* hash_path;
static uint32_t (*hash_ref)[1 + FH_N];
static unsigned nhash_ref, hash_bad;
static const char* hash_ref_path;

static uint32_t fnv(uint32_t h, const uint32_t* w, uint32_t n) {
    while (n-- > 0) {
        h = (h ^ *w++) * 16777619u;
    }
    return h;
}

static void hash_kinds(const uint32_t* w, const uint32_t* end, uint32_t* h, int only) {
    while (w < end) {
        uint32_t c = *w >> 24, x = *w++;
        int k = only >= 0 ? only : c >= 0x3A && c <= 0x41 ? FH_MATRIX : c >= 0xA0 && c <= 0xCF ? FH_TEX
                : c <= 0x06 || c == 0x12 ? FH_PRIM : FH_REST;
        if (c == 0x08) {
            w += (int32_t)((x & 0xFFFFFFu) - ((uint32_t)w & 0xFFFFFFu)) / 4;
        }
        if (c <= 0x02 || c == 0x08 || c == 0x0A || c == 0x10 || (c >= 0xA0 && c <= 0xB5)) {
            x = c << 24;
        }
        h[k] = (h[k] ^ x) * 16777619u;
    }
}

static __attribute__((cold)) void hash_open(void) {
    const char* e = getenv("MVC2_GE_HASHLOG");
    FILE* f;
    char line[128];
    if (e != NULL && (hash_text = malloc(HASH_TEXT)) != NULL) {
        SceUID fd = sceIoOpen(e, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        hash_path = e;
        if (fd >= 0) {
            sceIoClose(fd);
        }
    }
    hash_ref_path = getenv("MVC2_GE_REF");
    if (hash_ref_path == NULL || (f = fopen(hash_ref_path, "r")) == NULL) {
        return;
    }
    hash_ref = malloc(6000 * sizeof *hash_ref);
    while (hash_ref != NULL && nhash_ref < 6000 && fgets(line, sizeof line, f) != NULL) {
        uint32_t* r = hash_ref[nhash_ref];
        if (sscanf(line, "%lu %lx %lx %lx %lx %lx %lx", (unsigned long*)&r[0], (unsigned long*)&r[1],
                   (unsigned long*)&r[2], (unsigned long*)&r[3], (unsigned long*)&r[4], (unsigned long*)&r[5],
                   (unsigned long*)&r[6]) == 7) {
            nhash_ref++;
        }
    }
    fclose(f);
    host_log("GE: %u frames' hashes to check against, from %s", nhash_ref, hash_ref_path);
}


static void on_finish(int id) {
    (void)id;
    fin_at[nfin & 7] = sceKernelGetSystemTimeLow();
    nfin++;
}

static void on_signal(int id) {
    (void)id;
    sig_at[nsig & 31] = sceKernelGetSystemTimeLow();
    nsig++;
}

static uint32_t me_ahead;           /* MVC2_ME_AHEAD: the ME's write-ahead, 1 KB unless set (0 none) */
static int me_check;                /* MVC2_ME_CHECK=1: each ME list made again here and compared */
static int me_time;                 /* MVC2_ME_TIME=1: the kicker's waits */
static uint32_t start_margin;       /* MVC2_FRAME_START=us kept in hand (2000 with MVC2_FEEL and the ME, 0 off) */
static int start_here;              /* with the lists made here too: only if asked for */
static uint32_t now_us(void);

static RD_FRAME* rd_frames[2];      /* made apart from the list: the one the game fills, the one its list is made from */
const RD_FRAME* list_frame;

static int init(void) {
    int b;
    for (b = 0; b < 2; b++) {
        clists[b] = memalign(64, LIST_BYTES);
        verts_buf[b] = memalign(64, MAX_VERTS * sizeof *verts);
        over_buf[b] = memalign(64, MAX_OVER * sizeof *over);
        if (clists[b] == NULL || verts_buf[b] == NULL || over_buf[b] == NULL) {
            return 0;
        }
    }
    screen16 = psp_video_bits16();
    no_mips = getenv("MVC2_GE_MIPS") != NULL && atoi(getenv("MVC2_GE_MIPS")) == 0;
    mip_us = getenv("MVC2_GE_MIP_US") != NULL ? (uint32_t)atoi(getenv("MVC2_GE_MIP_US")) : mip_us;
    if (getenv("MVC2_FRAME_START") != NULL) {
        start_margin = (uint32_t)atoi(getenv("MVC2_FRAME_START"));
        start_here = 1;
    } else if (getenv("MVC2_FEEL") != NULL && atoi(getenv("MVC2_FEEL")) != 0) {
        start_margin = 2000;
    }
    if (!psp_paced || psp_video_frame_us() == 0) {
        start_margin = 0;
    }
    fixed_bias = getenv("MVC2_GE_MIP_BIAS") != NULL ? (float)atof(getenv("MVC2_GE_MIP_BIAS")) : -0.5f;
    mip_bias = front_bias = fixed_bias;
    if (getenv("MVC2_GE_MIP_AUTO") != NULL) {
        unsigned hi = auto_hi, lo = auto_lo;
        auto_on = sscanf(getenv("MVC2_GE_MIP_AUTO"), "%u,%u", &hi, &lo) >= 1 && hi != 0;
        auto_hi = hi;
        auto_lo = lo;
    }
    list_uncached = getenv("MVC2_GE_LIST_UNCACHED") != NULL && atoi(getenv("MVC2_GE_LIST_UNCACHED")) != 0;
    sections = getenv("MVC2_GE_SECTIONS") == NULL || atoi(getenv("MVC2_GE_SECTIONS")) != 0;
    list_crc = getenv("MVC2_GE_CRC") != NULL && atoi(getenv("MVC2_GE_CRC")) != 0;
    auto_on = auto_on && getenv("MVC2_GE_MIP_BIAS") == NULL && !list_crc;
    if (auto_on) {
        host_log("GE: the scene's made levels at -0.5 while frames take %u us or less, then +0.5 until %u us or less",
                 (unsigned)auto_hi, (unsigned)auto_lo);
    }
    if (list_crc) {
        hash_open();
    }
    rd_frames[0] = rd_frame;
    if (getenv("MVC2_ME") != NULL && strcmp(getenv("MVC2_ME"), "0") != 0) {
        int late = strcmp(getenv("MVC2_ME"), "late") == 0;
        rd_frames[1] = memalign(64, sizeof *rd_frames[1]);
        if (rd_frames[1] == NULL) {
            host_log("GE: no room for a second frame: each list made here");
        } else if (!late && !(strcmp(getenv("MVC2_ME"), "thread") == 0 ? psp_me_start_thread() : psp_me_start())) {
            host_log("GE: the Media Engine didn't answer: each list made here");
        } else {
            if (!late) {
                /* a sleep gives the ME back to the firmware and takes it again on waking (psp_me_frame);
                   MVC2_ME_SLEEP=0 keeps it and the power switch held off */
                psp_me_refuse_sleep(getenv("MVC2_ME_SLEEP") != NULL && atoi(getenv("MVC2_ME_SLEEP")) == 0);
                me_ahead = getenv("MVC2_ME_AHEAD") != NULL ? (uint32_t)atoi(getenv("MVC2_ME_AHEAD")) & ~63u : 1024;
            }
            memset(rd_frames[1], 0, sizeof *rd_frames[1]);
            me_check = getenv("MVC2_ME_CHECK") != NULL && atoi(getenv("MVC2_ME_CHECK")) != 0;
            me_time = getenv("MVC2_ME_TIME") != NULL && atoi(getenv("MVC2_ME_TIME")) != 0;
            list_where = late ? LIST_LATE : LIST_ME;
            host_log(late ? "GE: each list made a frame late, at the next frame's end"
                          : "GE: each list made by the Media Engine, beside the next frame");
        }
    }
    sceGuSetCallback(GU_CALLBACK_FINISH, on_finish);
    sceGuSetCallback(GU_CALLBACK_SIGNAL, on_signal);
    /* MVC2_GE_PROBE=START,FRAMES[,CYCLES] */
    if (getenv("MVC2_GE_PROBE") != NULL &&
        sscanf(getenv("MVC2_GE_PROBE"), "%d,%d,%d", &probe_start, &probe_frames, &probe_cycles) >= 2 &&
        probe_frames > 2) {
        host_log("GE: probes from frame %d, %d frames each", probe_start, probe_frames);
    } else {
        probe_frames = 0;
    }
    cache_init();
    {
        /* MVC2_GE_RAM_KB: the big ones' main RAM, a test of a tight budget */
        uint32_t vram_size, big = getenv("MVC2_GE_RAM_KB") != NULL ? (uint32_t)atoi(getenv("MVC2_GE_RAM_KB")) << 10
                                                                    : BIG_POOL;
        void* vram = psp_video_vram(&vram_size);
        uint8_t* ram = memalign(64, big + SMALL_POOL);
        if (ram == NULL) {
            return 0;
        }
        pool_init(&pools[0], vram, vram_size);
        pool_init(&pools[1], ram, big);
        slot_new = ram + big;
        slot_end = slot_new + SMALL_POOL;
        atlas_init();
        host_log("GE: textures in %lu KB of VRAM and %lu KB of main RAM, %lu KB of it for small ones",
                 (unsigned long)vram_size >> 10, (unsigned long)(big + SMALL_POOL) >> 10,
                 (unsigned long)SMALL_POOL >> 10);
    }
    return 1;
}

static uint32_t now_us(void) {
    return sceKernelGetSystemTimeLow();
}

/* each probe's times, for the frames past the first two of its turn */
static struct {
    uint32_t frames, ge_us, cpu_us, wait_us, gap_us, sec_us[SECTIONS];
} probes[PROBES];

static void probe_log(void) {
    int k;
    for (k = 0; k < PROBES; k++) {
        double f = probes[k].frames ? probes[k].frames : 1;
        host_log("  probe %-14s %4u frames: GE %5.2f ms (background %.2f, opaque %.2f, punch-through %.2f, "
                 "translucent %.2f, after %.2f), CPU %.2f ms, waited %.2f ms, frame to frame %.2f ms", probe_name[k],
                 probes[k].frames, probes[k].ge_us / f / 1000.0, probes[k].sec_us[0] / f / 1000.0,
                 probes[k].sec_us[1] / f / 1000.0, probes[k].sec_us[2] / f / 1000.0, probes[k].sec_us[3] / f / 1000.0,
                 probes[k].sec_us[4] / f / 1000.0, probes[k].cpu_us / f / 1000.0, probes[k].wait_us / f / 1000.0,
                 probes[k].gap_us / f / 1000.0);
    }
}

static volatile uint32_t ge_last_us;     /* the GE's time on the last frame it drew */

/* the frame the GE was given last: when, what it was, and whether its times count for its probe */
static int kicked, kick_probe, kick_counts;
static unsigned kick_frame;
static uint32_t kick_at, kick_cpu;
static unsigned kick_fin, kick_sig;

/* the GE's times for the frame it has drawn, with the CPU's and the wait for it */
static void drawn_times(uint32_t wait) {
    uint32_t fin, at = kick_at, sec[SECTIONS] = { 0 };
    int k;
    if (!kicked || nfin == kick_fin) {
        return;
    }
    fin = fin_at[kick_fin & 7];
    for (k = 0; k < SECTIONS - 1 && sections && kick_sig + (unsigned)k < nsig; k++) {
        uint32_t t = sig_at[(kick_sig + (unsigned)k) & 31];
        sec[k] = t - at;
        at = t;
    }
    sec[k] = fin - at;
    psp_rstats.ge_us += fin - kick_at;
    ge_last_us = fin - kick_at;
    psp_rstats.wait_us += wait;
    for (k = 0; k < SECTIONS; k++) {
        psp_rstats.sec_us[k] += sec[k];
    }
    if (kick_counts) {
        probes[kick_probe].frames++;
        probes[kick_probe].ge_us += fin - kick_at;
        probes[kick_probe].cpu_us += kick_cpu;
        probes[kick_probe].wait_us += wait;
        for (k = 0; k < SECTIONS; k++) {
            probes[kick_probe].sec_us[k] += sec[k];
        }
    }
}

/* what the list is made with this frame */
float mip_bias, front_bias;         /* the made levels' bias: the scene's and the front's */
RD_FOG list_fog;
int trans_sorted;
int fog_on, fog_off;
float fog_cut, fog_cut_w;
uint32_t fog_rgb;
int list_cached;
static LIST_JOB job;

/* the list's counts into the frame's */
static void add_list_stats(const LIST_STATS* st) {
    static int full_said;
    int k;
    for (k = 0; k < 4; k++) {
        psp_rstats.cpu_sec[k] += st->sec_us[k];
    }
    psp_rstats.tris += st->tris;
    psp_rstats.batches += st->batches;
    psp_rstats.clipped += st->clipped;
    psp_rstats.sprites += st->sprites;
    psp_rstats.skipped += st->skipped;
    psp_rstats.dropped += st->dropped;
    psp_rstats.offset_tris += st->offset_tris;
    psp_rstats.mirrored += st->mirrored;
    psp_rstats.groups += st->groups;
    psp_rstats.group_tris += st->group_tris;
    if (st->tiles_full != 0) {
        log_once(&full_said, "a frame's tiles drawn wrapped more than it has room for: some drawn untextured");
    }
}

/* MVC2_FRAME_START: each frame's input read as late as the last frames' times allow, for its picture to be shown a
   vblank after the last one */
static uint32_t chains[16];         /* the last frames' input to the picture drawn, waits left out (here: to the kick) */
static unsigned nchains;
static uint32_t began;              /* the frame being made: when its input was read */
static uint32_t due;                /* the vblank the frame last begun was meant to be shown from */
static uint32_t k_shown, k_shown_frame;     /* the kicker's last picture: the vblank it is shown from, its frame */
static uint32_t me_us = 6000;       /* the ME's last list, as the kicker saw it */

static void chain_add(uint32_t us) {
    chains[nchains++ % 16] = us;
}

static int later(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) > 0;
}

void psp_render_input(void) {
    began = sceKernelGetSystemTimeLow();
}

/* the frame whose list is made and not yet given to the GE */
static struct {
    int pending, b, probe, counts;
    int sharp;                      /* the scene's made levels at -0.5 */
    int on_me;                      /* its list being made there */
    unsigned frame;
    uint32_t cpu_us;
    uint32_t posted;                /* when it was given to the ME */
    uint32_t work;                  /* the main CPU's time on it from its input, waits left out */
    uint32_t* up_end;
} made_frame;

/* The scene's made levels from the first level up to two texels a pixel while the frame has room, else +0.5: in a
   fight the GE's reads of them slow the main CPU past the frame. A frame takes the longer of the main CPU's work and
   the GE's; six of eight sharp frames over HI turn them to +0.5, and auto_need frames with at most one of the last
   eight over LO back, that doubling (to 960) each time a sharp spell ends inside 600 frames. The front's are always
   at -0.5. */
static int auto_sharp = 1, auto_quiet, auto_held, auto_need = 120;
static uint32_t auto_over;          /* the last eight frames, a bit each: over HI while sharp, LO while not */
static uint32_t auto_n[2], auto_us[2];

static void auto_levels(unsigned frame) {
    uint32_t took = made_frame.work > ge_last_us ? made_frame.work : ge_last_us;
    int was = made_frame.sharp;
    if (!auto_on) {
        mip_bias = front_bias = fixed_bias;
        return;
    }
    auto_n[was]++;
    auto_us[was] += took;
    if (auto_sharp) {
        auto_over = (auto_over << 1 | (was && took > auto_hi)) & 0xFF;
        auto_held++;
        if (__builtin_popcount(auto_over) >= 6) {
            auto_need = auto_held >= 600 ? 120 : auto_need * 2 > 960 ? 960 : auto_need * 2;
            auto_sharp = auto_quiet = 0;
            auto_over = 0;
            host_log("GE: frame %u: the scene's made levels at +0.5 (%u us)", frame, (unsigned)took);
        }
    } else {
        auto_over = (auto_over << 1 | (was || took > auto_lo)) & 0xFF;
        auto_quiet = __builtin_popcount(auto_over) <= 1 ? auto_quiet + 1 : 0;
        if (auto_quiet >= auto_need) {
            auto_sharp = 1;
            auto_held = 0;
            auto_over = 0;
            host_log("GE: frame %u: the scene's made levels at -0.5 (%u us)", frame, (unsigned)took);
        }
    }
    if (auto_n[0] + auto_n[1] == 300) {
        host_log("GE: made levels sharp in %u of 300 frames, taking %.2f ms; %.2f ms not", (unsigned)auto_n[1],
                 auto_us[1] / (auto_n[1] ? auto_n[1] : 1) / 1000.0, auto_us[0] / (auto_n[0] ? auto_n[0] : 1) / 1000.0);
        auto_n[0] = auto_n[1] = auto_us[0] = auto_us[1] = 0;
    }
    front_bias = -0.5f;
    mip_bias = auto_sharp ? -0.5f : 0.5f;
}

/* the frame's inputs to its list, from rd's as the game left them: whether its times count for its probe */
static int prepare(int stretch, unsigned frame, uint32_t t0) {
    static uint32_t last_start;
    static int last_stretch = -1, clears_left, last_probe;
    int i, b, counts = 0, steps;
    uint32_t t;
    frame_no++;
    flight_from = frame_no - 1;
    if (rd_frame->dropped > 0) {
        static unsigned dropped_frames;
        if (dropped_frames++ % 300 == 0) {
            host_log("GE: frame %u: %d triangles past the full pool (%u frames so far)", frame, rd_frame->dropped,
                     dropped_frames);
        }
    }
    b = frame_no & 1;
    clist = clists[b];
    verts = verts_buf[b];
    over = over_buf[b];
    pal = pal_buf[b];
    list_frame = rd_frame;
    if (rd_palette_serial != pal_serial[b]) {
        pal_serial[b] = rd_palette_serial;
        for (i = 0; i < 1024; i++) {
            pal[i] = abgr(rd_palette[i]);
        }
        sceKernelDcacheWritebackRange(pal, sizeof pal_buf[b]);
    }
    probe = P_NONE;
    if (probe_frames > 0 && frame >= (unsigned)probe_start) {
        unsigned at = frame - (unsigned)probe_start;
        int done = probe_cycles > 0 && at / (unsigned)probe_frames / PROBES >= (unsigned)probe_cycles;
        probe = done ? P_NONE : (int)(at / (unsigned)probe_frames % PROBES);
        counts = !done && at % (unsigned)probe_frames >= 2;
        if (counts && last_start != 0) {
            probes[probe].gap_us += t0 - last_start;
        }
        if (probe == P_NONE && last_probe == PROBES - 1) {
            probe_log();
        }
        last_probe = probe;
    }
    last_start = t0;
    auto_levels(frame);
    /* the table's last entry not 0 is at L: from 16 entries a doubling past density * 1/w = 1, the value is 0 where
       density * 1/w is at least (1 + (L + 1) % 16 / 16) * 2^((L + 1) / 16) */
    list_fog = rd_fog;
    trans_sorted = !rd_q.nosort;
    fog_on = 0;
    for (i = 0; i < 128; i++) {
        if (list_fog.table[i] != 0.0f) {
            fog_on = i + 1;
        }
    }
    fog_cut = fog_on >= 127 || !(list_fog.density > 0.0f)
                  ? 1e30f
                  : (1.0f + (float)(fog_on & 15) / 16.0f) * (float)(1u << (fog_on >> 4)) / list_fog.density;
    fog_cut_w = 1.0f / fog_cut;
    fog_off = rd_q.nofog;
    fog_rgb = abgr(list_fog.color) & 0xFFFFFFu;
    layout_depth();
    list_cached = !list_uncached && probe != P_LIST_UNCACHED;
    up_list = up_lists[b];
    up_cur = up_list;
    t = now_us();
    make_textures();
    /* with MVC2_GE_CRC four bands a frame, so that the lists' hashes don't follow the clock */
    for (steps = 0; (list_crc ? steps < 4 : now_us() - t < mip_us) && mip_step(); steps++) {
    }
    *up_cur++ = (uint32_t)GE_RET << 24;
    sceKernelDcacheWritebackRange(up_list, (unsigned)(up_cur - up_list) * 4);
    if (kept_hi > kept_lo) {
        sceKernelDcacheWritebackRange(kept_rows + kept_lo * KEPT_BYTES, (unsigned)(kept_hi - kept_lo) * KEPT_BYTES);
        kept_lo = KEPT;
        kept_hi = 0;
    }
    if (stretch != last_stretch || clear_again > 0) {
        last_stretch = stretch;
        clears_left = clear_again > 0 ? clear_again : 2;
        clear_again = 0;
    }
    if (frame % 600 == 0) {
        pillars_left = 2;   /* again now and then, in both buffers, should anything have drawn over the bars */
    }
    job.in.clear = clears_left > 0;
    job.in.pillars = clears_left > 0 || pillars_left > 0 ? pillar_tex : NULL;
    clears_left -= job.in.clear;
    pillars_left -= pillars_left > 0;
    job.in.stretch = stretch;
    job.in.clock = now_us;
    job.in.ahead = 2048;
    made_frame.b = b;
    made_frame.probe = probe;
    made_frame.counts = counts;
    made_frame.sharp = mip_bias < 0.0f;
    made_frame.frame = frame;
    made_frame.up_end = up_cur;
    return counts;
}

static __attribute__((cold)) void hash_frame(int b, unsigned frame) {
    static const char* const kind[FH_N] = { "uploads", "matrices", "textures", "primitives", "the rest", "vertices" };
    uint32_t h[FH_N], vb = (uint32_t)job.out.nverts * sizeof(VTX), ob = (uint32_t)job.out.nover * sizeof(VTX);
    unsigned i, k;
    for (k = 0; k < FH_N; k++) {
        h[k] = 2166136261u;
    }
    if (made_frame.on_me) {
        sceKernelDcacheInvalidateRange(verts_buf[b], (vb + 63) & ~63u);
        sceKernelDcacheInvalidateRange(over_buf[b], (ob + 63) & ~63u);
    }
    hash_kinds(up_lists[b], made_frame.up_end - 1, h, FH_UP);
    hash_kinds(ge_list.start, ge_list.cur, h, -1);
    h[FH_VERTS] = fnv(fnv(h[FH_VERTS], (const uint32_t*)verts_buf[b], vb / 4), (const uint32_t*)over_buf[b], ob / 4);
    if (hash_text != NULL && hash_len + 80 <= HASH_TEXT) {
        size_t n = hash_len;
        n += (size_t)snprintf(hash_text + n, 80, "%u %08lx %08lx %08lx %08lx %08lx %08lx\n", frame, (unsigned long)h[0],
                              (unsigned long)h[1], (unsigned long)h[2], (unsigned long)h[3], (unsigned long)h[4],
                              (unsigned long)h[5]);
        __asm__ volatile("" ::: "memory");
        hash_len = n;
    }
    static unsigned at;
    for (i = at; i < nhash_ref && hash_ref[i][0] < frame; i++) {}
    at = i;
    if (i < nhash_ref && hash_ref[i][0] != frame) {
        i = nhash_ref;
    }
    if (i == nhash_ref || memcmp(&hash_ref[i][1], h, sizeof h) == 0 || hash_bad >= 10) {
        return;
    }
    {
        char text[160];
        int n = snprintf(text, sizeof text, "GE: frame %u differs from %s in", frame, hash_ref_path);
        for (k = 0; k < FH_N; k++) {
            if (hash_ref[i][1 + k] != h[k] && n < (int)sizeof text) {
                n += snprintf(text + n, sizeof text - (size_t)n, " %s", kind[k]);
            }
        }
        host_log("%s", text);
    }
    if (hash_bad++ == 0) {
        char path[256];
        FILE* f;
        uint32_t head[4];
        snprintf(path, sizeof path, "%s.bad", hash_ref_path);
        if ((f = fopen(path, "wb")) != NULL) {
            head[0] = frame;
            head[1] = (uint32_t)(made_frame.up_end - 1 - up_lists[b]) * 4;
            head[2] = (uint32_t)((const char*)ge_list.cur - (const char*)ge_list.start);
            head[3] = vb + ob;
            fwrite(head, sizeof head, 1, f);
            fwrite(up_lists[b], 1, head[1], f);
            fwrite(ge_list.start, 1, head[2], f);
            fwrite(verts_buf[b], 1, vb, f);
            fwrite(over_buf[b], 1, ob, f);
            fclose(f);
            host_log("GE: frame %u's lists written to %s", frame, path);
        }
    }
}

static void hash_write(int now) {
    static unsigned n;
    size_t len = hash_len;
    SceUID fd;
    if (hash_text == NULL || len == hash_written || (++n % 60 != 0 && !now)) {
        return;
    }
    fd = sceIoOpen(hash_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, hash_text + hash_written, (SceSize)(len - hash_written));
        sceIoClose(fd);
    }
    hash_written = len;
}

/* the list just made: its counts, and its words written back for the GE (the Media Engine's own) */
static void list_finished(void) {
    int b = made_frame.b;
    unsigned list_bytes = job.out.bytes;
    add_list_stats(&job.out.st);
    if (!made_frame.on_me) {
        sceKernelDcacheWritebackRange(clists[b], list_bytes);
        sceKernelDcacheWritebackRange(verts_buf[b], (unsigned)job.out.nverts * sizeof *verts);
        sceKernelDcacheWritebackRange(over_buf[b], (unsigned)job.out.nover * sizeof *over);
        sceKernelDcacheWritebackRange(tile_mem[b], (unsigned)job.out.ntile_tex * sizeof tile_mem[0][0]);
    }
    HOST_WRITTEN(HOST_W_LIST, list_bytes + (unsigned)(job.out.nverts + job.out.nover) * sizeof *verts);
    psp_rstats.verts += (uint32_t)job.out.nverts;
    if (list_crc) {
        hash_list(up_lists[b], made_frame.up_end - 1);
        hash_list(ge_list.start, ge_list.cur);
        if (hash_text != NULL || nhash_ref != 0) {
            hash_frame(b, made_frame.frame);
        }
        if (made_frame.frame % 300 == 0) {
            host_log("GE: the lists to frame %u hash to %08lx", made_frame.frame, (unsigned long)crc_at);
        }
    }
}

/* the window's counts for psp_render_window */
static struct {
    uint32_t frames, prep_us, wait_us;
    uint32_t moved, moved_frames;  /* the triangle pool's parts laid out again for a full one; frames that did */
    uint32_t on_me, busy;           /* lists the ME made; of them, those not done by the next frame's end */
    uint32_t here;                  /* lists made by the main CPU while the ME was asked for */
    /* MVC2_ME_TIME: the kicker's times: post to its first look, its waits for the ME, the back buffer and the GE */
    uint32_t k_look, k_me, k_back, k_ge, k_after, n_after;
    uint32_t held_us;               /* MVC2_FRAME_START's waits */
    uint32_t pictures, late, over;  /* the kicker's; those shown more than a vblank after the last, or by the next */
} win;

/* the Media Engine's list waited for, and what the main CPU reads of it taken from RAM, not its cache */
/* psp_render_me_after's job: given to the ME once each frame's list is done there, if ready says so; its end seen at
   the next list's post or at psp_render_me_after_wait */
static void (*after_fn)(void*), (*after_done)(void*);
static int (*after_ready)(void*);
static void* after_arg;
static volatile int after_posted;
static uint32_t after_at, after_seen;   /* MVC2_ME_TIME: its post, and its end where the kicker saw it */

void psp_render_me_after(void (*f)(void*), int (*ready)(void*), void (*done)(void*), void* arg) {
    after_fn = f;
    after_ready = ready;
    after_done = done;
    after_arg = arg;
}

int psp_render_me_takes(void) {
    return list_where == LIST_ME && psp_me_ours() != 0;
}

static void after_post(void) {
    if (after_fn != NULL && psp_me_ours() && (after_ready == NULL || after_ready(after_arg)) &&
        psp_me_post(after_fn, after_arg)) {
        after_posted = 1;
        after_at = now_us();
        after_seen = 0;
    }
}

static void after_end(void) {
    if (after_posted) {
        psp_me_wait(0xFFFFFFFFu);
        if (me_time) {
            win.k_after += (after_seen != 0 ? after_seen : now_us()) - after_at;
            win.n_after++;
        }
        after_posted = 0;
        if (after_done != NULL) {
            after_done(after_arg);
        }
    }
}

void psp_render_me_after_wait(void) {
    after_end();
}

/* f(on_me) called once each frame's list is given to the ME (or, on_me 0, kept to be made here), the last frame's
   after-job ended */
static void (*before_post)(int);

void psp_render_me_before(void (*f)(int on_me)) {
    before_post = f;
}

static void list_from_me(void) {
    static int late_said;
    uint32_t t = now_us();
    if (!psp_me_wait(0)) {
        win.busy++;
        if (!psp_me_wait(50000)) {
            log_once(&late_said, "the Media Engine's list took more than 50 ms");
            psp_me_wait(0xFFFFFFFFu);
        }
    }
    win.wait_us += now_us() - t;
    win.on_me++;
    sceKernelDcacheInvalidateRange(&job.out, sizeof job.out);
    if (list_crc) {
        sceKernelDcacheInvalidateRange(&ge_list, sizeof ge_list);
        sceKernelDcacheInvalidateRange(clists[made_frame.b], (job.out.bytes + 63) & ~63u);
    }
    after_post();
}

/* MVC2_ME_CHECK=1: each Media Engine's list made again here and compared, word by word, the first differences logged */
static uint8_t* check_buf;

static int compare_words(const char* what, const void* me, const void* here, unsigned bytes) {
    static int said;
    const uint32_t *a = me, *b = here;
    unsigned i, n = bytes / 4, bad = 0, first = 0;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            first = bad++ ? first : i;
        }
    }
    if (bad != 0 && said < 20) {
        said++;
        host_log("ME check: frame %u's %s differs in %u of %u words, the first at word %u: ME %08lx, here %08lx",
                 made_frame.frame, what, bad, n, first, (unsigned long)a[first], (unsigned long)b[first]);
    }
    return bad != 0;
}

static void check_me_list(void) {
    static unsigned checked, bad;
    int b = made_frame.b;
    LIST_JOB me = job;
    unsigned lb = (job.out.bytes + 63) & ~63u, vb = ((unsigned)job.out.nverts * sizeof(VTX) + 63) & ~63u,
             ob = ((unsigned)job.out.nover * sizeof(VTX) + 63) & ~63u;
    if (check_buf == NULL && (check_buf = memalign(64, LIST_BYTES + (MAX_VERTS + MAX_OVER) * sizeof(VTX))) == NULL) {
        return;
    }
    sceKernelDcacheInvalidateRange(clists[b], lb);
    sceKernelDcacheInvalidateRange(verts_buf[b], vb);
    sceKernelDcacheInvalidateRange(over_buf[b], ob);
    memcpy(check_buf, clists[b], job.out.bytes);
    memcpy(check_buf + LIST_BYTES, verts_buf[b], vb);
    memcpy(check_buf + LIST_BYTES + MAX_VERTS * sizeof(VTX), over_buf[b], ob);
    sceKernelDcacheWritebackInvalidateAll();
    job.in.clock = NULL;
    psp_list_job(&job);
    made_frame.on_me = 0;           /* this list written back as the main CPU's */
    checked++;
    if (me.out.bytes != job.out.bytes || me.out.nverts != job.out.nverts || me.out.nover != job.out.nover) {
        host_log("ME check: frame %u's sizes differ: ME %u bytes, %d vertices, %d over; here %u, %d, %d",
                 made_frame.frame, me.out.bytes, me.out.nverts, me.out.nover, job.out.bytes, job.out.nverts,
                 job.out.nover);
        bad++;
    } else {
        int d = compare_words("list", check_buf, clists[b], job.out.bytes);
        d |= compare_words("vertices", check_buf + LIST_BYTES, verts_buf[b], (unsigned)job.out.nverts * sizeof(VTX));
        d |= compare_words("fog vertices", check_buf + LIST_BYTES + MAX_VERTS * sizeof(VTX), over_buf[b],
                           (unsigned)job.out.nover * sizeof(VTX));
        bad += d;
    }
    if (checked % 300 == 0) {
        host_log("ME check: %u of %u lists differ", bad, checked);
    }
}

/* the frame's list given to the Media Engine where it is ours this frame (a sleep may have taken it), else made here
   at the next frame's end; the list's lines the main CPU kept emptied when it takes the list back */
static void list_to_me(void) {
    static int was_on_me, first = 1;
    after_end();
    if (list_where == LIST_ME && psp_me_sleep_asked()) {
        host_log("ME: a sleep asked for at frame %u", made_frame.frame);
        psp_log_flush();    /* written before the give-back, in case it doesn't end */
    }
    made_frame.on_me = list_where == LIST_ME && psp_me_frame();
    if (made_frame.on_me) {
        job.in.clock = NULL;
        job.in.ahead = me_ahead;
        made_frame.posted = now_us();
        psp_me_post(psp_list_job, &job);
    } else if (was_on_me) {
        sceKernelDcacheWritebackInvalidateAll();
    }
    if (before_post != NULL) {
        before_post(made_frame.on_me);
    }
    if (list_where == LIST_ME && made_frame.on_me != was_on_me && !first) {
        host_log(made_frame.on_me ? "ME: ours again at frame %u" : "ME: given back for a sleep at frame %u (%d)",
                 made_frame.frame, psp_me_given_back());
    }
    was_on_me = made_frame.on_me;
    first = 0;
}

/* the kicker: a thread above the game's that gives the Media Engine's list to the GE as soon as it is done, and
   shows it as soon as the GE has drawn it, rather than at the game's frame ends (a frame sooner on the screen).
   It owns sceGu while it runs; the frame's end waits until it has given its list to the GE */
static int kicker_on;               /* MVC2_KICKER, on with MVC2_ME unless 0 */
static int with_kicker;             /* the frame's end gave the kicker the last frame's list (the main thread's own) */
static SceUID kick_job, kick_given, kick_idle;
static void kick(void);

static uint32_t kicker_fcsr;

static int kicker(SceSize n, void* p) {
    (void)n;
    (void)p;
    /* the main CPU's rounding and flush, for the ME check's list made here */
    __asm__ volatile("ctc1 %0, $31" : : "r"(kicker_fcsr & 0x01000003u));
    for (;;) {
        uint32_t t, looked, done = 0, work;
        sceKernelWaitSema(kick_job, 1, NULL);
        sceKernelClearEventFlag(kick_idle, 0);
        looked = now_us();
        work = made_frame.work;
        while (!psp_me_wait(0)) {
            sceKernelDelayThread(me_time ? 50 : 200);
            done = now_us();
        }
        if (done != 0 || looked - made_frame.posted < me_us) {
            me_us = (done != 0 ? done : looked) - made_frame.posted;
        }
        sceKernelDcacheInvalidateRange(&job.out, sizeof job.out);
        win.k_look += looked - made_frame.posted;
        win.k_me += (done != 0 ? done : looked) - looked;
        if (list_crc) {
            sceKernelDcacheInvalidateRange(&ge_list, sizeof ge_list);
            sceKernelDcacheInvalidateRange(clists[made_frame.b], (job.out.bytes + 63) & ~63u);
        }
        after_post();
        if (me_check) {
            check_me_list();
        }
        list_finished();
        if (kicked) {
            /* a list the frame's end gave the GE itself (the ME wasn't ours that frame): shown first */
            sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
            drawn_times(0);
            psp_video_flip();
            psp_lat_shown(kick_frame);
            kicked = 0;
        }
        t = now_us();
        psp_video_wait_back();
        win.k_back += now_us() - t;
        kick();
        sceKernelSignalSema(kick_given, 1);
        t = now_us();
        sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
        win.k_ge += now_us() - t;
        if (me_time && after_posted && after_seen == 0 && psp_me_wait(0)) {
            after_seen = now_us();
        }
        drawn_times(now_us() - t);
        chain_add(work + me_us + now_us() - t);
        psp_video_flip();
        if (k_shown_frame != 0) {
            uint32_t d = psp_video_shown_us() - k_shown;
            win.pictures++;
            win.late += d > psp_video_frame_us() * 3 / 2;
            win.over += d < psp_video_frame_us() / 2;
        }
        k_shown = psp_video_shown_us();
        k_shown_frame = kick_frame;
        psp_lat_shown(kick_frame);
        kicked = 0;
        sceKernelSetEventFlag(kick_idle, 1);
    }
    return 0;
}

static void kicker_start(void) {
    SceUID th;
    const char* e = getenv("MVC2_KICKER");
    __asm__ volatile("cfc1 %0, $31" : "=r"(kicker_fcsr));
    if (list_where != LIST_ME || (e != NULL && atoi(e) == 0)) {
        return;
    }
    if (e == NULL && getenv("MVC2_SHOT") != NULL) {
        host_log("GE: no kicker thread with MVC2_SHOT (its shots would be of the frame before)");
        return;
    }
    kick_job = sceKernelCreateSema("kick job", 0, 0, 1, NULL);
    kick_given = sceKernelCreateSema("kick given", 0, 0, 1, NULL);
    kick_idle = sceKernelCreateEventFlag("kick idle", 0, 1, NULL);
    th = sceKernelCreateThread("kicker", kicker, sceKernelGetThreadCurrentPriority() - 2, 0x4000, PSP_THREAD_ATTR_USER,
                               NULL);
    kicker_on = kick_job >= 0 && kick_given >= 0 && kick_idle >= 0 && th >= 0 && sceKernelStartThread(th, 0, NULL) >= 0;
    psp_video_latch(kicker_on && (getenv("MVC2_LATCH") == NULL || atoi(getenv("MVC2_LATCH")) != 0));
    host_log(kicker_on ? "GE: each list given to the GE as the Media Engine finishes it, and shown as the GE does"
                       : "GE: no kicker thread: each list given to the GE at the frame's end");
}

/* as psp_render_idle, for us microseconds at most: 0 if the kicker is still busy */
int psp_render_idle_within(uint32_t us) {
    SceUInt wait = us;
    return !kicker_on || sceKernelWaitEventFlag(kick_idle, 1, PSP_EVENT_WAITOR, NULL, &wait) >= 0;
}

void psp_render_idle(void) {
    if (kicker_on) {
        sceKernelWaitEventFlag(kick_idle, 1, PSP_EVENT_WAITOR, NULL, NULL);
    }
}

/* at the end: the kicker waited for at most 2 s, then the hash log's last lines written */
void psp_render_end(void) {
    SceUInt wait = 2000000;
    if (kicker_on && sceKernelWaitEventFlag(kick_idle, 1, PSP_EVENT_WAITOR, NULL, &wait) < 0) {
        host_log("GE: the kicker still busy at the end");
    }
    hash_write(1);
}

/* the frames given to the GE drawn: none but the frame being made holds a texture now */
static void all_drawn(void) {
    if (flight_from != frame_no) {
        psp_render_idle();
        sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
        flight_from = frame_no;
    }
}

/* the frame the GE has drawn waited for and shown; then the frame made, if there is one, given to it */
static void kick(void) {
    uint32_t t1 = now_us(), t2;
    int b = made_frame.b;
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    t2 = now_us();
    drawn_times(t2 - t1);
    if (kicked) {
        psp_video_flip();
        psp_lat_shown(kick_frame);
        kicked = 0;
    }
    if (!made_frame.pending) {
        return;
    }
    made_frame.pending = 0;
    kick_frame = made_frame.frame;
    kick_fin = nfin;
    kick_sig = nsig;
    kick_probe = made_frame.probe;
    kick_counts = made_frame.counts;
    kick_cpu = made_frame.cpu_us;
    kicked = 1;
    sceGuStart(GU_DIRECT, dlists[b]);
    psp_video_target(screen16);
    kick_at = now_us();
    if (made_frame.up_end - 1 > up_lists[b]) {
        sceGuCallList(up_lists[b]);
    }
    sceGuCallList(clists[b]);
    sceGuFinish();
}

static void frame_stats(uint32_t cpu_us) {
    psp_rstats.cpu_us += cpu_us;
    psp_rstats.frames++;
    psp_rstats.tex_kb = (pools[0].used + pools[1].used + slot_bytes) >> 10;
    psp_rstats.tex_vram_kb = pools[0].used >> 10;
    psp_rstats.tex_count = (uint32_t)ngtex;
    psp_rstats.mesh_kb = mesh_pool.used >> 10;
}

/* a loaded PAL4 square's texels, a byte each, back to the twiddled 2x2 blocks the PSP's are kept as */
static void replay_unpack(const RD_TEXTURE* t, RD_TEXTURE* out) {
    const uint16_t* src = t->twiddled;
    uint8_t* o = out->texels;
    int x, y, n = t->width;
    for (y = 0; y < n; y++) {
        for (x = 0; x < n; x++) {
            uint16_t w = src[spread_bits((uint32_t)x >> 1) << 1 | spread_bits((uint32_t)y >> 1)];
            o[y * n + x] = (uint8_t)(w >> (4 * ((x & 1) * 2 + (y & 1))) & 15);
        }
    }
}

static void replay_textures(void) {
    static uint32_t serial = 0x7F000000u;
    int i, x, y;
    for (i = 0; i < rd_frame->nstates; i++) {
        RD_TEXTURE* t = (RD_TEXTURE*)rd_frame->states[i].tex;
        uint16_t* w;
        int n;
        if (t == NULL || t->serial != 0) {
            continue;
        }
        t->serial = ++serial;
        n = t->width;
        if (t->format != RD_PAL4 || t->width != t->height || n < 8 || n > 512 || t->levels != 0 ||
            (w = calloc((size_t)n * n / 4, 2)) == NULL) {
            continue;
        }
        for (y = 0; y < n; y++) {
            for (x = 0; x < n; x++) {
                uint16_t* q = &w[spread_bits((uint32_t)x >> 1) << 1 | spread_bits((uint32_t)y >> 1)];
                *q |= (uint16_t)((((const uint8_t*)t->texels)[y * n + x] & 15) << (4 * ((x & 1) * 2 + (y & 1))));
            }
        }
        t->twiddled = w;
        t->texels = w;
        t->unpack = replay_unpack;
        t->unpacked = (uint32_t)n * n;
    }
}

/* the triangle pool's moves in the frame just made */
static void pool_moves(void) {
    win.moved += (uint32_t)rd_frame->moved;
    win.moved_frames += rd_frame->moved != 0;
}

/* MVC2_RDLOAD=n:file,...: frame n drawn from a frame saved with MVC2_RDSAVE (on the PC, say) in place of the game's */
static void replay(unsigned frame) {
    const char* e = getenv("MVC2_RDLOAD");
    char name[160];
    unsigned at = 0;
    int list, i;
    while (e != NULL && !(sscanf(e, "%u:%159[^,]", &at, name) == 2 && at == frame)) {
        e = strchr(e, ',');
        e = e != NULL ? e + 1 : NULL;
    }
    if (e == NULL) {
        return;
    }
    if (!rd_frame_load(name)) {
        host_log("GE: %s not read", name);
        return;
    }
    replay_textures();
    rd_zrange_begin();
    for (list = 0; list < RD_LISTS; list++) {
        for (i = 0; i < rd_frame->ntris[list]; i++) {
            const RD_TRI* t = &rd_frame->tris[list][i];
            rd_tri_z(list, &t->v[0], &t->v[1], &t->v[2]);
        }
    }
    host_log("GE: frame %u drawn from %s", frame, name);
}

/* the frame's list made and given to the GE, which draws it while the game makes the next; or, a frame behind, the
   last frame's made, given to it, and this one's begun */
int psp_render_frame(int stretch, unsigned frame) {
    uint32_t t0 = now_us(), t1, t2, tp;
    if (!ready) {
        ready = 1;
        failed = !init();
        if (failed) {
            host_log("GE: no memory for the lists");
        } else {
            const char* e = getenv("MVC2_RD_ITEMS");
            rd_items = e == NULL || strcmp(e, "0") != 0;
        }
    }
    if (failed) {
        return 0;
    }
    if (ready == 1) {
        ready = 2;
        kicker_start();
    }
    replay(frame);
    hash_write(0);
    if (list_where == LIST_HERE) {
        prepare(stretch, frame, t0);
        pool_moves();
        tp = now_us();
        psp_rstats.cpu_sec[0] += tp - t0;
        psp_list_job(&job);
        tp = now_us();
        list_finished();
        t1 = now_us();
        psp_rstats.cpu_sec[4] += t1 - tp;
        made_frame.cpu_us = t1 - t0;
        made_frame.pending = 1;
        made_frame.work = t1 - began;
        chain_add(t1 - began);
        kick();
        frame_stats(t1 - t0);
        win.frames++;
        win.prep_us += t1 - t0;
        mesh_release();
        return 1;
    }
    /* a frame behind: the last frame's list made, then given to the GE once the one before is drawn */
    t1 = t0;
    if (with_kicker) {
        with_kicker = 0;
        /* the kicker's: waited for until it has given it to the GE */
        if (sceKernelPollSema(kick_given, 1) < 0) {
            win.busy++;
            sceKernelWaitSema(kick_given, 1, NULL);
        }
        t1 = now_us();
        win.wait_us += t1 - t0;
        win.on_me++;
        made_frame.cpu_us += t1 - t0;
    } else if (made_frame.pending) {
        psp_render_idle();
        if (made_frame.on_me) {
            list_from_me();
            if (me_check) {
                check_me_list();
            }
        } else {
            psp_list_job(&job);
            if (me_check) {
                check_me_list();    /* the check's own check: the same list made twice */
            }
        }
        tp = now_us();
        list_finished();
        t1 = now_us();
        psp_rstats.cpu_sec[4] += t1 - tp;
        made_frame.cpu_us += t1 - t0;
        kick();
    } else {
        psp_render_idle();
        kick();
    }
    mesh_release();
    tp = now_us();
    prepare(stretch, frame, tp);
    pool_moves();
    t2 = now_us();
    psp_rstats.cpu_sec[0] += t2 - tp;
    made_frame.cpu_us = t2 - tp;
    made_frame.pending = 1;
    made_frame.work = t0 - began + t2 - tp;
    list_to_me();
    if (made_frame.on_me && kicker_on) {
        with_kicker = 1;
        sceKernelSignalSema(kick_job, 1);
    }
    rd_frame = rd_frame == rd_frames[0] ? rd_frames[1] : rd_frames[0];
    frame_stats(t1 - t0 + t2 - tp);
    win.frames++;
    win.prep_us += t1 - t0 + t2 - tp;
    win.here += list_where == LIST_ME && !made_frame.on_me;
    return 1;
}

/* the frame's end: the wait before the next frame's input is read */
uint32_t psp_render_start_wait(void) {
    uint32_t v = psp_video_frame_us(), now, vb, need, mean, s, w;
    unsigned i;
    if (start_margin == 0 || failed || nchains < 16) {
        return 0;
    }
    need = mean = 0;
    for (i = 0; i < 16; i++) {
        need = chains[i] > need ? chains[i] : need;
        mean += chains[i];
    }
    mean = mean / 16 + start_margin / 2;
    need += start_margin;
    if (need >= 2 * v) {
        return 0;
    }
    if (list_where == LIST_HERE && start_here) {
        s = psp_video_shown_us() + v;   /* the next frame's end flips this one's picture a vblank after the last */
    } else if (with_kicker) {
        s = due + v;
        if (k_shown_frame != 0 && later(k_shown + (made_frame.frame + 1 - k_shown_frame) * v, s)) {
            s = k_shown + (made_frame.frame + 1 - k_shown_frame) * v;
        }
    } else {
        return 0;
    }
    now = now_us();
    vb = psp_video_vblank_us();
    if (later(now + mean, s)) {     /* a late frame that may still make it is begun at once, not a vblank later */
        s = vb + (now + need - vb + v - 1) / v * v;
    }
    due = s;
    w = s - need - now;
    if ((int32_t)w <= 0) {
        return 0;
    }
    w = w > v ? v : w;
    win.held_us += w;
    return w;
}

/* the window since the last call, in a line's words: the main CPU's time making each list or readying it for the
   Media Engine, its wait for the ME's, and the lists made here while the ME was asked for */
int psp_render_window(char* out, int n) {
    double f = win.frames ? win.frames : 1;
    int k;
    if (list_where != LIST_ME) {
        k = snprintf(out, (size_t)n, "list %.2f ms", win.prep_us / f / 1000.0);
    } else {
        k = snprintf(out, (size_t)n, "list prep %.2f ms, ME wait %.2f ms, %u of %u ME lists not done by the frame's "
                     "end, %u made here; ME write-ahead %u", win.prep_us / f / 1000.0, win.wait_us / f / 1000.0,
                     (unsigned)win.busy, (unsigned)win.on_me, (unsigned)win.here, (unsigned)me_ahead);
    }
    if (win.held_us != 0 && k < n) {
        k += snprintf(out + k, (size_t)(n - k), "; input read %.2f ms later", win.held_us / f / 1000.0);
    }
    if (win.pictures != 0 && k < n) {
        k += snprintf(out + k, (size_t)(n - k), "; of %u pictures %u a vblank late or more, %u shown over by the next",
                      (unsigned)win.pictures, (unsigned)win.late, (unsigned)win.over);
    }
    if (win.moved != 0 && k < n) {
        k += snprintf(out + k, (size_t)(n - k), "; the triangle pool laid out again %u times in %u frames",
                      (unsigned)win.moved, (unsigned)win.moved_frames);
    }
    if (me_time && win.pictures != 0) {
        host_log("  kicker a frame: post to its look %.2f ms, ME %.2f, back buffer %.2f, GE %.2f; the ME's job after the "
                 "list %.2f ms at most, %u times", win.k_look / f / 1000.0, win.k_me / f / 1000.0, win.k_back / f / 1000.0,
                 win.k_ge / f / 1000.0, win.n_after ? win.k_after / (double)win.n_after / 1000.0 : 0.0,
                 (unsigned)win.n_after);
    }
    memset(&win, 0, sizeof win);
    return k;
}
