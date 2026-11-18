/* The GE's commands written straight into a call list, word for word as sceGu writes them, but with no state
   shared with sceGu's own lists, so the list can be made apart from them. */
#ifndef GE_LIST_H
#define GE_LIST_H

#include <stdint.h>
#include <pspgu.h>

/* the list being written; its lines its writer's own, as it may be another CPU */
typedef struct __attribute__((aligned(64))) {
    uint32_t *start, *cur;
    int scissor_on;
    int scissor[4];                 /* x0, y0, x1, y1 */
    unsigned clear_colour, clear_depth;
} GE_LIST;

extern GE_LIST ge_list;

enum {
    GE_VADDR = 0x01, GE_IADDR = 0x02, GE_PRIM = 0x04, GE_JUMP = 0x08, GE_RET = 0x0B, GE_END = 0x0C,
    GE_SIGNAL = 0x0E, GE_BASE = 0x10, GE_VTYPE = 0x12, GE_REGION1 = 0x15, GE_REGION2 = 0x16,
    GE_LIGHTING_ON = 0x17, GE_LIGHT0_ON = 0x18, GE_CLIP_ON = 0x1C, GE_CULL_ON = 0x1D, GE_TEXTURE_ON = 0x1E,
    GE_FOG_ON = 0x1F, GE_DITHER_ON = 0x20, GE_BLEND_ON = 0x21, GE_ALPHA_TEST_ON = 0x22, GE_Z_TEST_ON = 0x23,
    GE_WORLD_N = 0x3A, GE_WORLD = 0x3B, GE_VIEW_N = 0x3C, GE_VIEW = 0x3D, GE_PROJ_N = 0x3E, GE_PROJ = 0x3F,
    GE_Z_SCALE = 0x44, GE_Z_CENTRE = 0x47, GE_TEX_SCALE_U = 0x48, GE_TEX_SCALE_V = 0x49, GE_TEX_OFFSET_U = 0x4A,
    GE_TEX_OFFSET_V = 0x4B, GE_SHADE = 0x50, GE_MATERIAL_COLOUR = 0x53, GE_AMBIENT = 0x55, GE_DIFFUSE = 0x56,
    GE_SPECULAR = 0x57, GE_AMBIENT_ALPHA = 0x58, GE_LIGHT_AMBIENT = 0x5C, GE_LIGHT_AMBIENT_ALPHA = 0x5D,
    GE_LIGHT_TYPE0 = 0x5F, GE_LIGHT0_X = 0x63, GE_LIGHT0_AMB = 0x8F, GE_LIGHT0_DIF = 0x90, GE_LIGHT0_SPEC = 0x91,
    GE_CULL = 0x9B, GE_TEX_ADDR0 = 0xA0, GE_TEX_WIDTH0 = 0xA8, GE_CLUT_ADDR = 0xB0, GE_CLUT_WIDTH = 0xB1,
    GE_COPY_SRC = 0xB2, GE_COPY_SRC_W = 0xB3, GE_COPY_DST = 0xB4, GE_COPY_DST_W = 0xB5, GE_TEX_SIZE0 = 0xB8,
    GE_TEX_MAP = 0xC0, GE_TEX_SHADE_MAP = 0xC1, GE_TEX_MODE = 0xC2, GE_TEX_FORMAT = 0xC3, GE_CLUT_LOAD = 0xC4,
    GE_CLUT_FORMAT = 0xC5, GE_TEX_FILTER = 0xC6, GE_TEX_WRAP = 0xC7, GE_TEX_LEVEL = 0xC8, GE_TEX_FUNC = 0xC9,
    GE_TEX_FLUSH = 0xCB, GE_TEX_SYNC = 0xCC, GE_FOG1 = 0xCD, GE_FOG2 = 0xCE, GE_FOG_COLOUR = 0xCF,
    GE_CLEAR = 0xD3, GE_SCISSOR1 = 0xD4, GE_SCISSOR2 = 0xD5, GE_MIN_Z = 0xD6, GE_MAX_Z = 0xD7,
    GE_ALPHA_TEST = 0xDB, GE_Z_TEST = 0xDE, GE_BLEND = 0xDF, GE_BLEND_A = 0xE0, GE_BLEND_B = 0xE1, GE_DITHER0 = 0xE2,
    GE_Z_MASK = 0xE7, GE_COPY_START = 0xEA, GE_COPY_SRC_AT = 0xEB, GE_COPY_DST_AT = 0xEC, GE_COPY_SIZE = 0xEE,
};

static inline void ge_i(int cmd, unsigned arg) {
    *ge_list.cur++ = (uint32_t)cmd << 24 | (arg & 0xFFFFFFu);
}

static inline void ge_f(int cmd, float f) {
    union { float f; uint32_t i; } u;
    u.f = f;
    ge_i(cmd, u.i >> 8);
}

static inline void ge_start(void* list) {
    ge_list.start = ge_list.cur = list;
}

