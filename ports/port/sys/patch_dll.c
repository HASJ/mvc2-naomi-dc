/*
 * patch_dll.c  Signed dc_game.dll loader with Ed25519 + SHA-256 verification before LoadLibraryA
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "patch_dll.h"
#include "ed25519_verify.h"
#include "naomi_input.h"
#include "patch_api_gen.h"
#include "rd.h"
#include "savestate.h"

extern void sdl_audio_step_silent(void);

/* Release/dev signing public key matching ports/port/tools/dev_patch_seed.hex */
static const uint8_t PATCH_RELEASE_PUBKEY[32] = {
    0x63, 0x79, 0x48, 0x04, 0xf8, 0x0c, 0x48, 0xf5,
    0x47, 0x72, 0x1d, 0x09, 0x9d, 0x2c, 0xba, 0x95,
    0x5e, 0x3c, 0x73, 0xfa, 0xa0, 0xeb, 0xdf, 0xeb,
    0x55, 0x85, 0xa6, 0x23, 0x20, 0x39, 0xde, 0xc9,
};

#ifdef _WIN32
static HMODULE active_hmod;
#else
static void* active_hmod;
#endif
static const HOST_PROGRAM* active_prog;
static char active_patch_id[64] = MVC2_DEFAULT_PATCH_ID;
static char active_overlay_dir[1280];

typedef const HOST_PROGRAM* (*mvc2_patch_bind_fn)(const MVC2_HOST_API* api);

static int is_safe_patch_id(const char* id) {
    size_t i, n;
    if (id == NULL || id[0] == 0) return 0;
    n = strlen(id);
    if (n >= 32) return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)id[i];
        if (!(isalnum(c) || c == '-' || c == '_' || c == '.')) return 0;
    }
    if (strstr(id, "..") != NULL) return 0;
    return 1;
}

static int is_safe_rel_path(const char* rel) {
    size_t i;
    if (rel == NULL || rel[0] == 0 || rel[0] == '/' || rel[0] == '\\') return 0;
    if (strchr(rel, ':') != NULL || strchr(rel, '\\') != NULL) return 0;
    if (strstr(rel, "..") != NULL) return 0;
    for (i = 0; rel[i] != 0; i++) {
        if ((unsigned char)rel[i] < 0x20) return 0;
    }
    return 1;
}

