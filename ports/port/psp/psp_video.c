/*
 * psp_video.c  The screen: its buffers in VRAM, rd_soft's 640x480 frame drawn onto it scaled (as two textures read
 * from main RAM: the GE's are at most 512 wide), the flip, and screenshots
 */

#include <malloc.h>
#include <string.h>

#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspkernel.h>

#include "rd.h"
#include "psp_front.h"

#define SCR_W 480
#define SCR_H 272
#define BUF_W 512
#define DEPTH_BYTES (BUF_W * SCR_H * 2)

static unsigned int __attribute__((aligned(64))) list[16 << 10];
static void* draw_buf;      /* VRAM offsets, as sceGu takes them */
static int on, psm = GU_PSM_8888;
static uint32_t frame_bytes, vram_used;     /* a colour buffer's; the two and the 16-bit depth after them */

typedef struct {
    float u, v;
    float x, y, z;
} VTX;

/* the vblanks' times, from the display's count of lines: the count as one began and the lines a frame */
static uint32_t vb_acc, vb_lines, vb_line_ns;

static void vblank_lines(void) {
    uint32_t a;
    int i;
    sceDisplayWaitVblankStart();
    a = (uint32_t)sceDisplayGetAccumulatedHcount();
    for (i = 0; i < 4; i++) {
        sceDisplayWaitVblankStart();
    }
    vb_acc = (uint32_t)sceDisplayGetAccumulatedHcount();
    vb_lines = (vb_acc - a + 2) / 4;
    if (vb_lines < SCR_H || vb_lines > 2 * SCR_H) {
        vb_lines = 0;
    } else {
        vb_line_ns = (uint32_t)(1e9f / (sceDisplayGetFramePerSec() * (float)vb_lines));
    }
}

uint32_t psp_video_frame_us(void) {
    return vb_lines * vb_line_ns / 1000;
}

uint32_t psp_video_vblank_us(void) {
    uint32_t t = sceKernelGetSystemTimeLow(), l;
    if (vb_lines == 0) {
        return 0;
    }
    l = ((uint32_t)sceDisplayGetAccumulatedHcount() - vb_acc) % vb_lines;
    return t - l * vb_line_ns / 1000;
}

static uint8_t* vram_unc;   /* VRAM uncached, for psp_video_box */

/* a box in both colour buffers, written straight to VRAM: safe from an interrupt */
void psp_video_box(int x, int y, int w, int h, uint32_t abgr) {
    uint16_t c16 = (uint16_t)((abgr & 0xFF) >> 3 | (abgr >> 10 & 0x3F) << 5 | (abgr >> 19 & 0x1F) << 11);
    int b, i, j;
    if (vram_unc == NULL) {
        return;
    }
    for (b = 0; b < 2; b++) {
        uint8_t* buf = vram_unc + b * frame_bytes;
        for (j = y; j < y + h; j++) {
            for (i = x; i < x + w; i++) {
                if (psm == GU_PSM_5650) {
                    ((volatile uint16_t*)buf)[j * BUF_W + i] = c16;
                } else {
                    ((volatile uint32_t*)buf)[j * BUF_W + i] = abgr;
                }
            }
        }
    }
}

/* lines of text on a dark box in both colour buffers, straight to VRAM as psp_video_box; chosen's in yellow */
void psp_video_panel(int x, int y, int w, int h, const char* const* lines, int n, int chosen) {
    int b, i, k;
    if (vram_unc == NULL) {
        return;
    }
    psp_video_box(x, y, w, h, 0xFF402010u);
    pspDebugScreenSetColorMode(psm);
    pspDebugScreenEnableBackColor(0);
    for (b = 0; b < 2; b++) {
        pspDebugScreenSetBase((u32*)(vram_unc + b * frame_bytes));
        for (i = 0; i < n; i++) {
            for (k = 0; lines[i][k] != 0 && 8 + 8 * (k + 1) <= w; k++) {
                pspDebugScreenPutChar(x + 8 + 8 * k, y + 8 + 12 * i, i == chosen ? 0xFF00FFFFu : 0xFFFFFFFFu,
                                      (u8)lines[i][k]);
            }
        }
    }
    pspDebugScreenEnableBackColor(1);
}

int psp_video_init(int bits16) {
    psm = bits16 ? GU_PSM_5650 : GU_PSM_8888;
    frame_bytes = BUF_W * SCR_H * (bits16 ? 2 : 4);
    vram_used = 2 * frame_bytes + DEPTH_BYTES;
    draw_buf = (void*)0;
    sceGuInit();
    sceGuStart(GU_DIRECT, list);
    sceGuDrawBuffer(psm, draw_buf, BUF_W);
    sceGuDispBuffer(SCR_W, SCR_H, (void*)frame_bytes, BUF_W);
    sceGuDepthBuffer((void*)(2 * frame_bytes), BUF_W);
    sceGuOffset(2048 - SCR_W / 2, 2048 - SCR_H / 2);
    sceGuViewport(2048, 2048, SCR_W, SCR_H);
    sceGuScissor(0, 0, SCR_W, SCR_H);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuClearColor(0xFF000000);
    sceGuClear(GU_COLOR_BUFFER_BIT);
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
    vblank_lines();
    vram_unc = (uint8_t*)(0x40000000u | (uintptr_t)sceGeEdramGetAddr());
    pspDebugScreenInitEx(vram_unc, psm, 0);     /* the debug screen's font for psp_video_panel; buffer 0 is clear */
    on = 1;
    return 1;
}

