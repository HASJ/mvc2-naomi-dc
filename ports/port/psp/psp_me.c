/*
 * psp_me.c  The Media Engine, the PSP's second CPU, taken from the firmware for our jobs. The firmware's ME code is
 * first asked to save its memory and halt, as before the PSP sleeps; ours is started at the reset vector and given
 * one job at a time; giving it back resumes the firmware's code as after waking, since its sleep waits on that code.
 */

#include "psp_me.h"

#include <string.h>

#include <kubridge.h>
#include <pspkernel.h>
#include <psppower.h>
#include <psputils.h>
#include <systemctrl.h>

#define REG(a) (*(volatile uint32_t*)(uintptr_t)(a))
#define RESETS 0xBC10004Cu      /* sysreg: bit 2 holds the ME in reset, bit 4 its VME */
#define BUS_CLOCKS 0xBC100050u  /* bit 0: the ME's bus clock */
#define ME_RAM 0xBFC00000u      /* the ME's own memory: its reset vector at 0x40, the firmware's words to 0x718 */
#define ME_VECTOR (ME_RAM + 0x40u)
#define ME_ARGS (ME_RAM + 0x740u)   /* ours: the routine, its argument, its stack, the bus bits */
#define ME_POST (ME_RAM + 0x750u)   /* the job's number, where the idle ME looks for it */
#define FW_STATE (ME_RAM + 0x700u)  /* the firmware's: -3 while its ME code is starting */
#define FW_BOOT_A (ME_RAM + 0x70Cu) /* and two words its first start sets */
#define FW_BOOT_B (ME_RAM + 0x714u)
#define FW_INIT 389u                /* the call it makes to its ME code once that has started */
#define FW_SAVE 399u                /* the call before sleep: the ME's memory saved, then it halts */
#define FW_TABLE (ME_RAM + 0x600u)  /* the call's number at [0], its arguments from [2] */
#define FW_AVC (ME_RAM + 0x718u)    /* the AVC's power bit, kept over a sleep */
#define AVC_POWER 0xBC100070u
#define FW_IMAGE_HI 0x64u           /* the firmware's start code: lui a0 / ori a0 with its image's address */
#define FW_IMAGE_LO 0x68u
#define SAVED 0x800u

#define KSEG0(p) ((uint32_t)(uintptr_t)(p) | 0x80000000u)            /* main RAM as the ME sees it, cached */
#define CPU_UNCACHED(p) ((uint32_t)(uintptr_t)(p) | 0x40000000u)     /* and as the main CPU sees it, uncached */

/* in a line of its own: the main CPU writes it through the uncached mirror, and a cached neighbour's line written back
   would put back its old words */
typedef struct __attribute__((aligned(64))) {
    uint32_t alive, seq, done, fn, arg, fcsr, poll_ram, dsize, fcsr_was, gp;
} ME_SHARED;

static ME_SHARED me_shared __attribute__((aligned(64)));
static uint8_t me_stack[32768] __attribute__((aligned(64)));
static uint32_t fw_saved[SAVED / 4];
static uint32_t seq;
static int ours, poll_ram = 1, asleep, given_back, refuse_sleep, refused;
static volatile int sleep_asked, woke, suspending;
static uint32_t given_at;   /* when the ME was given back for a sleep */
static int given_kind;      /* what was given back: 1 the ME, 2 the thread in its place */
static SceUID power_cb = -1;
static PSP_ME_FW fw;
static int halted;      /* the firmware's ME code saved and halted by us: give it back by resuming it */

#define shared ((volatile ME_SHARED*)(uintptr_t)CPU_UNCACHED(&me_shared))

/* the ME's first code, copied to its reset vector: its caches' tags cleared by the sizes in Config, the steps the
   firmware's own start takes, then our routine called with its stack and argument */
