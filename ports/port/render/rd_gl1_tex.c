/*
 * rd_gl1_tex.c  rd_gl1's palette texels in 16 bits, and the sprites' tiles in atlas pages
 */

#include <string.h>

#include "rd_gl1_tex.h"

#if defined(__ARM_NEON) && !defined(RD_NO_SIMD)
#include <arm_neon.h>
#define TEX_NEON
#endif

const uint32_t* rd_gl1_pal = rd_palette;

static int fits(uint32_t c, int form) {
    uint32_t a = c >> 24, r = c >> 16 & 255, g = c >> 8 & 255, b = c & 255;
    switch (form) {
    case RD_F1555:
        return (a == 0 || a == 255) && r == (r >> 3) * 255 / 31 && g == (g >> 3) * 255 / 31 && b == (b >> 3) * 255 / 31;
    case RD_F565:
        return a == 255 && r == (r >> 3) * 255 / 31 && g == (g >> 2) * 255 / 63 && b == (b >> 3) * 255 / 31;
    default:
        return a % 17 == 0 && r % 17 == 0 && g % 17 == 0 && b % 17 == 0;
    }
}

/* RGBA order, red lowest, for 1555 and 4444 (rd_gl1_tex.h) */
static uint16_t pack(uint32_t c, int form) {
    switch (form) {
    case RD_F1555:
        return (uint16_t)((c >> 31) << 15 | (c >> 3 & 31) << 10 | (c >> 11 & 31) << 5 | (c >> 19 & 31));
    case RD_F565:
        return (uint16_t)((c >> 19 & 31) << 11 | (c >> 10 & 63) << 5 | (c >> 3 & 31));
    default:
        return (uint16_t)((c >> 28) << 12 | (c >> 4 & 15) << 8 | (c >> 12 & 15) << 4 | (c >> 20 & 15));
    }
}

static int entries(const RD_TEXTURE* t) {
    return t->format == RD_PAL4 ? 16 : 256;
}

int rd_gl1_pal_form(const RD_TEXTURE* t, int palette) {
    int n = entries(t), form, i;
    for (form = 0; form < RD_FORMS; form++) {
        for (i = 0; i < n && fits(rd_gl1_pal[(palette + i) & 1023], form); i++) {
        }
        if (i == n) {
            return form;
        }
    }
    return -1;
}

static void table16(const RD_TEXTURE* t, int palette, int form, uint16_t* pal) {
    int n = entries(t), i;
    for (i = 0; i < n; i++) {
        pal[i] = pack(rd_gl1_pal[(palette + i) & 1023], form);
    }
}

static void expand(const uint8_t* s, int n, const uint16_t* pal, int pal4, uint16_t* out) {
    int i = 0;
#if defined(TEX_NEON)
    if (pal4) {
        uint8x8x2_t lo, hi;
        uint8x16x2_t k = vld2q_u8((const uint8_t*)pal);
        lo.val[0] = vget_low_u8(k.val[0]), lo.val[1] = vget_high_u8(k.val[0]);
        hi.val[0] = vget_low_u8(k.val[1]), hi.val[1] = vget_high_u8(k.val[1]);
        for (; i + 8 <= n; i += 8) {
            uint8x8_t x = vand_u8(vld1_u8(&s[i]), vdup_n_u8(15));
            uint8x8x2_t o;
            o.val[0] = vtbl2_u8(lo, x);
            o.val[1] = vtbl2_u8(hi, x);
            vst2_u8((uint8_t*)&out[i], o);
        }
    }
#endif
    for (; i < n; i++) {
        out[i] = pal[pal4 ? s[i] & 15 : s[i]];
    }
}

void rd_gl1_swap16(const uint16_t* s, int n, int form, uint16_t* out) {
    int i = 0;
    if (form == RD_F565) {
        memcpy(out, s, (size_t)n * 2);
        return;
    }
#if defined(TEX_NEON)
    if (form == RD_F1555) {
        for (; i + 8 <= n; i += 8) {
            uint16x8_t k = vld1q_u16(&s[i]);
            vst1q_u16(&out[i], vbslq_u16(vdupq_n_u16(0x83E0), k, vorrq_u16(vshrq_n_u16(k, 10), vshlq_n_u16(k, 10))));
        }
    } else {
        for (; i + 8 <= n; i += 8) {
            uint16x8_t k = vld1q_u16(&s[i]);
            vst1q_u16(&out[i], vbslq_u16(vdupq_n_u16(0xF0F0), k, vorrq_u16(vshrq_n_u16(k, 8), vshlq_n_u16(k, 8))));
        }
    }
#endif
    for (; i < n; i++) {
        uint16_t k = s[i];
        out[i] = form == RD_F1555 ? (uint16_t)((k & 0x83E0) | (k >> 10 & 31) | (k & 31) << 10)
                                  : (uint16_t)((k & 0xF0F0) | (k >> 8 & 15) | (k & 15) << 8);
    }
}

