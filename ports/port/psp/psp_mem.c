/*
 * psp_mem.c  The PSP's memory: the game's main RAM taken at NAOMI_RAM_BASE before newlib makes its heap, which
 * then has the free blocks either side of it (newlib's sbrk, replaced)
 */

#include <errno.h>
#include <malloc.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <pspdmac.h>
#include <pspkernel.h>

#include "naomi_host.h"
#include "psp_front.h"

/* left free for the kernel's own needs and the threads' stacks */
#define KEEP_FREE (1u << 20)

PSP_MEM psp_mem;
int psp_ram_mapped;                 /* for naomi_map_ram */

static int part;                /* the block sbrk is in */
static char* brk_at;
static uint32_t claimed;        /* what sbrk has given */

/* the first sbrk, from newlib's start-up before main: RAM, then the two largest free blocks in address order */
static void mem_init(void) {
    SceUID id;
    int i;
    psp_mem.free_at_start = sceKernelTotalFreeMemSize();
    psp_mem.max_at_start = sceKernelMaxFreeMemSize();
    id = sceKernelAllocPartitionMemory(PSP_MEMORY_PARTITION_USER, "mvc2 main RAM", PSP_SMEM_Addr, NAOMI_RAM_SIZE,
                                       (void*)(uintptr_t)NAOMI_RAM_BASE);
    if (id >= 0 && sceKernelGetBlockHeadAddr(id) == (void*)(uintptr_t)NAOMI_RAM_BASE) {
        psp_mem.ram_mapped = psp_ram_mapped = 1;
    } else if (id >= 0) {
        sceKernelFreePartitionMemory(id);
    }
    psp_mem.ram_error = id < 0 ? id : 0;
    for (i = 0; i < 2; i++) {
        SceSize n = sceKernelMaxFreeMemSize() & ~(SceSize)0xFFFF;
        SceSize keep = KEEP_FREE > sceKernelTotalFreeMemSize() - n ? KEEP_FREE - (sceKernelTotalFreeMemSize() - n) : 0;
        if (n <= keep + (1u << 20)) {
            break;
        }
        id = sceKernelAllocPartitionMemory(PSP_MEMORY_PARTITION_USER, i == 0 ? "mvc2 heap" : "mvc2 heap 2", PSP_SMEM_Low,
                                           n - keep, NULL);
        if (id < 0) {
            break;
        }
        psp_mem.heap[i].base = sceKernelGetBlockHeadAddr(id);
        psp_mem.heap[i].end = psp_mem.heap[i].base + (n - keep);
        psp_mem.nheaps++;
    }
    if (psp_mem.nheaps == 2 && psp_mem.heap[1].base < psp_mem.heap[0].base) {
        PSP_BLOCK t = psp_mem.heap[0];
        psp_mem.heap[0] = psp_mem.heap[1];
        psp_mem.heap[1] = t;
    }
    brk_at = psp_mem.heap[0].base;
}

/* malloc takes a jump to the higher block as foreign space, which it can use */
void* _sbrk(ptrdiff_t incr) {
    static int inited;
    char* old;
    if (!inited) {
        inited = 1;
        mem_init();
    }
    if (part >= psp_mem.nheaps) {
        errno = ENOMEM;
        return (void*)-1;
    }
    if (incr > psp_mem.heap[part].end - brk_at) {
        if (incr < 0 || part + 1 >= psp_mem.nheaps || incr > psp_mem.heap[part + 1].end - psp_mem.heap[part + 1].base) {
            errno = ENOMEM;
            return (void*)-1;
        }
        part++;
        brk_at = psp_mem.heap[part].base;
    }
    if (incr < 0 && brk_at + incr < psp_mem.heap[part].base) {
        errno = ENOMEM;
        return (void*)-1;
    }
    old = brk_at;
    brk_at += incr;
    claimed += (uint32_t)incr;
    return old;
}

/* what sbrk can still give: the rest of its block and the blocks above */
uint32_t psp_mem_unclaimed(void) {
    uint32_t n = 0;
    int i;
    for (i = part; i < psp_mem.nheaps; i++) {
        n += (uint32_t)(psp_mem.heap[i].end - (i == part ? brk_at : psp_mem.heap[i].base));
    }
    return n;
}

/* the heap's bytes in use: mallinfo's count has the gap between the blocks in it */
uint32_t psp_mem_in_use(void) {
    return claimed - (uint32_t)mallinfo().fordblks;
}

/* what can still be had: free in the heap and unclaimed */
uint32_t psp_mem_free(void) {
    return (uint32_t)mallinfo().fordblks + psp_mem_unclaimed();
}

/* every allocation that fails, logged (the first 20): malloc, calloc and realloc wrapped by the linker */
void* __real_malloc(size_t n);
void* __real_calloc(size_t n, size_t size);
void* __real_realloc(void* p, size_t n);

static void* tried(void* p, size_t bytes) {
    static int failed;
    if (p == NULL && bytes != 0 && failed++ < 20) {
        host_log("out of heap for %lu bytes at frame %u: %lu KB in use, %lu KB free", (unsigned long)bytes, host_frame(),
                 (unsigned long)psp_mem_in_use() >> 10, (unsigned long)psp_mem_free() >> 10);
    }
    return p;
}

void* __wrap_malloc(size_t n) {
    return tried(__real_malloc(n), n);
}

void* __wrap_calloc(size_t n, size_t size) {
    return tried(__real_calloc(n, size), n * size);
}

void* __wrap_realloc(void* p, size_t n) {
    return tried(__real_realloc(p, n), n);
}
/* n bytes copied by the DMA controller (2.5 times memcpy's speed), the cache kept right; the destination's lines it
   covers only in part, and a copy too small or not word for word, copied by memcpy. MVC2_DMA_COPY=0: memcpy only */
void* psp_copy(void* to, const void* from, size_t n) {
    static int on = -1;
    uint8_t* d = to;
    const uint8_t* s = from;
    size_t head = (64 - ((uintptr_t)d & 63)) & 63, body;
    if (on < 0) {
        on = getenv("MVC2_DMA_COPY") == NULL || atoi(getenv("MVC2_DMA_COPY")) != 0;
    }
    if (!on || n < (32u << 10) || ((uintptr_t)d - (uintptr_t)s) & 3) {
        return memcpy(to, from, n);
    }
    memcpy(d, s, head);
    d += head;
    s += head;
    n -= head;
    body = n & ~(size_t)63;
    sceKernelDcacheWritebackRange(s, body);
    sceKernelDcacheInvalidateRange(d, body);
    if (sceDmacMemcpy(d, s, body) < 0) {
        memcpy(d, s, body);
    }
    memcpy(d + body, s + body, n - body);
    return to;
}