extern char me_stub[], me_stub_end[];
__asm__(".section .rodata\n"
        ".align 2\n"
        ".set push\n"
        ".set noreorder\n"
        ".set noat\n"
        "me_stub:\n"
        "    lui   $26, 0xBFC0\n"
        "    lw    $8, 0x74C($26)\n"
        "    lui   $27, 0xBC10\n"
        "    sw    $8, 64($27)\n"
        "    li    $8, 7\n"
        "    sw    $8, 80($27)\n"
        "    li    $8, -1\n"
        "    sw    $8, 4($27)\n"
        "    sync\n"
        "    mfc0  $26, $16\n"
        "    mtc0  $0, $28\n"
        "    mtc0  $0, $29\n"
        "    li    $9, 2048\n"
        "    ext   $8, $26, 9, 3\n"
        "    sllv  $27, $9, $8\n"
        "1:  addiu $27, $27, -64\n"
        "    bnez  $27, 1b\n"
        "    cache 0x01, 0($27)\n"
        "    ext   $8, $26, 6, 3\n"
        "    sllv  $27, $9, $8\n"
        "2:  addiu $27, $27, -64\n"
        "    bnez  $27, 2b\n"
        "    cache 0x11, 0($27)\n"
        "    mtc0  $0, $13\n"
        "    lui   $26, 0x2000\n"
        "    mtc0  $26, $12\n"
        "    lui   $26, 0xBCC0\n"
        "    li    $9, 1\n"
        "    sw    $9, 16($26)\n"
        "3:  lw    $8, 16($26)\n"
        "    andi  $8, $8, 1\n"
        "    beq   $8, $9, 3b\n"
        "    nop\n"
        "    sw    $9, 112($26)\n"
        "    li    $9, 8\n"
        "    sw    $9, 48($26)\n"
        "    li    $9, 2\n"
        "    sw    $9, 64($26)\n"
        "    lui   $26, 0xBFC0\n"
        "    lw    $29, 0x748($26)\n"
        "    lw    $4, 0x744($26)\n"
        "    lw    $25, 0x740($26)\n"
        "    jalr  $25\n"
        "    sync\n"
        "4:  b     4b\n"
        "    nop\n"
        "me_stub_end:\n"
        ".set pop\n"
        ".text\n");

/* on the ME: its data cache written back and emptied, line by line, as the kernel does it */
static void me_dcache_flush(uint32_t size) {
    uint32_t a;
    for (a = 0; a < size; a += 64) {
        __asm__ volatile("cache 0x14, 0(%0)\n cache 0x14, 0(%0)" : : "r"(a) : "memory");
    }
    __asm__ volatile("sync" ::: "memory");
}

/* on the ME: each job run with an empty data cache, which is written back before its done is set */
static __attribute__((noinline, used)) void me_loop(uint32_t at) {
    volatile ME_SHARED* m = (volatile ME_SHARED*)(uintptr_t)(at | 0x20000000u);
    uint32_t config, size, last = 0, fcsr = m->fcsr;
    __asm__ volatile("mfc0 %0, $16" : "=r"(config));
    __asm__ volatile("cfc1 %0, $31" : "=r"(m->fcsr_was));
    __asm__ volatile("ctc1 %0, $31" : : "r"(fcsr & 0x01000003u));   /* the main CPU's rounding, no traps */
    __asm__ volatile("move $gp, %0" : : "r"(m->gp));     /* for the library code built with small data */
    size = 2048u << (config >> 6 & 7);
    m->dsize = size;
    m->alive = 1;
    for (;;) {
        uint32_t s, k, in_ram = m->poll_ram;
        for (;;) {
            s = in_ram ? m->seq : REG(ME_POST);
            if (s != last) {
                break;
            }
            for (k = 0; k < 256; k++) {
                __asm__ volatile("nop");
            }
        }
        last = s;
        me_dcache_flush(size);
        ((void (*)(void*))(uintptr_t)m->fn)((void*)(uintptr_t)m->arg);
        me_dcache_flush(size);
        m->done = s;
    }
}

static uint32_t interrupts_off(void) {
    uint32_t s;
    __asm__ volatile("mfic %0, $0\n mtic $0, $0\n sync" : "=r"(s) : : "memory");
    return s;
}

static void interrupts_back(uint32_t s) {
    __asm__ volatile("mtic %0, $0\n sync" : : "r"(s) : "memory");
}

static void hold_reset(void) {
    uint32_t s = interrupts_off();
    REG(RESETS) |= 4u;
    interrupts_back(s);
}

static void fw_call_k(uint32_t f, uint32_t x) {
    if (f != 0) {
        ((void (*)(uint32_t))(uintptr_t)f)(x);
    }
}

