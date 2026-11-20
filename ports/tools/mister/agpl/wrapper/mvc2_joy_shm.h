// Pads shared from MiSTer_MVC2 to the game. joy_mask bits are MvC2's buttons
// (the MVC2 core's J1 line; on Pinedra's core the wrapper maps its buttons to
// them): 0 right, 1 left, 2 down, 3 up, 4 LP, 5 HP, 6 A1, 7 LK, 8 HK, 9 A2,
// 10 Start, 11 Coin, 12 Test, 13 Service.
#ifndef MVC2_JOY_SHM_H
#define MVC2_JOY_SHM_H

#include <stdint.h>

#define MVC2_JOY_SHM_PATH    "/dev/shm/mvc2-joy"
#define MVC2_JOY_SHM_MAGIC   0x3243564Du   /* "MVC2" */
#define MVC2_JOY_SHM_VERSION 1
#define MVC2_JOY_PLAYERS     2

typedef struct {
	uint32_t magic;
	uint32_t version;
	uint32_t joy_mask[MVC2_JOY_PLAYERS];
	int8_t   left_stick_x[MVC2_JOY_PLAYERS];
	int8_t   left_stick_y[MVC2_JOY_PLAYERS];
	int8_t   right_stick_x[MVC2_JOY_PLAYERS];
	int8_t   right_stick_y[MVC2_JOY_PLAYERS];
} MisterJoyShm;

#endif
