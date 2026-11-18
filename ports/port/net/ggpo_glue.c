/*
 * ggpo_glue.c  Pure C bridge wiring pond3r/ggpo to deterministic Dreamcast savestates,
 *              rollback audio crossfade, smooth clock micro-pacing, and .mvc2r recording.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif

#include "ggponet.h"
#include "ggpo_glue.h"
#include "net_transport.h"
#include "replay.h"
#include "savestate.h"
#include "patch_dll.h"
#include "naomi_host.h"
#include "naomi_input.h"

#define POOL_SLOTS 32
#define HASH_RING  64

extern int rd_discard;
extern void naomi_run_frame(void);
extern void sdl_audio_frame(void);
extern void sdl_audio_step_silent(void);
extern void sdl_audio_notify_rollback(void);
extern void pace_set_rate_scale(double scale);

enum {
    GG_MODE_NONE = 0,
    GG_MODE_SYNCTEST = 1,
    GG_MODE_P2P = 2,
    GG_MODE_SPECTATE = 3
};

static struct {
    int active;
    int synchronized;
    int disconnected;
    int mode;
    int local_side;
    int check_distance;
    int scripted_inputs;
    int target_frames;
    uint32_t match_seed;

    GGPOSession* session;
    GGPOPlayerHandle players[2];

    SAVESTATE* pool[POOL_SLOTS];
    uint8_t slot_used[POOL_SLOTS];

    /* Ring indexed by frame & (HASH_RING - 1) storing 64-bit savestate_hash at multiples of 60 */
    uint32_t h_frame[HASH_RING];
    uint64_t h_val[HASH_RING];
    SAVESTATE_HASHES h_detail[HASH_RING];

    /* Peer checksum ring awaiting comparison (in case packet arrives out-of-order/before confirm) */
    uint32_t peer_cs_frame[HASH_RING];
    uint64_t peer_cs_val[HASH_RING];
    uint8_t  peer_cs_valid[HASH_RING];
    uint64_t local_cs_val[HASH_RING];
    uint32_t local_cs_frame[HASH_RING];
    uint8_t  local_cs_valid[HASH_RING];

    int sim_frame;        /* logical GGPO frame during forward + rollback resim */
    int forward_frames;   /* confirmed/local forward steps */
    int confirmed_frames;
    int rollbacks;
    int current_rb_depth;
    int in_rollback_sim;
    int desyncs;
    uint32_t cs_sent;
    uint32_t cs_verified;
    int rec_enabled;
    int pace_ticks_left;
    double pace_delta;
    uint32_t init_start_ms;
    uint32_t last_idle_pace_ms;
} gg;

static int last_exit_code = 0;

static uint32_t gg_now_ms(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (uint32_t)((now.QuadPart * 1000ULL) / (uint64_t)freq.QuadPart);
#else
    return 0;
#endif
}

static int is_pool_ptr(const void* p) {
    int i;
    for (i = 0; i < POOL_SLOTS; i++) {
        if (gg.pool[i] != NULL && (const void*)gg.pool[i] == p) {
            return i;
        }
    }
    return -1;
}

static void record_60f_hash(int frame) {
    uint32_t slot = ((uint32_t)frame / REPLAY_CHECKSUM_INTERVAL) & (HASH_RING - 1u);
    savestate_hash_detail(&gg.h_detail[slot]);
    gg.h_frame[slot] = (uint32_t)frame;
    gg.h_val[slot] = gg.h_detail[slot].total;
}

static uint64_t lookup_60f_hash(int frame) {
    uint32_t slot = ((uint32_t)frame / REPLAY_CHECKSUM_INTERVAL) & (HASH_RING - 1u);
    if (gg.h_frame[slot] != (uint32_t)frame) {
        record_60f_hash(frame);
    }
    return gg.h_val[slot];
}