void rd_gl1_expand16(const uint8_t* texels, int n, const RD_TEXTURE* t, int palette, int form, uint16_t* out) {
    uint16_t pal[256];
    table16(t, palette, form, pal);
    expand(texels, n, pal, t->format == RD_PAL4, out);
}

/* ---- the pool: tiles by their contents ---- */

#define POOL_HASH 2048

typedef struct {
    uint64_t key;
    uint32_t used, gen;
    int next;                       /* in its bucket, -1 last */
    int full;
    uint8_t texels[RD_TILE * RD_TILE];  /* a byte each, as rd.h keeps PAL4 */
    uint32_t pal[16];
} PSLOT;

static PSLOT pool[RD_POOL];
static int pool_head[POOL_HASH];
static int pool_hand, pool_ready;

void rd_gl1_pool_forget(void) {
    int i;
    for (i = 0; i < RD_POOL; i++) {
        pool[i].full = 0;
        pool[i].used = 0;
        pool[i].gen++;
    }
    for (i = 0; i < POOL_HASH; i++) {
        pool_head[i] = -1;
    }
    pool_hand = 0;
    pool_ready = 1;
}

int rd_gl1_pool_texture(const RD_TEXTURE* t) {
    return t->format == RD_PAL4 && t->width == RD_TILE && t->height == RD_TILE && t->levels == 0 && t->texels != NULL;
}

static uint64_t contents_key(const uint8_t* texels, const uint32_t* pal) {
    uint64_t h = 0x9E3779B97F4A7C15ull;
    uint32_t w;
    int i;
    for (i = 0; i < RD_TILE * RD_TILE; i += 4) {
        memcpy(&w, &texels[i], 4);
        h = (h ^ w) * 0xFF51AFD7ED558CCDull;
    }
    for (i = 0; i < 16; i++) {
        h = (h ^ pal[i]) * 0xFF51AFD7ED558CCDull;
    }
    return h ^ h >> 29;
}

int rd_gl1_pool(const RD_TEXTURE* t, int palette, uint32_t frame, int* slot) {
    const uint8_t* texels = t->texels;
    uint32_t pal[16];
    uint64_t key;
    int i, k, b;
    PSLOT* e;
    if (!pool_ready) {
        rd_gl1_pool_forget();
    }
    for (i = 0; i < 16; i++) {
        pal[i] = rd_gl1_pal[(palette + i) & 1023];
    }
    key = contents_key(texels, pal);
    b = (int)(key & (POOL_HASH - 1));
    for (i = pool_head[b]; i >= 0; i = pool[i].next) {
        e = &pool[i];
        if (e->key == key && memcmp(e->texels, texels, sizeof e->texels) == 0 && memcmp(e->pal, pal, sizeof pal) == 0) {
            e->used = frame;
            *slot = i;
            return 0;
        }
    }
    /* the next slot not drawn this frame, taken out of its bucket */
    for (k = 0; k < RD_POOL; k++) {
        e = &pool[pool_hand];
        pool_hand = pool_hand + 1 < RD_POOL ? pool_hand + 1 : 0;
        if (!e->full || e->used != frame) {
            break;
        }
    }
    if (k == RD_POOL) {
        return -1;
    }
    i = (int)(e - pool);
    if (e->full) {
        int* p = &pool_head[e->key & (POOL_HASH - 1)];
        while (*p != i) {
            p = &pool[*p].next;
        }
        *p = e->next;
    }
    e->key = key;
    e->full = 1;
    e->used = frame;
    e->gen++;
    memcpy(e->texels, texels, sizeof e->texels);
    memcpy(e->pal, pal, sizeof pal);
    e->next = pool_head[b];
    pool_head[b] = i;
    *slot = i;
    return 1;
}

uint32_t rd_gl1_pool_gen(int slot) {
    return pool[slot].gen;
}

void rd_gl1_pool_use(int slot, uint32_t frame) {
    pool[slot].used = frame;
}

/* x * 255 / 31 */
static const uint8_t five[32] = { 0,   8,   16,  24,  32,  41,  49,  57,  65,  74,  82,  90,  98,  106, 115, 123,
                                  131, 139, 148, 156, 164, 172, 180, 189, 197, 205, 213, 222, 230, 238, 246, 255 };

