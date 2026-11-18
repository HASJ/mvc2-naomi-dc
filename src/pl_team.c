/* The players' run each frame: the lists of the players in the fight, the team changes (tags,
   partners coming in, team hyper combos and a KO'd point character's replacement) and their draw. */
#include "player.h"
#include "pl_char.h"
#include "pl_color.h"

static PLAYER_WORK *pl_list_top ;	/* the players in the fight, run in this order */
static PLAYER_WORK *pl_list_end ;
static PLAYER_WORK *pl_list2_top ;	/* those with routine_1 2, run after them */
static PLAYER_WORK *pl_list2_end ;
static Uint8 double_assist_pending[2] ;	/* bit (1 << team_no) set when both assists were called together */
static Sint8 assist_extra_req[2][3] = { { -1, -1, -1 }, { -1, -1, -1 } } ;
static Sint8 assist_saved_personal[6] = { -1, -1, -1, -1, -1, -1 } ;
static Uint8 active_tag_req[2] ;	/* partner team_no (1 or 2) requested for active tag this frame */
static Uint8 active_tag_out_pending[6] ;	/* set on work_no while an actively tagged-out char is on screen */
static Uint8 was_actively_tagged_out[6] ;	/* set on work_no until combo and lockout/on-screen end */
static Uint8 active_tag_no_retag[6] ;	/* set on work_no when prohibited from being actively tagged in */
static Uint8 active_tag_combo_assist_used[6] ;	/* set on work_no once the 1 extra combo assist call is used */
static Uint8 counter_tag_req[2] ;	/* partner team_no (1 or 2) requested for counter-tag this frame */
static Uint8 counter_tag_pos_req[2] ;	/* 0 ground, 1 incoming height, 2 superjump height */
static Uint8 counter_tag_used[6] ;	/* set on work_no once that member has counter-tagged in this match */
static Uint8 counter_tag_out_pending[6] ;	/* set on outgoing point work_no until it leaves */
static Uint8 counter_tag_in_pos[6] ;	/* entry height kind for act_change_counter_tag */

void pl_counter_tag_reset_all( void )
{
	memset( counter_tag_req, 0, sizeof( counter_tag_req ) ) ;
	memset( counter_tag_pos_req, 0, sizeof( counter_tag_pos_req ) ) ;
	memset( counter_tag_used, 0, sizeof( counter_tag_used ) ) ;
	memset( counter_tag_out_pending, 0, sizeof( counter_tag_out_pending ) ) ;
	memset( counter_tag_in_pos, 0, sizeof( counter_tag_in_pos ) ) ;
}

int pl_counter_tag_ok_ck( PLAYER_WORK *nw )
{
	return !counter_tag_used[nw->work_no] && !counter_tag_out_pending[nw->work_no] ;
}

int pl_counter_tag_pending_ck( Uint8 pl_no )
{
	return pl_no < 2 && counter_tag_req[pl_no] != 0 ;
}

void pl_counter_tag_req_set( Uint8 pl_no, Uint8 team_idx, Uint8 pos_kind )
{
	if( pl_no < 2 && ( team_idx == 1 || team_idx == 2 ) && pos_kind <= 2 ){
		counter_tag_req[pl_no] = team_idx ;
		counter_tag_pos_req[pl_no] = pos_kind ;
	}
}

Uint8 pl_counter_tag_in_pos_get( const PLAYER_WORK *pl_w )
{
	return counter_tag_in_pos[pl_w->work_no] ;
}

int pl_counter_tag_invert_ck( void )
{
	int i ;

	if( vs_sys_work.vs_st != VS_FIGHT ) return 0 ;
	for( i = 0 ; i < 6 ; i ++ ){
		if( pl_work[i].be_flag && pl_work[i].routine_1 == PL_NORM
		 && pl_work[i].act_st == N_CHANGE && pl_work[i].change_type == 12 ){
			return 1 ;
		}
	}
	return 0 ;
}

void pl_assist_extra_set( Uint8 pl_no, Uint8 team_idx, Sint8 assist_type )
{
	if( pl_no < 2 && team_idx < 3 ) assist_extra_req[pl_no][team_idx] = assist_type ;
}

int pl_in_combo_ck( Uint8 pl_no )
{
	Uint8 opp = pl_no ^ 1 ;

	if( vs_sys_work.combo_total[pl_no] > 0 ) return 1 ;
	if( vs_sys_work.team_w[pl_no][0]->combo_cnt > 0 ) return 1 ;
	if( vs_sys_work.team_w[opp][0]->combo_reset_flag
	 || vs_sys_work.team_w[opp][1]->combo_reset_flag
	 || vs_sys_work.team_w[opp][2]->combo_reset_flag ) return 1 ;
	return 0 ;
}

int pl_active_tag_out_ck( PLAYER_WORK *pl_w )
{
	return active_tag_out_pending[pl_w->work_no] != 0 ;
}

int pl_active_tag_ok_ck( PLAYER_WORK *nw )
{
	if( active_tag_no_retag[nw->work_no] || active_tag_out_pending[nw->work_no] || counter_tag_out_pending[nw->work_no] ) return 0 ;
	return 1 ;
}

int pl_active_tag_pending_ck( Uint8 pl_no )
{
	return pl_no < 2 && active_tag_req[pl_no] != 0 ;
}

int pl_assist_slot_ok_ck( PLAYER_WORK *nw, int is_assist )
{
	if( is_assist && was_actively_tagged_out[nw->work_no] && pl_in_combo_ck( nw->pl_no ) ){
		return !active_tag_combo_assist_used[nw->work_no] ;
	}
	return !nw->vital_red_wait ;
}

void pl_active_tag_req_set( Uint8 pl_no, Uint8 team_idx )
{
	if( pl_no < 2 && ( team_idx == 1 || team_idx == 2 ) ) active_tag_req[pl_no] = team_idx ;
}

void pl_assist_out_lockout( PLAYER_WORK *pl_w )
{
	Uint8 mask ;
	Sint16 wait = 0 ;

	if( !pl_w->team_no ) return ;
	mask = (Uint8)( 1 << pl_w->team_no ) ;
	if( double_assist_pending[pl_w->pl_no] & mask ){
		double_assist_pending[pl_w->pl_no] &= (Uint8)~mask ;
		wait += 180 ;
	}
	if( active_tag_out_pending[pl_w->work_no] ){
		active_tag_out_pending[pl_w->work_no] = 0 ;
		wait += 120 ;
	}
	if( wait > 0 && pl_w->vital_new > 0 ){
		if( pl_w->vital_red_wait < wait ){
			pl_w->vital_red_wait = wait ;
		}
	}
}