static void log_desync_hashes(uint32_t frame, uint64_t local_h, uint64_t peer_h) {
    uint32_t slot = (frame / REPLAY_CHECKSUM_INTERVAL) & (HASH_RING - 1u);
    host_log("NETPLAY_DESYNC frame=%u local=%016llX != peer=%016llX",
             frame, (unsigned long long)local_h, (unsigned long long)peer_h);
    if (gg.h_frame[slot] == frame) {
        const SAVESTATE_HASHES* d = &gg.h_detail[slot];
        host_log("NETPLAY_HASH[side=%d f=%u]: ram=%016llX data=%016llX bss=%016llX coro=%016llX board=%016llX nl=%016llX tex=%016llX rd=%016llX dc=%016llX snd=%016llX aica_ram=%016llX (aica=%016llX arm7=%016llX snl=%016llX s3l=%016llX)",
                 gg.local_side, frame, (unsigned long long)d->ram, (unsigned long long)d->data,
                 (unsigned long long)d->bss, (unsigned long long)d->coro, (unsigned long long)d->board,
                 (unsigned long long)d->nl, (unsigned long long)d->tex, (unsigned long long)d->rd,
                 (unsigned long long)d->dc, (unsigned long long)d->snd, (unsigned long long)d->aica_ram,
                 (unsigned long long)d->snd_aica, (unsigned long long)d->snd_arm7,
                 (unsigned long long)d->snd_nl, (unsigned long long)d->snd_s3l);
    }
}

static void check_or_store_local_cs(uint32_t frame, uint64_t h) {
    uint32_t slot = (frame / REPLAY_CHECKSUM_INTERVAL) & (HASH_RING - 1u);
    gg.cs_sent++;
    gg.local_cs_frame[slot] = frame;
    gg.local_cs_val[slot] = h;
    gg.local_cs_valid[slot] = 1;
    if (gg.peer_cs_valid[slot] && gg.peer_cs_frame[slot] == frame) {
        gg.cs_verified++;
        if (gg.peer_cs_val[slot] != h) {
            gg.desyncs++;
            log_desync_hashes(frame, h, gg.peer_cs_val[slot]);
        }
        gg.peer_cs_valid[slot] = 0;
    }
}

static void check_or_store_peer_cs(uint32_t frame, uint64_t h) {
    uint32_t slot = (frame / REPLAY_CHECKSUM_INTERVAL) & (HASH_RING - 1u);
    if (gg.local_cs_valid[slot] && gg.local_cs_frame[slot] == frame) {
        gg.cs_verified++;
        if (gg.local_cs_val[slot] != h) {
            gg.desyncs++;
            log_desync_hashes(frame, gg.local_cs_val[slot], h);
        }
        return;
    }
    gg.peer_cs_frame[slot] = frame;
    gg.peer_cs_val[slot] = h;
    gg.peer_cs_valid[slot] = 1;
}

/* ---- GGPO callbacks ---- */

static bool __cdecl cb_begin_game(const char* game) {
    (void)game;
    return true;
}

static bool __cdecl cb_save_game_state(unsigned char** buffer, int* len, int* checksum, int frame) {
    int i, idx = -1;
    uint64_t fast_h;
    for (i = 0; i < POOL_SLOTS; i++) {
        if (!gg.slot_used[i] && gg.pool[i] != NULL) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        host_log("ggpo_glue: savestate pool exhausted at frame %d!", frame);
        return false;
    }
    if (!savestate_save(gg.pool[idx])) {
        return false;
    }
    gg.slot_used[idx] = 1;
    fast_h = savestate_fast_checksum(gg.pool[idx]);
    *buffer = (unsigned char*)gg.pool[idx];
    *len = (int)sizeof(SAVESTATE*);
    *checksum = (int)(fast_h ^ (fast_h >> 32));
    if (frame > 0 && (frame % (int)REPLAY_CHECKSUM_INTERVAL) == 0) {
        record_60f_hash(frame);
    }
    gg.sim_frame = frame;
    return true;
}

static bool __cdecl cb_load_game_state(unsigned char* buffer, int len) {
    int idx = is_pool_ptr(buffer);
    (void)len;
    if (idx < 0) {
        return false;
    }
    if (!savestate_load(gg.pool[idx])) {
        return false;
    }
    sdl_audio_notify_rollback();
    return true;
}

static bool __cdecl cb_log_game_state(char* filename, unsigned char* buffer, int len) {
    int idx = is_pool_ptr(buffer);
    FILE* f = fopen(filename, "w");
    (void)len;
    if (f) {
        fprintf(f, "frame=%d fast_checksum=%016llX\n",
                gg.sim_frame,
                idx >= 0 ? (unsigned long long)savestate_fast_checksum(gg.pool[idx]) : 0ULL);
        fclose(f);
    }
    return true;
}

