/*
 * nl_work.c  nlsystem's work that nlproj.c reads, the host's since both programs share the library
 */

#include <stddef.h>
#include <string.h>
#include "nlsystem.h"

NLSYSWORK NLSystemWork_ = { .scale_factor = 1.0f };
NLSCREEN nl_scr;
Sint32 nl_VGA_flag, nl_PAL_EXT_flag, nl_ext_pal_height;
Uint8 nl_objput_param[0x2c];        /* NLOBJPUTPARAM */

/* nlSetScaleFactor's scale, which the next model drawn takes */
__attribute__((section(".text.nlhot"))) float nl_take_scale_factor(void) {
    float s = NLSystemWork_.scale_factor;
    NLSystemWork_.scale_factor = 1.0f;
    return s;
}

typedef struct {
    NLSYSWORK work;
    NLSCREEN scr;
    Sint32 vga, pal_ext, ext_h;
    Uint8 objput_param[0x2c];
} NL_WORK_STATE;

size_t nl_work_state_size(void) {
    return sizeof(NL_WORK_STATE);
}

void nl_work_state_save(void* out) {
    NL_WORK_STATE s;
    s.work = NLSystemWork_;
    s.scr = nl_scr;
    s.vga = nl_VGA_flag;
    s.pal_ext = nl_PAL_EXT_flag;
    s.ext_h = nl_ext_pal_height;
    memcpy(s.objput_param, nl_objput_param, sizeof nl_objput_param);
    memcpy(out, &s, sizeof s);
}

void nl_work_state_load(const void* in) {
    NL_WORK_STATE s;
    memcpy(&s, in, sizeof s);
    NLSystemWork_ = s.work;
    nl_scr = s.scr;
    nl_VGA_flag = s.vga;
    nl_PAL_EXT_flag = s.pal_ext;
    nl_ext_pal_height = s.ext_h;
    memcpy(nl_objput_param, s.objput_param, sizeof nl_objput_param);
}
