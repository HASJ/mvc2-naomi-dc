/*
 * rd_soft.c  The frame drawn in software, as the PowerVR2 draws it. The translucent list is sorted
 * per pixel (auto-sort mode): fragments blended far to near, at one depth in the order sent.
 */

/* SSE, not the 387, whose wider registers round wherever the compiler spills them */
#if defined(__GNUC__) && defined(__i386__)
#pragma GCC target("sse2", "fpmath=sse")
#endif
/* and no fused multiply-add */
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rd_sys.h"
/* SSE2 and NEON give the same pixels as the plain C (RD_PLAIN) */
#if defined(__SSE2__) && !defined(RD_PLAIN)
#define RD_SSE2 1
#include <emmintrin.h>
#elif (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !defined(RD_PLAIN)
#include <arm_neon.h>
#define RD_NEON 1
#endif

#include "rd.h"
#include "rd_list.h"

#if defined(__GNUC__)
#define PIXEL_STEP static inline __attribute__((always_inline))
#else
#define PIXEL_STEP static inline
#endif

/* rd_q: half draws at half size, each pixel shown as four; fast takes the nearest texel; lite is near, in integers */
static int scale = 1, draw_w = RD_WIDTH, draw_h = RD_HEIGHT, fast, nofog, nosort, lite;

#define LSEG 16
#define LPLANES 12
static int64_t lrecip[LSEG + 1];    /* 2^32 / n */
static float lrcp[LSEG + 1];        /* 1 / n */

/* made when first drawn: a front end with another renderer never needs them */
static float* zbuf;
static uint32_t* cbuf;      /* ARGB, the alpha kept for the blend factors */

/* the translucent fragments, a list a pixel */
typedef struct {
    float z;
    uint32_t color;
    int order;          /* in the list */
    int next;
} FRAG;

static int* fhead;

typedef struct WORKER WORKER;
typedef struct lbuf LBUF;

typedef struct {
    int a, r, g, b;
} COLOR;

static COLOR unpack(uint32_t c) {
    COLOR k;
    k.a = (int)(c >> 24);
    k.r = (int)(c >> 16 & 255);
    k.g = (int)(c >> 8 & 255);
    k.b = (int)(c & 255);
    return k;
}

static uint32_t pack(COLOR c) {
    return (uint32_t)c.a << 24 | (uint32_t)c.r << 16 | (uint32_t)c.g << 8 | (uint32_t)c.b;
}

/* clamped, mirrored or repeated, as the TSP says */
static int wrap(int x, int n, int clamp, int flip) {
    if (clamp) {
        return x < 0 ? 0 : x >= n ? n - 1 : x;
    }
    if (flip) {
        x &= 2 * n - 1;
        return x < n ? x : 2 * n - 1 - x;
    }
    return x & (n - 1);
}

/* (int)f, INT_MIN out of range as on x86, so every CPU draws the same pixels */
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
#define to_int(f) ((int)(f))
#else
static int to_int(float f) {
    return f >= -2147483648.0f && f < 2147483648.0f ? (int)f : INT_MIN;
}
#endif

/* (int)floorf(f) without the call */
static int ifloor(float f) {
    int i = to_int(f);
    return f < (float)i && i != INT_MIN ? i - 1 : i;
}

/* a 5- and a 6-bit channel to 8, rounded down */
static uint8_t c5[32], c6[64];
/* and as floats */
static float f4[16], f5[32], f6[64];

/* 0 when there is no memory for them */
static int make_tables(void) {
    int i;
    zbuf = malloc(RD_WIDTH * RD_HEIGHT * sizeof *zbuf);
    cbuf = malloc(RD_WIDTH * RD_HEIGHT * sizeof *cbuf);
    fhead = malloc(RD_WIDTH * RD_HEIGHT * sizeof *fhead);
    if (zbuf == NULL || cbuf == NULL || fhead == NULL) {
        free(zbuf), free(cbuf), free(fhead);
        zbuf = NULL;
        return 0;
    }
    for (i = 0; i < 32; i++) {
        c5[i] = (uint8_t)(i * 255 / 31);
    }
    for (i = 0; i < 64; i++) {
        c6[i] = (uint8_t)(i * 255 / 63);
        f6[i] = (float)c6[i];
    }
    for (i = 0; i < 32; i++) {
        f5[i] = (float)c5[i];
    }
    for (i = 0; i < 16; i++) {
        f4[i] = (float)(i * 17);
    }
    for (i = 1; i <= LSEG; i++) {
        lrecip[i] = ((1LL << 32) + i / 2) / i;
        lrcp[i] = 1.0f / (float)i;
    }
    return 1;
}

PIXEL_STEP uint32_t texel16(int format, unsigned p) {
    switch (format) {
    case RD_ARGB1555:
        return (p & 0x8000 ? 0xFF000000u : 0) | (uint32_t)c5[p >> 10 & 31] << 16 | (uint32_t)c5[p >> 5 & 31] << 8 | c5[p & 31];
    case RD_RGB565:
        return 0xFF000000u | (uint32_t)c5[p >> 11] << 16 | (uint32_t)c6[p >> 5 & 63] << 8 | c5[p & 31];
    default:
        return (p >> 12) * 0x11000000u | (p >> 8 & 15) * 0x110000u | (p >> 4 & 15) * 0x1100u | (p & 15) * 0x11u;
    }
}

PIXEL_STEP uint32_t fetch(const RD_STATE* s, const void* d, int at) {
    if (s->tex->format == RD_PAL4 || s->tex->format == RD_PAL8) {
        return rd_palette[(s->palette + ((const uint8_t*)d)[at]) & 1023];
    }
    return texel16(s->tex->format, ((const uint16_t*)d)[at]);
}

static uint32_t texel(const RD_STATE* s, int k, int x, int y) {
    const RD_TEXTURE* t = s->tex;
    int w = t->width >> k, h = t->height >> k;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    return fetch(s, k == 0 ? t->texels : t->mip[k],
                 wrap(y, h, s->uv_mode & 1, s->uv_mode & 4) * w + wrap(x, w, s->uv_mode & 2, s->uv_mode & 8));
}

/* the four texels last blended: the next pixel often wants the same */
typedef struct {
    const RD_STATE* s;
    int k, x, y;
    _Alignas(16) float c[4][4];     /* (x, y), (x + 1, y), (x, y + 1), (x + 1, y + 1); a r g b */
} TEXCACHE;

/* the palette as floats, made each frame one is read bilinear */
static float fpal[1024][4];

/* texel at of d as a, r, g, b */
PIXEL_STEP void texel_floats(float* c, const RD_STATE* s, const int tex, const void* d, int at) {
    unsigned p;
    if (tex == 2) {
        memcpy(c, fpal[(s->palette + ((const uint8_t*)d)[at]) & 1023], 4 * sizeof *c);
        return;
    }
    p = ((const uint16_t*)d)[at];
    switch (s->tex->format) {
    case RD_ARGB1555:
        c[0] = p & 0x8000 ? 255.0f : 0.0f, c[1] = f5[p >> 10 & 31], c[2] = f5[p >> 5 & 31], c[3] = f5[p & 31];
        break;
    case RD_RGB565:
        c[0] = 255.0f, c[1] = f5[p >> 11], c[2] = f6[p >> 5 & 63], c[3] = f5[p & 31];
        break;
    default:
        c[0] = f4[p >> 12], c[1] = f4[p >> 8 & 15], c[2] = f4[p >> 4 & 15], c[3] = f4[p & 15];
        break;
    }
}

/* the texel at (u, v), ARGB */
PIXEL_STEP uint32_t point_texel(const RD_STATE* s, const int tex, const void* d, int w, int h, float u, float v) {
    float fu = u * w, fv = v * h;
    int at = wrap(ifloor(fv), h, s->uv_mode & 1, s->uv_mode & 4) * w + wrap(ifloor(fu), w, s->uv_mode & 2, s->uv_mode & 8);
    if (tex == 2) {
        return rd_palette[(s->palette + ((const uint8_t*)d)[at]) & 1023];
    }
    return texel16(s->tex->format, ((const uint16_t*)d)[at]);
}

PIXEL_STEP COLOR sample_point(const RD_STATE* s, const int tex, const void* d, int w, int h, float u, float v) {
    return unpack(point_texel(s, tex, d, w, h, u, v));
}

/* the four texels around (u, v) into tc, and the weights of the right and lower ones */
PIXEL_STEP void bilinear_texels(const RD_STATE* s, const int tex, int k, const void* d, int w, int h, float u, float v,
                                TEXCACHE* tc, float* ax_, float* ay_) {
    int cu = s->uv_mode & 2, cv = s->uv_mode & 1, fu_ = s->uv_mode & 8, fv_ = s->uv_mode & 4;
    float fu = u * w, fv = v * h;
    int x = ifloor(fu - 0.5f), y = ifloor(fv - 0.5f);
    *ax_ = fu - 0.5f - x, *ay_ = fv - 0.5f - y;
    if (tc->s != s || tc->k != k || tc->x != x || tc->y != y) {
        int x0 = wrap(x, w, cu, fu_), x1 = wrap(x + 1, w, cu, fu_);
        int y0 = wrap(y, h, cv, fv_) * w, y1 = wrap(y + 1, h, cv, fv_) * w;
        if (tc->s == s && tc->k == k && tc->x + 1 == x && tc->y == y) {
            /* one texel on: the right two become the left two */
            memcpy(tc->c[0], tc->c[1], sizeof tc->c[0]);
            memcpy(tc->c[2], tc->c[3], sizeof tc->c[2]);
            texel_floats(tc->c[1], s, tex, d, y0 + x1);
            texel_floats(tc->c[3], s, tex, d, y1 + x1);
        } else {
            texel_floats(tc->c[0], s, tex, d, y0 + x0);
            texel_floats(tc->c[1], s, tex, d, y0 + x1);
            texel_floats(tc->c[2], s, tex, d, y1 + x0);
            texel_floats(tc->c[3], s, tex, d, y1 + x1);
        }
        tc->s = s, tc->k = k, tc->x = x, tc->y = y;
    }
}

#if defined(RD_NEON)
/* the texels mixed: a r g b, by to_int */
PIXEL_STEP int32x4_t bilinear_mix(const TEXCACHE* tc, float ax, float ay) {
    float32x4_t bx = vdupq_n_f32(1 - ax), fx = vdupq_n_f32(ax), by = vdupq_n_f32(1 - ay), fy = vdupq_n_f32(ay);
    float32x4_t top = vaddq_f32(vmulq_f32(vld1q_f32(tc->c[0]), bx), vmulq_f32(vld1q_f32(tc->c[1]), fx));
    float32x4_t bottom = vaddq_f32(vmulq_f32(vld1q_f32(tc->c[2]), bx), vmulq_f32(vld1q_f32(tc->c[3]), fx));
    float32x4_t m = vaddq_f32(vmulq_f32(top, by), vmulq_f32(bottom, fy));
    return vbslq_s32(vcaltq_f32(m, vdupq_n_f32(2147483648.0f)), vcvtq_s32_f32(m), vdupq_n_s32(INT_MIN));
}
#elif defined(RD_SSE2)
/* m's bits from a, the others from b */
PIXEL_STEP __m128i sel128(__m128i m, __m128i a, __m128i b) {
    return _mm_or_si128(_mm_and_si128(m, a), _mm_andnot_si128(m, b));
}

/* the texels mixed: a r g b, by to_int */
PIXEL_STEP __m128i bilinear_mix(const TEXCACHE* tc, float ax, float ay) {
    __m128 bx = _mm_set1_ps(1 - ax), fx = _mm_set1_ps(ax), by = _mm_set1_ps(1 - ay), fy = _mm_set1_ps(ay);
    __m128 top = _mm_add_ps(_mm_mul_ps(_mm_load_ps(tc->c[0]), bx), _mm_mul_ps(_mm_load_ps(tc->c[1]), fx));
    __m128 bottom = _mm_add_ps(_mm_mul_ps(_mm_load_ps(tc->c[2]), bx), _mm_mul_ps(_mm_load_ps(tc->c[3]), fx));
    return _mm_cvttps_epi32(_mm_add_ps(_mm_mul_ps(top, by), _mm_mul_ps(bottom, fy)));
}
#endif

PIXEL_STEP COLOR sample_bilinear(const RD_STATE* s, const int tex, int k, const void* d, int w, int h, float u, float v,
                                 TEXCACHE* tc) {
    float ax, ay;
    COLOR c;
    bilinear_texels(s, tex, k, d, w, h, u, v, tc, &ax, &ay);
#if defined(RD_SSE2)
    {
        __m128i k = bilinear_mix(tc, ax, ay);
        c.a = _mm_cvtsi128_si32(k);
        c.r = _mm_cvtsi128_si32(_mm_srli_si128(k, 4));
        c.g = _mm_cvtsi128_si32(_mm_srli_si128(k, 8));
        c.b = _mm_cvtsi128_si32(_mm_srli_si128(k, 12));
    }
#elif defined(RD_NEON)
    {
        int32x4_t k = bilinear_mix(tc, ax, ay);
        c.a = vgetq_lane_s32(k, 0);
        c.r = vgetq_lane_s32(k, 1);
        c.g = vgetq_lane_s32(k, 2);
        c.b = vgetq_lane_s32(k, 3);
    }
#else
#define MIX(j) to_int((tc->c[0][j] * (1 - ax) + tc->c[1][j] * ax) * (1 - ay) + (tc->c[2][j] * (1 - ax) + tc->c[3][j] * ax) * ay)
    c.a = MIX(0);
    c.r = MIX(1);
    c.g = MIX(2);
    c.b = MIX(3);
#undef MIX
#endif
    return c;
}

/* the mip level nearest the larger texel step, moved by the D adjust */
static int mip_level(const RD_STATE* s, float dudx, float dvdx, float dudy, float dvdy) {
    static const float bias[16] = { 0, -4, -2, -1 };
    float w = (float)s->tex->width, h = (float)s->tex->height;
    float lx = dudx * dudx * w * w + dvdx * dvdx * h * h, ly = dudy * dudy * w * w + dvdy * dvdy * h * h;
    float l = lx > ly ? lx : ly, lod;
    uint32_t bits;
    int k;
    memcpy(&bits, &l, 4);
    if ((bits >> 23) - 1 < 254 && (bits & 0x7FFFFF) > 0x800 && (bits & 0x7FFFFF) < 0x7FF800) {
        /* log2 l well inside (e, e + 1): no log2f needed */
        int e = (int)(bits >> 23) - 127;
        k = ((e + 1) >> 1) + (int)bias[s->mip_d & 15];
    } else {
        lod = 0.5f * log2f(l) + bias[s->mip_d & 15];
        k = to_int(floorf(lod + 0.5f));
    }
    return k < 0 ? 0 : k > s->tex->levels ? s->tex->levels : k;
}

static int clamp255(int x) {
    return x < 0 ? 0 : x > 255 ? 255 : x;
}

/* the fog table at 1/w z: 16 entries a doubling of density * z, the two either side blended */
static float fog_value(float z) {
    float q = rd_fog.density * z, m, pos;
    uint32_t bits;
    int e, i;
    if (!(q >= 1.0f)) {
        q = 1.0f;
    } else if (q > 255.9999f) {
        q = 255.9999f;
    }
    memcpy(&bits, &q, 4);       /* q = m 2^e, m in [0.5, 1) */
    e = (int)(bits >> 23) - 126;
    bits = (bits & 0x007FFFFFu) | 0x3F000000u;
    memcpy(&m, &bits, 4);
    pos = (m * 2.0f - 1.0f) * 16.0f + (float)((e - 1) * 16);
    i = (int)pos;
    if (i >= 127) {
        return rd_fog.table[127];
    }
    return rd_fog.table[i] + (rd_fog.table[i + 1] - rd_fog.table[i]) * (pos - (float)i);
}

static int fog_on;      /* the table is not all 0 */

/* fog_value f taken */
PIXEL_STEP void fog(const RD_STATE* s, COLOR* c, float f) {
    COLOR k = unpack(rd_fog.color);
    if (s->fog == RD_FOG_TABLE2) {
        k.a = (int)(f * 255.0f + 0.5f);
        *c = k;
        return;
    }
    if (f == 0.0f) {
        return;
    }
    if (f == 1.0f) {
        c->r = k.r, c->g = k.g, c->b = k.b;
        return;
    }
    c->r += (int)((k.r - c->r) * f + (k.r > c->r ? 0.5f : -0.5f));
    c->g += (int)((k.g - c->g) * f + (k.g > c->g ? 0.5f : -0.5f));
    c->b += (int)((k.b - c->b) * f + (k.b > c->b ? 0.5f : -0.5f));
}