static void __cdecl cb_free_buffer(void* buffer) {
    int idx = is_pool_ptr(buffer);
    if (idx >= 0) {
        gg.slot_used[idx] = 0;
    }
}

static bool __cdecl cb_advance_frame(int flags) {
    uint16_t inputs[2] = { 0, 0 };
    int disconnect_flags = 0;
    HOST_INPUT in;
    int saved_discard = rd_discard;
    (void)flags;

    ggpo_synchronize_input(gg.session, inputs, sizeof inputs, &disconnect_flags);
    memset(&in, 0, sizeof in);
    in.player[0] = net_unpack_input(inputs[0]);
    in.player[1] = net_unpack_input(inputs[1]);
    host_set_input(&in);

    rd_discard = 1;
    gg.in_rollback_sim = 1;
    naomi_run_frame();
    sdl_audio_step_silent();
    gg.in_rollback_sim = 0;
    rd_discard = saved_discard;

    gg.current_rb_depth++;
    ggpo_advance_frame(gg.session);
    return true;
}

static bool __cdecl cb_on_event(GGPOEvent* info) {
    switch (info->code) {
    case GGPO_EVENTCODE_CONNECTED_TO_PEER:
        host_log("ggpo: connected to peer (player=%d)", info->u.connected.player);
        break;
    case GGPO_EVENTCODE_SYNCHRONIZING_WITH_PEER:
        host_log("ggpo: synchronizing %d/%d", info->u.synchronizing.count, info->u.synchronizing.total);
        break;
    case GGPO_EVENTCODE_SYNCHRONIZED_WITH_PEER:
        host_log("ggpo: synchronized with peer");
        break;
    case GGPO_EVENTCODE_RUNNING:
        gg.synchronized = 1;
        host_log("ggpo: session RUNNING (mode=%d side=%d)", gg.mode, gg.local_side);
        break;
    case GGPO_EVENTCODE_TIMESYNC:
        if (info->u.timesync.frames_ahead > 0) {
            /* Smoothly adjust frame pacing over 45 frames rather than halting whole frames */
            double d = -0.025 * (double)info->u.timesync.frames_ahead;
            if (d < -0.05) d = -0.05;
            gg.pace_delta = d;
            gg.pace_ticks_left = 45;
        }
        break;
    case GGPO_EVENTCODE_DISCONNECTED_FROM_PEER:
        gg.disconnected = 1;
        host_log("ggpo: disconnected from peer");
        break;
    case GGPO_EVENTCODE_CONNECTION_INTERRUPTED:
        host_log("ggpo: connection interrupted (timeout=%dms)", info->u.connection_interrupted.disconnect_timeout);
        break;
    case GGPO_EVENTCODE_CONNECTION_RESUMED:
        host_log("ggpo: connection resumed");
        break;
    default:
        break;
    }
    return true;
}

static void __cdecl on_confirmed_frame(int frame, const void* inputs, int input_bytes,
                                        int disconnect_flags, void* user_data) {
    const uint16_t* w = (const uint16_t*)inputs;
    uint16_t p1 = (input_bytes >= 2) ? w[0] : 0;
    uint16_t p2 = (input_bytes >= 4) ? w[1] : 0;
    uint32_t next_idx = (uint32_t)frame + 1u;
    (void)disconnect_flags;
    (void)user_data;

    gg.confirmed_frames = (int)next_idx;
    if ((next_idx % REPLAY_CHECKSUM_INTERVAL) == 0u) {
        uint64_t h64 = lookup_60f_hash((int)next_idx);
        if (gg.mode == GG_MODE_P2P) {
            check_or_store_local_cs(next_idx, h64);
            net_transport_send_checksum(next_idx, h64);
        }
        if (gg.rec_enabled) {
            replay_rec_push_frame(p1, p2, h64);
        }
    } else if (gg.rec_enabled) {
        replay_rec_push_frame(p1, p2, 0ULL);
    }
}

