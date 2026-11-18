/*
 * patch_dll.h  Ed25519 + SHA-256 verified hot-loadable Dreamcast game patch DLLs (dc_game.dll)
 */

#ifndef PATCH_DLL_H
#define PATCH_DLL_H

#include <stddef.h>
#include "naomi_host.h"

const char* patch_dll_default_id(void);
const char* patch_dll_active_id(void);
const char* patch_dll_overlay_dir(void);

int patch_dll_verify_dir(const char* patch_dir, const char* expected_patch_id, char* err, size_t err_cap);
int patch_dll_activate(const char* patch_id);
void patch_dll_unload(void);

const HOST_PROGRAM* patch_dll_program(const HOST_PROGRAM* builtin_dc);
void patch_dll_apply_overlay(void);

int patch_dll_run_self_test(void);

#endif