/* blend factor f, 0 to 255, for a r g b */
static void factors(int f, const int* other, int sa, int da, int* k) {
    int i;
    switch (f) {
    case RD_ZERO: k[0] = k[1] = k[2] = k[3] = 0; break;
    case RD_ONE: k[0] = k[1] = k[2] = k[3] = 255; break;
    case RD_OTHER: for (i = 0; i < 4; i++) k[i] = other[i]; break;
    case RD_INV_OTHER: for (i = 0; i < 4; i++) k[i] = 255 - other[i]; break;
    case RD_SRC_ALPHA: k[0] = k[1] = k[2] = k[3] = sa; break;
    case RD_INV_SRC_ALPHA: k[0] = k[1] = k[2] = k[3] = 255 - sa; break;
    case RD_DST_ALPHA: k[0] = k[1] = k[2] = k[3] = da; break;
    default: k[0] = k[1] = k[2] = k[3] = 255 - da; break;
    }
}

PIXEL_STEP uint32_t blend(const RD_STATE* s, COLOR c, uint32_t dst) {
    int sc[4], dc[4], fs[4], fd[4], i;
    uint32_t out = 0;
    if (s->src == RD_SRC_ALPHA && s->dst == RD_INV_SRC_ALPHA) {
        int a = c.a, na = 255 - a;
        return (uint32_t)((a * a + (int)(dst >> 24) * na + 127) / 255) << 24 |
               (uint32_t)((c.r * a + (int)(dst >> 16 & 255) * na + 127) / 255) << 16 |
               (uint32_t)((c.g * a + (int)(dst >> 8 & 255) * na + 127) / 255) << 8 |
               (uint32_t)((c.b * a + (int)(dst & 255) * na + 127) / 255);
    }
    if (s->src == RD_SRC_ALPHA && s->dst == RD_ONE) {
        int a = c.a;
        return (uint32_t)clamp255((int)(dst >> 24) + (a * a + 127) / 255) << 24 |
               (uint32_t)clamp255((int)(dst >> 16 & 255) + (c.r * a + 127) / 255) << 16 |
               (uint32_t)clamp255((int)(dst >> 8 & 255) + (c.g * a + 127) / 255) << 8 |
               (uint32_t)clamp255((int)(dst & 255) + (c.b * a + 127) / 255);
    }
    sc[0] = c.a, sc[1] = c.r, sc[2] = c.g, sc[3] = c.b;
    dc[0] = (int)(dst >> 24), dc[1] = (int)(dst >> 16 & 255), dc[2] = (int)(dst >> 8 & 255), dc[3] = (int)(dst & 255);
    factors(s->src, dc, c.a, dc[0], fs);
    factors(s->dst, sc, c.a, dc[0], fd);
    for (i = 0; i < 4; i++) {
        out = out << 8 | (uint32_t)clamp255((sc[i] * fs[i] + dc[i] * fd[i] + 127) / 255);
    }
    return out;
}

/* doubles: a vertex cut at the near plane can be far off the screen */
static double edge(const RD_VTX* a, const RD_VTX* b, double x, double y) {
    return ((double)b->x - a->x) * (y - a->y) - ((double)b->y - a->y) * (x - a->x);
}

/* the top-left rule: two triangles that share an edge draw each pixel on it once */
static int edge_owns(const RD_VTX* a, const RD_VTX* b, double sign) {
    double dx = -((double)b->y - a->y) * sign, dy = ((double)b->x - a->x) * sign;
    return dx > 0 || (dx == 0 && dy > 0);
}

/* rows in bands of 8, dealt out to the workers in turn: the same frame for any count */
#define BAND 8
#define MAX_WORKERS 32

#define MAX_BANDS ((RD_HEIGHT + BAND - 1) / BAND)

struct WORKER {
    _Alignas(64) int index;     /* a cache line each */
    TEXCACHE tex;
    FRAG* frags;
    int nfrags, maxfrags;
    RD_SEM* go;
    int band, band_y;           /* the band being drawn, its first row */
    int* win;                   /* the band's pixels' opaque triangles, -1 for none */
    int* won;                   /* by triangle, the last band it took a pixel of */
    int maxwon;
    int* bin;                   /* the worker's bands' triangles, in order, band b's from bin_at[b] to bin_at[b + 1] */
    int16_t (*spans)[2];        /* the opaque ones' spans in the band, BAND a triangle, by its place in the band's */
    float lz[RD_WIDTH];         /* a row's depths, and which pass */
    uint8_t lok[RD_WIDTH];
    LBUF* lb;                   /* lite's passes */
    int maxbin;
    int bin_at[MAX_BANDS + 1];
};

static WORKER workers[MAX_WORKERS];
static int nworkers;
static RD_SEM* done;
static int phase, ntotal;
static uint32_t* out_frame;
static uint16_t* out565;    /* or RGB565 */
static int out_pitch;
static int field = -1;      /* 0 or 1: only the even or odd rows */

/* whether row y is drawn: half draws all its rows, each two out */
static int row_on(int y) {
    return field < 0 || scale != 1 || (y & 1) == field;
}
/* a pixel out: ARGB, or with RD_ABGR (the PSP's GE) ABGR */
#ifdef RD_ABGR
#define OUT_PIXEL(c) (0xFF000000u | ((c) & 0xFF00u) | ((c) >> 16 & 0xFFu) | ((c) & 0xFFu) << 16)
#else
#define OUT_PIXEL(c) (0xFF000000u | (c))
#endif

/* the worker's first band from band b on */
static int first_band(const WORKER* w, int b) {
    return b + ((w->index - b) % nworkers + nworkers) % nworkers;
}

/* a fragment at pixel at, its colour for the caller; NULL when there is no memory for it */
PIXEL_STEP FRAG* new_frag(WORKER* w, int at, float z, int order) {
    FRAG* f;
    if (w->nfrags == w->maxfrags) {
        int n = w->maxfrags ? w->maxfrags * 2 : 1 << 16;
        FRAG* p = realloc(w->frags, (size_t)n * sizeof *p);
        if (p == NULL) {
            return NULL;
        }
        w->frags = p;
        w->maxfrags = n;
    }
    f = &w->frags[w->nfrags];
    f->z = z;
    f->order = order;
    f->next = fhead[at];
    fhead[at] = w->nfrags++;
    return f;
}

PIXEL_STEP void add_frag(WORKER* w, int at, float z, uint32_t color, int order) {
    FRAG* f = new_frag(w, at, z, order);
    if (f != NULL) {
        f->color = color;
    }
}

#if defined(RD_NEON)
/* x / 255 rounded down, for x from 0 to 65534 */
PIXEL_STEP int32x4_t div255q(int32x4_t x) {
    return vshrq_n_s32(vaddq_s32(vaddq_s32(x, vdupq_n_s32(1)), vshrq_n_s32(x, 8)), 8);
}
#elif defined(RD_SSE2)
PIXEL_STEP __m128i div255q(__m128i x) {
    return _mm_srai_epi32(_mm_add_epi32(_mm_add_epi32(x, _mm_set1_epi32(1)), _mm_srai_epi32(x, 8)), 8);
}

/* the lowest and the highest of 4 lanes' bits */
static const int8_t lane_lo[16] = { 0, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0 };
static const int8_t lane_hi[16] = { 0, 0, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3 };
#endif

/* how raster takes a triangle's pixels: drawn; opaque, only its depth, noting the pixels it takes; opaque, only those
   it took, shaded */
enum { DRAW, DEPTH, SHADE_WON };

/* depth_pass by mode, a bit for each place of z against the old: below, equal, above, unordered */
static const uint8_t depth_mask[8] = { 0, 1, 2, 3, 4, 13, 6, 15 };

#define PLANES 11

/* a triangle set up to draw */
typedef struct {
    const RD_STATE* s;
    const RD_TRI* t;
    RD_VTX v[3];            /* on the drawn frame */
    int list, order;
    int x0, x1, y0, y1;     /* y1 <= y0: not drawn */
    int own[3], depth, zwrite;
    /* edge i, from (ox, oy) on by (ex, ey): b to c, c to a, a to b */
    double ox[3], oy[3], ex[3], ey[3];
    double rd[3];           /* 1 / (ey sign), with near */
    int near;               /* no corner more than 65536 pixels out */
    int flat;               /* one colour all over */
    int level;              /* -1: chosen pixel by pixel */
    double area, sign;
    double gx[3], gy[3];    /* the barycentrics' steps */
    float zx, zy;
    float uz_x, uz_y, vz_x, vz_y;
    float col[3][8];        /* the colours over w */
    int kind;               /* span_kind's */
    int dmask, fogged;
    int clear0;             /* translucent, a pixel of alpha 0 blended leaving what is there */
    COLOR fbase, foffs;     /* flat: its colours */
    COLOR fogc;             /* the fog's colour */
    /* at + dx x + dy y: 1/w, u/w, v/w and the eight colour channels over w */
    double at[PLANES], dx[PLANES], dy[PLANES];
    /* lite: edge i from (lx, ly) by lk a row, lside 1 on the left, -1 the right, 0 level */
    double lx[3], ly[3], lk[3];
    int lside[3];
    int lys[3], lye[3];     /* its rows */
    int lox, loy;           /* the planes' origin */
    float lza, lzx, lzy;    /* 1/w */
    /* u and v in texels, then the colours, by 65536: (lpa + lpx x + lpy y) / w + lpb */
    float lpa[LPLANES], lpx[LPLANES], lpy[LPLANES], lpb[LPLANES];
} SETUP;

static SETUP* setups;
static const RD_STATE** tstate;     /* by translucent triangle */
static int maxsetups;

/* fmin and fmax without the calls (no NaNs here) */
static double dmin(double a, double b) {
    return a < b ? a : b;
}

static double dmax(double a, double b) {
    return a > b ? a : b;
}

/* FPU_CULL_VAL, as libnaomi sets it */
#define CULL_VAL 0.02

static void lite_setup(SETUP* p);

static void setup_tri(SETUP* p, int list, const RD_TRI* t, int order) {
    const RD_STATE* s = &rd_frame->states[t->state];
    const RD_VTX *a = &p->v[0], *b = &p->v[1], *c = &p->v[2];
    double area;
    if (list == RD_TRANS) {
        tstate[order] = s;
    }
    COLOR k[6];
    int i;
    p->y0 = p->y1 = 0;
    for (i = 0; i < 3; i++) {
        p->v[i] = t->v[i];
        if (scale != 1) {
            p->v[i].x /= scale, p->v[i].y /= scale;
        }
    }
    area = edge(a, b, c->x, c->y);
    if (area == 0.0 || !(area == area) || (s->cull != 0 && fabs(area) < CULL_VAL) || (s->cull == 2 && area < 0) ||
        (s->cull == 3 && area > 0)) {
        return;
    }
    p->s = s;
    p->t = t;
    p->list = list;
    p->order = order;
    p->flat = s->flat || (a->base == b->base && a->base == c->base && a->offset == b->offset && a->offset == c->offset);
    p->area = area;
    p->sign = area > 0 ? 1.0 : -1.0;
    p->own[0] = edge_owns(b, c, p->sign);
    p->own[1] = edge_owns(c, a, p->sign);
    p->own[2] = edge_owns(a, b, p->sign);
    for (i = 0; i < 3; i++) {
        const RD_VTX *e = &p->v[(i + 1) % 3], *f = &p->v[(i + 2) % 3];
        p->ox[i] = e->x, p->oy[i] = e->y;
        p->ex[i] = (double)f->x - e->x, p->ey[i] = (double)f->y - e->y;
        p->rd[i] = p->ey[i] != 0 ? 1.0 / (p->ey[i] * p->sign) : 0;
    }
    p->near = 1;
    for (i = 0; i < 3; i++) {
        p->near &= fabsf(p->v[i].x) <= 65536.0f && fabsf(p->v[i].y) <= 65536.0f;
    }
    p->gx[0] = -((double)c->y - b->y) / area, p->gy[0] = ((double)c->x - b->x) / area;
    p->gx[1] = -((double)a->y - c->y) / area, p->gy[1] = ((double)a->x - c->x) / area;
    p->gx[2] = -((double)b->y - a->y) / area, p->gy[2] = ((double)b->x - a->x) / area;
    p->zx = (float)(p->gx[0] * a->z + p->gx[1] * b->z + p->gx[2] * c->z);
    p->zy = (float)(p->gy[0] * a->z + p->gy[1] * b->z + p->gy[2] * c->z);
    if (s->tex != NULL && s->tex->levels > 0) {
        const double *gx = p->gx, *gy = p->gy;
        p->uz_x = (float)(gx[0] * a->u * a->z + gx[1] * b->u * b->z + gx[2] * c->u * c->z);
        p->uz_y = (float)(gy[0] * a->u * a->z + gy[1] * b->u * b->z + gy[2] * c->u * c->z);
        p->vz_x = (float)(gx[0] * a->v * a->z + gx[1] * b->v * b->z + gx[2] * c->v * c->z);
        p->vz_y = (float)(gy[0] * a->v * a->z + gy[1] * b->v * b->z + gy[2] * c->v * c->z);
    }
    if (list == RD_OPAQUE) {
        p->depth = s->depth;
        p->zwrite = s->zwrite;
    } else {
        p->depth = RD_GEQUAL;      /* punch-through and auto-sorted translucent */
        p->zwrite = list == RD_PUNCH;
    }
    /* clamped before the casts: a vertex cut at the near plane can be past int's range */
    p->x0 = (int)dmin(dmax(floor(dmin(a->x, dmin(b->x, c->x))), 0.0), draw_w);
    p->x1 = (int)dmax(dmin(ceil(dmax(a->x, dmax(b->x, c->x))), draw_w), 0.0);
    p->y0 = (int)dmin(dmax(floor(dmin(a->y, dmin(b->y, c->y))), 0.0), draw_h);
    p->y1 = (int)dmax(dmin(ceil(dmax(a->y, dmax(b->y, c->y))), draw_h), 0.0);
    p->level = s->tex != NULL && s->tex->levels > 0 ? -1 : 0;
    for (i = 0; i < 3; i++) {
        float w = p->v[i].z;
        k[i] = unpack(p->v[i].base);
        k[i + 3] = unpack(p->v[i].offset);
        p->col[i][0] = k[i].a * w, p->col[i][1] = k[i].r * w, p->col[i][2] = k[i].g * w, p->col[i][3] = k[i].b * w;
        p->col[i][4] = k[i + 3].a * w, p->col[i][5] = k[i + 3].r * w, p->col[i][6] = k[i + 3].g * w;
        p->col[i][7] = k[i + 3].b * w;
    }
    {
        double w0 = edge(b, c, 0.5, 0.5) / area, w1 = edge(c, a, 0.5, 0.5) / area, w2 = edge(a, b, 0.5, 0.5) / area;
        double f[3][PLANES];
        int j;
        for (i = 0; i < 3; i++) {
            const RD_VTX* v = &p->v[i];
            f[i][0] = v->z;
            f[i][1] = (double)v->u * v->z;
            f[i][2] = (double)v->v * v->z;
            for (j = 0; j < 8; j++) {
                f[i][3 + j] = p->col[i][j];
            }
        }
        for (j = 0; j < PLANES; j++) {
            p->at[j] = w0 * f[0][j] + w1 * f[1][j] + w2 * f[2][j];
            p->dx[j] = p->gx[0] * f[0][j] + p->gx[1] * f[1][j] + p->gx[2] * f[2][j];
            p->dy[j] = p->gy[0] * f[0][j] + p->gy[1] * f[1][j] + p->gy[2] * f[2][j];
        }
    }
    p->dmask = depth_mask[p->depth & 7];
    p->clear0 = list == RD_TRANS && s->src == RD_SRC_ALPHA && (s->dst == RD_INV_SRC_ALPHA || s->dst == RD_ONE);
    p->fogged = (s->fog == RD_FOG_TABLE && fog_on && !nofog) || s->fog == RD_FOG_TABLE2;
    p->fbase = unpack(c->base);
    if (!s->use_alpha) {
        p->fbase.a = 255;
    }
    p->foffs = unpack(c->offset);
    p->foffs.a = 0;
    p->fogc = unpack(rd_fog.color);
    p->kind = p->flat;
    if (s->tex != NULL) {
        int pal = s->tex->format == RD_PAL4 || s->tex->format == RD_PAL8;
        p->kind += pal ? 4 : 2;
        if (s->bilinear && !fast) {
            p->kind += 4;
        }
    }
    if ((fast || lite) && p->level < 0) {
        /* one level for the whole triangle, at its middle */
        double x = (p->x0 + p->x1) * 0.5, y = (p->y0 + p->y1) * 0.5;
        float z = (float)(p->at[0] + p->dx[0] * x + p->dy[0] * y), rz, u, v;
        p->level = 0;
        if (z > 0.0f) {
            rz = 1.0f / z;
            u = (float)((p->at[1] + p->dx[1] * x + p->dy[1] * y) * rz);
            v = (float)((p->at[2] + p->dx[2] * x + p->dy[2] * y) * rz);
            p->level = mip_level(s, (p->uz_x - u * p->zx) * rz, (p->vz_x - v * p->zx) * rz,
                                 (p->uz_y - u * p->zy) * rz, (p->vz_y - v * p->zy) * rz);
        }
    }
    if (lite) {
        lite_setup(p);
    }
}

