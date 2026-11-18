/*
 * snd_lines_end.c  Linked just after snd_nl.c (snd_lines_begin.c): the sound's data ends on a cache line of its own
 */

#include "snd_lines.h"

char snd_lines_data_end[64] __attribute__((aligned(64))) = { 1 };
char snd_lines_bss_end[64] __attribute__((aligned(64)));
