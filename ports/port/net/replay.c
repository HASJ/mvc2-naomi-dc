/*
 * replay.c  .mvc2r v1 binary replay recording, 120f sparse keyframes, and Replay Takeover
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "replay.h"
#include "naomi_host.h"
#include "naomi_input.h"
#include "patch_dll.h"
#include "rd.h"
#include "savestate.h"

extern void sdl_audio_step_silent(void);
extern void sdl_audio_notify_rollback(void);

#define MVC2R_HEADER_BYTES 216u

static double now_ms(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    return 0.0;
#endif
}

static void put_u16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void put_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void put_u64(uint8_t* p, uint64_t v) {
    int i;
    for (i = 0; i < 8; i++) p[i] = (uint8_t)((v >> (i * 8)) & 0xFFu);
}

static uint16_t get_u16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get_u64(const uint8_t* p) {
    uint64_t v = 0;
    int i;
    for (i = 0; i < 8; i++) v |= ((uint64_t)p[i] << (i * 8));
    return v;
}

void mvc2r_init(MVC2R_FILE* r) {
    memset(r, 0, sizeof *r);
    memcpy(r->hdr.magic, "MVC2R\x01", 6);
    r->hdr.winner_side = -1;
    r->hdr.checksum_interval = REPLAY_CHECKSUM_INTERVAL;
}

void mvc2r_free(MVC2R_FILE* r) {
    if (r == NULL) return;
    free(r->frames);
    free(r->checksums);
    memset(r, 0, sizeof *r);
}

int mvc2r_push_frame(MVC2R_FILE* r, uint16_t p1, uint16_t p2, const uint64_t* opt_checksum) {
    uint32_t word = (uint32_t)p1 | ((uint32_t)p2 << 16);
    if (r->hdr.duration_frames >= r->frame_cap) {
        uint32_t ncap = r->frame_cap == 0 ? 1024u : r->frame_cap * 2u;
        uint32_t* nf = (uint32_t*)realloc(r->frames, ncap * sizeof(uint32_t));
        if (nf == NULL) return 0;
        r->frames = nf;
        r->frame_cap = ncap;
    }
    r->frames[r->hdr.duration_frames++] = word;
    if (r->hdr.checksum_interval > 0 && (r->hdr.duration_frames % r->hdr.checksum_interval) == 0) {
        if (r->hdr.checksum_count >= r->checksum_cap) {
            uint32_t ccap = r->checksum_cap == 0 ? 64u : r->checksum_cap * 2u;
            uint64_t* nc = (uint64_t*)realloc(r->checksums, ccap * sizeof(uint64_t));
            if (nc == NULL) return 0;
            r->checksums = nc;
            r->checksum_cap = ccap;
        }
        r->checksums[r->hdr.checksum_count++] = opt_checksum != NULL ? *opt_checksum : savestate_hash();
    }
    return 1;
}

/* XOR delta + RLE:
 * Tag < 0x80: run of (tag + 1) zero deltas (unchanged inputs)
 * Tag >= 0x80: (tag & 0x7F) + 1 literal 32-bit LE XOR-delta words follow
 */
int mvc2r_encode_payload(const uint32_t* frames, uint32_t count, uint8_t** out_buf, uint32_t* out_len) {
    uint32_t cap = count == 0 ? 16u : count * 5u + 16u;
    uint8_t* out = (uint8_t*)malloc(cap);
    uint32_t pos = 0, i = 0, prev = 0;
    if (out == NULL) return 0;
    while (i < count) {
        uint32_t d = frames[i] ^ prev;
        if (d == 0) {
            uint32_t run = 1u;
            while (i + run < count && run < 128u && (frames[i + run] ^ prev) == 0) {
                run++;
            }
            out[pos++] = (uint8_t)(run - 1u);
            i += run;
        } else {
            uint32_t run = 1u, k, tmp_prev = frames[i];
            while (i + run < count && run < 128u) {
                uint32_t nd = frames[i + run] ^ tmp_prev;
                if (nd == 0 && i + run + 1 < count && (frames[i + run + 1] ^ tmp_prev) == 0) {
                    break; /* worth switching to zero run once >= 2 zeroes */
                }
                tmp_prev = frames[i + run];
                run++;
            }
            out[pos++] = (uint8_t)(0x80u | (run - 1u));
            for (k = 0; k < run; k++) {
                uint32_t delta = frames[i + k] ^ prev;
                prev = frames[i + k];
                put_u32(out + pos, delta);
                pos += 4u;
            }
            i += run;
        }
    }
    *out_buf = out;
    *out_len = pos;
    return 1;
}

