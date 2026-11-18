/* psp_me.h  The Media Engine, the PSP's second CPU, as a worker for one job at a time. */
#ifndef PSP_ME_H
#define PSP_ME_H

#include <stdint.h>

/* the ME taken from the firmware and started; 1 if it answers. Its code must be built -G0, use no syscalls and no
   VFPU, and write only memory the main CPU leaves alone until the job is done. */
int psp_me_start(void);

/* the same worker as a thread of the main CPU's, below the caller's priority (for PPSSPP, which doesn't run the ME) */
int psp_me_start_thread(void);

/* f(arg) run on the ME; 0 if the ME isn't ours. The main CPU's data cache is written back first. */
int psp_me_post(void (*f)(void*), void* arg);

/* up to us microseconds for the job posted last; 1 when it is done (and always when the ME isn't ours) */
int psp_me_wait(uint32_t us);

/* the ME stopped; with firmware_back, the firmware's own ME code started again and called once: how long that took
   in microseconds; -1 if its code didn't start, and as psp_me_fw_call if it didn't answer. Only after an answer is
   the power switch let go: the firmware's sleep waits on that code */
int psp_me_stop(int firmware_back);

/* at a frame's end: a sleep asked for gives the ME back to the firmware and, once its code answers, lets the PSP
   sleep (otherwise the switch stays held off); after waking the ME is taken again. 1 if the ME is ours after it. */
int psp_me_frame(void);

int psp_me_ours(void);
int psp_me_sleep_asked(void);   /* psp_me_frame will give the ME back for a sleep */
void psp_me_hold(void);         /* the ME held in reset, our code off it (an exit whose give-back didn't end) */

/* the firmware's own call to its ME code (sceMeBootStart's last): 1 answered; -2 the call not found, -3 no thread,
   -4 no answer in 0.5 s. Where it was found, and the firmware's words at 0xBFC00600 and 0xBFC00700 (tests) */
int psp_me_fw_call(void);
uint32_t psp_me_fw_call_at(void);
void psp_me_fw_words(uint32_t out[24]);

/* what was found of the firmware's ME sleep: its routines, where it saves the ME's memory (two names each for the
   address and size getters: their first words and answers), its start code's image address and first word, and
   how the last halt (0, -1 busy, -2 no answer) and resume (0, -1) went */
typedef struct {
    uint32_t found, to_other, ddr_flush, getter_at[4], getter_code[4][4], getter_gave[4], getter_ok[4];
    uint32_t save_at, save_size, pair, can_halt, boot_hi, boot_lo, image_at, image_word, avc;
    int halt, resume;
} PSP_ME_FW;
const PSP_ME_FW* psp_me_fw(void);

/* with on, psp_me_frame keeps the ME and the power switch held off when a sleep is asked for: the PSP doesn't sleep
   while the game runs. How many it has refused */
void psp_me_refuse_sleep(int on);
int psp_me_sleeps_refused(void);

/* what the last sleep's psp_me_stop(1) gave */
int psp_me_given_back(void);

/* the ME's floating-point control word before ours was put in, and its data cache's size */
uint32_t psp_me_fcsr_was(void);
uint32_t psp_me_dcache(void);

/* how the idle ME waits for a job: 1 (the default) a word in main RAM, looked at every 256 cycles or so; 0 its own
   memory at 0xBFC00000, each post a kernel call */
void psp_me_poll_ram(int on);

#endif