static int not_leaf(uint32_t w) {
    uint32_t op = w >> 26;
    return op == 2u || op == 3u || op >= 0x28u || op == 0x10u || (op == 0u && ((w & 0x3Fu) == 9u || (w & 0x3Fu) == 0xCu));
}

/* in kernel mode: 1 if the code at f is a short routine that returns without calls, jumps or stores */
static int is_getter(uint32_t f) {
    uint32_t i;
    for (i = 0; i < 8; i++) {
        uint32_t w = REG(f + 4u * i);
        if (not_leaf(w)) {
            return 0;
        }
        if (w == 0x03E00008u) {
            return !not_leaf(REG(f + 4u * i + 4u));
        }
    }
    return 0;
}

/* in kernel mode: each getter's first words and, where it is one, what it returns; the image's first word */
static int fw_probe(void) {
    uint32_t i, k;
    for (i = 0; i < 4; i++) {
        uint32_t f = fw.getter_at[i];
        if (f == 0) {
            continue;
        }
        for (k = 0; k < 4; k++) {
            fw.getter_code[i][k] = REG(f + 4u * k);
        }
        if (is_getter(f)) {
            fw.getter_gave[i] = ((uint32_t (*)(void))(uintptr_t)f)();
            fw.getter_ok[i] = 1;
        }
    }
    fw.boot_hi = REG(ME_RAM + FW_IMAGE_HI);
    fw.boot_lo = REG(ME_RAM + FW_IMAGE_LO);
    fw.image_at = (fw.boot_hi << 16) | (fw.boot_lo & 0xFFFFu);
    if ((fw.image_at & 0xF8000000u) == 0x88000000u) {
        fw.image_word = REG(fw.image_at);
    }
    fw.avc = REG(AVC_POWER);
    return 0;
}

/* in kernel mode: the firmware's ME code asked to save its memory and halt, as the firmware's sleep does; 0 once it
   has, -1 if it is busy, -2 if it never says so */
static int fw_halt(void) {
    uint32_t i, n;
    if (REG(FW_STATE) != 0) {
        return -1;
    }
    REG(FW_STATE) = (uint32_t)-1;
    REG(FW_TABLE) = FW_SAVE;
    REG(FW_TABLE + 8) = fw.save_at;
    REG(FW_TABLE + 12) = fw.save_size;
    for (i = 4; i < 10; i++) {
        REG(FW_TABLE + 4 * i) = 0;
    }
    __asm__ volatile("sync" ::: "memory");
    fw_call_k(fw.ddr_flush, 5);
    fw_call_k(fw.to_other, 0);
    for (n = 0; REG(FW_STATE) != (uint32_t)-2; n++) {
        if (n > 20000000u) {
            return -2;
        }
    }
    fw_call_k(fw.ddr_flush, 8);
    REG(FW_AVC) = REG(AVC_POWER) & 4u;
    return 0;
}

/* in kernel mode: the firmware's words saved, the ME held in reset with its bus clock on, our start put at its
   vector, then let go in the firmware's order (VME, then the ME) */
static int me_take(void) {
    uint32_t i, n = (uint32_t)(me_stub_end - me_stub), s;
    hold_reset();
    for (i = 0; i < SAVED; i += 4) {
        fw_saved[i / 4] = REG(ME_RAM + i);
    }
    s = interrupts_off();
    REG(BUS_CLOCKS) |= 1u;
    interrupts_back(s);
    for (i = 0; i < n; i += 4) {
        uint32_t w;
        memcpy(&w, me_stub + i, 4);
        REG(ME_VECTOR + i) = w;
    }
    REG(ME_ARGS) = KSEG0(me_loop);
    REG(ME_ARGS + 4) = KSEG0(&me_shared);
    REG(ME_ARGS + 8) = KSEG0(me_stack + sizeof me_stack);
    REG(ME_ARGS + 12) = REG(0xBC100040u) & 3u;
    REG(ME_POST) = 0;
    __asm__ volatile("sync" ::: "memory");
    s = interrupts_off();
    REG(RESETS) &= ~0x10u;
    REG(RESETS) &= ~4u;
    interrupts_back(s);
    return 0;
}

static int me_reset(void) {
    hold_reset();
    return 0;
}

/* in kernel mode: the firmware's words put back and its halted ME code resumed from its saved memory, as the
   firmware does after waking; 0 once it says so, -1 if it doesn't */