int mvc2r_decode_payload(const uint8_t* buf, uint32_t len, uint32_t expected_frames, uint32_t** out_frames) {
    uint32_t* frames = expected_frames > 0 ? (uint32_t*)malloc(expected_frames * sizeof(uint32_t)) : NULL;
    uint32_t pos = 0, out_i = 0, prev = 0;
    if (expected_frames > 0 && frames == NULL) return 0;
    while (pos < len) {
        uint8_t tag = buf[pos++];
        uint32_t run = (uint32_t)(tag & 0x7Fu) + 1u;
        if (out_i + run > expected_frames) {
            free(frames);
            return 0;
        }
        if ((tag & 0x80u) == 0) {
            uint32_t k;
            for (k = 0; k < run; k++) frames[out_i++] = prev;
        } else {
            uint32_t k;
            if (pos + run * 4u > len) {
                free(frames);
                return 0;
            }
            for (k = 0; k < run; k++) {
                uint32_t delta = get_u32(buf + pos);
                pos += 4u;
                prev ^= delta;
                frames[out_i++] = prev;
            }
        }
    }
    if (out_i != expected_frames) {
        free(frames);
        return 0;
    }
    *out_frames = frames;
    return 1;
}

static void write_hdr_bytes(const MVC2R_HEADER* h, uint32_t payload_bytes, uint8_t out[MVC2R_HEADER_BYTES]) {
    int side, s;
    memset(out, 0, MVC2R_HEADER_BYTES);
    memcpy(out + 0, h->magic, 6);
    memcpy(out + 6, h->patch_id, 32);
    put_u32(out + 38, h->match_seed);
    put_u64(out + 42, (uint64_t)h->timestamp_utc);
    put_u32(out + 50, h->duration_frames);
    out[54] = (uint8_t)h->winner_side;
    out[55] = h->is_ranked;
    out[56] = h->start_mode;
    out[57] = h->stage_no;
    memcpy(out + 58, h->p1_id, 16);
    memcpy(out + 74, h->p2_id, 16);
    memcpy(out + 90, h->p1_name, 32);
    memcpy(out + 122, h->p2_name, 32);
    put_u16(out + 154, h->p1_rating);
    put_u16(out + 156, h->p2_rating);
    for (side = 0; side < 2; side++) {
        for (s = 0; s < 3; s++) {
            size_t off = 158u + (size_t)(side * 3 + s) * 4u;
            out[off + 0] = h->teams[side][s].char_id;
            out[off + 1] = h->teams[side][s].assist_type;
            out[off + 2] = h->teams[side][s].color;
            out[off + 3] = h->teams[side][s].reserved;
        }
    }
    memcpy(out + 182, h->rules, 16);
    put_u32(out + 198, h->checksum_interval);
    put_u32(out + 202, h->checksum_count);
    put_u32(out + 206, payload_bytes);
}

static int read_hdr_bytes(const uint8_t in[MVC2R_HEADER_BYTES], MVC2R_HEADER* h) {
    int side, s;
    if (memcmp(in, "MVC2R\x01", 6) != 0) return 0;
    memset(h, 0, sizeof *h);
    memcpy(h->magic, in + 0, 6);
    memcpy(h->patch_id, in + 6, 32);
    h->patch_id[31] = 0;
    h->match_seed = get_u32(in + 38);
    h->timestamp_utc = (int64_t)get_u64(in + 42);
    h->duration_frames = get_u32(in + 50);
    h->winner_side = (int8_t)in[54];
    h->is_ranked = in[55];
    h->start_mode = in[56];
    h->stage_no = in[57];
    memcpy(h->p1_id, in + 58, 16); h->p1_id[15] = 0;
    memcpy(h->p2_id, in + 74, 16); h->p2_id[15] = 0;
    memcpy(h->p1_name, in + 90, 32); h->p1_name[31] = 0;
    memcpy(h->p2_name, in + 122, 32); h->p2_name[31] = 0;
    h->p1_rating = get_u16(in + 154);
    h->p2_rating = get_u16(in + 156);
    for (side = 0; side < 2; side++) {
        for (s = 0; s < 3; s++) {
            size_t off = 158u + (size_t)(side * 3 + s) * 4u;
            h->teams[side][s].char_id = in[off + 0];
            h->teams[side][s].assist_type = in[off + 1];
            h->teams[side][s].color = in[off + 2];
            h->teams[side][s].reserved = in[off + 3];
        }
    }
    memcpy(h->rules, in + 182, 16);
    h->checksum_interval = get_u32(in + 198);
    h->checksum_count = get_u32(in + 202);
    h->payload_bytes = get_u32(in + 206);
    return 1;
}

