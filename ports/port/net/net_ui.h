/*
 * net_ui.h  Native F4 SDL Netplay Overlay, 6-Input Arcade PIN Pad, Ranked Standby, Lobbies,
 *           Leaderboards, Replay DB Search, Verified Patch Auto-Fetch, and F8 Replay HUD
 */

#ifndef NET_UI_H
#define NET_UI_H

#include <stdint.h>
#include "sdl_front.h"

int  net_ui_init(void);
void net_ui_shutdown(void);

int  net_ui_is_open(void);
int  net_ui_has_hud(void);
int  net_ui_event(const SDL_Event* e);
void net_ui_tick(void);

void net_ui_draw_overlay(uint32_t* frame, int w, int h);
void net_ui_draw_hud(uint32_t* argb, int w, int h);

void net_ui_handle_join_uri(const char* uri);
void net_ui_toggle_replay_hud(void);
int  net_ui_replay_hud_visible(void);

int  net_ui_run_smoke_test(void);

#endif
