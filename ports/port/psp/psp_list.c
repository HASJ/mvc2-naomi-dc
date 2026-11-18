/*
 * psp_list.c  The frame's call list for the GE, written from rd's lists and models
 *
 * What it writes is its own: the list, the vertices and what it keeps of the GE's state, so that it can be made
 * apart from the CPU that runs the game.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <pspgu.h>

#include "psp_ge.h"
#include "psp_front.h"
#include "host_cache.h"
#include "../nl/nl_port.h"

GE_LIST ge_list;


static int is_flat(const TV* t) {
    return t->v[0]->z == t->v[1]->z && t->v[1]->z == t->v[2]->z;
}

/* both lines of a triangle read into the cache ahead of its use; one past the list's end is harmless */
static void prefetch_tri(const RD_TRI* t) {
    host_prefetch(t);
    host_prefetch((const char*)(t + 1) - 1);
}

/* ---- the GE's state, as last set in the frame's list ---- */

typedef struct {
    int tex;                        /* 0 off, 1 on */
    const uint8_t* image;           /* its first level: an entry may move in gtex */
    int nlev;                       /* the levels past the first given in the texture's mode */
    float lod_bias;
    int clut;                       /* palette | format << 16 */
    int func;                       /* tfx | tcc << 8 */
    int filter, wrap;
    int blend;                      /* -1 off, else src | dst << 8 */
    int depth, zmask, alpha_test, fog;
    float zscale, zcentre, fog1, fog2;
    int cull;                       /* 0 none, 2 the front faces clockwise, 3 counter-clockwise */
    int flat, lighting, light0;
    int view;                       /* 1: the identity, as the CPU's triangles want it */
    uint32_t ambient, diffuse;      /* a lit model's; diffuse 0: not known, as its alpha is always 255 */
    int ambient_known;
    int colour_material;
    float light[3];
} GE;

/* what put_one takes from a state alone, for the state of the triangles before (the frame's numbers: si -1 at its
   start) */
typedef struct {
    int si, cell;                   /* cell: its texture's in the atlas, -1 none */
    const RD_STATE* s;
    uint8_t no_texels, table2, fog, plain_tex, opaque_alpha;
} PUT_STATE;

/* the translucent list's items: n, then the polygons' keys, firsts and order, room for each */
#define TRANS_ROOM (RD_MAX_TRIS + 1 + RD_GPU_GROUPS)
#define GROUP_ITEM 0x80000000u      /* in trans_order: a group's index, not a triangle's */

/* All the list's writer keeps, in cache lines of its own: run on the Media Engine, it shares no line with what the
   main CPU writes, which would lose one CPU's writes */
static struct __attribute__((aligned(64))) {
    GE ge;
    float tex_w, tex_h;             /* the texture's size, for through mode's texels */
    float tex_u0, tex_v0;           /* and where it starts in the one bound: an atlas cell's tile */
    const GTEX* tex_cur;            /* the bound texture's levels, filter and level bias set for */
    const RD_STATE* tex_state;
    GTEX tile_gtex[TILE_TEXTURES];  /* the frame's tiles drawn wrapped, by state */
    int tile_si[TILE_TEXTURES], ntile_tex;
    int nverts, nover, batch_first;
    /* the batch being added to: its kind, state, depth and fog line */
    int batch_kind, cur_state, cur_table2, cur_clamp;
    int cur_cell;                   /* the batch is the atlas's */
    float cur_depth, cur_fog1, cur_fog2;
    float scr_x, scr_kx, scr_ky;    /* the game's pixel on the screen: x * kx + x0, y * ky */
    /* the GE's models have their depth from its own sums: the CPU's punch-through and translucent triangles in the
       3D range a step nearer, so that a decal on a model stays on it */
    float zbias;
    uint8_t *verts_made, *list_made;
    uint32_t ahead;
    PUT_STATE put_st;
    float flat_z_was, flat_depth_was;   /* a flat triangle's depth, the last z's kept */
    uint32_t flat_frame;
    float over_depth[MAX_OVER / 3]; /* a fogged triangle's */
    uint32_t trans_groups[RD_GPU_GROUPS];
    uint32_t trans_order[TRANS_ROOM * 6];
    LIST_STATS st;
} lw;


/* nothing known: each value set again when first wanted */
static void ge_reset(void) {
    lw.ge.tex = lw.ge.clut = lw.ge.func = lw.ge.filter = lw.ge.wrap = lw.ge.nlev = lw.ge.blend = -2;
    lw.ge.lod_bias = NAN;
    lw.ge.cull = lw.ge.flat = lw.ge.lighting = lw.ge.light0 = lw.ge.view = lw.ge.colour_material = -2;
    lw.ge.ambient = lw.ge.diffuse = 0;
    lw.ge.ambient_known = 0;
    lw.ge.light[0] = lw.ge.light[1] = lw.ge.light[2] = NAN;
    lw.ge.depth = lw.ge.zmask = lw.ge.alpha_test = lw.ge.fog = -2;
    lw.ge.image = NULL;
    lw.ge.zscale = lw.ge.zcentre = lw.ge.fog1 = lw.ge.fog2 = NAN;
}

static void set_enable(int* now, int want, int what) {
    if (*now != want) {
        *now = want;
        if (want) {
            ge_enable(what);
        } else {
            ge_disable(what);
        }
    }
}

static void set_cull(int cull) {
    if (cull != lw.ge.cull) {
        if (cull >= 2) {
            ge_front_face(cull == 2 ? GU_CW : GU_CCW);
            if (lw.ge.cull < 2) {
                ge_enable(GU_CULL_FACE);
            }
        } else {
            ge_disable(GU_CULL_FACE);
        }
        lw.ge.cull = cull;
    }
}

static void set_flat(int flat) {
    if (flat != lw.ge.flat) {
        lw.ge.flat = flat;
        ge_i(GE_SHADE, flat ? GU_FLAT : GU_SMOOTH);
    }
}

/* the state the CPU's triangles are drawn in: no culling or lighting, smooth, the view the identity */
static void plain(void) {
    static const ScePspFMatrix4 identity = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };
    set_cull(0);
    set_flat(0);
    set_enable(&lw.ge.lighting, 0, GU_LIGHTING);
    if (lw.ge.view != 1) {
        lw.ge.view = 1;
        ge_matrix(GU_VIEW, &identity);
    }
}

static void set_depth_value(float scale, float centre) {
    if (scale != lw.ge.zscale || centre != lw.ge.zcentre) {
        lw.ge.zscale = scale;
        lw.ge.zcentre = centre;
        ge_f(GE_Z_SCALE, scale);
        ge_f(GE_Z_CENTRE, centre);
    }
}

static void set_fog_line(float a, float b) {
    if (a != lw.ge.fog1 || b != lw.ge.fog2) {
        lw.ge.fog1 = a;
        lw.ge.fog2 = b;
        ge_f(GE_FOG1, a);
        ge_f(GE_FOG2, b);
    }
}

/* the GE's blend factor for rd's: the fixed colour for one and zero */
static int factor(int f, unsigned* fix) {
    static const int map[8] = { GU_FIX, GU_FIX, GU_OTHER_COLOR, GU_ONE_MINUS_OTHER_COLOR, GU_SRC_ALPHA,
                                GU_ONE_MINUS_SRC_ALPHA, GU_DST_ALPHA, GU_ONE_MINUS_DST_ALPHA };
    *fix = f == RD_ONE ? 0xFFFFFFu : 0;
    return map[f & 7];
}

static int state_wrap(const RD_STATE* s) {
    return (s->uv_mode & 2 ? GU_CLAMP : GU_REPEAT) | (s->uv_mode & 1 ? GU_CLAMP : GU_REPEAT) << 8;
}

static void set_wrap(int wrap) {
    if (wrap != lw.ge.wrap) {
        lw.ge.wrap = wrap;
        ge_i(GE_TEX_WRAP, (unsigned)wrap);
    }
}

