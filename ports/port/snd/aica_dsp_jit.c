/*
 * aica_dsp_jit.c  The AICA DSP's program translated to the host's ARM code (ARMv7 Linux): each step as aica.c's
 * dsp_sample runs it, with its flags, selects and addresses fixed, so the same results without its branches.
 */

#include "aica_dsp.h"

#ifdef AICA_DSP_JIT

#include <stddef.h>
#include <sys/mman.h>

/* in the code: r4 the accumulator, r5 the registers, r6 the inputs, r7 the ring, r8 mdec, r9 the ring's mask, r10
   memval, r11 adrs; r1 the step's input, r2 its temp, r3 the shifted accumulator; r0, r12, lr scratch */
enum { R_ACC = 4, R_D = 5, R_MIX = 6, R_RING = 7, R_MDEC = 8, R_RBL = 9, R_MEMVAL = 10, R_ADRS = 11, IP = 12, LR = 14 };
enum { AL = 0xE, NE = 0x1, LS = 0x9, HI = 0x8 };
enum { AND = 0, EOR = 1, SUB = 2, RSB = 3, ADD = 4, TST = 8, CMP = 10, ORR = 12, MOV = 13, MVN = 15 };
enum { LSL = 0, LSR = 1, ASR = 2 };

#define O_MEMS offsetof(DSP_REGS, mems)
#define O_EF offsetof(DSP_REGS, efreg)
#define O(f) ((uint32_t)offsetof(DSP_REGS, f))
_Static_assert(offsetof(DSP_REGS, temp) == 0, "temp is read at the registers' address");

#define CODE_WORDS 16384
static uint32_t* code;
static uint32_t *at, *end;
static void (*run)(DSP_REGS*, const int32_t*);

static void put(uint32_t w) {
    if (at < end) {
        *at++ = w;
    } else {
        end = NULL;     /* out of room: the program is not used */
    }
}

/* an ARM immediate for v, or -1 */
static int32_t imm(uint32_t v) {
    int r;
    for (r = 0; r < 16; r++) {
        uint32_t x = r ? v << (2 * r) | v >> (32 - 2 * r) : v;
        if (x < 256) {
            return (int32_t)(r << 8 | x);
        }
    }
    return -1;
}

static uint32_t shifted(int rm, int type, int n) {     /* an operand: rm shifted by n */
    return (uint32_t)(rm | type << 5 | n << 7);
}

static void dp_c(int cond, int op, int rd, int rn, uint32_t op2) {
    put((uint32_t)cond << 28 | (uint32_t)op << 21 | (op == TST || op == CMP) << 20 | (uint32_t)rn << 16 |
        (uint32_t)rd << 12 | op2);
}

static void dp(int op, int rd, int rn, uint32_t op2) {
    dp_c(AL, op, rd, rn, op2);
}

static void dpi_c(int cond, int op, int rd, int rn, uint32_t v) {
    int32_t e = imm(v);
    put((uint32_t)cond << 28 | 1u << 25 | (uint32_t)op << 21 | (op == TST || op == CMP) << 20 | (uint32_t)rn << 16 |
        (uint32_t)rd << 12 | (uint32_t)e);
    if (e < 0) {
        end = NULL;
    }
}

static void dpi(int op, int rd, int rn, uint32_t v) {
    dpi_c(AL, op, rd, rn, v);
}

static void mov_imm(int rd, uint32_t v) {
    if (imm(v) >= 0) {
        dpi(MOV, rd, 0, v);
    } else if (imm(~v) >= 0) {
        dpi(MVN, rd, 0, ~v);
    } else {
        put(0xE3000000u | (v & 0xF000) << 4 | (uint32_t)rd << 12 | (v & 0xFFF));     /* movw */
        if (v >> 16) {
            put(0xE3400000u | (v >> 16 & 0xF000) << 4 | (uint32_t)rd << 12 | (v >> 16 & 0xFFF));   /* movt */
        }
    }
}

static void ldr(int rd, int rn, uint32_t off) {
    put(0xE5900000u | (uint32_t)rn << 16 | (uint32_t)rd << 12 | off);
}

