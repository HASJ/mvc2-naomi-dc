/*
 * snd_lines_begin.c  Linked just before arm7.c, aica.c, snd_drv.c and snd_nl.c, as snd_lines_end.c is just after:
 * their data, which the Media Engine writes when it makes the sound, starts on a cache line of its own
 */

#include "snd_lines.h"

char snd_lines_data_begin[64] __attribute__((aligned(64))) = { 1 };
char snd_lines_bss_begin[64] __attribute__((aligned(64)));
