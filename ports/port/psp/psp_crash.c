/*
 * psp_crash.c  A crash's report in the log. The kernel's default exception handler, set through the custom
 * firmware's kubridge, keeps the registers and sends the thread on to psp_crash_report, which writes them, the
 * stack and the breadcrumbs, then ends the program. Bus errors (a null pointer's) have a handler of the kernel's
 * own, which ends the thread, so ours goes ahead of it for those. MVC2_CRASH=kind crashes on purpose, to test it.
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <kubridge.h>
#include <pspctrl.h>
#include <pspdebug.h>
#include <pspge.h>
#include <pspkernel.h>
#include <pspthreadman.h>
#include <systemctrl.h>

#include "naomi_host.h"
#include "psp_front.h"
#include "psp_me.h"

/* the layout the handler below stores */
typedef struct {
    uint32_t r[32];
    uint32_t hi, lo, status, cause, epc, badvaddr, fcr31;
    uint32_t fpr[32];
} CRASH_REGS;

CRASH_REGS psp_crash_regs;
uint32_t psp_crash_probe_pc = 1;                /* the test fault's address while it runs, else 1 */
uint32_t psp_crash_sp;                          /* the report's own stack, for a crash on none of the program's */
uint32_t psp_crash_hits;                        /* the times the handler has sent a thread to the report */
static uint32_t crash_stack[4096] __attribute__((aligned(16)));

extern char _ftext[], _etext[];

void psp_crash_report(CRASH_REGS* r);
void psp_crash_vector(void);
void psp_crash_vector_ibe(void);
void psp_crash_vector_dbe(void);

/* kernel mode, interrupts off: the vector has put v0 and v1 in COP0 control registers 4 and 5. The registers are
   kept, the FPU's exceptions turned off, and the eret goes to psp_crash_report, in the thread's own mode, 1 KB down
   the thread's own stack (the kernel ends a thread whose sp leaves its stack), or on crash_stack when the stack
   isn't the program's memory */