void* psp_video_vram(uint32_t* size) {
    *size = sceGeEdramGetSize() - vram_used;
    return (char*)sceGeEdramGetAddr() + vram_used;
}

int psp_video_bits16(void) {
    return psm == GU_PSM_5650;
}

/* columns [u0, u1) of the frame, a texture from column u0, onto [x0, x1) of the screen */
static void strip(const uint32_t* frame, int u0, int u1, float x0, float x1, float y0, float y1) {
    VTX* v = sceGuGetMemory(2 * sizeof(VTX));
    int w = u1 - u0 > 256 ? 512 : u1 - u0 > 128 ? 256 : 128;
    sceGuTexImage(0, w, 512, RD_WIDTH, frame + u0);
    sceGuTexFlush();
    v[0].u = 0;
    v[0].v = 0;
    v[0].x = x0;
    v[0].y = y0;
    v[0].z = 0;
    v[1].u = (float)(u1 - u0);
    v[1].v = RD_HEIGHT;
    v[1].x = x1;
    v[1].y = y1;
    v[1].z = 0;
    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, NULL, v);
}

void psp_video_show(const uint32_t* frame, int stretch) {
    float w = stretch ? SCR_W : SCR_H * 4.0f / 3.0f, x = (SCR_W - w) / 2, split = x + w * 512 / RD_WIDTH;
    if (!on) {
        return;
    }
    sceKernelDcacheWritebackRange(frame, RD_WIDTH * RD_HEIGHT * 4);
    sceGuStart(GU_DIRECT, list);
    sceGuScissor(0, 0, SCR_W, SCR_H);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_FOG);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuClear(GU_COLOR_BUFFER_BIT);
    strip(frame, 0, 512, x, split, 0, SCR_H);
    strip(frame, 512, RD_WIDTH, split, x + w, 0, SCR_H);
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
}

void psp_video_target(int bits16) {
    (void)bits16;
    sceGuDrawBuffer(psm, draw_buf, BUF_W);
}

static unsigned flip_v = ~0u;   /* the vblank count at the last flip */
static uint32_t shown_us;       /* the vblank it is shown from: latched, the next; else this one's if flipped in its
                                   blank */
static int latched;             /* flips shown from the next vblank, never part way down the screen */

void psp_video_latch(int on) {
    latched = on;
}

/* until the vblank count is past v: a vblank begun since it was read not waited for again, nor one too near to be
   waited for (looked for instead) */
void psp_video_wait_past(unsigned v) {
    uint32_t t = sceKernelGetSystemTimeLow();
    if (vb_lines != 0 && psp_video_vblank_us() + psp_video_frame_us() - t < 300) {
        while (sceDisplayGetVcount() == v && sceKernelGetSystemTimeLow() - t < 600) {
        }
    }
    while (sceDisplayGetVcount() == v) {
        if (sceDisplayIsVblank()) {
            sceDisplayWaitVblankStart();
        } else {
            sceDisplayWaitVblank();
        }
    }
}

/* the frame shown from the next vblank; a second flip before that vblank waits for it, so no frame is lost */
void psp_video_flip(void) {
    if (on) {
        if (psp_paced) {
            psp_video_wait_past(flip_v);
        }
        guSwapBuffersBehaviour(latched ? PSP_DISPLAY_SETBUF_NEXTFRAME : PSP_DISPLAY_SETBUF_IMMEDIATE);
        draw_buf = sceGuSwapBuffers();
        flip_v = sceDisplayGetVcount();
        shown_us = psp_video_vblank_us();
        if (latched || sceKernelGetSystemTimeLow() - shown_us >= (vb_lines - SCR_H) * vb_line_ns / 1000) {
            shown_us += psp_video_frame_us();
        }
    }
}

uint32_t psp_video_shown_us(void) {
    return shown_us;
}

/* the buffer to draw in no longer on the screen: the vblank after the last flip has passed */
void psp_video_wait_back(void) {
    if (on) {
        psp_video_wait_past(flip_v);
    }
}

/* the drawn screen copied by the GE into main RAM, where the CPU reads it */
int psp_video_shot(const char* name) {
    uint32_t* px = memalign(64, SCR_W * SCR_H * 4);
    if (px == NULL || !on) {
        free(px);
        return 0;
    }
    sceKernelDcacheWritebackInvalidateRange(px, SCR_W * SCR_H * 4);
    sceGuStart(GU_DIRECT, list);
    sceGuCopyImage(psm, 0, 0, SCR_W, SCR_H, BUF_W, (char*)sceGeEdramGetAddr() + (uintptr_t)draw_buf, 0, 0, SCR_W, px);
    sceGuTexSync();
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    sceKernelDcacheInvalidateRange(px, SCR_W * SCR_H * 4);
    if (psm == GU_PSM_5650) {
        /* 5:6:5 to 8:8:8, from the last pixel back, in place */
        const uint16_t* p16 = (const uint16_t*)px;
        int i;
        for (i = SCR_W * SCR_H - 1; i >= 0; i--) {
            uint32_t c = p16[i], r = c & 31, g = c >> 5 & 63, b = c >> 11;
            px[i] = 0xFF000000u | (b << 3 | b >> 2) << 16 | (g << 2 | g >> 4) << 8 | (r << 3 | r >> 2);
        }
    }
    psp_save_bmp(name, px, SCR_W, SCR_H, SCR_W);
    free(px);
    return 1;
}

void psp_video_text(const char* title, const char* text) {
    if (on) {
        sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
        sceGuTerm();
        on = 0;
    }
    pspDebugScreenInit();
    pspDebugScreenClear();
    pspDebugScreenPrintf("%s\n\n%s\n", title, text);
}
