/*
 * coro_x86.c  Serializable stackful coroutines for 32-bit x86 Windows
 *
 * Uses VirtualAlloc stacks (fixed virtual address for the lifetime of the coroutine) and swaps
 * callee-saved registers, x87/SSE control words, and Win32 TIB stack bounds (fs:[0x00/0x04/0x08/0xE0C])
 * so WOW64/VEH exception handling and stack unwinding stay valid while a coroutine runs.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "coro.h"

#define STACK_FILL 0xA5A5A5A5u

struct coro {
    void* sp;
    void* stack;
    unsigned stack_size;
    void (*fn)(void*);
    void* arg;
};

typedef struct {
    uint32_t magic;
    uint32_t stack_size;
    uint32_t sp_offset;
    uint32_t used_bytes;
} CORO_STATE_HDR;

#define CORO_STATE_MAGIC 0x4F524F43u /* 'CORO' */

static coro main_coro;
static coro* current;

/* Primary 1 MB coroutine stack kept in fixed-base .bss so VirtualAlloc collisions never alter VAs. */
static __attribute__((aligned(4096))) uint8_t s_primary_stack[1u << 20];
static int s_primary_in_use;

extern void __attribute__((regparm(2))) coro_swap(void** save_sp, void* load_sp) __asm__("coro_swap");

__asm__(
    ".text\n"
    ".globl coro_swap\n"
    "coro_swap:\n"
    "    pushl %ebp\n"
    "    pushl %ebx\n"
    "    pushl %esi\n"
    "    pushl %edi\n"
    "    subl $4, %esp\n"
    "    movl $0, (%esp)\n"
    "    fnstcw (%esp)\n"
    "    subl $4, %esp\n"
    "    stmxcsr (%esp)\n"
    "    andl $0xFFC0, (%esp)\n"
    "    movl %fs:0x00, %ecx\n"
    "    pushl %ecx\n"
    "    movl %fs:0x04, %ecx\n"
    "    pushl %ecx\n"
    "    movl %fs:0x08, %ecx\n"
    "    pushl %ecx\n"
    "    movl %fs:0xE0C, %ecx\n"
    "    pushl %ecx\n"
    "    movl %esp, (%eax)\n"
    "    movl %edx, %esp\n"
    "    popl %ecx\n"
    "    movl %ecx, %fs:0xE0C\n"
    "    popl %ecx\n"
    "    movl %ecx, %fs:0x08\n"
    "    popl %ecx\n"
    "    movl %ecx, %fs:0x04\n"
    "    popl %ecx\n"
    "    movl %ecx, %fs:0x00\n"
    "    ldmxcsr (%esp)\n"
    "    addl $4, %esp\n"
    "    fldcw (%esp)\n"
    "    addl $4, %esp\n"
    "    popl %edi\n"
    "    popl %esi\n"
    "    popl %ebx\n"
    "    popl %ebp\n"
    "    ret\n"
);

static void coro_start(void) {
    current->fn(current->arg);
    abort();
}

coro* coro_self(void) {
    if (current == NULL) {
        current = &main_coro;
    }
    return current;
}

static void start_on_stack(coro* c, void (*fn)(void*), void* arg) {
    uint32_t* w = (uint32_t*)c->stack;
    uint32_t* top;
    uint16_t fcw;
    uint32_t mxcsr;
    unsigned i;

    c->fn = fn;
    c->arg = arg;
    for (i = 0; i < c->stack_size / 4; i++) {
        w[i] = STACK_FILL;
    }
    __asm__ volatile("fnstcw %0" : "=m"(fcw));
    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
    mxcsr &= 0xFFC0u;

    top = (uint32_t*)(((uintptr_t)c->stack + c->stack_size) & ~(uintptr_t)15);
    /* dummy return address above coro_start so coro_start enters with 16-byte aligned (esp + 4) */
    *--top = 0;
    *--top = (uint32_t)(uintptr_t)coro_start;
    /* callee-saved ebp, ebx, esi, edi */
    *--top = 0;
    *--top = 0;
    *--top = 0;
    *--top = 0;
    /* fnstcw slot, stmxcsr slot */
    *--top = (uint32_t)fcw;
    *--top = mxcsr;
    /* TIB: ExceptionList (-1), StackBase, StackLimit, DeallocationStack */
    *--top = 0xFFFFFFFFu;
    *--top = (uint32_t)((uintptr_t)c->stack + c->stack_size);
    *--top = (uint32_t)(uintptr_t)c->stack;
    *--top = (uint32_t)(uintptr_t)c->stack;
    c->sp = top;
}

