/*
 * ggpo_glue.h  Pure C bridge between pond3r/ggpo, COW savestates, audio crossfade & .mvc2r recording
 */

#ifndef GGPO_GLUE_H
#define GGPO_GLUE_H

#include <stdint.h>

typedef struct GGPO_GLUE_STATUS {
    int      active;
    int      synchronized;
    int      mode;             /* 0=none, 1=synctest, 2=p2p, 3=spectate */
    int      local_side;       /* 1 or 2 */
    int      ping_ms;
    int      frame_delay;
    int      rollback_frames;
    int      total_rollbacks;
    int      frames_run;
    int      confirmed_frames;
    int      desync_count;
    int      using_relay;
    int      disconnected;
} GGPO_GLUE_STATUS;

int  ggpo_glue_init_from_cli(void);
int  ggpo_glue_start_synctest(int check_distance, const char* record_path, uint32_t seed);
int  ggpo_glue_start_p2p(int side, uint16_t local_port, const char* peer_addr,
                         const char* relay_addr, uint32_t relay_token, int force_relay,
                         int frame_delay, uint32_t match_seed, const char* patch_id,
                         const char* record_path);
int  ggpo_glue_start_spectate(uint16_t local_port, const char* host_addr, uint32_t match_seed,
                              const char* patch_id, const char* record_path);
void ggpo_glue_stop(void);

int  ggpo_glue_is_active(void);
int  ggpo_glue_is_synchronized(void);
int  ggpo_glue_step_frame(uint32_t local_p1_jvs, uint32_t local_p2_jvs);
void ggpo_glue_finish_and_log(void);
int  ggpo_glue_exit_code(void);
void ggpo_glue_get_status(GGPO_GLUE_STATUS* out);

#endif