/* the edges' sums at row py, edge(a, b, x, py) being t - ey (x - a->x) */
typedef struct {
    double t[3];
} ROW;

/* the exact test, with the top-left rule */
PIXEL_STEP int inside(const SETUP* p, const ROW* r, int x) {
    double px = x + 0.5, sign = p->sign;
    double e0 = (r->t[0] - p->ey[0] * (px - p->ox[0])) * sign, e1 = (r->t[1] - p->ey[1] * (px - p->ox[1])) * sign;
    double e2 = (r->t[2] - p->ey[2] * (px - p->ox[2])) * sign;
    return !(e0 < 0 || e1 < 0 || e2 < 0 || (e0 == 0 && !p->own[0]) || (e1 == 0 && !p->own[1]) ||
             (e2 == 0 && !p->own[2]));
}

/* the columns where edge i can be inside, a pixel or two wide of it */
PIXEL_STEP void edge_span(const SETUP* p, const ROW* r, int i, double* lo, double* hi) {
    double d = p->ey[i] * p->sign, e = r->t[i] * p->sign, x;
    if (d == 0) {
        if (e < 0) {
            *lo = 1e30, *hi = -1e30;
        }
        return;
    }
    /* near the screen, its reciprocal: the edge found within far less than the two pixels' margin */
    x = (p->near ? e * p->rd[i] : e / d) + p->ox[i] - 0.5;
    if (!(x == x)) {
        return;
    }
    if (d > 0) {
        *hi = dmin(*hi, x + 2);
    } else {
        *lo = dmax(*lo, x - 2);
    }
}

/* row y's pixels inside p, from *xs to *xe - 1; 0 when there are none */
static int row_span(const SETUP* p, int y, int* xs_, int* xe_) {
    double py = y + 0.5, lo = p->x0, hi = p->x1;
    ROW r;
    int xs, xe, i;
    for (i = 0; i < 3; i++) {
        r.t[i] = p->ex[i] * (py - p->oy[i]);
    }
    edge_span(p, &r, 0, &lo, &hi);
    edge_span(p, &r, 1, &lo, &hi);
    edge_span(p, &r, 2, &lo, &hi);
    if (!(lo < hi)) {
        return 0;
    }
    xs = lo <= p->x0 ? p->x0 : (int)lo;     /* floor: lo > x0 >= 0 */
    xe = hi >= p->x1 ? p->x1 : (int)hi + (hi > (int)hi);
    while (xs < xe && !inside(p, &r, xs)) {
        xs++;
    }
    while (xe > xs && !inside(p, &r, xe - 1)) {
        xe--;
    }
    *xs_ = xs, *xe_ = xe;
    return xs < xe;
}

PIXEL_STEP int depth_ok(int mask, float z, float old) {
    int m = (z < old) | (z == old) << 1 | (z > old) << 2;
    return (mask & (m != 0 ? m : 8)) != 0;
}

/* row y's 1/w from xs to xe - 1, fz the first and the exact span's sums on, into wk->lz and tested: TAKE, the pixels
   that pass taken for triangle id, else which pass into wk->lok. The first and last that pass; 0 when none do */
PIXEL_STEP int depth_run(const SETUP* p, WORKER* wk, int y, int xs, int xe, int id, double fz, const int take,
                         int* first_, int* last_) {
    float* zrow = &zbuf[y * RD_WIDTH];
    int* win = &wk->win[(y - wk->band_y) * RD_WIDTH];
    float* lz = wk->lz;
    uint8_t* lok = wk->lok;
    const double dz = p->dx[0];
    const int mask = p->dmask;
    int x, first = xe, last = -1;
    for (x = xs; x < xe; x++) {
        if (x > xs) {
            fz += dz;
        }
        lz[x] = (float)fz;
    }
    x = xs;
#if defined(RD_NEON)
    /* on the bits: for z > 0 their order as ints is the floats', but for an old NaN */
    if (xe - xs >= 4) {
        const uint32x4_t m1 = vdupq_n_u32(mask & 1 ? ~0u : 0), m2 = vdupq_n_u32(mask & 2 ? ~0u : 0);
        const uint32x4_t m4 = vdupq_n_u32(mask & 4 ? ~0u : 0), m8 = vdupq_n_u32(mask & 8 ? ~0u : 0);
        const uint32x4_t zw = vdupq_n_u32(p->zwrite ? ~0u : 0);
        const uint32x4_t one = vdupq_n_u32(1), inf = vdupq_n_u32(0x7F800000);
        uint32x4_t lo = vdupq_n_u32(~0u), hi = vdupq_n_u32(0);
        uint32x2_t l2, h2;
        /* the last four end at xe, again over some: the same again */
        for (;;) {
            uint32x4_t at = vaddq_u32(vdupq_n_u32((uint32_t)x), (uint32x4_t){ 1, 2, 3, 4 });
            int32x4_t zb = vld1q_s32((const int32_t*)&lz[x]), ob = vld1q_s32((const int32_t*)&zrow[x]);
            uint32x4_t nan = vcgtq_u32(vandq_u32(vreinterpretq_u32_s32(ob), vdupq_n_u32(0x7FFFFFFF)), inf);
            uint32x4_t pos = vandq_u32(vcgtq_s32(zb, vdupq_n_s32(0)), vcleq_s32(zb, vreinterpretq_s32_u32(inf)));
            uint32x4_t ord = vorrq_u32(vorrq_u32(vandq_u32(vcltq_s32(zb, ob), m1), vandq_u32(vceqq_s32(zb, ob), m2)),
                                       vandq_u32(vcgtq_s32(zb, ob), m4));
            uint32x4_t ok = vandq_u32(vbslq_u32(nan, m8, ord), pos);
            if (take) {
                vst1q_s32(&win[x], vbslq_s32(ok, vdupq_n_s32(id), vld1q_s32(&win[x])));
                vst1q_s32((int32_t*)&zrow[x], vbslq_s32(vandq_u32(ok, zw), zb, ob));
            } else {
                uint16x4_t k = vmovn_u32(vandq_u32(ok, one));
                vst1_lane_u32((uint32_t*)&lok[x], vreinterpret_u32_u8(vmovn_u16(vcombine_u16(k, k))), 0);
            }
            lo = vminq_u32(lo, vbslq_u32(ok, at, vdupq_n_u32(~0u)));
            hi = vmaxq_u32(hi, vandq_u32(ok, at));
            if (x + 4 >= xe) {
                break;
            }
            x = x + 8 <= xe ? x + 4 : xe - 4;
        }
        x = xe;
        l2 = vpmin_u32(vget_low_u32(lo), vget_high_u32(lo));
        h2 = vpmax_u32(vget_low_u32(hi), vget_high_u32(hi));
        l2 = vpmin_u32(l2, l2), h2 = vpmax_u32(h2, h2);
        if (vget_lane_u32(h2, 0) != 0) {
            first = (int)vget_lane_u32(l2, 0) - 1, last = (int)vget_lane_u32(h2, 0) - 1;
        }
    }
#elif defined(RD_SSE2)
    /* on the bits, as NEON's */
    if (xe - xs >= 4) {
        const __m128i m1 = _mm_set1_epi32(mask & 1 ? -1 : 0), m2 = _mm_set1_epi32(mask & 2 ? -1 : 0);
        const __m128i m4 = _mm_set1_epi32(mask & 4 ? -1 : 0), m8 = _mm_set1_epi32(mask & 8 ? -1 : 0);
        const __m128i zw = _mm_set1_epi32(p->zwrite ? -1 : 0), idv = _mm_set1_epi32(id);
        const __m128i one = _mm_set1_epi32(1), inf = _mm_set1_epi32(0x7F800000), abs = _mm_set1_epi32(0x7FFFFFFF);
        for (;;) {
            __m128i zb = _mm_loadu_si128((const __m128i*)&lz[x]), ob = _mm_loadu_si128((const __m128i*)&zrow[x]);
            __m128i nan = _mm_cmpgt_epi32(_mm_and_si128(ob, abs), inf);
            __m128i pos = _mm_andnot_si128(_mm_cmpgt_epi32(zb, inf), _mm_cmpgt_epi32(zb, _mm_setzero_si128()));
            __m128i ord = _mm_or_si128(_mm_or_si128(_mm_and_si128(_mm_cmplt_epi32(zb, ob), m1),
                                                    _mm_and_si128(_mm_cmpeq_epi32(zb, ob), m2)),
                                       _mm_and_si128(_mm_cmpgt_epi32(zb, ob), m4));
            __m128i ok = _mm_and_si128(sel128(nan, m8, ord), pos);
            int k;
            if (take) {
                _mm_storeu_si128((__m128i*)&win[x], sel128(ok, idv, _mm_loadu_si128((const __m128i*)&win[x])));
                _mm_storeu_si128((__m128i*)&zrow[x], sel128(_mm_and_si128(ok, zw), zb, ob));
            } else {
                __m128i n = _mm_packs_epi32(_mm_and_si128(ok, one), ok);
                k = _mm_cvtsi128_si32(_mm_packus_epi16(n, n));
                memcpy(&lok[x], &k, 4);
            }
            k = _mm_movemask_ps(_mm_castsi128_ps(ok));
            if (k != 0) {
                first = x + lane_lo[k] < first ? x + lane_lo[k] : first;
                last = x + lane_hi[k] > last ? x + lane_hi[k] : last;
            }
            if (x + 4 >= xe) {
                break;
            }
            x = x + 8 <= xe ? x + 4 : xe - 4;
        }
        x = xe;
    }
#endif
    for (; x < xe; x++) {
        float z = lz[x];
        int ok = depth_ok(mask, z, zrow[x]) && z > 0.0f;
        if (take) {
            if (ok) {
                win[x] = id;
                if (p->zwrite) {
                    zrow[x] = z;
                }
            }
        } else {
            lok[x] = (uint8_t)ok;
        }
        if (ok) {
            first = x < first ? x : first;
            last = x;
        }
    }
    *first_ = first, *last_ = last;
    return last >= 0;
}

/* the texture's level k */
PIXEL_STEP const void* level_texels(const RD_TEXTURE* t, int k, int* w, int* h) {
    *w = t->width >> k, *h = t->height >> k;
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
    return k == 0 ? t->texels : t->mip[k];
}

/* p's pixels xs to xe - 1 of row y: TEX 0 none, 1 a 16-bit format, 2 a palette's */
/* the depth pass gives the column after the last it took, 0 for none */
PIXEL_STEP int span(const SETUP* p, WORKER* wk, int y, int xs, int xe, int id, const int mode, const int flat,
                    const int tex, const int bil) {
    const RD_STATE* s = p->s;
    const int list = p->list, offset = s->offset, mask = p->dmask, fogged = p->fogged;
    float* zrow = &zbuf[y * RD_WIDTH];
    uint32_t* crow = &cbuf[y * RD_WIDTH];
    int* win = &wk->win[(y - wk->band_y) * RD_WIDTH];
    double fz = p->at[0] + p->dx[0] * xs + p->dy[0] * y, dz = p->dx[0];
    double fu = 0, fv = 0, du = 0, dv = 0;
    double c0 = 0, c1 = 0, c2 = 0, c3 = 0, o1 = 0, o2 = 0, o3 = 0;
    const void* d = NULL;
    int w = 1, h = 1, x;
    /* 1/w the same all along: z, 1/z and the fog once */
    const int zconst = dz == 0.0;
    float rz0 = 1.0f, fog0 = 0.0f;
    if (mode == DEPTH) {
        int first, last;
        if (!depth_run(p, wk, y, xs, xe, id, fz, 1, &first, &last)) {
            return 0;
        }
        wk->won[id] = wk->band;
        return last + 1;
    }
    if (tex) {
        fu = p->at[1] + p->dx[1] * xs + p->dy[1] * y, du = p->dx[1];
        fv = p->at[2] + p->dx[2] * xs + p->dy[2] * y, dv = p->dx[2];
        if (p->level >= 0) {
            d = level_texels(s->tex, p->level, &w, &h);
        }
    }
    if (!flat) {
        c0 = p->at[3] + p->dx[3] * xs + p->dy[3] * y;
        c1 = p->at[4] + p->dx[4] * xs + p->dy[4] * y;
        c2 = p->at[5] + p->dx[5] * xs + p->dy[5] * y;
        c3 = p->at[6] + p->dx[6] * xs + p->dy[6] * y;
        if (offset) {
            o1 = p->at[8] + p->dx[8] * xs + p->dy[8] * y;
            o2 = p->at[9] + p->dx[9] * xs + p->dy[9] * y;
            o3 = p->at[10] + p->dx[10] * xs + p->dy[10] * y;
        }
    }
    if (zconst && mode != DEPTH) {
        float z = (float)fz;
        if (z > 0.0f) {
            rz0 = 1.0f / z;
            if (fogged) {
                fog0 = fog_value(z);
            }
        }
    }
    for (x = xs; x < xe; x++) {
        float z, rz, u = 0, v = 0;
        COLOR base, offs, c;
        if (x > xs) {
            fz += dz;
            if (tex) {
                fu += du;
                fv += dv;
            }
            if (!flat) {
                c0 += p->dx[3], c1 += p->dx[4], c2 += p->dx[5], c3 += p->dx[6];
                if (offset) {
                    o1 += p->dx[8], o2 += p->dx[9], o3 += p->dx[10];
                }
            }
        }
        z = (float)fz;
        if (mode == SHADE_WON) {
            if (win[x] != id) {
                continue;
            }
        } else if (!depth_ok(mask, z, zrow[x]) || !(z > 0.0f)) {
            continue;
        }
        rz = zconst ? rz0 : 1.0f / z;
        if (tex) {
            u = (float)(fu * rz);
            v = (float)(fv * rz);
        }
        if (flat) {
            base = p->fbase;
            offs = p->foffs;
        } else {
            base.a = s->use_alpha ? clamp255(to_int((float)(c0 * rz) + 0.5f)) : 255;
            base.r = clamp255(to_int((float)(c1 * rz) + 0.5f));
            base.g = clamp255(to_int((float)(c2 * rz) + 0.5f));
            base.b = clamp255(to_int((float)(c3 * rz) + 0.5f));
            offs.a = 0;
            if (offset) {
                offs.r = clamp255(to_int((float)(o1 * rz) + 0.5f));
                offs.g = clamp255(to_int((float)(o2 * rz) + 0.5f));
                offs.b = clamp255(to_int((float)(o3 * rz) + 0.5f));
            } else {
                offs.r = offs.g = offs.b = 0;
            }
        }
#if defined(RD_NEON)
        /* a texel's colour taken in NEON from the texture to the pixel, the plain C's sums, its lanes a r g b */
        if (tex && p->level >= 0 && (mode == SHADE_WON || list == RD_OPAQUE || (list == RD_TRANS && !nosort)) &&
            (!bil || (fabsf(u * w) < 16777216.0f && fabsf(v * h) < 16777216.0f))) {
            const uint32x4_t lane_a = vsetq_lane_u32(~0u, vdupq_n_u32(0), 0);
            int32x4_t t, b, q;
            uint32x2_t out;
            if (bil) {
                float ax, ay;
                bilinear_texels(s, tex, p->level, d, w, h, u, v, &wk->tex, &ax, &ay);
                t = bilinear_mix(&wk->tex, ax, ay);
            } else {
                /* b g r a, turned */
                uint32x4_t k = vmovl_u16(vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(point_texel(s, tex, d, w, h, u, v))))));
                k = vrev64q_u32(k);
                t = vreinterpretq_s32_u32(vcombine_u32(vget_high_u32(k), vget_low_u32(k)));
            }
            if (!s->tex_alpha) {
                t = vsetq_lane_s32(255, t, 0);
            }
            b = vld1q_s32(&base.a);
            switch (s->shading) {
            case RD_DECAL:
                q = t;
                break;
            case RD_MODULATE:
                q = vbslq_s32(lane_a, t, div255q(vmulq_s32(t, b)));
                break;
            case RD_DECAL_ALPHA: {
                int32x4_t ta = vdupq_lane_s32(vget_low_s32(t), 0);
                q = div255q(vaddq_s32(vmulq_s32(t, ta), vmulq_s32(b, vsubq_s32(vdupq_n_s32(255), ta))));
                q = vbslq_s32(lane_a, b, q);
                break;
            }
            default:
                q = div255q(vmulq_s32(t, b));
                break;
            }
            if (offset) {
                q = vminq_s32(vmaxq_s32(vaddq_s32(q, vld1q_s32(&offs.a)), vdupq_n_s32(0)), vdupq_n_s32(255));
            }
            if (fogged) {
                float f = zconst ? fog0 : fog_value(z);
                int32x4_t k = vld1q_s32(&p->fogc.a);
                if (s->fog == RD_FOG_TABLE2) {
                    q = vsetq_lane_s32((int)(f * 255.0f + 0.5f), k, 0);
                } else {
                    float32x4_t g;
                    k = vbslq_s32(lane_a, q, k);
                    g = vmulq_f32(vcvtq_f32_s32(vsubq_s32(k, q)), vdupq_n_f32(f));
                    g = vaddq_f32(g, vbslq_f32(vcgtq_s32(k, q), vdupq_n_f32(0.5f), vdupq_n_f32(-0.5f)));
                    q = vaddq_s32(q, vcvtq_s32_f32(g));
                }
            }
            {
                uint16x4_t n = vmovn_u32(vreinterpretq_u32_s32(q));
                out = vreinterpret_u32_u8(vrev32_u8(vmovn_u16(vcombine_u16(n, n))));
            }
            if (mode == SHADE_WON || list == RD_OPAQUE) {
                vst1_lane_u32(&crow[x], out, 0);
                if (mode != SHADE_WON && p->zwrite) {
                    zrow[x] = z;
                }
            } else if (!(p->clear0 && vgetq_lane_s32(q, 0) == 0)) {
                FRAG* g = new_frag(wk, y * RD_WIDTH + x, z, p->order);
                if (g != NULL) {
                    vst1_lane_u32(&g->color, out, 0);
                }
            }
            continue;
        }