static void drain_peer_checksums(void) {
    uint32_t f = 0;
    uint64_t h = 0;
    while (net_transport_poll_checksum(&f, &h)) {
        check_or_store_peer_cs(f, h);
    }
}

static void alloc_pool(void) {
    int i;
    for (i = 0; i < POOL_SLOTS; i++) {
        gg.pool[i] = savestate_alloc();
        gg.slot_used[i] = 0;
    }
}

static void free_pool(void) {
    int i;
    for (i = 0; i < POOL_SLOTS; i++) {
        savestate_free(gg.pool[i]);
        gg.pool[i] = NULL;
        gg.slot_used[i] = 0;
    }
}

static void fill_callbacks(GGPOSessionCallbacks* cb) {
    memset(cb, 0, sizeof *cb);
    cb->begin_game = cb_begin_game;
    cb->save_game_state = cb_save_game_state;
    cb->load_game_state = cb_load_game_state;
    cb->log_game_state = cb_log_game_state;
    cb->free_buffer = cb_free_buffer;
    cb->advance_frame = cb_advance_frame;
    cb->on_event = cb_on_event;
}

int ggpo_glue_start_synctest(int check_distance, const char* record_path, uint32_t seed) {
    GGPOSessionCallbacks cb;
    GGPOPlayer p1, p2;
    const char* patch = cli_get_opt("patch");
    char game_name[] = "umvsc2";
    ggpo_glue_stop();

    if (check_distance <= 0) check_distance = 1;
    if (check_distance > 8) check_distance = 8;

    if (!replay_boot_canonical_match(patch && patch[0] ? patch : patch_dll_active_id(),
                                     seed, 946684800LL, MVC2R_START_CHAR_SELECT)) {
        return 0;
    }

    memset(&gg, 0, sizeof gg);
    alloc_pool();
    fill_callbacks(&cb);
    gg.mode = GG_MODE_SYNCTEST;
    gg.check_distance = check_distance;
    gg.local_side = 1;
    gg.match_seed = seed;
    gg.scripted_inputs = (getenv("MVC2_PRESS") == NULL) ? 1 : 0;

    if (record_path != NULL && record_path[0] != 0) {
        replay_rec_begin(record_path, patch_dll_active_id(), seed, 946684800LL, MVC2R_START_CHAR_SELECT,
                         0, "P-SYNC0001", "P-SYNC0002", "SyncP1", "SyncP2", 1500, 1500);
        gg.rec_enabled = 1;
    }

    ggpo_set_transport_callbacks(NULL);
    if (ggpo_start_synctest(&gg.session, &cb, game_name, 2, (int)sizeof(uint16_t), check_distance) != GGPO_OK) {
        free_pool();
        return 0;
    }

    memset(&p1, 0, sizeof p1);
    p1.size = sizeof p1; p1.type = GGPO_PLAYERTYPE_LOCAL; p1.player_num = 1;
    memset(&p2, 0, sizeof p2);
    p2.size = sizeof p2; p2.type = GGPO_PLAYERTYPE_LOCAL; p2.player_num = 2;
    ggpo_add_player(gg.session, &p1, &gg.players[0]);
    ggpo_add_player(gg.session, &p2, &gg.players[1]);

    gg.active = 1;
    gg.init_start_ms = gg_now_ms();
    ggpo_idle(gg.session, 0);
    return 1;
}