/* the bound texture's levels, filter and level bias: those made here only where drawn in 3D with a bilinear state */

static void tex_levels(int threed) {
    static const float bias[16] = { 0, -4, -2, -1 };
    const GTEX* g = lw.tex_cur;
    const RD_STATE* s = lw.tex_state;
    int n = g->own, filter;
    if (threed && s->bilinear && probe != P_NO_MIPS) {
        n = g->levels;
    }
    if (n != lw.ge.nlev) {
        lw.ge.nlev = n;
        ge_tex_mode(g->psm, n, g->swizzled);
    }
    filter = s->bilinear && probe != P_NEAREST
                 ? (n > 0 ? GU_LINEAR_MIPMAP_NEAREST : GU_LINEAR) | GU_LINEAR << 8
                 : (n > 0 ? GU_NEAREST_MIPMAP_NEAREST : GU_NEAREST) | GU_NEAREST << 8;
    if (filter != lw.ge.filter) {
        lw.ge.filter = filter;
        ge_i(GE_TEX_FILTER, (unsigned)filter);
    }
    if (n > 0) {
        float made = s->origin == RD_SCENE ? mip_bias : front_bias;
        float b = n > g->own ? made + (probe == P_MIP_BIAS) : bias[s->mip_d & 15];
        if (b != lw.ge.lod_bias) {
            lw.ge.lod_bias = b;
            ge_tex_level_mode(GU_TEXTURE_AUTO, b);
        }
    }
}


/* A tile drawn wrapped, which its cell can't be, is a texture of its own for the frame, made from its rows: past the
   texels drawn, their edge. */
uint8_t tile_mem[2][TILE_TEXTURES][32 * 16] __attribute__((aligned(64)));

static const GTEX* tile_texture(int si) {
    const STATE_TEX* e = &state_tex[si];
    int n = e->side, w = e->width, i, y;
    GTEX* g;
    uint8_t* o;
    for (i = 0; i < lw.ntile_tex; i++) {
        if (lw.tile_si[i] == si) {
            return &lw.tile_gtex[i];
        }
    }
    if (lw.ntile_tex == TILE_TEXTURES) {
        lw.st.tiles_full++;
        return NULL;
    }
    lw.tile_si[lw.ntile_tex] = si;
    g = &lw.tile_gtex[lw.ntile_tex];
    o = tile_mem[frame_no & 1][lw.ntile_tex++];
    memset(g, 0, sizeof *g);
    g->psm = GU_PSM_T4;
    g->w = g->h = w;
    g->tbw[0] = 32;
    g->lv[0] = o;
    for (y = 0; y < w; y++, o += 16) {
        const uint8_t* r = e->rows + (1 + (y < n ? y : n - 1)) * KEPT_ROW + 1;
        memcpy(o, r, (size_t)n / 2);
        memset(o + n / 2, (r[n / 2 - 1] >> 4) * 0x11, (size_t)(w - n) / 2);
    }
    return g;
}

/* state si's texture of its own */
static const GTEX* own_texture(int si) {
    const STATE_TEX* e = &state_tex[si];
    return e->tex != NULL ? e->tex : e->rows != NULL ? tile_texture(si) : NULL;
}

/* s's look; its texture as tex where that is given (the atlas) */
static void set_state(const RD_STATE* s, int list, int table2, int threed, const GTEX* tex) {
    static const int depth_map[8] = { GU_NEVER, GU_LESS, GU_EQUAL, GU_LEQUAL, GU_GREATER, GU_NOTEQUAL, GU_GEQUAL, GU_ALWAYS };
    const GTEX* g = s->tex == NULL || table2 || probe == P_NO_TEX ? NULL
                    : tex != NULL                                    ? tex
                                                                     : own_texture((int)(s - list_frame->states));
    int blend, depth, zmask;
    set_enable(&lw.ge.tex, g != NULL, GU_TEXTURE_2D);
    lw.tex_w = g != NULL ? (float)g->w : 0.0f;
    lw.tex_h = g != NULL ? (float)g->h : 0.0f;
    lw.tex_u0 = lw.tex_v0 = 0.0f;
    if (g != NULL) {
        int func, clut, j;
        if (g->lv[0] != lw.ge.image) {
            lw.ge.image = g->lv[0];
            lw.ge.nlev = -1;
            for (j = 0; j <= g->levels; j++) {
                int w = g->w >> j, h = g->h >> j;
                ge_tex_image(j, w < 1 ? 1 : w, h < 1 ? 1 : h, g->tbw[j], g->lv[j]);
            }
            ge_i(GE_TEX_FLUSH, 0);
        }
        if (g->psm == GU_PSM_T4 || g->psm == GU_PSM_T8) {
            clut = s->palette | g->psm << 16;
            if (clut != lw.ge.clut) {
                lw.ge.clut = clut;
                ge_i(GE_CLUT_FORMAT, GU_PSM_8888 | 0xFF << 8);
                ge_clut_load(g->psm == GU_PSM_T4 ? 2 : 32, &pal[s->palette & (g->psm == GU_PSM_T4 ? 1008 : 768)]);
            }
        }
        switch (s->shading) {
        case RD_DECAL: func = GU_TFX_REPLACE; break;
        case RD_DECAL_ALPHA: func = GU_TFX_DECAL; break;
        default: func = GU_TFX_MODULATE; break;
        }
        func |= (s->tex_alpha ? GU_TCC_RGBA : GU_TCC_RGB) << 8;
        if (func != lw.ge.func) {
            lw.ge.func = func;
            ge_i(GE_TEX_FUNC, (unsigned)func);
        }
        lw.tex_cur = g;
        lw.tex_state = s;
        tex_levels(threed);
        set_wrap(state_wrap(s));
        if (s->uv_mode & 12) {
            lw.st.mirrored++;
        }
    }
    /* opaque: the colour as it is, the depth as the state says; punch-through and translucent: blended, tested
       greater or equal, and only punch-through's written */
    if (list == RD_OPAQUE || (s->src == RD_ONE && s->dst == RD_ZERO) || probe == P_NO_BLEND) {
        blend = -1;
    } else {
        blend = s->src | s->dst << 8;
    }
    if (blend != lw.ge.blend) {
        if (blend < 0) {
            ge_disable(GU_BLEND);
        } else {
            unsigned fs, fd;
            int a = factor(s->src, &fs), b = factor(s->dst, &fd);
            ge_enable(GU_BLEND);
            ge_blend(GU_ADD, a, b, fs, fd);
        }
        lw.ge.blend = blend;
    }
    set_enable(&lw.ge.alpha_test, list == RD_PUNCH, GU_ALPHA_TEST);
    depth = list == RD_OPAQUE ? depth_map[s->depth & 7] : GU_GEQUAL;
    zmask = list == RD_OPAQUE ? !s->zwrite : list != RD_PUNCH;
    if (depth != lw.ge.depth) {
        lw.ge.depth = depth;
        ge_i(GE_Z_TEST, depth);
    }
    if (zmask != lw.ge.zmask) {
        lw.ge.zmask = zmask;
        ge_i(GE_Z_MASK, zmask);
    }
}


/* ---- triangles ---- */

static float fog_value(float z);

/* a batch's kind: 3D triangles, flat ones in through mode, or upright quads as the GE's sprites */
enum { B_3D, B_FLAT, B_SPRITES };

static void draw_batch(void) {
    int n = lw.nverts - lw.batch_first;
    if (n > 0) {
        plain();
        ge_draw(lw.batch_kind == B_SPRITES ? GU_SPRITES : GU_TRIANGLES, lw.batch_kind == B_3D ? VTYPE : VTYPE_2D, n,
                       NULL, &verts[lw.batch_first]);
        lw.st.batches++;
    }
    lw.batch_first = lw.nverts;
}

