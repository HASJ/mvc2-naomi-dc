/*
 * nl_port.h  What the port's NAOMI library routines share
 */

#ifndef NL_PORT_H
#define NL_PORT_H

#include <stddef.h>
#include <stdint.h>

#include "rd.h"

/* a texture header, as the game's data holds them */
typedef struct {
    int16_t width, height;
    int32_t type;
    void* address;
    int32_t globl_index;
} NLthd;

typedef struct {
    RD_TEXTURE tex;
    int type;
    int loaded;
    int palette;        /* the bank a sprite draws with */
    size_t bytes;
    size_t later;       /* with nl_tex_later: where in texels its source waits for its first draw, else 0 */
    uint32_t used;      /* the frame given to the renderer that last drew it */
} NL_TEX;

#define NL_MAX_TEX 0x1000

extern NL_TEX nl_tex[NL_MAX_TEX];
extern int nl_tex_offset;
extern int nl_palette_direct;

int nl_tex_load(NL_TEX* t, const NLthd* thd);
int nl_tex_reload(NL_TEX* t, const void* texels);
void nl_tex_use(NL_TEX* t);         /* before a draw takes its texels */
void nl_tex_free(NL_TEX* t);        /* the game's free: its texels given back when the renderer is past them */
void nl_tex_frame(void);            /* before each frame is given to the renderer */
void nl_tex_free_all(void);
uint64_t nl_hash64(const void* src, size_t n);    /* a texture source's, as its cache keeps them */
int nl_lz_key(const void* p, size_t n, uint64_t* key);   /* bytes from the cell last unpacked: their key */
/* set by a front end before the game starts: PAL4 and VQ textures kept as their sources, rd_texture's unpack making
   their texels when the renderer wants them (not for rd_soft) */
extern int nl_tex_sources;
/* set by a front end before the game starts: a texture loaded while no frame in flight draws it is unpacked when it
   is first drawn (most of those a round loads never are); MVC2_TEX_LATER=0 or 1 overrides */
extern int nl_tex_later;
/* set by a front end whose renderer is done with a frame when it returns: freed texels go after the next frames
   whether drawn or not */
extern int nl_tex_sync;
/* set by a front end whose renderer reads a frame while the game makes the next: a texture's new texels go to new
   memory, its old given back as a free's are */
extern int nl_tex_fresh;
size_t nl_tex_held(void);           /* the bytes the textures take */

/* nlObjPutSetFadeColor's, 0 to 1 */
extern float nl_fade_r, nl_fade_g, nl_fade_b;

static inline uint16_t nl_palette_base(const NL_TEX* t, int bank) {
    return (uint16_t)(t->tex.format == RD_PAL8 ? (bank & 3) * 256 : (bank & 63) * 16);
}

/* 0 at or below 0, 255 at or above 1. The bounds are tested on the float's bits: on the ARM11 each
   VFP compare waits for the pipeline to drain. A NaN is converted as it is. */
static inline __attribute__((always_inline)) uint32_t nl_float_byte(float f) {
    union {
        float f;
        int32_t i;
    } u = { f };
    if ((u.i > 0 && u.i < 0x3F800000) || (u.i & 0x7FFFFFFF) > 0x7F800000) {
        return (uint32_t)(int)(f * 255.0f + 0.5f);
    }
    return u.i > 0 ? 255 : 0;
}

static inline __attribute__((always_inline)) uint32_t nl_float_argb(float a, float r, float g, float b) {
    return nl_float_byte(a) << 24 | nl_float_byte(r) << 16 | nl_float_byte(g) << 8 | nl_float_byte(b);
}

/* nl_xmtr.c's current matrix, by rows, and the view matrix */
const float* nl_xmtrx(void);
const float* nl_view_matrix(void);

/* nl_work.c: nlSetScaleFactor's scale, set back to 1 */
float nl_take_scale_factor(void);

#endif