static void str(int rd, int rn, uint32_t off) {
    put(0xE5800000u | (uint32_t)rn << 16 | (uint32_t)rd << 12 | off);
}

static void ldr_x4(int rd, int rn, int rm) {    /* rd = [rn + rm * 4] */
    put(0xE7900100u | (uint32_t)rn << 16 | (uint32_t)rd << 12 | (uint32_t)rm);
}

static void str_x4(int rd, int rn, int rm) {
    put(0xE7800100u | (uint32_t)rn << 16 | (uint32_t)rd << 12 | (uint32_t)rm);
}

static void sbfx(int rd, int rn, int lsb, int width) {
    put(0xE7A00050u | (uint32_t)(width - 1) << 16 | (uint32_t)rd << 12 | (uint32_t)lsb << 7 | (uint32_t)rn);
}

static void ubfx(int rd, int rn, int lsb, int width) {
    put(0xE7E00050u | (uint32_t)(width - 1) << 16 | (uint32_t)rd << 12 | (uint32_t)lsb << 7 | (uint32_t)rn);
}

static void ssat24(int rd, int rn, int lsl) {   /* rd = rn << lsl, saturated to 24 bits */
    put(0xE6A00010u | 23u << 16 | (uint32_t)rd << 12 | (uint32_t)lsl << 7 | (uint32_t)rn);
}

/* the temp register at (n + mdec) & 127: its index in r0 */
static void temp_index(int n) {
    if (n) {
        dpi(ADD, 0, R_MDEC, (uint32_t)n);
        dpi(AND, 0, 0, 0x7F);
    } else {
        dpi(AND, 0, R_MDEC, 0x7F);
    }
}

/* r12 = dsp_pack(r3), with lr and r2 */
static void pack(void) {
    dp(EOR, LR, 3, shifted(3, LSL, 1));
    dp(MOV, LR, 0, shifted(LR, LSL, 8));
    put(0xE16F0F10u | (uint32_t)LR << 12 | (uint32_t)LR);      /* clz */
    dpi(CMP, 0, LR, 12);
    dpi_c(HI, MOV, LR, 0, 12);
    dpi(CMP, 0, LR, 11);
    dp_c(LS, MOV, 2, 0, (uint32_t)LR);
    dpi_c(HI, MOV, 2, 0, 11);
    dp(MOV, IP, 0, (uint32_t)(3 | 2 << 8 | LSL << 5 | 1 << 4));  /* r3 << r2 */
    ubfx(IP, IP, 11, 11);
    dp(ORR, IP, IP, shifted(LR, LSL, 11));
    dpi(TST, 0, 3, 0x800000);
    dpi_c(NE, ORR, IP, IP, 0x8000);
}

/* memval = dsp_unpack(r12), with lr and r2: the mantissa under the table's top bits, shifted by its low byte */
static void unpack(void) {
    dp(MOV, LR, 0, shifted(IP, LSR, 11));
    dp(ADD, LR, R_D, shifted(LR, LSL, 2));
    ldr(LR, LR, O(unpack));
    ubfx(2, IP, 0, 11);
    dp(ORR, 2, LR, shifted(2, LSL, 19));
    dp(MOV, R_MEMVAL, 0, (uint32_t)(2 | LR << 8 | ASR << 5 | 1 << 4));  /* r2 >> lr */
}