#elif defined(RD_SSE2)
        /* as NEON's: the channels all under 2^15, so a madd lane multiplies one */
        if (tex && p->level >= 0 && (mode == SHADE_WON || list == RD_OPAQUE || (list == RD_TRANS && !nosort)) &&
            (!bil || (fabsf(u * w) < 16777216.0f && fabsf(v * h) < 16777216.0f))) {
            const __m128i lane_a = _mm_setr_epi32(-1, 0, 0, 0);
            __m128i t, b, q;
            uint32_t out;
            if (bil) {
                float ax, ay;
                bilinear_texels(s, tex, p->level, d, w, h, u, v, &wk->tex, &ax, &ay);
                t = bilinear_mix(&wk->tex, ax, ay);
            } else {
                /* b g r a, turned */
                __m128i z0 = _mm_setzero_si128();
                t = _mm_cvtsi32_si128((int)point_texel(s, tex, d, w, h, u, v));
                t = _mm_shuffle_epi32(_mm_unpacklo_epi16(_mm_unpacklo_epi8(t, z0), z0), _MM_SHUFFLE(0, 1, 2, 3));
            }
            if (!s->tex_alpha) {
                t = sel128(lane_a, _mm_set1_epi32(255), t);
            }
            b = _mm_loadu_si128((const __m128i*)&base.a);
            switch (s->shading) {
            case RD_DECAL:
                q = t;
                break;
            case RD_MODULATE:
                q = sel128(lane_a, t, div255q(_mm_madd_epi16(t, b)));
                break;
            case RD_DECAL_ALPHA: {
                __m128i ta = _mm_shuffle_epi32(t, 0);
                q = div255q(_mm_add_epi32(_mm_madd_epi16(t, ta), _mm_madd_epi16(b, _mm_sub_epi32(_mm_set1_epi32(255), ta))));
                q = sel128(lane_a, b, q);
                break;
            }
            default:
                q = div255q(_mm_madd_epi16(t, b));
                break;
            }
            if (offset) {
                /* both 0 to 255 */
                q = _mm_min_epi16(_mm_add_epi32(q, _mm_loadu_si128((const __m128i*)&offs.a)), _mm_set1_epi32(255));
            }
            if (fogged) {
                float f = zconst ? fog0 : fog_value(z);
                __m128i k = _mm_loadu_si128((const __m128i*)&p->fogc.a);
                if (s->fog == RD_FOG_TABLE2) {
                    q = sel128(lane_a, _mm_cvtsi32_si128((int)(f * 255.0f + 0.5f)), k);
                } else {
                    __m128 g;
                    k = sel128(lane_a, q, k);
                    g = _mm_mul_ps(_mm_cvtepi32_ps(_mm_sub_epi32(k, q)), _mm_set1_ps(f));
                    g = _mm_add_ps(g, _mm_castsi128_ps(sel128(_mm_cmpgt_epi32(k, q), _mm_castps_si128(_mm_set1_ps(0.5f)),
                                                              _mm_castps_si128(_mm_set1_ps(-0.5f)))));
                    q = _mm_add_epi32(q, _mm_cvttps_epi32(g));
                }
            }
            q = _mm_packs_epi32(_mm_shuffle_epi32(q, _MM_SHUFFLE(0, 1, 2, 3)), q);
            out = (uint32_t)_mm_cvtsi128_si32(_mm_packus_epi16(q, q));
            if (mode == SHADE_WON || list == RD_OPAQUE) {
                crow[x] = out;
                if (mode != SHADE_WON && p->zwrite) {
                    zrow[x] = z;
                }
            } else if (!(p->clear0 && out >> 24 == 0)) {
                add_frag(wk, y * RD_WIDTH + x, z, out, p->order);
            }
            continue;
        }
#endif
        c = base;
        if (tex) {
            COLOR t;
            int k = p->level;
            if (k < 0) {
                k = mip_level(s, (p->uz_x - u * p->zx) * rz, (p->vz_x - v * p->zx) * rz, (p->uz_y - u * p->zy) * rz,
                              (p->vz_y - v * p->zy) * rz);
                d = level_texels(s->tex, k, &w, &h);
            }
            t = bil ? sample_bilinear(s, tex, k, d, w, h, u, v, &wk->tex) : sample_point(s, tex, d, w, h, u, v);
            if (!s->tex_alpha) {
                t.a = 255;
            }
            switch (s->shading) {
            case RD_DECAL:
                c = t;
                break;
            case RD_MODULATE:
                c.r = t.r * base.r / 255;
                c.g = t.g * base.g / 255;
                c.b = t.b * base.b / 255;
                c.a = t.a;
                break;
            case RD_DECAL_ALPHA:
                c.r = (t.r * t.a + base.r * (255 - t.a)) / 255;
                c.g = (t.g * t.a + base.g * (255 - t.a)) / 255;
                c.b = (t.b * t.a + base.b * (255 - t.a)) / 255;
                c.a = base.a;
                break;
            default:
                c.r = t.r * base.r / 255;
                c.g = t.g * base.g / 255;
                c.b = t.b * base.b / 255;
                c.a = t.a * base.a / 255;
                break;
            }
        }
        if (offset) {
            c.r = clamp255(c.r + offs.r);
            c.g = clamp255(c.g + offs.g);
            c.b = clamp255(c.b + offs.b);
        }
        if (fogged) {
            fog(s, &c, zconst ? fog0 : fog_value(z));
        }
        if (mode == SHADE_WON) {
            crow[x] = pack(c);
            continue;
        }
        if (c.a == 0 && p->clear0) {
            continue;
        }
        if (list == RD_TRANS && !nosort) {
            add_frag(wk, y * RD_WIDTH + x, z, pack(c), p->order);
            continue;
        }
        if (list == RD_PUNCH && c.a < 128) {
            continue;
        }
        crow[x] = list == RD_OPAQUE ? pack(c) : blend(s, c, crow[x]);
        if (p->zwrite) {
            zrow[x] = z;
        }
    }
    return 0;
}

#define SPAN(mode, flat, tex, bil) span(p, wk, y, xs, xe, id, mode, flat, tex, bil)

/* by kind: whether flat, the texture's kind, and bilinear */
PIXEL_STEP void span_kind(const SETUP* p, WORKER* wk, int y, int xs, int xe, int id, const int mode) {
    switch (p->kind) {
    case 0: SPAN(mode, 0, 0, 0); break;
    case 1: SPAN(mode, 1, 0, 0); break;
    case 2: SPAN(mode, 0, 1, 0); break;
    case 3: SPAN(mode, 1, 1, 0); break;
    case 4: SPAN(mode, 0, 2, 0); break;
    case 5: SPAN(mode, 1, 2, 0); break;
    case 6: SPAN(mode, 0, 1, 1); break;
    case 7: SPAN(mode, 1, 1, 1); break;
    case 8: SPAN(mode, 0, 2, 1); break;
    default: SPAN(mode, 1, 2, 1); break;
    }
}

/* lite: near the exact pixels for less work, for a CPU slow to turn floats into integers. Each row's ends from the
   edges walked in a line, the planes in fixed point along it, exact every LSEG pixels, one mip level a triangle and
   the texels mixed in integers. A span goes through in passes, each over all its pixels: the planes, the texels'
   places, the texels, the colours and the pixels out */

/* the fog at 1/w z, 0 to 256 */
static int fog256(float z) {
    return (int)(fog_value(z) * 256.0f + 0.5f);
}

/* the first row whose centre is at v or below */
static int lite_rowof(double v) {
    v -= 0.5;
    return v > RD_HEIGHT ? RD_HEIGHT + 1 : v > -1.0 ? (int)ceil(v) : -1;
}

static void lite_setup(SETUP* p) {
    const RD_STATE* s = p->s;
    double ox = p->x0, oy = p->y0, m[LPLANES];
    int i, k, w = 1, h = 1;
    for (i = 0; i < 3; i++) {
        const RD_VTX *e = &p->v[(i + 1) % 3], *f = &p->v[(i + 2) % 3], *g = &p->v[i];
        if (f->y < e->y || (f->y == e->y && f->x < e->x)) {
            const RD_VTX* t = e;
            e = f, f = t;
        }
        p->lx[i] = e->x, p->ly[i] = e->y;
        p->lside[i] = 0, p->lys[i] = p->lye[i] = 0;
        if (f->y > e->y) {
            p->lys[i] = lite_rowof(e->y), p->lye[i] = lite_rowof(f->y);
            double cross = ((double)f->x - e->x) * ((double)g->y - e->y) - ((double)f->y - e->y) * ((double)g->x - e->x);
            p->lk[i] = ((double)f->x - e->x) / ((double)f->y - e->y);
            p->lside[i] = cross < 0 ? 1 : -1;
        }
    }
    for (k = 0; k < LPLANES; k++) {
        m[k] = 65536.0, p->lpb[k] = 32768.0f;   /* the colours rounded */
    }
    if (s->tex != NULL) {
        level_texels(s->tex, p->level, &w, &h);
        m[0] = 65536.0 * w, m[1] = 65536.0 * h;
        /* bilinear: from the texel centres; nearest: just past a texel's edge */
        p->lpb[0] = p->lpb[1] = p->kind >= 6 ? -32768.0f : 1024.0f;
    }
    /* from the bounding box's corner */
    p->lox = p->x0, p->loy = p->y0;
    p->lza = (float)(p->at[0] + p->dx[0] * ox + p->dy[0] * oy), p->lzx = (float)p->dx[0], p->lzy = (float)p->dy[0];
    for (k = 0; k < LPLANES; k++) {
        if (k < PLANES - 1) {
            p->lpa[k] = (float)((p->at[k + 1] + p->dx[k + 1] * ox + p->dy[k + 1] * oy) * m[k]);
            p->lpx[k] = (float)(p->dx[k + 1] * m[k]), p->lpy[k] = (float)(p->dy[k + 1] * m[k]);
        } else {
            p->lpa[k] = p->lpx[k] = p->lpy[k] = p->lpb[k] = 0.0f;
        }
    }
}

/* row y's columns from *xs to *xe - 1, those whose centres are inside the edges; 0 when there are none */
static int lite_row(const SETUP* p, int y, int* xs_, int* xe_) {
    double py = y + 0.5, lo = p->x0, hi = p->x1;
    int i, n = 0, xs, xe;
    for (i = 0; i < 3; i++) {
        if (y >= p->lys[i] && y < p->lye[i]) {
            double x = p->lx[i] + (py - p->ly[i]) * p->lk[i];
            if (p->lside[i] > 0) {
                lo = dmax(lo, x);
            } else {
                hi = dmin(hi, x);
            }
            n++;
        }
    }
    if (n < 2 || !(lo < hi)) {
        return 0;
    }
    lo -= 0.5, hi -= 0.5;       /* ceilings: both above -1 */
    xs = (int)lo + (lo > (int)lo);
    xe = (int)hi + (hi > (int)hi);
    *xs_ = xs, *xe_ = xe;
    return xs < xe;
}

/* the planes at pixel (x, y) into q: u and v in texels, then the colours, by 65536; 1/w back */
PIXEL_STEP float lite_point(const SETUP* p, int x, int y, float* q) {
    float xf = (float)(x - p->lox), yf = (float)(y - p->loy), z = p->lza + p->lzx * xf, rz;
    int k;
    z = z + p->lzy * yf;
    rz = z > 0.0f ? 1.0f / z : 0.0f;
#if defined(RD_NEON)
    for (k = 0; k < LPLANES; k += 4) {
        float32x4_t t = vmlaq_n_f32(vmlaq_n_f32(vld1q_f32(&p->lpa[k]), vld1q_f32(&p->lpx[k]), xf), vld1q_f32(&p->lpy[k]), yf);
        vst1q_f32(&q[k], vaddq_f32(vmulq_n_f32(t, rz), vld1q_f32(&p->lpb[k])));
    }
#elif defined(RD_SSE2)
    for (k = 0; k < LPLANES; k += 4) {
        __m128 t = _mm_add_ps(_mm_loadu_ps(&p->lpa[k]), _mm_mul_ps(_mm_loadu_ps(&p->lpx[k]), _mm_set1_ps(xf)));
        t = _mm_add_ps(t, _mm_mul_ps(_mm_loadu_ps(&p->lpy[k]), _mm_set1_ps(yf)));
        _mm_storeu_ps(&q[k], _mm_add_ps(_mm_mul_ps(t, _mm_set1_ps(rz)), _mm_loadu_ps(&p->lpb[k])));
    }
#else
    for (k = 0; k < LPLANES; k++) {
        float t = p->lpa[k] + p->lpx[k] * xf;
        t = t + p->lpy[k] * yf;
        t = t * rz;
        q[k] = t + p->lpb[k];
    }
#endif
    return z;
}

#if !defined(RD_NEON)
/* x / 255 rounded down, for x from 0 to 65534 */
PIXEL_STEP int div255(int x) {
    return (x + 1 + (x >> 8)) >> 8;
}

/* f as an int, held to +-lim, NaN 0: as NEON takes it */
PIXEL_STEP int lite_int(float f, float lim) {
    return f > lim ? (int)lim : f < -lim ? -(int)lim : f == f ? (int)f : 0;
}
#endif

#if defined(RD_SSE2)
/* lite_int's, 4 at a time */
PIXEL_STEP __m128i lite_int4(__m128 f, float lim) {
    __m128i k = _mm_cvttps_epi32(_mm_min_ps(_mm_max_ps(f, _mm_set1_ps(-lim)), _mm_set1_ps(lim)));
    return _mm_and_si128(k, _mm_castps_si128(_mm_cmpord_ps(f, f)));
}

/* s + d at for at from a to a + 3 */
PIXEL_STEP __m128i plane4(int32_t s, int32_t d, int a) {
    uint32_t k = (uint32_t)s + (uint32_t)d * (uint32_t)a;
    return _mm_setr_epi32((int32_t)k, (int32_t)(k + (uint32_t)d), (int32_t)(k + 2 * (uint32_t)d), (int32_t)(k + 3 * (uint32_t)d));
}
#endif

#define LMAXSEG (RD_WIDTH / LSEG + 2)
#define LPAD (RD_WIDTH + 16)

struct lbuf {
    int32_t s[LMAXSEG][LPLANES], d[LMAXSEG][LPLANES];   /* the planes at a segment's start, and their steps */
    int32_t fs[LMAXSEG], fd[LMAXSEG];                   /* the fog, by 65536 */
    int32_t o[4][LPAD];         /* the texels' places: (x, y), (x + 1, y), (x, y + 1), (x + 1, y + 1) */
    uint8_t fx[LPAD], fy[LPAD]; /* the weights of the right and the lower, of 256 */
    float fu[LPAD], fv[LPAD];   /* nearest: u and v in texels, as the exact span has them */
    uint16_t t16[4][LPAD];      /* the texels, 16-bit */
    uint32_t t32[4][LPAD];      /* or ARGB */
    uint32_t col[LPAD];         /* the pixels' colours, ARGB */
};

