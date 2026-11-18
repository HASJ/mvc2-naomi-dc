/*
 * replay.h  .mvc2r v1 binary replay recording, 120f keyframe scrubbing, and Replay Takeover
 */

#ifndef REPLAY_H
#define REPLAY_H

#include <stddef.h>
#include <stdint.h>

#define MVC2R_START_CHAR_SELECT 0u
#define MVC2R_START_FIGHT       1u
#define REPLAY_CHECKSUM_INTERVAL 60u
#define REPLAY_KEYFRAME_INTERVAL 20u

typedef struct MVC2R_TEAM_SLOT {
    uint8_t char_id;
    uint8_t assist_type;
    uint8_t color;
    uint8_t reserved;
} MVC2R_TEAM_SLOT;

typedef struct MVC2R_HEADER {
    char magic[6];
    char patch_id[32];
    uint32_t match_seed;
    int64_t timestamp_utc;
    uint32_t duration_frames;
    int8_t winner_side;
    uint8_t is_ranked;
    uint8_t start_mode;
    uint8_t stage_no;
    char p1_id[16];
    char p2_id[16];
    char p1_name[32];
    char p2_name[32];
    uint16_t p1_rating;
    uint16_t p2_rating;
    MVC2R_TEAM_SLOT teams[2][3];
    uint8_t rules[16];
    uint32_t checksum_interval;
    uint32_t checksum_count;
    uint32_t payload_bytes;
} MVC2R_HEADER;

typedef struct MVC2R_FILE {
    MVC2R_HEADER hdr;
    uint32_t* frames;       /* packed (p1_u16 | ((uint32_t)p2_u16 << 16)) per frame */
    uint32_t frame_cap;
    uint64_t* checksums;    /* savestate_hash() every checksum_interval frames */
    uint32_t checksum_cap;
} MVC2R_FILE;

typedef struct REPLAY_STATUS {
    int active;
    uint32_t cur_frame;
    uint32_t duration_frames;
    int paused;
    int takeover_slot;      /* -1 = none, 0 = P1, 1 = P2 */
    int32_t bookmark_frame; /* -1 = none */
    int32_t takeover_mark_frame;
    uint32_t checksum_mismatches;
    double last_seek_ms;
    char patch_id[32];
    char p1_name[32];
    char p2_name[32];
} REPLAY_STATUS;

void mvc2r_init(MVC2R_FILE* r);
void mvc2r_free(MVC2R_FILE* r);
int  mvc2r_push_frame(MVC2R_FILE* r, uint16_t p1, uint16_t p2, const uint64_t* opt_checksum);
int  mvc2r_encode_payload(const uint32_t* frames, uint32_t count, uint8_t** out_buf, uint32_t* out_len);
int  mvc2r_decode_payload(const uint8_t* buf, uint32_t len, uint32_t expected_frames, uint32_t** out_frames);
int  mvc2r_save(const MVC2R_FILE* r, const char* path);
int  mvc2r_load(MVC2R_FILE* r, const char* path);
int  mvc2r_compare_inputs(const MVC2R_FILE* a, const MVC2R_FILE* b);

/* Active recording helper */
int  replay_rec_begin(const char* out_path, const char* patch_id, uint32_t match_seed, int64_t timestamp_utc,
                      uint8_t start_mode, uint8_t is_ranked, const char* p1_id, const char* p2_id,
                      const char* p1_name, const char* p2_name, uint16_t p1_rating, uint16_t p2_rating);
int  replay_rec_is_active(void);
void replay_rec_push_frame(uint16_t p1, uint16_t p2, uint64_t state_hash);
void replay_rec_poll_match_info(void);
int  replay_rec_finish_and_save(void);

/* Playback, Scrubbing & Replay Takeover session */
int  replay_session_open(const char* path);
void replay_session_close(void);
int  replay_session_is_active(void);
int  replay_session_seek(uint32_t target_frame, double* out_ms);
int  replay_session_step_frame(uint16_t live_p1, uint16_t live_p2);
void replay_session_toggle_pause(void);
void replay_session_step_once(void);
void replay_session_set_bookmark(int32_t frame);
int  replay_session_enter_takeover(int slot);
int  replay_session_reset_takeover(void);
void replay_session_exit_takeover(void);
void replay_session_get_status(REPLAY_STATUS* out);

/* CLI / automated verification runners (return process exit code 0 on success) */
int  replay_run_record_cli(const char* out_path, int frames, uint32_t seed);
int  replay_run_verify_cli(const char* path);
int  replay_run_takeover_test_cli(const char* path);

#endif