/* a corner on the way to the GE: its values over w, as the screen interpolates them */
typedef struct {
    float x, y, z;
    float uz, vz;
    float c[4];                     /* a r g b over w */
} CV;

static void to_cv(CV* o, const RD_VTX* v, uint32_t col) {
    float z = v->z;
    o->x = v->x;
    o->y = v->y;
    o->z = z;
    o->uz = v->u * z;
    o->vz = v->v * z;
    o->c[0] = (float)(col >> 24) * z;
    o->c[1] = (float)(col >> 16 & 255) * z;
    o->c[2] = (float)(col >> 8 & 255) * z;
    o->c[3] = (float)(col & 255) * z;
}

static void lerp_cv(CV* o, const CV* a, const CV* b, float f) {
    const float* p = &a->x;
    const float* q = &b->x;
    float* r = &o->x;
    int i;
    for (i = 0; i < (int)(sizeof(CV) / sizeof(float)); i++) {
        r[i] = p[i] + (q[i] - p[i]) * f;
    }
}

/* the polygon cut to k * coord <= k * at (axis 0 x, 1 y); how many corners are left */
static int clip(CV* out, const CV* in, int n, int axis, float k, float at) {
    int i, m = 0;
    for (i = 0; i < n && m < MAX_CORNERS; i++) {
        const CV* a = &in[i];
        const CV* b = &in[(i + 1) % n];
        float da = k * ((axis ? a->y : a->x) - at), db = k * ((axis ? b->y : b->x) - at);
        if (da <= 0) {
            out[m++] = *a;
        }
        if ((da < 0 && db > 0) || (da > 0 && db < 0)) {
            if (m < MAX_CORNERS) {
                lerp_cv(&out[m++], a, b, da / (da - db));
            }
        }
    }
    return m;
}

static int clamp255(float f) {
    int i = (int)(f + 0.5f);
    return i < 0 ? 0 : i > 255 ? 255 : i;
}

static void put_vertex(VTX* o, float x, float y, float z, float u, float v, uint32_t c) {
    float w = 1.0f / z;
    o->u = u;
    o->v = v;
    o->c = c;
    o->x = x * w;
    o->y = y * w;
    o->z = w;
}

/* the state's look takes no alpha from its vertices: theirs is taken as 1 */
/* a vertex's colour as the GE takes it: ABGR, the alpha 1 where the state's look takes none from it */
static uint32_t vertex_colour(uint32_t argb, int opaque_alpha) {
    return abgr(opaque_alpha ? argb | 0xFF000000u : argb);
}

static uint32_t table2_colour(float z) {
    return fog_rgb | (uint32_t)clamp255(fog_value(z) * 255.0f) << 24;
}

/* triangle v, whose corners' 1/w are z, to the opaque list's fog */
static void add_over(const VTX* v, const float* z, float depth) {
    int k;
    for (k = 0; k < 3; k++) {
        over[lw.nover + k] = v[k];
        over[lw.nover + k].c = table2_colour(z[k]);
    }
    lw.over_depth[lw.nover / 3] = depth;
    lw.nover += 3;
}

/* f at the nearest whole number, halves up: floorf(f + 0.5f) without the call */
static float nearest(float f) {
    float r = f + 0.5f;
    int i = (int)r;
    return (float)(i - ((float)i > r));
}

/* the game's pixel edge an edge at f falls on: the first pixel whose centre is at or past it, ceilf(f - 0.5f) */
static float game_edge(float f) {
    float e = f - 0.5f;
    int i = (int)e;
    return (float)(i + ((float)i < e));
}

/* in through mode, at the nearest whole pixel to the game's pixel edge: tiles that meet on the game's screen, or leave
   a gap no pixel centre is in, meet on the same one */
static void put_vertex_2d(VTX* o, const RD_VTX* v, float depth, uint32_t c) {
    o->u = lw.tex_u0 + v->u * lw.tex_w;
    o->v = lw.tex_v0 + v->v * lw.tex_h;
    o->c = c;
    o->x = nearest(game_edge(v->x) * lw.scr_kx + lw.scr_x);
    o->y = nearest(game_edge(v->y) * lw.scr_ky);
    o->z = depth;
}

/* the texture's corners inside it: filtered at a smaller size, its edges are not mixed with the far side's */
static int inside_uv(const RD_VTX* a, const RD_VTX* b, const RD_VTX* c) {
    return a->u >= 0.0f && a->u <= 1.0f && a->v >= 0.0f && a->v <= 1.0f && b->u >= 0.0f && b->u <= 1.0f &&
           b->v >= 0.0f && b->v <= 1.0f && c->u >= 0.0f && c->u <= 1.0f && c->v >= 0.0f && c->v <= 1.0f;
}

/* t and the triangle after it, u its quad's last corner, an upright rectangle whose texture is upright too, all one
   colour */
static __attribute__((noinline)) int sprite_pair(const RD_STATE* s, const TV* t, const TV* u) {
    const RD_VTX *a = t->v[0], *b = t->v[1], *c = t->v[2], *d = u->v[2];
    if (u->share != RD_SHARE(0, 2) || u->state != t->state || d->z != a->z) {
        return 0;
    }
    if (s->flat ? c->base != d->base : a->base != b->base || b->base != c->base || c->base != d->base) {
        return 0;
    }
    if (a->y == b->y && b->x == c->x && c->y == d->y && d->x == a->x) {
        return a->u == d->u && b->u == c->u && a->v == b->v && c->v == d->v;
    }
    if (a->x == b->x && b->y == c->y && c->x == d->x && d->y == a->y) {
        return a->v == d->v && b->v == c->v && a->u == b->u && c->u == d->u;
    }
    return 0;
}

static int culled(const RD_STATE* s, const TV* t) {
    const RD_VTX *a = t->v[0], *b = t->v[1], *c = t->v[2];
    float area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
    return area == 0.0f || !(area == area) || (s->cull != 0 && fabsf(area) < (float)RD_CULL_VAL) ||
           (s->cull == 2 && area < 0) || (s->cull == 3 && area > 0);
}

/* triangle t, cut by the screen's sides, as a fan; its corners' colours col */
static __attribute__((noinline)) void put_clipped(const TV* t, const uint32_t* col, int table2, int opaque_alpha,
                                                  int overlay, float depth) {
    CV p[MAX_CORNERS], q[MAX_CORNERS];
    int n = 3, i, k;
    for (k = 0; k < 3; k++) {
        to_cv(&p[k], t->v[k], col[k]);
    }
    n = clip(q, p, n, 0, -1.0f, 0.0f);
    n = clip(p, q, n, 0, 1.0f, RD_WIDTH);
    n = clip(q, p, n, 1, -1.0f, 0.0f);
    n = clip(p, q, n, 1, 1.0f, RD_HEIGHT);
    lw.st.clipped++;
    for (i = 1; i + 1 < n; i++) {
        const CV* fan[3] = { &p[0], &p[i], &p[i + 1] };
        for (k = 0; k < 3; k++) {
            const CV* v = fan[k];
            float w = 1.0f / v->z;
            uint32_t cc = (uint32_t)clamp255(v->c[0] * w) << 24 | (uint32_t)clamp255(v->c[1] * w) << 16 |
                          (uint32_t)clamp255(v->c[2] * w) << 8 | (uint32_t)clamp255(v->c[3] * w);
            put_vertex(&verts[lw.nverts++], v->x, v->y, v->z, v->uz * w, v->vz * w,
                       table2 ? table2_colour(v->z) : vertex_colour(cc, opaque_alpha));
        }
        if (overlay && lw.nover + 3 <= MAX_OVER) {
            float z[3] = { fan[0]->z, fan[1]->z, fan[2]->z };
            add_over(&verts[lw.nverts - 3], z, depth);
        }
    }
}