extern int dbg_flag_ck( int a, int b ) ;
extern void pl_prio_set( PLAYER_WORK *pl_l, PLAYER_WORK *ef_l ) ;
extern void pl_push_ck( void ) ;
extern void vital_red_recover( void ) ;
extern void vital_red_timer( void ) ;
#ifdef TARGET_DC
extern void vital_drain_move( void ) ;
#endif
extern void pl_change_set( PLAYER_WORK *pl_w, PLAYER_WORK *nw ) ;
extern void pl_change_in_set2( PLAYER_WORK *pl_w ) ;
extern void pl_hyper_scr_flash( PLAYER_WORK *pl_w ) ;
extern void pl_hyper_lvup_flash( PLAYER_WORK *pl_w ) ;
extern void pl_gauge_sub( PLAYER_WORK *pl_w, int no ) ;
extern void pl_tlocal_init( PLAYER_WORK *pl_w ) ;
extern void catch_ko_ck( PLAYER_WORK *pl_w ) ;

void pl_work_move( void ) ;
void pl_hit_work_set( void ) ;
static void pl_list_set( void ) ;
void team_w_set( void ) ;
void pl_work_disp( void ) ;
static void team_change_ck( void ) ;
static void change_tag( PLAYER_WORK *pl_w ) ;
static void change_active_tag( PLAYER_WORK *pl_w, Uint8 idx ) ;
static void change_counter_tag( PLAYER_WORK *pl_w, Uint8 idx, Uint8 pos_kind ) ;
static void change_in( PLAYER_WORK *pl_w ) ;
static void change_next( PLAYER_WORK *pl_w ) ;
static void change_vcounter( PLAYER_WORK *pl_w ) ;
static void change_unison( PLAYER_WORK *pl_w ) ;
static void change_gattai( PLAYER_WORK *pl_w ) ;
static void change_hyper( PLAYER_WORK *pl_w ) ;
static void change_delay( PLAYER_WORK *pl_w ) ;
static void change_snap( PLAYER_WORK *pl_w ) ;
static void team_hyper_ck( void ) ;
static int partner_neutral_ck( PLAYER_WORK *w ) ;
static void team_end_ck( PLAYER_WORK *pl_w, PLAYER_WORK *w1, PLAYER_WORK *w2 ) ;
static void team_end_ck_all( void ) ;
#ifdef TARGET_DC
static void gauge_auto_add( void ) ;
#endif
static void hyper_bg_se_ck( void ) ;

/****************************************************************/
/*			The players' frame: the team changes, then each		*/
/*			player's move										*/
/****************************************************************/
void pl_work_move( void )
{
	PLAYER_WORK *w ;

	team_hyper_ck() ;
	team_end_ck_all() ;
	pl_list_set() ;
	if( !( vs_sys_work.slow_bit & 1 << vs_sys_work.slow_step ) ){
		vs_sys_work.move_bit = 63 ;
		w = pl_list_top ;
		if( w ){
			do {
				vs_sys_work.move_again_flag = 0 ;
				vs_sys_work.move_bit &= ~( 1 << w->work_no ) ;
				player_move( w ) ;
				if( vs_sys_work.move_again_flag ) player_move( w ) ;
				w = w->behind_w ;
			} while( w ) ;
			/* those a player's move left out */
			w = pl_list_top ;
			do {
				if( vs_sys_work.move_bit & 1 << w->work_no ) player_move( w ) ;
				w = w->behind_w ;
			} while( w ) ;
		}
		w = pl_list2_top ;
		if( w ){
			do {
				vs_sys_work.move_again_flag = 0 ;
				player_move( w ) ;
				if( vs_sys_work.move_again_flag ) player_move( w ) ;
				w = w->behind_w ;
			} while( w ) ;
		}
		pl_prio_set( pl_list_top, pl_list2_top ) ;
		team_change_ck() ;
		team_w_set() ;
		hyper_bg_se_ck() ;
		pl_push_ck() ;
	}
	vital_red_recover() ;
	vital_red_timer() ;
	cpu_level_set() ;
#ifdef TARGET_DC	/* the home rules */
	gauge_auto_add() ;
	vital_drain_move() ;
#endif
}

/****************************************************************/
/*			Enters both lists for the hit check					*/
/****************************************************************/
void pl_hit_work_set( void )
{
	PLAYER_WORK *w ;

	if( ( w = pl_list_top ) ){
		do {
			hit_work_set( w ) ;
			w = w->behind_w ;
		} while( w ) ;
	}
	w = pl_list2_top ;
	if( w ){
		do {
			hit_work_set( w ) ;
			w = w->behind_w ;
		} while( w ) ;
	}
}

/*==============================================================*/
/*			Builds the two lists from the teams, starting		*/
/*			with a different side each frame					*/
/*==============================================================*/
static void pl_list_set( void )
{
	PLAYER_WORK *w, *end, *end2 ;
	int side ;
	int i, j ;

	pl_list_top = pl_list_end = 0 ;
	pl_list2_top = pl_list2_end = 0 ;
	end = end2 = 0 ;
	side = Sys->game_timer0 & 1 ;
	for( i = 0 ; i < 2 ; i ++ ){
		j = 0 ;
		while( j < 3 ){
			w = vs_sys_work.team_w[side][j] ;
			if( vs_sys_work.team_w[side][j]->be_flag ){
				if( w->routine_1 != PL_CAUGHT ){
					if( !pl_list_top ) pl_list_top = w ;
					else end->behind_w = w ;
					end = w ;
				} else {
					if( !pl_list2_top ) pl_list2_top = w ;
					else end2->behind_w = w ;
					end2 = w ;
				}
			}
			j ++ ;
		}
		side ^= 1 ;
	}
	pl_list_end = end ;
	if( end ) end->behind_w = 0 ;
	if( pl_list2_end = end2 ) end2->behind_w = 0 ;
	if( 0 ) pl_list_top = 0 ;
}

/****************************************************************/
/*			Puts each player work in its place in team_w,		*/
/*			and counts each side's players in the fight			*/
/****************************************************************/
void team_w_set( void )
{
	PLAYER_WORK *w ;
	int i ;

	vs_sys_work.be_cnt[0] = vs_sys_work.be_cnt[1] = 0 ;
	w = pl_work ;
	i = 6 ;
	do {
		vs_sys_work.team_w[w->pl_no][w->team_no] = w ;
		if( w->be_flag ) vs_sys_work.be_cnt[w->pl_no] ++ ;
		w ++ ;
	} while( --i ) ;
	pl_work[0].target_w = pl_work[2].target_w = pl_work[4].target_w = vs_sys_work.team_w[1][0] ;
	pl_work[1].target_w = pl_work[3].target_w = pl_work[5].target_w = vs_sys_work.team_w[0][0] ;
}

/****************************************************************/
/*			Draws the player works								*/
/****************************************************************/
void pl_work_disp( void )
{
	PLAYER_WORK *w ;
	int i ;

	if( dbg_flag_ck( 0, 1 ) ) nlSetBorderColor( 0x777777 ) ;
	for( i = 0 ; i < 6 ; i ++ ){
		w = &pl_work[i] ;
		if( w->disp_flag ){
			sprite_tex_set( w ) ;
			disp_prio_set( w ) ;
		}
	}
}