static uint8_t* read_entire_file(const char* path, size_t max_bytes, size_t* out_len) {
    FILE* f;
    long sz;
    uint8_t* buf;
    size_t n;
    f = fopen(path, "rb");
    if (f == NULL) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    sz = ftell(f);
    if (sz < 0 || (size_t)sz > max_bytes || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = (uint8_t*)malloc((size_t)sz + 1u);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    n = sz > 0 ? fread(buf, 1, (size_t)sz, f) : 0;
    fclose(f);
    if (n != (size_t)sz) {
        free(buf);
        return NULL;
    }
    buf[n] = 0;
    if (out_len != NULL) *out_len = n;
    return buf;
}

static const char* skip_ws(const char* p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

static int json_parse_str(const char** pp, char* out, size_t cap) {
    const char* p = skip_ws(*pp);
    size_t n = 0;
    if (*p != '"') return 0;
    p++;
    while (*p != 0 && *p != '"') {
        char c = *p++;
        if (c == '\\') {
            if (*p == 0) return 0;
            c = *p++;
            if (c == 'n') c = '\n';
            else if (c == 'r') c = '\r';
            else if (c == 't') c = '\t';
        }
        if (n + 1 >= cap) return 0;
        out[n++] = c;
    }
    if (*p != '"') return 0;
    out[n] = 0;
    *pp = p + 1;
    return 1;
}

static int json_find_key_in_range(const char* start, const char* end, const char* key, const char** val_out) {
    size_t klen = strlen(key);
    const char* p = start;
    while (p < end) {
        if (*p == '"') {
            const char* q = p + 1;
            int depth_ok = 1;
            while (q < end && *q != '"') {
                if (*q == '\\' && q + 1 < end) q += 2;
                else q++;
            }
            if (q >= end) break;
            if (depth_ok && (size_t)(q - (p + 1)) == klen && memcmp(p + 1, key, klen) == 0) {
                const char* c = skip_ws(q + 1);
                if (*c == ':') {
                    *val_out = skip_ws(c + 1);
                    return 1;
                }
            }
            p = q + 1;
        } else if (*p == '[' || *p == '{') {
            /* skip nested structure when searching top-level object */
            if (p != start) {
                char open_ch = *p++;
                char close_ch = open_ch == '[' ? ']' : '}';
                int nest = 1;
                while (p < end && nest > 0) {
                    if (*p == '"') {
                        p++;
                        while (p < end && *p != '"') {
                            if (*p == '\\' && p + 1 < end) p += 2;
                            else p++;
                        }
                        if (p < end) p++;
                    } else {
                        if (*p == open_ch) nest++;
                        else if (*p == close_ch) nest--;
                        p++;
                    }
                }
            } else {
                p++;
            }
        } else {
            p++;
        }
    }
    return 0;
}

static int json_get_string(const char* obj, const char* end, const char* key, char* out, size_t cap) {
    const char* v;
    if (!json_find_key_in_range(obj, end, key, &v)) return 0;
    return json_parse_str(&v, out, cap);
}

static int json_get_u64(const char* obj, const char* end, const char* key, uint64_t* out) {
    const char* v;
    char* ep = NULL;
    unsigned long long val;
    if (!json_find_key_in_range(obj, end, key, &v) || !isdigit((unsigned char)*v)) return 0;
    val = strtoull(v, &ep, 10);
    if (ep == v) return 0;
    *out = (uint64_t)val;
    return 1;
}

static int file_size_exact(const char* path, uint64_t* out_sz) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA fad;
    ULARGE_INTEGER u;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) return 0;
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return 0;
    u.LowPart = fad.nFileSizeLow;
    u.HighPart = fad.nFileSizeHigh;
    *out_sz = (uint64_t)u.QuadPart;
    return 1;
#else
    FILE* f = fopen(path, "rb");
    long sz;
    if (f == NULL) return 0;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fclose(f);
    if (sz < 0) return 0;
    *out_sz = (uint64_t)sz;
    return 1;
#endif
}