/* states a and b the same but for their textures */
static __attribute__((noinline)) int same_look(int a, int b) {
    RD_STATE x, y;
    if (a == b) {
        return 1;
    }
    x = list_frame->states[a];
    y = list_frame->states[b];
    x.tex = y.tex = NULL;
    return memcmp(&x, &y, sizeof x) == 0;
}

/* the GE's fog is (w + f1) * f2, one minus the table's: a line through t's nearest and farthest corners */
static __attribute__((noinline)) void fog_line(const TV* t, float* f1, float* f2) {
    const RD_VTX *n = t->v[0], *f = t->v[0];
    float gn, gf;
    int k;
    for (k = 1; k < 3; k++) {
        n = t->v[k]->z > n->z ? t->v[k] : n;
        f = t->v[k]->z < f->z ? t->v[k] : f;
    }
    gn = 1.0f - fog_value(n->z);
    gf = 1.0f - fog_value(f->z);
    if (gn == gf || n->z == f->z) {
        *f2 = 1e-10f;
        *f1 = gn / *f2 - 1.0f / n->z;
    } else {
        float wn = 1.0f / n->z, wf = 1.0f / f->z;
        *f2 = (gf - gn) / (wf - wn);
        *f1 = gn / *f2 - wn;
    }
}

/* The vertices and the list are written in order: their lines are taken into the cache ahead of the writes, without
   being read from RAM, the job's ahead bytes at a time. A line once taken is not taken again (it may hold what was
   written), nor one that starts before the write position. */
static void lines_ahead(const void* cur, uint8_t** made, uint32_t room, const void* end) {
    if ((const uint8_t*)cur + room > *made) {
        uint8_t* from = (const uint8_t*)cur > *made ? (uint8_t*)cur : *made;
        host_lines_new(from, lw.ahead, end);
        *made = from + lw.ahead;
    }
}

/* room taken ahead for a triangle's vertices, cut into a fan at most, and twice the bytes taken at once of the list */
static void write_ahead(void) {
    if (lw.ahead == 0) {
        return;
    }
    lines_ahead(&verts[lw.nverts], &lw.verts_made, 3 * (MAX_CORNERS - 2) * sizeof(VTX), verts + MAX_VERTS);
    if (list_cached) {
        lines_ahead(ge_list.cur, &lw.list_made, 2 * lw.ahead, (const uint8_t*)clist + LIST_BYTES);
    }
}

/* the batch before drawn and the next begun in state si (set again where new), its kind, texture wrap, depth and fog */
static __attribute__((noinline)) void begin_batch(const RD_STATE* s, int list, int si, int new_state, int table2,
                                                  int kind, int cell, int clamp, float depth, int fogged, float f1,
                                                  float f2) {
    draw_batch();
    lw.batch_kind = kind;
    if (new_state) {
        set_state(s, list, table2, kind == B_3D, cell >= 0 ? &atlas_tex : NULL);
        lw.cur_state = si;
        lw.cur_table2 = table2;
        lw.cur_cell = cell >= 0;
    }
    if (lw.ge.tex == 1) {
        tex_levels(kind == B_3D);
        set_wrap(clamp ? GU_CLAMP | GU_CLAMP << 8 : state_wrap(s));
    }
    lw.cur_clamp = clamp;
    if (kind == B_3D) {
        if (depth == AFFINE) {
            set_depth_value(aff_scale, aff_centre + lw.zbias);
        } else {
            set_depth_value(0.0f, depth);
        }
        lw.cur_depth = depth;
        if (fogged) {
            set_fog_line(f1, f2);
        }
        lw.cur_fog1 = f1;
        lw.cur_fog2 = f2;
    }
    set_enable(&lw.ge.fog, fogged, GU_FOG);
}

/* The vertices of a triangle put_one has taken, each kind in a routine of its own: put_one's decisions, which every
   triangle runs, are then all of it the cache holds for them. */

/* a sprite from corner a to corner c, colour cc */
static __attribute__((noinline)) void put_sprite(const RD_VTX* a, const RD_VTX* c, float flat_at, uint32_t cc) {
    VTX* o = &verts[lw.nverts];
    put_vertex_2d(&o[0], a, flat_at, cc);
    put_vertex_2d(&o[1], c, flat_at, cc);
    lw.nverts += 2;
    lw.st.tris += 2;
    lw.st.sprites++;
}

static __attribute__((noinline)) void put_flat(const TV* t, const uint32_t* col, int table2, int opaque_alpha,
                                               float flat_at) {
    VTX* o = &verts[lw.nverts];
    int k;
    for (k = 0; k < 3; k++) {
        put_vertex_2d(&o[k], t->v[k], flat_at,
                      table2 ? table2_colour(t->v[0]->z) : vertex_colour(col[k], opaque_alpha));
    }
    lw.nverts += 3;
}

static __attribute__((noinline)) void put_3d(const TV* t, const uint32_t* col, int table2, int opaque_alpha,
                                             int overlay, float depth) {
    VTX* o = &verts[lw.nverts];
    int k;
    for (k = 0; k < 3; k++) {
        const RD_VTX* v = t->v[k];
        put_vertex(&o[k], v->x, v->y, v->z, v->u, v->v,
                   table2 ? table2_colour(v->z) : vertex_colour(col[k], opaque_alpha));
    }
    if (overlay) {
        float z[3] = { t->v[0]->z, t->v[1]->z, t->v[2]->z };
        add_over(o, z, depth);
    }
    lw.nverts += 3;
}

static __attribute__((noinline)) void take_state(int si) {
    const RD_STATE* s = &list_frame->states[si];
    lw.put_st.si = si;
    lw.put_st.s = s;
    lw.put_st.cell = state_tex[si].cell;
    lw.put_st.no_texels = state_tex[si].no_texels;
    lw.put_st.table2 = s->fog == RD_FOG_TABLE2;
    lw.put_st.fog = s->fog == RD_FOG_TABLE && fog_on && !fog_off;
    lw.put_st.plain_tex = state_tex[si].plain;
    lw.put_st.opaque_alpha = (uint8_t)opaque_alpha_of(s);
}

static float flat_depth_at(float z) {
    if (z != lw.flat_z_was || lw.flat_frame != frame_no) {
        lw.flat_z_was = z;
        lw.flat_frame = frame_no;
        lw.flat_depth_was = flat_depth(z);
    }
    return lw.flat_depth_was;
}