__asm__(".text\n"
        ".align 2\n"
        ".globl psp_crash_vector\n"
        ".type psp_crash_vector, @function\n"
        ".ent psp_crash_vector\n"
        "psp_crash_vector:\n"
        ".set push\n"
        ".set noreorder\n"
        ".set noat\n"
        "nop\n"
        "nop\n"
        "lui $2, %hi(psp_crash_probe_pc)\n"     /* the start-up's test fault: passed over, the thread goes on */
        "lw $2, %lo(psp_crash_probe_pc)($2)\n"
        "mfc0 $3, $14\n"
        "bne $2, $3, 2f\n"
        "nop\n"
        "addiu $3, $3, 4\n"
        "mtc0 $3, $14\n"
        "lui $2, %hi(psp_crash_probe_pc)\n"
        "mfc0 $3, $12\n"                        /* the thread's Status, odd so as never to be an address */
        "ori $3, $3, 1\n"
        "sw $3, %lo(psp_crash_probe_pc)($2)\n"
        "cfc0 $2, $4\n"
        "cfc0 $3, $5\n"
        "sync\n"
        "nop\n"
        "nop\n"
        "eret\n"
        "nop\n"
        "2:\n"
        "lui $2, %hi(psp_crash_regs)\n"
        "addiu $2, $2, %lo(psp_crash_regs)\n"
        "sw $0, 0($2)\n"
        "sw $1, 4($2)\n"
        "cfc0 $1, $4\n"
        "sw $1, 8($2)\n"
        "cfc0 $1, $5\n"
        "sw $1, 12($2)\n"
        "sw $4, 16($2)\n"
        "sw $5, 20($2)\n"
        "sw $6, 24($2)\n"
        "sw $7, 28($2)\n"
        "sw $8, 32($2)\n"
        "sw $9, 36($2)\n"
        "sw $10, 40($2)\n"
        "sw $11, 44($2)\n"
        "sw $12, 48($2)\n"
        "sw $13, 52($2)\n"
        "sw $14, 56($2)\n"
        "sw $15, 60($2)\n"
        "sw $16, 64($2)\n"
        "sw $17, 68($2)\n"
        "sw $18, 72($2)\n"
        "sw $19, 76($2)\n"
        "sw $20, 80($2)\n"
        "sw $21, 84($2)\n"
        "sw $22, 88($2)\n"
        "sw $23, 92($2)\n"
        "sw $24, 96($2)\n"
        "sw $25, 100($2)\n"
        "sw $26, 104($2)\n"
        "sw $27, 108($2)\n"
        "sw $28, 112($2)\n"
        "sw $29, 116($2)\n"
        "sw $30, 120($2)\n"
        "sw $31, 124($2)\n"
        "mfhi $1\n"
        "sw $1, 128($2)\n"
        "mflo $1\n"
        "sw $1, 132($2)\n"
        "mfc0 $1, $12\n"
        "sw $1, 136($2)\n"
        "mfc0 $3, $13\n"
        "sw $3, 140($2)\n"
        "mfc0 $3, $14\n"
        "sw $3, 144($2)\n"
        "mfc0 $3, $8\n"
        "sw $3, 148($2)\n"
        "lui $3, 0x2000\n"              /* the FPU's registers only when the thread had it on */
        "and $3, $3, $1\n"
        "beqz $3, 1f\n"
        "nop\n"
        "cfc1 $3, $31\n"
        "sw $3, 152($2)\n"
        "ctc1 $0, $31\n"
        "swc1 $f0, 156($2)\n"
        "swc1 $f1, 160($2)\n"
        "swc1 $f2, 164($2)\n"
        "swc1 $f3, 168($2)\n"
        "swc1 $f4, 172($2)\n"
        "swc1 $f5, 176($2)\n"
        "swc1 $f6, 180($2)\n"
        "swc1 $f7, 184($2)\n"
        "swc1 $f8, 188($2)\n"
        "swc1 $f9, 192($2)\n"
        "swc1 $f10, 196($2)\n"
        "swc1 $f11, 200($2)\n"
        "swc1 $f12, 204($2)\n"
        "swc1 $f13, 208($2)\n"
        "swc1 $f14, 212($2)\n"
        "swc1 $f15, 216($2)\n"
        "swc1 $f16, 220($2)\n"
        "swc1 $f17, 224($2)\n"
        "swc1 $f18, 228($2)\n"
        "swc1 $f19, 232($2)\n"
        "swc1 $f20, 236($2)\n"
        "swc1 $f21, 240($2)\n"
        "swc1 $f22, 244($2)\n"
        "swc1 $f23, 248($2)\n"
        "swc1 $f24, 252($2)\n"
        "swc1 $f25, 256($2)\n"
        "swc1 $f26, 260($2)\n"
        "swc1 $f27, 264($2)\n"
        "swc1 $f28, 268($2)\n"
        "swc1 $f29, 272($2)\n"
        "swc1 $f30, 276($2)\n"
        "swc1 $f31, 280($2)\n"
        "1:\n"
        "lui $3, 0x4400\n"            /* a white band at the top of each colour buffer, 16 or 32-bit: the vector ran */
        "addiu $4, $3, 4096\n"
        "li $1, -1\n"
        "lui $5, 4\n"
        "ori $5, $5, 0x4000\n"
        "3:\n"
        "sw $1, 0($3)\n"
        "addu $6, $3, $5\n"
        "sw $1, 0($6)\n"
        "addu $6, $6, $5\n"
        "sw $1, 0($6)\n"
        "addiu $3, $3, 4\n"
        "bne $3, $4, 3b\n"
        "nop\n"
        "lui $3, %hi(psp_crash_hits)\n"
        "lw $1, %lo(psp_crash_hits)($3)\n"
        "addiu $1, $1, 1\n"
        "sw $1, %lo(psp_crash_hits)($3)\n"
        "lui $3, %hi(psp_crash_report)\n"
        "addiu $3, $3, %lo(psp_crash_report)\n"
        "mtc0 $3, $14\n"
        "lui $3, 0x0880\n"                      /* sp in 0x08800000-0x0BFFFFFF: kept */
        "sltu $1, $29, $3\n"
        "bnez $1, 4f\n"
        "lui $3, 0x0C00\n"
        "sltu $1, $29, $3\n"
        "beqz $1, 4f\n"
        "li $1, -16\n"
        "addiu $29, $29, -1024\n"
        "b 5f\n"
        "and $29, $29, $1\n"
        "4:\n"
        "lui $3, %hi(psp_crash_sp)\n"
        "lw $29, %lo(psp_crash_sp)($3)\n"
        "5:\n"
        "move $4, $2\n"
        "sync\n"
        "nop\n"
        "nop\n"
        "eret\n"
        "nop\n"
        ".set pop\n"
        ".end psp_crash_vector\n"
        ".size psp_crash_vector, .-psp_crash_vector\n");

