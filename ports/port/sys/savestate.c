/*
 * savestate.c  Deterministic page-COW savestates, hashing, and --savestate-check torture harness
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "savestate.h"
#include "coro.h"
#include "naomi_host.h"
#include "naomi_input.h"
#include "rd.h"
#include "aica.h"
#include "arm7.h"

/* Subsystem state hooks */
extern void nlmath_state_save(unsigned int* rng);
extern void nlmath_state_load(const unsigned int* rng);
extern unsigned int nlproj_state_size(void);
extern void nlproj_state_save(void* out);
extern void nlproj_state_load(const void* in);
extern size_t nl_am_state_size(void);
extern void nl_am_state_save(void* out);
extern void nl_am_state_load(const void* in);
extern size_t nl_work_state_size(void);
extern void nl_work_state_save(void* out);
extern void nl_work_state_load(const void* in);
extern size_t nl_sys_state_size(void);
extern void nl_sys_state_save(void* out);
extern void nl_sys_state_load(const void* in);
extern size_t nl_sprite_state_size(void);
extern void nl_sprite_state_save(void* out);
extern void nl_sprite_state_load(const void* in);
extern size_t nl_strip_state_size(void);
extern void nl_strip_state_save(void* out);
extern void nl_strip_state_load(const void* in);
extern size_t nl_xmtr_state_size(void);
extern void nl_xmtr_state_save(void* out);
extern void nl_xmtr_state_load(const void* in);
extern uint64_t nl_xmtr_state_hash(void);
extern size_t nl_obj_state_size(void);
extern void nl_obj_state_save(void* out);
extern void nl_obj_state_load(const void* in);
extern size_t rd_state_size(void);
extern void rd_state_save(void* out);
extern void rd_state_load(const void* in);
extern uint64_t rd_state_hash(void);
extern size_t nl_tex_state_size(void);
extern void nl_tex_state_init(void* buf);
extern void nl_tex_state_free(void* buf);
extern void nl_tex_state_save(void* buf);
extern void nl_tex_state_load(const void* buf);
extern uint64_t nl_tex_state_hash(void);
extern uint64_t nl_tex_state_snap_hash(const void* buf);
extern size_t dc_gd_state_size(void);
extern void dc_gd_state_save(void* out);
extern void dc_gd_state_load(const void* in);
extern uint64_t dc_gd_state_hash(void);
extern size_t dc_shinobi_state_size(void);
extern void dc_shinobi_state_save(void* out);
extern void dc_shinobi_state_load(const void* in);
extern uint64_t dc_shinobi_state_hash(void);
extern size_t dc_vmu_state_size(void);
extern void dc_vmu_state_save(void* out);
extern void dc_vmu_state_load(const void* in);
extern uint64_t dc_vmu_state_hash(void);
extern size_t dc_adx_state_size(void);
extern void dc_adx_state_save(void* out);
extern void dc_adx_state_load(const void* in);
extern uint64_t dc_adx_state_hash(void);
extern size_t aica_state_size(void);
extern uint64_t aica_state_hash(void);
extern uint64_t aica_state_struct_hash(const AICA_STATE* s);
extern uint64_t arm7_state_hash(void);
extern size_t snd_nl_state_size(void);
extern void snd_nl_state_save(void* out);
extern void snd_nl_state_load(const void* in);
extern uint64_t snd_nl_state_hash(void);
extern size_t s3l_state_size(void);
extern void s3l_state_save(void* out);
extern void s3l_state_load(const void* in);
extern uint64_t s3l_state_hash(void);
extern void sdl_audio_step_silent(void);
extern void sdl_audio_notify_rollback(void);

#define SS_PAGE_SIZE 4096u
#define RAM_PAGES (NAOMI_RAM_SIZE / SS_PAGE_SIZE)

typedef struct COW_PAGE {
    uint32_t refcnt;
    uint32_t immortal;
    uint64_t hash;
    uint8_t data[SS_PAGE_SIZE];
} COW_PAGE;

static COW_PAGE zero_page = { 1, 1, 0ULL, { 0 } };
static size_t live_cow_bytes;