void rd_gl1_rgba16(const uint16_t* s, int n, int format, uint32_t* out) {
    int i = 0;
#if defined(TEX_NEON)
    uint8x8x4_t t5 = { { vld1_u8(five), vld1_u8(five + 8), vld1_u8(five + 16), vld1_u8(five + 24) } };
    uint16x8_t m5 = vdupq_n_u16(31);
    for (; i + 8 <= n; i += 8) {
        uint16x8_t k = vld1q_u16(&s[i]);
        uint8x8x4_t c;
        if (format == RD_ARGB1555) {
            c.val[0] = vtbl4_u8(t5, vmovn_u16(vandq_u16(vshrq_n_u16(k, 10), m5)));
            c.val[1] = vtbl4_u8(t5, vmovn_u16(vandq_u16(vshrq_n_u16(k, 5), m5)));
            c.val[2] = vtbl4_u8(t5, vmovn_u16(vandq_u16(k, m5)));
            c.val[3] = vmovn_u16(vreinterpretq_u16_s16(vshrq_n_s16(vreinterpretq_s16_u16(k), 15)));
        } else if (format == RD_RGB565) {
            /* x * 255 / 63 = 4x + x / 21: each compare true is 255, -1 */
            uint8x8_t g = vmovn_u16(vandq_u16(vshrq_n_u16(k, 5), vdupq_n_u16(63)));
            c.val[0] = vtbl4_u8(t5, vmovn_u16(vshrq_n_u16(k, 11)));
            c.val[1] = vsub_u8(vsub_u8(vsub_u8(vshl_n_u8(g, 2), vcgt_u8(g, vdup_n_u8(20))), vcgt_u8(g, vdup_n_u8(41))),
                               vcgt_u8(g, vdup_n_u8(62)));
            c.val[2] = vtbl4_u8(t5, vmovn_u16(vandq_u16(k, m5)));
            c.val[3] = vdup_n_u8(255);
        } else {
            uint8x8_t hi = vshrn_n_u16(k, 8), lo = vmovn_u16(k);
            uint8x8_t m4 = vdup_n_u8(15);
            uint8x8_t a = vshr_n_u8(hi, 4), r = vand_u8(hi, m4), g = vshr_n_u8(lo, 4), b = vand_u8(lo, m4);
            c.val[0] = vsli_n_u8(r, r, 4);
            c.val[1] = vsli_n_u8(g, g, 4);
            c.val[2] = vsli_n_u8(b, b, 4);
            c.val[3] = vsli_n_u8(a, a, 4);
        }
        vst4_u8((uint8_t*)&out[i], c);
    }
#endif
    for (; i < n; i++) {
        uint32_t p = s[i];
        if (format == RD_ARGB1555) {
            out[i] = (p & 0x8000 ? 0xFF000000u : 0) | (uint32_t)five[p & 31] << 16 | (uint32_t)five[p >> 5 & 31] << 8 |
                     five[p >> 10 & 31];
        } else if (format == RD_RGB565) {
            out[i] = 0xFF000000u | (uint32_t)five[p & 31] << 16 | ((p >> 5 & 63) * 255 / 63) << 8 | five[p >> 11];
        } else {
            out[i] = (p >> 12) * 0x11000000u | (p & 15) * 0x110000u | (p >> 4 & 15) * 0x1100u | (p >> 8 & 15) * 0x11u;
        }
    }
}

void rd_gl1_expand32(const uint8_t* texels, int n, const RD_TEXTURE* t, int palette, uint32_t* out) {
    uint32_t pal[256];
    int k = entries(t), i = 0;
    for (i = 0; i < k; i++) {
        uint32_t c = rd_gl1_pal[(palette + i) & 1023];
        pal[i] = (c & 0xFF00FF00u) | (c >> 16 & 255) | (c & 255) << 16;
    }
    i = 0;
#if defined(TEX_NEON)
    if (k == 16) {
        uint8x16x4_t q = vld4q_u8((const uint8_t*)pal);     /* each byte of the entries, a register each */
        uint8x8x2_t b[4];
        int c;
        for (c = 0; c < 4; c++) {
            b[c].val[0] = vget_low_u8(q.val[c]), b[c].val[1] = vget_high_u8(q.val[c]);
        }
        for (; i + 8 <= n; i += 8) {
            uint8x8_t x = vand_u8(vld1_u8(&texels[i]), vdup_n_u8(15));
            uint8x8x4_t o;
            o.val[0] = vtbl2_u8(b[0], x), o.val[1] = vtbl2_u8(b[1], x);
            o.val[2] = vtbl2_u8(b[2], x), o.val[3] = vtbl2_u8(b[3], x);
            vst4_u8((uint8_t*)&out[i], o);
        }
    }
#endif
    for (; i < n; i++) {
        out[i] = pal[k == 16 ? texels[i] & 15 : texels[i]];
    }
}

