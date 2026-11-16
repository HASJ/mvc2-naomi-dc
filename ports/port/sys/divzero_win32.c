/*
 * divzero_win32.c  Division by zero gives 0 quotient and remainder, as SHC's code did: the exception
 * handler zeroes the faulting DIV/IDIV's results and skips it
 */

#if defined(_WIN32) && defined(_M_IX86) || defined(_WIN32) && defined(__i386__)

#include <stdio.h>
#include <windows.h>
#include <dbghelp.h>
#include "naomi_host.h"

static const unsigned char* image_lo;
static const unsigned char* image_hi;

static int is_fatal_exception(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_INT_OVERFLOW:
        return 1;
    default:
        return 0;
    }
}

static void format_symbol(HMODULE dbghelp, HANDLE proc, DWORD64 addr, char* out, size_t out_sz) {
    typedef BOOL(WINAPI * SymFromAddrFn)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
    static SymFromAddrFn pSymFromAddr;
    static int sym_checked;
    struct {
        SYMBOL_INFO info;
        char name_buf[256];
    } sym;
    DWORD64 disp = 0;

    if (!sym_checked && dbghelp != NULL) {
        typedef BOOL(WINAPI * SymInitFn)(HANDLE, PCSTR, BOOL);
        SymInitFn pSymInitialize = (SymInitFn)(void*)GetProcAddress(dbghelp, "SymInitialize");
        pSymFromAddr = (SymFromAddrFn)(void*)GetProcAddress(dbghelp, "SymFromAddr");
        if (pSymInitialize != NULL) {
            pSymInitialize(proc, NULL, TRUE);
        }
        sym_checked = 1;
    }
    if (pSymFromAddr != NULL) {
        memset(&sym, 0, sizeof sym);
        sym.info.SizeOfStruct = sizeof(SYMBOL_INFO);
        sym.info.MaxNameLen = sizeof sym.name_buf;
        if (pSymFromAddr(proc, addr, &disp, &sym.info)) {
            snprintf(out, out_sz, "%s+0x%llx (0x%08lX)", sym.info.Name, (unsigned long long)disp, (unsigned long)addr);
            return;
        }
    }
    snprintf(out, out_sz, "0x%08lX", (unsigned long)addr);
}

static void log_fatal_exception(EXCEPTION_POINTERS* ep) {
    static volatile LONG crashed;
    CONTEXT* c = ep->ContextRecord;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    HMODULE dbghelp;
    HANDLE proc;
    void* frames[16];
    USHORT n, i;
    char sym_buf[320];

    if (InterlockedExchange(&crashed, 1) != 0) {
        return;
    }
    dbghelp = LoadLibraryA("dbghelp.dll");
    proc = GetCurrentProcess();
    format_symbol(dbghelp, proc, (DWORD64)c->Eip, sym_buf, sizeof sym_buf);
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        host_log("CRASH: exception 0x%08lX (access violation, op=%lu addr=0x%08lX) at %s [frame %u]",
                 (unsigned long)code, (unsigned long)ep->ExceptionRecord->ExceptionInformation[0],
                 (unsigned long)ep->ExceptionRecord->ExceptionInformation[1], sym_buf, host_frame());
    } else {
        host_log("CRASH: exception 0x%08lX at %s [frame %u]", (unsigned long)code, sym_buf, host_frame());
    }
    host_log("  EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX ESI=%08lX EDI=%08lX EBP=%08lX ESP=%08lX",
             (unsigned long)c->Eax, (unsigned long)c->Ebx, (unsigned long)c->Ecx, (unsigned long)c->Edx,
             (unsigned long)c->Esi, (unsigned long)c->Edi, (unsigned long)c->Ebp, (unsigned long)c->Esp);
    n = CaptureStackBackTrace(0, 16, frames, NULL);
    for (i = 0; i < n; i++) {
        format_symbol(dbghelp, proc, (DWORD64)(uintptr_t)frames[i], sym_buf, sizeof sym_buf);
        host_log("  frame[%u] = %s", i, sym_buf);
    }
}

static LONG CALLBACK divzero_handler(EXCEPTION_POINTERS* ep) {
    CONTEXT* c = ep->ContextRecord;
    const unsigned char* p = (const unsigned char*)c->Eip;
    int op16 = 0;
    int byte_op;
    int len;
    unsigned char modrm;
    int mod, rm;

    if (is_fatal_exception(ep->ExceptionRecord->ExceptionCode)) {
        log_fatal_exception(ep);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    /* only the exe's own divisions */
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_INT_DIVIDE_BY_ZERO || p < image_lo || p >= image_hi) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    len = 0;
    while (p[len] == 0x66 || p[len] == 0x2E || p[len] == 0x3E || p[len] == 0x26 || p[len] == 0x36 || p[len] == 0x64 ||
           p[len] == 0x65) {
        if (p[len] == 0x66) {
            op16 = 1;
        }
        len++;
    }
    if (p[len] != 0xF6 && p[len] != 0xF7) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    byte_op = p[len] == 0xF6;
    modrm = p[len + 1];
    if (((modrm >> 3) & 7) < 6) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    mod = modrm >> 6;
    rm = modrm & 7;
    len += 2;
    if (mod != 3) {
        if (rm == 4) {
            unsigned char sib = p[len++];
            if (mod == 0 && (sib & 7) == 5) {
                len += 4;
            }
        }
        if (mod == 1) {
            len += 1;
        } else if (mod == 2 || (mod == 0 && rm == 5)) {
            len += 4;
        }
    }
    if (byte_op) {
        c->Eax &= 0xFFFF0000;               /* AL quotient, AH remainder */
    } else if (op16) {
        c->Eax &= 0xFFFF0000;
        c->Edx &= 0xFFFF0000;
    } else {
        c->Eax = 0;
        c->Edx = 0;
    }
    c->Eip += len;
    return EXCEPTION_CONTINUE_EXECUTION;
}

void host_divzero_install(void) {
    const unsigned char* base = (const unsigned char*)GetModuleHandleA(NULL);
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
    image_lo = base;
    image_hi = base + nt->OptionalHeader.SizeOfImage;
    AddVectoredExceptionHandler(1, divzero_handler);
}

#else

void host_divzero_install(void) {
}

#endif