int ggpo_glue_start_p2p(int side, uint16_t local_port, const char* peer_addr,
                        const char* relay_addr, uint32_t relay_token, int force_relay,
                        int frame_delay, uint32_t match_seed, const char* patch_id,
                        const char* record_path) {
    NET_TRANSPORT_CFG tcfg;
    GGPOTransportCallbacks tcb;
    GGPOSessionCallbacks cb;
    GGPOPlayer local_pl, remote_pl;
    uint32_t peer_ip_be = 0;
    uint16_t peer_port_be = 0;
    struct in_addr ina;
    const char* use_peer = (peer_addr && peer_addr[0]) ? peer_addr : relay_addr;
    const char* use_patch = (patch_id && patch_id[0]) ? patch_id : cli_get_opt("patch");
    int remote_side = (side == 1) ? 2 : 1;

    ggpo_glue_stop();
    if (side != 1 && side != 2) return 0;
    if (!net_transport_parse_endpoint(use_peer, &peer_ip_be, &peer_port_be)) {
        host_log("ggpo_glue: invalid peer address %s", use_peer ? use_peer : "(null)");
        return 0;
    }
    if (!replay_boot_canonical_match(use_patch && use_patch[0] ? use_patch : patch_dll_active_id(),
                                     match_seed, 946684800LL, MVC2R_START_CHAR_SELECT)) {
        return 0;
    }

    srand((unsigned)GetCurrentProcessId() ^ (unsigned)side * 7919u ^ match_seed);

    memset(&tcfg, 0, sizeof tcfg);
    tcfg.local_port = local_port;
    tcfg.peer_addr = use_peer;
    tcfg.relay_addr = relay_addr;
    tcfg.relay_token = relay_token ? relay_token : (match_seed ? match_seed : 0x4D564332u);
    tcfg.side = (uint8_t)side;
    tcfg.force_relay = force_relay;
    if (!net_transport_open(&tcfg)) {
        return 0;
    }
    net_transport_punch();

    memset(&tcb, 0, sizeof tcb);
    tcb.send_to = net_transport_ggpo_send;
    tcb.recv_from = net_transport_ggpo_recv;
    ggpo_set_transport_callbacks(&tcb);

    memset(&gg, 0, sizeof gg);
    alloc_pool();
    fill_callbacks(&cb);
    gg.mode = GG_MODE_P2P;
    gg.local_side = side;
    gg.match_seed = match_seed;
    gg.scripted_inputs = cli_has_flag("scripted-inputs");

    if (record_path != NULL && record_path[0] != 0) {
        replay_rec_begin(record_path, patch_dll_active_id(), match_seed, 946684800LL, MVC2R_START_CHAR_SELECT,
                         0, "P-NETP0001", "P-NETP0002", "NetP1", "NetP2", 1500, 1500);
        gg.rec_enabled = 1;
    }

    if (ggpo_start_session(&gg.session, &cb, "umvsc2", 2, (int)sizeof(uint16_t), local_port) != GGPO_OK) {
        free_pool();
        net_transport_close();
        return 0;
    }
    ggpo_set_confirmed_callback(gg.session, on_confirmed_frame, NULL);
    ggpo_set_disconnect_timeout(gg.session, 8000);
    ggpo_set_disconnect_notify_start(gg.session, 2000);

    ina.s_addr = peer_ip_be;
    memset(&local_pl, 0, sizeof local_pl);
    local_pl.size = sizeof local_pl;
    local_pl.type = GGPO_PLAYERTYPE_LOCAL;
    local_pl.player_num = side;

    memset(&remote_pl, 0, sizeof remote_pl);
    remote_pl.size = sizeof remote_pl;
    remote_pl.type = GGPO_PLAYERTYPE_REMOTE;
    remote_pl.player_num = remote_side;
    snprintf(remote_pl.u.remote.ip_address, sizeof remote_pl.u.remote.ip_address, "%s", inet_ntoa(ina));
    remote_pl.u.remote.port = ntohs(peer_port_be);

    if (side == 1) {
        ggpo_add_player(gg.session, &local_pl, &gg.players[0]);
        ggpo_add_player(gg.session, &remote_pl, &gg.players[1]);
        ggpo_set_frame_delay(gg.session, gg.players[0], frame_delay);
    } else {
        ggpo_add_player(gg.session, &remote_pl, &gg.players[0]);
        ggpo_add_player(gg.session, &local_pl, &gg.players[1]);
        ggpo_set_frame_delay(gg.session, gg.players[1], frame_delay);
    }

    gg.active = 1;
    gg.init_start_ms = gg_now_ms();
    return 1;
}