/*==============================================================*/
/*			Runs the team changes the players asked for this	*/
/*			frame (change_req)										*/
/*==============================================================*/
static void team_change_ck( void )
{
	static void ( *change_jmp[] )( PLAYER_WORK *pl_w ) = {
		change_tag, change_tag, change_next, change_vcounter, change_unison,
		change_gattai, change_hyper, change_tag, change_tag, change_snap,
	} ;
	PLAYER_WORK *list[6] ;
	PLAYER_WORK *w ;
	int i ;

	if( vs_sys_work.vs_st < VS_FIGHT ){
		double_assist_pending[0] = double_assist_pending[1] = 0 ;
		active_tag_req[0] = active_tag_req[1] = 0 ;
		counter_tag_req[0] = counter_tag_req[1] = 0 ;
		counter_tag_pos_req[0] = counter_tag_pos_req[1] = 0 ;
		for( i = 0 ; i < 6 ; i ++ ){
			active_tag_out_pending[i] = 0 ;
			was_actively_tagged_out[i] = 0 ;
			active_tag_no_retag[i] = 0 ;
			active_tag_combo_assist_used[i] = 0 ;
			counter_tag_out_pending[i] = 0 ;
			counter_tag_in_pos[i] = 0 ;
		}
	}
	for( i = 0 ; i < 6 ; i ++ ) list[i] = &pl_work[i] ;
	for( i = 0 ; i < 6 ; i ++ ){
		w = list[i] ;
		if( w->change_req ){
			change_jmp[w->change_req]( w ) ;
			w->change_req = 0 ;
		}
	}
	for( i = 0 ; i < 2 ; i ++ ){
		w = vs_sys_work.team_w[i][0] ;
		if( w->delay_change_flag ){
			change_delay( w ) ;
			w->delay_change_flag = 0 ;
		}
		if( active_tag_req[i] ){
			change_active_tag( vs_sys_work.team_w[i][0], active_tag_req[i] ) ;
			active_tag_req[i] = 0 ;
		}
		if( counter_tag_req[i] ){
			change_counter_tag( vs_sys_work.team_w[i][0], counter_tag_req[i], counter_tag_pos_req[i] ) ;
			counter_tag_req[i] = 0 ;
		}
		if( !vs_sys_work.team_w[i][1]->be_flag && !vs_sys_work.team_w[i][2]->be_flag ){
			double_assist_pending[i] = 0 ;
		}
	}
	for( i = 0 ; i < 6 ; i ++ ){
		if( assist_saved_personal[i] >= 0 && ( !pl_work[i].be_flag || !pl_work[i].team_no || pl_work[i].team_type != 8 ) ){
			pl_work[i].pl_personal = assist_saved_personal[i] ;
			assist_saved_personal[i] = -1 ;
		}
		if( !pl_work[i].be_flag && active_tag_out_pending[i] ){
			pl_assist_out_lockout( &pl_work[i] ) ;
		}
		if( !pl_work[i].be_flag || !pl_work[i].team_no ){
			counter_tag_out_pending[i] = 0 ;
		}
		if( !pl_work[i].team_no || ( !pl_work[i].be_flag && !active_tag_out_pending[i] && !pl_work[i].vital_red_wait && !pl_in_combo_ck( pl_work[i].pl_no ) ) ){
			was_actively_tagged_out[i] = 0 ;
			active_tag_no_retag[i] = 0 ;
			active_tag_combo_assist_used[i] = 0 ;
		}
	}
}

/*==============================================================*/
/*			Active tag: cancels the on-screen assist into point	*/
/*			control, leaving the old point to finish to neutral	*/
/*==============================================================*/
static void change_active_tag( PLAYER_WORK *pl_w, Uint8 idx )
{
	Uint8 pl_no = pl_w->pl_no ;
	Uint8 other_idx = idx ^ 3 ;
	PLAYER_WORK *nw = vs_sys_work.team_w[pl_no][idx] ;
	PLAYER_WORK *other_w = vs_sys_work.team_w[pl_no][other_idx] ;
	VEC3 pos ;
	Sint8 partner_cnt ;

	if( !nw->be_flag || nw->team_type != 8 || nw->vital_new <= 0 || nw->routine_1 != PL_NORM ) return ;
	if( pl_w->vital_new <= 0 || pl_w->routine_1 != PL_NORM ) return ;

	if( ( double_assist_pending[pl_no] & ( 1 << other_idx ) )
	 || ( other_w->be_flag && other_w->team_type == 8 && !active_tag_out_pending[other_w->work_no] ) ){
		active_tag_no_retag[other_w->work_no] = 1 ;
	}
	double_assist_pending[pl_no] &= (Uint8)~( 1 << idx ) ;

	if( assist_saved_personal[nw->work_no] >= 0 ){
		nw->pl_personal = assist_saved_personal[nw->work_no] ;
		assist_saved_personal[nw->work_no] = -1 ;
	}

	nw->team_no = 0 ;
	nw->team_type = 0 ;
	pl_w->team_no = idx ;
	pl_w->team_type = 8 ;
	vs_sys_work.team_w[pl_no][0] = nw ;
	vs_sys_work.team_w[pl_no][idx] = pl_w ;
	vs_sys_work.team_hyper_bit[pl_no] |= (Uint8)( 1 << idx ) ;
	partner_cnt = 0 ;
	if( vs_sys_work.team_w[pl_no][1]->be_flag && vs_sys_work.team_w[pl_no][1]->team_type == 8 ) partner_cnt ++ ;
	if( vs_sys_work.team_w[pl_no][2]->be_flag && vs_sys_work.team_w[pl_no][2]->team_type == 8 ) partner_cnt ++ ;
	nw->v_combi_cnt = partner_cnt ;

	nw->combo_cnt = vs_sys_work.combo_total[pl_no] ? vs_sys_work.combo_total[pl_no] : pl_w->combo_cnt ;
	nw->combo_kind = pl_w->combo_kind ;
	nw->combo_kind_old = pl_w->combo_kind_old ;
	nw->combo_hit_type = pl_w->combo_hit_type ;
	nw->combo_hit_type_old = pl_w->combo_hit_type_old ;
	nw->combo_hit_types = pl_w->combo_hit_types ;
	nw->air_combo_cnt = pl_w->air_combo_cnt ;
	pl_w->combo_cnt = 0 ;
	pl_w->combo_kind = 0 ;
	pl_w->air_combo_cnt = 0 ;
	pl_w->combo_hit_types = 0 ;
	pl_w->combo_hit_type = 0 ;

	active_tag_out_pending[pl_w->work_no] = 1 ;
	was_actively_tagged_out[pl_w->work_no] = 1 ;
	active_tag_no_retag[pl_w->work_no] = 1 ;
	active_tag_combo_assist_used[pl_w->work_no] = 0 ;
	pl_w->vital_red_cnt = 300 ;
	pl_w->fly_screen_flag = 0 ;
	pl_w->delay_flag = 0 ;
	pl_w->delay_timer = 0 ;
	pl_w->delay_change_flag = 0 ;
	memset( &pl_w->dash_w, 0, sizeof( COM_WORK ) * 17 ) ;
	pl_w->pl_sw_new = pl_w->pl_sw_now = pl_w->pl_sw_old = 0 ;
	pl_w->pl_new_lvbt = pl_w->pl_now_lvbt = pl_w->pl_last_lvbt = 0 ;
	pl_w->pl_old_lvbt = pl_w->pl_old2_lvbt = 0 ;
	pl_w->pl_stop_new = pl_w->pl_stop_now = 0 ;
	pl_w->assist_timer[0] = pl_w->assist_timer[1] = 0 ;
	pl_w->change_timer[0] = pl_w->change_timer[1] = 0 ;

	scr_excep_clr() ;
	pl_tlocal_init( nw ) ;
	memset( &nw->dash_w, 0, sizeof( COM_WORK ) * 17 ) ;
	nw->assist_timer[0] = nw->assist_timer[1] = 0 ;
	nw->change_timer[0] = nw->change_timer[1] = 0 ;
	nw->routine_0 = PL_FIGHT ;
	nw->routine_1 = PL_NORM ;
	nw->routine_2 = 0 ;
	nw->routine_3 = 0 ;
	nw->fly_flag = 0 ;
	nw->sp_ng_timer = 0 ;
	nw->atck_ng_timer = 0 ;
	nw->catch_ng_timer = 0 ;
	nw->wall_ng_timer = 0 ;
	nw->stop_move_flag = 0 ;
	nw->stop_move_timer = 0 ;
	nw->cg_atck_flag = 0 ;
	nw->hit_flag = -1 ;
	nw->dm_flag = -128 ;
	nw->vital_red = nw->vital_new ;
	nw->vital_red_wait = 0 ;
#ifdef TARGET_DC
	nw->drain_timer = 100 ;
#endif
	nw->rot[0] = nw->rot[1] = nw->rot[2] = 0 ;
	nw->scale = nw->pl_scale ;
	nw->alpha = 1.0f ;
	nw->disp_flag = 1 ;
	pl_color_set( nw, 1 ) ;
	if( nw->pos.x < scr_work.eye[0].x - 285.0f ) nw->pos.x = scr_work.eye[0].x - 285.0f ;
	if( nw->pos.x > scr_work.eye[0].x + 285.0f ) nw->pos.x = scr_work.eye[0].x + 285.0f ;
	pl_dir_set( nw ) ;
	pl_sw_set( nw ) ;
	nw->pl_now_lvbt &= ~( KEY_A1 | KEY_A2 ) ;
	nw->pl_sw_now &= ~( KEY_A1 | KEY_A2 ) ;
	if( nw->pos.y > nw->ground_y ){
		nw->spd.x = 0.0f ;
		nw->spd.y = 0.0f ;
		nw->acc.x = 0.0f ;
		nw->acc.y = -DOT_Y( 0.375f ) ;
		nw->pl_stance = STANCE_AIR ;
		nw->sjump_flag = 0 ;
		nw->hop_stop_cnt = 0 ;
		nw->jump_atk_cnt = JATK_ONE ;
		nw->jump_com_cnt = 0 ;
		nw->jump_dash_cnt = 0 ;
		nw->air_jump_cnt = 0 ;
		nw->jump_dir = -1 ;
		pl_jump_ret( nw ) ;
	} else {
		nw->pos.y = nw->ground_y ;
		nw->pl_stance = STANCE_STAND ;
		pl_stop_stand_ret( nw ) ;
	}
	pos.x = nw->pos.x ;
	pos.y = nw->pos.y + DOT_Y( 50.0f ) ;
	pos.z = nw->pos.z ;
	clash_ef_set( &pos, nw->disp_dir ) ;
	pl_se_req( nw, 63 ) ;
}

