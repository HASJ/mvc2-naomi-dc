/*
 * sprite_host.c  sprite.c for the PSP
 *
 * A sprite's tile is a 32 by 32 PAL4 texture reloaded from the cells unpacked for the frame, and its sprite draws,
 * point sampled, only the corner its first bytes hold; the rest is whatever the buffer held after them. The reload is
 * told how many bytes that is, so the rest doesn't make each tile a new texture.
 */

#include "player.h"

extern int nlCommonTextureReloadDrawn( int index, const void *texels, int drawn ) ;

#define nlCommonTextureReloadA( tex, dst ) nlCommonTextureReloadDrawn( tex, dst, bytes )
#include "sprite.c"
#undef nlCommonTextureReloadA