int ggpo_glue_start_spectate(uint16_t local_port, const char* host_addr, uint32_t match_seed,
                             const char* patch_id, const char* record_path) {
    NET_TRANSPORT_CFG tcfg;
    GGPOTransportCallbacks tcb;
    GGPOSessionCallbacks cb;
    uint32_t ip_be = 0;
    uint16_t port_be = 0;
    struct in_addr ina;
    char host_ip[32];
    const char* use_patch = (patch_id && patch_id[0]) ? patch_id : cli_get_opt("patch");

    ggpo_glue_stop();
    if (!net_transport_parse_endpoint(host_addr, &ip_be, &port_be)) return 0;
    if (!replay_boot_canonical_match(use_patch && use_patch[0] ? use_patch : patch_dll_active_id(),
                                     match_seed, 946684800LL, MVC2R_START_CHAR_SELECT)) {
        return 0;
    }

    memset(&tcfg, 0, sizeof tcfg);
    tcfg.local_port = local_port;
    tcfg.peer_addr = host_addr;
    tcfg.side = 1;
    if (!net_transport_open(&tcfg)) return 0;

    memset(&tcb, 0, sizeof tcb);
    tcb.send_to = net_transport_ggpo_send;
    tcb.recv_from = net_transport_ggpo_recv;
    ggpo_set_transport_callbacks(&tcb);

    memset(&gg, 0, sizeof gg);
    alloc_pool();
    fill_callbacks(&cb);
    gg.mode = GG_MODE_SPECTATE;
    gg.match_seed = match_seed;

    ina.s_addr = ip_be;
    snprintf(host_ip, sizeof host_ip, "%s", inet_ntoa(ina));
    if (record_path != NULL && record_path[0] != 0) {
        replay_rec_begin(record_path, patch_dll_active_id(), match_seed, 946684800LL, MVC2R_START_CHAR_SELECT,
                         0, "P-SPEC0001", "P-SPEC0002", "SpecP1", "SpecP2", 1500, 1500);
        gg.rec_enabled = 1;
    }
    if (ggpo_start_spectating(&gg.session, &cb, "umvsc2", 2, (int)sizeof(uint16_t),
                              local_port, host_ip, ntohs(port_be)) != GGPO_OK) {
        free_pool();
        net_transport_close();
        return 0;
    }
    ggpo_set_confirmed_callback(gg.session, on_confirmed_frame, NULL);
    gg.active = 1;
    gg.init_start_ms = gg_now_ms();
    return 1;
}

void ggpo_glue_stop(void) {
    if (!gg.active) return;
    if (gg.rec_enabled && replay_rec_is_active()) {
        replay_rec_finish_and_save();
    }
    if (gg.session != NULL) {
        ggpo_close_session(gg.session);
        gg.session = NULL;
    }
    ggpo_set_transport_callbacks(NULL);
    net_transport_close();
    free_pool();
    pace_set_rate_scale(1.0);
    memset(&gg, 0, sizeof gg);
}

int ggpo_glue_is_active(void) {
    return gg.active;
}

int ggpo_glue_is_synchronized(void) {
    return gg.active && gg.synchronized;
}