/*==============================================================*/
/*			Counter-tag: brings a partner in from the screen	*/
/*			edge while leaving the hit point vulnerable			*/
/*==============================================================*/
static void change_counter_tag( PLAYER_WORK *pl_w, Uint8 idx, Uint8 pos_kind )
{
	Uint8 pl_no = pl_w->pl_no ;
	PLAYER_WORK *nw = vs_sys_work.team_w[pl_no][idx] ;
	PLAYER_WORK *em = vs_sys_work.team_w[pl_no ^ 1][0] ;
	VEC3 pos ;
	Float ofs ;
	Sint16 total ;
	Sint8 partner_cnt ;
	Uint8 from_left ;

	if( pl_w->team_no || pl_w->vital_new <= 0 || pl_w->ko_flag || pl_w->routine_1 != PL_DAMAGE ) return ;
	if( nw->be_flag || nw->vital_new <= 0 || counter_tag_used[nw->work_no] ) return ;

	total = (Sint16)( *pl_w->gauge_level ) * 144 + *pl_w->gauge ;
	if( total < 360 ) return ;
	total -= 360 ;
	*pl_w->gauge_level = (Sint8)( total / 144 ) ;
	*pl_w->gauge = total % 144 ;

	counter_tag_used[nw->work_no] = 1 ;
	double_assist_pending[pl_no] &= (Uint8)~( 1 << idx ) ;
	if( assist_saved_personal[nw->work_no] >= 0 ){
		nw->pl_personal = assist_saved_personal[nw->work_no] ;
		assist_saved_personal[nw->work_no] = -1 ;
	}

	nw->team_no = 0 ;
	nw->team_type = 0 ;
	pl_w->team_no = idx ;
	pl_w->team_type = 8 ;
	vs_sys_work.team_w[pl_no][0] = nw ;
	vs_sys_work.team_w[pl_no][idx] = pl_w ;
	vs_sys_work.team_hyper_bit[pl_no] |= (Uint8)( 1 << idx ) ;
	partner_cnt = 0 ;
	if( vs_sys_work.team_w[pl_no][1]->be_flag && vs_sys_work.team_w[pl_no][1]->team_type == 8 ) partner_cnt ++ ;
	if( vs_sys_work.team_w[pl_no][2]->be_flag && vs_sys_work.team_w[pl_no][2]->team_type == 8 ) partner_cnt ++ ;
	nw->v_combi_cnt = partner_cnt ;

	nw->combo_cnt = 0 ;
	nw->combo_kind = 0 ;
	nw->air_combo_cnt = 0 ;
	nw->combo_hit_types = 0 ;
	nw->combo_hit_type = 0 ;
	pl_w->combo_cnt = 0 ;
	pl_w->combo_kind = 0 ;
	pl_w->air_combo_cnt = 0 ;
	pl_w->combo_hit_types = 0 ;
	pl_w->combo_hit_type = 0 ;

	counter_tag_out_pending[pl_w->work_no] = 1 ;
	if( !pl_w->down_hit_flag ) pl_w->dm_flag = -128 ;
	pl_w->vital_red_cnt = 300 ;
	pl_w->delay_flag = 0 ;
	pl_w->delay_timer = 0 ;
	pl_w->delay_change_flag = 0 ;
	memset( &pl_w->dash_w, 0, sizeof( COM_WORK ) * 17 ) ;
	pl_w->pl_sw_new = pl_w->pl_sw_now = pl_w->pl_sw_old = 0 ;
	pl_w->pl_new_lvbt = pl_w->pl_now_lvbt = pl_w->pl_last_lvbt = 0 ;
	pl_w->pl_old_lvbt = pl_w->pl_old2_lvbt = 0 ;
	pl_w->pl_stop_new = pl_w->pl_stop_now = 0 ;
	pl_w->assist_timer[0] = pl_w->assist_timer[1] = 0 ;
	pl_w->change_timer[0] = pl_w->change_timer[1] = 0 ;

	scr_excep_clr() ;
	pl_tlocal_init( nw ) ;
	memset( &nw->dash_w, 0, sizeof( COM_WORK ) * 17 ) ;
	nw->assist_timer[0] = nw->assist_timer[1] = 0 ;
	nw->change_timer[0] = nw->change_timer[1] = 0 ;
	nw->be_flag = 1 ;
	nw->disp_flag = 1 ;
	nw->routine_0 = PL_FIGHT ;
	nw->routine_1 = PL_NORM ;
	nw->routine_2 = 0 ;
	nw->routine_3 = 0 ;
	act_st_set( nw, N_CHANGE ) ;
	nw->change_type = 12 ;
	counter_tag_in_pos[nw->work_no] = pos_kind ;
	nw->fly_flag = 0 ;
	nw->sjump_flag = 0 ;
	nw->hop_stop_cnt = 0 ;
	nw->sjump_cnt = 0 ;
	nw->sp_ng_timer = 0 ;
	nw->atck_ng_timer = 0 ;
	nw->catch_ng_timer = 2 ;
	nw->nml_catch_ng_timer = 2 ;
	nw->wall_ng_timer = 2 ;
	nw->push_ng_timer = 2 ;
	nw->stop_move_flag = 0 ;
	nw->stop_move_timer = 0 ;
	nw->cg_atck_flag = 0 ;
	nw->atck_flag = 0 ;
	nw->hit_flag = -1 ;
	nw->dm_flag = 0 ;
	nw->stun_cnt = 80 ;
	nw->vital_red = nw->vital_new ;
	nw->vital_red_wait = 0 ;
#ifdef TARGET_DC
	nw->drain_timer = 100 ;
#endif
	nw->rot[0] = nw->rot[1] = nw->rot[2] = 0 ;
	nw->scale = nw->pl_scale ;
	nw->alpha = 1.0f ;
	nw->wall_flag = 0 ;

	ofs = pl_w->pl_dir ? DOT_X( 14.0f ) : -DOT_X( 14.0f ) ;
	from_left = ( pl_w->pos.x <= em->pos.x + ofs ) ? 1 : 0 ;
	if( from_left ){
		nw->pos.x = scr_work.scr_left - DOT_X( 48.0f ) ;
		nw->pl_dir = nw->disp_dir = 1 ;
		nw->spd.x = DOT_X( 9.0f ) ;
	} else {
		nw->pos.x = scr_work.scr_right + DOT_X( 48.0f ) ;
		nw->pl_dir = nw->disp_dir = 0 ;
		nw->spd.x = -DOT_X( 9.0f ) ;
	}
	nw->acc.x = 0.0f ;
	nw->pos.z = pl_w->pos.z ;
	if( pos_kind == 0 ){
		nw->pos.y = nw->ground_y + DOT_Y( 24.0f ) ;
		nw->acc.y = -DOT_Y( 0.5f ) ;
		nw->spd.y = ( nw->ground_y - nw->pos.y ) / 15.0f - nw->acc.y * 14.0f / 2.0f ;
		nw->pl_stance = STANCE_AIR ;
		pl_char_set( nw, Char_Jump, 1 ) ;
	} else if( pos_kind == 1 ){
		nw->pos.y = nw->ground_y + DOT_Y( 192.0f ) ;
		nw->spd.y = -DOT_Y( 1.0f ) ;
		nw->acc.y = -DOT_Y( 0.2f ) ;
		nw->pl_stance = STANCE_AIR ;
		pl_char_set( nw, Char_Jump, 1 ) ;
	} else {
		nw->pos.y = nw->ground_y + DOT_Y( 320.0f ) ;
		nw->spd.y = -DOT_Y( 0.5f ) ;
		nw->acc.y = -DOT_Y( 0.2f ) ;
		nw->pl_stance = STANCE_AIR ;
		nw->sjump_flag = 2 ;
		pl_char_set( nw, Char_Jump, 7 ) ;
	}
	nw->timer0 = 15 ;
	pl_color_set( nw, 1 ) ;
	pl_voice_req( nw, 43 ) ;
	pl_sw_set( nw ) ;
	nw->pl_now_lvbt &= ~( KEY_LIGHT | KEY_HEAVY ) ;
	nw->pl_sw_now &= ~( KEY_LIGHT | KEY_HEAVY ) ;

	pos.x = pl_w->pos.x ;
	pos.y = pl_w->pos.y + DOT_Y( 50.0f ) ;
	pos.z = pl_w->pos.z ;
	clash_ef_set( &pos, pl_w->disp_dir ) ;
	pl_se_req( nw, 63 ) ;
}