static void step(const DSP_STEP* p, uint16_t* ef_seen) {
    uint32_t f = p->f;
    int no_product = p->ysel == 1 && p->coef == 0, x = f & DF_XSEL ? 1 : 2;
    int use_in = ((f & DF_XSEL) && !no_product) || (f & DF_YRL) || ((f & DF_ADRL) && p->shift != 3);
    int use_t = (!(f & DF_XSEL) && !no_product) || !(f & (DF_ZERO | DF_BSEL));
    int use_sh = (f & (DF_TWT | DF_FRCL | DF_MWT | DF_EWT)) || ((f & DF_ADRL) && p->shift == 3);

    /* the input, r1 */
    if (use_in) {
        if ((f & DF_IWT) && p->ira == p->iwa) {
            dp(MOV, 1, 0, R_MEMVAL);
        } else if (p->ira <= 0x1F) {
            ldr(1, R_D, (uint32_t)(O_MEMS + p->ira * 4));
            sbfx(1, 1, 0, 24);
        } else if (p->ira <= 0x2F) {
            ldr(1, R_MIX, (uint32_t)(p->ira - 0x20) * 4);
            dp(MOV, 1, 0, shifted(1, LSL, 12));
            dp(MOV, 1, 0, shifted(1, ASR, 8));
        } else {
            dpi(MOV, 1, 0, 0);
        }
    }
    if (f & DF_IWT) {
        str(R_MEMVAL, R_D, (uint32_t)(O_MEMS + p->iwa * 4));
    }
    /* the temp, r2 */
    if (use_t) {
        temp_index(p->tra);
        ldr_x4(2, R_D, 0);
        sbfx(2, 2, 0, 24);
    }
    /* the accumulator shifted, r3, from the last step's */
    if (use_sh) {
        switch (p->shift) {
        case 0: ssat24(3, R_ACC, 0); break;
        case 1: ssat24(3, R_ACC, 1); break;
        case 2: dp(MOV, 3, 0, shifted(R_ACC, LSL, 1)); sbfx(3, 3, 0, 24); break;
        default: sbfx(3, R_ACC, 0, 24); break;
        }
    }
    /* y, r0, then the input to y's register */
    if (!no_product) {
        switch (p->ysel) {
        case 0: ldr(0, R_D, O(frc)); sbfx(0, 0, 0, 13); break;
        case 1: mov_imm(0, (uint32_t)(int32_t)p->coef); break;
        case 2: ldr(0, R_D, O(y)); sbfx(0, 0, 11, 13); break;
        default: ldr(0, R_D, O(y)); ubfx(0, 0, 4, 12); break;
        }
    }
    if (f & DF_YRL) {
        str(1, R_D, O(y));
    }
    /* acc = x * y >> 12 + b */
    if (no_product) {
        if (f & DF_ZERO) {
            dpi(MOV, R_ACC, 0, 0);
        } else if (f & DF_NEGB) {
            dpi(RSB, R_ACC, f & DF_BSEL ? R_ACC : 2, 0);
        } else if (!(f & DF_BSEL)) {
            dp(MOV, R_ACC, 0, 2);
        }
    } else {
        /* x has 24 bits and y 13: x * y >> 12 is (x << 4) * y >> 16, a 32x16 multiply */
        int b = f & DF_BSEL ? R_ACC : 2;
        dp(MOV, IP, 0, shifted(x, LSL, 4));
        if (f & DF_ZERO) {
            put(0xE12000A0u | (uint32_t)R_ACC << 16 | 0u << 8 | (uint32_t)IP);     /* smulwb r4, ip, r0 */
        } else if (f & DF_NEGB) {
            put(0xE12000A0u | (uint32_t)IP << 16 | 0u << 8 | (uint32_t)IP);        /* smulwb ip, ip, r0 */
            dp(SUB, R_ACC, IP, (uint32_t)b);
        } else {
            put(0xE1200080u | (uint32_t)R_ACC << 16 | (uint32_t)b << 12 | 0u << 8 | (uint32_t)IP);  /* smlawb */
        }
    }
    if (f & DF_TWT) {
        temp_index(p->twa);
        str_x4(3, R_D, 0);
    }
    if (f & DF_FRCL) {
        if (p->shift == 3) {
            ubfx(0, 3, 0, 12);
        } else {
            ubfx(0, 3, 11, 13);
        }
        str(0, R_D, O(frc));
    }
    if (f & (DF_MRD | DF_MWT)) {
        uint32_t a = (uint32_t)p->madrs + (f & DF_NXADR ? 1 : 0);
        if (f & DF_TABLE) {
            mov_imm(0, a);
        } else if (imm(a) >= 0) {
            dpi(ADD, 0, R_MDEC, a);
        } else {
            mov_imm(0, a);
            dp(ADD, 0, 0, R_MDEC);
        }
        if (f & DF_ADREB) {
            ubfx(IP, R_ADRS, 0, 12);
            dp(ADD, 0, 0, IP);
        }
        if (f & DF_TABLE) {
            ubfx(0, 0, 0, 16);
        } else {
            dp(AND, 0, 0, R_RBL);
        }
        dp(ADD, 0, R_RING, shifted(0, LSL, 1));
        if (f & DF_MRD) {
            if (f & DF_NOFL) {
                put(0xE1D000F0u | (uint32_t)IP << 12);      /* ldrsh ip, [r0] */
                dp(MOV, R_MEMVAL, 0, shifted(IP, LSL, 8));
            } else {
                put(0xE1D000B0u | (uint32_t)IP << 12);      /* ldrh ip, [r0] */
                unpack();
            }
        }
        if (f & DF_MWT) {
            if (f & DF_NOFL) {
                dp(MOV, IP, 0, shifted(3, ASR, 8));
            } else {
                pack();
            }
            put(0xE1C000B0u | (uint32_t)IP << 12);          /* strh ip, [r0] */
        }
    }
    if (f & DF_ADRL) {
        if (p->shift == 3) {
            ubfx(R_ADRS, 3, 12, 12);
        } else {
            dp(MOV, R_ADRS, 0, shifted(1, ASR, 16));
        }
    }
    if (f & DF_EWT) {
        uint32_t o = (uint32_t)(O_EF + p->ewa * 4);
        if (*ef_seen >> p->ewa & 1) {
            ldr(0, R_D, o);
            dp(ADD, 0, 0, shifted(3, ASR, 8));
        } else {
            dp(MOV, 0, 0, shifted(3, ASR, 8));      /* the first this sample: the register is 0 */
            *ef_seen |= (uint16_t)(1u << p->ewa);
        }
        str(0, R_D, o);
    }
}

