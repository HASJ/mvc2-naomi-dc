/* frame_log.h  MVC2_FRAMELOG: per-frame counters for the load investigation (not for commit) */
#ifndef FRAME_LOG_H
#define FRAME_LOG_H
#include <stdint.h>
#include <time.h>
typedef struct {
    uint64_t gd_ns, tex_ns, conv_ns;
    uint64_t sync_ns, lz_ns, open_ns, end_ns, adx_ns, fopen_ns, seek_ns, fclose_ns;
    uint32_t gd_bytes, gd_sync, gd_req, gd_open, tex_calls, tex_conv, gd_wait;
} FRAME_LOG;
extern FRAME_LOG fl;
#ifdef __3DS__
#include <3ds/types.h>
#include <3ds/svc.h>
static inline uint64_t fl_now(void) {
    return svcGetSystemTick() * 1000ull / 268ull;
}
#elif defined(__PSP__)
#include <pspkernel.h>
static inline uint64_t fl_now(void) {
    return (uint64_t)sceKernelGetSystemTimeWide() * 1000ull;
}
#elif defined(MVC2_MISTER)
/* MVC2_LOAD_TIMES=1: the times kept (main_mister.c); else none, a clock read being a system call on the MiSTer's A9 */
extern int fl_timing;
static inline uint64_t fl_now(void) {
    struct timespec ts;
    if (!fl_timing) {
        return 0;
    }
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#else
static inline uint64_t fl_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#endif
#endif
