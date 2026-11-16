/*
 * dc_board.c  The Dreamcast as the game sees it: its program from the disc, and the NAOMI library's
 * Dreamcast entries
 *
 * The console's boot ROM reads 1ST_READ.BIN to 8C010000. The program is linked there, in the P1
 * mirror of main RAM, and the port maps main RAM at 0C000000, so the image goes to 0C010000, and
 * gen_host masks the addresses in its data the same way. The rest of the board (the frame, the
 * interrupts, the timer) is the NAOMI's, which is the same chip set.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "naomi_host.h"

void gdFsFinish(void);

#define DC_PROGRAM_ADDRESS 0x0C010000u
#define DC_PROGRAM_MAX 0x00400000
#define DC_SCRAMBLE_MAX_CHUNK (2048u * 1024u)
#define DC_SCRAMBLE_SLICE 32u

static int dc_looks_unscrambled(const uint8_t* img, long size) {
    return size >= 0x40 && memcmp(img + 0x30, "Lib Handle Start", 16) == 0;
}

/* Dreamcast MIL-CD images store 1ST_READ.BIN scrambled in 32-byte slices; restore it if needed. */
static int dc_descramble_boot(uint8_t* dst, long size) {
    uint8_t* src;
    uint32_t* idx;
    uint32_t seed;
    size_t src_pos;
    size_t dst_pos;
    size_t rem;
    size_t chunk_sz;

    if ((size_t)size < DC_SCRAMBLE_SLICE || dc_looks_unscrambled(dst, size)) {
        return 1;
    }

    src = (uint8_t*)malloc((size_t)size);
    idx = (uint32_t*)malloc((DC_SCRAMBLE_MAX_CHUNK / DC_SCRAMBLE_SLICE) * sizeof(uint32_t));
    if (!src || !idx) {
        free(src);
        free(idx);
        return 0;
    }
    memcpy(src, dst, (size_t)size);

    seed = (uint32_t)size & 0xffffu;
    src_pos = 0;
    dst_pos = 0;
    rem = (size_t)size;
    chunk_sz = DC_SCRAMBLE_MAX_CHUNK;

    while (rem >= DC_SCRAMBLE_SLICE) {
        if (rem >= chunk_sz) {
            uint32_t n = (uint32_t)(chunk_sz / DC_SCRAMBLE_SLICE);
            uint32_t i;
            for (i = 0; i < n; i++) {
                idx[i] = i;
            }
            for (i = n; i-- > 0;) {
                uint32_t r;
                uint32_t j;
                uint32_t tmp;
                seed = (seed * 2109u + 9273u) & 0x7fffu;
                r = (seed + 0xc000u) & 0xffffu;
                j = (r * i) >> 16;
                tmp = idx[i];
                idx[i] = idx[j];
                idx[j] = tmp;
                memcpy(dst + dst_pos + (size_t)idx[i] * DC_SCRAMBLE_SLICE,
                       src + src_pos, DC_SCRAMBLE_SLICE);
                src_pos += DC_SCRAMBLE_SLICE;
            }
            dst_pos += chunk_sz;
            rem -= chunk_sz;
        } else {
            chunk_sz >>= 1;
        }
    }
    if (rem > 0) {
        memcpy(dst + dst_pos, src + src_pos, rem);
    }

    if (dc_looks_unscrambled(dst, size)) {
        host_log("descrambled 1ST_READ.BIN (%ld bytes)", size);
    } else {
        memcpy(dst, src, (size_t)size);
        host_log("warning: 1ST_READ.BIN lacks expected header (%ld bytes)", size);
    }

    free(src);
    free(idx);
    return 1;
}

int dc_init(const HOST_PROGRAM* program, const char* disc_path) {
    long size;
    if (!dc_gd_open(disc_path)) {
        return 0;
    }
    size = dc_gd_read_file("1ST_READ.BIN", (void*)(uintptr_t)DC_PROGRAM_ADDRESS, DC_PROGRAM_MAX);
    if (size <= 0) {
        host_log("no 1ST_READ.BIN in %s", disc_path);
        return 0;
    }
    if (!dc_descramble_boot((uint8_t*)(uintptr_t)DC_PROGRAM_ADDRESS, size)) {
        host_log("out of memory descrambling 1ST_READ.BIN");
        return 0;
    }
    board_at_stop(gdFsFinish);
    board_start(program);
    return 1;
}

/* ---- the NAOMI library's Dreamcast entries ---- */

void nlInitLibrary(void);
void nlInitInterrupt(void);

/* the library's start for a video mode (VGA, NTSC or PAL): the port's screen is VGA's */
void nlInitLibraryDC(int tvmode, int fbmode, int vwait) {
    (void)tvmode;
    (void)fbmode;
    (void)vwait;
    nlInitLibrary();
}

void nlInitInterruptDC(void) {
    nlInitInterrupt();
}

/* the interrupts' own stack: the host's interrupts run on the game's */
void nlSetIntrStack(void* stack) {
    (void)stack;
}

/* the way out of the game, back to the console's menu */
void nlExitSystem(void) {
}
