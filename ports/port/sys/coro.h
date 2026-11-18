/*
 * coro.h  Stackful coroutines for the game's task kernel (coro_win32.c, coro_arm.c)
 */

#ifndef CORO_H
#define CORO_H

#include <stddef.h>

typedef struct coro coro;

coro* coro_self(void);                                  /* the first call adopts the thread */
coro* coro_create(void (*fn)(void*), void* arg, unsigned stack_size);
void coro_restart(coro* c, void (*fn)(void*), void* arg);
void coro_delete(coro* c);
void coro_switch(coro* to);

size_t coro_state_size(const coro* c);
size_t coro_state_save(const coro* c, void* buf, size_t max_bytes);
int coro_state_load(coro* c, const void* buf, size_t bytes);
unsigned coro_stack_used(const coro* c);

#endif
