/* psp_front.h  The PSP front end's memory (psp_mem.c), screen (psp_video.c, psp_render.c), sound (psp_audio.c) and
   messages (main_psp.c) */

#ifndef PSP_FRONT_H
#define PSP_FRONT_H

#include <stdint.h>

typedef struct {
    char *base, *end;
} PSP_BLOCK;

typedef struct {
    uint32_t free_at_start, max_at_start;   /* the user partition before anything was taken */
    int ram_mapped;
    int ram_error;                          /* the kernel's, when it wasn't */
    PSP_BLOCK heap[2];                      /* in address order */
    int nheaps;
} PSP_MEM;

extern PSP_MEM psp_mem;
uint32_t psp_mem_unclaimed(void);          /* what the heap can still take from the kernel */
uint32_t psp_mem_in_use(void);
uint32_t psp_mem_free(void);               /* free in the heap and unclaimed */

/* rd_soft's 640x480 ABGR frame on the 480x272 screen, at 4:3 or stretched across it */
int psp_video_init(int bits16);                             /* the buffers at 16 bits a pixel or 32 */
int psp_video_bits16(void);
void psp_video_show(const uint32_t* frame, int stretch);
void psp_video_flip(void);                                  /* the drawn screen shown */
void psp_video_wait_back(void);                             /* until the last flip is on the screen */
void psp_video_wait_past(unsigned v);                       /* until the vblank count is past v */
void psp_video_latch(int on);                               /* flips shown from the next vblank (the kicker's) */
uint32_t psp_video_frame_us(void);                          /* a vblank to the next, 0 if the lines can't be read */
uint32_t psp_video_vblank_us(void);                         /* when this frame's vblank began */
uint32_t psp_video_shown_us(void);                          /* the vblank the last flip is shown from */
void psp_lat_shown(unsigned frame);                        /* frame's picture flipped, for the input-to-flip log */
void psp_video_target(int bits16);                          /* in a list: the draw buffer set again */
int psp_video_shot(const char* name);                       /* the drawn screen into a BMP */
void psp_video_text(const char* title, const char* text);   /* the debug screen's text in place of the game */
void psp_video_box(int x, int y, int w, int h, uint32_t abgr);  /* in both buffers, safe from an interrupt */
void psp_video_panel(int x, int y, int w, int h, const char* const* lines, int n, int chosen);   /* text, the GE idle */
void* psp_video_vram(uint32_t* size);                       /* the VRAM past the screen's buffers */

/* psp_render.c: the frame's lists drawn by the GE onto the screen; 0 when it can't */
int psp_render_frame(int stretch, unsigned frame);
int psp_render_window(char* out, int n);           /* the window's list times, in a line's words */
/* f(arg) run on the Media Engine after each frame's list there (its code built as the list's is, -G0, no syscalls, no
   VFPU), if ready(arg), called on the main CPU just before and only when it would be posted, says so (NULL: always);
   done(arg) called on the game's thread once it has ended, before the next list is given to the ME or at
   psp_render_me_after_wait. None while lists are made here. psp_render_me_takes: whether lists are made on the ME now */
void psp_render_me_after(void (*f)(void*), int (*ready)(void*), void (*done)(void*), void* arg);
void psp_render_me_after_wait(void);
int psp_render_me_takes(void);
void psp_render_me_before(void (*f)(int on_me));   /* f once each frame's list is given to the ME */
void psp_render_input(void);        /* the next frame's input read now */
uint32_t psp_render_start_wait(void);   /* the microseconds to wait before it is: MVC2_FRAME_START */
void psp_render_idle(void);         /* the kicker's frame drawn and shown: sceGu is the caller's (a shot) */
int psp_render_idle_within(uint32_t us);   /* the same, us at most: 0 if not */
const uint32_t* psp_art_pillars(void); /* the 3DS's pillars as a 64 by 512 texture (psp_art.c), or NULL */
int psp_art_wanted(void);            /* mvc2.cfg's pillars (on unless off) */
void psp_render_pillars(int on);     /* the pillars beside a 4:3 picture on or off */
void psp_render_end(void);          /* at the end: the kicker given 2 s, the hash log written */
void psp_render_start(void);        /* the game's models turned and lit by the GE, unless MVC2_GPU_TL=0 */