static int me_resume(void) {
    uint32_t i, n, s, at = 0x80000000u | (fw.save_at & 0x1FFFFFFFu);
    hold_reset();
    for (i = 0; i < SAVED; i += 4) {
        REG(ME_RAM + i) = fw_saved[i / 4];
    }
    REG(ME_RAM + FW_IMAGE_HI) = (fw_saved[FW_IMAGE_HI / 4] & 0xFFFF0000u) | (at >> 16);
    REG(ME_RAM + FW_IMAGE_LO) = (fw_saved[FW_IMAGE_LO / 4] & 0xFFFF0000u) | (at & 0xFFFFu);
    s = interrupts_off();
    REG(BUS_CLOCKS) |= 1u;
    interrupts_back(s);
    __asm__ volatile("sync" ::: "memory");
    fw_call_k(fw.ddr_flush, 4);
    REG(FW_STATE) = (uint32_t)-3;
    __asm__ volatile("sync" ::: "memory");
    s = interrupts_off();
    REG(RESETS) &= ~0x10u;
    REG(RESETS) &= ~4u;
    interrupts_back(s);
    REG(FW_STATE) = 0;
    s = interrupts_off();
    REG(RESETS) &= ~0x20u;
    interrupts_back(s);
    fw_call_k(fw.ddr_flush, 8);
    for (n = 0; REG(FW_STATE) != (uint32_t)-4; n++) {
        if (n > 20000000u) {
            return -1;
        }
    }
    if (REG(FW_AVC) == 0) {
        s = interrupts_off();
        REG(AVC_POWER) &= ~4u;
        REG(RESETS) |= 0x20u;
        interrupts_back(s);
    }
    REG(FW_STATE) = 0;
    return 0;
}

/* in kernel mode: the firmware's words put back and its ME code started from them as sceMeBootStart's first start
   does; 0 once it answers, -1 if it doesn't */
static int me_give_back(void) {
    uint32_t i, n, s;
    hold_reset();
    for (i = 0; i < SAVED; i += 4) {
        REG(ME_RAM + i) = fw_saved[i / 4];
    }
    REG(FW_BOOT_A) = 0x01FF01FFu;
    REG(FW_BOOT_B) = 2u;
    REG(FW_STATE) = (uint32_t)-3;
    __asm__ volatile("sync" ::: "memory");
    s = interrupts_off();
    REG(RESETS) &= ~0x10u;
    REG(RESETS) &= ~4u;
    interrupts_back(s);
    for (n = 0; REG(FW_STATE) == (uint32_t)-3; n++) {
        if (n > 50000000u) {
            return -1;
        }
    }
    REG(FW_STATE) = 0;
    return 0;
}

static int me_post_word(uint32_t s) {
    REG(ME_POST) = s;
    __asm__ volatile("sync" ::: "memory");
    return 0;
}

static int kernel(int (*f)(void)) {
    struct KernelCallArg a;
    memset(&a, 0, sizeof a);
    if (kuKernelCall((void*)(uintptr_t)KSEG0(f), &a) < 0) {
        return -0x7FFFFFFF;
    }
    return (int)a.ret1;
}

static int kernel1(int (*f)(uint32_t), uint32_t x) {
    struct KernelCallArg a;
    memset(&a, 0, sizeof a);
    a.arg1 = x;
    if (kuKernelCall((void*)(uintptr_t)KSEG0(f), &a) < 0) {
        return -0x7FFFFFFF;
    }
    return (int)a.ret1;
}

/* the firmware's own call to its ME code, as sceMeBootStart ends with: proof that code answers, as the firmware's
   sleep needs it to. On a thread of its own, since an ME that never answers keeps it waiting */
static uint32_t fw_call;
static volatile int fw_answered;

static int fw_call_thread(SceSize n, void* p) {
    struct KernelCallArg a;
    (void)n;
    (void)p;
    memset(&a, 0, sizeof a);
    a.arg1 = FW_INIT;
    a.arg2 = 0;
    if (kuKernelCall((void*)(uintptr_t)fw_call, &a) >= 0) {
        fw_answered = 1;
    }
    return 0;
}

/* 1 if the firmware's ME code answered its call; -2 no such call found, -3 no thread for it, -4 no answer in 0.5 s
   (the thread is left waiting, holding the firmware's ME lock) */
