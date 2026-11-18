/*
 * host_check.c  MVC2_CHECK builds: gcc's -fsanitize checks on the game's C, where the host can differ from
 * the SH-4 (a float out of an integer's range, a shift of 32 or more, a table read past its end, a division
 * by zero). Each place is logged once to check.log (MVC2_CHECK_LOG), with what the SH-4 and the host make
 * of it; the counts and ranges are written at exit. The checks' data sits between the game's tables, so
 * where the game reads past one (pl_com.c's cpu_renda_tbl) a run drifts from a normal build's.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char* file;
    uint32_t line, col;
} LOC;

typedef struct {
    uint16_t kind, info;
    char name[1];
} TYPE;

typedef struct {
    const LOC* loc;
    const char* what;
    char type[48];
    unsigned count, differ, differ_arm;
    double lo, hi;
} SITE;

#define SITES 4096
static SITE sites[SITES];
static int nsites;
static FILE* out;

static void check_report(void) {
    int i;
    if (out == NULL) {
        return;
    }
    fprintf(out, "-- at exit: place, what, type, count, x86 differs, ARM differs, lowest, highest\n");
    for (i = 0; i < nsites; i++) {
        SITE* s = &sites[i];
        fprintf(out, "%s:%u:%u %s %s n=%u x86=%u arm=%u lo=%.9g hi=%.9g\n", s->loc->file, s->loc->line, s->loc->col,
                s->what, s->type, s->count, s->differ, s->differ_arm, s->lo, s->hi);
    }
    fclose(out);
    out = NULL;
}

static SITE* site(const LOC* loc, const char* what, const TYPE* t, double v, int differ, int differ_arm) {
    int i;
    SITE* s;
    for (i = 0; i < nsites; i++) {
        if (sites[i].loc == loc) {
            break;
        }
    }
    if (out == NULL) {
        const char* name = getenv("MVC2_CHECK_LOG");
        out = fopen(name ? name : "check.log", "w");
        atexit(check_report);
    }
    if (i == nsites) {
        if (nsites == SITES) {
            return NULL;
        }
        s = &sites[nsites++];
        s->loc = loc;
        s->what = what;
        snprintf(s->type, sizeof s->type, "%s", t ? t->name : "?");
        s->lo = s->hi = v;
        if (out) {
            fprintf(out, "%s:%u:%u %s %s value %.9g%s%s\n", loc->file, loc->line, loc->col, what, s->type, v,
                    differ ? " x86-differs" : "", differ_arm ? " arm-differs" : "");
            fflush(out);
        }
    } else {
        s = &sites[i];
    }
    s->count++;
    s->differ += differ != 0;
    s->differ_arm += differ_arm != 0;
    if (v < s->lo) {
        s->lo = v;
    }
    if (v > s->hi) {
        s->hi = v;
    }
    return s;
}

/* the SH-4's ftrc: NaN and below -2^31 give 0x80000000, 2^31 and above 0x7FFFFFFF */
static uint32_t sh_ftrc(float f) {
    if (f != f || f < -2147483648.0f) {
        return 0x80000000u;
    }
    if (f >= 2147483648.0f) {
        return 0x7FFFFFFFu;
    }
    return (uint32_t)(int32_t)f;
}

/* VFP's vcvt: saturating, NaN 0 */
static uint32_t arm_cvt(float f, int is_signed) {
    if (f != f) {
        return 0;
    }
    if (is_signed) {
        if (f < -2147483648.0f) {
            return 0x80000000u;
        }
        if (f >= 2147483648.0f) {
            return 0x7FFFFFFFu;
        }
        return (uint32_t)(int32_t)f;
    }
    if (f <= -1.0f) {
        return 0;
    }
    if (f >= 4294967296.0f) {
        return 0xFFFFFFFFu;
    }
    return f < 1.0f ? 0 : (uint32_t)f;
}

static volatile float cast_in;

void __ubsan_handle_float_cast_overflow(const void* data, uintptr_t from) {
    /* gcc's data: the place, then the two types */
    const LOC* loc = (const LOC*)data;
    const TYPE* ft = *(const TYPE* const*)((const char*)data + sizeof(LOC));
    const TYPE* tt = *(const TYPE* const*)((const char*)data + sizeof(LOC) + sizeof(void*));
    float f;
    int bits, is_signed;
    uint32_t sh, x86, arm, mask;
    if (ft->kind != 1 || ft->info != 32 || tt->kind != 0) {
        site(loc, "float-cast", tt, 0, 0, 0);
        return;
    }
    memcpy(&f, &from, 4);
    bits = 1 << (tt->info >> 1);
    is_signed = tt->info & 1;
    mask = bits >= 32 ? 0xFFFFFFFFu : (1u << bits) - 1;
    /* SHC's code for an unsigned int takes 2^32 off a value of 2^31 or more first; a narrower one is truncated */
    if (!is_signed && bits == 32) {
        float g = f;
        if (!(2147483648.0f > g)) {
            g -= 4294967296.0f;
        }
        sh = sh_ftrc(g);
    } else {
        sh = sh_ftrc(f);
    }
    sh &= mask;
    /* what this compiler's own conversion gives */
    cast_in = f;
    if (bits == 8) {
        x86 = is_signed ? (uint8_t)(int8_t)cast_in : (uint8_t)cast_in;
    } else if (bits == 16) {
        x86 = is_signed ? (uint16_t)(int16_t)cast_in : (uint16_t)cast_in;
    } else {
        x86 = is_signed ? (uint32_t)(int32_t)cast_in : (uint32_t)cast_in;
    }
    arm = arm_cvt(f, is_signed) & mask;
    site(loc, "float-cast", tt, f, sh != x86, sh != arm);
}

void __ubsan_handle_float_cast_overflow_abort(const void* data, uintptr_t from) {
    __ubsan_handle_float_cast_overflow(data, from);
}

typedef struct {
    LOC loc;
    const TYPE* lhs;
    const TYPE* rhs;
} SHIFT;

void __ubsan_handle_shift_out_of_bounds(const SHIFT* data, uintptr_t lhs, uintptr_t rhs) {
    (void)lhs;
    site(&data->loc, "shift", data->rhs, (double)(int32_t)rhs, 1, 1);
}

void __ubsan_handle_shift_out_of_bounds_abort(const SHIFT* data, uintptr_t lhs, uintptr_t rhs) {
    __ubsan_handle_shift_out_of_bounds(data, lhs, rhs);
}

typedef struct {
    LOC loc;
    const TYPE* array;
    const TYPE* index;
} BOUNDS;

void __ubsan_handle_out_of_bounds(const BOUNDS* data, uintptr_t index) {
    double v = (data->index->info & 1) ? (double)(int32_t)index : (double)(uint32_t)index;
    site(&data->loc, "bounds", data->array, v, 0, 0);
}

void __ubsan_handle_out_of_bounds_abort(const BOUNDS* data, uintptr_t index) {
    __ubsan_handle_out_of_bounds(data, index);
}

typedef struct {
    LOC loc;
    const TYPE* type;
} DIVREM;

void __ubsan_handle_divrem_overflow(const DIVREM* data, uintptr_t lhs, uintptr_t rhs) {
    /* a division by zero gives 0 on both (divzero_*.c); the most negative number over -1 traps on x86 */
    site(&data->loc, "divrem", data->type, (double)(int32_t)lhs, rhs != 0, 0);
}

void __ubsan_handle_divrem_overflow_abort(const DIVREM* data, uintptr_t lhs, uintptr_t rhs) {
    __ubsan_handle_divrem_overflow(data, lhs, rhs);
}