/* the same for the bus errors' chains: each handler's first two words are the kernel's to link it by */
__asm__(".text\n"
        ".align 2\n"
        ".set push\n"
        ".set noreorder\n"
        ".globl psp_crash_vector_ibe\n"
        "psp_crash_vector_ibe:\n"
        "nop\n"
        "nop\n"
        "j psp_crash_vector + 8\n"
        "nop\n"
        ".globl psp_crash_vector_dbe\n"
        "psp_crash_vector_dbe:\n"
        "nop\n"
        "nop\n"
        "j psp_crash_vector + 8\n"
        "nop\n"
        ".set pop\n");

/* ---- the report ---- */

static void out(const char* text) {
    char line[200];
    int n = snprintf(line, sizeof line - 1, "%s", text);
    n = n < 0 ? 0 : n > (int)sizeof line - 2 ? (int)sizeof line - 2 : n;
    line[n++] = '\n';
    psp_log_write(line, n);
}

/* the report's first lines, on the screen before the log is tried: a crash inside the kernel's file system would
   stop the log */
static void screen(const char* text) {
    pspDebugScreenPrintf("%.67s\n", text);
}

static const char* cause_name(unsigned code) {
    static const char* const names[32] = {
        "interrupt", "TLB modified", "TLB load", "TLB store",
        "address error on a load or fetch (unaligned, or a kernel address)", "address error on a store (unaligned, or a kernel address)",
        "bus error on a fetch", "bus error on a load or store", "syscall", "break", "reserved instruction",
        "coprocessor unusable", "overflow", "trap", NULL, "floating point",
    };
    return code < 32 && names[code] != NULL ? names[code] : "?";
}

static int in_text(uint32_t a) {
    return (a & 3) == 0 && a >= (uint32_t)(uintptr_t)_ftext && a < (uint32_t)(uintptr_t)_etext;
}

static int in_ram(uint32_t a) {
    return (a & 3) == 0 && a >= 0x08800000u && a < 0x0C000000u;
}

/* a call's return address: the jal or jalr two words before it */
static int after_call(uint32_t ra) {
    uint32_t insn;
    if (!in_text(ra) || ra < (uint32_t)(uintptr_t)_ftext + 8) {
        return 0;
    }
    insn = *(const uint32_t*)(uintptr_t)(ra - 8);
    return insn >> 26 == 3 || (insn >> 26 == 0 && (insn & 0x3F) == 9);
}

static volatile int crashed;
static uint32_t crash_us;        /* when the report began */

/* white band k under the vector's, in each colour buffer and with no syscall: how far a report got before its screen */
static void band(unsigned k) {
    unsigned i, b;
    for (b = 0; b < 3; b++) {
        volatile uint32_t* p = (volatile uint32_t*)(uintptr_t)(0x44000000u + b * 0x44000u + k * 8192u);
        for (i = 0; i < 1024; i++) {
            p[i] = 0xFFFFFFFFu;
        }
    }
}

/* the thread's stack, or the kernel's when the crash was inside a call to it */
static int in_stack(uint32_t a) {
    return in_ram(a) || ((a & 3) == 0 && a >= 0x88000000u && a < 0x8C000000u);
}

/* the program's return addresses on the stack: inside the kernel, any address in the program's code (the call's
   saved registers are among them) */
static void return_addresses(uint32_t sp, int kernel, void (*line)(const char* text), unsigned most) {
    char text[64];
    uint32_t a;
    unsigned n;
    for (a = sp, n = 0; n < most && a < sp + (64u << 10) && in_stack(a); a += 4) {
        uint32_t w = *(const uint32_t*)(uintptr_t)a;
        if (kernel ? in_text(w) : after_call(w)) {
            snprintf(text, sizeof text, "  +%05lX: %08lX", (unsigned long)(a - sp), (unsigned long)w);
            line(text);
            n++;
        }
    }
}