static inline unsigned ge_used(void) {
    return (unsigned)((uint8_t*)ge_list.cur - (uint8_t*)ge_list.start);
}

/* a call list's end */
static inline unsigned ge_finish(void) {
    ge_i(GE_RET, 0);
    return ge_used();
}

static inline void ge_scissor_words(void) {
    int end = ge_list.scissor[3] << 10 | ge_list.scissor[2];
    ge_i(GE_SCISSOR1, (unsigned)(ge_list.scissor[1] << 10 | ge_list.scissor[0]));
    ge_i(GE_SCISSOR2, (unsigned)end);
    ge_i(GE_REGION1, 0);
    ge_i(GE_REGION2, (unsigned)end);
}

static inline void ge_set(int state, int on) {
    static const uint8_t cmd[] = { GE_ALPHA_TEST_ON, GE_Z_TEST_ON, 0, 0x24, GE_BLEND_ON, GE_CULL_ON, GE_DITHER_ON,
                                   GE_FOG_ON, GE_CLIP_ON, GE_TEXTURE_ON, GE_LIGHTING_ON, GE_LIGHT0_ON };
    if (state == GU_SCISSOR_TEST) {
        /* on only: off would need the draw buffer's size */
        ge_list.scissor_on = 1;
        ge_scissor_words();
    } else {
        ge_i(cmd[state], (unsigned)on);
    }
}

static inline void ge_enable(int state) {
    ge_set(state, 1);
}

static inline void ge_disable(int state) {
    ge_set(state, 0);
}

static inline void ge_scissor(int x, int y, int w, int h) {
    ge_list.scissor[0] = x;
    ge_list.scissor[1] = y;
    ge_list.scissor[2] = x + w - 1;
    ge_list.scissor[3] = y + h - 1;
    if (ge_list.scissor_on) {
        ge_scissor_words();
    }
}

/* GU_PROJECTION's 4x4, the others' 4x3 */
static inline void ge_matrix(int type, const ScePspFMatrix4* m) {
    const float* f = (const float*)m;
    int i, j;
    if (type == GU_PROJECTION) {
        ge_f(GE_PROJ_N, 0.0f);
        for (i = 0; i < 16; i++) {
            ge_f(GE_PROJ, f[i]);
        }
        return;
    }
    int n = type == GU_VIEW ? GE_VIEW_N : GE_WORLD_N;
    ge_f(n, 0.0f);
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 3; j++) {
            ge_f(n + 1, f[j + i * 4]);
        }
    }
}

static inline void ge_draw(int prim, int vtype, int count, const void* indices, const void* vertices) {
    if (vtype) {
        ge_i(GE_VTYPE, (unsigned)vtype);
    }
    if (indices) {
        ge_i(GE_BASE, ((unsigned)indices >> 8) & 0xF0000u);
        ge_i(GE_IADDR, (unsigned)indices);
    }
    if (vertices) {
        ge_i(GE_BASE, ((unsigned)vertices >> 8) & 0xF0000u);
        ge_i(GE_VADDR, (unsigned)vertices);
    }
    ge_i(GE_PRIM, (unsigned)(prim << 16 | count));
}

static inline void ge_front_face(int order) {
    ge_i(GE_CULL, order ? 0 : 1);
}

static inline void ge_material(int mode, unsigned colour) {
    if (mode & GU_AMBIENT) {
        ge_i(GE_AMBIENT, colour);
        ge_i(GE_AMBIENT_ALPHA, (unsigned)((int)colour >> 24));     /* its sign spread, as sceGu's */
    }
    if (mode & GU_DIFFUSE) {
        ge_i(GE_DIFFUSE, colour);
    }
    if (mode & GU_SPECULAR) {
        ge_i(GE_SPECULAR, colour);
    }
}

static inline void ge_blend(int op, int src, int dest, unsigned fix_a, unsigned fix_b) {
    ge_i(GE_BLEND, (unsigned)(src | dest << 4 | op << 8));
    ge_i(GE_BLEND_A, fix_a);
    ge_i(GE_BLEND_B, fix_b);
}

static inline void ge_tex_mode(int psm, int max_mips, int swizzled) {
    ge_i(GE_TEX_MODE, (unsigned)(max_mips << 16 | swizzled));
    ge_i(GE_TEX_FORMAT, (unsigned)psm);
    ge_i(GE_TEX_FLUSH, 0);
}

static inline void ge_tex_level_mode(unsigned mode, float bias) {
    int offset = (int)(bias * 16.0f);
    offset = offset >= 128 ? 128 : offset < -128 ? -128 : offset;
    ge_i(GE_TEX_LEVEL, (unsigned)offset << 16 | mode);
}

