/*
 * nl_sys.c  The NAOMI library's background and fade colour
 */

#include <stddef.h>
#include <string.h>
#include "nl_port.h"

float nl_fade_r = 1.0f, nl_fade_g = 1.0f, nl_fade_b = 1.0f;

/* three colours (top left, top right, bottom left) at 1/z, over the screen */
void nlSetBackColorZ(uint32_t col1, uint32_t col2, uint32_t col3, float z) {
    RD_VTX v[3] = { { 0 } };
    v[1].x = 640.0f;
    v[2].y = 480.0f;
    v[0].z = v[1].z = v[2].z = 1.0f / z;
    v[0].base = col1;
    v[1].base = col2;
    v[2].base = col3;
    rd_background_plane(v);
}

void nlSetBackColor(uint32_t col1, uint32_t col2, uint32_t col3) {
    nlSetBackColorZ(col1, col2, col3, 100000.0f);
}

void nlObjPutSetFadeColor(float r, float g, float b) {
    nl_fade_r = r;
    nl_fade_g = g;
    nl_fade_b = b;
}

void nlInvertScreen(void) {
    rd_invert = 1;
}

void nlSetWindowSize(float left, float top, float width, float height);

/* the window is the whole screen, for nlproj.c's screen matrix */
void nlInitLibrary(void) {
    nlSetWindowSize(0.0f, 0.0f, 640.0f, 480.0f);
}

/* the renderer draws the whole screen */
void kmSetPixelClipping(int32_t left, int32_t top, int32_t width, int32_t height) {
    (void)left;
    (void)top;
    (void)width;
    (void)height;
}

size_t nl_sys_state_size(void) {
    return 3 * sizeof(float);
}

void nl_sys_state_save(void* out) {
    float f[3] = { nl_fade_r, nl_fade_g, nl_fade_b };
    memcpy(out, f, sizeof f);
}

void nl_sys_state_load(const void* in) {
    float f[3];
    memcpy(f, in, sizeof f);
    nl_fade_r = f[0];
    nl_fade_g = f[1];
    nl_fade_b = f[2];
}
