/*
 * coro_psp.c  Coroutines on the PSP as threads, one running at a time and each waiting on its own semaphore: the
 * PSP stops dead when a thread makes a system call on a stack the kernel did not give it
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <pspkernel.h>
#include <pspthreadman.h>

#include "coro.h"

struct coro {
    SceUID thread, sema;
    int started;
    unsigned stack_size;
    uint32_t fcr31;         /* the creator's FPU modes: a new thread starts with its exceptions on */
    void (*fn)(void*);
    void* arg;
};

static coro main_coro;
static coro* current;
unsigned coro_psp_switches;

static void fail(const char* what, int err) {
    fprintf(stderr, "coro: %s failed (%08X)\n", what, (unsigned)err);
    abort();
}

static int entry(SceSize args, void* argp) {
    coro* c = *(coro**)argp;
    (void)args;
    __asm__ volatile("ctc1 %0, $31" : : "r"(c->fcr31));
    c->fn(c->arg);
    abort();                                            /* a coroutine never returns */
    return 0;
}

static void make_thread(coro* c) {
    __asm__ volatile("cfc1 %0, $31" : "=r"(c->fcr31));
    c->thread = sceKernelCreateThread("game", entry, sceKernelGetThreadCurrentPriority(), c->stack_size,
                                      THREAD_ATTR_USER | THREAD_ATTR_VFPU, NULL);
    if (c->thread < 0) {
        fail("sceKernelCreateThread", c->thread);
    }
    c->started = 0;
}

coro* coro_self(void) {
    if (current == NULL) {
        main_coro.thread = sceKernelGetThreadId();
        main_coro.sema = sceKernelCreateSema("coro", 0, 0, 1, NULL);
        main_coro.started = 1;
        current = &main_coro;
    }
    return current;
}

coro* coro_create(void (*fn)(void*), void* arg, unsigned stack_size) {
    coro* c = calloc(1, sizeof(coro));
    coro_self();
    if (c == NULL) {
        fail("calloc", 0);
    }
    c->sema = sceKernelCreateSema("coro", 0, 0, 1, NULL);
    if (c->sema < 0) {
        fail("sceKernelCreateSema", c->sema);
    }
    c->stack_size = stack_size;
    c->fn = fn;
    c->arg = arg;
    make_thread(c);
    return c;
}

void coro_restart(coro* c, void (*fn)(void*), void* arg) {
    if (c != current && c != &main_coro) {
        sceKernelTerminateDeleteThread(c->thread);
        sceKernelDeleteSema(c->sema);
        c->sema = sceKernelCreateSema("coro", 0, 0, 1, NULL);
        c->fn = fn;
        c->arg = arg;
        make_thread(c);
    }
}

void coro_delete(coro* c) {
    if (c != NULL && c != current && c != &main_coro) {
        sceKernelTerminateDeleteThread(c->thread);
        sceKernelDeleteSema(c->sema);
        free(c);
    }
}

void coro_switch(coro* to) {
    coro* from = coro_self();
    if (to == from) {
        return;
    }
    current = to;
    coro_psp_switches++;
    if (!to->started) {
        to->started = 1;
        sceKernelStartThread(to->thread, sizeof to, &to);
    } else {
        sceKernelSignalSema(to->sema, 1);
    }
    sceKernelWaitSema(from->sema, 1, NULL);
}

static coro* bench_back;

static void bench_fn(void* arg) {
    (void)arg;
    for (;;) {
        coro_switch(bench_back);
    }
}

/* a switch's time in microseconds, from n round trips to a coroutine that does nothing */
float coro_psp_bench(unsigned n) {
    coro* c;
    uint64_t t0;
    unsigned i, before = coro_psp_switches;
    bench_back = coro_self();
    c = coro_create(bench_fn, NULL, 16u << 10);
    coro_switch(c);
    t0 = sceKernelGetSystemTimeWide();
    for (i = 0; i < n; i++) {
        coro_switch(c);
    }
    t0 = sceKernelGetSystemTimeWide() - t0;
    coro_delete(c);
    coro_psp_switches = before;
    return (float)t0 / (float)(2 * n);
}