static int fw_answers(void) {
    SceUInt timeout = 500000;
    SceUID th;
    if (fw_call == 0) {
        fw_call = (uint32_t)sctrlHENFindFunction("sceMeCodecWrapper", "sceMeCore_driver", 0xFA398D71u);
    }
    if (fw_call < 0x88000000u || fw_call >= 0x88800000u) {
        return -2;
    }
    fw_answered = 0;
    th = sceKernelCreateThread("me firmware call", fw_call_thread, 0x10, 0x1000, 0, NULL);
    if (th < 0 || sceKernelStartThread(th, 0, NULL) < 0) {
        return -3;
    }
    if (sceKernelWaitThreadEnd(th, &timeout) < 0) {
        return -4;
    }
    sceKernelDeleteThread(th);
    return fw_answered ? 1 : -3;
}

int psp_me_fw_call(void) {
    return fw_answers();
}

uint32_t psp_me_fw_call_at(void) {
    return fw_call;
}

/* in kernel mode: the firmware's words at 0xBFC00600 (its call table) and 0xBFC00700 */
static int fw_words(uint32_t out) {
    uint32_t* o = (uint32_t*)(uintptr_t)out;
    int i;
    for (i = 0; i < 12; i++) {
        o[i] = REG(ME_RAM + 0x600u + 4u * (uint32_t)i);
        o[12 + i] = REG(ME_RAM + 0x700u + 4u * (uint32_t)i);
    }
    return 0;
}

void psp_me_fw_words(uint32_t out[24]) {
    static uint32_t w[32] __attribute__((aligned(64)));
    struct KernelCallArg a;
    memset(&a, 0, sizeof a);
    a.arg1 = (uint32_t)(uintptr_t)w;
    sceKernelDcacheWritebackInvalidateRange(w, sizeof w);
    kuKernelCall((void*)(uintptr_t)KSEG0(fw_words), &a);
    memcpy(out, w, 24 * 4);
}

static int on_power(int count, int flags, void* arg) {
    (void)count;
    (void)arg;
    if (flags & (PSP_POWER_CB_SUSPENDING | PSP_POWER_CB_STANDBY)) {
        suspending = 1;
    }
    if (flags & (PSP_POWER_CB_RESUMING | PSP_POWER_CB_RESUME_COMPLETE)) {
        woke = flags & PSP_POWER_CB_RESUME_COMPLETE ? 1 : woke;     /* the switch's bit on waking asks for nothing */
    } else if (flags & (PSP_POWER_CB_POWER_SWITCH | PSP_POWER_CB_SUSPENDING | PSP_POWER_CB_STANDBY)) {
        sleep_asked = 1;
    }
    return 0;
}

static int power_thread(SceSize n, void* p) {
    (void)n;
    (void)p;
    power_cb = sceKernelCreateCallback("me power", on_power, NULL);
    scePowerRegisterCallback(-1, power_cb);
    sceKernelSleepThreadCB();
    return 0;
}

static uint32_t find(const char* mod, const char* lib, uint32_t nid) {
    uint32_t f = (uint32_t)sctrlHENFindFunction((char*)mod, (char*)lib, nid);
    return f >= 0x88000000u && f < 0x88800000u ? f : 0;
}

static int sane_save(uint32_t at, uint32_t size) {
    return (at & 0x3Fu) == 0 && (at & 0xF8000000u) == 0x88000000u && (at & 0x07FFFFFFu) >= 0x00400000u && size != 0 &&
           size <= 0x00400000u;
}

/* the firmware's routines its sleep uses on the ME, and where it saves the ME's memory: the getters by two names
   each (pspsdk's 6.60 and the one ARK translates), taken where one answers sanely or both agree */