static int count_files_recursive(const char* dir, int* count) {
#ifdef _WIN32
    char pattern[1200];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snprintf(pattern, sizeof pattern, "%s\\*", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 1;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            char sub[1200];
            snprintf(sub, sizeof sub, "%s/%s", dir, fd.cFileName);
            if (!count_files_recursive(sub, count)) {
                FindClose(h);
                return 0;
            }
        } else {
            (*count)++;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return 1;
#else
    (void)dir;
    *count = 0;
    return 1;
#endif
}

int patch_dll_verify_dir(const char* patch_dir, const char* expected_patch_id, char* err, size_t err_cap) {
    char sig_path[1200], man_path[1200], dll_path[1200], disc_dir[1200];
    uint8_t* sig = NULL;
    uint8_t* man = NULL;
    size_t sig_len = 0, man_len = 0;
    const char* dev_env = getenv("MVC2_DEV_PATCHES");
    int dev_mode = dev_env != NULL && strcmp(dev_env, "1") == 0;
    char man_id[64], dll_name[64], dll_sha_expected[128], dll_sha_actual[65];
    uint64_t dll_size_expected = 0, dll_size_actual = 0;
    uint8_t digest[32];
    const char* man_end;
    const char* arr = NULL;
    int manifested_disc_count = 0, actual_disc_count = 0;

    if (err != NULL && err_cap > 0) err[0] = 0;
    if (patch_dir == NULL || !is_safe_patch_id(expected_patch_id)) {
        if (err) snprintf(err, err_cap, "invalid patch id or dir");
        return 0;
    }

    snprintf(sig_path, sizeof sig_path, "%s/manifest.sig", patch_dir);
    snprintf(man_path, sizeof man_path, "%s/manifest.json", patch_dir);
    snprintf(dll_path, sizeof dll_path, "%s/dc_game.dll", patch_dir);
    snprintf(disc_dir, sizeof disc_dir, "%s/disc", patch_dir);

    sig = read_entire_file(sig_path, 128u, &sig_len);
    if (sig == NULL && dev_mode) {
        if (!file_size_exact(dll_path, &dll_size_actual)) {
            if (err) snprintf(err, err_cap, "missing dc_game.dll in dev mode");
            return 0;
        }
        host_log("patch_dll: DEV MODE unsigned load for %s", expected_patch_id);
        return 1;
    }
    if (sig == NULL || sig_len != 64u) {
        free(sig);
        if (err) snprintf(err, err_cap, "missing or invalid manifest.sig");
        return 0;
    }

    man = read_entire_file(man_path, 262144u, &man_len);
    if (man == NULL) {
        free(sig);
        if (err) snprintf(err, err_cap, "missing manifest.json");
        return 0;
    }

    if (!ed25519_verify(sig, man, man_len, PATCH_RELEASE_PUBKEY)) {
        free(sig);
        free(man);
        if (err) snprintf(err, err_cap, "Ed25519 signature verification failed");
        return 0;
    }
    free(sig);

    man_end = (const char*)man + man_len;
    if (!json_get_string((const char*)man, man_end, "patch_id", man_id, sizeof man_id) ||
        strcmp(man_id, expected_patch_id) != 0) {
        free(man);
        if (err) snprintf(err, err_cap, "manifest patch_id mismatch");
        return 0;
    }
    if (!json_get_string((const char*)man, man_end, "dll_name", dll_name, sizeof dll_name) ||
        strcmp(dll_name, "dc_game.dll") != 0 ||
        !json_get_string((const char*)man, man_end, "dll_sha256", dll_sha_expected, sizeof dll_sha_expected) ||
        !json_get_u64((const char*)man, man_end, "dll_size", &dll_size_expected)) {
        free(man);
        if (err) snprintf(err, err_cap, "manifest missing dll fields");
        return 0;
    }
    if (!file_size_exact(dll_path, &dll_size_actual) || dll_size_actual != dll_size_expected) {
        free(man);
        if (err) snprintf(err, err_cap, "dc_game.dll size mismatch");
        return 0;
    }
    if (!sha256_file(dll_path, digest)) {
        free(man);
        if (err) snprintf(err, err_cap, "failed to hash dc_game.dll");
        return 0;
    }
    sha256_hex(digest, dll_sha_actual);
    if (strcmp(dll_sha_actual, dll_sha_expected) != 0) {
        free(man);
        if (err) snprintf(err, err_cap, "dc_game.dll SHA-256 mismatch");
        return 0;
    }

    if (json_find_key_in_range((const char*)man, man_end, "disc_files", &arr) && *arr == '[') {
        const char* p = arr + 1;
        while (p < man_end) {
            const char* obj_start;
            const char* obj_end;
            int depth;
            char rel[260], exp_sha[128], act_sha[65], fpath[1600];
            uint64_t exp_sz = 0, act_sz = 0;

            p = skip_ws(p);
            if (*p == ']') break;
            if (*p == ',') { p++; continue; }
            if (*p != '{') {
                free(man);
                if (err) snprintf(err, err_cap, "malformed disc_files entry");
                return 0;
            }
            obj_start = p;
            depth = 1;
            p++;
            while (p < man_end && depth > 0) {
                if (*p == '"') {
                    p++;
                    while (p < man_end && *p != '"') {
                        if (*p == '\\' && p + 1 < man_end) p += 2;
                        else p++;
                    }
                    if (p < man_end) p++;
                } else {
                    if (*p == '{') depth++;
                    else if (*p == '}') depth--;
                    p++;
                }
            }
            obj_end = p;
            if (!json_get_string(obj_start, obj_end, "name", rel, sizeof rel) ||
                !json_get_string(obj_start, obj_end, "sha256", exp_sha, sizeof exp_sha) ||
                !json_get_u64(obj_start, obj_end, "size", &exp_sz) ||
                !is_safe_rel_path(rel)) {
                free(man);
                if (err) snprintf(err, err_cap, "invalid disc file metadata in manifest");
                return 0;
            }
            snprintf(fpath, sizeof fpath, "%s/%s", disc_dir, rel);
            if (!file_size_exact(fpath, &act_sz) || act_sz != exp_sz || !sha256_file(fpath, digest)) {
                free(man);
                if (err) snprintf(err, err_cap, "disc overlay file missing or size mismatch: %s", rel);
                return 0;
            }
            sha256_hex(digest, act_sha);
            if (strcmp(act_sha, exp_sha) != 0) {
                free(man);
                if (err) snprintf(err, err_cap, "disc overlay file SHA-256 mismatch: %s", rel);
                return 0;
            }
            manifested_disc_count++;
        }
    }

    if (!count_files_recursive(disc_dir, &actual_disc_count) || actual_disc_count != manifested_disc_count) {
        free(man);
        if (err) snprintf(err, err_cap, "unexpected unmanifested file in disc overlay");
        return 0;
    }

    free(man);
    return 1;
}

static int find_patch_dir(const char* patch_id, char* out, size_t cap) {
#ifdef _WIN32
    char rel[128], candidate[1024], dll_check[1200];
    DWORD attr;
    snprintf(rel, sizeof rel, "patches/%s", patch_id);
    snprintf(candidate, sizeof candidate, "%s", host_config_path(rel));
    snprintf(dll_check, sizeof dll_check, "%s/dc_game.dll", candidate);
    attr = GetFileAttributesA(dll_check);
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        snprintf(out, cap, "%s", candidate);
        return 1;
    }
    snprintf(candidate, sizeof candidate, "%s", host_exe_path(rel));
    snprintf(dll_check, sizeof dll_check, "%s/dc_game.dll", candidate);
    attr = GetFileAttributesA(dll_check);
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        snprintf(out, cap, "%s", candidate);
        return 1;
    }