int mvc2r_save(const MVC2R_FILE* r, const char* path) {
    uint8_t hdr_buf[MVC2R_HEADER_BYTES];
    uint8_t* payload = NULL;
    uint32_t payload_len = 0, i;
    FILE* f;
    if (r == NULL || path == NULL) return 0;
    if (!mvc2r_encode_payload(r->frames, r->hdr.duration_frames, &payload, &payload_len)) return 0;
    f = fopen(path, "wb");
    if (f == NULL) {
        free(payload);
        return 0;
    }
    write_hdr_bytes(&r->hdr, payload_len, hdr_buf);
    if (fwrite(hdr_buf, 1, sizeof hdr_buf, f) != sizeof hdr_buf) {
        fclose(f);
        free(payload);
        return 0;
    }
    if (payload_len > 0 && fwrite(payload, 1, payload_len, f) != payload_len) {
        fclose(f);
        free(payload);
        return 0;
    }
    free(payload);
    for (i = 0; i < r->hdr.checksum_count; i++) {
        uint8_t b[8];
        put_u64(b, r->checksums[i]);
        if (fwrite(b, 1, 8, f) != 8) {
            fclose(f);
            return 0;
        }
    }
    fclose(f);
    return 1;
}

int mvc2r_load(MVC2R_FILE* r, const char* path) {
    uint8_t hdr_buf[MVC2R_HEADER_BYTES];
    uint8_t* payload = NULL;
    FILE* f;
    uint32_t i;

    if (r == NULL || path == NULL) return 0;
    mvc2r_free(r);
    f = fopen(path, "rb");
    if (f == NULL) return 0;
    if (fread(hdr_buf, 1, sizeof hdr_buf, f) != sizeof hdr_buf || !read_hdr_bytes(hdr_buf, &r->hdr)) {
        fclose(f);
        return 0;
    }
    if (r->hdr.duration_frames > 360000u || r->hdr.payload_bytes > (16u << 20) || r->hdr.checksum_count > 64000u) {
        fclose(f);
        return 0;
    }
    if (r->hdr.payload_bytes > 0) {
        payload = (uint8_t*)malloc(r->hdr.payload_bytes);
        if (payload == NULL || fread(payload, 1, r->hdr.payload_bytes, f) != r->hdr.payload_bytes) {
            free(payload);
            fclose(f);
            return 0;
        }
    }
    if (!mvc2r_decode_payload(payload, r->hdr.payload_bytes, r->hdr.duration_frames, &r->frames)) {
        free(payload);
        fclose(f);
        return 0;
    }
    free(payload);
    r->frame_cap = r->hdr.duration_frames;
    if (r->hdr.checksum_count > 0) {
        r->checksums = (uint64_t*)malloc(r->hdr.checksum_count * sizeof(uint64_t));
        if (r->checksums == NULL) {
            fclose(f);
            mvc2r_free(r);
            return 0;
        }
        r->checksum_cap = r->hdr.checksum_count;
        for (i = 0; i < r->hdr.checksum_count; i++) {
            uint8_t b[8];
            if (fread(b, 1, 8, f) != 8) {
                fclose(f);
                mvc2r_free(r);
                return 0;
            }
            r->checksums[i] = get_u64(b);
        }
    }
    fclose(f);
    return 1;
}

int mvc2r_compare_inputs(const MVC2R_FILE* a, const MVC2R_FILE* b) {
    if (a == NULL || b == NULL) return 0;
    if (a->hdr.duration_frames != b->hdr.duration_frames) return 0;
    if (a->hdr.checksum_count != b->hdr.checksum_count) return 0;
    if (a->hdr.duration_frames > 0 && memcmp(a->frames, b->frames, a->hdr.duration_frames * sizeof(uint32_t)) != 0) {
        return 0;
    }
    if (a->hdr.checksum_count > 0 && memcmp(a->checksums, b->checksums, a->hdr.checksum_count * sizeof(uint64_t)) != 0) {
        return 0;
    }
    return 1;
}

/* ---- Canonical Dreamcast Boot Helper ---- */

static void poll_match_into_hdr(MVC2R_HEADER* hdr) {
    const HOST_PROGRAM* prog = board_program();
    MVC2_MATCH_INFO mi;
    int side, s;
    if (prog == NULL || prog->match == NULL) return;
    prog->match->get_info(&mi);
    if (mi.in_fight || mi.match_over) {
        for (s = 0; s < 3; s++) {
            hdr->teams[0][s].char_id = mi.p1_char[s];
            hdr->teams[0][s].assist_type = mi.p1_assist[s];
            hdr->teams[0][s].color = mi.p1_color[s];
            hdr->teams[1][s].char_id = mi.p2_char[s];
            hdr->teams[1][s].assist_type = mi.p2_assist[s];
            hdr->teams[1][s].color = mi.p2_color[s];
        }
        for (side = 0; side < 2; side++) (void)side;
        hdr->stage_no = mi.stage_no;
        if (mi.match_over) hdr->winner_side = mi.winner_side;
    }
}