coro* coro_create(void (*fn)(void*), void* arg, unsigned stack_size) {
    coro* c = (coro*)calloc(1, sizeof(coro));
    coro_self();
    stack_size = (stack_size + 4095u) & ~4095u;
    if (c != NULL) {
        if (!s_primary_in_use && stack_size <= sizeof(s_primary_stack)) {
            c->stack = s_primary_stack;
            s_primary_in_use = 1;
        } else {
            /* Try fixed virtual address first so secondary coroutine stacks stay uniform across peers. */
            c->stack = VirtualAlloc((void*)(uintptr_t)0x1C000000u, stack_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (c->stack == NULL) {
                c->stack = VirtualAlloc(NULL, stack_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            }
        }
    }
    if (c == NULL || c->stack == NULL) {
        fprintf(stderr, "coro_create: VirtualAlloc failed for %u byte stack\n", stack_size);
        abort();
    }
    c->stack_size = stack_size;
    start_on_stack(c, fn, arg);
    return c;
}

void coro_restart(coro* c, void (*fn)(void*), void* arg) {
    if (c != NULL && c != current && c != &main_coro) {
        start_on_stack(c, fn, arg);
    }
}

void coro_delete(coro* c) {
    if (c != NULL && c != current && c != &main_coro) {
        if (c->stack == s_primary_stack) {
            s_primary_in_use = 0;
        } else if (c->stack != NULL) {
            VirtualFree(c->stack, 0, MEM_RELEASE);
        }
        free(c);
    }
}

void coro_switch(coro* to) {
    coro* from = coro_self();
    if (to != from) {
        current = to;
        coro_swap(&from->sp, to->sp);
    }
}

size_t coro_state_size(const coro* c) {
    uintptr_t base, sp, end;
    if (c == NULL || c->stack == NULL || c == &main_coro) {
        return 0;
    }
    base = (uintptr_t)c->stack;
    end = base + c->stack_size;
    sp = (uintptr_t)c->sp;
    if (sp < base || sp > end) {
        return 0;
    }
    return sizeof(CORO_STATE_HDR) + (size_t)(end - sp);
}

size_t coro_state_save(const coro* c, void* buf, size_t max_bytes) {
    CORO_STATE_HDR hdr;
    size_t need = coro_state_size(c);
    uintptr_t sp_off, used;
    if (need == 0 || buf == NULL || max_bytes < need) {
        return 0;
    }
    sp_off = (uintptr_t)c->sp - (uintptr_t)c->stack;
    used = (uintptr_t)c->stack_size - sp_off;
    hdr.magic = CORO_STATE_MAGIC;
    hdr.stack_size = c->stack_size;
    hdr.sp_offset = (uint32_t)sp_off;
    hdr.used_bytes = (uint32_t)used;
    memcpy(buf, &hdr, sizeof hdr);
    if (used > 0) {
        memcpy((uint8_t*)buf + sizeof hdr, (const uint8_t*)c->stack + sp_off, used);
    }
    return need;
}

int coro_state_load(coro* c, const void* buf, size_t bytes) {
    CORO_STATE_HDR hdr;
    if (c == NULL || c->stack == NULL || c == &main_coro || c == current || buf == NULL ||
        bytes < sizeof hdr) {
        return 0;
    }
    memcpy(&hdr, buf, sizeof hdr);
    if (hdr.magic != CORO_STATE_MAGIC || hdr.stack_size != c->stack_size || hdr.sp_offset > c->stack_size ||
        hdr.used_bytes != c->stack_size - hdr.sp_offset || bytes < sizeof hdr + hdr.used_bytes) {
        return 0;
    }
    if (hdr.used_bytes > 0) {
        memcpy((uint8_t*)c->stack + hdr.sp_offset, (const uint8_t*)buf + sizeof hdr, hdr.used_bytes);
    }
    c->sp = (void*)((uintptr_t)c->stack + hdr.sp_offset);
    return 1;
}

unsigned coro_stack_used(const coro* c) {
    uintptr_t base, end, sp;
    if (c == NULL || c->stack == NULL) {
        return 0;
    }
    base = (uintptr_t)c->stack;
    end = base + c->stack_size;
    sp = (uintptr_t)c->sp;
    if (sp < base || sp > end) {
        return 0;
    }
    return (unsigned)(end - sp);
}
