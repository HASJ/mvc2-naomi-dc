/* What the GE renderer's two halves share: the main CPU's (psp_render.c: textures, the depth layout, models' meshes,
   the frame's handing to the GE) and the list's (psp_list.c: the frame's triangles and models written as the GE's
   call list). */
#ifndef PSP_GE_H
#define PSP_GE_H

#include <stdint.h>
#include <pspgu.h>

#include "rd.h"
#include "rd_list.h"
#include "ge_list.h"

#define SCR_W 480
#define SCR_H 272
#define LIST_BYTES (512u << 10)
#define LIST_ROOM (4u << 10)        /* what a batch's state and draw may take */
#define MAX_VERTS 24576
#define MAX_OVER 12288              /* the opaque list's fog drawn over it */
#define MAX_BATCH 3072              /* vertices: a batch is drawn when it has this many */
#define GUARD 1024.0f               /* game pixels past the screen the GE takes without a cut */
#define MAX_CORNERS 9               /* a triangle cut by the screen's 4 sides */
#define AFFINE (-1.0f)              /* a batch's depth on the 3D range's scale, not a step of its own */

typedef struct {
    float u, v;
    uint32_t c;                     /* ABGR */
    float x, y, z;                  /* x * w, y * w, w */
} VTX;

#define VTYPE (GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D)
/* a flat triangle's, in the GE's through mode: the screen's pixel, its depth, the texture's texel */
#define VTYPE_2D (GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D)

/* a texture as the GE has it */
typedef struct {
    uint32_t serial;                /* 0: free */
    uint32_t used;                  /* the frame last drawn in */
    int16_t prev, next;             /* its kind's list, least recently drawn first; a free one's next */
    uint8_t kind;                   /* its slot size, or BIG */
    uint8_t levels;                 /* past the first */
    uint8_t own;                    /* of them, the source's; those after are made here, in ext */
    uint8_t swizzled;               /* in the GE's 16-byte by 8-row blocks */
    int psm, w, h;                  /* as the GE has it: at most 512 */
    uint16_t tbw[8];
    uint8_t* lv[8];
    uint8_t* mem;
    uint32_t bytes;
    uint8_t* ext;                   /* the levels made here: VRAM where it has room */
    uint32_t ext_bytes;
} GTEX;

/* the sprites' tiles' atlas: cells of a 4-bit texture, each a tile with a border of its edge texels */
#define ATLAS_SIDE 512
#define CELL_W 36
#define CELL_H 34
#define CELLS_X (ATLAS_SIDE / CELL_W)
#define CELLS (CELLS_X * (ATLAS_SIDE / CELL_H))
#define KEPT_ROW 32                 /* bytes: a bordered row, 16 of the GE's 16-bit pixels */
#define KEPT_BYTES (KEPT_ROW * CELL_H)

/* a triangle as put_one takes it: its corners where they are, in an item's slots or made beside it */
typedef struct {
    const RD_VTX* v[3];
    uint16_t state;
    uint8_t share, stays;
} TV;

static inline int usable_z(float z) {
    return z > 0.0f && z < 1e30f;
}

static inline uint32_t abgr(uint32_t argb) {
    return (argb & 0xFF00FF00u) | (argb >> 16 & 0xFFu) | (argb & 0xFFu) << 16;
}

/* the state's look takes no alpha from the vertices */
static inline int opaque_alpha_of(const RD_STATE* s) {
    return !s->use_alpha || (s->tex != NULL && (s->shading == RD_DECAL || s->shading == RD_MODULATE));
}

/* MVC2_GE_PROBE=start,frames: from frame start, each probe in turn for that many frames, their times logged */
enum { P_NONE, P_NO_TEX, P_NEAREST, P_NO_BLEND, P_NO_MODELS, P_MODELS_ONLY, P_NO_MIPS, P_NO_FOG_OVER, P_NO_DEPTH,
       P_MIP_BIAS, P_LIST_UNCACHED, P_NO_ATLAS, PROBES };

