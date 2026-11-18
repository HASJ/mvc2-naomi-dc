/*
 * psp_art.c  The PSP's own pictures: the pillars beside the game's 4:3 picture (port/psp/gfx, the 3DS's at 1:1)
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <malloc.h>
#include <pspkernel.h>

#include "naomi_host.h"
#include "psp_front.h"
#include "miniz.h"
#include "psp_art_png.h"

static uint32_t be32(const unsigned char* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static int paeth(int a, int b, int c) {
    int pa = abs(b - c), pb = abs(a - c), pc = abs(a + b - 2 * c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

/* an 8-bit RGBA PNG, cols by max_h at most, into out (stride texels a row, as the GE reads them: R in the low byte),
   each row's last texel repeated to cols */
static int png_rgba(const unsigned char* png, size_t size, uint32_t* out, int stride, int cols, int max_h) {
    size_t at = 8, nz = 0, room;
    unsigned char *z, *raw;
    uint32_t w = 0, h = 0, x, y;
    int ok = 0;
    if (size < 8 || (z = malloc(size)) == NULL) {
        return 0;
    }
    while (at + 12 <= size) {
        uint32_t n = be32(png + at);
        const unsigned char* t = png + at + 4;
        if (at + 12 + n > size) {
            break;
        }
        if (memcmp(t, "IHDR", 4) == 0 && n >= 13) {
            w = be32(t + 4);
            h = be32(t + 8);
            if (t[12] != 8 || t[13] != 6 || t[16] != 0) {
                break;      /* 8 bits, RGBA, not interlaced only */
            }
        } else if (memcmp(t, "IDAT", 4) == 0) {
            memcpy(z + nz, t + 4, n);
            nz += n;
        }
        at += 12 + n;
    }
    room = (size_t)(w * 4 + 1) * h;
    if (w == 0 || (int)w > cols || (int)h > max_h || (raw = malloc(room)) == NULL) {
        free(z);
        return 0;
    }
    if (tinfl_decompress_mem_to_mem(raw, room, z, nz, TINFL_FLAG_PARSE_ZLIB_HEADER) == room) {
        unsigned char* prev = NULL;
        for (y = 0; y < h; y++) {
            unsigned char* r = raw + (size_t)y * (w * 4 + 1);
            int f = r[0];
            for (x = 0; x < w * 4; x++) {
                int a = x >= 4 ? r[1 + x - 4] : 0, b = prev != NULL ? prev[x] : 0;
                int c = x >= 4 && prev != NULL ? prev[x - 4] : 0;
                r[1 + x] += f == 1 ? a : f == 2 ? b : f == 3 ? (a + b) / 2 : f == 4 ? paeth(a, b, c) : 0;
            }
            prev = r + 1;
            for (x = 0; x < w; x++) {
                out[y * stride + x] = 0xFF000000u | (uint32_t)prev[x * 4 + 2] << 16 | (uint32_t)prev[x * 4 + 1] << 8 |
                                      prev[x * 4];
            }
            for (; (int)x < cols; x++) {
                out[y * stride + x] = out[y * stride + w - 1];  /* the edge's texels again, for filtering */
            }
        }
        ok = 1;
    }
    free(raw);
    free(z);
    return ok;
}

/* the pillars wanted: not with pillars = off in mvc2.cfg (or MVC2_PILLARS=0) */
int psp_art_wanted(void) {
    const char* off = getenv("MVC2_PILLARS") != NULL ? getenv("MVC2_PILLARS") : config_get("pillars");
    return off == NULL || (strcmp(off, "0") != 0 && strcmp(off, "off") != 0);
}

/* the left and right pillars, 59 by 272, in columns 0 and 64 of a 128-texel-wide texture; NULL if they can't be
   read */
const uint32_t* psp_art_pillars(void) {
    size_t size = 128 * 272 * 4;
    uint32_t* t = memalign(64, size);
    if (t == NULL) {
        return NULL;
    }
    if (!png_rgba(left_png, sizeof left_png, t, 128, 64, 272) ||
        !png_rgba(right_png, sizeof right_png, t + 64, 128, 64, 272)) {
        host_log("pillars: their pictures could not be read");
        free(t);
        return NULL;
    }
    sceKernelDcacheWritebackRange(t, size);
    return t;
}