int aica_dsp_jit_make(const DSP_STEP* prog, int n, DSP_REGS* d) {
    uint16_t ef_seen = 0;
    int s;
    run = NULL;
    /* dsp_unpack: sx24(m) >> e, m the mantissa at bit 11 under bits 22 and 23 from the sign and e; here those bits
       at 30 and 31 and e + 8 in the low byte, the mantissa put at 19 */
    for (s = 0; s < 32; s++) {
        int sign = s >> 4, e = s & 15;
        uint32_t top = e > 11 ? (uint32_t)sign * 3 : (uint32_t)sign * 2 + (uint32_t)(sign ^ 1);
        d->unpack[s] = (int32_t)(top << 30 | (uint32_t)((e > 11 ? 11 : e) + 8));
    }
    if (code == NULL) {
        void* m = mmap(NULL, CODE_WORDS * 4, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED) {
            return 0;
        }
        code = m;
    }
    at = code;
    end = code + CODE_WORDS;
    put(0xE92D4FF8u);                   /* push {r3-r11, lr} */
    dp(MOV, R_D, 0, 0);
    dp(MOV, R_MIX, 0, 1);
    ldr(R_RING, R_D, O(ring));
    ldr(R_MDEC, R_D, O(mdec));
    ldr(R_RBL, R_D, O(rbl_mask));
    ldr(R_MEMVAL, R_D, O(memval));
    ldr(R_ADRS, R_D, O(adrs));
    dpi(MOV, R_ACC, 0, 0);
    for (s = 0; s < n; s++) {
        step(&prog[s], &ef_seen);
    }
    dpi(SUB, R_MDEC, R_MDEC, 1);
    str(R_MDEC, R_D, O(mdec));
    str(R_MEMVAL, R_D, O(memval));
    str(R_ADRS, R_D, O(adrs));
    put(0xE8BD8FF8u);                   /* pop {r3-r11, pc} */
    if (end == NULL) {
        return 0;
    }
    __builtin___clear_cache((char*)code, (char*)at);
    run = (void (*)(DSP_REGS*, const int32_t*))(uintptr_t)code;
    return (int)(at - code);
}

void aica_dsp_jit_run(DSP_REGS* d, const int32_t* mix) {
    run(d, mix);
}

#endif