void psp_crash_report(CRASH_REGS* r) {
    static const char* const reg[32] = { "zr", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",
                                         "t3", "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5",
                                         "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra" };
    char text[200], head[3][120];
    unsigned code = r->cause >> 2 & 31, i;
    uint32_t sp = r->r[29], a;
    int kernel = (r->status & 0x18u) != 0x10u;      /* KSU: not the user's mode */
    SceKernelThreadInfo th;
    int intr_off;
    band(1);
    if (crashed++) {
        sceKernelExitGame();
    }
    intr_off = psp_intr_back();
    crash_us = sceKernelGetSystemTimeLow();
    band(2);
    psp_log_report();
    memset(&th, 0, sizeof th);
    th.size = sizeof th;
    /* no thread to be had: an interrupt's handler, such as the GE's callbacks or an alarm */
    if (sceKernelReferThreadStatus(sceKernelGetThreadId(), &th) < 0) {
        snprintf(th.name, sizeof th.name, "(none: in an interrupt)");
    }
    band(3);
    snprintf(head[0], sizeof head[0], "crash: %s (cause %08lX) in thread %s at frame %u%s%s", cause_name(code),
             (unsigned long)r->cause, th.name, host_frame(), kernel ? ", inside the kernel" : "",
             intr_off ? ", interrupts off" : "");
    snprintf(head[1], sizeof head[1], "  EPC %08lX  BadVAddr %08lX  Status %08lX%s", (unsigned long)r->epc,
             (unsigned long)r->badvaddr, (unsigned long)r->status,
             r->cause & 0x80000000u ? "  (in a delay slot)" : "");
    snprintf(head[2], sizeof head[2], "  ra %08lX  sp %08lX  a0 %08lX  a1 %08lX  a2 %08lX", (unsigned long)r->r[31],
             (unsigned long)sp, (unsigned long)r->r[4], (unsigned long)r->r[5], (unsigned long)r->r[6]);
    psp_screen_take();
    for (i = 0; i < 3; i++) {
        screen(head[i]);
    }
    screen("  return addresses:");
    return_addresses(sp, kernel, screen, 6);
    screen("  the last crumbs:");
    psp_crumbs_dump(screen, 6);
    snprintf(text, sizeof text, "writing mvc2.log... (%lu ms after the crash)",
             (unsigned long)(sceKernelGetSystemTimeLow() - crash_us) / 1000);
    screen(text);

    out(head[0]);
    snprintf(text, sizeof text, "%s  FCR31 %08lX", head[1], (unsigned long)r->fcr31);
    out(text);
    for (i = 0; i < 32; i += 4) {
        snprintf(text, sizeof text, "  %s %08lX  %s %08lX  %s %08lX  %s %08lX", reg[i], (unsigned long)r->r[i],
                 reg[i + 1], (unsigned long)r->r[i + 1], reg[i + 2], (unsigned long)r->r[i + 2], reg[i + 3],
                 (unsigned long)r->r[i + 3]);
        out(text);
    }
    snprintf(text, sizeof text, "  hi %08lX  lo %08lX", (unsigned long)r->hi, (unsigned long)r->lo);
    out(text);
    if (r->status & 0x20000000u) {
        for (i = 0; i < 32; i += 8) {
            snprintf(text, sizeof text, "  f%-2u %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX", i,
                     (unsigned long)r->fpr[i], (unsigned long)r->fpr[i + 1], (unsigned long)r->fpr[i + 2],
                     (unsigned long)r->fpr[i + 3], (unsigned long)r->fpr[i + 4], (unsigned long)r->fpr[i + 5],
                     (unsigned long)r->fpr[i + 6], (unsigned long)r->fpr[i + 7]);
            out(text);
        }
    }
    if (in_text(r->epc) && r->epc >= (uint32_t)(uintptr_t)_ftext + 16) {
        const uint32_t* p = (const uint32_t*)(uintptr_t)(r->epc - 16);
        snprintf(text, sizeof text, "  code from EPC-16: %08lX %08lX %08lX %08lX [%08lX] %08lX %08lX %08lX",
                 (unsigned long)p[0], (unsigned long)p[1], (unsigned long)p[2], (unsigned long)p[3],
                 (unsigned long)p[4], (unsigned long)p[5], (unsigned long)p[6], (unsigned long)p[7]);
        out(text);
    }
    if (in_stack(sp)) {
        out("  stack from sp:");
        for (a = sp, i = 0; i < 16 && in_stack(a + 28); i++, a += 32) {
            const uint32_t* p = (const uint32_t*)(uintptr_t)a;
            snprintf(text, sizeof text, "  %08lX: %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX", (unsigned long)a,
                     (unsigned long)p[0], (unsigned long)p[1], (unsigned long)p[2], (unsigned long)p[3],
                     (unsigned long)p[4], (unsigned long)p[5], (unsigned long)p[6], (unsigned long)p[7]);
            out(text);
        }
        out("  return addresses on the stack (sp+offset: address):");
        return_addresses(sp, kernel, out, 40);
    }
    snprintf(text, sizeof text, "  thread %s: stack %08lX-%08lX, priority %d", th.name,
             (unsigned long)(uintptr_t)th.stack, (unsigned long)(uintptr_t)th.stack + th.stackSize, th.currentPriority);
    out(text);
    out("  the last crumbs:");
    psp_crumbs_dump(out, 32);
    out("crash: the program ends, the memory card not saved (psp-addr2line -f -e mvc2dc EPC ra ... names them)");
    psp_log_rescue(psp_report_written);
}