/* triangle t, and the next (NULL: none) where the two are a sprite; how many were taken */
static int put_one(int list, int si, const TV* t, const TV* next) {
    const RD_STATE* s;
    const RD_VTX *a = t->v[0], *b = t->v[1], *c = t->v[2];
    float depth, f1 = 0, f2 = 0;
    int flat = is_flat(t), fogged, overlay, table2, opaque_alpha, k, guard, kind, sprite;
    float flat_at;
    uint32_t col[3];
    int clamp, cell, same;
    if (si != lw.put_st.si) {
        take_state(si);
    }
    s = lw.put_st.s;
    if (probe == P_MODELS_ONLY || (!t->stays && culled(s, t))) {
        return 1;
    }
    if (!usable_z(a->z) || (!flat && (!usable_z(b->z) || !usable_z(c->z)))) {
        lw.st.skipped++;
        return 1;
    }
    if ((a->x < 0 && b->x < 0 && c->x < 0) || (a->y < 0 && b->y < 0 && c->y < 0) ||
        (a->x > RD_WIDTH && b->x > RD_WIDTH && c->x > RD_WIDTH) ||
        (a->y > RD_HEIGHT && b->y > RD_HEIGHT && c->y > RD_HEIGHT)) {
        return 1;
    }
    if (lw.put_st.no_texels) {
        lw.st.skipped++;
        return 1;
    }
    table2 = lw.put_st.table2;
    fogged = lw.put_st.fog && (a->z < fog_cut || (!flat && (b->z < fog_cut || c->z < fog_cut)));
    depth = flat && (!has3d || a->z < lo3 || a->z > hi3) ? flat_depth_at(a->z) : AFFINE;
    /* the opaque list's fog drawn over it after; the others' by the GE's, which takes a line a triangle */
    overlay = fogged && list == RD_OPAQUE && lw.nover + 3 * (MAX_CORNERS - 2) <= MAX_OVER;
    fogged = fogged && !overlay;
    if (fogged) {
        fog_line(t, &f1, &f2);
    }
    if (lw.nverts + 3 * (MAX_CORNERS - 2) > MAX_VERTS) {
        lw.st.dropped++;
        return 1;
    }
    write_ahead();
    guard = a->x < -GUARD || b->x < -GUARD || c->x < -GUARD || a->y < -GUARD || b->y < -GUARD || c->y < -GUARD ||
            a->x > RD_WIDTH + GUARD || b->x > RD_WIDTH + GUARD || c->x > RD_WIDTH + GUARD ||
            a->y > RD_HEIGHT + GUARD || b->y > RD_HEIGHT + GUARD || c->y > RD_HEIGHT + GUARD;
    /* a flat triangle at a depth of its own has no perspective: through mode, its depth in its vertices */
    kind = flat && !fogged && !overlay && !guard && lw.put_st.plain_tex ? B_FLAT : B_3D;
    flat_at = depth == AFFINE ? aff_centre + lw.zbias + aff_slope * (a->z - aff_lo) : depth;
    sprite = kind == B_FLAT && next != NULL && sprite_pair(s, t, next) && !culled(s, next);
    kind = sprite ? B_SPRITES : kind;
    clamp = kind != B_3D && s->tex != NULL && inside_uv(a, b, c) && (!sprite || inside_uv(a, b, next->v[2]));
    cell = clamp ? lw.put_st.cell : -1;
    same = cell >= 0 ? lw.cur_cell && lw.cur_state >= 0 && same_look(si, lw.cur_state) : !lw.cur_cell && si == lw.cur_state;
    if (lw.nverts - lw.batch_first + 3 * (MAX_CORNERS - 2) > MAX_BATCH || !same || table2 != lw.cur_table2 ||
        kind != lw.batch_kind || clamp != lw.cur_clamp ||
        (kind == B_3D && (depth != lw.cur_depth || (fogged && (f1 != lw.cur_fog1 || f2 != lw.cur_fog2))))) {
        if (ge_used() + LIST_ROOM > LIST_BYTES) {
            lw.st.dropped++;
            return 1;
        }
        begin_batch(s, list, si, !same || table2 != lw.cur_table2, table2, kind, cell, clamp, depth, fogged, f1, f2);
    }
    if (cell >= 0) {
        lw.tex_w = lw.tex_h = (float)state_tex[si].width;
        lw.tex_u0 = (float)(cell % CELLS_X * CELL_W + 2);
        lw.tex_v0 = (float)(cell / CELLS_X * CELL_H + 1);
    }
    opaque_alpha = lw.put_st.opaque_alpha;
    for (k = 0; k < 3; k++) {
        col[k] = s->flat ? c->base : t->v[k]->base;
        if (s->offset && (t->v[k]->offset & 0xFFFFFFu) != 0) {
            lw.st.offset_tris++;
        }
    }
    if (kind == B_SPRITES) {
        put_sprite(a, c, flat_at, table2 ? table2_colour(a->z) :
                                  vertex_colour(s->flat ? next->v[2]->base : a->base, opaque_alpha));
        return 2;
    }
    if (kind == B_FLAT) {
        put_flat(t, col, table2, opaque_alpha, flat_at);
    } else if (!guard) {
        put_3d(t, col, table2, opaque_alpha, overlay, depth);
    } else {
        put_clipped(t, col, table2, opaque_alpha, overlay, depth);
    }
    lw.st.tris++;
    return 1;
}

static void tv_of(TV* o, const RD_TRI* t) {
    o->v[0] = &t->v[0];
    o->v[1] = &t->v[1];
    o->v[2] = &t->v[2];
    o->state = t->state;
    o->share = t->share;
    o->stays = t->stays;
}

/* triangle j of run t, as rd_strip added it */
static void run_tv(const RD_TRI* t, int j, TV* o) {
    int odd = ((t->stays & RD_ODD) != 0) ^ (j & 1);
    o->state = t->state;
    o->share = j == 0 ? t->share : odd ? RD_SHARE(2, 1) : RD_SHARE(0, 2);
    o->stays = 1;
    o->v[0] = rd_run_corner(t, odd ? j + 1 : j);
    o->v[1] = rd_run_corner(t, odd ? j : j + 1);
    o->v[2] = rd_run_corner(t, j + 2);
}

/* item t (a triangle, RD_QUAD or RD_STRIP) and the next item, whose first triangle is as stored, as put_one takes their
   triangles: 2 if the next was taken too */
static int put_tri(int list, int si, const RD_TRI* t, const RD_TRI* next) {
    TV q[2], nx;
    const TV* after = NULL;
    int j, n, c;
    if (next != NULL) {
        tv_of(&nx, next);
        after = &nx;
    }
    if (!(t->stays & (RD_QUAD | RD_STRIP))) {
        tv_of(&q[0], t);
        return put_one(list, si, &q[0], after);
    }
    if (t->stays & RD_QUAD) {
        RD_VTX d;
        rd_quad_last(&t->v[0], &t->v[1], &t->v[2], &d);
        tv_of(&q[0], t);
        q[0].stays = t->stays & 1;
        q[1] = q[0];
        q[1].v[1] = &t->v[2];
        q[1].v[2] = &d;
        q[1].share = RD_SHARE(0, 2);
        return put_one(list, si, &q[0], &q[1]) == 2 ? 1 : put_one(list, si, &q[1], after);
    }
    n = t[1].state;
    c = 0;
    run_tv(t, 0, &q[0]);
    for (j = 0;;) {
        if (j + 1 == n) {
            return put_one(list, si, &q[c], after);
        }
        run_tv(t, j + 1, &q[c ^ 1]);
        if (put_one(list, si, &q[c], &q[c ^ 1]) == 2) {
            j += 2;
            if (j == n) {
                return 1;
            }
            run_tv(t, j, &q[c]);
        } else {
            j++;
            c ^= 1;
        }
    }
}


static uint32_t argb_scaled(const float* argb, float k) {
    return nl_float_argb(argb[0], argb[1] * k, argb[2] * k, argb[3] * k);
}

/* group d of list `list`: its mesh with its put's matrix as the view, so that the GE's eye is x * w, y * w, w as the
   CPU's corners are, and its lighting in the model's space */
static void set_ambient(uint32_t abgr_colour) {
    if (abgr_colour != lw.ge.ambient || !lw.ge.ambient_known) {
        lw.ge.ambient = abgr_colour;
        lw.ge.ambient_known = 1;
        ge_i(GE_LIGHT_AMBIENT, abgr_colour);
        ge_i(GE_LIGHT_AMBIENT_ALPHA, abgr_colour >> 24);
    }
}

static void set_colour_material(int what) {
    if (what != lw.ge.colour_material) {
        lw.ge.colour_material = what;
        ge_i(GE_MATERIAL_COLOUR, (unsigned)what);
    }
}