#else
    (void)patch_id;
    (void)out;
    (void)cap;
#endif
    return 0;
}

void patch_dll_unload(void) {
#ifdef _WIN32
    if (active_prog != NULL) {
        host_forget_program(&active_prog->span);
        active_prog = NULL;
    }
    if (active_hmod != NULL) {
        host_divzero_disallow_module(active_hmod);
        FreeLibrary(active_hmod);
        active_hmod = NULL;
    }
    active_overlay_dir[0] = 0;
    dc_gd_set_overlay_dir(NULL);
    snprintf(active_patch_id, sizeof active_patch_id, "%s", MVC2_DEFAULT_PATCH_ID);
    savestate_reset_pools();
#endif
}

int patch_dll_activate(const char* patch_id) {
#ifdef _WIN32
    char patch_dir[1024], dll_path[1200], disc_path[1200], err[256];
    HMODULE hm;
    mvc2_patch_bind_fn bind_fn;
    const HOST_PROGRAM* prog;
    DWORD attr;

    if (!is_safe_patch_id(patch_id)) {
        host_log("patch_dll: rejected unsafe patch_id");
        return 0;
    }
    if (active_hmod != NULL && strcmp(active_patch_id, patch_id) == 0) {
        patch_dll_apply_overlay();
        return 1;
    }
    if (!find_patch_dir(patch_id, patch_dir, sizeof patch_dir)) {
        host_log("patch_dll: patch %s not found locally", patch_id);
        return 0;
    }
    if (!patch_dll_verify_dir(patch_dir, patch_id, err, sizeof err)) {
        host_log("patch_dll: REJECTED %s before LoadLibraryA: %s", patch_id, err);
        return 0;
    }

    if (board_program() != NULL) {
        host_restart();
    }
    patch_dll_unload();

    snprintf(dll_path, sizeof dll_path, "%s/dc_game.dll", patch_dir);
    hm = LoadLibraryA(dll_path);
    if (hm == NULL) {
        host_log("patch_dll: LoadLibraryA(%s) failed (%lu)", dll_path, (unsigned long)GetLastError());
        return 0;
    }
    host_divzero_allow_module(hm);
    bind_fn = (mvc2_patch_bind_fn)(void*)GetProcAddress(hm, "mvc2_patch_bind");
    if (bind_fn == NULL) {
        host_log("patch_dll: mvc2_patch_bind missing in %s", dll_path);
        host_divzero_disallow_module(hm);
        FreeLibrary(hm);
        return 0;
    }
    prog = bind_fn(host_get_patch_api());
    if (prog == NULL) {
        host_log("patch_dll: host API bind rejected by %s", dll_path);
        host_divzero_disallow_module(hm);
        FreeLibrary(hm);
        return 0;
    }
    active_hmod = hm;
    active_prog = prog;
    snprintf(active_patch_id, sizeof active_patch_id, "%s", patch_id);
    snprintf(disc_path, sizeof disc_path, "%s/disc", patch_dir);
    attr = GetFileAttributesA(disc_path);
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        snprintf(active_overlay_dir, sizeof active_overlay_dir, "%s", disc_path);
    } else {
        active_overlay_dir[0] = 0;
    }
    dc_gd_set_overlay_dir(active_overlay_dir);
    savestate_reset_pools();
    host_log("patch_dll: activated %s (%s)", active_patch_id, dll_path);
    return 1;