typedef struct {
    uint32_t frames, cpu_us, ge_us;         /* the CPU's time making the GE's list, the GE's drawing it */
    uint32_t wait_us;                       /* the CPU's waiting for the GE to finish the frame before */
    uint32_t sec_us[5];                     /* the GE's: background, opaque, punch-through, translucent, after */
    uint32_t cpu_sec[5];                    /* the CPU's: set-up and depth layout, the three lists, the finish */
    uint32_t tris, verts, batches, clipped, sprites;  /* sprites: quads drawn as the GE's */
    uint32_t skipped, dropped;              /* not drawable; past a full list */
    uint32_t offset_tris, mirrored;         /* drawn without the offset colour, or mirroring */
    uint32_t tex_hits, tex_made, tex_made_bytes, tex_evicted;  /* textures found, converted, let go */
    uint32_t tex_us;                        /* converting them */
    uint32_t atlas_made, atlas_copies, atlas_full;  /* sprite tiles converted, copied into a cell; not */
    uint32_t tex_kb, tex_vram_kb, tex_count;                    /* held at the last frame */
    uint32_t depth_values;                  /* flat 1/w values given their own depth */
    uint32_t groups, group_tris;            /* models the GE turned and lit */
    uint32_t mesh_kb;                       /* their meshes, at the last frame */
} PSP_RSTATS;

extern PSP_RSTATS psp_rstats;

void psp_save_bmp(const char* name, const uint32_t* px, int w, int h, int pitch);    /* ABGR pixels */

void psp_tell(const char* title, const char* text);         /* shown until START, except in a test run */
int psp_ask(const char* text);                              /* 0 arcade, 1 Dreamcast, -1 quit */
void psp_unpack_screen(const char* text);                  /* a long unpack's text, or NULL to end it */
void psp_unpack_progress(uint32_t done, uint32_t total);    /* its bar and time, in KB */

/* the sound (psp_audio.c) */
void psp_audio_init(void);      /* after start_game: sceAudio, and the threads that make and play the sound */
void psp_audio_due(void);       /* at each host_present's start: the frame's sound to be made */
void psp_audio_frame(void);     /* before host_present's pace: the frame's sound made, if the list's post hasn't */
void psp_audio_idle(void);      /* before switch_game: waits until the sound's state is the game's alone */
void psp_audio_exit(void);

/* the log, crumbs and watchdog (psp_log.c) */
void psp_log_open(const char* name);
void psp_log_sync(int on);
void psp_log_now(const char* text);                         /* written and closed at once, the held lines first */
void psp_log_flush(void);                                   /* the held lines written now, the log still held */
void psp_log_hold(int on);                                   /* lines kept in memory until it is turned off or the log closed */
void psp_log_report(void);                                  /* a crash's or a hang's lines held, with room kept for them */
void psp_log_rescuer_start(void);
void psp_log_rescue(void (*then)(int r));                   /* the held log written by the rescuer; then ends the program */
int psp_intr_back(void);                                    /* interrupts on again in a failed thread; 1 when they were off */
extern int psp_paced;           /* one frame a vblank at most (main_psp.c): not in a test run unless MVC2_PACE=1 */
void psp_log_close(void);
int psp_log_write(const char* s, int n);
void psp_screen_take(void);
void psp_crumb(const char* what, unsigned n);
void psp_crumb_frame(unsigned frame);
void psp_crumbs_dump(void (*line)(const char* text), unsigned most);   /* the last most, oldest first */
void psp_crumbs_start(int test_run);

/* the coroutines, threads taking turns (port/sys/coro_psp.c) */
extern unsigned coro_psp_switches;
extern volatile int psp_reader_doing;  /* psp_reader.c: what the reader is doing, for MVC2_BEAT */
float coro_psp_bench(unsigned n);                           /* a switch's time in microseconds */

/* a crash's report (psp_crash.c) */
void psp_crash_install(void);
void psp_crash_test(unsigned frame);
void psp_exit(void);                                        /* the log closed, back to the menu */
void psp_exit_now(void);        /* from HOME's quit, when the game's thread hasn't ended the program in time */
extern volatile uint32_t psp_exit_step_us;  /* when psp_exit last moved on (0 before it began) */
void psp_report_written(int r);                             /* a crash's or a hang's end: says if its report was written */
int psp_kernel_call(uint32_t f, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t* ret);   /* kubridge's */

/* the CPU's own counters and a sample of where it is (psp_prof.c) */
void psp_prof_start(void);
void psp_prof_frame(unsigned frame);
void psp_prof_window(unsigned frames);                      /* the counters a frame since the last window, logged */
void psp_prof_stop(void);                                   /* and the samples saved */

#endif