static inline void ge_tex_image(int level, int w, int h, int tbw, const void* tbp) {
    ge_i(GE_TEX_ADDR0 + level, (unsigned)tbp);
    ge_i(GE_TEX_WIDTH0 + level, (((unsigned)tbp >> 8) & 0x0F0000u) | (unsigned)tbw);
    ge_i(GE_TEX_SIZE0 + level, (unsigned)((31 - __builtin_clz(h & 0x3FF)) << 8 | (31 - __builtin_clz(w & 0x3FF))));
    ge_i(GE_TEX_FLUSH, 0);
}

static inline void ge_clut_load(int blocks, const void* cbp) {
    ge_i(GE_CLUT_ADDR, (unsigned)cbp);
    ge_i(GE_CLUT_WIDTH, ((unsigned)cbp >> 8) & 0xF0000u);
    ge_i(GE_CLUT_LOAD, (unsigned)blocks);
}

/* a copy by the GE from main RAM to VRAM, then its texture cache flushed, written at w: past its words */
static inline uint32_t* ge_copy(uint32_t* w, int psm, int width, int height, int srcw, const void* src, int dx, int dy,
                                int destw, const void* dest) {
#define GE_WORD(c, a) ((uint32_t)(c) << 24 | ((unsigned)(a) & 0xFFFFFFu))
    w[0] = GE_WORD(GE_COPY_SRC, (unsigned)src);
    w[1] = GE_WORD(GE_COPY_SRC_W, (((unsigned)src & 0xFF000000u) >> 8) | (unsigned)srcw);
    w[2] = GE_WORD(GE_COPY_SRC_AT, 0);
    w[3] = GE_WORD(GE_COPY_DST, (unsigned)dest);
    w[4] = GE_WORD(GE_COPY_DST_W, (((unsigned)dest & 0xFF000000u) >> 8) | (unsigned)destw);
    w[5] = GE_WORD(GE_COPY_DST_AT, (unsigned)(dy << 10 | dx));
    w[6] = GE_WORD(GE_COPY_SIZE, (unsigned)((height - 1) << 10 | (width - 1)));
    w[7] = GE_WORD(GE_COPY_START, psm == GU_PSM_8888);
    w[8] = GE_WORD(GE_TEX_SYNC, 0);
    w[9] = GE_WORD(GE_TEX_FLUSH, 0);
#undef GE_WORD
    return w + 10;
}

static inline void ge_light(int light, int type, const ScePspFVector3* at) {
    ge_f(GE_LIGHT0_X + light * 3, at->x);
    ge_f(GE_LIGHT0_X + 1 + light * 3, at->y);
    ge_f(GE_LIGHT0_X + 2 + light * 3, at->z);
    ge_i(GE_LIGHT_TYPE0 + light, (unsigned)(type & GU_AMBIENT_AND_DIFFUSE) << 8);
}

static inline void ge_fog(float near, float far, unsigned colour) {
    float d = far - near;
    if (d != 0.0f) {
        d = 1.0f / d;
    }
    ge_i(GE_FOG_COLOUR, colour);
    ge_f(GE_FOG1, far);
    ge_f(GE_FOG2, d);
}

static inline void ge_dither(const ScePspIMatrix4* m) {
    const int* r = &m->x.x;
    int i;
    for (i = 0; i < 4; i++, r += 4) {
        ge_i(GE_DITHER0 + i, (unsigned)((r[0] & 15) | (r[1] & 15) << 4 | (r[2] & 15) << 8 | (r[3] & 15) << 12));
    }
}

static inline void ge_depth_range(int near, int far) {
    unsigned max = (unsigned)near + (unsigned)far;
    float z = (float)((int)((max >> 31) + max) >> 1);
    ge_f(GE_Z_SCALE, z - (float)near);
    ge_f(GE_Z_CENTRE, z);
    ge_i(GE_MIN_Z, (unsigned)(near < far ? near : far));
    ge_i(GE_MAX_Z, (unsigned)(near < far ? far : near));
}

/* the colour and depth buffers cleared by a sprite over the screen, its two corners in the list */
static inline void ge_clear(int flags, int width, int height) {
    uint32_t* v = ge_list.cur + 2;
    uint32_t* after = v + 6;
    ge_list.cur[0] = (uint32_t)GE_BASE << 24 | (((unsigned)after >> 8) & 0xF0000u);
    ge_list.cur[1] = (uint32_t)GE_JUMP << 24 | ((unsigned)after & 0xFFFFFFu);
    ge_list.cur = after;
    v[0] = v[3] = ge_list.clear_colour & 0xFFFFFFu;
    v[1] = 0;
    v[2] = ge_list.clear_depth;
    v[4] = (uint32_t)height << 16 | (uint32_t)width;
    v[5] = ge_list.clear_depth;
    ge_i(GE_CLEAR, (unsigned)(flags & (GU_COLOR_BUFFER_BIT | GU_STENCIL_BUFFER_BIT | GU_DEPTH_BUFFER_BIT)) << 8 | 1);
    ge_draw(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, v);
    ge_i(GE_CLEAR, 0);
}

#endif