static int boot_dc_match_canonical(const char* patch_id, uint32_t seed, int64_t timestamp_utc, uint8_t start_mode) {
    const HOST_PROGRAM* prog;
    int boot = 0, k, saved_discard = rd_discard;
    if (patch_id != NULL && patch_id[0] != 0) {
        if (!patch_dll_activate(patch_id)) {
            return 0;
        }
    }
    host_restart();
    dc_vmu_set_netplay_lock(1);
    host_set_netplay_canonical(1);
    host_clock_set_override(1, timestamp_utc > 0 ? timestamp_utc : 946684800LL);
    if (!start_game()) {
        return 0;
    }
    prog = board_program();
    if (prog == NULL || prog->match == NULL) {
        return 0;
    }
    rd_discard = 1;
    while (!prog->match->is_ready() && boot < 600) {
        HOST_INPUT in;
        memset(&in, 0, sizeof in);
        host_set_input(&in);
        naomi_run_frame();
        sdl_audio_step_silent();
        boot++;
    }
    for (k = 0; k < 10; k++) {
        HOST_INPUT in;
        memset(&in, 0, sizeof in);
        host_set_input(&in);
        naomi_run_frame();
        sdl_audio_step_silent();
    }
    rd_discard = saved_discard;
    if (!prog->match->is_ready()) {
        return 0;
    }
    if (start_mode == MVC2R_START_CHAR_SELECT) {
        prog->match->start_versus(seed);
    } else {
        prog->match->start_training();
    }
    savestate_reset_pools();
    return 1;
}

/* ---- Active Recording ---- */

static MVC2R_FILE rec_file;
static char rec_path[1024];
static int rec_active;

int replay_rec_begin(const char* out_path, const char* patch_id, uint32_t match_seed, int64_t timestamp_utc,
                     uint8_t start_mode, uint8_t is_ranked, const char* p1_id, const char* p2_id,
                     const char* p1_name, const char* p2_name, uint16_t p1_rating, uint16_t p2_rating) {
    mvc2r_free(&rec_file);
    mvc2r_init(&rec_file);
    snprintf(rec_path, sizeof rec_path, "%s", out_path != NULL ? out_path : "match.mvc2r");
    snprintf(rec_file.hdr.patch_id, sizeof rec_file.hdr.patch_id, "%s",
             (patch_id != NULL && patch_id[0] != 0) ? patch_id : patch_dll_active_id());
    rec_file.hdr.match_seed = match_seed;
    rec_file.hdr.timestamp_utc = timestamp_utc > 0 ? timestamp_utc : 946684800LL;
    rec_file.hdr.is_ranked = is_ranked;
    rec_file.hdr.start_mode = start_mode;
    if (p1_id) snprintf(rec_file.hdr.p1_id, sizeof rec_file.hdr.p1_id, "%s", p1_id);
    if (p2_id) snprintf(rec_file.hdr.p2_id, sizeof rec_file.hdr.p2_id, "%s", p2_id);
    if (p1_name) snprintf(rec_file.hdr.p1_name, sizeof rec_file.hdr.p1_name, "%s", p1_name);
    if (p2_name) snprintf(rec_file.hdr.p2_name, sizeof rec_file.hdr.p2_name, "%s", p2_name);
    rec_file.hdr.p1_rating = p1_rating;
    rec_file.hdr.p2_rating = p2_rating;
    rec_active = 1;
    return 1;
}

int replay_rec_is_active(void) {
    return rec_active;
}

void replay_rec_push_frame(uint16_t p1, uint16_t p2, uint64_t state_hash) {
    uint32_t next_idx;
    if (!rec_active) return;
    next_idx = rec_file.hdr.duration_frames + 1u;
    if (rec_file.hdr.checksum_interval > 0 && (next_idx % rec_file.hdr.checksum_interval) == 0) {
        uint64_t h = state_hash != 0 ? state_hash : savestate_hash();
        mvc2r_push_frame(&rec_file, p1, p2, &h);
    } else {
        mvc2r_push_frame(&rec_file, p1, p2, NULL);
    }
    poll_match_into_hdr(&rec_file.hdr);
}

void replay_rec_poll_match_info(void) {
    if (rec_active) poll_match_into_hdr(&rec_file.hdr);
}