static void draw_group(int list, const RD_GPU_DRAW* d) {
    const RD_STATE* s = &list_frame->states[d->state];
    const RD_GPU_GROUP* g = &d->g;
    const GMESH* e = &gmesh[g->mesh];
    const float* m = list_frame->puts[d->put].m;
    ScePspFMatrix4 v;
    float cols[16];                 /* the view's columns: x * w, y * w and w from the model's x, y, z and 1 */
    int fogged, i, lit = 0;
    if (probe == P_NO_MODELS) {
        return;
    }
    if (ge_used() + LIST_ROOM > LIST_BYTES) {
        lw.st.dropped++;
        return;
    }
    write_ahead();
    draw_batch();
    /* the state of the triangles or model before it, but for its depth test, kept */
    if (d->state != lw.cur_state || lw.cur_cell || lw.cur_table2) {
        set_state(s, list, 0, 1, NULL);
    } else if (lw.ge.tex == 1) {
        tex_levels(1);
        set_wrap(state_wrap(s));
    }
    lw.cur_state = d->state;
    lw.cur_cell = lw.cur_table2 = lw.cur_clamp = 0;
    if (g->faces != 0 && lw.ge.depth == GU_GEQUAL) {
        /* a shell over a model's faces, a hair behind them where it meets them: not drawn where the depths tie */
        lw.ge.depth = GU_GREATER;
        ge_i(GE_Z_TEST, GU_GREATER);
        lw.cur_state = -1;
    }
    lw.batch_kind = -1;                /* the next triangles' depth and fog set again */
    set_depth_value(aff_scale, aff_centre);
    lw.cur_depth = AFFINE;
    fogged = s->fog == RD_FOG_TABLE && fog_on && !fog_off && g->whi > fog_cut_w;
    if (fogged) {
        /* the GE's fog line through the group's nearest and farthest w */
        float gn = 1.0f - fog_value(1.0f / g->wlo), gf = 1.0f - fog_value(1.0f / g->whi);
        if (gn == gf || g->whi == g->wlo) {
            set_fog_line(gn / 1e-10f - g->wlo, 1e-10f);
        } else {
            float f2 = (gf - gn) / (g->whi - g->wlo);
            set_fog_line(gn / f2 - g->wlo, f2);
        }
    }
    set_enable(&lw.ge.fog, fogged, GU_FOG);
    for (i = 0; i < 4; i++) {
        cols[4 * i] = m[4 * i + 1];
        cols[4 * i + 1] = m[4 * i + 2];
        cols[4 * i + 2] = m[4 * i];
        cols[4 * i + 3] = i == 3;
    }
    memcpy(&v, cols, sizeof v);
    ge_matrix(GU_VIEW, &v);
    lw.ge.view = 0;
    if (g->kind == RD_GPU_UNLIT) {
        ge_material(GU_AMBIENT | GU_DIFFUSE | GU_SPECULAR, vertex_colour(g->face_argb, opaque_alpha_of(s)));
    } else if (g->kind == RD_GPU_VTXCOL) {
        if (!(g->face[0] == 1.0f && g->face[1] == 1.0f && g->face[2] == 1.0f && g->face[3] == 1.0f)) {
            /* the face times the vertex's colour: the ambient light on the vertex's colour as its ambient */
            lit = 1;
            set_ambient(abgr(g->face_argb));
            set_colour_material(GU_AMBIENT);
            set_enable(&lw.ge.light0, 0, GU_LIGHT0);
        }
    } else {
        /* the face times the ambient and the light's side: one light, white, the face its diffuse */
        lit = 1;
        set_ambient(vertex_colour(argb_scaled(g->face, g->ambient), opaque_alpha_of(s)));
        set_colour_material(0);
        if ((abgr(g->face_argb) | 0xFF000000u) != lw.ge.diffuse) {
            lw.ge.diffuse = abgr(g->face_argb) | 0xFF000000u;
            ge_material(GU_DIFFUSE, lw.ge.diffuse);
        }
        if (g->l[0] != lw.ge.light[0] || g->l[1] != lw.ge.light[1] || g->l[2] != lw.ge.light[2]) {
            ScePspFVector3 l = { g->l[0], g->l[1], g->l[2] };
            memcpy(lw.ge.light, g->l, sizeof lw.ge.light);
            ge_light(0, GU_DIRECTIONAL, &l);
        }
        set_enable(&lw.ge.light0, 1, GU_LIGHT0);
    }
    set_enable(&lw.ge.lighting, lit, GU_LIGHTING);
    for (i = 0; i < e->nruns; i++) {
        const RUN* r = &e->runs[i];
        set_cull(g->faces == 1 ? 2 : g->faces == 2 ? 3 : r->cull);     /* a shell's back faces, then its front */
        set_flat(r->flat);
        ge_draw(GU_TRIANGLES, e->vtype, r->count * 3, e->idx + r->first * 3, e->verts);
        lw.st.group_tris += r->count;
    }
    lw.st.groups++;
    lw.st.batches++;
}

/* ---- fog ---- */

/* rd_soft.c's: the fog table at 1/w z, 16 entries a doubling of density * z, the two either side blended */
static float fog_value(float z) {
    float q = list_fog.density * z, m, pos;
    uint32_t bits;
    int e, i;
    if (!(q >= 1.0f)) {
        q = 1.0f;
    } else if (q > 255.9999f) {
        q = 255.9999f;
    }
    memcpy(&bits, &q, 4);
    e = (int)(bits >> 23) - 126;
    bits = (bits & 0x007FFFFFu) | 0x3F000000u;
    memcpy(&m, &bits, 4);
    pos = (m * 2.0f - 1.0f) * 16.0f + (float)((e - 1) * 16);
    i = (int)pos;
    if (i >= 127) {
        return list_fog.table[127];
    }
    return list_fog.table[i] + (list_fog.table[i + 1] - list_fog.table[i]) * (pos - (float)i);
}


/* ---- the frame ---- */

/* the background plane over the whole game: its corners' colours and depths from its three */
static void draw_background(void) {
    static const float corner[4][2] = { { 0, 0 }, { RD_WIDTH, 0 }, { 0, RD_HEIGHT }, { RD_WIDTH, RD_HEIGHT } };
    static const int order[6] = { 0, 1, 2, 2, 1, 3 };
    const RD_VTX *a = &list_frame->background[0], *b = &list_frame->background[1], *c = &list_frame->background[2];
    float area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
    int flat = a->z == b->z && b->z == c->z, i, k;
    VTX v[4];
    for (i = 0; i < 4; i++) {
        float x = corner[i][0], y = corner[i][1], w0 = 1, w1 = 0, w2 = 0, z;
        uint32_t col = 0;
        if (area != 0.0f && area == area) {
            w0 = ((c->x - b->x) * (y - b->y) - (c->y - b->y) * (x - b->x)) / area;
            w1 = ((a->x - c->x) * (y - c->y) - (a->y - c->y) * (x - c->x)) / area;
            w2 = 1 - w0 - w1;
        }
        for (k = 0; k < 4; k++) {
            int sh = 24 - 8 * k;
            col |= (uint32_t)clamp255(w0 * (float)(a->base >> sh & 255) + w1 * (float)(b->base >> sh & 255) +
                                      w2 * (float)(c->base >> sh & 255)) << sh;
        }
        z = flat ? a->z : w0 * a->z + w1 * b->z + w2 * c->z;
        put_vertex(&v[i], x, y, usable_z(z) ? z : 1e-30f, 0, 0, abgr(col));
    }
    set_enable(&lw.ge.tex, 0, GU_TEXTURE_2D);
    set_enable(&lw.ge.alpha_test, 0, GU_ALPHA_TEST);
    set_enable(&lw.ge.fog, 0, GU_FOG);
    ge_disable(GU_BLEND);
    lw.ge.blend = -1;
    ge_i(GE_Z_TEST, GU_ALWAYS);
    lw.ge.depth = GU_ALWAYS;
    ge_i(GE_Z_MASK, 0);
    lw.ge.zmask = 0;
    if (flat || !has3d) {
        set_depth_value(0.0f, flat_depth(usable_z(a->z) ? a->z : 1e-30f));
    } else {
        set_depth_value(aff_scale, aff_centre);
    }
    for (i = 0; i < 6; i++) {
        verts[lw.nverts++] = v[order[i]];
    }
    draw_batch();
}

