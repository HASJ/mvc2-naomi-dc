/*
 * hunt.h  A scripted run for comparing moves with an emulator (hunt.c)
 */

#ifndef HUNT_H
#define HUNT_H

#include "naomi_input.h"

int hunt_init(void);
int hunt_frame(HOST_INPUT* in, unsigned frame);

#endif
