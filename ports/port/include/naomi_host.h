/*
 * naomi_host.h  What the port supplies in place of the NAOMI board and Sega's libraries
 */

#ifndef NAOMI_HOST_H
#define NAOMI_HOST_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "mvc2_match_api.h"

/* mapped at its arcade address: the game keeps buffers there */
#define NAOMI_RAM_BASE  0x0C000000u
#define NAOMI_RAM_SIZE  0x02000000u

/* a word of the program's data that points at what the host defines (port/tools/gen_host.py) */
typedef struct {
    char* at;
    char* to;
} HOST_RELOC;

extern const HOST_RELOC host_reloc[];

/* .data and .bss, between port/game/span_begin.c and span_end.c */
typedef struct {
    char* data;
    char* data_end;
    char* bss;
    char* bss_end;
} HOST_SPAN;

/* port/game/program.c */
typedef struct {
    void (*main)(void);
    const HOST_RELOC* reloc;
    HOST_SPAN span;
    const MVC2_MATCH_API* match;
} HOST_PROGRAM;

extern const HOST_PROGRAM arc_program;
extern const HOST_PROGRAM dc_program;

/* the cartridge's ROM as one address space (naomi_rom.c) */
int naomi_rom_open(const char* path);
extern uint32_t naomi_rom_cache_bytes;      /* the most the ROM cache holds (the 3DS front end sets it) */
const char* naomi_rom_check(const char* path);  /* a file missing or the wrong size, or NULL */
int naomi_rom_read(uint32_t offset, void* dst, uint32_t size);   /* 0 past the end */
uint32_t naomi_rom_size(void);
void naomi_rom_close(void);
/* a front end's own reads of the ROM's 64 KB blocks, from the image naomi_rom_make_image writes */
typedef struct {
    int (*open)(const char* dir);       /* 1: it reads the ROM in dir (else the files are read as usual) */
    int (*read)(uint32_t block, uint32_t last, void* dst);  /* a block the cache lacks, now: 1 when read; the
                                                               request it is in runs to last */
    void (*missed)(uint32_t block, uint32_t last);  /* after one: the request it was in runs to last */
    void (*close)(void);
} NAOMI_ROM_READER;
void naomi_rom_set_reader(const NAOMI_ROM_READER* reader);
int naomi_rom_make_image(const char* dir, const char* path, void (*progress)(uint32_t done, uint32_t total));
const char* naomi_rom_image_name(void);
int naomi_rom_cached(uint32_t block);
void naomi_rom_put(uint32_t block, const void* data);   /* a block read ahead, into the cache */
/* the set's files from its zip (naomi_zip.c) */
typedef struct NAOMI_ZIP NAOMI_ZIP;
NAOMI_ZIP* naomi_zip_open(const char* path);    /* NULL if no zip */
int naomi_zip_has(const NAOMI_ZIP* zip, const char* name, uint32_t size);
int naomi_zip_read(NAOMI_ZIP* zip, const char* name, uint8_t* dst, uint32_t size,
                   void (*step)(uint32_t done));   /* 1 when read, CRC checked; step is called as it goes */
void naomi_zip_close(NAOMI_ZIP* zip);
void naomi_m1_read(uint32_t src, uint8_t* dst, uint32_t size);     /* a packed file (naomi_m1.c) */

/* the board (naomi_board.c) */
int naomi_map_ram(void);
int naomi_init(const HOST_PROGRAM* program, const char* rom_path);
void board_start(const HOST_PROGRAM* program);  /* a program loaded in RAM */
void board_at_stop(void (*fn)(void));
void board_at_frame(void (*fn)(void));  /* before the game runs */
void board_stop(void);              /* the game and what it holds let go, RAM cleared */
const HOST_PROGRAM* board_program(void);
struct coro* board_game_coro(void);
int naomi_ram_has_write_watch(void);
int naomi_ram_get_dirty_pages(void** out_pages, unsigned long* inout_count);
void naomi_ram_reset_write_watch(void);
size_t naomi_board_state_size(void);
void naomi_board_state_save(void* out);
void naomi_board_state_load(const void* in);
uint64_t naomi_board_state_hash(void);
void naomi_run_frame(void);         /* until the game waits for the next vertical blank */
void host_poll(void);               /* from the game's busy waits: the interrupts happen here */
void host_stub(const char* name);   /* a library routine the port lacks: says so once */
void host_stub_report(void);
unsigned host_frame(void);
extern uint32_t host_fills;         /* counts the port's writes into the game's memory: ROM and disc reads */
void host_log(const char* fmt, ...);
void host_divzero_install(void);    /* a division by zero gives 0, as SHC's code did */
void host_divzero_allow_module(const void* hmod);
void host_divzero_disallow_module(const void* hmod);
struct tm;
void host_clock(struct tm* tm);
void host_clock_set_override(int on, long long unix_sec);
extern int host_unlock;             /* unlock = all */
int host_is_unlocked(void);
void host_set_netplay_canonical(int on);
int host_is_netplay_canonical(void);
void dc_vmu_set_netplay_lock(int locked);
int dc_vmu_is_netplay_locked(void);

