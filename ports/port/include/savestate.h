/*
 * savestate.h  Deterministic page-COW savestate snapshots and hashing for GGPO rollback & replays
 */

#ifndef SAVESTATE_H
#define SAVESTATE_H

#include <stddef.h>
#include <stdint.h>

typedef struct SAVESTATE SAVESTATE;

typedef struct SAVESTATE_HASHES {
    uint64_t total;
    uint64_t ram;
    uint64_t data;
    uint64_t bss;
    uint64_t coro;
    uint64_t board;
    uint64_t nl;
    uint64_t tex;
    uint64_t rd;
    uint64_t dc;
    uint64_t snd;
    uint64_t snd_aica;
    uint64_t snd_arm7;
    uint64_t snd_nl;
    uint64_t snd_s3l;
    uint64_t aica_ram;
} SAVESTATE_HASHES;

SAVESTATE* savestate_alloc(void);
void savestate_free(SAVESTATE* s);
int savestate_save(SAVESTATE* s);
int savestate_load(const SAVESTATE* s);
uint64_t savestate_hash(void);
void savestate_hash_detail(SAVESTATE_HASHES* out);
void savestate_reset_pools(void);
size_t savestate_pool_bytes(void);

int savestate_run_self_test(void);

#endif
