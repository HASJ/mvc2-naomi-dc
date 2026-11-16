/*
 * test_mode.h  The arcade's test menu (test_mode.c), as a 40 by 30 grid of characters for a front end to draw
 */

#ifndef TEST_MODE_H
#define TEST_MODE_H

#include <stdint.h>

enum { TM_COLS = 40, TM_ROWS = 30 };
enum { TM_UP = 1, TM_DOWN = 2, TM_LEFT = 4, TM_RIGHT = 8, TM_OK = 16, TM_BACK = 32 };
enum { TM_OPEN, TM_CLOSED, TM_RESTART };    /* test_mode_keys: still open, closed, closed with new settings */

typedef struct {
    char ch[TM_ROWS][TM_COLS];
    uint32_t color[TM_ROWS][TM_COLS];   /* 0xRRGGBB */
} TEST_SCREEN;

void test_mode_open(const char* ok, const char* back);  /* the names of the front end's two keys */
int test_mode_keys(int keys);                           /* the TM_ keys pressed: TM_OPEN, TM_CLOSED or TM_RESTART */
const TEST_SCREEN* test_mode_screen(void);

#endif