/*==============================================================*/
/*			A tag: the partner comes in from the screen's		*/
/*			edge behind the player								*/
/*==============================================================*/
static void change_tag( PLAYER_WORK *pl_w )
{
	PLAYER_WORK *nw = vs_sys_work.team_w[pl_w->pl_no][pl_w->partner_no] ;

	pl_w->team_no = pl_w->partner_no ;
	vs_sys_work.team_w[pl_w->pl_no][pl_w->partner_no] = pl_w ;
	nw->be_flag = 1 ;
	nw->disp_flag = 1 ;
	nw->team_no = 0 ;
	nw->routine_0 = PL_FIGHT ;
	nw->routine_1 = PL_NORM ;
	nw->routine_2 = 0 ;
	nw->routine_3 = 0 ;
	act_st_set( nw, N_CHANGE ) ;
	nw->change_type = 9 ;
	pl_w->team_type = pl_w->change_req ;
	nw->team_type = pl_w->change_req ;
	if( !pl_w->pl_dir ) nw->pos.x = scr_work.scr_right + DOT_X( 80.0f ) ;
	else nw->pos.x = scr_work.scr_left + -DOT_X( 80.0f ) ;
	nw->pos.y = nw->ground_y + DOT_Y( 64.0f ) ;
	nw->wall_ng_timer = 8 ;
	nw->cg_shadow = 255 ;
	pl_dir_set( nw ) ;
	pl_w->dm_flag = 0 ;
	pl_w->catch_ng_timer = 3 ;
	combo_reset( pl_w ) ;
	pl_change_set( nw, pl_w ) ;
}