/* the planes along the span: segment k from pixel first + LSEG k */
static void lite_points(const SETUP* p, LBUF* b, int first, int last, int y) {
    int len = last - first, ns = len / LSEG + 1, k, i, f0, fe = 0;
    float pk[LPLANES], pn[LPLANES];
    float z = lite_point(p, first, y, pk);
    f0 = p->fogged ? fog256(z) : 0;
    for (k = 0; k < ns; k++) {
        int n = k < ns - 1 ? LSEG : len % LSEG;
        if (n > 0) {
            z = lite_point(p, first + LSEG * k + n, y, pn);
            fe = p->fogged ? fog256(z) : 0;
        } else {
            memcpy(pn, pk, sizeof pn);
            fe = f0;
        }
#if defined(RD_NEON)
        for (i = 0; i < LPLANES; i += 4) {
            float32x4_t a = vld1q_f32(&pk[i]), e = vld1q_f32(&pn[i]), d = vmulq_n_f32(vsubq_f32(e, a), lrcp[n]);
            a = vminq_f32(vmaxq_f32(a, vdupq_n_f32(-1073741824.0f)), vdupq_n_f32(1073741824.0f));
            d = vminq_f32(vmaxq_f32(d, vdupq_n_f32(-33554432.0f)), vdupq_n_f32(33554432.0f));
            vst1q_s32(&b->s[k][i], vcvtq_s32_f32(a));
            vst1q_s32(&b->d[k][i], vcvtq_s32_f32(d));
        }
#elif defined(RD_SSE2)
        for (i = 0; i < LPLANES; i += 4) {
            __m128 a = _mm_loadu_ps(&pk[i]), d = _mm_mul_ps(_mm_sub_ps(_mm_loadu_ps(&pn[i]), a), _mm_set1_ps(lrcp[n]));
            _mm_storeu_si128((__m128i*)&b->s[k][i], lite_int4(a, 1073741824.0f));
            _mm_storeu_si128((__m128i*)&b->d[k][i], lite_int4(d, 33554432.0f));
        }
#else
        for (i = 0; i < LPLANES; i++) {
            float d = pn[i] - pk[i];
            b->s[k][i] = lite_int(pk[i], 1073741824.0f);
            b->d[k][i] = lite_int(d * lrcp[n], 33554432.0f);
        }
#endif
        b->fs[k] = f0 * 65536;
        b->fd[k] = (int)((int64_t)(fe - f0) * 65536 * lrecip[n] >> 32);
        memcpy(pk, pn, sizeof pk);
        f0 = fe;
    }
    /* past the end, for the last group's spare pixels */
    memcpy(b->s[ns], b->s[ns - 1], sizeof b->s[ns]);
    memcpy(b->d[ns], b->d[ns - 1], sizeof b->d[ns]);
    b->fs[ns] = b->fs[ns - 1], b->fd[ns] = b->fd[ns - 1];
}

/* the wrap modes, as wrap takes them */
enum { L_REPEAT, L_CLAMP, L_FLIP };

#if defined(RD_NEON)
PIXEL_STEP int32x4_t wrap4(int32x4_t x, int n, int mode) {
    switch (mode) {
    case L_CLAMP:
        return vminq_s32(vmaxq_s32(x, vdupq_n_s32(0)), vdupq_n_s32(n - 1));
    case L_FLIP:
        x = vandq_s32(x, vdupq_n_s32(2 * n - 1));
        return vbslq_s32(vcltq_s32(x, vdupq_n_s32(n)), x, vsubq_s32(vdupq_n_s32(2 * n - 1), x));
    default:
        return vandq_s32(x, vdupq_n_s32(n - 1));
    }
}
#elif defined(RD_SSE2)
PIXEL_STEP __m128i wrap4(__m128i x, int n, int mode) {
    switch (mode) {
    case L_CLAMP:
        x = _mm_andnot_si128(_mm_srai_epi32(x, 31), x);
        return sel128(_mm_cmpgt_epi32(x, _mm_set1_epi32(n - 1)), _mm_set1_epi32(n - 1), x);
    case L_FLIP:
        x = _mm_and_si128(x, _mm_set1_epi32(2 * n - 1));
        return sel128(_mm_cmplt_epi32(x, _mm_set1_epi32(n)), x, _mm_sub_epi32(_mm_set1_epi32(2 * n - 1), x));
    default:
        return _mm_and_si128(x, _mm_set1_epi32(n - 1));
    }
}

/* 4 bytes from the low ones of 4 lanes, each 0 to 255 */
PIXEL_STEP void store4_u8(uint8_t* o, __m128i k) {
    int n;
    k = _mm_packs_epi32(k, k);
    n = _mm_cvtsi128_si32(_mm_packus_epi16(k, k));
    memcpy(o, &n, 4);
}
#endif

/* the texels' places and weights for count pixels */
static void lite_places(const SETUP* p, LBUF* b, int count, int w, int h) {
    const int uv = p->s->uv_mode, bil = p->kind >= 6;
    const int mu = uv & 2 ? L_CLAMP : uv & 8 ? L_FLIP : L_REPEAT, mv = uv & 1 ? L_CLAMP : uv & 4 ? L_FLIP : L_REPEAT;
    int j;
    for (j = 0; j < count; j += 4) {
        const int32_t *sk = b->s[j / LSEG], *dk = b->d[j / LSEG];
#if defined(RD_NEON)
        const int32x4_t at = vaddq_s32(vdupq_n_s32(j % LSEG), (int32x4_t){ 0, 1, 2, 3 });
        int32x4_t u = vmlaq_s32(vld1q_dup_s32(&sk[0]), at, vld1q_dup_s32(&dk[0]));
        int32x4_t v = vmlaq_s32(vld1q_dup_s32(&sk[1]), at, vld1q_dup_s32(&dk[1]));
        int32x4_t x0 = vshrq_n_s32(u, 16), y0 = vshrq_n_s32(v, 16), wv = vdupq_n_s32(w);
        if (bil) {
            int32x4_t x1 = wrap4(vaddq_s32(x0, vdupq_n_s32(1)), w, mu), y1 = vmulq_s32(wrap4(vaddq_s32(y0, vdupq_n_s32(1)), h, mv), wv);
            uint16x4_t f;
            x0 = wrap4(x0, w, mu), y0 = vmulq_s32(wrap4(y0, h, mv), wv);
            vst1q_s32(&b->o[0][j], vaddq_s32(y0, x0));
            vst1q_s32(&b->o[1][j], vaddq_s32(y0, x1));
            vst1q_s32(&b->o[2][j], vaddq_s32(y1, x0));
            vst1q_s32(&b->o[3][j], vaddq_s32(y1, x1));
            f = vmovn_u32(vandq_u32(vreinterpretq_u32_s32(vshrq_n_s32(u, 8)), vdupq_n_u32(255)));
            vst1_lane_u32((uint32_t*)&b->fx[j], vreinterpret_u32_u8(vmovn_u16(vcombine_u16(f, f))), 0);
            f = vmovn_u32(vandq_u32(vreinterpretq_u32_s32(vshrq_n_s32(v, 8)), vdupq_n_u32(255)));
            vst1_lane_u32((uint32_t*)&b->fy[j], vreinterpret_u32_u8(vmovn_u16(vcombine_u16(f, f))), 0);
        } else {
            vst1q_s32(&b->o[0][j], vmlaq_s32(wrap4(x0, w, mu), wrap4(y0, h, mv), wv));
        }
#elif defined(RD_SSE2)
        /* a row and the width both under 2^15: a madd lane multiplies them */
        __m128i u = plane4(sk[0], dk[0], j % LSEG), v = plane4(sk[1], dk[1], j % LSEG);
        __m128i x0 = _mm_srai_epi32(u, 16), y0 = _mm_srai_epi32(v, 16), wv = _mm_set1_epi32(w);
        if (bil) {
            const __m128i one = _mm_set1_epi32(1), m8 = _mm_set1_epi32(255);
            __m128i x1 = wrap4(_mm_add_epi32(x0, one), w, mu), y1 = _mm_madd_epi16(wrap4(_mm_add_epi32(y0, one), h, mv), wv);
            x0 = wrap4(x0, w, mu), y0 = _mm_madd_epi16(wrap4(y0, h, mv), wv);
            _mm_storeu_si128((__m128i*)&b->o[0][j], _mm_add_epi32(y0, x0));
            _mm_storeu_si128((__m128i*)&b->o[1][j], _mm_add_epi32(y0, x1));
            _mm_storeu_si128((__m128i*)&b->o[2][j], _mm_add_epi32(y1, x0));
            _mm_storeu_si128((__m128i*)&b->o[3][j], _mm_add_epi32(y1, x1));
            store4_u8(&b->fx[j], _mm_and_si128(_mm_srai_epi32(u, 8), m8));
            store4_u8(&b->fy[j], _mm_and_si128(_mm_srai_epi32(v, 8), m8));
        } else {
            _mm_storeu_si128((__m128i*)&b->o[0][j], _mm_add_epi32(wrap4(x0, w, mu), _mm_madd_epi16(wrap4(y0, h, mv), wv)));
        }
#else
        int i;
        for (i = 0; i < 4; i++) {
            int at = j % LSEG + i, u = sk[0] + dk[0] * at, v = sk[1] + dk[1] * at, x0 = u >> 16, y0 = v >> 16;
            if (bil) {
                int x1 = wrap(x0 + 1, w, mu == L_CLAMP, mu == L_FLIP), y1 = wrap(y0 + 1, h, mv == L_CLAMP, mv == L_FLIP) * w;
                x0 = wrap(x0, w, mu == L_CLAMP, mu == L_FLIP), y0 = wrap(y0, h, mv == L_CLAMP, mv == L_FLIP) * w;
                b->o[0][j + i] = y0 + x0, b->o[1][j + i] = y0 + x1, b->o[2][j + i] = y1 + x0, b->o[3][j + i] = y1 + x1;
                b->fx[j + i] = (uint8_t)(u >> 8 & 255), b->fy[j + i] = (uint8_t)(v >> 8 & 255);
            } else {
                b->o[0][j + i] = wrap(y0, h, mv == L_CLAMP, mv == L_FLIP) * w + wrap(x0, w, mu == L_CLAMP, mu == L_FLIP);
            }
        }
#endif
    }
}

#if defined(RD_NEON)
/* ifloor's, 4 at a time */
PIXEL_STEP int32x4_t ifloor4(float32x4_t f) {
    uint32x4_t in = vandq_u32(vcgeq_f32(f, vdupq_n_f32(-2147483648.0f)), vcltq_f32(f, vdupq_n_f32(2147483648.0f)));
    int32x4_t t = vbslq_s32(in, vcvtq_s32_f32(f), vdupq_n_s32(INT_MIN));
    return vaddq_s32(t, vreinterpretq_s32_u32(vandq_u32(vcltq_f32(f, vcvtq_f32_s32(t)), in)));
}
#elif defined(RD_SSE2)
PIXEL_STEP __m128i ifloor4(__m128 f) {
    __m128i t = _mm_cvttps_epi32(f);
    __m128i m = _mm_andnot_si128(_mm_cmpeq_epi32(t, _mm_set1_epi32(INT_MIN)), _mm_castps_si128(_mm_cmplt_ps(f, _mm_cvtepi32_ps(t))));
    return _mm_add_epi32(t, m);
}
#endif

/* nearest: the texels' places as the exact span takes them, its sums from the row's start xs, so the sprites' texels
   are the exact ones */
static void lite_places_exact(const SETUP* p, LBUF* b, int y, int xs, int first, int count, int w, int h) {
    const int uv = p->s->uv_mode;
    double fz = p->at[0] + p->dx[0] * xs + p->dy[0] * y, dz = p->dx[0];
    double fu = p->at[1] + p->dx[1] * xs + p->dy[1] * y, du = p->dx[1];
    double fv = p->at[2] + p->dx[2] * xs + p->dy[2] * y, dv = p->dx[2];
    const int zconst = dz == 0.0;
    float rz0 = 1.0f;
    int x, j;
    if (zconst && (float)fz > 0.0f) {
        rz0 = 1.0f / (float)fz;
    }
    for (x = xs; x < first + count; x++) {
        float z, rz;
        if (x > xs) {
            fz += dz, fu += du, fv += dv;
        }
        if (x < first) {
            continue;
        }
        z = (float)fz;
        rz = zconst ? rz0 : 1.0f / z;
        b->fu[x - first] = (float)(fu * rz) * w;
        b->fv[x - first] = (float)(fv * rz) * h;
    }
#if defined(RD_NEON)
    {
        const int mu = uv & 2 ? L_CLAMP : uv & 8 ? L_FLIP : L_REPEAT, mv = uv & 1 ? L_CLAMP : uv & 4 ? L_FLIP : L_REPEAT;
        for (j = 0; j < count; j += 4) {
            int32x4_t xi = wrap4(ifloor4(vld1q_f32(&b->fu[j])), w, mu), yi = wrap4(ifloor4(vld1q_f32(&b->fv[j])), h, mv);
            vst1q_s32(&b->o[0][j], vmlaq_s32(xi, yi, vdupq_n_s32(w)));
        }
    }
#elif defined(RD_SSE2)
    {
        const int mu = uv & 2 ? L_CLAMP : uv & 8 ? L_FLIP : L_REPEAT, mv = uv & 1 ? L_CLAMP : uv & 4 ? L_FLIP : L_REPEAT;
        for (j = 0; j < count; j += 4) {
            __m128i xi = wrap4(ifloor4(_mm_loadu_ps(&b->fu[j])), w, mu), yi = wrap4(ifloor4(_mm_loadu_ps(&b->fv[j])), h, mv);
            _mm_storeu_si128((__m128i*)&b->o[0][j], _mm_add_epi32(xi, _mm_madd_epi16(yi, _mm_set1_epi32(w))));
        }
    }
#else
    for (j = 0; j < count; j++) {
        b->o[0][j] = wrap(ifloor(b->fv[j]), h, uv & 1, uv & 4) * w + wrap(ifloor(b->fu[j]), w, uv & 2, uv & 8);
    }
#endif
}

/* the texels at the places */
static void lite_gather(const SETUP* p, LBUF* b, int count, const void* d) {
    const RD_STATE* s = p->s;
    int c, j, corners = p->kind >= 6 ? 4 : 1;
    if (s->tex->format == RD_PAL4 || s->tex->format == RD_PAL8) {
        const uint8_t* d8 = d;
        for (c = 0; c < corners; c++) {
            const int32_t* o = b->o[c];
            uint32_t* t = b->t32[c];
            for (j = 0; j < count; j++) {
                t[j] = rd_palette[(s->palette + d8[o[j]]) & 1023];
            }
        }
        return;
    }
    for (c = 0; c < corners; c++) {
        const int32_t* o = b->o[c];
        const uint16_t* d16 = d;
        uint16_t* t = b->t16[c];
        for (j = 0; j < count; j++) {
            t[j] = d16[o[j]];
        }
    }
}

#if defined(RD_NEON)
typedef struct {
    uint8x8_t a, r, g, b;
} LCOL;

/* 8 of a corner's texels as channels */
PIXEL_STEP LCOL lite_texels(const LBUF* b, int c, int j, int format, int pal) {
    LCOL k;
    if (pal) {
        uint8x8x4_t q = vld4_u8((const uint8_t*)&b->t32[c][j]);
        k.b = q.val[0], k.g = q.val[1], k.r = q.val[2], k.a = q.val[3];
        return k;
    }
    {
        uint16x8_t t = vld1q_u16(&b->t16[c][j]);
        const uint8x8x4_t t5 = { { vld1_u8(c5), vld1_u8(c5 + 8), vld1_u8(c5 + 16), vld1_u8(c5 + 24) } };
        const uint8x8_t m5 = vdup_n_u8(31);
        switch (format) {
        case RD_ARGB1555:
            k.a = vreinterpret_u8_s8(vneg_s8(vreinterpret_s8_u8(vmovn_u16(vshrq_n_u16(t, 15)))));
            k.r = vtbl4_u8(t5, vand_u8(vmovn_u16(vshrq_n_u16(t, 10)), m5));
            k.g = vtbl4_u8(t5, vand_u8(vmovn_u16(vshrq_n_u16(t, 5)), m5));
            k.b = vtbl4_u8(t5, vand_u8(vmovn_u16(t), m5));
            break;
        case RD_RGB565: {
            const uint8x8x4_t t6a = { { vld1_u8(c6), vld1_u8(c6 + 8), vld1_u8(c6 + 16), vld1_u8(c6 + 24) } };
            const uint8x8x4_t t6b = { { vld1_u8(c6 + 32), vld1_u8(c6 + 40), vld1_u8(c6 + 48), vld1_u8(c6 + 56) } };
            uint8x8_t i6 = vand_u8(vmovn_u16(vshrq_n_u16(t, 5)), vdup_n_u8(63));
            k.a = vdup_n_u8(255);
            k.r = vtbl4_u8(t5, vmovn_u16(vshrq_n_u16(t, 11)));
            k.g = vtbx4_u8(vtbl4_u8(t6a, i6), t6b, vsub_u8(i6, vdup_n_u8(32)));
            k.b = vtbl4_u8(t5, vand_u8(vmovn_u16(t), m5));
            break;
        }
        default: {
            const uint8x8_t m4 = vdup_n_u8(15), x17 = vdup_n_u8(17);
            k.a = vmul_u8(vmovn_u16(vshrq_n_u16(t, 12)), x17);
            k.r = vmul_u8(vand_u8(vmovn_u16(vshrq_n_u16(t, 8)), m4), x17);
            k.g = vmul_u8(vand_u8(vmovn_u16(vshrq_n_u16(t, 4)), m4), x17);
            k.b = vmul_u8(vand_u8(vmovn_u16(t), m4), x17);
            break;
        }
        }
    }
    return k;
}