int ggpo_glue_step_frame(uint32_t local_p1_jvs, uint32_t local_p2_jvs) {
    uint16_t local16[2];
    uint16_t synced[2] = { 0, 0 };
    int disconnect_flags = 0;
    HOST_INPUT in;
    GGPOErrorCode err;
    uint32_t wait_start;

    if (!gg.active || gg.disconnected) return 0;

    /* Decay smooth clock-drift micro-pacing toward 1.0 */
    if (!host_is_headless()) {
        if (gg.pace_ticks_left > 0) {
            pace_set_rate_scale(1.0 + gg.pace_delta);
            gg.pace_ticks_left--;
        } else {
            pace_set_rate_scale(1.0);
        }
    }

    net_transport_tick();
    ggpo_idle(gg.session, 0);
    drain_peer_checksums();

    if (!gg.synchronized) {
        if (gg_now_ms() - gg.init_start_ms > 20000u) {
            host_log("ggpo_glue: synchronization timed out after 20s");
            gg.disconnected = 1;
            return 0;
        }
#ifdef _WIN32
        Sleep(1);
#endif
        return 1;
    }

    if (gg.scripted_inputs) {
        replay_scripted_match_inputs(gg.forward_frames, &local16[0], &local16[1]);
    } else {
        local16[0] = net_pack_input(local_p1_jvs);
        local16[1] = net_pack_input(local_p2_jvs);
    }

    if (gg.mode == GG_MODE_SYNCTEST) {
        ggpo_add_local_input(gg.session, gg.players[0], &local16[0], sizeof local16[0]);
        ggpo_add_local_input(gg.session, gg.players[1], &local16[1], sizeof local16[1]);
    } else if (gg.mode == GG_MODE_P2P) {
        int my_idx = (gg.local_side == 1) ? 0 : 1;
        uint16_t my_in = local16[my_idx];
        wait_start = gg_now_ms();
        for (;;) {
            err = ggpo_add_local_input(gg.session, gg.players[my_idx], &my_in, sizeof my_in);
            if (err == GGPO_OK) break;
            if (err != GGPO_ERRORCODE_PREDICTION_THRESHOLD) {
                host_log("ggpo_glue: add_local_input failed err=%d", (int)err);
                return 0;
            }
            if (gg_now_ms() - wait_start > 15000u || gg.disconnected) {
                host_log("ggpo_glue: prediction barrier timeout");
                gg.disconnected = 1;
                return 0;
            }
            net_transport_tick();
            ggpo_idle(gg.session, 1);
            drain_peer_checksums();
#ifdef _WIN32
            Sleep(1);
#endif
        }
    }

    err = ggpo_synchronize_input(gg.session, synced, sizeof synced, &disconnect_flags);
    if (err == GGPO_ERRORCODE_PREDICTION_THRESHOLD || err == GGPO_ERRORCODE_NOT_SYNCHRONIZED) {
#ifdef _WIN32
        Sleep(1);
#endif
        return 1;
    }
    if (!GGPO_SUCCEEDED(err)) {
        return 0;
    }

    memset(&in, 0, sizeof in);
    in.player[0] = net_unpack_input(synced[0]);
    in.player[1] = net_unpack_input(synced[1]);
    host_set_input(&in);

    gg.current_rb_depth = 0;
    naomi_run_frame();
    if (!host_is_headless()) {
        sdl_audio_frame();
    } else {
        sdl_audio_step_silent();
    }
    ggpo_advance_frame(gg.session);
    if (gg.current_rb_depth > 0) {
        gg.rollbacks++;
    }
    if (gg.mode == GG_MODE_SYNCTEST && gg.rec_enabled) {
        replay_rec_push_frame(synced[0], synced[1], 0ULL);
    }
    gg.forward_frames++;
    drain_peer_checksums();
    return 1;
}

void ggpo_glue_finish_and_log(void) {
    uint32_t sent = 0, recv = 0, relayed = 0, dropped = 0;
    if (!gg.active) return;

    /* Allow final frames & 60f peer checksums to confirm before closing session */
    if (gg.mode == GG_MODE_P2P && !gg.disconnected) {
        uint32_t start = gg_now_ms();
        while ((gg.confirmed_frames < gg.forward_frames || gg.cs_verified < gg.cs_sent) &&
               (gg_now_ms() - start) < 2500u && !gg.disconnected) {
            net_transport_tick();
            ggpo_idle(gg.session, 2);
            drain_peer_checksums();
#ifdef _WIN32
            Sleep(2);
#endif
        }
        /* Extra 200ms courtesy tail so peer also receives final ACK/checksum */
        start = gg_now_ms();
        while ((gg_now_ms() - start) < 200u && !gg.disconnected) {
            net_transport_tick();
            ggpo_idle(gg.session, 2);
            drain_peer_checksums();
#ifdef _WIN32
            Sleep(2);
#endif
        }
    }

    net_transport_stats(&sent, &recv, &relayed, &dropped);
    last_exit_code = (gg.desyncs == 0 && !gg.disconnected && gg.forward_frames > 0) ? 0 : 1;
    if (gg.mode == GG_MODE_SYNCTEST) {
        host_log("SYNCTEST PASS frames=%d check_distance=%d rollbacks=%d mismatches=%d",
                 gg.forward_frames, gg.check_distance, gg.rollbacks, gg.desyncs);
    } else if (gg.mode == GG_MODE_P2P) {
        if (last_exit_code == 0 && gg.confirmed_frames >= gg.forward_frames) {
            host_log("NETPLAY_P2P PASS side=%d frames=%d confirmed=%d checks=%u/%u rollbacks=%d relayed=%u dropped=%u desyncs=0",
                     gg.local_side, gg.forward_frames, gg.confirmed_frames,
                     gg.cs_verified, gg.cs_sent, gg.rollbacks, relayed, dropped);
        } else {
            last_exit_code = 1;
            host_log("NETPLAY_P2P FAIL side=%d frames=%d confirmed=%d checks=%u/%u disconnected=%d desyncs=%d",
                     gg.local_side, gg.forward_frames, gg.confirmed_frames,
                     gg.cs_verified, gg.cs_sent, gg.disconnected, gg.desyncs);
        }
    } else if (gg.mode == GG_MODE_SPECTATE) {
        host_log("NETPLAY_SPECTATE PASS frames=%d desyncs=%d", gg.forward_frames, gg.desyncs);
    }

    if (gg.rec_enabled && replay_rec_is_active()) {
        replay_rec_finish_and_save();
    }
    ggpo_glue_stop();
}