int replay_rec_finish_and_save(void) {
    int ok;
    if (!rec_active) return 0;
    poll_match_into_hdr(&rec_file.hdr);
    ok = mvc2r_save(&rec_file, rec_path);
    rec_active = 0;
    mvc2r_free(&rec_file);
    return ok;
}

/* ---- Playback, Keyframe Scrubbing & Replay Takeover Session ---- */

static struct {
    int active;
    MVC2R_FILE file;
    SAVESTATE** keyframes;
    uint32_t keyframe_count;
    SAVESTATE* takeover_checkpoint;
    uint32_t cur_frame;
    int paused;
    int step_once;
    int takeover_slot; /* -1, 0, or 1 */
    int32_t bookmark_frame;
    int32_t takeover_mark_frame;
    uint32_t checksum_mismatches;
    double last_seek_ms;
} sess;

static void step_one_frame_with_inputs(uint16_t p1, uint16_t p2, int silent_audio) {
    HOST_INPUT in;
    memset(&in, 0, sizeof in);
    in.player[0] = net_unpack_input(p1);
    in.player[1] = net_unpack_input(p2);
    host_set_input(&in);
    naomi_run_frame();
    if (silent_audio) sdl_audio_step_silent();
}

static int ensure_keyframe(uint32_t kf_idx) {
    if (kf_idx >= sess.keyframe_count) return 0;
    if (sess.keyframes[kf_idx] != NULL) return 1;
    sess.keyframes[kf_idx] = savestate_alloc();
    if (sess.keyframes[kf_idx] == NULL) return 0;
    return savestate_save(sess.keyframes[kf_idx]);
}

void replay_session_close(void) {
    uint32_t i;
    if (sess.keyframes != NULL) {
        for (i = 0; i < sess.keyframe_count; i++) {
            savestate_free(sess.keyframes[i]);
        }
        free(sess.keyframes);
    }
    savestate_free(sess.takeover_checkpoint);
    mvc2r_free(&sess.file);
    memset(&sess, 0, sizeof sess);
    sess.takeover_slot = -1;
    sess.bookmark_frame = -1;
    sess.takeover_mark_frame = -1;
}

int replay_session_open(const char* path) {
    replay_session_close();
    if (!mvc2r_load(&sess.file, path)) {
        host_log("replay: failed to open %s", path ? path : "(null)");
        return 0;
    }
    if (!boot_dc_match_canonical(sess.file.hdr.patch_id, sess.file.hdr.match_seed,
                                 sess.file.hdr.timestamp_utc, sess.file.hdr.start_mode)) {
        host_log("replay: failed to boot canonical patch %s", sess.file.hdr.patch_id);
        replay_session_close();
        return 0;
    }
    sess.keyframe_count = (sess.file.hdr.duration_frames / REPLAY_KEYFRAME_INTERVAL) + 1u;
    sess.keyframes = (SAVESTATE**)calloc(sess.keyframe_count, sizeof(SAVESTATE*));
    sess.takeover_checkpoint = savestate_alloc();
    if (sess.keyframes == NULL || sess.takeover_checkpoint == NULL || !ensure_keyframe(0)) {
        replay_session_close();
        return 0;
    }
    sess.cur_frame = 0;
    sess.paused = 0;
    sess.step_once = 0;
    sess.takeover_slot = -1;
    sess.bookmark_frame = -1;
    sess.takeover_mark_frame = -1;
    sess.active = 1;
    host_log("replay: opened %s (frames=%u patch=%s keyframes=%u)",
             path, sess.file.hdr.duration_frames, sess.file.hdr.patch_id, sess.keyframe_count);
    return 1;
}

int replay_session_is_active(void) {
    return sess.active;
}

int replay_session_seek(uint32_t target_frame, double* out_ms) {
    uint32_t kf_idx;
    int saved_discard;
    double t0;
    if (!sess.active) return 0;
    if (target_frame > sess.file.hdr.duration_frames) {
        target_frame = sess.file.hdr.duration_frames;
    }
    t0 = now_ms();
    kf_idx = target_frame / REPLAY_KEYFRAME_INTERVAL;
    while (kf_idx > 0 && sess.keyframes[kf_idx] == NULL) {
        kf_idx--;
    }
    if (!savestate_load(sess.keyframes[kf_idx])) {
        return 0;
    }
    sess.cur_frame = kf_idx * REPLAY_KEYFRAME_INTERVAL;
    saved_discard = rd_discard;
    rd_discard = 1;
    while (sess.cur_frame < target_frame) {
        uint32_t word = sess.file.frames[sess.cur_frame];
        uint16_t p1 = (uint16_t)(word & 0xFFFFu);
        uint16_t p2 = (uint16_t)(word >> 16);
        step_one_frame_with_inputs(p1, p2, 1);
        sess.cur_frame++;
        if ((sess.cur_frame % REPLAY_KEYFRAME_INTERVAL) == 0) {
            ensure_keyframe(sess.cur_frame / REPLAY_KEYFRAME_INTERVAL);
        }
    }
    rd_discard = saved_discard;
    sdl_audio_notify_rollback();
    sess.takeover_slot = -1;
    sess.last_seek_ms = now_ms() - t0;
    if (out_ms != NULL) *out_ms = sess.last_seek_ms;
    return 1;
}