/*==============================================================*/
/*			The next player jumps in from the screen's edge		*/
/*==============================================================*/
static void change_in( PLAYER_WORK *pl_w )
{
	pl_w->team_no = 0 ;
	pl_w->disp_flag = 1 ;
	pl_w->be_flag = 1 ;
	pl_w->team_type = 2 ;
	if( pl_w->target_w->pl_dir ) pl_w->pos.x = scr_work.scr_right + DOT_X( 80.0f ) ;
	else pl_w->pos.x = scr_work.scr_left + -DOT_X( 80.0f ) ;
	pl_w->pos.y = pl_w->ground_y ;
	pl_dir_set( pl_w ) ;
	pl_w->pos.y += DOT_Y( 192.0f ) ;
	pl_w->spd.y = -DOT_Y( 4.0f ) ;
	pl_w->acc.y = -DOT_Y( 0.375f ) ;
	pl_w->spd.x = DOT_X( 8.0f ) ;
	if( !pl_w->pl_dir ) pl_w->spd.x = -pl_w->spd.x ;
	pl_w->acc.x = 0.0f ;
	pl_w->routine_0 = PL_FIGHT ;
	pl_w->routine_1 = PL_NORM ;
	act_st_set( pl_w, N_JUMP_AIR ) ;
	pl_w->routine_2 = 1 ;
	pl_w->routine_3 = 1 ;
	pl_w->jump_atk_cnt = JATK_ONE ;
	pl_w->jump_com_cnt = 0 ;
	pl_w->jump_dash_cnt = 1 ;
	pl_w->air_jump_cnt = 4 ;
	pl_w->pl_stance = STANCE_AIR ;
	pl_w->wall_ng_timer = 18 ;
	pl_w->sp_ng_timer = 22 ;
	pl_w->nml_catch_ng_timer = 30 ;
	pl_w->jump_dir = 0 ;
	pl_char_set( pl_w, Char_Jump, 10 ) ;
	pl_color_set( pl_w, 1 ) ;
}

/*==============================================================*/
/*			The point character is out: the next one with		*/
/*			vital left comes in									*/
/*==============================================================*/
static void change_next( PLAYER_WORK *pl_w )
{
	PLAYER_WORK *nw, *w2 ;

	if( !pl_w->team_no ){
		nw = vs_sys_work.team_w[pl_w->pl_no][1] ;
		if( !nw->vital_new ) return ;
		w2 = vs_sys_work.team_w[pl_w->pl_no][2] ;
		if( w2->vital_new ){
			w2->team_no = 1 ;
			pl_w->team_no = 2 ;
		} else {
			pl_w->team_no = 1 ;
		}
		pl_w->be_flag = pl_w->disp_flag = 0 ;
		change_in( nw ) ;
		pl_change_set( nw, pl_w ) ;
	} else {
		pl_w->be_flag = pl_w->disp_flag = 0 ;
		pl_assist_out_lockout( pl_w ) ;
		if( !pl_w->vital_new && pl_w->team_no != 2 ){
			w2 = vs_sys_work.team_w[pl_w->pl_no][2] ;
			if( w2->vital_new ){
				pl_w->team_no = 2 ;
				w2->team_no = 1 ;
				if( double_assist_pending[pl_w->pl_no] & ( 1 << 2 ) ){
					double_assist_pending[pl_w->pl_no] &= (Uint8)~( 1 << 2 ) ;
					double_assist_pending[pl_w->pl_no] |= (Uint8)( 1 << 1 ) ;
				}
			}
		}
	}
}

/*==============================================================*/
/*			A variable counter: the partner comes in where		*/
/*			the player stands									*/
/*==============================================================*/
static void change_vcounter( PLAYER_WORK *pl_w )
{
	PLAYER_WORK *nw = vs_sys_work.team_w[pl_w->pl_no][pl_w->partner_no] ;

	if( nw->vital_new > 0 && !nw->vital_red_wait ){
		nw->be_flag = 1 ;
		nw->disp_flag = 0 ;
		nw->routine_0 = PL_FIGHT ;
		nw->routine_1 = PL_NORM ;
		nw->routine_2 = 0 ;
		nw->routine_3 = 0 ;
		act_st_set( nw, N_CHANGE ) ;
		nw->change_type = 5 ;
		nw->team_type = 3 ;
		nw->stop_move_flag = 255 ;
		nw->stop_move_timer = 16 ;
		nw->team_no = 0 ;
		pl_w->team_no = pl_w->partner_no ;
		nw->pl_dir = nw->disp_dir = pl_w->pl_dir ;
		nw->pos = pl_w->pos ;
		nw->cg_shadow = 255 ;
		pl_dir_set( nw ) ;
		pl_change_in_set2( nw ) ;
		pl_change_set( nw, pl_w ) ;
		pl_hyper_scr_flash( nw ) ;
	}
}

/*==============================================================*/
/*			A team hyper combo by the character's own			*/
/*			routine												*/
/*==============================================================*/
static void change_unison( PLAYER_WORK *pl_w )
{
	pl_w->pl_tbl[TBL_SP_UNISON]( pl_w ) ;
	pl_w->team_type = pl_w->change_req ;
}

/*==============================================================*/
/*			A variable combination: the partners with vital		*/
/*			left join the hyper combo							*/
/*==============================================================*/
static void change_gattai( PLAYER_WORK *pl_w )
{
	PLAYER_WORK *nw ;
	int i ;
	Sint8 cnt ;
	Uint8 bits ;

	bits = pl_w->work_no / 2 ;
	for( i = 1, cnt = 1 ; i < 3 && cnt < pl_w->v_combi_cnt ; i ++ ){
		nw = vs_sys_work.team_w[pl_w->pl_no][i] ;
		if( nw->vital_new > 0 && !nw->vital_red_wait ){
			nw->be_flag = 1 ;
			nw->disp_flag = 0 ;
			nw->routine_0 = PL_FIGHT ;
			nw->routine_1 = PL_NORM ;
			nw->routine_2 = 0 ;
			nw->routine_3 = 0 ;
			act_st_set( nw, N_CHANGE ) ;
			nw->change_type = 3 ;
			nw->team_type = 5 ;
			nw->stop_move_flag = 255 ;
			nw->stop_move_timer = 16 ;
			nw->pl_dir = nw->disp_dir = pl_w->pl_dir ;
			nw->pos = pl_w->pos ;
			cnt ++ ;
			nw->cg_shadow = 255 ;
			nw->pl_last_lvbt = 0 ;
			nw->pl_now_lvbt = 0 ;
			nw->pl_new_lvbt = 0 ;
			nw->pl_sw_old = 0 ;
			nw->pl_sw_now = 0 ;
			nw->pl_sw_new = 0 ;
			vs_sys_work.team_hyper_bit[pl_w->pl_no] |= 1 << nw->team_no ;
			bits |= 1 << nw->work_no ;
		}
	}
	pl_w->v_combi_cnt = cnt ;
	pl_w->hyper_team_bit = bits ;
	pl_gauge_sub( pl_w, cnt ) ;
}