/* (a (256 - f) + b f) / 256, rounded or down: all of it fits 16 bits */
PIXEL_STEP uint8x8_t lerp8x8(uint8x8_t a, uint8x8_t b, uint8x8_t f, const int round) {
    uint16x8_t k = vmlsl_u8(vmlal_u8(vshll_n_u8(a, 8), b, f), a, f);
    return round ? vrshrn_n_u16(k, 8) : vshrn_n_u16(k, 8);
}

/* rounded across, down after: near the one truncation of the exact mix */
PIXEL_STEP LCOL lerp_col(LCOL a, LCOL b, uint8x8_t f, const int round) {
    LCOL k;
    k.a = lerp8x8(a.a, b.a, f, round), k.r = lerp8x8(a.r, b.r, f, round);
    k.g = lerp8x8(a.g, b.g, f, round), k.b = lerp8x8(a.b, b.b, f, round);
    return k;
}

/* x / 255 rounded down, for x from 0 to 65534 */
PIXEL_STEP uint8x8_t div255x8(uint16x8_t x) {
    return vshrn_n_u16(vaddq_u16(vaddq_u16(x, vshrq_n_u16(x, 8)), vdupq_n_u16(1)), 8);
}

/* plane i at 8 pixels from j as 0 to 255 */
PIXEL_STEP uint8x8_t lite_channel(const LBUF* b, int j, int i) {
    const int32_t *sk = &b->s[j / LSEG][i], *dk = &b->d[j / LSEG][i];
    const int32x4_t at = vaddq_s32(vdupq_n_s32(j % LSEG), (int32x4_t){ 0, 1, 2, 3 });
    int32x4_t lo = vmlaq_s32(vld1q_dup_s32(sk), at, vld1q_dup_s32(dk));
    int32x4_t hi = vmlaq_s32(vld1q_dup_s32(sk), vaddq_s32(at, vdupq_n_s32(4)), vld1q_dup_s32(dk));
    return vqmovn_u16(vcombine_u16(vqshrun_n_s32(lo, 16), vqshrun_n_s32(hi, 16)));
}
#elif defined(RD_SSE2)
/* 8 pixels' channels, a 16-bit lane each */
typedef struct {
    __m128i a, r, g, b;
} LCOL;

/* c5's and c6's: i 255 / 31 and i 255 / 63 rounded down, by multiplying */
PIXEL_STEP __m128i c5x8(__m128i i) {
    return _mm_srli_epi16(_mm_mulhi_epu16(_mm_mullo_epi16(i, _mm_set1_epi16(255)), _mm_set1_epi16(8457)), 2);
}

PIXEL_STEP __m128i c6x8(__m128i i) {
    return _mm_srli_epi16(_mm_mulhi_epu16(_mm_mullo_epi16(i, _mm_set1_epi16(255)), _mm_set1_epi16(16645)), 4);
}

/* 8 of a corner's texels as channels */
PIXEL_STEP LCOL lite_texels(const LBUF* b, int c, int j, int format, int pal) {
    LCOL k;
    if (pal) {
        const __m128i m8 = _mm_set1_epi32(255);
        __m128i lo = _mm_loadu_si128((const __m128i*)&b->t32[c][j]), hi = _mm_loadu_si128((const __m128i*)&b->t32[c][j + 4]);
        k.a = _mm_packs_epi32(_mm_srli_epi32(lo, 24), _mm_srli_epi32(hi, 24));
        k.r = _mm_packs_epi32(_mm_and_si128(_mm_srli_epi32(lo, 16), m8), _mm_and_si128(_mm_srli_epi32(hi, 16), m8));
        k.g = _mm_packs_epi32(_mm_and_si128(_mm_srli_epi32(lo, 8), m8), _mm_and_si128(_mm_srli_epi32(hi, 8), m8));
        k.b = _mm_packs_epi32(_mm_and_si128(lo, m8), _mm_and_si128(hi, m8));
        return k;
    }
    {
        __m128i t = _mm_loadu_si128((const __m128i*)&b->t16[c][j]);
        const __m128i m5 = _mm_set1_epi16(31), m8 = _mm_set1_epi16(255);
        switch (format) {
        case RD_ARGB1555:
            k.a = _mm_and_si128(_mm_srai_epi16(t, 15), m8);
            k.r = c5x8(_mm_and_si128(_mm_srli_epi16(t, 10), m5));
            k.g = c5x8(_mm_and_si128(_mm_srli_epi16(t, 5), m5));
            k.b = c5x8(_mm_and_si128(t, m5));
            break;
        case RD_RGB565:
            k.a = m8;
            k.r = c5x8(_mm_srli_epi16(t, 11));
            k.g = c6x8(_mm_and_si128(_mm_srli_epi16(t, 5), _mm_set1_epi16(63)));
            k.b = c5x8(_mm_and_si128(t, m5));
            break;
        default: {
            const __m128i m4 = _mm_set1_epi16(15), x17 = _mm_set1_epi16(17);
            k.a = _mm_mullo_epi16(_mm_srli_epi16(t, 12), x17);
            k.r = _mm_mullo_epi16(_mm_and_si128(_mm_srli_epi16(t, 8), m4), x17);
            k.g = _mm_mullo_epi16(_mm_and_si128(_mm_srli_epi16(t, 4), m4), x17);
            k.b = _mm_mullo_epi16(_mm_and_si128(t, m4), x17);
            break;
        }
        }
    }
    return k;
}

/* (a (256 - f) + b f) / 256, rounded or down: all of it fits 16 bits */
PIXEL_STEP __m128i lerp16(__m128i a, __m128i b, __m128i f, const int round) {
    __m128i k = _mm_add_epi16(_mm_mullo_epi16(a, _mm_sub_epi16(_mm_set1_epi16(256), f)), _mm_mullo_epi16(b, f));
    return _mm_srli_epi16(round ? _mm_add_epi16(k, _mm_set1_epi16(128)) : k, 8);
}

/* rounded across, down after: near the one truncation of the exact mix */
PIXEL_STEP LCOL lerp_col(LCOL a, LCOL b, __m128i f, const int round) {
    LCOL k;
    k.a = lerp16(a.a, b.a, f, round), k.r = lerp16(a.r, b.r, f, round);
    k.g = lerp16(a.g, b.g, f, round), k.b = lerp16(a.b, b.b, f, round);
    return k;
}

/* x / 255 rounded down, for x from 0 to 65025 */
PIXEL_STEP __m128i div255x8(__m128i x) {
    return _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(x, _mm_srli_epi16(x, 8)), _mm_set1_epi16(1)), 8);
}

/* (s + d at) >> 16 for 8 pixels from at, held to 0 to hi */
PIXEL_STEP __m128i plane8(int32_t s, int32_t d, int at, int hi) {
    __m128i k = _mm_packs_epi32(_mm_srai_epi32(plane4(s, d, at), 16), _mm_srai_epi32(plane4(s, d, at + 4), 16));
    return _mm_min_epi16(_mm_max_epi16(k, _mm_setzero_si128()), _mm_set1_epi16((short)hi));
}

/* plane i at 8 pixels from j as 0 to 255 */
PIXEL_STEP __m128i lite_channel(const LBUF* b, int j, int i) {
    return plane8(b->s[j / LSEG][i], b->d[j / LSEG][i], j % LSEG, 255);
}

/* 8 bytes as 16-bit lanes */
PIXEL_STEP __m128i load8_u8(const uint8_t* p) {
    return _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i*)p), _mm_setzero_si128());
}
#endif

/* the colours of count pixels */
static void lite_colours(const SETUP* p, LBUF* b, int count) {
    const RD_STATE* s = p->s;
    const int tex = s->tex != NULL, bil = p->kind >= 6, flat = p->flat, fogged = p->fogged;
    const int pal = tex && (s->tex->format == RD_PAL4 || s->tex->format == RD_PAL8), format = tex ? s->tex->format : 0;
    int j;
#if defined(RD_NEON)
    for (j = 0; j < count; j += 8) {
        LCOL c, t;
        if (flat) {
            c.a = vdup_n_u8((uint8_t)p->fbase.a), c.r = vdup_n_u8((uint8_t)p->fbase.r);
            c.g = vdup_n_u8((uint8_t)p->fbase.g), c.b = vdup_n_u8((uint8_t)p->fbase.b);
        } else {
            c.a = s->use_alpha ? lite_channel(b, j, 2) : vdup_n_u8(255);
            c.r = lite_channel(b, j, 3), c.g = lite_channel(b, j, 4), c.b = lite_channel(b, j, 5);
        }
        if (tex) {
            t = lite_texels(b, 0, j, format, pal);
            if (bil) {
                uint8x8_t fx = vld1_u8(&b->fx[j]);
                t = lerp_col(lerp_col(t, lite_texels(b, 1, j, format, pal), fx, 1),
                             lerp_col(lite_texels(b, 2, j, format, pal), lite_texels(b, 3, j, format, pal), fx, 1),
                             vld1_u8(&b->fy[j]), 0);
            }
            if (!s->tex_alpha) {
                t.a = vdup_n_u8(255);
            }
            switch (s->shading) {
            case RD_DECAL:
                c = t;
                break;
            case RD_MODULATE:
                c.r = div255x8(vmull_u8(t.r, c.r)), c.g = div255x8(vmull_u8(t.g, c.g)), c.b = div255x8(vmull_u8(t.b, c.b));
                c.a = t.a;
                break;
            case RD_DECAL_ALPHA: {
                uint8x8_t na = vmvn_u8(t.a);
                c.r = div255x8(vmlal_u8(vmull_u8(t.r, t.a), c.r, na));
                c.g = div255x8(vmlal_u8(vmull_u8(t.g, t.a), c.g, na));
                c.b = div255x8(vmlal_u8(vmull_u8(t.b, t.a), c.b, na));
                break;
            }
            default:
                c.r = div255x8(vmull_u8(t.r, c.r)), c.g = div255x8(vmull_u8(t.g, c.g)), c.b = div255x8(vmull_u8(t.b, c.b));
                c.a = div255x8(vmull_u8(t.a, c.a));
                break;
            }
        }
        if (s->offset) {
            if (flat) {
                c.r = vqadd_u8(c.r, vdup_n_u8((uint8_t)p->foffs.r)), c.g = vqadd_u8(c.g, vdup_n_u8((uint8_t)p->foffs.g));
                c.b = vqadd_u8(c.b, vdup_n_u8((uint8_t)p->foffs.b));
            } else {
                c.r = vqadd_u8(c.r, lite_channel(b, j, 7)), c.g = vqadd_u8(c.g, lite_channel(b, j, 8));
                c.b = vqadd_u8(c.b, lite_channel(b, j, 9));
            }
        }
        if (fogged) {
            const int32_t *fs = &b->fs[j / LSEG], *fd = &b->fd[j / LSEG];
            const int32x4_t at = vaddq_s32(vdupq_n_s32(j % LSEG), (int32x4_t){ 0, 1, 2, 3 });
            int32x4_t lo = vshrq_n_s32(vmlaq_s32(vld1q_dup_s32(fs), at, vld1q_dup_s32(fd)), 16);
            int32x4_t hi = vshrq_n_s32(vmlaq_s32(vld1q_dup_s32(fs), vaddq_s32(at, vdupq_n_s32(4)), vld1q_dup_s32(fd)), 16);
            uint16x8_t g = vcombine_u16(vqmovun_s32(lo), vqmovun_s32(hi));
            g = vminq_u16(g, vdupq_n_u16(256));
            if (s->fog == RD_FOG_TABLE2) {
                c.a = vshrn_n_u16(vaddq_u16(vmulq_n_u16(g, 255), vdupq_n_u16(128)), 8);
                c.r = vdup_n_u8((uint8_t)p->fogc.r), c.g = vdup_n_u8((uint8_t)p->fogc.g), c.b = vdup_n_u8((uint8_t)p->fogc.b);
            } else {
                uint16x8_t ng = vsubq_u16(vdupq_n_u16(256), g), k = vdupq_n_u16(128);
                c.r = vshrn_n_u16(vaddq_u16(vmlaq_u16(vmulq_u16(vmovl_u8(c.r), ng), g, vdupq_n_u16((uint16_t)p->fogc.r)), k), 8);
                c.g = vshrn_n_u16(vaddq_u16(vmlaq_u16(vmulq_u16(vmovl_u8(c.g), ng), g, vdupq_n_u16((uint16_t)p->fogc.g)), k), 8);
                c.b = vshrn_n_u16(vaddq_u16(vmlaq_u16(vmulq_u16(vmovl_u8(c.b), ng), g, vdupq_n_u16((uint16_t)p->fogc.b)), k), 8);
            }
        }
        {
            uint8x8x4_t q = { { c.b, c.g, c.r, c.a } };
            vst4_u8((uint8_t*)&b->col[j], q);
        }
    }
#elif defined(RD_SSE2)
    for (j = 0; j < count; j += 8) {
        const __m128i k255 = _mm_set1_epi16(255);
        LCOL c, t;
        if (flat) {
            c.a = _mm_set1_epi16((short)p->fbase.a), c.r = _mm_set1_epi16((short)p->fbase.r);
            c.g = _mm_set1_epi16((short)p->fbase.g), c.b = _mm_set1_epi16((short)p->fbase.b);
        } else {
            c.a = s->use_alpha ? lite_channel(b, j, 2) : k255;
            c.r = lite_channel(b, j, 3), c.g = lite_channel(b, j, 4), c.b = lite_channel(b, j, 5);
        }
        if (tex) {
            t = lite_texels(b, 0, j, format, pal);
            if (bil) {
                __m128i fx = load8_u8(&b->fx[j]);
                t = lerp_col(lerp_col(t, lite_texels(b, 1, j, format, pal), fx, 1),
                             lerp_col(lite_texels(b, 2, j, format, pal), lite_texels(b, 3, j, format, pal), fx, 1),
                             load8_u8(&b->fy[j]), 0);
            }
            if (!s->tex_alpha) {
                t.a = k255;
            }
            switch (s->shading) {
            case RD_DECAL:
                c = t;
                break;
            case RD_MODULATE:
                c.r = div255x8(_mm_mullo_epi16(t.r, c.r)), c.g = div255x8(_mm_mullo_epi16(t.g, c.g));
                c.b = div255x8(_mm_mullo_epi16(t.b, c.b)), c.a = t.a;
                break;
            case RD_DECAL_ALPHA: {
                __m128i na = _mm_sub_epi16(k255, t.a);
                c.r = div255x8(_mm_add_epi16(_mm_mullo_epi16(t.r, t.a), _mm_mullo_epi16(c.r, na)));
                c.g = div255x8(_mm_add_epi16(_mm_mullo_epi16(t.g, t.a), _mm_mullo_epi16(c.g, na)));
                c.b = div255x8(_mm_add_epi16(_mm_mullo_epi16(t.b, t.a), _mm_mullo_epi16(c.b, na)));
                break;
            }
            default:
                c.r = div255x8(_mm_mullo_epi16(t.r, c.r)), c.g = div255x8(_mm_mullo_epi16(t.g, c.g));
                c.b = div255x8(_mm_mullo_epi16(t.b, c.b)), c.a = div255x8(_mm_mullo_epi16(t.a, c.a));
                break;
            }
        }
        if (s->offset) {
            if (flat) {
                c.r = _mm_add_epi16(c.r, _mm_set1_epi16((short)p->foffs.r));
                c.g = _mm_add_epi16(c.g, _mm_set1_epi16((short)p->foffs.g));
                c.b = _mm_add_epi16(c.b, _mm_set1_epi16((short)p->foffs.b));
            } else {
                c.r = _mm_add_epi16(c.r, lite_channel(b, j, 7)), c.g = _mm_add_epi16(c.g, lite_channel(b, j, 8));
                c.b = _mm_add_epi16(c.b, lite_channel(b, j, 9));
            }
            c.r = _mm_min_epi16(c.r, k255), c.g = _mm_min_epi16(c.g, k255), c.b = _mm_min_epi16(c.b, k255);
        }
        if (fogged) {
            __m128i g = plane8(b->fs[j / LSEG], b->fd[j / LSEG], j % LSEG, 256), k = _mm_set1_epi16(128);
            if (s->fog == RD_FOG_TABLE2) {
                c.a = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(g, k255), k), 8);
                c.r = _mm_set1_epi16((short)p->fogc.r), c.g = _mm_set1_epi16((short)p->fogc.g);
                c.b = _mm_set1_epi16((short)p->fogc.b);
            } else {
                __m128i ng = _mm_sub_epi16(_mm_set1_epi16(256), g);
                c.r = _mm_add_epi16(_mm_mullo_epi16(c.r, ng), _mm_mullo_epi16(g, _mm_set1_epi16((short)p->fogc.r)));
                c.g = _mm_add_epi16(_mm_mullo_epi16(c.g, ng), _mm_mullo_epi16(g, _mm_set1_epi16((short)p->fogc.g)));
                c.b = _mm_add_epi16(_mm_mullo_epi16(c.b, ng), _mm_mullo_epi16(g, _mm_set1_epi16((short)p->fogc.b)));
                c.r = _mm_srli_epi16(_mm_add_epi16(c.r, k), 8), c.g = _mm_srli_epi16(_mm_add_epi16(c.g, k), 8);
                c.b = _mm_srli_epi16(_mm_add_epi16(c.b, k), 8);
            }
        }
        {
            __m128i bg = _mm_or_si128(c.b, _mm_slli_epi16(c.g, 8)), ra = _mm_or_si128(c.r, _mm_slli_epi16(c.a, 8));
            _mm_storeu_si128((__m128i*)&b->col[j], _mm_unpacklo_epi16(bg, ra));
            _mm_storeu_si128((__m128i*)&b->col[j + 4], _mm_unpackhi_epi16(bg, ra));
        }
    }