int replay_session_step_frame(uint16_t live_p1, uint16_t live_p2) {
    uint16_t p1 = 0, p2 = 0;
    uint32_t word;
    if (!sess.active) return 0;
    if (sess.paused && !sess.step_once) {
        return 1;
    }
    sess.step_once = 0;
    if (sess.cur_frame >= sess.file.hdr.duration_frames) {
        if (sess.takeover_slot >= 0) {
            p1 = sess.takeover_slot == 0 ? live_p1 : 0;
            p2 = sess.takeover_slot == 1 ? live_p2 : 0;
            step_one_frame_with_inputs(p1, p2, host_is_headless());
            sess.cur_frame++;
            return 1;
        }
        sess.paused = 1;
        return 1;
    }
    word = sess.file.frames[sess.cur_frame];
    p1 = (sess.takeover_slot == 0) ? live_p1 : (uint16_t)(word & 0xFFFFu);
    p2 = (sess.takeover_slot == 1) ? live_p2 : (uint16_t)(word >> 16);
    step_one_frame_with_inputs(p1, p2, host_is_headless());
    sess.cur_frame++;
    if (sess.takeover_slot < 0) {
        if ((sess.cur_frame % REPLAY_KEYFRAME_INTERVAL) == 0) {
            ensure_keyframe(sess.cur_frame / REPLAY_KEYFRAME_INTERVAL);
        }
        if (sess.file.hdr.checksum_interval > 0 && (sess.cur_frame % sess.file.hdr.checksum_interval) == 0) {
            uint32_t cidx = (sess.cur_frame / sess.file.hdr.checksum_interval) - 1u;
            if (cidx < sess.file.hdr.checksum_count) {
                uint64_t h = savestate_hash();
                if (h != sess.file.checksums[cidx]) {
                    sess.checksum_mismatches++;
                    if (sess.checksum_mismatches == 1u) {
                        SAVESTATE_HASHES d;
                        savestate_hash_detail(&d);
                        host_log("REPLAY_HASH[verify f=%u]: ram=%016llX data=%016llX bss=%016llX coro=%016llX board=%016llX nl=%016llX tex=%016llX rd=%016llX dc=%016llX snd=%016llX aica_ram=%016llX",
                                 sess.cur_frame, (unsigned long long)d.ram, (unsigned long long)d.data,
                                 (unsigned long long)d.bss, (unsigned long long)d.coro, (unsigned long long)d.board,
                                 (unsigned long long)d.nl, (unsigned long long)d.tex, (unsigned long long)d.rd,
                                 (unsigned long long)d.dc, (unsigned long long)d.snd, (unsigned long long)d.aica_ram);
                    }
                    host_log("REPLAY DESYNC at frame=%u: got %016llX != expected %016llX",
                             sess.cur_frame, (unsigned long long)h,
                             (unsigned long long)sess.file.checksums[cidx]);
                }
            }
        }
    }
    return 1;
}

void replay_session_toggle_pause(void) {
    if (sess.active) sess.paused = !sess.paused;
}

void replay_session_step_once(void) {
    if (sess.active) {
        sess.paused = 1;
        sess.step_once = 1;
    }
}

void replay_session_set_bookmark(int32_t frame) {
    if (!sess.active) return;
    if (frame < 0) {
        sess.bookmark_frame = (int32_t)sess.cur_frame;
    } else if ((uint32_t)frame <= sess.file.hdr.duration_frames) {
        sess.bookmark_frame = frame;
    }
}

int replay_session_enter_takeover(int slot) {
    if (!sess.active || (slot != 0 && slot != 1)) return 0;
    if (sess.bookmark_frame >= 0 && (uint32_t)sess.bookmark_frame != sess.cur_frame) {
        if (!replay_session_seek((uint32_t)sess.bookmark_frame, NULL)) return 0;
    }
    if (!savestate_save(sess.takeover_checkpoint)) return 0;
    sess.takeover_mark_frame = (int32_t)sess.cur_frame;
    sess.takeover_slot = slot;
    sess.paused = 0;
    host_log("replay: takeover P%d at frame %d", slot + 1, sess.takeover_mark_frame);
    return 1;
}