static inline uint64_t fnv_mix_bytes(uint64_t h, const void* ptr, size_t n) {
    const uint64_t* q = (const uint64_t*)ptr;
    const uint8_t* b;
    size_t nq = n / 8, rem = n & 7, i;
    if (((uintptr_t)ptr & 7u) == 0) {
        for (i = 0; i < nq; i++) {
            uint64_t k = q[i];
            h ^= k;
            h *= 0x9E3779B97F4A7C15ULL;
            h ^= h >> 30;
        }
        b = (const uint8_t*)(q + nq);
    } else {
        b = (const uint8_t*)ptr;
        rem = n;
    }
    for (i = 0; i < rem; i++) {
        h ^= b[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static uint64_t hash_block(const void* ptr, size_t n) {
    return fnv_mix_bytes(14695981039346656037ULL, ptr, n);
}

static int is_zero_page(const uint8_t* p, size_t n) {
    size_t i;
    if (n == SS_PAGE_SIZE && ((uintptr_t)p & 7u) == 0) {
        const uint64_t* q = (const uint64_t*)p;
        for (i = 0; i < SS_PAGE_SIZE / 8; i++) {
            if (q[i] != 0) {
                return 0;
            }
        }
        return 1;
    }
    for (i = 0; i < n; i++) {
        if (p[i] != 0) {
            return 0;
        }
    }
    return 1;
}

static COW_PAGE* cow_page_create(const uint8_t* src, size_t n) {
    COW_PAGE* p;
    if (is_zero_page(src, n)) {
        return &zero_page;
    }
    p = (COW_PAGE*)malloc(sizeof(COW_PAGE));
    if (p == NULL) {
        return NULL;
    }
    p->refcnt = 1;
    p->immortal = 0;
    memcpy(p->data, src, n);
    if (n < SS_PAGE_SIZE) {
        memset(p->data + n, 0, SS_PAGE_SIZE - n);
    }
    p->hash = hash_block(p->data, n);
    live_cow_bytes += sizeof(COW_PAGE);
    return p;
}

static void cow_page_retain(COW_PAGE* p) {
    if (p != NULL && !p->immortal) {
        p->refcnt++;
    }
}

static void cow_page_release(COW_PAGE* p) {
    if (p != NULL && !p->immortal && --p->refcnt == 0) {
        live_cow_bytes -= sizeof(COW_PAGE);
        free(p);
    }
}

static void cow_page_replace(COW_PAGE** slot, COW_PAGE* np) {
    cow_page_retain(np);
    cow_page_release(*slot);
    *slot = np;
}

static void cow_array_free(COW_PAGE** arr, size_t count) {
    size_t i;
    if (arr == NULL) {
        return;
    }
    for (i = 0; i < count; i++) {
        cow_page_release(arr[i]);
        arr[i] = NULL;
    }
    free(arr);
}

/* Global current COW mirror of RAM, program .data/.bss, and AICA RAM */
static COW_PAGE* cur_ram[RAM_PAGES];
static int cur_ram_valid;
static COW_PAGE** cur_data;
static size_t cur_data_pages;
static const uint8_t* cur_data_base;
static size_t cur_data_bytes;
static COW_PAGE** cur_bss;
static size_t cur_bss_pages;
static const uint8_t* cur_bss_base;
static size_t cur_bss_bytes;
static COW_PAGE** cur_aica;
static size_t cur_aica_pages;
static size_t cur_aica_bytes;

static void* ww_addrs[RAM_PAGES];
static uint8_t ww_bitmap[RAM_PAGES];

struct SAVESTATE {
    COW_PAGE** ram;
    COW_PAGE** data;
    size_t data_pages;
    size_t data_bytes;
    COW_PAGE** bss;
    size_t bss_pages;
    size_t bss_bytes;
    COW_PAGE** aica;
    size_t aica_pages;
    size_t aica_bytes;

    uint8_t* coro_buf;
    size_t coro_cap;
    size_t coro_len;

    void* board_buf;
    size_t board_sz;

    unsigned int nl_rng;
    void* nlproj_buf;
    size_t nlproj_sz;
    void* nl_am_buf;
    size_t nl_am_sz;
    void* nl_work_buf;
    size_t nl_work_sz;
    void* nl_sys_buf;
    size_t nl_sys_sz;
    void* nl_sprite_buf;
    size_t nl_sprite_sz;
    void* nl_strip_buf;
    size_t nl_strip_sz;
    void* nl_xmtr_buf;
    size_t nl_xmtr_sz;
    void* nl_obj_buf;
    size_t nl_obj_sz;
    void* rd_buf;
    size_t rd_sz;
    void* nl_tex_buf;
    size_t nl_tex_sz;

    void* dc_gd_buf;
    size_t dc_gd_sz;
    void* dc_shinobi_buf;
    size_t dc_shinobi_sz;
    void* dc_vmu_buf;
    size_t dc_vmu_sz;
    void* dc_adx_buf;
    size_t dc_adx_sz;

    AICA_STATE* aica_st;
    ARM7_REGS arm7_regs;
    void* snd_nl_buf;
    size_t snd_nl_sz;
    void* s3l_buf;
    size_t s3l_sz;
};

static size_t page_span_count(size_t bytes) {
    return (bytes + SS_PAGE_SIZE - 1u) / SS_PAGE_SIZE;
}

static size_t page_chunk_len(size_t total_bytes, size_t page_idx) {
    size_t off = page_idx * SS_PAGE_SIZE;
    if (off >= total_bytes) {
        return 0;
    }
    return (total_bytes - off) < SS_PAGE_SIZE ? (total_bytes - off) : SS_PAGE_SIZE;
}

void savestate_reset_pools(void) {
    size_t i;
    for (i = 0; i < RAM_PAGES; i++) {
        cow_page_release(cur_ram[i]);
        cur_ram[i] = NULL;
    }
    cur_ram_valid = 0;
    cow_array_free(cur_data, cur_data_pages);
    cur_data = NULL;
    cur_data_pages = 0;
    cur_data_base = NULL;
    cur_data_bytes = 0;
    cow_array_free(cur_bss, cur_bss_pages);
    cur_bss = NULL;
    cur_bss_pages = 0;
    cur_bss_base = NULL;
    cur_bss_bytes = 0;
    cow_array_free(cur_aica, cur_aica_pages);
    cur_aica = NULL;
    cur_aica_pages = 0;
    cur_aica_bytes = 0;
    naomi_ram_reset_write_watch();
}

size_t savestate_pool_bytes(void) {
    return live_cow_bytes;
}

SAVESTATE* savestate_alloc(void) {
    SAVESTATE* s = (SAVESTATE*)calloc(1, sizeof(SAVESTATE));
    if (s == NULL) {
        return NULL;
    }
    s->ram = (COW_PAGE**)calloc(RAM_PAGES, sizeof(COW_PAGE*));
    s->board_sz = naomi_board_state_size();
    s->board_buf = calloc(1, s->board_sz);
    s->nlproj_sz = nlproj_state_size();
    s->nlproj_buf = calloc(1, s->nlproj_sz);
    s->nl_am_sz = nl_am_state_size();
    s->nl_am_buf = calloc(1, s->nl_am_sz);
    s->nl_work_sz = nl_work_state_size();
    s->nl_work_buf = calloc(1, s->nl_work_sz);
    s->nl_sys_sz = nl_sys_state_size();
    s->nl_sys_buf = calloc(1, s->nl_sys_sz);
    s->nl_sprite_sz = nl_sprite_state_size();
    s->nl_sprite_buf = calloc(1, s->nl_sprite_sz);
    s->nl_strip_sz = nl_strip_state_size();
    s->nl_strip_buf = calloc(1, s->nl_strip_sz);
    s->nl_xmtr_sz = nl_xmtr_state_size();
    s->nl_xmtr_buf = calloc(1, s->nl_xmtr_sz);
    s->nl_obj_sz = nl_obj_state_size();
    s->nl_obj_buf = calloc(1, s->nl_obj_sz);
    s->rd_sz = rd_state_size();
    s->rd_buf = calloc(1, s->rd_sz);
    s->nl_tex_sz = nl_tex_state_size();
    s->nl_tex_buf = calloc(1, s->nl_tex_sz);
    if (s->nl_tex_buf != NULL) {
        nl_tex_state_init(s->nl_tex_buf);
    }
    s->dc_gd_sz = dc_gd_state_size();
    s->dc_gd_buf = calloc(1, s->dc_gd_sz);
    s->dc_shinobi_sz = dc_shinobi_state_size();
    s->dc_shinobi_buf = calloc(1, s->dc_shinobi_sz);
    s->dc_vmu_sz = dc_vmu_state_size();
    s->dc_vmu_buf = calloc(1, s->dc_vmu_sz);
    s->dc_adx_sz = dc_adx_state_size();
    s->dc_adx_buf = calloc(1, s->dc_adx_sz);
    s->aica_st = aica_state_new();
    s->snd_nl_sz = snd_nl_state_size();
    s->snd_nl_buf = calloc(1, s->snd_nl_sz);
    s->s3l_sz = s3l_state_size();
    s->s3l_buf = calloc(1, s->s3l_sz);

    if (!s->ram || !s->board_buf || !s->nlproj_buf || !s->nl_am_buf || !s->nl_work_buf || !s->nl_sys_buf ||
        !s->nl_sprite_buf || !s->nl_strip_buf || !s->nl_xmtr_buf || !s->nl_obj_buf || !s->rd_buf || !s->nl_tex_buf ||
        !s->dc_gd_buf || !s->dc_shinobi_buf || !s->dc_vmu_buf || !s->dc_adx_buf || !s->aica_st || !s->snd_nl_buf ||
        !s->s3l_buf) {
        savestate_free(s);
        return NULL;
    }
    return s;
}

void savestate_free(SAVESTATE* s) {
    if (s == NULL) {
        return;
    }
    cow_array_free(s->ram, RAM_PAGES);
    cow_array_free(s->data, s->data_pages);
    cow_array_free(s->bss, s->bss_pages);
    cow_array_free(s->aica, s->aica_pages);
    free(s->coro_buf);
    free(s->board_buf);
    free(s->nlproj_buf);
    free(s->nl_am_buf);
    free(s->nl_work_buf);
    free(s->nl_sys_buf);
    free(s->nl_sprite_buf);
    free(s->nl_strip_buf);
    free(s->nl_xmtr_buf);
    free(s->nl_obj_buf);
    free(s->rd_buf);
    if (s->nl_tex_buf != NULL) {
        nl_tex_state_free(s->nl_tex_buf);
        free(s->nl_tex_buf);
    }
    free(s->dc_gd_buf);
    free(s->dc_shinobi_buf);
    free(s->dc_vmu_buf);
    free(s->dc_adx_buf);
    free(s->aica_st);
    free(s->snd_nl_buf);
    free(s->s3l_buf);
    free(s);
}

static int cow_sync_region(const uint8_t* live, size_t bytes, COW_PAGE*** cur_ptr, size_t* cur_pages,
                           const uint8_t** cur_base, size_t* cur_bytes, COW_PAGE*** dst_ptr, size_t* dst_pages,
                           size_t* dst_bytes) {
    size_t pages = page_span_count(bytes);
    size_t i;
    if (*cur_ptr != NULL && (*cur_base != live || *cur_bytes != bytes)) {
        cow_array_free(*cur_ptr, *cur_pages);
        *cur_ptr = NULL;
    }
    if (*cur_ptr == NULL && pages > 0) {
        *cur_ptr = (COW_PAGE**)calloc(pages, sizeof(COW_PAGE*));
        if (*cur_ptr == NULL) {
            return 0;
        }
        *cur_pages = pages;
        *cur_base = live;
        *cur_bytes = bytes;
    }
    if (*dst_ptr != NULL && *dst_pages != pages) {
        cow_array_free(*dst_ptr, *dst_pages);
        *dst_ptr = NULL;
    }
    if (*dst_ptr == NULL && pages > 0) {
        *dst_ptr = (COW_PAGE**)calloc(pages, sizeof(COW_PAGE*));
        if (*dst_ptr == NULL) {
            return 0;
        }
        *dst_pages = pages;
    }
    *dst_bytes = bytes;
    for (i = 0; i < pages; i++) {
        size_t n = page_chunk_len(bytes, i);
        const uint8_t* pg = live + i * SS_PAGE_SIZE;
        COW_PAGE* cur = (*cur_ptr)[i];
        if (cur == NULL || memcmp(cur->data, pg, n) != 0) {
            COW_PAGE* np = cow_page_create(pg, n);
            if (np == NULL) {
                return 0;
            }
            cow_page_release(cur);
            (*cur_ptr)[i] = np;
        }
        cow_page_replace(&(*dst_ptr)[i], (*cur_ptr)[i]);
    }
    return 1;
}

static int cow_load_region(uint8_t* live, size_t bytes, COW_PAGE** cur_arr, COW_PAGE* const* snap_arr, size_t pages) {
    size_t i;
    int low_changed = 0;
    for (i = 0; i < pages; i++) {
        size_t n = page_chunk_len(bytes, i);
        uint8_t* pg = live + i * SS_PAGE_SIZE;
        COW_PAGE* sp = snap_arr[i];
        if (cur_arr[i] != sp || memcmp(pg, sp->data, n) != 0) {
            if (memcmp(pg, sp->data, n) != 0) {
                memcpy(pg, sp->data, n);
                if (i * SS_PAGE_SIZE < 0x20000u) {
                    low_changed = 1;
                }
            }
            cow_page_replace(&cur_arr[i], sp);
        }
    }
    return low_changed;
}

static int ram_save_cow(SAVESTATE* s) {
    const uint8_t* base = (const uint8_t*)(uintptr_t)NAOMI_RAM_BASE;
    size_t p;
    if (!cur_ram_valid || !naomi_ram_has_write_watch()) {
        for (p = 0; p < RAM_PAGES; p++) {
            const uint8_t* pg = base + p * SS_PAGE_SIZE;
            if (cur_ram[p] == NULL || memcmp(cur_ram[p]->data, pg, SS_PAGE_SIZE) != 0) {
                COW_PAGE* np = cow_page_create(pg, SS_PAGE_SIZE);
                if (np == NULL) {
                    return 0;
                }
                cow_page_release(cur_ram[p]);
                cur_ram[p] = np;
            }
            cow_page_replace(&s->ram[p], cur_ram[p]);
        }
        cur_ram_valid = 1;
        naomi_ram_reset_write_watch();
        return 1;
    }

    {
        unsigned long cnt = RAM_PAGES;
        unsigned long i;
        if (!naomi_ram_get_dirty_pages(ww_addrs, &cnt)) {
            cur_ram_valid = 0;
            return ram_save_cow(s);
        }
        for (i = 0; i < cnt; i++) {
            uintptr_t a = (uintptr_t)ww_addrs[i];
            if (a >= NAOMI_RAM_BASE && a < NAOMI_RAM_BASE + NAOMI_RAM_SIZE) {
                p = (a - NAOMI_RAM_BASE) / SS_PAGE_SIZE;
                if (memcmp(cur_ram[p]->data, (const void*)a, SS_PAGE_SIZE) != 0) {
                    COW_PAGE* np = cow_page_create((const uint8_t*)a, SS_PAGE_SIZE);
                    if (np == NULL) {
                        return 0;
                    }
                    cow_page_release(cur_ram[p]);
                    cur_ram[p] = np;
                }
            }
        }
        for (p = 0; p < RAM_PAGES; p++) {
            cow_page_replace(&s->ram[p], cur_ram[p]);
        }
    }
    return 1;
}

static void ram_load_cow(const SAVESTATE* s) {
    uint8_t* base = (uint8_t*)(uintptr_t)NAOMI_RAM_BASE;
    size_t p;
    if (cur_ram_valid && naomi_ram_has_write_watch()) {
        unsigned long cnt = RAM_PAGES, i;
        if (naomi_ram_get_dirty_pages(ww_addrs, &cnt)) {
            memset(ww_bitmap, 0, sizeof ww_bitmap);
            for (i = 0; i < cnt; i++) {
                uintptr_t a = (uintptr_t)ww_addrs[i];
                if (a >= NAOMI_RAM_BASE && a < NAOMI_RAM_BASE + NAOMI_RAM_SIZE) {
                    ww_bitmap[(a - NAOMI_RAM_BASE) / SS_PAGE_SIZE] = 1;
                }
            }
            for (p = 0; p < RAM_PAGES; p++) {
                if (ww_bitmap[p] || cur_ram[p] != s->ram[p]) {
                    uint8_t* dst = base + p * SS_PAGE_SIZE;
                    if (memcmp(dst, s->ram[p]->data, SS_PAGE_SIZE) != 0) {
                        memcpy(dst, s->ram[p]->data, SS_PAGE_SIZE);
                    }
                    cow_page_replace(&cur_ram[p], s->ram[p]);
                }
            }
            naomi_ram_reset_write_watch();
            return;
        }
    }
    for (p = 0; p < RAM_PAGES; p++) {
        uint8_t* dst = base + p * SS_PAGE_SIZE;
        if (memcmp(dst, s->ram[p]->data, SS_PAGE_SIZE) != 0) {
            memcpy(dst, s->ram[p]->data, SS_PAGE_SIZE);
        }
        cow_page_replace(&cur_ram[p], s->ram[p]);
    }
    cur_ram_valid = 1;
    naomi_ram_reset_write_watch();
}

int savestate_save(SAVESTATE* s) {
    const HOST_PROGRAM* prog = board_program();
    coro* gc = board_game_coro();
    size_t need, data_len, bss_len, aica_len;
    if (s == NULL || prog == NULL || gc == NULL) {
        return 0;
    }
    if (!ram_save_cow(s)) {
        return 0;
    }
    data_len = (size_t)(prog->span.data_end - prog->span.data);
    bss_len = (size_t)(prog->span.bss_end - prog->span.bss);
    if (!cow_sync_region((const uint8_t*)prog->span.data, data_len, &cur_data, &cur_data_pages, &cur_data_base,
                         &cur_data_bytes, &s->data, &s->data_pages, &s->data_bytes)) {
        return 0;
    }
    if (!cow_sync_region((const uint8_t*)prog->span.bss, bss_len, &cur_bss, &cur_bss_pages, &cur_bss_base,
                         &cur_bss_bytes, &s->bss, &s->bss_pages, &s->bss_bytes)) {
        return 0;
    }
    aica_len = aica_ram_size();
    if (aica_len > 0 && aica_ram != NULL) {
        static const uint8_t* aica_dummy_base;
        if (!cow_sync_region(aica_ram, aica_len, &cur_aica, &cur_aica_pages, &aica_dummy_base, &cur_aica_bytes,
                             &s->aica, &s->aica_pages, &s->aica_bytes)) {
            return 0;
        }
    }
    need = coro_state_size(gc);
    if (need == 0) {
        return 0;
    }
    if (need > s->coro_cap) {
        uint8_t* nb = (uint8_t*)realloc(s->coro_buf, need);
        if (nb == NULL) {
            return 0;
        }
        s->coro_buf = nb;
        s->coro_cap = need;
    }
    s->coro_len = coro_state_save(gc, s->coro_buf, s->coro_cap);
    if (s->coro_len == 0) {
        return 0;
    }
    naomi_board_state_save(s->board_buf);
    nlmath_state_save(&s->nl_rng);
    nlproj_state_save(s->nlproj_buf);
    nl_am_state_save(s->nl_am_buf);
    nl_work_state_save(s->nl_work_buf);
    nl_sys_state_save(s->nl_sys_buf);
    nl_sprite_state_save(s->nl_sprite_buf);
    nl_strip_state_save(s->nl_strip_buf);
    nl_xmtr_state_save(s->nl_xmtr_buf);
    nl_obj_state_save(s->nl_obj_buf);
    rd_state_save(s->rd_buf);
    nl_tex_state_save(s->nl_tex_buf);
    dc_gd_state_save(s->dc_gd_buf);
    dc_shinobi_state_save(s->dc_shinobi_buf);
    dc_vmu_state_save(s->dc_vmu_buf);
    dc_adx_state_save(s->dc_adx_buf);
    aica_state_save(s->aica_st);
    arm7_get_regs(&s->arm7_regs);
    snd_nl_state_save(s->snd_nl_buf);
    s3l_state_save(s->s3l_buf);
    return 1;
}

int savestate_load(const SAVESTATE* s) {
    const HOST_PROGRAM* prog = board_program();
    coro* gc = board_game_coro();
    size_t data_len, bss_len, aica_len;
    if (s == NULL || prog == NULL || gc == NULL) {
        return 0;
    }
    ram_load_cow(s);
    data_len = (size_t)(prog->span.data_end - prog->span.data);
    bss_len = (size_t)(prog->span.bss_end - prog->span.bss);
    if (s->data_bytes != data_len || s->bss_bytes != bss_len) {
        return 0;
    }
    if (cur_data != NULL && cur_data_bytes == data_len) {
        cow_load_region((uint8_t*)prog->span.data, data_len, cur_data, s->data, s->data_pages);
    }
    if (cur_bss != NULL && cur_bss_bytes == bss_len) {
        cow_load_region((uint8_t*)prog->span.bss, bss_len, cur_bss, s->bss, s->bss_pages);
    }
    aica_len = aica_ram_size();
    if (aica_len > 0 && aica_ram != NULL && s->aica_bytes == aica_len && cur_aica != NULL) {
        if (cow_load_region(aica_ram, aica_len, cur_aica, s->aica, s->aica_pages)) {
            arm7_code_written(0, 0x20000u);
        }
    }
    if (!coro_state_load(gc, s->coro_buf, s->coro_len)) {
        return 0;
    }
    naomi_board_state_load(s->board_buf);
    nlmath_state_load(&s->nl_rng);
    nlproj_state_load(s->nlproj_buf);
    nl_am_state_load(s->nl_am_buf);
    nl_work_state_load(s->nl_work_buf);
    nl_sys_state_load(s->nl_sys_buf);
    nl_sprite_state_load(s->nl_sprite_buf);
    nl_strip_state_load(s->nl_strip_buf);
    nl_xmtr_state_load(s->nl_xmtr_buf);
    nl_obj_state_load(s->nl_obj_buf);
    rd_state_load(s->rd_buf);
    nl_tex_state_load(s->nl_tex_buf);
    dc_gd_state_load(s->dc_gd_buf);
    dc_shinobi_state_load(s->dc_shinobi_buf);
    dc_vmu_state_load(s->dc_vmu_buf);
    dc_adx_state_load(s->dc_adx_buf);
    aica_state_load(s->aica_st);
    arm7_set_regs(&s->arm7_regs);
    snd_nl_state_load(s->snd_nl_buf);
    s3l_state_load(s->s3l_buf);
    return 1;
}

static uint8_t* hash_tmp_buf;
static size_t hash_tmp_cap;

static uint64_t hash_subsys(size_t sz, void (*save_fn)(void*)) {
    if (sz > hash_tmp_cap) {
        uint8_t* nb = (uint8_t*)realloc(hash_tmp_buf, sz);
        if (nb == NULL) {
            abort();
        }
        hash_tmp_buf = nb;
        hash_tmp_cap = sz;
    }
    memset(hash_tmp_buf, 0, sz);
    save_fn(hash_tmp_buf);
    return hash_block(hash_tmp_buf, sz);
}

void savestate_hash_detail(SAVESTATE_HASHES* out) {
    const HOST_PROGRAM* prog = board_program();
    coro* gc = board_game_coro();
    static uint8_t tmp_coro[1u << 20];
    uint64_t h = 14695981039346656037ULL;
    size_t clen;
    unsigned int rng;

    memset(out, 0, sizeof *out);
    if (prog == NULL || gc == NULL) {
        return;
    }
    out->ram = hash_block((const void*)(uintptr_t)NAOMI_RAM_BASE, NAOMI_RAM_SIZE);
    out->data = hash_block(prog->span.data, (size_t)(prog->span.data_end - prog->span.data));
    out->bss = hash_block(prog->span.bss, (size_t)(prog->span.bss_end - prog->span.bss));

    clen = coro_state_save(gc, tmp_coro, sizeof tmp_coro);
    out->coro = clen > 0 ? hash_block(tmp_coro, clen) : 0;

    out->board = naomi_board_state_hash();

    nlmath_state_save(&rng);
    out->nl = hash_block(&rng, sizeof rng);
    out->nl ^= hash_subsys(nlproj_state_size(), nlproj_state_save) * 0x9E3779B97F4A7C15ULL;
    out->nl ^= hash_subsys(nl_am_state_size(), nl_am_state_save) * 0xBF58476D1CE4E5B9ULL;
    out->nl ^= hash_subsys(nl_work_state_size(), nl_work_state_save) * 0x94D049BB133111EBULL;
    out->nl ^= hash_subsys(nl_sys_state_size(), nl_sys_state_save) * 0xD6E8FEB86659FD93ULL;
    out->nl ^= hash_subsys(nl_sprite_state_size(), nl_sprite_state_save) * 0xA0761D6478BD642FULL;
    out->nl ^= hash_subsys(nl_strip_state_size(), nl_strip_state_save) * 0xE7037ED1A0B428DBULL;
    out->nl ^= nl_xmtr_state_hash() * 0x8EBC6AF09C88C6E3ULL;
    out->nl ^= hash_subsys(nl_obj_state_size(), nl_obj_state_save) * 0x589965CC75374CC3ULL;

    out->tex = nl_tex_state_hash();
    out->rd = rd_state_hash();

    out->dc = dc_gd_state_hash();
    out->dc ^= dc_shinobi_state_hash() * 0x9E3779B97F4A7C15ULL;
    out->dc ^= dc_vmu_state_hash() * 0xBF58476D1CE4E5B9ULL;
    out->dc ^= dc_adx_state_hash() * 0x94D049BB133111EBULL;

    out->snd_aica = aica_state_hash();
    out->snd_arm7 = arm7_state_hash();
    out->snd_nl = snd_nl_state_hash();
    out->snd_s3l = s3l_state_hash();
    out->snd = out->snd_aica ^ (out->snd_arm7 * 0x9E3779B97F4A7C15ULL) ^
               (out->snd_nl * 0xBF58476D1CE4E5B9ULL) ^ (out->snd_s3l * 0x94D049BB133111EBULL);

    out->aica_ram = (aica_ram != NULL && aica_ram_size() > 0) ? hash_block(aica_ram, aica_ram_size()) : 0;

    h = fnv_mix_bytes(h, &out->ram, sizeof(SAVESTATE_HASHES) - sizeof(uint64_t));
    out->total = h;
}

uint64_t savestate_hash(void) {
    SAVESTATE_HASHES d;
    savestate_hash_detail(&d);
    return d.total;
}

static uint64_t hash_cow_array(COW_PAGE* const* arr, size_t n) {
    uint64_t h = 14695981039346656037ULL;
    size_t i;
    if (!arr) return 0ULL;
    for (i = 0; i < n; i++) {
        uint64_t ph = arr[i] ? arr[i]->hash : 0ULL;
        h ^= ph;
        h *= 0x9E3779B97F4A7C15ULL;
        h ^= h >> 30;
    }
    return h;
}

/* Sub-100us checksum over a captured SAVESTATE using cached COW_PAGE hashes and flat buffers. */
uint64_t savestate_fast_checksum(const SAVESTATE* s) {
    uint64_t vec[27];
    uint64_t aica_h;
    if (!s) return 0ULL;
    aica_h = aica_state_struct_hash(s->aica_st);
    vec[0]  = hash_cow_array(s->ram, RAM_PAGES);
    vec[1]  = hash_cow_array(s->data, s->data_pages);
    vec[2]  = hash_cow_array(s->bss, s->bss_pages);
    vec[3]  = hash_cow_array(s->aica, s->aica_pages);
    vec[4]  = (s->coro_buf && s->coro_len) ? hash_block(s->coro_buf, s->coro_len) : 0ULL;
    vec[5]  = (s->board_buf && s->board_sz) ? hash_block(s->board_buf, s->board_sz) : 0ULL;
    vec[6]  = (uint64_t)s->nl_rng;
    vec[7]  = (s->nlproj_buf && s->nlproj_sz) ? hash_block(s->nlproj_buf, s->nlproj_sz) : 0ULL;
    vec[8]  = (s->nl_am_buf && s->nl_am_sz) ? hash_block(s->nl_am_buf, s->nl_am_sz) : 0ULL;
    vec[9]  = (s->nl_work_buf && s->nl_work_sz) ? hash_block(s->nl_work_buf, s->nl_work_sz) : 0ULL;
    vec[10] = (s->nl_sys_buf && s->nl_sys_sz) ? hash_block(s->nl_sys_buf, s->nl_sys_sz) : 0ULL;
    vec[11] = (s->nl_sprite_buf && s->nl_sprite_sz) ? hash_block(s->nl_sprite_buf, s->nl_sprite_sz) : 0ULL;
    vec[12] = (s->nl_strip_buf && s->nl_strip_sz) ? hash_block(s->nl_strip_buf, s->nl_strip_sz) : 0ULL;
    vec[13] = (s->nl_xmtr_buf && s->nl_xmtr_sz) ? hash_block(s->nl_xmtr_buf, s->nl_xmtr_sz) : 0ULL;
    vec[14] = (s->nl_obj_buf && s->nl_obj_sz) ? hash_block(s->nl_obj_buf, s->nl_obj_sz) : 0ULL;
    vec[15] = (s->rd_buf && s->rd_sz) ? hash_block(s->rd_buf, s->rd_sz) : 0ULL;
    vec[16] = nl_tex_state_snap_hash(s->nl_tex_buf);
    vec[17] = (s->dc_gd_buf && s->dc_gd_sz) ? hash_block(s->dc_gd_buf, s->dc_gd_sz) : 0ULL;
    vec[18] = (s->dc_shinobi_buf && s->dc_shinobi_sz) ? hash_block(s->dc_shinobi_buf, s->dc_shinobi_sz) : 0ULL;
    vec[19] = (s->dc_vmu_buf && s->dc_vmu_sz) ? hash_block(s->dc_vmu_buf, s->dc_vmu_sz) : 0ULL;
    vec[20] = (s->dc_adx_buf && s->dc_adx_sz) ? hash_block(s->dc_adx_buf, s->dc_adx_sz) : 0ULL;
    vec[21] = aica_h;
    vec[22] = hash_block(&s->arm7_regs, sizeof s->arm7_regs);
    vec[23] = (s->snd_nl_buf && s->snd_nl_sz) ? hash_block(s->snd_nl_buf, s->snd_nl_sz) : 0ULL;
    vec[24] = (s->s3l_buf && s->s3l_sz) ? hash_block(s->s3l_buf, s->s3l_sz) : 0ULL;
    vec[25] = (uint64_t)s->data_bytes;
    vec[26] = (uint64_t)s->bss_bytes;
    return hash_block(vec, sizeof vec);
}

static double now_ms(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    return 0.0;
#endif
}

static void scripted_inputs(int frame, uint32_t* p1, uint32_t* p2) {
    *p1 = 0;
    *p2 = 0;
    if (frame < 300) {
        /* Character select: step cursors & confirm 3 characters + assists + stage */
        if (frame == 30) *p1 = JVS_RIGHT;
        if (frame == 45 || frame == 75 || frame == 105) *p1 = JVS_B1;
        if (frame == 55 || frame == 85 || frame == 115) *p1 = JVS_B1;
        if (frame == 35) *p2 = JVS_DOWN;
        if (frame == 50 || frame == 80 || frame == 110) *p2 = JVS_B1;
        if (frame == 60 || frame == 90 || frame == 120) *p2 = JVS_B1;
        if (frame == 145 || frame == 165) {
            *p1 = JVS_B1;
            *p2 = JVS_B1;
        }
        return;
    }
    {
        int m = (frame - 300) % 90;
        if (m < 12) *p1 |= JVS_RIGHT;
        else if (m < 24) *p1 |= JVS_LEFT;
        if (m == 14) *p1 |= HOST_DASH;
        if (m == 26) *p1 |= HOST_SJUMP;
        if (m == 32) *p1 |= JVS_DOWN | JVS_B1;
        if (m == 38) *p1 |= JVS_DOWN | JVS_RIGHT | JVS_B2;
        if (m == 46) *p1 |= JVS_B3;
        if (m == 56) *p1 |= JVS_B1 | JVS_B2;
        if (m == 68) *p1 |= JVS_B4 | JVS_B5;

        if (m >= 10 && m < 22) *p2 |= JVS_LEFT;
        else if (m >= 22 && m < 34) *p2 |= JVS_RIGHT;
        if (m == 18) *p2 |= JVS_DOWN | JVS_B4;
        if (m == 36) *p2 |= JVS_B6;
        if (m == 50) *p2 |= HOST_DASH | JVS_LEFT;
        if (m == 62) *p2 |= JVS_DOWN | JVS_LEFT | JVS_B5;
        if (m == 74) *p2 |= JVS_UP | JVS_B2;
    }
}

static void step_sim_frame(uint32_t p1, uint32_t p2) {
    HOST_INPUT in;
    memset(&in, 0, sizeof in);
    in.player[0] = p1;
    in.player[1] = p2;
    host_set_input(&in);
    naomi_run_frame();
    sdl_audio_step_silent();
}

static void log_hash_diff(const char* tag, int f, int k, const SAVESTATE_HASHES* a, const SAVESTATE_HASHES* b) {
    host_log("SAVESTATE_TEST FAIL [%s] f=%d k=%d total %016llX!=%016llX", tag, f, k, (unsigned long long)a->total,
             (unsigned long long)b->total);
    if (a->ram != b->ram) host_log("  diff ram: %016llX vs %016llX", (unsigned long long)a->ram, (unsigned long long)b->ram);
    if (a->data != b->data) host_log("  diff data: %016llX vs %016llX", (unsigned long long)a->data, (unsigned long long)b->data);
    if (a->bss != b->bss) host_log("  diff bss: %016llX vs %016llX", (unsigned long long)a->bss, (unsigned long long)b->bss);
    if (a->coro != b->coro) host_log("  diff coro: %016llX vs %016llX", (unsigned long long)a->coro, (unsigned long long)b->coro);
    if (a->board != b->board) host_log("  diff board: %016llX vs %016llX", (unsigned long long)a->board, (unsigned long long)b->board);
    if (a->nl != b->nl) host_log("  diff nl: %016llX vs %016llX", (unsigned long long)a->nl, (unsigned long long)b->nl);
    if (a->tex != b->tex) host_log("  diff tex: %016llX vs %016llX", (unsigned long long)a->tex, (unsigned long long)b->tex);
    if (a->rd != b->rd) host_log("  diff rd: %016llX vs %016llX", (unsigned long long)a->rd, (unsigned long long)b->rd);
    if (a->dc != b->dc) host_log("  diff dc: %016llX vs %016llX", (unsigned long long)a->dc, (unsigned long long)b->dc);
    if (a->snd != b->snd) host_log("  diff snd: %016llX vs %016llX (aica=%016llX/%016llX arm7=%016llX/%016llX nl=%016llX/%016llX s3l=%016llX/%016llX)",
                                   (unsigned long long)a->snd, (unsigned long long)b->snd,
                                   (unsigned long long)a->snd_aica, (unsigned long long)b->snd_aica,
                                   (unsigned long long)a->snd_arm7, (unsigned long long)b->snd_arm7,
                                   (unsigned long long)a->snd_nl, (unsigned long long)b->snd_nl,
                                   (unsigned long long)a->snd_s3l, (unsigned long long)b->snd_s3l);
    if (a->aica_ram != b->aica_ram) host_log("  diff aica_ram: %016llX vs %016llX", (unsigned long long)a->aica_ram, (unsigned long long)b->aica_ram);
}

int savestate_run_self_test(void) {
    const HOST_PROGRAM* prog = board_program();
    SAVESTATE* snap;
    int boot = 0, f, k, checks = 0;
    double save_ms = 0.0, load_ms = 0.0;

    if (prog == NULL || prog->match == NULL) {
        host_log("SAVESTATE_TEST FAIL: no Dreamcast match API (run with --dc)");
        return 1;
    }
    rd_discard = 1;
    while (!prog->match->is_ready() && boot < 600) {
        step_sim_frame(0, 0);
        boot++;
    }
    for (k = 0; k < 10; k++) {
        step_sim_frame(0, 0);
        boot++;
    }
    if (!prog->match->is_ready()) {
        host_log("SAVESTATE_TEST FAIL: Dreamcast game did not become ready within %d frames", boot);
        return 1;
    }
    dc_vmu_set_netplay_lock(1);
    host_set_netplay_canonical(1);
    host_clock_set_override(1, 946684800LL);
    prog->match->start_versus(0x12345678u);
    savestate_reset_pools();

    snap = savestate_alloc();
    if (snap == NULL) {
        host_log("SAVESTATE_TEST FAIL: savestate_alloc returned NULL");
        return 1;
    }

    for (f = 0; f < 1800; f++) {
        uint32_t p1, p2;
        if (f % 8 == 0 && f + 8 < 1800) {
            SAVESTATE_HASHES h_pre, h_post_save, h1[8], h2[8];
            double t0, t1;
            savestate_hash_detail(&h_pre);
            t0 = now_ms();
            if (!savestate_save(snap)) {
                host_log("SAVESTATE_TEST FAIL: savestate_save failed at f=%d", f);
                savestate_free(snap);
                return 1;
            }
            t1 = now_ms();
            save_ms += (t1 - t0);
            savestate_hash_detail(&h_post_save);
            if (h_pre.total != h_post_save.total) {
                log_hash_diff("after_save", f, -1, &h_pre, &h_post_save);
                savestate_free(snap);
                return 1;
            }
            for (k = 0; k < 8; k++) {
                scripted_inputs(f + k, &p1, &p2);
                step_sim_frame(p1, p2);
                savestate_hash_detail(&h1[k]);
            }
            t0 = now_ms();
            if (!savestate_load(snap)) {
                host_log("SAVESTATE_TEST FAIL: savestate_load failed at f=%d", f);
                savestate_free(snap);
                return 1;
            }
            t1 = now_ms();
            load_ms += (t1 - t0);
            savestate_hash_detail(&h_post_save);
            if (h_pre.total != h_post_save.total) {
                log_hash_diff("after_load", f, -1, &h_pre, &h_post_save);
                savestate_free(snap);
                return 1;
            }
            for (k = 0; k < 8; k++) {
                scripted_inputs(f + k, &p1, &p2);
                step_sim_frame(p1, p2);
                savestate_hash_detail(&h2[k]);
                if (h1[k].total != h2[k].total) {
                    log_hash_diff("resim", f, k, &h1[k], &h2[k]);
                    savestate_free(snap);
                    return 1;
                }
            }
            checks++;
            f += 7; /* outer f++ advances to f + 8 */
            continue;
        }
        scripted_inputs(f, &p1, &p2);
        step_sim_frame(p1, p2);
    }

    host_log("SAVESTATE_TEST PASS frames=1800 checks=%d save_avg_ms=%.3f load_avg_ms=%.3f pool_kb=%u write_watch=%d",
             checks, checks > 0 ? save_ms / checks : 0.0, checks > 0 ? load_ms / checks : 0.0,
             (unsigned)(savestate_pool_bytes() / 1024u), naomi_ram_has_write_watch());
    savestate_free(snap);
    savestate_reset_pools();
    return 0;
}