int ggpo_glue_exit_code(void) {
    return last_exit_code;
}

void ggpo_glue_get_status(GGPO_GLUE_STATUS* out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->active = gg.active;
    out->synchronized = gg.synchronized;
    out->mode = gg.mode;
    out->local_side = gg.local_side;
    out->rollback_frames = gg.current_rb_depth;
    out->total_rollbacks = gg.rollbacks;
    out->frames_run = gg.forward_frames;
    out->confirmed_frames = gg.confirmed_frames;
    out->desync_count = gg.desyncs;
    out->using_relay = net_transport_using_relay();
    out->disconnected = gg.disconnected;
    if (gg.active && gg.mode == GG_MODE_P2P && gg.synchronized && gg.session != NULL) {
        GGPONetworkStats st;
        int remote_idx = (gg.local_side == 1) ? 1 : 0;
        memset(&st, 0, sizeof st);
        if (ggpo_get_network_stats(gg.session, gg.players[remote_idx], &st) == GGPO_OK) {
            out->ping_ms = st.network.ping;
        }
    }
}

int ggpo_glue_init_from_cli(void) {
    const char* sync_opt = cli_get_opt("synctest");
    const char* sync_env = getenv("MVC2_SYNCTEST");
    const char* p2p_side = cli_get_opt_n("p2p", 0);
    const char* p2p_port = cli_get_opt_n("p2p", 1);
    const char* p2p_peer = cli_get_opt_n("p2p", 2);
    const char* spec_port = cli_get_opt_n("spectate", 0);
    const char* spec_host = cli_get_opt_n("spectate", 1);
    const char* rec = cli_get_opt("record");
    const char* seed_opt = cli_get_opt("match-seed");
    uint32_t seed = seed_opt ? (uint32_t)strtoul(seed_opt, NULL, 0) : 0x12345678u;
    if (!seed_opt && cli_get_opt("seed")) {
        seed = (uint32_t)strtoul(cli_get_opt("seed"), NULL, 0);
    }

    if (sync_opt != NULL || sync_env != NULL) {
        int dist = sync_opt ? atoi(sync_opt) : atoi(sync_env);
        if (dist <= 0) dist = 1;
        return ggpo_glue_start_synctest(dist, rec, seed) ? 1 : -1;
    }

    if (p2p_side != NULL && p2p_port != NULL && p2p_peer != NULL) {
        int side = atoi(p2p_side);
        uint16_t lport = (uint16_t)atoi(p2p_port);
        const char* relay = cli_get_opt("relay");
        const char* tok_opt = cli_get_opt("relay-token");
        const char* delay_opt = cli_get_opt("delay");
        uint32_t tok = tok_opt ? (uint32_t)strtoul(tok_opt, NULL, 0) : 0x4D564332u;
        int delay = delay_opt ? atoi(delay_opt) : 1;
        int force_relay = cli_has_flag("force-relay");
        return ggpo_glue_start_p2p(side, lport, p2p_peer, relay, tok, force_relay,
                                   delay, seed, cli_get_opt("patch"), rec) ? 1 : -1;
    }

    if (spec_port != NULL && spec_host != NULL) {
        uint16_t lport = (uint16_t)atoi(spec_port);
        return ggpo_glue_start_spectate(lport, spec_host, seed, cli_get_opt("patch"), rec) ? 1 : -1;
    }

    return 0;
}