static void fw_find(void) {
    static const uint32_t nids[4] = {0x287DC9FDu, 0xCDA3A2F7u, 0x5C55CAD3u, 0xB9F8561Cu};
    uint32_t i, at[2], size[2];
    memset(&fw, 0, sizeof fw);
    fw.found = 1;
    /* sysreg and the DDR's driver are libraries of the low I/O module */
    fw.to_other = find("sceLowIO_Driver", "sceSysreg_driver", 0x61BFF85Fu);
    if (fw.to_other == 0) {
        fw.to_other = find("sceLowIO_Driver", "sceSysreg_driver", 0x61FAE917u);
    }
    fw.ddr_flush = find("sceLowIO_Driver", "sceDdr_driver", 0x87D86769u);
    for (i = 0; i < 4; i++) {
        fw.getter_at[i] = find("sceSystemMemoryManager", "SysMemForKernel", nids[i]);
    }
    kernel(fw_probe);
    for (i = 0; i < 2; i++) {
        at[i] = fw.getter_ok[i] ? fw.getter_gave[i] : 0;
        size[i] = fw.getter_ok[2 + i] ? fw.getter_gave[2 + i] : 0;
        if (sane_save(at[i], size[i])) {
            fw.pair |= 1u << i;
        }
    }
    if (fw.pair == 3u && (at[0] != at[1] || size[0] != size[1])) {
        fw.pair |= 4u;      /* two sane answers that differ: neither taken */
    } else if (fw.pair & 1u) {
        fw.save_at = at[0];
        fw.save_size = size[0];
    } else if (fw.pair & 2u) {
        fw.save_at = at[1];
        fw.save_size = size[1];
    }
    fw.can_halt = fw.to_other != 0 && fw.save_at != 0 && (fw.boot_hi >> 16) == 0x3C04u && (fw.boot_lo >> 16) == 0x3484u;
}

const PSP_ME_FW* psp_me_fw(void) {
    return &fw;
}

int psp_me_start(void) {
    uint32_t t0, fcsr;
    if (ours) {
        return 1;
    }
    if (power_cb < 0) {
        SceUID th = sceKernelCreateThread("me power", power_thread, 0x10, 0x1000, THREAD_ATTR_USER, NULL);
        if (th >= 0) {
            sceKernelStartThread(th, 0, NULL);
        }
    }
    scePowerLock(0);
    __asm__ volatile("cfc1 %0, $31" : "=r"(fcsr));
    memset(&me_shared, 0, sizeof me_shared);
    me_shared.fcsr = fcsr;
    __asm__ volatile("move %0, $gp" : "=r"(me_shared.gp));
    me_shared.poll_ram = (uint32_t)poll_ram;
    seq = 0;
    if (!fw.found) {
        fw_find();
    }
    sceKernelDcacheWritebackInvalidateAll();
    halted = 0;
    if (fw.can_halt) {
        fw.halt = kernel(fw_halt);
        if (fw.halt == -2) {
            scePowerUnlock(0);  /* the firmware's ME code asked to halt and never said so: leave it be */
            return 0;
        }
        halted = fw.halt == 0;
    }
    if (kernel(me_take) < 0) {
        scePowerUnlock(0);
        return 0;
    }
    t0 = sceKernelGetSystemTimeLow();
    while (shared->alive != 1) {
        if (sceKernelGetSystemTimeLow() - t0 > 100000u) {
            kernel(me_reset);
            kernel(halted ? me_resume : me_give_back);
            scePowerUnlock(0);
            return 0;
        }
    }
    ours = 1;
    return 1;
}

/* MVC2_ME=thread: the jobs on a thread of the main CPU's below the game's, for PPSSPP, which doesn't run the ME */
static SceUID worker_go = -1;
static void (*worker_fn)(void*);
static void* worker_arg;

static uint32_t worker_fcsr;

static int worker(SceSize n, void* p) {
    (void)n;
    (void)p;
    __asm__ volatile("ctc1 %0, $31" : : "r"(worker_fcsr & 0x01000003u));     /* the main CPU's rounding and flush */
    for (;;) {
        sceKernelWaitSema(worker_go, 1, NULL);
        worker_fn(worker_arg);
        sceKernelDcacheWritebackAll();  /* as the ME's own flush: the main CPU may empty these lines from its cache */
        shared->done = seq;
    }
    return 0;
}

int psp_me_start_thread(void) {
    SceUID th;
    if (ours) {
        return 1;
    }
    if (worker_go < 0) {
        __asm__ volatile("cfc1 %0, $31" : "=r"(worker_fcsr));
        worker_go = sceKernelCreateSema("me worker", 0, 0, 1, NULL);
        th = sceKernelCreateThread("me worker", worker, sceKernelGetThreadCurrentPriority() + 1, 0x8000,
                                   THREAD_ATTR_USER, NULL);
        if (worker_go < 0 || th < 0 || sceKernelStartThread(th, 0, NULL) < 0) {
            return 0;
        }
    }
    memset(&me_shared, 0, sizeof me_shared);
    sceKernelDcacheWritebackInvalidateRange(&me_shared, sizeof me_shared);
    seq = 0;
    ours = 2;
    return 1;
}