/* ---- setting it up ---- */

extern char psp_crash_probe_insn[];

/* an unaligned load, which the PSP faults and an emulator may not */
static __attribute__((noinline)) uint32_t probe_load(const uint32_t* p) {
    uint32_t v = 0;
    __asm__ volatile(".set push\n"
                     ".set noreorder\n"
                     ".globl psp_crash_probe_insn\n"
                     "psp_crash_probe_insn:\n"
                     "lw %0, 1(%1)\n"
                     "nop\n"
                     ".set pop\n"
                     : "+r"(v)
                     : "r"(p)
                     : "memory");
    return v;
}

/* the handler tried on a fault it passes over */
static void probe(void) {
    static uint32_t words[2] = { 0x11223344u, 0x55667788u };
    uint32_t v, insn = (uint32_t)(uintptr_t)psp_crash_probe_insn;
    host_log("crash handler: a test fault");
    psp_crash_probe_pc = insn;
    v = probe_load(words);
    if (psp_crash_probe_pc != insn) {
        host_log("crash handler: it caught the test fault (%08lX); the thread's Status %08lX, %s mode", (unsigned long)v,
                 (unsigned long)psp_crash_probe_pc, (psp_crash_probe_pc & 0x18u) == 0x10u ? "user" : "kernel");
    } else {
        host_log("crash handler: no fault (an emulator allows unaligned loads)");
    }
    psp_crash_probe_pc = 1;
}

#define NID_REGISTER_DEFAULT 0x565C0B0Eu
#define NID_GET_DEFAULT 0xDF83875Eu
#define NID_RELEASE_DEFAULT 0x291FF031u
#define NID_REGISTER_PRIORITY 0x5A837AD4u
#define NID_RELEASE 0x1AA6CFFAu
#define EXC_IBE 6
#define EXC_DBE 7

/* the kernel's call, by its old NID: the custom firmware's search translates it to this firmware's */
static uint32_t find_exception_call(uint32_t nid) {
    uint32_t f = sctrlHENFindFunction("sceExceptionManager", "ExceptionManagerForKernel", nid);
    return f >= 0x88000000u && f < 0x8C000000u ? f : 0;     /* else an error, as from a missing library */
}

int psp_kernel_call(uint32_t f, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t* ret) {
    struct KernelCallArg a;
    int r;
    memset(&a, 0, sizeof a);
    a.arg1 = arg1;
    a.arg2 = arg2;
    a.arg3 = arg3;
    r = kuKernelCall((void*)(uintptr_t)f, &a);
    if (ret != NULL) {
        *ret = a.ret1;
    }
    return r;
}

static int kernel_call(uint32_t f, uint32_t arg, uint32_t* ret) {
    return psp_kernel_call(f, arg, 0, 0, ret);
}

static int kernel_call_2(uint32_t f, uint32_t arg1, uint32_t arg2, uint32_t* ret) {
    return psp_kernel_call(f, arg1, arg2, 0, ret);
}

/* the handlers in place before ours, put back as the program ends */
static uint32_t release_call, register_call, ours_held, old_released;
static uint32_t release_bus_call;   /* ours for the bus errors, released as the program ends */