int replay_session_reset_takeover(void) {
    if (!sess.active || sess.takeover_mark_frame < 0) return 0;
    if (!savestate_load(sess.takeover_checkpoint)) return 0;
    sdl_audio_notify_rollback();
    sess.cur_frame = (uint32_t)sess.takeover_mark_frame;
    if (sess.takeover_slot < 0) sess.takeover_slot = 0;
    sess.paused = 0;
    return 1;
}

void replay_session_exit_takeover(void) {
    if (!sess.active) return;
    sess.takeover_slot = -1;
}

void replay_session_get_status(REPLAY_STATUS* out) {
    if (out == NULL) return;
    memset(out, 0, sizeof *out);
    out->active = sess.active;
    if (!sess.active) {
        out->takeover_slot = -1;
        out->bookmark_frame = -1;
        out->takeover_mark_frame = -1;
        return;
    }
    out->cur_frame = sess.cur_frame;
    out->duration_frames = sess.file.hdr.duration_frames;
    out->paused = sess.paused;
    out->takeover_slot = sess.takeover_slot;
    out->bookmark_frame = sess.bookmark_frame;
    out->takeover_mark_frame = sess.takeover_mark_frame;
    out->checksum_mismatches = sess.checksum_mismatches;
    out->last_seek_ms = sess.last_seek_ms;
    snprintf(out->patch_id, sizeof out->patch_id, "%s", sess.file.hdr.patch_id);
    snprintf(out->p1_name, sizeof out->p1_name, "%s", sess.file.hdr.p1_name);
    snprintf(out->p2_name, sizeof out->p2_name, "%s", sess.file.hdr.p2_name);
}

/* ---- CLI / Automated Verification ---- */

static void scripted_match_inputs(int frame, uint16_t* out_p1, uint16_t* out_p2) {
    uint32_t p1 = 0, p2 = 0;
    if (frame < 300) {
        if (frame == 30) p1 = JVS_RIGHT;
        if (frame == 45 || frame == 75 || frame == 105) p1 = JVS_B1;
        if (frame == 55 || frame == 85 || frame == 115) p1 = JVS_B1;
        if (frame == 35) p2 = JVS_DOWN;
        if (frame == 50 || frame == 80 || frame == 110) p2 = JVS_B1;
        if (frame == 60 || frame == 90 || frame == 120) p2 = JVS_B1;
        if (frame == 145 || frame == 165) { p1 = JVS_B1; p2 = JVS_B1; }
    } else {
        int m = (frame - 300) % 90;
        if (m < 12) p1 |= JVS_RIGHT; else if (m < 24) p1 |= JVS_LEFT;
        if (m == 14) p1 |= HOST_DASH;
        if (m == 26) p1 |= HOST_SJUMP;
        if (m == 32) p1 |= JVS_DOWN | JVS_B1;
        if (m == 46) p1 |= JVS_B3;
        if (m >= 10 && m < 22) p2 |= JVS_LEFT; else if (m >= 22 && m < 34) p2 |= JVS_RIGHT;
        if (m == 18) p2 |= JVS_DOWN | JVS_B4;
        if (m == 36) p2 |= JVS_B6;
    }
    *out_p1 = net_pack_input(p1);
    *out_p2 = net_pack_input(p2);
}

int replay_run_record_cli(const char* out_path, int frames, uint32_t seed) {
    const char* patch = cli_get_opt("patch");
    int f;
    if (frames <= 0) frames = 900;
    if (patch == NULL || patch[0] == 0) patch = patch_dll_active_id();
    if (!boot_dc_match_canonical(patch, seed, 946684800LL, MVC2R_START_CHAR_SELECT)) {
        host_log("REPLAY_RECORD FAIL: boot_dc_match_canonical failed for patch=%s", patch);
        return 1;
    }
    replay_rec_begin(out_path, patch, seed, 946684800LL, MVC2R_START_CHAR_SELECT, 0,
                     "P-TEST0001", "P-TEST0002", "Player1", "Player2", 1500, 1500);
    rd_discard = 1;
    for (f = 0; f < frames; f++) {
        uint16_t p1, p2;
        scripted_match_inputs(f, &p1, &p2);
        step_one_frame_with_inputs(p1, p2, 1);
        replay_rec_push_frame(p1, p2, 0);
    }
    if (!replay_rec_finish_and_save()) {
        host_log("REPLAY_RECORD FAIL: failed to write %s", out_path);
        return 1;
    }
    host_log("REPLAY_RECORD PASS path=%s frames=%d patch=%s", out_path, frames, patch);
    return 0;
}