/*==============================================================*/
/*			The player is sent out and the partner comes in		*/
/*			where it stood, with the gauge flash				*/
/*==============================================================*/
static void change_hyper( PLAYER_WORK *pl_w )
{
	PLAYER_WORK *nw = vs_sys_work.team_w[pl_w->pl_no][pl_w->partner_no] ;
	PLAYER_WORK *w2 ;

	if( nw->vital_new > 0 && !nw->vital_red_wait ){
		catch_ko_ck( pl_w ) ;
		scr_excep_clr() ;
		nw->be_flag = 1 ;
		nw->disp_flag = 0 ;
		nw->routine_0 = PL_FIGHT ;
		nw->routine_1 = PL_NORM ;
		nw->routine_3 = 0 ;
		nw->routine_2 = 0 ;
		act_st_set( nw, N_CHANGE ) ;
		nw->change_type = 8 ;
		nw->team_type = 6 ;
		nw->stop_move_flag = 255 ;
		nw->stop_move_timer = 16 ;
		nw->dm_flag = 0 ;
		nw->catch_ng_timer = 3 ;
		nw->delay_flag = 0 ;
		nw->delay_timer = 0 ;
		nw->team_no = 0 ;
		w2 = vs_sys_work.team_w[pl_w->pl_no][2] ;
		if( !w2->vital_new ){
			pl_w->team_no = pl_w->partner_no ;
		} else {
			w2->team_no = 1 ;
			pl_w->team_no = 2 ;
		}
		pl_w->routine_0 = PL_FIGHT ;
		pl_w->routine_1 = PL_NORM ;
		pl_w->routine_3 = 0 ;
		pl_w->routine_2 = 0 ;
		act_st_set( pl_w, N_CHANGE ) ;
		pl_w->change_type = 2 ;
		pl_w->dm_flag = 0 ;
		pl_w->catch_ng_timer = 3 ;
		combo_reset( pl_w ) ;
		nw->pos = pl_w->pos ;
		nw->pl_dir = nw->disp_dir = pl_w->pl_dir ;
		nw->wall_flag = pl_w->wall_flag ;
		nw->cg_shadow = 255 ;
		pl_dir_set( nw ) ;
		pl_change_in_set2( nw ) ;
		pl_change_set( nw, pl_w ) ;
		pl_hyper_lvup_flash( nw ) ;
		combo_dm_clr( nw ) ;
	}
}

/*==============================================================*/
/*			The partner a delayed hyper combo calls in			*/
/*			(delay_change_flag)											*/
/*==============================================================*/
static void change_delay( PLAYER_WORK *pl_w )
{
	Uint8 idx ;
	Uint8 mask = pl_w->delay_change_flag < 0 ? (Uint8)( 1 << pl_w->partner_no ) : (Uint8)pl_w->delay_change_flag ;

	for( idx = 1 ; idx <= 2 ; idx ++ ){
		PLAYER_WORK *nw ;
		Sint8 req ;
		if( !( mask & ( 1 << idx ) ) ) continue ;
		req = assist_extra_req[pl_w->pl_no][idx] ;
		assist_extra_req[pl_w->pl_no][idx] = -1 ;
		nw = vs_sys_work.team_w[pl_w->pl_no][idx] ;
		if( nw->vital_new > 0 && pl_assist_slot_ok_ck( nw, 1 ) ){
			if( was_actively_tagged_out[nw->work_no] && pl_in_combo_ck( pl_w->pl_no ) ){
				active_tag_combo_assist_used[nw->work_no] = 1 ;
			}
			if( req >= 0 ){
				if( assist_saved_personal[nw->work_no] < 0 ){
					assist_saved_personal[nw->work_no] = nw->pl_personal ;
				}
				nw->pl_personal = req ;
			}
			nw->be_flag = 1 ;
			nw->disp_flag = 0 ;
			nw->routine_0 = PL_FIGHT ;
			nw->routine_1 = PL_NORM ;
			nw->routine_3 = 0 ;
			nw->routine_2 = 0 ;
			act_st_set( nw, N_CHANGE ) ;
			nw->change_type = 7 ;
			nw->team_type = 8 ;
			nw->pl_dir = nw->disp_dir = pl_w->pl_dir ;
			nw->pos = pl_w->pos ;
			nw->cg_shadow = 255 ;
			nw->pl_sw_new = nw->pl_sw_now = nw->pl_sw_old = nw->pl_new_lvbt = nw->pl_now_lvbt = nw->pl_last_lvbt = 0 ;
			vs_sys_work.team_hyper_bit[pl_w->pl_no] |= 1 << nw->team_no ;
			pl_w->v_combi_cnt = 1 ;
		}
	}
	{
		PLAYER_WORK *w1 = vs_sys_work.team_w[pl_w->pl_no][1] ;
		PLAYER_WORK *w2 = vs_sys_work.team_w[pl_w->pl_no][2] ;
		if( w1->be_flag && w1->team_type == 8 && !active_tag_out_pending[w1->work_no]
		 && w2->be_flag && w2->team_type == 8 && !active_tag_out_pending[w2->work_no] ){
			double_assist_pending[pl_w->pl_no] |= (Uint8)( ( 1 << 1 ) | ( 1 << 2 ) ) ;
		}
	}
}

/*==============================================================*/
/*			The point character leaves: the partner called,		*/
/*			or else the other one, jumps in						*/
/*==============================================================*/
static void change_snap( PLAYER_WORK *pl_w )
{
	PLAYER_WORK *nw ;

	if( pl_w->team_no ) return ;
	nw = vs_sys_work.team_w[pl_w->pl_no][pl_w->partner_no] ;
	if( !nw->vital_new ){
		nw = vs_sys_work.team_w[pl_w->pl_no][pl_w->partner_no ^ 3] ;
		if( !nw->vital_new ) return ;
	}
	pl_w->team_no = nw->team_no ;
	nw->team_no = 0 ;
	pl_w->disp_flag = 0 ;
	pl_w->be_flag = 0 ;
	change_in( nw ) ;
	pl_change_set( nw, pl_w ) ;
	pl_change_out( pl_w ) ;
}

/*==============================================================*/
/*			The sides in a team hyper combo; a side's point		*/
/*			character gets the stick back when it ends			*/
/*==============================================================*/
static void team_hyper_ck( void )
{
	PLAYER_WORK *w ;
	int old = vs_sys_work.team_hyper_flag ;
	Uint8 f0 = 0, f1 = 0 ;
	int i, j ;

	for( i = 0 ; i < 2 ; i ++ ){
		for( j = 0 ; j < 3 ; j ++ ){
			w = vs_sys_work.team_w[i][j] ;
			if( w->be_flag && ( w->team_type == 4 || w->team_type == 5 ) ) f0 |= 1 << w->pl_no ;
			if( w->be_flag && w->team_type == 8 ) f1 |= 1 << w->pl_no ;
		}
	}
	vs_sys_work.team_hyper_flag = f0 ;
	vs_sys_work.team_delay_flag = f1 ;
	for( i = 0 ; i < 2 ; i ++ ){
		w = vs_sys_work.team_w[i][0] ;
		if( old & 1 << w->pl_no && !( vs_sys_work.team_hyper_flag & 1 << w->pl_no ) ){
			w->pl_new_lvbt = w->pl_last_lvbt = w->pad_sw.now & ( KEY_LEVER | KEY_BUTTON | KEY_START ) ;
		}
	}
}