static void bus_handlers(void) {
    static const struct {
        int code;
        void (*f)(void);
    } bus[2] = { { EXC_IBE, psp_crash_vector_ibe }, { EXC_DBE, psp_crash_vector_dbe } };
    uint32_t reg = find_exception_call(NID_REGISTER_PRIORITY), ret = 0;
    int i, r, held = 0;
    for (i = 0; reg != 0 && i < 2; i++) {
        r = psp_kernel_call(reg, (uint32_t)bus[i].code, 0, (uint32_t)(uintptr_t)bus[i].f | 0x80000000u, &ret);
        host_log("crash handler: ours for exception %d first: %d %08lX", bus[i].code, r, (unsigned long)ret);
        held += r >= 0 && (int)ret >= 0;
    }
    if (reg == 0) {
        host_log("crash handler: none for bus errors (sceKernelRegisterPriorityExceptionHandler not found)");
    }
    release_bus_call = held == 2 ? find_exception_call(NID_RELEASE) : 0;
}

static void release_handlers(void) {
    uint32_t ret;
    if (release_bus_call != 0) {
        kernel_call_2(release_bus_call, EXC_IBE, (uint32_t)(uintptr_t)psp_crash_vector_ibe | 0x80000000u, &ret);
        kernel_call_2(release_bus_call, EXC_DBE, (uint32_t)(uintptr_t)psp_crash_vector_dbe | 0x80000000u, &ret);
        release_bus_call = 0;
    }
    if (ours_held != 0 && release_call != 0) {
        kernel_call(release_call, ours_held, &ret);
        if (old_released != 0) {
            kernel_call(register_call, old_released, &ret);
        }
        ours_held = 0;
    }
}

/* the ME given back to the firmware before the program ends or runs another: the firmware's exit waits on its own
   ME code, and with ours still there the PSP hangs */
static volatile int me_back_gave;

static int me_back_thread(SceSize n, void* p) {
    (void)n;
    (void)p;
    psp_me_wait(200000);
    me_back_gave = psp_me_stop(1);
    return 0;
}

/* on a thread of its own, waited for 1 s at most: a wedged ME doesn't keep the program from ending */
static void me_back(void (*line)(const char* text)) {
    char text[64];
    SceUInt wait = 1000000;
    SceUID th;
    if (!psp_me_ours()) {
        return;
    }
    line("giving the ME back to the firmware...");
    me_back_gave = -99;
    th = sceKernelCreateThread("me back", me_back_thread, 0x10, 0x4000, THREAD_ATTR_USER, NULL);
    if (th < 0 || sceKernelStartThread(th, 0, NULL) < 0) {
        line("the ME not given back: no thread for it");
        return;
    }
    if (sceKernelWaitThreadEnd(th, &wait) < 0) {
        psp_me_hold();
        line("the ME not given back in 1 s: held in reset, our code off it");
        return;
    }
    sceKernelDeleteThread(th);
    snprintf(text, sizeof text, "the ME given back (%d)", me_back_gave);
    line(text);
}

/* the C library's own ways out: exit() and abort() */
static void at_exit(void) {
    psp_prof_stop();
    host_log("exit() at frame %u; the last crumbs:", host_frame());
    psp_crumbs_dump(out, 32);
    me_back(out);
    psp_log_close();
}

static void on_signal(int sig) {
    host_log("signal %d (abort) at frame %u; the last crumbs:", sig, host_frame());
    psp_crumbs_dump(out, 32);
    psp_exit();
}