/* the opaque list's fog: its colour blended over each fogged triangle where it is what was drawn */
static void draw_over(void) {
    int i = 0;
    if (lw.nover == 0 || probe == P_NO_FOG_OVER) {
        return;
    }
    draw_batch();
    set_enable(&lw.ge.tex, 0, GU_TEXTURE_2D);
    set_enable(&lw.ge.alpha_test, 0, GU_ALPHA_TEST);
    set_enable(&lw.ge.fog, 0, GU_FOG);
    if (lw.ge.blend != (RD_SRC_ALPHA | RD_INV_SRC_ALPHA << 8)) {
        lw.ge.blend = RD_SRC_ALPHA | RD_INV_SRC_ALPHA << 8;
        ge_enable(GU_BLEND);
        ge_blend(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    }
    if (lw.ge.depth != GU_EQUAL) {
        lw.ge.depth = GU_EQUAL;
        ge_i(GE_Z_TEST, GU_EQUAL);
    }
    if (lw.ge.zmask != 1) {
        lw.ge.zmask = 1;
        ge_i(GE_Z_MASK, 1);
    }
    while (i < lw.nover) {
        int j = i + 3;
        float d = lw.over_depth[i / 3];
        while (j < lw.nover && lw.over_depth[j / 3] == d) {
            j += 3;
        }
        if (d == AFFINE) {
            set_depth_value(aff_scale, aff_centre);
        } else {
            set_depth_value(0.0f, d);
        }
        plain();
        ge_draw(GU_TRIANGLES, VTYPE, j - i, NULL, &over[i]);
        lw.st.batches++;
        i = j;
    }
    lw.cur_state = -1;
}

/* the translucent list far to near, a polygon (a strip's triangles) at a time by its farthest 1/w, in its order where
   they're the same: a decal on a tilted polygon, its middle nearer than the decal's, still goes after it */

static uint32_t far_bits_one(const TV* t) {
    float lo = t->v[0]->z;
    uint32_t bits;
    lo = t->v[1]->z < lo ? t->v[1]->z : lo;
    lo = t->v[2]->z < lo ? t->v[2]->z : lo;
    if (!usable_z(lo)) {
        return 0;
    }
    memcpy(&bits, &lo, 4);
    return bits;
}

/* an item's: the least of its triangles' */
static uint32_t far_bits(const RD_TRI* t) {
    TV u;
    uint32_t bits, k;
    int j;
    tv_of(&u, t);
    bits = far_bits_one(&u);
    if (!(t->stays & RD_STRIP)) {
        return bits;
    }
    for (j = 1; j < t[1].state; j++) {
        run_tv(t, j, &u);
        k = far_bits_one(&u);
        bits = k < bits ? k : bits;
    }
    return bits;
}

/* the translucent list's items far to near, in trans_order: how many */
static int sort_trans(void) {
    const RD_TRI* tris = list_frame->tris[RD_TRANS];
    int n = list_frame->ntris[RD_TRANS], np = 0, npoly, i, m, pass, prev;
    uint32_t *key, *first, *order, *key2, *order2;
    key = lw.trans_order + TRANS_ROOM;
    first = key + TRANS_ROOM;
    order = first + TRANS_ROOM;
    key2 = order + TRANS_ROOM;
    order2 = key2 + TRANS_ROOM;
    for (i = 0, prev = 0; i < n; prev = i, i += rd_slots(&tris[i])) {
        uint32_t k;
        prefetch_tri(&tris[i + 4]);
        k = far_bits(&tris[i]);
        if (i == 0 || tris[i].share == 0 || tris[i].state != tris[prev].state) {
            first[np] = (uint32_t)i;
            key[np] = k;
            order[np] = (uint32_t)np;
            np++;
        } else if (k < key[np - 1]) {
            key[np - 1] = k;
        }
    }
    first[np] = (uint32_t)n;
    npoly = np;
    /* a group as a polygon, at its farthest 1/w and before one there; a shell's two passes at their halves' middles */
    for (m = 0; m < nlist_groups[RD_TRANS]; m++) {
        int gi = list_groups[RD_TRANS][m];
        const RD_GPU_GROUP* g = &list_frame->groups[gi].g;
        float k = g->faces != 0 ? g->key : 1.0f / g->whi;
        k = k > 0.0f ? k : 0.0f;
        memcpy(&key[np], &k, 4);
        key[np] -= key[np] != 0;
        order[np] = (uint32_t)np;
        lw.trans_groups[np - npoly] = (uint32_t)gi;
        np++;
    }
    /* a stable radix sort, 8 bits a pass, a pass where all have the same byte left out */
    for (pass = 0; pass < 4 && np > 1; pass++) {
        uint32_t count[257], *t;
        int sh = pass * 8;
        memset(count, 0, sizeof count);
        for (i = 0; i < np; i++) {
            count[(key[i] >> sh & 255) + 1]++;
        }
        if (count[(key[0] >> sh & 255) + 1] == (uint32_t)np) {
            continue;
        }
        for (i = 0; i < 256; i++) {
            count[i + 1] += count[i];
        }
        for (i = 0; i < np; i++) {
            uint32_t at = count[key[i] >> sh & 255]++;
            key2[at] = key[i];
            order2[at] = order[i];
        }
        t = key;
        key = key2;
        key2 = t;
        t = order;
        order = order2;
        order2 = t;
    }
    for (i = m = 0; i < np; i++) {
        uint32_t j;
        if (order[i] >= (uint32_t)npoly) {
            lw.trans_order[m++] = GROUP_ITEM | lw.trans_groups[order[i] - npoly];
            continue;
        }
        for (j = first[order[i]]; j < first[order[i] + 1]; j += (uint32_t)rd_slots(&tris[j])) {
            lw.trans_order[m++] = j;
        }
    }
    return m;
}

static void projection(int stretch) {
    float out_w = stretch ? SCR_W : SCR_H * 4.0f / 3.0f, ox = (SCR_W - out_w) / 2;
    ScePspFMatrix4 p, id;
    memset(&p, 0, sizeof p);
    memset(&id, 0, sizeof id);
    id.x.x = id.y.y = id.z.z = id.w.w = 1;
    /* x * w, y * w, w in; x and y as the screen's -1 to 1, and z / w = zk * (1 / w - aff_lo) out */
    lw.scr_kx = out_w / RD_WIDTH;
    lw.scr_ky = (float)SCR_H / RD_HEIGHT;
    lw.scr_x = ox;
    p.x.x = out_w / RD_WIDTH / (SCR_W / 2);
    p.z.x = (ox - SCR_W / 2) / (SCR_W / 2);
    p.y.y = -(float)SCR_H / RD_HEIGHT / (SCR_H / 2);
    p.z.y = 1.0f;
    p.w.z = zk;
    p.z.z = -zk * aff_lo;
    p.z.w = 1.0f;
    ge_matrix(GU_PROJECTION, &p);
    ge_matrix(GU_VIEW, &id);
    ge_matrix(GU_MODEL, &id);
    ge_scissor((int)(ox + 0.5f), 0, (int)(ox + out_w + 0.5f) - (int)(ox + 0.5f), SCR_H);    /* x, y, w, h */
}

/* the pillars in the bars beside the 4:3 picture, texel for pixel: each bar 59 pixels wide (the picture's 362.67 leaves
   58.67), in a 128 by 512 texture of two 64-texel columns */
#define PILLAR_W 59

static void pillars(const void* tex) {
    uint32_t* after = ge_list.cur + 2 + 20;
    float* v = (float*)(ge_list.cur + 2);
    int side;
    ge_list.cur[0] = (uint32_t)GE_BASE << 24 | (((unsigned)after >> 8) & 0xF0000u);
    ge_list.cur[1] = (uint32_t)GE_JUMP << 24 | ((unsigned)after & 0xFFFFFFu);
    ge_list.cur = after;
    for (side = 0; side < 2; side++) {
        float* s = v + side * 10;
        s[0] = side * 64;
        s[1] = 0;
        s[2] = side ? SCR_W - PILLAR_W : 0;
        s[3] = 0;
        s[4] = 0;
        s[5] = side * 64 + PILLAR_W;
        s[6] = SCR_H;
        s[7] = side ? SCR_W : PILLAR_W;
        s[8] = SCR_H;
        s[9] = 0;
    }
    ge_disable(GU_DEPTH_TEST);
    ge_disable(GU_BLEND);
    ge_disable(GU_ALPHA_TEST);
    ge_disable(GU_FOG);
    ge_enable(GU_TEXTURE_2D);
    ge_tex_mode(GU_PSM_8888, 0, 0);
    ge_tex_image(0, 128, 512, 128, tex);
    ge_i(GE_TEX_FUNC, GU_TFX_REPLACE | GU_TCC_RGB << 8);
    ge_i(GE_TEX_FILTER, GU_NEAREST | GU_NEAREST << 8);
    ge_i(GE_TEX_WRAP, GU_CLAMP | GU_CLAMP << 8);
    ge_draw(GU_SPRITES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 4, 0, v);
}

static void section_end(void) {
    if (sections) {
        draw_batch();
        ge_i(GE_SIGNAL, GU_SIGNAL_NOWAIT << 16 | 1);
        ge_i(GE_END, 0);
    }
}

/* the frame's call list made from rd's lists, as job asks */
void psp_list_job(void* job) {
    LIST_JOB* j = job;
    uint32_t (*clock)(void) = j->in.clock;
    uint32_t t0 = clock != NULL ? clock() : 0, t1, tp;
    int list, i;
    memset(&lw.st, 0, sizeof lw.st);
    lw.ahead = j->in.ahead;
    lw.nverts = lw.batch_first = lw.nover = 0;
    /* through the cache, written back whole before the GE is given it, or past it */
    ge_start(list_cached ? (void*)clist : (void*)((uintptr_t)clist | 0x40000000u));
    lw.verts_made = (uint8_t*)verts;
    lw.list_made = (uint8_t*)clist;
    ge_reset();
    lw.cur_state = -1;
    lw.put_st.si = -1;
    lw.cur_clamp = 0;
    lw.cur_cell = 0;
    lw.ntile_tex = 0;
    /* the whole screen until projection sets the picture's: nothing taken from the list before */
    ge_list.scissor_on = 0;
    ge_list.scissor[0] = ge_list.scissor[1] = 0;
    ge_list.scissor[2] = SCR_W - 1;
    ge_list.scissor[3] = SCR_H - 1;
    ge_enable(GU_SCISSOR_TEST);     /* the call list's own context sends the scissor only with it on */
    /* the background covers the picture: the whole screen cleared only for the bars, once in each buffer */
    if (j->in.clear) {
        ge_scissor(0, 0, SCR_W, SCR_H);
        ge_list.clear_colour = 0xFF000000u;
        ge_list.clear_depth = 0;
        ge_clear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT, SCR_W, SCR_H);
    }
    if (j->in.pillars != NULL && !j->in.stretch) {
        pillars(j->in.pillars);
    }
    projection(j->in.stretch);
    lw.ge.view = 1;
    ge_depth_range(65535, 0);
    if (probe == P_NO_DEPTH) {
        ge_disable(GU_DEPTH_TEST);
    } else {
        ge_enable(GU_DEPTH_TEST);
    }
    ge_enable(GU_CLIP_PLANES);
    ge_disable(GU_CULL_FACE);
    ge_disable(GU_LIGHTING);
    ge_disable(GU_LIGHT0);
    ge_i(GE_SHADE, GU_SMOOTH);
    lw.ge.cull = lw.ge.lighting = lw.ge.light0 = lw.ge.flat = 0;
    /* a lit model's: white ambient material and light, no emissive colour */
    ge_material(GU_AMBIENT, 0xFFFFFFFFu);
    ge_i(GE_LIGHT0_DIF, 0xFFFFFFFFu);
    ge_i(0x54, 0);                  /* no emissive colour */
    if (screen16) {
        static const ScePspIMatrix4 dither = { { -4, 0, -3, 1 }, { 2, -2, 3, -1 }, { -3, 1, -4, 0 }, { 3, -1, 2, -2 } };
        ge_dither(&dither);
        ge_enable(GU_DITHER);
    } else {
        ge_disable(GU_DITHER);
    }
    ge_i(GE_ALPHA_TEST, GU_GEQUAL | 128 << 8 | 0xFF << 16);
    ge_f(GE_TEX_SCALE_U, 1.0f);
    ge_f(GE_TEX_SCALE_V, 1.0f);
    ge_f(GE_TEX_OFFSET_U, 0.0f);
    ge_f(GE_TEX_OFFSET_V, 0.0f);
    ge_i(GE_TEX_MAP, GU_TEXTURE_COORDS);
    ge_i(GE_TEX_SHADE_MAP, 0);
    ge_fog(0.0f, 1.0f, fog_rgb);
    lw.batch_kind = B_3D;
    draw_background();
    section_end();
    tp = clock != NULL ? clock() : 0;
    lw.st.sec_us[0] = tp - t0;
    for (list = 0; list < RD_LISTS; list++) {
        const RD_TRI* tris = list_frame->tris[list];
        int n = list_frame->ntris[list], items = list == RD_TRANS && trans_sorted ? sort_trans() : -1;
        const uint16_t* lg = list_groups[list];
        int gk = 0, ng = nlist_groups[list];
        lw.zbias = list != RD_OPAQUE && list_frame->ngroups > 0 ? 1.0f : 0.0f;
        if (items >= 0) {
            /* sorted: triangles and groups far to near */
            for (i = 0; i < items;) {
                uint32_t it = lw.trans_order[i];
                if (i + 2 < items && !(lw.trans_order[i + 2] & GROUP_ITEM)) {
                    prefetch_tri(&tris[lw.trans_order[i + 2]]);
                }
                if (it & GROUP_ITEM) {
                    draw_group(list, &list_frame->groups[it & ~GROUP_ITEM]);
                    i++;
                } else {
                    const RD_TRI* next =
                        i + 1 < items && !(lw.trans_order[i + 1] & GROUP_ITEM) ? &tris[lw.trans_order[i + 1]] : NULL;
                    if (next != NULL) {
                        host_prefetch(&list_frame->states[next->state]);
                    }
                    i += put_tri(list, tris[it].state, &tris[it], next);
                }
            }
            section_end();
            t1 = clock != NULL ? clock() : 0;
            lw.st.sec_us[1 + list] = t1 - tp;
            tp = t1;
            continue;
        }
        /* in order: each group where its list's triangles sent before it end */
        for (i = 0; i < n;) {
            int j = i + rd_slots(&tris[i]);
            const RD_TRI* next = j < n ? &tris[j] : NULL;
            prefetch_tri(&tris[j + 1]);
            if (next != NULL) {
                host_prefetch(&list_frame->states[next->state]);
            }
            for (; gk < ng && list_frame->groups[lg[gk]].at <= i; gk++) {
                draw_group(list, &list_frame->groups[lg[gk]]);
            }
            i = put_tri(list, tris[i].state, &tris[i], next) == 2 ? j + rd_slots(&tris[j]) : j;
        }
        for (; gk < ng; gk++) {
            draw_group(list, &list_frame->groups[lg[gk]]);
        }
        if (list == RD_OPAQUE) {
            draw_over();
        }
        section_end();
        t1 = clock != NULL ? clock() : 0;
        lw.st.sec_us[1 + list] = t1 - tp;
        tp = t1;
    }
    draw_batch();
    j->out.bytes = ge_finish();
    j->out.nverts = lw.nverts;
    j->out.nover = lw.nover;
    j->out.ntile_tex = lw.ntile_tex;
    j->out.st = lw.st;
    j->out.took = clock != NULL ? clock() - t0 : 0;
}