int replay_run_verify_cli(const char* path) {
    REPLAY_STATUS st;
    uint32_t f;
    if (!replay_session_open(path)) {
        host_log("REPLAY_VERIFY FAIL: open failed for %s", path ? path : "(null)");
        return 1;
    }
    rd_discard = 1;
    replay_session_get_status(&st);
    for (f = 0; f < st.duration_frames; f++) {
        if (!replay_session_step_frame(0, 0)) {
            host_log("REPLAY_VERIFY FAIL: step failed at %u", f);
            replay_session_close();
            return 1;
        }
    }
    replay_session_get_status(&st);
    replay_session_close();
    if (st.checksum_mismatches != 0) {
        host_log("REPLAY_VERIFY FAIL mismatches=%u path=%s", st.checksum_mismatches, path);
        return 1;
    }
    host_log("REPLAY_VERIFY PASS frames=%u checks=%u mismatches=0 patch=%s",
             st.duration_frames, st.duration_frames / REPLAY_CHECKSUM_INTERVAL, st.patch_id);
    return 0;
}

int replay_run_takeover_test_cli(const char* path) {
    REPLAY_STATUS st;
    double seek_ms_fwd = 0.0, seek_ms_back = 0.0, seek_ms_sub = 0.0;
    uint64_t h_orig_480, h_take_p1_run1, h_take_p1_run2, h_recorded_480;
    uint32_t i, f;

    if (!replay_session_open(path)) {
        host_log("TAKEOVER_TEST FAIL: failed to open %s", path ? path : "(null)");
        return 1;
    }
    rd_discard = 1;
    replay_session_get_status(&st);
    if (st.duration_frames < 500u) {
        host_log("TAKEOVER_TEST FAIL: replay too short (%u < 500)", st.duration_frames);
        replay_session_close();
        return 1;
    }
    for (f = 0; f < st.duration_frames; f++) {
        replay_session_step_frame(0, 0);
        if (f == 479u) h_recorded_480 = savestate_hash();
    }
    (void)h_recorded_480;

    /* Seek to keyframe-aligned 240 and worst-case unaligned 459 (19f fast-forward) */
    if (!replay_session_seek(480u, &seek_ms_fwd)) {
        replay_session_close();
        return 1;
    }
    h_orig_480 = savestate_hash();
    if (!replay_session_seek(240u, &seek_ms_back)) {
        replay_session_close();
        return 1;
    }
    if (!replay_session_seek(459u, &seek_ms_sub)) {
        replay_session_close();
        return 1;
    }
    if (seek_ms_sub >= 12.0) {
        host_log("TAKEOVER_TEST FAIL: sub-keyframe seek too slow (%.3f ms >= 12.0 ms)", seek_ms_sub);
        replay_session_close();
        return 1;
    }

    /* Enter Takeover P1 at frame 459, feed diverging P1 input for 21 frames to reach frame 480 */
    if (!replay_session_enter_takeover(0)) {
        host_log("TAKEOVER_TEST FAIL: enter_takeover failed");
        replay_session_close();
        return 1;
    }
    for (i = 0; i < 21u; i++) {
        replay_session_step_frame(net_pack_input(JVS_RIGHT | JVS_B3 | HOST_DASH), 0);
    }
    h_take_p1_run1 = savestate_hash();
    if (h_take_p1_run1 == h_orig_480) {
        host_log("TAKEOVER_TEST FAIL: live P1 input did not diverge from original state");
        replay_session_close();
        return 1;
    }

    /* Reset Takeover (F7) and verify identical live rerun produces identical hash */
    if (!replay_session_reset_takeover()) {
        host_log("TAKEOVER_TEST FAIL: reset_takeover failed");
        replay_session_close();
        return 1;
    }
    for (i = 0; i < 21u; i++) {
        replay_session_step_frame(net_pack_input(JVS_RIGHT | JVS_B3 | HOST_DASH), 0);
    }
    h_take_p1_run2 = savestate_hash();
    replay_session_close();

    if (h_take_p1_run1 != h_take_p1_run2) {
        host_log("TAKEOVER_TEST FAIL: reset_takeover determinism mismatch (%016llX != %016llX)",
                 (unsigned long long)h_take_p1_run1, (unsigned long long)h_take_p1_run2);
        return 1;
    }
    host_log("TAKEOVER_TEST PASS seek_aligned_ms=%.3f seek_sub_ms=%.3f diverged=1 reset_exact=1",
             seek_ms_back, seek_ms_sub);
    return 0;
}