/* MVC2_CRASH_HANDLER=0 leaves the kernel's own */
void psp_crash_install(void) {
    uint32_t reg, get, rel, old = 0, ret = 0, ours = (uint32_t)(uintptr_t)psp_crash_vector | 0x80000000u;
    int r;
    atexit(at_exit);
    signal(SIGABRT, on_signal);
    if (getenv("MVC2_CRASH_HANDLER") != NULL && atoi(getenv("MVC2_CRASH_HANDLER")) == 0) {
        host_log("crash handler: off (MVC2_CRASH_HANDLER=0)");
        return;
    }
    psp_crash_sp = (uint32_t)(uintptr_t)&crash_stack[sizeof crash_stack / sizeof *crash_stack - 4];
    reg = find_exception_call(NID_REGISTER_DEFAULT);
    get = find_exception_call(NID_GET_DEFAULT);
    rel = find_exception_call(NID_RELEASE_DEFAULT);
    host_log("crash handler: the kernel's calls at %08lX %08lX %08lX, kubridge's user level %d", (unsigned long)reg,
             (unsigned long)get, (unsigned long)rel, kuKernelGetUserLevel());
    if (reg == 0) {
        host_log("crash handler: none (sceKernelRegisterDefaultExceptionHandler not found)");
        return;
    }
    if (get != 0) {
        r = kernel_call(get, 0, &old);
        host_log("crash handler: the active one %08lX (%d)", (unsigned long)old, r);
    }
    r = kernel_call(reg, ours, &ret);
    if ((r < 0 || (int)ret < 0) && rel != 0 && old != 0) {
        uint32_t ret2;
        int r2 = kernel_call(rel, old, &ret2);
        host_log("crash handler: registering gave %d %08lX; the old one released: %d %08lX", r, (unsigned long)ret, r2,
                 (unsigned long)ret2);
        r = kernel_call(reg, ours, &ret);
        old_released = r2 >= 0 && (int)ret2 >= 0 ? old : 0;
    }
    host_log("crash handler: %08lX registered: %d %08lX", (unsigned long)ours, r, (unsigned long)ret);
    if (r >= 0 && (int)ret >= 0) {
        release_call = rel;
        register_call = reg;
        ours_held = ours;
    }
    if (get != 0) {
        kernel_call(get, 0, &old);
        host_log("crash handler: now the active one is %08lX%s", (unsigned long)old, old == ours ? ", ours" : "");
    }
    if (r >= 0 && (int)ret >= 0) {
        bus_handlers();
        probe();
    }
}

/* MVC2_CRASH=align|null|break|fpe|hang|sleep at MVC2_CRASH_AT's frame (0: at once) */
void psp_crash_test(unsigned frame) {
    static uint32_t words[2];
    const char* kind = getenv("MVC2_CRASH");
    const char* at = getenv("MVC2_CRASH_AT");
    if (kind == NULL || (at != NULL ? (unsigned)atoi(at) : 0u) != frame) {
        return;
    }
    host_log("crash test: %s at frame %u", kind, frame);
    if (strcmp(kind, "align") == 0) {
        volatile uint32_t* p = (volatile uint32_t*)((char*)words + 1);
        words[0] = *p;
    } else if (strcmp(kind, "null") == 0) {
        words[0] = *(volatile uint32_t*)0;
    } else if (strcmp(kind, "intr") == 0) {
        int intr = sceKernelCpuSuspendIntr();   /* a crash with interrupts off */
        words[0] = *(volatile uint32_t*)0;
        sceKernelCpuResumeIntr(intr);
    } else if (strcmp(kind, "break") == 0) {
        __asm__ volatile("break");
    } else if (strcmp(kind, "fpe") == 0) {
        volatile float z = 0.0f, q;
        uint32_t fcr;
        __asm__ volatile("cfc1 %0, $31" : "=r"(fcr));
        fcr |= 0x00000800u;     /* invalid operation enabled */
        __asm__ volatile("ctc1 %0, $31" : : "r"(fcr));
        q = z / z;
        (void)q;
    } else if (strcmp(kind, "hang") == 0) {
        for (;;) {
        }
    } else if (strcmp(kind, "sleep") == 0) {
        sceKernelSleepThread();
    } else if (strcmp(kind, "ge") == 0) {
        /* a GE list that jumps to itself, waited for: the GE never finishes */
        static uint32_t list[4] __attribute__((aligned(64)));
        uint32_t a = (uint32_t)(uintptr_t)list;
        list[0] = 0x10u << 24 | (a >> 24 & 15) << 16;   /* BASE */
        list[1] = 0x08u << 24 | (a & 0xFFFFFF);          /* JUMP */
        sceKernelDcacheWritebackRange(list, sizeof list);
        sceGeListEnQueue(list, NULL, -1, NULL);
        sceGeDrawSync(0);
    }
    host_log("crash test: %s did not crash", kind);
}

