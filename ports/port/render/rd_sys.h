/*
 * rd_sys.h  The few SDL calls the back ends make (threads, semaphores, the clock), for SDL2 and,
 * with RD_SDL3 defined, SDL3
 */

#ifndef RD_SYS_H
#define RD_SYS_H

#if defined(__3DS__) || defined(__PSP__)
/* the 3DS draws with its GPU (port/ctr); rd_soft runs there on the one thread, for checks (CTR_SOFT), and on the
   PSP's one CPU */
#include <time.h>
typedef int RD_SEM;
#define rd_sem_wait(s) ((void)(s))
#define rd_sem_post(s) ((void)(s))
#define rd_cpu_count() 1
#define SDL_CreateSemaphore(n) ((RD_SEM*)NULL)
#define SDL_CreateThread(f, name, data) ((void)(f), (void*)NULL)
#define SDL_GetPerformanceCounter() ((uint64_t)clock())
#define SDL_GetPerformanceFrequency() ((uint64_t)CLOCKS_PER_SEC)
#elif defined(MVC2_MISTER) || defined(RD_PTHREADS)
/* POSIX threads: the MiSTer's two A9 cores, or a bench without SDL */
#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
typedef sem_t RD_SEM;
#define rd_sem_wait sem_wait
#define rd_sem_post sem_post
#define rd_cpu_count() ((int)sysconf(_SC_NPROCESSORS_ONLN))
static inline RD_SEM* SDL_CreateSemaphore(unsigned n) {
    RD_SEM* s = malloc(sizeof *s);
    if (s != NULL && sem_init(s, 0, n) != 0) {
        free(s);
        s = NULL;
    }
    return s;
}
typedef struct {
    int (*f)(void*);
    void* data;
} RD_THREAD;
static inline void* rd_thread_main(void* arg) {
    RD_THREAD t = *(RD_THREAD*)arg;
    free(arg);
    t.f(t.data);
    return NULL;
}
static inline void* SDL_CreateThread(int (*f)(void*), const char* name, void* data) {
    RD_THREAD* t = malloc(sizeof *t);
    pthread_t id;
    (void)name;
    if (t == NULL) {
        return NULL;
    }
    t->f = f, t->data = data;
    if (pthread_create(&id, NULL, rd_thread_main, t) != 0) {
        free(t);
        return NULL;
    }
    pthread_detach(id);
    return (void*)1;
}
static inline uint64_t SDL_GetPerformanceCounter(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
#define SDL_GetPerformanceFrequency() ((uint64_t)1000000000u)
#elif defined(RD_SDL3)
#include <SDL3/SDL.h>
typedef SDL_Semaphore RD_SEM;
#define rd_sem_wait SDL_WaitSemaphore
#define rd_sem_post SDL_SignalSemaphore
#define rd_cpu_count SDL_GetNumLogicalCPUCores
#else
#include <SDL.h>
typedef SDL_sem RD_SEM;
#define rd_sem_wait SDL_SemWait
#define rd_sem_post SDL_SemPost
#define rd_cpu_count SDL_GetCPUCount
#endif

#endif
