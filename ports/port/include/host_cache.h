/*
 * host_cache.h  Data cache hints for the drawing's hot loops: the PSP's cache instruction, nothing elsewhere
 */
#ifndef HOST_CACHE_H
#define HOST_CACHE_H

#include <stdint.h>
#include <string.h>

/* on the PSP, the bytes written a frame by path, logged with MVC2_PROF_HW (psp_prof.c): what makes the writebacks */
enum {
    HOST_W_LZ,          /* nl_lz: kept cells copied out, and unpacked into its arena */
    HOST_W_KEEP,        /* nl_tex: textures' sources kept */
    HOST_W_TEX,         /* nl_tex: texels copied from its caches */
    HOST_W_TRIS,        /* rd: triangle slots */
    HOST_W_VCACHE,      /* nl_obj: its vertex cache */
    HOST_W_MODELS,      /* nl_obj: the records of the models drawn here, which the game writes */
    HOST_W_LIST,        /* psp_render: the GE's list and vertices */
    HOST_W_ATLAS,       /* psp_render: tiles and textures made */
    HOST_W_N
};
#ifdef __PSP__
extern uint32_t host_written[HOST_W_N];
#define HOST_WRITTEN(k, n) (host_written[k] += (uint32_t)(n))
#else
#define HOST_WRITTEN(k, n) ((void)0)
#endif

/* the 64-byte line holding p read into the cache ahead of its use */
static inline void host_prefetch(const void* p) {
#if defined(__PSP__) && !defined(HOST_NO_PREFETCH)
    __asm__ volatile("cache 30, 0(%0)" : : "r"(p));
#else
    (void)p;
#endif
}

/* the lines that start in [from, from + n) and end by limit taken into the cache without being read, for memory
   written in order from from on, whose bytes past from are all to be written before they are read */
static inline void host_lines_new(void* from, uint32_t n, const void* limit) {
#ifdef __PSP__
    uintptr_t a = ((uintptr_t)from + 63) & ~(uintptr_t)63, end = (uintptr_t)from + n;
    for (; a < end && a + 64 <= (uintptr_t)limit; a += 64) {
        __asm__ volatile("cache 24, 0(%0)" : : "r"(a) : "memory");
    }
#else
    (void)from;
    (void)n;
    (void)limit;
#endif
}

/* memcpy, the destination's whole lines taken a kilobyte at a time without being read first */
static inline void* host_copy(void* dst, const void* src, uint32_t n) {
#ifdef __PSP__
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    while (n > 0) {
        uint32_t k = n < 1024 ? n : 1024;
        host_lines_new(d, k, d + k);
        memcpy(d, s, k);
        d += k;
        s += k;
        n -= k;
    }
    return dst;
#else
    return memcpy(dst, src, n);
#endif
}

#endif