/* MVC2_NEXT=ms0:/PSP/GAME/.../EBOOT.PBP: that program run next, as for a hardware A/B in one sitting */
static void next_program(void) {
    static char path[256];
    struct SceKernelLoadExecVSHParam vsh;
    struct SceKernelLoadExecParam param;
    const char* next = getenv("MVC2_NEXT");
    int r;
    if (next == NULL || *next == 0) {
        return;
    }
    snprintf(path, sizeof path, "%s", next);
    host_log("the next program: %s", path);
    psp_log_close();
    memset(&vsh, 0, sizeof vsh);
    vsh.size = sizeof vsh;
    vsh.args = strlen(path) + 1;
    vsh.argp = path;
    vsh.key = "game";
    r = sctrlKernelLoadExecVSHMs2(path, &vsh);
    memset(&param, 0, sizeof param);
    param.size = sizeof param;
    param.args = strlen(path) + 1;
    param.argp = path;
    param.key = NULL;
    sceKernelLoadExec(path, &param);
    host_log("the next program: not run (%08X)", (unsigned)r);
}

/* a crash's or a hang's end. Memory may be wrong, so the memory card isn't saved */
/* the screen kept until X is pressed; inside an interrupt, for good */
static void wait_for_button(void) {
    SceCtrlData pad;
    int down = 1;
    if (sceKernelGetThreadId() < 0) {
        screen("(inside an interrupt: hold the power switch to turn the PSP off)");
        for (;;) {
        }
    }
    screen("press X to end the program");
    for (;;) {
        memset(&pad, 0, sizeof pad);
        sceCtrlPeekBufferPositive(&pad, 1);
        if (!(pad.Buttons & PSP_CTRL_CROSS)) {
            down = 0;
        } else if (!down) {
            return;
        }
        sceKernelDelayThread(50 * 1000);
        psp_crumb("a report shown", 0);     /* the watchdog's: waiting on the player */
    }
}

void psp_report_written(int r) {
    char text[80];
    if (r < 0) {
        snprintf(text, sizeof text, "not written (%08X): the screen is the report", (unsigned)r);
    } else {
        snprintf(text, sizeof text, "written (%d bytes, %lu ms after the crash)", r,
                 crash_us != 0 ? (unsigned long)(sceKernelGetSystemTimeLow() - crash_us) / 1000 : 0ul);
    }
    screen(text);
    wait_for_button();
    me_back(screen);
    release_handlers();
    sceKernelExitGame();
}

volatile uint32_t psp_exit_step_us;

static void exit_step(const char* what) {
    host_log("exit: %s", what);
    psp_exit_step_us = sceKernelGetSystemTimeLow() | 1u;
}

void psp_exit(void) {
    psp_prof_stop();
    exit_step("saving the memory card");
    dc_vmu_flush();
    psp_exit_step_us = sceKernelGetSystemTimeLow() | 1u;
    me_back(out);
    exit_step("letting go of the exception handlers");
    release_handlers();
    next_program();
    exit_step("back to the menu");
    psp_log_close();
    sceKernelExitGame();
}

/* HOME's quit when the game's thread hasn't ended the program: the log, the ME given back (1 s at most), out */
void psp_exit_now(void) {
    host_log("exit: the game's thread didn't end the program; ending it from HOME's quit");
    psp_log_hold(0);
    me_back(out);
    release_handlers();
    host_log("exit: back to the menu");
    psp_log_close();
    sceKernelExitGame();
}

/* ---- misaligned accesses: a build with -fsanitize=alignment logs each place once (for PPSSPP, which allows them) */

typedef struct {
    const char* file;
    uint32_t line, column;
} UBSAN_PLACE;

typedef struct {
    UBSAN_PLACE place;
    const void* type;
    unsigned char log_alignment;
    unsigned char kind;
} UBSAN_MISMATCH;

void __ubsan_handle_type_mismatch_v1(UBSAN_MISMATCH* d, uintptr_t ptr) {
    static const UBSAN_MISMATCH* seen[256];
    static int nseen;
    int i;
    if (ptr == 0) {
        return;
    }
    for (i = 0; i < nseen; i++) {
        if (seen[i] == d) {
            return;
        }
    }
    if (nseen < (int)(sizeof seen / sizeof *seen)) {
        seen[nseen++] = d;
        host_log("misaligned: %s:%lu:%lu: %u-byte aligned access at %08lX (frame %u)", d->place.file,
                 (unsigned long)d->place.line, (unsigned long)d->place.column, 1u << d->log_alignment,
                 (unsigned long)ptr, host_frame());
    }
}

void __ubsan_handle_type_mismatch_v1_abort(UBSAN_MISMATCH* d, uintptr_t ptr) {
    __ubsan_handle_type_mismatch_v1(d, ptr);
}