/* a model's mesh as the GE reads it, and its runs of triangles by how they are culled and shaded */
#define MAX_MESHES 1024

typedef struct {
    uint16_t first, count;          /* triangles */
    uint8_t cull, flat;
} RUN;

typedef struct {
    uint8_t* mem;                   /* NULL: free */
    uint32_t bytes, freed;          /* the frame after whose drawing it goes; 0 while it is drawn */
    int kind, vtype, nruns;
    int opaque;                     /* every vertex's alpha 1 */
    const void* verts;
    const uint16_t* idx;
    const RUN* runs;
    float lo[3], hi[3];
} GMESH;

/* each state's texture as the list takes it, made before the frame's triangles are put: its texture, or for a
   sprite's tile its cell in the atlas (-1: none), bordered rows and size; and what the triangles ask of its source */
typedef struct {
    const GTEX* tex;
    const uint8_t* rows;
    int16_t cell;
    uint16_t side, width;           /* a tile's: the texels its cell holds on a side, its width */
    uint8_t no_texels;              /* the source has none */
    uint8_t plain;                  /* no levels past the first */
} STATE_TEX;

/* what the list's writer counts, added into psp_rstats by the main CPU */
typedef struct {
    uint32_t sec_us[4];             /* set-up, the three lists: when it has a clock */
    uint32_t tris, batches, clipped, sprites, skipped, dropped, offset_tris, mirrored, groups, group_tris;
    uint32_t tiles_full;            /* tiles drawn untextured, past the frame's room for their own */
} LIST_STATS;

/* a frame's list as it is asked for and as it is made: each in lines of its own, the main CPU writing the one and
   the list's writer the other */
typedef struct {
    struct __attribute__((aligned(64))) {
        int stretch;
        int clear;                  /* the whole screen cleared first */
        const void* pillars;        /* the pillars' texture, drawn in the bars beside a 4:3 picture; NULL not */
        uint32_t (*clock)(void);    /* NULL: not timed */
        uint32_t ahead;             /* bytes of the list and vertices taken into the cache at once; 0 none */
    } in;
    struct __attribute__((aligned(64))) {
        unsigned bytes;
        int nverts, nover, ntile_tex;
        uint32_t took;              /* the whole job by its clock */
        LIST_STATS st;
    } out;
} LIST_JOB;

/* the main CPU's, for the list: all written before the job is posted, and left alone until it is done */
extern const RD_FRAME* list_frame;   /* the frame the list is made from */
extern uint32_t frame_no;
extern int probe, screen16, sections;
extern float mip_bias, front_bias;
extern unsigned int* clist;
extern VTX *verts, *over;
extern uint32_t* pal;
extern GTEX atlas_tex;
extern STATE_TEX state_tex[RD_MAX_STATES];
extern GMESH gmesh[MAX_MESHES];
extern uint16_t list_groups[RD_LISTS][RD_GPU_GROUPS];
extern int nlist_groups[RD_LISTS];
extern RD_FOG list_fog;             /* rd_fog, as it was */
extern int trans_sorted;            /* the translucent list sorted: not nosort */
extern int fog_on, fog_off;         /* the table isn't all 0; nofog */
extern float fog_cut;               /* the table is 0 from this 1/w nearer */
extern float fog_cut_w;             /* 1 / fog_cut */
extern uint32_t fog_rgb;            /* ABGR */
extern float lo3, hi3;              /* the 3D triangles' 1/w */
extern int has3d;
extern float zk;                    /* the GE's depth z is zk / w */
extern float aff_scale, aff_centre; /* the 3D range's viewport z */
extern float aff_slope, aff_lo;     /* its depth is aff_centre + aff_slope * (1 / w - aff_lo) */
extern int list_cached;             /* the list written through the cache this frame */
float flat_depth(float z);

/* the list's */
#define TILE_TEXTURES 32            /* a frame's tiles drawn wrapped: textures of their own */
extern uint8_t tile_mem[2][TILE_TEXTURES][32 * 16];
void psp_list_job(void* job);

#endif