#else
    for (j = 0; j < count; j++) {
        const int32_t *sk = b->s[j / LSEG], *dk = b->d[j / LSEG];
        int at = j % LSEG, ca, cr, cg, cb;
        if (flat) {
            ca = p->fbase.a, cr = p->fbase.r, cg = p->fbase.g, cb = p->fbase.b;
        } else {
            ca = s->use_alpha ? clamp255((sk[2] + dk[2] * at) >> 16) : 255;
            cr = clamp255((sk[3] + dk[3] * at) >> 16);
            cg = clamp255((sk[4] + dk[4] * at) >> 16);
            cb = clamp255((sk[5] + dk[5] * at) >> 16);
        }
        if (tex) {
            uint32_t k[4];
            int ta, tr, tg, tb, i, n = bil ? 4 : 1;
            for (i = 0; i < n; i++) {
                k[i] = pal ? b->t32[i][j] : texel16(format, b->t16[i][j]);
            }
            if (bil) {
                int fx = b->fx[j], fy = b->fy[j], sh;
                uint32_t m = 0;
                for (sh = 0; sh < 32; sh += 8) {
                    int c00 = k[0] >> sh & 255, c01 = k[1] >> sh & 255, c10 = k[2] >> sh & 255, c11 = k[3] >> sh & 255;
                    int top = (c00 * (256 - fx) + c01 * fx + 128) >> 8, bottom = (c10 * (256 - fx) + c11 * fx + 128) >> 8;
                    m |= (uint32_t)((top * (256 - fy) + bottom * fy) >> 8) << sh;
                }
                k[0] = m;
            }
            ta = s->tex_alpha ? (int)(k[0] >> 24) : 255;
            tr = (int)(k[0] >> 16 & 255), tg = (int)(k[0] >> 8 & 255), tb = (int)(k[0] & 255);
            switch (s->shading) {
            case RD_DECAL:
                ca = ta, cr = tr, cg = tg, cb = tb;
                break;
            case RD_MODULATE:
                cr = div255(tr * cr), cg = div255(tg * cg), cb = div255(tb * cb), ca = ta;
                break;
            case RD_DECAL_ALPHA:
                cr = div255(tr * ta + cr * (255 - ta));
                cg = div255(tg * ta + cg * (255 - ta));
                cb = div255(tb * ta + cb * (255 - ta));
                break;
            default:
                cr = div255(tr * cr), cg = div255(tg * cg), cb = div255(tb * cb), ca = div255(ta * ca);
                break;
            }
        }
        if (s->offset) {
            if (flat) {
                cr = clamp255(cr + p->foffs.r), cg = clamp255(cg + p->foffs.g), cb = clamp255(cb + p->foffs.b);
            } else {
                cr = clamp255(cr + clamp255((sk[7] + dk[7] * at) >> 16));
                cg = clamp255(cg + clamp255((sk[8] + dk[8] * at) >> 16));
                cb = clamp255(cb + clamp255((sk[9] + dk[9] * at) >> 16));
            }
        }
        if (fogged) {
            int g = (b->fs[j / LSEG] + b->fd[j / LSEG] * at) >> 16;
            g = g < 0 ? 0 : g > 256 ? 256 : g;
            if (s->fog == RD_FOG_TABLE2) {
                ca = (g * 255 + 128) >> 8, cr = p->fogc.r, cg = p->fogc.g, cb = p->fogc.b;
            } else {
                cr = (cr * (256 - g) + p->fogc.r * g + 128) >> 8;
                cg = (cg * (256 - g) + p->fogc.g * g + 128) >> 8;
                cb = (cb * (256 - g) + p->fogc.b * g + 128) >> 8;
            }
        }
        b->col[j] = (uint32_t)ca << 24 | (uint32_t)cr << 16 | (uint32_t)cg << 8 | (uint32_t)cb;
    }
#endif
}

/* the lite span: as span's, but near */
static void lite_span(const SETUP* p, WORKER* wk, int y, int xs, int xe, int id, int mode) {
    const RD_STATE* s = p->s;
    const int list = p->list;
    LBUF* b = wk->lb;
    float* zrow = &zbuf[y * RD_WIDTH];
    uint32_t* crow = &cbuf[y * RD_WIDTH];
    const int* win = &wk->win[(y - wk->band_y) * RD_WIDTH];
    int first = xs, last = xe - 1, count, j;
    if (mode == DRAW) {
        double fz = p->at[0] + p->dx[0] * xs + p->dy[0] * y;
        if (!depth_run(p, wk, y, xs, xe, id, fz, 0, &first, &last)) {
            return;
        }
    }
    count = (last - first + 8) & ~7;
    lite_points(p, b, first, last, y);
    if (s->tex != NULL) {
        int w, h;
        const void* d = level_texels(s->tex, p->level, &w, &h);
        if (p->kind >= 6) {
            lite_places(p, b, count, w, h);
        } else {
            lite_places_exact(p, b, y, xs, first, count, w, h);
        }
        lite_gather(p, b, count, d);
    }
    lite_colours(p, b, count);
#if defined(RD_NEON)
    if (mode == SHADE_WON && last - first >= 3) {
        const int32x4_t idv = vdupq_n_s32(id);
        int x = first;
        /* the last four end at last, again over some: the same again */
        for (;;) {
            uint32x4_t m = vceqq_s32(vld1q_s32(&win[x]), idv);
            vst1q_u32(&crow[x], vbslq_u32(m, vld1q_u32(&b->col[x - first]), vld1q_u32(&crow[x])));
            if (x + 4 > last) {
                break;
            }
            x = x + 8 <= last + 1 ? x + 4 : last - 3;
        }
        return;
    }
#elif defined(RD_SSE2)
    if (mode == SHADE_WON && last - first >= 3) {
        const __m128i idv = _mm_set1_epi32(id);
        int x = first;
        for (;;) {
            __m128i m = _mm_cmpeq_epi32(_mm_loadu_si128((const __m128i*)&win[x]), idv);
            _mm_storeu_si128((__m128i*)&crow[x], sel128(m, _mm_loadu_si128((const __m128i*)&b->col[x - first]),
                                                      _mm_loadu_si128((const __m128i*)&crow[x])));
            if (x + 4 > last) {
                break;
            }
            x = x + 8 <= last + 1 ? x + 4 : last - 3;
        }
        return;
    }
#endif
    for (j = 0; j <= last - first; j++) {
        int x = first + j;
        uint32_t k = b->col[j];
        if (mode == SHADE_WON) {
            if (win[x] == id) {
                crow[x] = k;
            }
            continue;
        }
        if (!wk->lok[x] || ((k >> 24) == 0 && p->clear0)) {
            continue;
        }
        if (list == RD_TRANS && !nosort) {
            add_frag(wk, y * RD_WIDTH + x, wk->lz[x], k, p->order);
            continue;
        }
        if (list == RD_PUNCH && (k >> 24) < 128) {
            continue;
        }
        crow[x] = list == RD_OPAQUE ? k : blend(s, unpack(k), crow[x]);
        if (p->zwrite) {
            zrow[x] = wk->lz[x];
        }
    }
}

/* rows ya to yb - 1 of triangle id; its spans kept in sp by the depth pass for the shading one, each to the last pixel
   it took */
static void raster(const SETUP* p, WORKER* wk, int ya, int yb, int mode, int id, int16_t (*sp)[2]) {
    int y, y0 = ya;
    if (ya < p->y0) {
        ya = p->y0;
    }
    if (yb > p->y1) {
        yb = p->y1;
    }
    for (y = ya; y < yb; y++) {
        int xs = 0, xe = 0;
        if (!row_on(y)) {
            continue;
        }
        if (mode == SHADE_WON) {
            xs = sp[y - y0][0], xe = sp[y - y0][1];
        } else if (!(lite ? lite_row(p, y, &xs, &xe) : row_span(p, y, &xs, &xe))) {
            xs = xe = 0;
        }
        if (mode == DEPTH) {
            int last = xs < xe ? span(p, wk, y, xs, xe, id, DEPTH, 0, 0, 0) : 0;
            sp[y - y0][0] = (int16_t)xs, sp[y - y0][1] = (int16_t)(last != 0 ? last : xs);
            continue;
        }
        if (xs >= xe) {
            continue;
        }
        if (lite) {
            lite_span(p, wk, y, xs, xe, id, mode);
        } else if (mode == SHADE_WON) {
            span_kind(p, wk, y, xs, xe, id, SHADE_WON);
        } else {
            span_kind(p, wk, y, xs, xe, id, DRAW);
        }
    }
}

#define MAX_LAYERS 256

/* MVC2_RDLOG=n: frame n's lists into rdlog.txt; MVC2_RDTEX adds the textures, MVC2_RDVTX=state
   a state's triangles, MVC2_RDPIX=x,y a pixel's translucent fragments */
static FILE* dbg_log;
static int dbg_pix = -1;

static void dump_texture(const RD_STATE* s, int state) {
    char name[32];
    FILE* p;
    int x, y, alpha;
    for (alpha = 0; alpha < 2; alpha++) {
        snprintf(name, sizeof name, alpha ? "tex_%d_a.pgm" : "tex_%d.ppm", state);
        if ((p = fopen(name, "wb")) == NULL) {
            continue;
        }
        fprintf(p, "P%d %d %d 255\n", alpha ? 5 : 6, s->tex->width, s->tex->height);
        for (y = 0; y < s->tex->height; y++) {
            for (x = 0; x < s->tex->width; x++) {
                uint32_t c = texel(s, 0, x, y);
                if (alpha) {
                    fputc((int)(c >> 24), p);
                } else {
                    fputc((int)(c >> 16 & 255), p), fputc((int)(c >> 8 & 255), p), fputc((int)(c & 255), p);
                }
            }
        }
        fclose(p);
    }
}

static int texel_bytes(const RD_TEXTURE* t) {
    return t->format == RD_PAL4 || t->format == RD_PAL8 ? 1 : 2;
}

/* MVC2_RDSAVE=n,m,...: those frames into rdframe_N.bin, for port/tools (rd_frame_file.c) */
static void save_frame(void) {
    static unsigned count;
    const char* e = getenv("MVC2_RDSAVE");
    const RD_TEXTURE** tex;
    int ntex = 0, i, j, k;
    FILE* f;
    char name[32];
    count++;
    while (e != NULL && (unsigned)atoi(e) != count) {
        e = strchr(e, ',');
        e = e != NULL ? e + 1 : NULL;
    }
    snprintf(name, sizeof name, "rdframe_%u.bin", count);
    if (e == NULL || (f = fopen(name, "wb")) == NULL) {
        return;
    }
    tex = malloc(sizeof *tex * (size_t)(rd_frame->nstates + 1));
    fwrite("RDF1", 1, 4, f);
    fwrite(rd_frame->ntris, sizeof(int), RD_LISTS, f);
    fwrite(&rd_frame->nstates, sizeof(int), 1, f);
    for (i = 0; i < RD_LISTS; i++) {
        fwrite(rd_frame->tris[i], sizeof(RD_TRI), (size_t)rd_frame->ntris[i], f);
    }
    fwrite(rd_frame->background, sizeof rd_frame->background, 1, f);
    fwrite(&rd_fog, sizeof rd_fog, 1, f);
    fwrite(rd_palette, sizeof rd_palette, 1, f);
    for (i = 0; i < rd_frame->nstates; i++) {
        const RD_STATE* s = &rd_frame->states[i];
        int n = -1;
        for (j = 0; s->tex != NULL && j < ntex; j++) {
            if (tex[j] == s->tex) {
                n = j;
            }
        }
        if (s->tex != NULL && n < 0 && tex != NULL) {
            tex[ntex] = s->tex;
            n = ntex++;
        }
        fwrite(s, sizeof *s, 1, f);
        fwrite(&n, sizeof n, 1, f);
    }
    fwrite(&ntex, sizeof ntex, 1, f);
    for (i = 0; i < ntex; i++) {
        const RD_TEXTURE* t = tex[i];
        fwrite(&t->width, sizeof(int), 1, f);
        fwrite(&t->height, sizeof(int), 1, f);
        fwrite(&t->format, sizeof(int), 1, f);
        fwrite(&t->levels, sizeof(int), 1, f);
        for (k = 0; k <= t->levels; k++) {
            int w = t->width >> k, h = t->height >> k;
            fwrite(k == 0 ? t->texels : t->mip[k], (size_t)texel_bytes(t), (size_t)(w < 1 ? 1 : w) * (h < 1 ? 1 : h), f);
        }
    }
    free(tex);
    fclose(f);
}

static void log_frame(void) {
    static unsigned count;
    const char* e = getenv("MVC2_RDLOG");
    int list, i, px, py;
    count++;
    dbg_pix = -1;
    if (e == NULL || (unsigned)atoi(e) != count || (dbg_log = fopen("rdlog.txt", "w")) == NULL) {
        return;
    }
    fprintf(dbg_log, "fog colour %08X density %g table", rd_fog.color, rd_fog.density);
    for (i = 0; i < 128; i++) {
        fprintf(dbg_log, " %.2f", rd_fog.table[i]);
    }
    fprintf(dbg_log, "\nbackground %08X %08X %08X z %g\n", rd_frame->background[0].base, rd_frame->background[1].base,
            rd_frame->background[2].base, rd_frame->background[0].z);
    e = getenv("MVC2_RDPIX");
    if (e != NULL && sscanf(e, "%d,%d", &px, &py) == 2) {
        dbg_pix = py * RD_WIDTH + px;
    }
    for (list = 0; list < RD_LISTS; list++) {
        for (i = 0; i < rd_frame->ntris[list];) {
            const RD_TRI* t = &rd_frame->tris[list][i];
            const RD_STATE* s = &rd_frame->states[t->state];
            float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, z0 = 1e9f, z1 = -1e9f;
            int j = i, k;
            for (; j < rd_frame->ntris[list] && rd_frame->tris[list][j].state == t->state; j++) {
                for (k = 0; k < 3; k++) {
                    const RD_VTX* v = &rd_frame->tris[list][j].v[k];
                    x0 = fminf(x0, v->x), x1 = fmaxf(x1, v->x), y0 = fminf(y0, v->y), y1 = fmaxf(y1, v->y);
                    z0 = fminf(z0, v->z), z1 = fmaxf(z1, v->z);
                }
            }
            fprintf(dbg_log, "L%d tri%d st%d n%d dep%d zw%d bl%d/%d sh%d ua%d ta%d bi%d of%d fl%d fog%d cull%d uv%d mipd%d pal%d tex%p %dx%d f%d lv%d | "
                             "%.0f,%.0f-%.0f,%.0f z %g..%g base %08X ofs %08X uv %.3f,%.3f\n",
                    list, i, t->state, j - i, s->depth, s->zwrite, s->src, s->dst, s->shading, s->use_alpha, s->tex_alpha,
                    s->bilinear, s->offset, s->flat, s->fog, s->cull, s->uv_mode, s->mip_d, s->palette, (const void*)s->tex,
                    s->tex ? s->tex->width : 0, s->tex ? s->tex->height : 0, s->tex ? s->tex->format : -1,
                    s->tex ? s->tex->levels : 0, x0, y0, x1, y1, z0, z1, t->v[0].base,
                    t->v[0].offset, t->v[0].u, t->v[0].v);
            if (s->tex != NULL && getenv("MVC2_RDTEX") != NULL) {
                dump_texture(s, t->state);
            }
            if (getenv("MVC2_RDVTX") != NULL && atoi(getenv("MVC2_RDVTX")) == t->state) {
                for (k = i; k < j; k++) {
                    const RD_VTX* v = rd_frame->tris[list][k].v;
                    fprintf(dbg_log, "  %.1f,%.1f %.3f,%.3f  %.1f,%.1f %.3f,%.3f  %.1f,%.1f %.3f,%.3f\n", v[0].x, v[0].y, v[0].u,
                            v[0].v, v[1].x, v[1].y, v[1].u, v[1].v, v[2].x, v[2].y, v[2].u, v[2].v);
                }
            }
            i = j;
        }
    }
}

