/*
 * rd_gpu.h  The SDL_GPU back end (SDL3): the frame drawn by the GPU, by rd_soft.c's rules.
 * MVC2_GPULAYERS sets the translucent layers (12, 4 with nosort); MVC2_GPUEDGES=0 the GPU's own edges.
 */

#ifndef RD_GPU_H
#define RD_GPU_H

#include <SDL3/SDL_gpu.h>

/* for SDL_CreateGPUDevice: DXBC for Direct3D 12, SPIR-V for Vulkan */
#define RD_GPU_SHADER_FORMATS (SDL_GPU_SHADERFORMAT_DXBC | SDL_GPU_SHADERFORMAT_SPIRV)

#define RD_GPU_MAX_SCALE 12

enum { RD_SPRITES_ARCADE, RD_SPRITES_SHARP, RD_SPRITES_PIXEL, RD_SPRITES_SMOOTH, RD_SPRITES_TEXEL };
enum { RD_HUD_SHARP, RD_HUD_SMOOTH, RD_HUD_ARCADE };

bool rd_gpu_init(SDL_GPUDevice* device);
/* From the next frame: drawn at resolution times the game's 640 by 480 (0: enough for h, times aa; exactly h times aa
   for texel sprites), then
   filtered down to w by h when that is smaller (0: not). Drawn larger, a sprite's texture is read as the
   arcade does, sharp (bilinear only across its texels' edges) or by the nearest texel, each at the game's pixels;
   texel reads it where its texels fall, each pixel the mean of the texels under it */
void rd_gpu_view(float resolution, int aa, int w, int h, int sprites);
/* the size rd_gpu_view's frame would be drawn at, when the GPU can make it */
void rd_gpu_view_size(float resolution, int aa, int h, int sprites, int* fw, int* fh);
/* drawn larger, what isn't a sprite: sharp as sprites are, smooth (read as the game filters it), or arcade (smooth,
   and the game's front in its own pixels, each a block of the pixels drawn) */
void rd_gpu_hud(int mode);
/* texel: each character sprite on its own texels' lattice (0), or (the default) every one on one grid of the CPS2's pixels, 5/3 by
   15/7 of the game's from the screen's corner, so that a character's parts drawn apart meet as its art does (1) */
void rd_gpu_texel_grid(int shared);
/* the frame drawn by cmd into the back end's texture (R8G8B8A8), in place of rd_end_frame */
SDL_GPUTexture* rd_gpu_end_frame(SDL_GPUCommandBuffer* cmd);
/* cmd submitted, once the frame two before it is done: a hidden or minimised window's swapchain holds nothing
   back, and frames run ahead unpaced pile up on the GPU */
void rd_gpu_submit(SDL_GPUCommandBuffer* cmd);
void rd_gpu_frame_size(int* w, int* h);     /* that texture's */
/* the last frame as ARGB, at 640 by 480 / at rd_gpu_frame_size's: waits for the GPU */
bool rd_gpu_read(uint32_t* argb);
bool rd_gpu_read_view(uint32_t* argb);
void rd_gpu_quit(void);

#endif