#else
    (void)patch_id;
    return 0;
#endif
}

const char* patch_dll_default_id(void) {
    return MVC2_DEFAULT_PATCH_ID;
}

const char* patch_dll_active_id(void) {
    return active_patch_id;
}

const char* patch_dll_overlay_dir(void) {
    return active_overlay_dir;
}

const HOST_PROGRAM* patch_dll_program(const HOST_PROGRAM* builtin_dc) {
    return active_prog != NULL ? active_prog : builtin_dc;
}

void patch_dll_apply_overlay(void) {
    dc_gd_set_overlay_dir(active_overlay_dir[0] != 0 ? active_overlay_dir : NULL);
}

#ifdef _WIN32
static void remove_dir_recursive(const char* dir) {
    char pattern[1200];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snprintf(pattern, sizeof pattern, "%s\\*", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char sub[1200];
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        snprintf(sub, sizeof sub, "%s/%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            remove_dir_recursive(sub);
        } else {
            DeleteFileA(sub);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    RemoveDirectoryA(dir);
}

static int flip_one_byte_at(const char* path, long offset) {
    FILE* f = fopen(path, "r+b");
    int c;
    if (f == NULL) return 0;
    if (fseek(f, offset, SEEK_SET) != 0 || (c = fgetc(f)) == EOF) {
        fclose(f);
        return 0;
    }
    if (fseek(f, offset, SEEK_SET) != 0 || fputc(c ^ 0x5Au, f) == EOF) {
        fclose(f);
        return 0;
    }
    fclose(f);
    return 1;
}
#endif

int patch_dll_run_self_test(void) {
#ifdef _WIN32
    char def_dir[1024], tmp_dir[1024], src[1200], dst[1200], err[256];
    const char* env_old = getenv("MVC2_DEV_PATCHES");
    int f;

    if (!ed25519_self_test()) {
        host_log("PATCH_CHECK FAIL: RFC 8032 Ed25519 self-test failed");
        return 1;
    }
    if (env_old != NULL && strcmp(env_old, "1") == 0) {
        SetEnvironmentVariableA("MVC2_DEV_PATCHES", "0");
    }
    if (!find_patch_dir(MVC2_DEFAULT_PATCH_ID, def_dir, sizeof def_dir)) {
        host_log("PATCH_CHECK FAIL: default patch %s not found in build-dist/patches", MVC2_DEFAULT_PATCH_ID);
        return 1;
    }
    if (!patch_dll_verify_dir(def_dir, MVC2_DEFAULT_PATCH_ID, err, sizeof err)) {
        host_log("PATCH_CHECK FAIL: default signed manifest rejected: %s", err);
        return 1;
    }

    snprintf(tmp_dir, sizeof tmp_dir, "%s", host_exe_path("patches/_tmp_tamper_test"));
    remove_dir_recursive(tmp_dir);
    CreateDirectoryA(tmp_dir, NULL);

    snprintf(src, sizeof src, "%s/dc_game.dll", def_dir);
    snprintf(dst, sizeof dst, "%s/dc_game.dll", tmp_dir);
    if (!CopyFileA(src, dst, FALSE)) {
        host_log("PATCH_CHECK FAIL: CopyFileA dll failed");
        return 1;
    }
    snprintf(src, sizeof src, "%s/manifest.json", def_dir);
    snprintf(dst, sizeof dst, "%s/manifest.json", tmp_dir);
    if (!CopyFileA(src, dst, FALSE)) {
        host_log("PATCH_CHECK FAIL: CopyFileA manifest.json failed");
        return 1;
    }
    snprintf(src, sizeof src, "%s/manifest.sig", def_dir);
    snprintf(dst, sizeof dst, "%s/manifest.sig", tmp_dir);
    if (!CopyFileA(src, dst, FALSE)) {
        host_log("PATCH_CHECK FAIL: CopyFileA manifest.sig failed");
        return 1;
    }

    if (!patch_dll_verify_dir(tmp_dir, MVC2_DEFAULT_PATCH_ID, err, sizeof err)) {
        host_log("PATCH_CHECK FAIL: copied clean patch dir failed verify: %s", err);
        remove_dir_recursive(tmp_dir);
        return 1;
    }

    /* Tamper 1 byte in dc_game.dll -> must reject before LoadLibraryA */
    snprintf(dst, sizeof dst, "%s/dc_game.dll", tmp_dir);
    if (!flip_one_byte_at(dst, 64) || patch_dll_verify_dir(tmp_dir, MVC2_DEFAULT_PATCH_ID, err, sizeof err)) {
        host_log("PATCH_CHECK FAIL: tampered dc_game.dll was NOT rejected");
        remove_dir_recursive(tmp_dir);
        return 1;
    }
    flip_one_byte_at(dst, 64);

    /* Tamper 1 byte in manifest.json -> must reject Ed25519 */
    snprintf(dst, sizeof dst, "%s/manifest.json", tmp_dir);
    if (!flip_one_byte_at(dst, 4) || patch_dll_verify_dir(tmp_dir, MVC2_DEFAULT_PATCH_ID, err, sizeof err)) {
        host_log("PATCH_CHECK FAIL: tampered manifest.json was NOT rejected");
        remove_dir_recursive(tmp_dir);
        return 1;
    }
    flip_one_byte_at(dst, 4);

    /* Tamper 1 byte in manifest.sig -> must reject Ed25519 */
    snprintf(dst, sizeof dst, "%s/manifest.sig", tmp_dir);
    if (!flip_one_byte_at(dst, 0) || patch_dll_verify_dir(tmp_dir, MVC2_DEFAULT_PATCH_ID, err, sizeof err)) {
        host_log("PATCH_CHECK FAIL: tampered manifest.sig was NOT rejected");
        remove_dir_recursive(tmp_dir);
        return 1;
    }
    remove_dir_recursive(tmp_dir);

    /* Activate signed default dc_game.dll and advance 120 frames */
    if (!patch_dll_activate(MVC2_DEFAULT_PATCH_ID)) {
        host_log("PATCH_CHECK FAIL: patch_dll_activate(%s) failed", MVC2_DEFAULT_PATCH_ID);
        return 1;
    }
    host_restart();
    if (!start_game()) {
        host_log("PATCH_CHECK FAIL: start_game() with dc_game.dll failed");
        return 1;
    }
    rd_discard = 1;
    for (f = 0; f < 120; f++) {
        HOST_INPUT in;
        memset(&in, 0, sizeof in);
        host_set_input(&in);
        naomi_run_frame();
        sdl_audio_step_silent();
    }
    host_log("PATCH_CHECK PASS patch_id=%s ed25519=ok tamper_reject=3/3 frames=120", MVC2_DEFAULT_PATCH_ID);
    return 0;
#else
    return ed25519_self_test() ? 0 : 1;
#endif
}