/* row y's translucent fragments, far to near, into the frame */
static void resolve_row(const WORKER* w, int y) {
    int idx[MAX_LAYERS], at;
    for (at = y * RD_WIDTH; at < y * RD_WIDTH + draw_w; at++) {
        int n = 0, i, f = fhead[at];
        if (f < 0) {
            continue;
        }
        if (w->frags[f].next < 0 && at != dbg_pix) {
            cbuf[at] = blend(tstate[w->frags[f].order], unpack(w->frags[f].color), cbuf[at]);
            continue;
        }
        for (; f >= 0 && n < MAX_LAYERS; f = w->frags[f].next) {
            /* by depth, then order: the list runs newest first */
            const FRAG* p = &w->frags[f];
            for (i = n; i > 0; i--) {
                const FRAG* q = &w->frags[idx[i - 1]];
                if (q->z < p->z || (q->z == p->z && q->order < p->order)) {
                    break;
                }
                idx[i] = idx[i - 1];
            }
            idx[i] = f;
            n++;
        }
        for (i = 0; i < n; i++) {
            const FRAG* p = &w->frags[idx[i]];
            if (at == dbg_pix) {
                fprintf(dbg_log, "pixel: under %08X, z %g tri %d st %d colour %08X\n", cbuf[at], p->z, p->order,
                        rd_frame->tris[RD_TRANS][p->order].state, p->color);
            }
            cbuf[at] = blend(tstate[p->order], unpack(p->color), cbuf[at]);
        }
    }
}

static RD_VTX background[3];    /* on the drawn frame */

static void background_row(int y) {
    const RD_VTX *a = &background[0], *b = &background[1], *c = &background[2];
    double area = edge(a, b, c->x, c->y);
    COLOR ca = unpack(a->base), cb = unpack(b->base), cc = unpack(c->base), k;
    int x;
    if (a->base == b->base && a->base == c->base && a->z == b->z && a->z == c->z && isfinite(a->z) &&
        isfinite(a->x) && isfinite(a->y) && isfinite(b->x) && isfinite(b->y) && isfinite(c->x) && isfinite(c->y)) {
        for (x = 0; x < draw_w; x++) {
            cbuf[y * RD_WIDTH + x] = a->base;
            zbuf[y * RD_WIDTH + x] = a->z;
            fhead[y * RD_WIDTH + x] = -1;
        }
        return;
    }
    for (x = 0; x < draw_w; x++) {
        int at = y * RD_WIDTH + x;
        double w0 = 1, w1 = 0, w2 = 0;
        if (area != 0) {
            w0 = edge(b, c, x + 0.5, y + 0.5) / area;
            w1 = edge(c, a, x + 0.5, y + 0.5) / area;
            w2 = 1 - w0 - w1;
        }
        k.a = clamp255((int)(w0 * ca.a + w1 * cb.a + w2 * cc.a + 0.5));
        k.r = clamp255((int)(w0 * ca.r + w1 * cb.r + w2 * cc.r + 0.5));
        k.g = clamp255((int)(w0 * ca.g + w1 * cb.g + w2 * cc.g + 0.5));
        k.b = clamp255((int)(w0 * ca.b + w1 * cb.b + w2 * cc.b + 0.5));
        cbuf[at] = pack(k);
        zbuf[at] = (float)(w0 * a->z + w1 * b->z + w2 * c->z);
        fhead[at] = -1;
    }
}

/* the triangles in the worker's bands, in order; 0 when there is no memory for them */
static int bin_tris(WORKER* w) {
    int nb = (draw_h + BAND - 1) / BAND, i, b, n, at[MAX_BANDS];
    if (w->win == NULL && (w->win = malloc(BAND * RD_WIDTH * sizeof *w->win)) == NULL) {
        return 0;
    }
    if (lite && w->lb == NULL && (w->lb = malloc(sizeof *w->lb)) == NULL) {
        return 0;
    }
    memset(w->bin_at, 0, sizeof w->bin_at);
    for (i = 0; i < ntotal; i++) {
        const SETUP* p = &setups[i];
        if (p->y1 > p->y0) {
            for (b = first_band(w, p->y0 / BAND); b <= (p->y1 - 1) / BAND; b += nworkers) {
                w->bin_at[b + 1]++;
            }
        }
    }
    for (b = 0; b < nb; b++) {
        w->bin_at[b + 1] += w->bin_at[b];
    }
    n = w->bin_at[nb];
    if (n > w->maxbin) {
        int* q = realloc(w->bin, (size_t)n * sizeof *q);
        int16_t(*sp)[2] = q != NULL ? realloc(w->spans, (size_t)n * BAND * sizeof *sp) : NULL;
        if (q != NULL) {
            w->bin = q;
        }
        if (sp == NULL) {
            return 0;
        }
        w->spans = sp;
        w->maxbin = n;
    }
    if (ntotal > w->maxwon) {
        int* q = realloc(w->won, (size_t)ntotal * sizeof *q);
        if (q == NULL) {
            return 0;
        }
        w->won = q;
        w->maxwon = ntotal;
    }
    memcpy(at, w->bin_at, sizeof at);
    for (i = 0; i < ntotal; i++) {
        const SETUP* p = &setups[i];
        if (p->y1 > p->y0) {
            for (b = first_band(w, p->y0 / BAND); b <= (p->y1 - 1) / BAND; b += nworkers) {
                w->bin[at[b]++] = i;
            }
        }
    }
    return 1;
}

/* a row of the drawn frame out as RGB565, written in order and never read */
static void row565(uint16_t* o, const uint32_t* c) {
    int x = 0;
#if defined(RD_NEON)
    for (; x + 8 <= draw_w; x += 8) {
        uint8x8x4_t q = vld4_u8((const uint8_t*)&c[x]);
        uint16x8_t k = vsriq_n_u16(vsriq_n_u16(vshll_n_u8(q.val[2], 8), vshll_n_u8(q.val[1], 8), 5), vshll_n_u8(q.val[0], 8), 11);
        if (scale == 1) {
            vst1q_u16(&o[x], k);
        } else {
            uint16x8x2_t z = vzipq_u16(k, k);
            vst1q_u16(&o[2 * x], z.val[0]);
            vst1q_u16(&o[2 * x + 8], z.val[1]);
        }
    }
#elif defined(RD_SSE2)
    for (; x + 8 <= draw_w; x += 8) {
        const __m128i mr = _mm_set1_epi32(0xF800), mg = _mm_set1_epi32(0x07E0), mb = _mm_set1_epi32(0x001F);
        __m128i lo = _mm_loadu_si128((const __m128i*)&c[x]), hi = _mm_loadu_si128((const __m128i*)&c[x + 4]), k;
        lo = _mm_or_si128(_mm_or_si128(_mm_and_si128(_mm_srli_epi32(lo, 8), mr), _mm_and_si128(_mm_srli_epi32(lo, 5), mg)),
                          _mm_and_si128(_mm_srli_epi32(lo, 3), mb));
        hi = _mm_or_si128(_mm_or_si128(_mm_and_si128(_mm_srli_epi32(hi, 8), mr), _mm_and_si128(_mm_srli_epi32(hi, 5), mg)),
                          _mm_and_si128(_mm_srli_epi32(hi, 3), mb));
        /* sign-extended from 16 bits, so the pack keeps them */
        k = _mm_packs_epi32(_mm_srai_epi32(_mm_slli_epi32(lo, 16), 16), _mm_srai_epi32(_mm_slli_epi32(hi, 16), 16));
        if (scale == 1) {
            _mm_storeu_si128((__m128i*)&o[x], k);
        } else {
            _mm_storeu_si128((__m128i*)&o[2 * x], _mm_unpacklo_epi16(k, k));
            _mm_storeu_si128((__m128i*)&o[2 * x + 8], _mm_unpackhi_epi16(k, k));
        }
    }
#endif
    for (; x < draw_w; x++) {
        uint16_t k = (uint16_t)((c[x] >> 8 & 0xF800) | (c[x] >> 5 & 0x07E0) | (c[x] >> 3 & 0x001F));
        if (scale == 1) {
            o[x] = k;
        } else {
            o[2 * x] = o[2 * x + 1] = k;
        }
    }
}

/* a band: its background, the opaque triangles' depths and then their colours, each pixel shaded once as the PowerVR2
   does it, the rest drawn in order, the translucent fragments resolved, and the rows out */
static void draw_band(WORKER* w, int band) {
    int ya = band * BAND, yb = ya + BAND < draw_h ? ya + BAND : draw_h, i, j, x, y;
    w->band = band;
    w->band_y = ya;
    w->nfrags = 0;
    for (y = ya; y < yb; y++) {
        if (row_on(y)) {
            background_row(y);
        }
    }
    for (i = 0; i < (yb - ya) * RD_WIDTH; i++) {
        w->win[i] = -1;
    }
    for (i = w->bin_at[band]; i < w->bin_at[band + 1] && setups[w->bin[i]].list == RD_OPAQUE; i++) {
        raster(&setups[w->bin[i]], w, ya, yb, DEPTH, w->bin[i], &w->spans[(i - w->bin_at[band]) * BAND]);
    }
    for (j = w->bin_at[band]; j < i; j++) {
        if (w->won[w->bin[j]] == band) {
            raster(&setups[w->bin[j]], w, ya, yb, SHADE_WON, w->bin[j], &w->spans[(j - w->bin_at[band]) * BAND]);
        }
    }
    for (; i < w->bin_at[band + 1]; i++) {
        raster(&setups[w->bin[i]], w, ya, yb, DRAW, w->bin[i], NULL);
    }
    for (y = ya; y < yb; y++) {
        if (!row_on(y)) {
            continue;
        }
        resolve_row(w, y);
        if (out565 != NULL) {
            int k;
            for (k = 0; k < scale; k++) {
                if (field < 0 || (y * scale + k) % 2 == field) {
                    row565(&out565[(y * scale + k) * out_pitch], &cbuf[y * RD_WIDTH]);
                }
            }
        } else if (scale == 1) {
            for (x = 0; x < RD_WIDTH; x++) {
                out_frame[y * out_pitch + x] = OUT_PIXEL(cbuf[y * RD_WIDTH + x]);
            }
        } else {
            uint32_t* o = &out_frame[2 * y * out_pitch];
            for (x = 0; x < RD_WIDTH; x++) {
                o[x] = o[out_pitch + x] = OUT_PIXEL(cbuf[y * RD_WIDTH + x / 2]);
            }
        }
    }
}

/* phase 0: set up a share of the triangles; 1: draw the worker's bands */
static void work(WORKER* w) {
    int i;
    if (phase == 0) {
        int per = (ntotal + nworkers - 1) / nworkers, end = (w->index + 1) * per, base[RD_LISTS + 1], list = 0;
        base[0] = 0;
        for (i = 0; i < RD_LISTS; i++) {
            base[i + 1] = base[i] + rd_frame->ntris[i];
        }
        for (i = w->index * per; i < end && i < ntotal; i++) {
            while (i >= base[list + 1]) {
                list++;
            }
            setup_tri(&setups[i], list, &rd_frame->tris[list][i - base[list]], i - base[list]);
        }
        return;
    }
    w->tex.s = NULL;
    if (!bin_tris(w)) {
        return;
    }
    for (i = 0; i < ntotal; i++) {
        w->won[i] = -1;
    }
    for (i = first_band(w, 0); i * BAND < draw_h; i += nworkers) {
        draw_band(w, i);
    }
}

static int worker_main(void* arg) {
    WORKER* w = arg;
    for (;;) {
        rd_sem_wait(w->go);
        work(w);
        rd_sem_post(done);
    }
    return 0;
}

static void take_quality(void) {
    fast = rd_q.fast;
    nofog = rd_q.nofog;
    nosort = rd_q.nosort;
    lite = rd_q.lite;
    scale = rd_q.half ? 2 : 1;
    draw_w = RD_WIDTH / scale, draw_h = RD_HEIGHT / scale;
}

/* MVC2_RDTHREADS=n workers, by default one a CPU */
static void start_workers(void) {
    const char* e = getenv("MVC2_RDTHREADS");
    const char* q = getenv("MVC2_RDQUALITY");
    int i, n = e != NULL ? atoi(e) : rd_cpu_count();
    if (q != NULL) {
        rd_quality(q);
    }
    n = n < 1 ? 1 : n > MAX_WORKERS ? MAX_WORKERS : n;
    done = SDL_CreateSemaphore(0);
    nworkers = 1;
    workers[0].index = 0;
    for (i = 1; i < n && done != NULL; i++) {
        WORKER* w = &workers[i];
        w->index = i;
        w->go = SDL_CreateSemaphore(0);
        if (w->go == NULL || SDL_CreateThread(worker_main, "render", w) == NULL) {
            break;
        }
        nworkers++;
    }
}

static void run_phase(int p) {
    int i;
    phase = p;
    for (i = 1; i < nworkers; i++) {
        rd_sem_post(workers[i].go);
    }
    work(&workers[0]);
    for (i = 1; i < nworkers; i++) {
        rd_sem_wait(done);
    }
}

static double now_ms(void) {
    return SDL_GetPerformanceCounter() * 1000.0 / SDL_GetPerformanceFrequency();
}

/* MVC2_RDTIME: the frame times on stderr */
static void report_time(double ms) {
    static int on = -1, frames;
    static double total, most;
    if (on < 0) {
        on = getenv("MVC2_RDTIME") != NULL;
    }
    if (!on) {
        return;
    }
    total += ms;
    most = ms > most ? ms : most;
    if (++frames == 300) {
        fprintf(stderr, "render: %.2f ms a frame, at most %.2f (%d workers)\n", total / frames, most, nworkers);
        frames = 0;
        total = most = 0;
    }
}

static void end_frame(void);

void rd_end_frame(uint32_t* frame, int pitch) {
    out_frame = frame, out565 = NULL;
    out_pitch = pitch;
    field = -1;
    end_frame();
}

void rd_end_frame565(uint16_t* frame, int pitch, int f) {
    out_frame = NULL, out565 = frame;
    out_pitch = pitch;
    field = f < 0 ? -1 : f & 1;
    end_frame();
}

static void end_frame(void) {
    double start = now_ms();
    int list, i;
    if (zbuf == NULL && !make_tables()) {
        return;
    }
    if (nworkers == 0) {
        start_workers();
    }
    take_quality();
    log_frame();
    save_frame();
    ntotal = 0;
    for (list = 0; list < RD_LISTS; list++) {
        ntotal += rd_frame->ntris[list];
    }
    if (ntotal > maxsetups) {
        SETUP* p = realloc(setups, (size_t)ntotal * sizeof *p);
        const RD_STATE** q = p != NULL ? realloc(tstate, (size_t)ntotal * sizeof *q) : NULL;
        if (p != NULL) {
            setups = p;
        }
        if (q == NULL) {
            return;
        }
        tstate = q;
        maxsetups = ntotal;
    }
    for (i = 0; i < rd_frame->nstates; i++) {
        const RD_STATE* s = &rd_frame->states[i];
        if (s->tex != NULL && s->bilinear && (s->tex->format == RD_PAL4 || s->tex->format == RD_PAL8)) {
            break;
        }
    }
    for (i = i < rd_frame->nstates ? 0 : 1024; i < 1024; i++) {
        uint32_t q = rd_palette[i];
        fpal[i][0] = (float)(int)(q >> 24), fpal[i][1] = (float)(int)(q >> 16 & 255);
        fpal[i][2] = (float)(int)(q >> 8 & 255), fpal[i][3] = (float)(int)(q & 255);
    }
    fog_on = 0;
    for (i = 0; i < 128; i++) {
        fog_on |= rd_fog.table[i] != 0.0f;
    }
    for (i = 0; i < 3; i++) {
        background[i] = rd_frame->background[i];
        if (scale != 1) {
            background[i].x /= scale, background[i].y /= scale;
        }
    }
    run_phase(0);
    run_phase(1);
    if (dbg_log != NULL) {
        fclose(dbg_log);
        dbg_log = NULL;
    }
    report_time(now_ms() - start);
}
