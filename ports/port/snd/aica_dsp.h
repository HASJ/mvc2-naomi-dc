/* aica_dsp.h  The AICA's DSP between aica.c and its translator to the host's ARM code (aica_dsp_jit.c) */

#ifndef AICA_DSP_H
#define AICA_DSP_H

#include <stdint.h>

#if defined(__arm__) && defined(__linux__) && defined(__ARM_ARCH) && __ARM_ARCH >= 7
#define AICA_DSP_JIT
#endif

/* the DSP's registers, and for the translated program its ring: where it starts, its words less one */
typedef struct {
    int32_t temp[128], mems[32], efreg[16];
    int32_t frc, y, adrs, memval;
    uint32_t mdec;
    uint8_t* ring;
    uint32_t rbl_mask;
    int32_t unpack[32];     /* the translator's: a word's sign and exponent, as its top bits and its shift */
} DSP_REGS;

/* a step of the program, decoded when the program changes */
typedef struct {
    uint8_t tra, twa, ira, iwa, ewa, masa, shift, ysel;
    uint16_t f;             /* the DF_ flags */
    int16_t coef;           /* its coefficient, 13 bits */
    uint16_t madrs;
} DSP_STEP;

enum {
    DF_TWT = 1, DF_XSEL = 2, DF_IWT = 4, DF_TABLE = 8, DF_MWT = 16, DF_MRD = 32, DF_EWT = 64, DF_ADRL = 128,
    DF_FRCL = 256, DF_YRL = 512, DF_NEGB = 1024, DF_ZERO = 2048, DF_BSEL = 4096, DF_NOFL = 8192, DF_ADREB = 16384,
    DF_NXADR = 32768
};

/* the program as ARM code, run a sample at a time with the same results as aica.c's dsp_sample where its ring
   doesn't wrap at the RAM's end and is past the sound CPU's code; its words, or 0 */
int aica_dsp_jit_make(const DSP_STEP* prog, int n, DSP_REGS* d);
void aica_dsp_jit_run(DSP_REGS* d, const int32_t* mix);

#endif
