/*
 * nl_matha.c  libnaomi's nlAtan256 (nlmatha.src, assembler on the SH-4) in C: the angle of (x, y) in 256ths
 * of a turn, from the smaller over the larger looked up in a table, as the SH-4 code does it.
 */

#include "types.h"

Sint32 nlAtan256(Float y, Float x);

Uint8 nlAtan256tbl[129] = {
    0,  0,  1,  1,  1,  2,  2,  2,  3,  3,  3,  3,  4,  4,  4,  5,
    5,  5,  6,  6,  6,  7,  7,  7,  8,  8,  8,  8,  9,  9,  9,  10,
    10, 10, 11, 11, 11, 11, 12, 12, 12, 13, 13, 13, 13, 14, 14, 14,
    15, 15, 15, 15, 16, 16, 16, 17, 17, 17, 17, 18, 18, 18, 18, 19,
    19, 19, 19, 20, 20, 20, 20, 21, 21, 21, 21, 22, 22, 22, 22, 23,
    23, 23, 23, 23, 24, 24, 24, 24, 25, 25, 25, 25, 25, 26, 26, 26,
    26, 26, 27, 27, 27, 27, 27, 28, 28, 28, 28, 28, 29, 29, 29, 29,
    29, 29, 30, 30, 30, 30, 30, 31, 31, 31, 31, 31, 31, 32, 32, 32,
    32,
};

/* by octant (y < 0, |y| > |x|, x < 0): the base angle, and whether the table's angle is taken away */
static const struct {
    Sint16 base;
    Sint16 neg;
} octant[8] = {
    { 0x00, 0 }, { 0x80, 1 }, { 0x40, 1 }, { 0x40, 0 }, { 0x100, 1 }, { 0x80, 0 }, { 0xc0, 0 }, { 0xc0, 1 },
};

Sint32 nlAtan256(Float y, Float x) {
    int i = 0;
    int a;
    Float t;

    if (0.0f > y) {
        y = -y;
        i = 4;
    }
    if (0.0f > x) {
        x = -x;
        i |= 1;
    }
    if (y > x) {
        t = x;
        x = y;
        y = t;
        i |= 2;
    }
    /* ftrc, then the low byte: 0/0 gives 0 as on the SH-4 */
    a = nlAtan256tbl[(Uint8)(Sint32)(y / x * 128.0f + 0.5f)];
    return octant[i].base + (octant[i].neg ? -a : a);
}
