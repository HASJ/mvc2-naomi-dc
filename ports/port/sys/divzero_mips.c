/*
 * divzero_mips.c  Division by zero gives 0 quotient and remainder, as SHC's code did: on MIPS the assembler gcc
 * runs makes each division's check for 0 do that (port/psp/divzero_as.py), so there is nothing to install
 */

void host_divzero_install(void) {
}