/*==============================================================*/
/*			Whether a partner on screen has finished its		*/
/*			action and returned to ground neutral state			*/
/*==============================================================*/
static int partner_neutral_ck( PLAYER_WORK *w )
{
	if( w->fly_flag && w->act_st != N_SPECIAL && w->act_st != N_SUPER && w->act_st != N_NML_ATCK && w->act_st != N_CMD_ATCK ){
		w->fly_flag = 0 ;
		if( w->pos.y > w->ground_y ){
			pl_jump_ret2( w ) ;
			return 0 ;
		}
		if( w->act_st == N_FLY ){
			w->pos.y = w->ground_y ;
			w->pl_stance = STANCE_STAND ;
			pl_stop_stand_ret( w ) ;
			return 0 ;
		}
	}
	if( w->pl_stance == STANCE_AIR ) return 0 ;
	if( w->act_st == N_SPECIAL || w->act_st == N_SUPER || w->act_st == N_NML_ATCK || w->act_st == N_CMD_ATCK ) return 0 ;
	if( w->act_st == N_DASH || w->act_st == N_BACK_DASH || w->act_st == N_JUMP || w->act_st == N_SJUMP ) return 0 ;
	if( w->act_st == N_GET_UP || w->act_st == N_DIZZY || w->act_st == N_ESCAPE || w->act_st == N_FLY ) return 0 ;
	if( ( w->act_st == N_GUARD && ( w->guard_timer || w->routine_2 == 3 ) )
	 || ( w->act_st == N_AIR_GUARD && ( w->guard_timer || w->routine_2 == 2 ) ) ) return 0 ;
	return 1 ;
}

/*==============================================================*/
/*			A side's team hyper combo: the partners leave		*/
/*			once their part is done								*/
/*==============================================================*/
static void team_end_ck( PLAYER_WORK *pl_w, PLAYER_WORK *w1, PLAYER_WORK *w2 )
{
	Uint8 bits ;
	Sint8 cnt ;

	if( !pl_w->be_flag ){
		pl_w->team_ng_timer = 60 ;
		pl_w->be_flag = 1 ;
		pl_w->routine_0 = PL_FIGHT ;
		pl_w->routine_1 = PL_NORM ;
		act_st_set( pl_w, N_STAND ) ;
		return ;
	}
	if( !w1->be_flag && !w2->be_flag ){
		vs_sys_work.team_delay_flag &= ~( 1 << pl_w->pl_no ) ;
		if( pl_w->team_type != 4 ){
			vs_sys_work.team_hyper_flag &= ~( 1 << pl_w->pl_no ) ;
			vs_sys_work.team_hyper_bit[pl_w->pl_no] = 0 ;
			return ;
		}
	}
	cnt = 0 ;
	bits = vs_sys_work.team_hyper_bit[pl_w->pl_no] ;
	if( bits & 1 && !pl_w->routine_1 && pl_w->act_st != N_SUPER && pl_w->act_st != N_SPECIAL ) cnt ++ ;
	if( w1->be_flag && !w1->routine_1 && w1->act_st != N_CHANGE && w1->act_st != N_DOWN ){
		if( partner_neutral_ck( w1 ) ){
			if( bits & 2 ) cnt ++ ;
			if( w1->team_type == 8 && !counter_tag_out_pending[w1->work_no] ){
				w1->change_type = 11 ;
			} else {
				counter_tag_out_pending[w1->work_no] = 0 ;
				w1->change_type = 2 ;
				w1->dm_flag = 0 ;
				w1->catch_ng_timer = 3 ;
			}
			act_st_set( w1, N_CHANGE ) ;
			w1->routine_2 = w1->routine_3 = 0 ;
			combo_reset( w1 ) ;
		}
	}
	if( w2->be_flag && !w2->routine_1 && w2->act_st != N_CHANGE && w2->act_st != N_DOWN ){
		if( partner_neutral_ck( w2 ) ){
			if( bits & 4 ) cnt ++ ;
			if( w2->team_type == 8 && !counter_tag_out_pending[w2->work_no] ){
				w2->change_type = 11 ;
			} else {
				counter_tag_out_pending[w2->work_no] = 0 ;
				w2->change_type = 2 ;
				w2->dm_flag = 0 ;
				w2->catch_ng_timer = 3 ;
			}
			act_st_set( w2, N_CHANGE ) ;
			w2->routine_2 = w2->routine_3 = 0 ;
			combo_reset( w2 ) ;
		}
	}
	if( !( pl_w->v_combi_cnt -= cnt ) ){
		vs_sys_work.team_hyper_flag &= ~( 1 << pl_w->pl_no ) ;
		vs_sys_work.team_delay_flag &= ~( 1 << pl_w->pl_no ) ;
	}
}

/*==============================================================*/
/*			Both sides, until the fight is over					*/
/*==============================================================*/
static void team_end_ck_all( void )
{
	PLAYER_WORK *w0, *w1, *w2 ;

	if( vs_sys_work.vs_st < VS_END_WAIT ){
		w0 = vs_sys_work.team_w[0][0] ;
		w1 = vs_sys_work.team_w[0][1] ;
		w2 = vs_sys_work.team_w[0][2] ;
		team_end_ck( w0, w1, w2 ) ;
		w0 = vs_sys_work.team_w[1][0] ;
		w1 = vs_sys_work.team_w[1][1] ;
		w2 = vs_sys_work.team_w[1][2] ;
		team_end_ck( w0, w1, w2 ) ;
	}
}

#ifdef TARGET_DC
/*==============================================================*/
/*			Training: the gauges fill by themselves				*/
/*==============================================================*/
static void gauge_auto_add( void )
{
	if( !Sys->auto_gauge ) return ;
	if( vs_sys_w->game_stop ) return ;
	pl_gauge_add( vs_sys_w->team_w[0][0], 4 ) ;
	pl_gauge_add( vs_sys_w->team_w[1][0], 4 ) ;
}
#endif

/*==============================================================*/
/*			The sound when the hyper combo's screen effect		*/
/*			ends												*/
/*==============================================================*/
static void hyper_bg_se_ck( void )
{
	PLAYER_WORK *w ;
	Uint8 tm[2] ;	/* the players' hyper_bg_tm this frame, and hyper_bg_req last frame */
	int side ;
	int i ;

	tm[1] = Sys->hyper_bg ;
	tm[0] = 0 ;
	for( i = 0 ; i < 6 ; i ++ ){
		w = &pl_work[i] ;
		if( w->be_flag ) tm[0] |= w->hyper_bg_tm ;
	}
	if( !tm[0] && tm[1] ){
		side = vs_sys_w->hyper_bg_req >> 4 & 1 ;
		pl_se_req( vs_sys_w->team_w[side][0], 31 ) ;
		vs_sys_w->hyper_bg_req = 0 ;
	}
	Sys->hyper_bg = vs_sys_w->hyper_bg_req ;
}

/*==============================================================*/
/*	End of File.												*/
/*==============================================================*/
/* "pl_team.c" End of File	*/