int psp_me_post(void (*f)(void*), void* arg) {
    if (!ours) {
        return 0;
    }
    if (ours == 2) {
        worker_fn = f;
        worker_arg = arg;
        seq++;
        sceKernelSignalSema(worker_go, 1);
        return 1;
    }
    shared->fn = KSEG0(f);
    shared->arg = (uint32_t)(uintptr_t)arg;
    sceKernelDcacheWritebackAll();
    seq++;
    if (poll_ram) {
        shared->seq = seq;
    } else {
        kernel1(me_post_word, seq);
    }
    return 1;
}

int psp_me_wait(uint32_t us) {
    uint32_t t0 = sceKernelGetSystemTimeLow();
    if (!ours) {
        return 1;
    }
    while (shared->done != seq) {
        int k;
        if (sceKernelGetSystemTimeLow() - t0 > us) {
            return 0;
        }
        if (ours == 2) {
            sceKernelDelayThread(100);  /* the worker is below the caller: it runs only while the caller waits */
        }
        for (k = 0; k < 256; k++) {
            __asm__ volatile("nop");    /* the bus left to the ME and the GE between reads, as the ME's own wait does */
        }
    }
    return 1;
}

int psp_me_stop(int firmware_back) {
    int t = 0;
    if (!ours) {
        return 0;
    }
    if (ours == 2) {
        psp_me_wait(0xFFFFFFFFu);
        ours = 0;
        return 0;
    }
    ours = 0;
    kernel(me_reset);
    if (firmware_back) {
        uint32_t t0 = sceKernelGetSystemTimeLow();
        sceKernelDcacheWritebackInvalidateAll();
        fw.resume = kernel(halted ? me_resume : me_give_back);
        if (fw.resume < 0) {
            return -1;      /* the PSP kept from sleeping, which would wait on the firmware's ME for ever */
        }
        int r = fw_answers();
        if (r != 1) {
            return r;
        }
        t = (int)(sceKernelGetSystemTimeLow() - t0);
    }
    scePowerUnlock(0);
    return t;
}

int psp_me_frame(void) {
    if (ours && sleep_asked && refuse_sleep) {
        sleep_asked = 0;    /* the power switch stays held off; the ME stays ours */
        refused++;
        return 1;
    }
    if (ours && sleep_asked) {
        sleep_asked = 0;
        woke = 0;
        suspending = 0;
        given_at = sceKernelGetSystemTimeLow();
        given_kind = ours;
        psp_me_wait(200000);
        given_back = psp_me_stop(1);
        asleep = given_back >= 0;
        return 0;
    }
    /* taken again after waking, or when no sleep followed the switch in 3 s */
    if (asleep && (woke || (!suspending && sceKernelGetSystemTimeLow() - given_at > 3000000u))) {
        asleep = 0;
        woke = 0;
        suspending = 0;
        sleep_asked = 0;
        return given_kind == 2 ? psp_me_start_thread() : psp_me_start();
    }
    return ours;
}

int psp_me_ours(void) {
    return ours;
}

int psp_me_sleep_asked(void) {
    return ours && sleep_asked && !refuse_sleep;
}

/* the ME held in reset, whatever it is doing: our code off it when giving it back didn't end */
void psp_me_hold(void) {
    ours = 0;
    kernel(me_reset);
}

void psp_me_refuse_sleep(int on) {
    refuse_sleep = on;
}

int psp_me_sleeps_refused(void) {
    return refused;
}

int psp_me_given_back(void) {
    return given_back;
}

uint32_t psp_me_fcsr_was(void) {
    return shared->fcsr_was;
}

uint32_t psp_me_dcache(void) {
    return shared->dsize;
}

static void nothing(void* p) {
    (void)p;
}

void psp_me_poll_ram(int on) {
    if (ours) {
        shared->poll_ram = (uint32_t)on;
        psp_me_post(nothing, NULL);     /* the ME looks at it after this job */
        psp_me_wait(100000);
    }
    poll_ram = on;
}
