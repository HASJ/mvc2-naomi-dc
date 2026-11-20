/*
 * mister_prof.c  MVC2_PROF=file: where the CPUs spend their time, the program counter sampled every millisecond of
 * CPU time and written at the end as "address count" lines, for nm to name. Each sample
 * is kept by its phase and the CPU it ran on; samples in a library also count their caller (the link register), and
 * each thread's samples are counted by CPU. MVC2_PROF_FRAMES=FIRST,LAST: only in those game frames.
 */

#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>

#include "mister_front.h"
#include "naomi_host.h"

#define SHIFT 3                     /* 8 bytes a bucket */
#define CPUS 2
#define SLOTS (MISTER_PHASES * CPUS)
#define THREADS 64

extern char __executable_start[], etext[];
static const char* const phase_names[MISTER_PHASES] = { "game", "present", "sound", "threads" };
volatile int mister_prof_phase;
static pthread_t main_thread;
static uint32_t* counts;        /* nbuckets a slot: a phase on a CPU */
static uint32_t* callers;       /* nbuckets a CPU: the link register of samples outside the program */
static uint32_t nbuckets, outside;
static unsigned first, last = ~0u;
static const char* path;
static __thread int my_tid;
static int tids[THREADS];
static uint32_t tid_n[THREADS][CPUS];

#define OUTN 65536
static uint32_t out_pc[OUTN], out_n[OUTN];

static void count_thread(int cpu) {
    uint32_t k;
    if (my_tid == 0) {
        my_tid = (int)syscall(SYS_gettid);
    }
    for (k = (uint32_t)my_tid % THREADS;; k = (k + 1) % THREADS) {
        int want = 0;
        if (tids[k] == my_tid || __atomic_compare_exchange_n(&tids[k], &want, my_tid, 0, __ATOMIC_RELAXED,
                                                             __ATOMIC_RELAXED) || tids[k] == my_tid) {
            __atomic_fetch_add(&tid_n[k][cpu], 1, __ATOMIC_RELAXED);
            return;
        }
        if (k == ((uint32_t)my_tid + THREADS - 1) % THREADS) {
            return;
        }
    }
}

static void sample(int sig, siginfo_t* info, void* context) {
    uintptr_t pc = ((ucontext_t*)context)->uc_mcontext.arm_pc;
    uintptr_t i = (pc - (uintptr_t)__executable_start) >> SHIFT;
    int phase = pthread_equal(pthread_self(), main_thread) ? mister_prof_phase : MISTER_PHASES - 1;
    int cpu, slot;
    (void)sig, (void)info;
    if (host_frame() < first || host_frame() > last) {
        return;
    }
    cpu = sched_getcpu() & (CPUS - 1);
    slot = phase * CPUS + cpu;
    count_thread(cpu);
    if (i < nbuckets) {
        __atomic_fetch_add(&counts[(uintptr_t)slot * nbuckets + i], 1, __ATOMIC_RELAXED);
    } else {
        uint32_t k = (uint32_t)(pc >> 3) * 2654435761u >> 16, j;
        for (j = 0; j < 64; j++, k = (k + 1) & (OUTN - 1)) {
            uint32_t want = 0;
            uint32_t key = (uint32_t)(pc & ~7u) | (uint32_t)slot;     /* the slot in the bucket's low bits */
            if (out_pc[k] == key || __atomic_compare_exchange_n(&out_pc[k], &want, key, 0, __ATOMIC_RELAXED,
                                                                __ATOMIC_RELAXED) || out_pc[k] == key) {
                __atomic_fetch_add(&out_n[k], 1, __ATOMIC_RELAXED);
                break;
            }
        }
        __atomic_fetch_add(&outside, 1, __ATOMIC_RELAXED);
        i = (((ucontext_t*)context)->uc_mcontext.arm_lr - (uintptr_t)__executable_start) >> SHIFT;
        if (i < nbuckets) {
            __atomic_fetch_add(&callers[(uintptr_t)cpu * nbuckets + i], 1, __ATOMIC_RELAXED);
        }
    }
}

void mister_prof_start(void) {
    struct sigaction sa;
    struct itimerval t = { { 0, 1000 }, { 0, 1000 } };
    const char* frames = getenv("MVC2_PROF_FRAMES");
    path = getenv("MVC2_PROF");
    if (path == NULL) {
        return;
    }
    if (frames != NULL && sscanf(frames, "%u,%u", &first, &last) != 2) {
        first = 0, last = ~0u;
    }
    nbuckets = (uint32_t)((etext - __executable_start) >> SHIFT) + 1;
    counts = calloc((size_t)nbuckets * SLOTS, sizeof *counts);
    callers = calloc((size_t)nbuckets * CPUS, sizeof *callers);
    main_thread = pthread_self();
    if (counts == NULL || callers == NULL) {
        return;
    }
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = sample;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, NULL);
    setitimer(ITIMER_PROF, &t, NULL);
}

static unsigned long at(uint32_t i) {
    return (unsigned long)((uintptr_t)__executable_start + ((uintptr_t)i << SHIFT));
}

void mister_prof_stop(void) {
    struct itimerval off = { { 0, 0 }, { 0, 0 } };
    FILE* f;
    uint32_t i, total = outside;
    int k;
    if (counts == NULL) {
        return;
    }
    setitimer(ITIMER_PROF, &off, NULL);
    f = fopen(path, "w");
    if (f == NULL) {
        return;
    }
    for (k = 0; k < SLOTS; k++) {
        fprintf(f, "phase %s cpu%d\n", phase_names[k / CPUS], k % CPUS);
        for (i = 0; i < nbuckets; i++) {
            uint32_t n = counts[(size_t)k * nbuckets + i];
            if (n != 0) {
                fprintf(f, "%08lx %u\n", at(i), n);
                total += n;
            }
        }
    }
    fprintf(f, "outside %u\n", outside);
    for (k = 0; k < CPUS; k++) {
        for (i = 0; i < nbuckets; i++) {
            if (callers[(size_t)k * nbuckets + i] != 0) {
                fprintf(f, "caller %08lx %u cpu%d\n", at(i), callers[(size_t)k * nbuckets + i], k);
            }
        }
    }
    for (i = 0; i < OUTN; i++) {
        if (out_n[i] != 0) {
            fprintf(f, "out %08x %u %s cpu%u\n", out_pc[i] & ~7u, out_n[i], phase_names[(out_pc[i] & 7) / CPUS],
                    (out_pc[i] & 7) % CPUS);
        }
    }
    for (i = 0; i < THREADS; i++) {
        char name[64] = "?", file[64];
        FILE* c;
        if (tids[i] == 0) {
            continue;
        }
        snprintf(file, sizeof file, "/proc/self/task/%d/comm", tids[i]);
        if ((c = fopen(file, "r")) != NULL) {
            if (fgets(name, sizeof name, c) != NULL) {
                name[strcspn(name, "\n")] = 0;
            }
            fclose(c);
        }
        fprintf(f, "thread %d %s %u %u\n", tids[i], name[0] != 0 ? name : "?", tid_n[i][0], tid_n[i][1]);
    }
    {
        FILE* m = fopen("/proc/self/maps", "r");
        char line[512];
        while (m != NULL && fgets(line, sizeof line, m) != NULL) {
            if (strstr(line, " r-xp ") != NULL) {
                fprintf(f, "map %s", line);
            }
        }
        if (m != NULL) {
            fclose(m);
        }
    }
    fclose(f);
    host_log("prof: %u samples into %s", total, path);
}
