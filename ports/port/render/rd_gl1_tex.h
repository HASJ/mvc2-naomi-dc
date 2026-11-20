/*
 * rd_gl1_tex.h  rd_gl1's texels on the CPU: palette textures in 16 bits where the palette fits a 16-bit form exactly,
 * and the sprites' 32 by 32 tiles kept in atlas pages, so that their triangles share a texture
 */

#ifndef RD_GL1_TEX_H
#define RD_GL1_TEX_H

#include <stdint.h>

#include "rd.h"

/* the 16-bit forms as sent: 1555 and 4444 in RGBA order, red lowest (GL_RGBA with the _REV types: Pinedra's driver
   reads GL_BGRA as GL_RGBA), 565 red highest; in rd.h's order */
enum { RD_F1555, RD_F565, RD_F4444, RD_FORMS };

/* the palette of the frame being drawn */
extern const uint32_t* rd_gl1_pal;

/* the 16-bit form the texture's palette entries all fit exactly, or -1 */
int rd_gl1_pal_form(const RD_TEXTURE* t, int palette);

/* n texels of an rd.h 16-bit format (its form) with red and blue swapped into the form's order */
void rd_gl1_swap16(const uint16_t* s, int n, int form, uint16_t* out);

/* n palette texels in a form */
void rd_gl1_expand16(const uint8_t* texels, int n, const RD_TEXTURE* t, int palette, int form, uint16_t* out);

/* n palette texels as GL_RGBA bytes */
void rd_gl1_expand32(const uint8_t* texels, int n, const RD_TEXTURE* t, int palette, uint32_t* out);

/* n texels of an rd.h 16-bit format as GL_RGBA bytes, each channel c of b bits as c * 255 / (2^b - 1) */
void rd_gl1_rgba16(const uint16_t* s, int n, int format, uint32_t* out);

#define RD_ATLAS 512                                /* a page's side: on Pinedra a 1024 page's update costs the core 2.5 ms a frame */
#define RD_TILE 32                                  /* a tile's */
#define RD_SLOT (RD_TILE + 2)                       /* a slot's: the tile and a texel each side, as repeat reads it */
#define RD_SLOTS_ROW (RD_ATLAS / RD_SLOT)

/* a state whose texture can be a tile: PAL4, 32 by 32, one level, point sampled, repeated */
int rd_gl1_tile_state(const RD_STATE* s);

/* the tile's slot in its form's page (*at: x + y * RD_ATLAS of its first texel), placed this frame if need be; 1 when
   its texels are to be sent (in the slot's rect, RD_SLOT square, from out), 0 when the page has them, -1 when the page
   is full of this frame's tiles */
int rd_gl1_tile(const RD_TEXTURE* t, int palette, int form, uint32_t frame, int* at, uint16_t* out);

/* the pages' slots all free (their texture remade) */
void rd_gl1_tiles_forget(void);

/* the sprites' tiles kept by what they hold (texels and palette entries), each in a texture of its own: an animation
   draws the same cells again and again from other textures. RD_POOL of them (2 KB each in 16 bits) */
#define RD_POOL 1024

/* a texture the pool can hold: PAL4, 32 by 32, one level */
int rd_gl1_pool_texture(const RD_TEXTURE* t);

/* the pool's slot holding the texture's contents with these palette entries (*slot), taken this frame: 1 when the
   slot was just given them and its texture is to be sent, 0 when it has them, -1 when every slot is in this frame's
   use */
int rd_gl1_pool(const RD_TEXTURE* t, int palette, uint32_t frame, int* slot);

/* a slot's count of contents given it: a texture found there earlier is still there while it stands */
uint32_t rd_gl1_pool_gen(int slot);

/* the slot drawn this frame: not to be given other contents before the next */
void rd_gl1_pool_use(int slot, uint32_t frame);

void rd_gl1_pool_forget(void);

#endif