int rd_gl1_tile_state(const RD_STATE* s) {
    const RD_TEXTURE* t = s->tex;
    return t != NULL && t->format == RD_PAL4 && t->width == RD_TILE && t->height == RD_TILE && t->levels == 0 &&
           t->texels != NULL && !s->bilinear && (s->uv_mode & 15) == 0;
}

/* ---- the pages' slots ---- */

#define PAGE_SLOTS (RD_SLOTS_ROW * RD_SLOTS_ROW)
#define HASH 4096

typedef struct {
    const RD_TEXTURE* t;            /* NULL: free */
    int palette;
    uint32_t serial, used;
    int filled;
    uint16_t pal[16];               /* the entries its texels were sent with */
} SLOT;

static SLOT slots[RD_FORMS][PAGE_SLOTS];
static int16_t hash[RD_FORMS][HASH];   /* slot + 1, 0 empty */
static int hand[RD_FORMS];

static unsigned hash_of(const RD_TEXTURE* t, int palette) {
    return (((unsigned)((uintptr_t)t >> 4) + (unsigned)palette * 40503u) * 2654435761u) >> 20;
}

static void rehash(int form) {
    int i;
    memset(hash[form], 0, sizeof hash[form]);
    for (i = 0; i < PAGE_SLOTS; i++) {
        if (slots[form][i].t != NULL) {
            unsigned h = hash_of(slots[form][i].t, slots[form][i].palette);
            while (hash[form][h] != 0) {
                h = (h + 1) & (HASH - 1);
            }
            hash[form][h] = (int16_t)(i + 1);
        }
    }
}

void rd_gl1_tiles_forget(void) {
    memset(slots, 0, sizeof slots);
    memset(hash, 0, sizeof hash);
    memset(hand, 0, sizeof hand);
}

/* the tile's texels with a texel of the other side's round them, as a repeat reads past an edge */
static void bordered(const uint8_t* s, const uint16_t* pal, uint16_t* out) {
    uint16_t row[RD_TILE];
    int y;
    for (y = 0; y < RD_SLOT; y++) {
        uint16_t* o = &out[y * RD_SLOT];
        expand(&s[((y + RD_TILE - 1) & (RD_TILE - 1)) * RD_TILE], RD_TILE, pal, 1, row);
        o[0] = row[RD_TILE - 1];
        memcpy(&o[1], row, sizeof row);
        o[RD_SLOT - 1] = row[0];
    }
}

int rd_gl1_tile(const RD_TEXTURE* t, int palette, int form, uint32_t frame, int* at, uint16_t* out) {
    uint16_t pal[16];
    unsigned h = hash_of(t, palette);
    SLOT* e = NULL;
    int i, k;
    for (; hash[form][h] != 0; h = (h + 1) & (HASH - 1)) {
        SLOT* c = &slots[form][hash[form][h] - 1];
        if (c->t == t && c->palette == palette) {
            e = c;
            break;
        }
    }
    if (e == NULL) {
        /* a free slot, or the next one not used this frame */
        for (k = 0; k < PAGE_SLOTS; k++) {
            SLOT* c = &slots[form][hand[form]];
            hand[form] = hand[form] + 1 < PAGE_SLOTS ? hand[form] + 1 : 0;
            if (c->t == NULL || c->used != frame) {
                e = c;
                break;
            }
        }
        if (e == NULL) {
            return -1;
        }
        if (e->t != NULL) {
            e->t = NULL;
            rehash(form);
        }
        e->t = t, e->palette = palette, e->filled = 0;
        for (h = hash_of(t, palette); hash[form][h] != 0; h = (h + 1) & (HASH - 1)) {
        }
        hash[form][h] = (int16_t)(e - slots[form] + 1);
    }
    i = (int)(e - slots[form]);
    *at = (i % RD_SLOTS_ROW) * RD_SLOT + 1 + ((i / RD_SLOTS_ROW) * RD_SLOT + 1) * RD_ATLAS;
    e->used = frame;
    table16(t, palette, form, pal);
    if (e->filled && e->serial == t->serial && memcmp(e->pal, pal, sizeof pal) == 0) {
        return 0;
    }
    e->filled = 1;
    e->serial = t->serial;
    memcpy(e->pal, pal, sizeof pal);
    bordered(t->texels, pal, out);
    return 1;
}