/* the Dreamcast (port/dc), from the disc's files in a folder */
int dc_init(const HOST_PROGRAM* program, const char* disc_path);
int dc_gd_open(const char* disc_path);
void dc_gd_set_overlay_dir(const char* overlay_dir);
const char* dc_disc_check(const char* disc_path);  /* a file missing or the wrong size, or NULL */
long dc_gd_read_file(const char* name, void* buf, long max);  /* a whole file; its size, or -1 */
const char* dc_gd_path(const char* name);   /* a file's path on the host */

/* the 3DS's read-ahead of one file (ctr_stream.c) */
int ctr_stream_open(const char* path, uint32_t most);
void ctr_stream_drop(void);
void ctr_stream_close(void);
int ctr_stream_queue(uint32_t off, uint32_t len);
int ctr_stream_put(uint32_t off, uint32_t len, const void* bytes);
int ctr_stream_find(uint32_t off, uint32_t n);
const uint8_t* ctr_stream_take(int i, uint32_t* off);
void ctr_stream_free(int i);
uint32_t ctr_stream_times(float* mean, float* most);   /* the reads since the last call; mean and most in ms */
int ctr_rom_dma(uint32_t offset, void* dst, uint32_t size);   /* the game's DMA from the ROM, 1 when read (ctr_rom.c) */
int ctr_rom_packed(uint32_t offset, void* dst, uint32_t size); /* and its packed reads, 1 when given */
/* and its disc reader, one read at a time (MVC2_GD_ASYNC) */
int ctr_read_start(FILE* f, long off, void* to, size_t want);
int ctr_read_busy(void);
size_t ctr_read_end(float* ms);
int ctr_ahead_start(const char* path, size_t most);
int ctr_ahead_has(const char* path);
int ctr_ahead_idle(void);
FILE* ctr_ahead_file(const char* path, long* size);
int ctr_ahead_take(size_t off, void* to, size_t want, float* ms);
void ctr_ahead_drop(int all);
void ctr_close_later(FILE* f);
int ctr_small_start(const char* path, size_t most);
FILE* ctr_small_open(const char* path, long* size);
int ctr_small_take(const char* path, size_t off, void* to, size_t want);
void ctr_small_drop(void);

/* port/sys/restart.c */
void host_keep_start(void);         /* first thing, before any of them runs */
void host_keep_program(const HOST_PROGRAM* program);   /* before it first runs */
void host_forget_program(const HOST_SPAN* span);
void host_restart(void);            /* with the board stopped */
extern void (*host_restart_hook)(int after);   /* the front end's settings in the layers taken before it (0), given back after (1) */
void* host_keep_block(size_t size); /* memory kept through restarts (the sound's RAM), or NULL */

/* the front end (main_sdl.c, main_3ds.c) */
void host_present(void);
int pick_game(int argc, char** argv);   /* start.c: from the data found */
const char* pick_message(void);         /* why none can run ("" when the player quit) */
const char* config_get(const char* key);             /* mvc2.cfg's, or NULL */
const char* host_save_path(const char* name);
const char* host_config_dir(void);
const char* host_config_path(const char* name);
const char* host_exe_path(const char* name);
const char* cli_get_opt(const char* name);
int cli_has_flag(const char* name);
void config_set(const char* key, const char* value); /* and saves mvc2.cfg */
int start_game(void);
int switch_game(void);              /* to the other one, when its data was found */
const char* game_title(void);
void pace_set_rate_scale(double scale);
int host_is_headless(void);
/* memory the caller can do without: NULL when there is none. On the 3DS any other allocation that fails stops
   the program with host_out_of_memory. */
void* host_try_malloc(size_t bytes);
void* host_try_calloc(size_t n, size_t size);
void host_out_of_memory(const char* what, size_t bytes);   /* says so, and the program ends */

#endif
