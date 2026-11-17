/* Ground bounce and wall bounce system mechanics and per-move trigger table. */
#include "player.h"
#include "pl_char.h"
#include "pl_act.h"
#include "effect.h"
#include "pl_bounce.h"

extern void dm_voice_req( PLAYER_WORK *pl_w );

typedef enum BOUNCE_MODE {
	BOUNCE_MODE_NONE = 0,
	BOUNCE_MODE_GROUND_FALL,		/* knocked toward floor before ground bounce */
	BOUNCE_MODE_GROUND_RISE,		/* bounced off floor back into the air */
	BOUNCE_MODE_WALL_FLY,			/* flying horizontally toward stage/camera wall */
	BOUNCE_MODE_WALL_REBOUND,		/* bounced off wall back toward center */
	BOUNCE_MODE_WALL_NO_BOUNCE_FLY,	/* spent wall bounce: flies toward wall, then drops */
	BOUNCE_MODE_WALL_NO_BOUNCE_FALL,/* spent wall bounce: falling from wall to floor */
} BOUNCE_MODE;

typedef struct PL_BOUNCE_STATE {
	Uint8 gb_used;		/* 1 once a ground bounce has been used in the current combo */
	Uint8 wb_used;		/* 1 once a wall bounce has been used in the current combo */
	Uint8 mode;			/* active BOUNCE_MODE while in PL_DAMAGE */
	Sint8 wall_dir;		/* +1 moving toward WALL_RIGHT, -1 moving toward WALL_LEFT */
	Uint8 char_switched;/* 1 once airborne rebound anim transitioned rising -> falling */
	Float bounce_height;/* configured rebound Y velocity in DOT_Y units (0 = default) */
} PL_BOUNCE_STATE;

typedef struct BOUNCE_RULE {
	Uint8 pl_type;
	Uint8 atck_no;
	Uint8 need_air;			/* 1: trigger only against airborne / non-grounded targets */
	Uint8 prop;				/* BOUNCE_PROP */
	Uint8 no_fly_screen;	/* 1: suppress flying screen on matching hit */
	Float bounce_height;	/* rebound Y velocity in DOT_Y units (0 = default) */
} BOUNCE_RULE;

static PL_BOUNCE_STATE bounce_state[6] ;

static const BOUNCE_RULE bounce_rules[] = {
	{ PL_AKUMA,   64,            1, BOUNCE_PROP_GROUND, 1, 8.0f },	/* Akuma j236p (Zankuu Hadouken fireball) */
	{ PL_AKUMA,   65,            1, BOUNCE_PROP_GROUND, 1, 8.0f },	/* Akuma j236p (Zankuu Hadouken level 1) */
	{ PL_MAGNETO, ATCK_STAND_HP, 1, BOUNCE_PROP_WALL,   1, 6.5f },	/* Magneto 5HP vs non-grounded opponents */
} ;

static void bounce_clear_motion( Uint8 work_no )
{
	if( work_no >= 6 ) return ;
	bounce_state[work_no].mode = BOUNCE_MODE_NONE ;
	bounce_state[work_no].wall_dir = 0 ;
	bounce_state[work_no].char_switched = 0 ;
	bounce_state[work_no].bounce_height = 0.0f ;
}

static void bounce_quake( const PLAYER_WORK *pl_w )
{
	Sint8 lv = pl_w->weight < 5 ? 1 : 3 ;

	scr_work.quake_level = lv ;
	scr_work.quake_flag = 1 ;
}

static const BOUNCE_RULE *bounce_rule_find( const PLAYER_WORK *dm_w, const PLAYER_WORK *at_w )
{
	Uint8 atck_no ;
	int i ;
	int count ;

	if( !dm_w || !at_w ) return 0 ;
	/* Do not hijack throws, holds, snapbacks, dizzy stuns, or armored/sandbag states. */
	if( dm_w->snap_partner || dm_w->dm_no == 3 || dm_w->dm_no == 5 || dm_w->dm_no == 6 || dm_w->dm_no == 12 || dm_w->dm_no == 19 ) return 0 ;

	atck_no = ( Uint8 )( dm_w->dm_atck_no & ATCK_NO_MASK ) ;
	count = ( int )( sizeof( bounce_rules ) / sizeof( bounce_rules[0] ) ) ;
	for( i = 0 ; i < count ; i ++ ){
		const BOUNCE_RULE *rule = &bounce_rules[i] ;

		if( rule->prop == BOUNCE_PROP_NONE ) continue ;
		if( at_w->pl_type != rule->pl_type || atck_no != rule->atck_no ) continue ;
		if( rule->need_air && dm_w->pl_stance != STANCE_AIR ) continue ;
		return rule ;
	}
	return 0 ;
}

static Float bounce_away_dir( const PLAYER_WORK *dm_w, const PLAYER_WORK *at_w )
{
	( void )at_w ;
	if( dm_w->back_spd_x > 0.0f ) return 1.0f ;
	if( dm_w->back_spd_x < 0.0f ) return -1.0f ;
	return dm_w->dm_at_dir ? -1.0f : 1.0f ;
}

/* Orient spinning damage sprite so the head points toward the direction of travel. */
static void bounce_spin_face_dir( PLAYER_WORK *pl_w )
{
	if( pl_w->spd.x > 0.0f ) pl_w->disp_dir = 1 ;
	else if( pl_w->spd.x < 0.0f ) pl_w->disp_dir = 0 ;
}

static void bounce_wall_vfx( PLAYER_WORK *pl_w, Sint8 wall_dir )
{
	VEC3 vfx_pos ;

	vfx_pos.x = ( pl_w->wall_flag & WALL_RIGHT ) ? scr_work.scr_right : scr_work.scr_left ;
	vfx_pos.y = pl_w->pos.y + ( pl_w->size_top / 2 ) * pl_w->scale.y * 2.142857f ;
	vfx_pos.z = pl_w->pos.z ;
	shock_wall_set( &vfx_pos, wall_dir ) ;
}

static void bounce_wall_check_sfx( PLAYER_WORK *pl_w )
{
	if( pl_w->wall_flag && !pl_w->wall_hit_flag ){
		pl_w->wall_hit_flag = 1 ;
		down_se_req( pl_w ) ;
		wall_dust_set( pl_w ) ;
	}
}

static void bounce_finish_to_down( PLAYER_WORK *pl_w )
{
	bounce_clear_motion( pl_w->work_no ) ;
	pl_w->spd.y = 0.0f ;
	pl_w->acc.y = 0.0f ;
	pl_w->dm_no = 2 ;
	pl_w->routine_2 = 2 ;		/* hand off to dm_buttobi_down */
	pl_w->pl_stance = STANCE_DOWN ;
	pl_w->ukemi_flag = 120 ;	/* untouched end of bounce settles as a soft knockdown */
	pl_char_set( pl_w, Char_Damage, 26 ) ;
}

static void bounce_arc_move( PLAYER_WORK *pl_w, PL_BOUNCE_STATE *st, int apply_x_drag, Uint8 fall_char )
{
	dm_blow_move( pl_w ) ;
	if( apply_x_drag ){
		pl_w->spd.x *= 0.96f ;
	}
	pl_char_move( pl_w ) ;
	bounce_wall_check_sfx( pl_w ) ;
	if( !st->char_switched && pl_w->spd.y <= 0.0f ){
		st->char_switched = 1 ;
		pl_char_set( pl_w, Char_Damage, fall_char ) ;
	}
	if( pl_w->spd.y <= 0.0f && pl_land_ck( pl_w ) ){
		land_se_req( pl_w ) ;
		land_dust_set( &pl_w->pos, pl_w->pl_no ) ;
		bounce_finish_to_down( pl_w ) ;
	}
}

static void bounce_wall_fly_move( PLAYER_WORK *pl_w, PL_BOUNCE_STATE *st, int do_bounce )
{
	Uint8 hit_expected_wall ;

	hit_expected_wall = ( st->wall_dir > 0 && ( pl_w->wall_flag & WALL_RIGHT ) )
	                 || ( st->wall_dir < 0 && ( pl_w->wall_flag & WALL_LEFT ) ) ;
	if( hit_expected_wall ){
		Float rebound_y ;

		pl_w->wall_hit_flag = 1 ;
		down_se_req( pl_w ) ;
		pl_se_req( pl_w, 50 ) ;
		dm_voice_req( pl_w ) ;
		wall_dust_set( pl_w ) ;
		bounce_wall_vfx( pl_w, st->wall_dir ) ;
		/* Stop spinning and face back into the stage with a natural knockback pose. */
		pl_w->disp_dir = st->wall_dir > 0 ? 1 : 0 ;
		if( do_bounce ){
			bounce_quake( pl_w ) ;
			pl_w->spd.x = st->wall_dir > 0 ? -DOT_X( 6.5f ) : DOT_X( 6.5f ) ;
			pl_w->acc.x = 0.0f ;
			rebound_y = st->bounce_height > 0.0f
			          ? st->bounce_height
			          : ( pl_w->pos.y > pl_w->ground_y + DOT_Y( 90.0f ) ? 5.5f : 7.5f ) ;
			pl_w->spd.y = DOT_Y( rebound_y ) ;
			pl_w->acc.y = -DOT_Y( 0.375f ) ;
			pl_w->pl_stance = STANCE_AIR ;
			st->mode = BOUNCE_MODE_WALL_REBOUND ;
			st->char_switched = 0 ;
			pl_char_set( pl_w, Char_Damage, 17 ) ;
		} else {
			/* Wall bounce already used: drop down from the wall into a soft knockdown. */
			pl_w->spd.x = 0.0f ;
			pl_w->acc.x = 0.0f ;
			pl_w->spd.y = 0.0f ;
			pl_w->acc.y = -DOT_Y( 0.375f ) ;
			pl_w->ukemi_flag = 120 ;
			st->mode = BOUNCE_MODE_WALL_NO_BOUNCE_FALL ;
			st->char_switched = 1 ;
			pl_char_set( pl_w, Char_Damage, 22 ) ;
		}
		dm_blow_move( pl_w ) ;
		pl_char_move( pl_w ) ;
		return ;
	}
	dm_blow_move( pl_w ) ;
	pl_char_move( pl_w ) ;
	if( pl_w->spd.y <= 0.0f && pl_land_ck( pl_w ) ){
		land_se_req( pl_w ) ;
		land_dust_set( &pl_w->pos, pl_w->pl_no ) ;
		bounce_finish_to_down( pl_w ) ;
	}
}

void pl_bounce_reset_all( void )
{
	int i ;

	for( i = 0 ; i < 6 ; i ++ ){
		bounce_state[i].gb_used = 0 ;
		bounce_state[i].wb_used = 0 ;
		bounce_clear_motion( ( Uint8 )i ) ;
	}
}

void pl_bounce_combo_reset( const PLAYER_WORK *pl_w )
{
	if( !pl_w || pl_w->work_no >= 6 ) return ;
	bounce_state[pl_w->work_no].gb_used = 0 ;
	bounce_state[pl_w->work_no].wb_used = 0 ;
	bounce_clear_motion( pl_w->work_no ) ;
}

int pl_bounce_on_damage_pre( PLAYER_WORK *dm_w, const PLAYER_WORK *at_w, BOUNCE_PROP *prop_out, Uint8 *no_fly_screen_out )
{
	const BOUNCE_RULE *rule ;

	if( prop_out ) *prop_out = BOUNCE_PROP_NONE ;
	if( no_fly_screen_out ) *no_fly_screen_out = 0 ;
	if( !dm_w || dm_w->work_no >= 6 ) return 0 ;

	/* Any new hit cancels an in-progress bounce trajectory while preserving combo resource usage. */
	bounce_clear_motion( dm_w->work_no ) ;
	rule = bounce_rule_find( dm_w, at_w ) ;
	if( !rule ) return 0 ;

	if( prop_out ) *prop_out = ( BOUNCE_PROP )rule->prop ;
	if( no_fly_screen_out ) *no_fly_screen_out = rule->no_fly_screen ;
	bounce_state[dm_w->work_no].bounce_height = rule->bounce_height ;
	/* Use dm_buttobi_ng (2) so dm_air_combo_ck cannot set fly_screen_flag on AERIAL_FINISH hits. */
	if( rule->prop != BOUNCE_PROP_NONE ){
		dm_w->dm_no = 2 ;
		return 1 ;
	}
	return 0 ;
}

void pl_bounce_on_damage_post( PLAYER_WORK *dm_w, const PLAYER_WORK *at_w, BOUNCE_PROP prop, Uint8 no_fly_screen )
{
	PL_BOUNCE_STATE *st ;
	Float dir ;
	int is_forced ;

	if( !dm_w || dm_w->work_no >= 6 ) return ;
	if( no_fly_screen || prop != BOUNCE_PROP_NONE ){
		dm_w->fly_screen_flag = 0 ;
	}
	if( prop == BOUNCE_PROP_NONE ) return ;

	st = &bounce_state[dm_w->work_no] ;
	dir = bounce_away_dir( dm_w, at_w ) ;

	switch( prop ){
	case BOUNCE_PROP_GROUND:
	case BOUNCE_PROP_GROUND_FORCED:
		is_forced = ( prop == BOUNCE_PROP_GROUND_FORCED ) ;
		dm_w->recover_ng_flag = 1 ;
		dm_w->spd.x = dir * DOT_X( 3.0f ) ;
		dm_w->acc.x = 0.0f ;
		dm_w->spd.y = -DOT_Y( 5.0f ) ;
		dm_w->acc.y = -DOT_Y( 0.375f ) ;
		bounce_spin_face_dir( dm_w ) ;
		if( is_forced || !st->gb_used ){
			st->gb_used = 1 ;
			st->mode = BOUNCE_MODE_GROUND_FALL ;
			st->wall_dir = 0 ;
			st->char_switched = 0 ;
			dm_w->ukemi_flag = 0 ;
			pl_char_set( dm_w, Char_Damage, 29 ) ;
		} else {
			/* Already spent in this combo: falls directly into knockdown without bouncing. */
			bounce_clear_motion( dm_w->work_no ) ;
		}
		break ;

	case BOUNCE_PROP_WALL:
	case BOUNCE_PROP_WALL_FORCED:
		is_forced = ( prop == BOUNCE_PROP_WALL_FORCED ) ;
		dm_w->recover_ng_flag = 1 ;
		dm_w->wall_ng_timer = 0 ;
		if( dm_w->pos.y < dm_w->ground_y + 8.0f ){
			dm_w->pos.y = dm_w->ground_y + 8.0f ;
		}
		dm_w->spd.x = dm_w->back_spd_x ;
		dm_w->acc.x = 0.0f ;
		dm_w->spd.y = dm_w->back_spd_y ;
		dm_w->acc.y = -DOT_Y( 0.375f ) ;
		st->wall_dir = dir > 0.0f ? 1 : -1 ;
		st->char_switched = 0 ;
		bounce_spin_face_dir( dm_w ) ;
		pl_char_set( dm_w, Char_Damage, 28 ) ;
		if( is_forced || !st->wb_used ){
			st->wb_used = 1 ;
			st->mode = BOUNCE_MODE_WALL_FLY ;
			dm_w->ukemi_flag = 0 ;
		} else {
			st->mode = BOUNCE_MODE_WALL_NO_BOUNCE_FLY ;
		}
		break ;

	default:
		break ;
	}
}

int pl_bounce_dm_move( PLAYER_WORK *pl_w )
{
	PL_BOUNCE_STATE *st ;

	if( !pl_w || pl_w->work_no >= 6 ) return 0 ;
	st = &bounce_state[pl_w->work_no] ;
	if( st->mode == BOUNCE_MODE_NONE ) return 0 ;
	if( pl_w->routine_1 != PL_DAMAGE || pl_w->dm_no != 2 ){
		bounce_clear_motion( pl_w->work_no ) ;
		return 0 ;
	}

	if( vs_sys_work.vs_st < VS_END_WAIT && pl_w->ko_flag && pl_w->wall_hit_flag ){
		pl_w->catch_ng_timer = 2 ;
	}

	switch( st->mode ){
	case BOUNCE_MODE_GROUND_FALL:
		dm_blow_move( pl_w ) ;
		pl_char_move( pl_w ) ;
		bounce_wall_check_sfx( pl_w ) ;
		if( pl_w->spd.y <= 0.0f && pl_land_ck( pl_w ) ){
			Float rise_y = st->bounce_height > 0.0f ? st->bounce_height : 8.0f ;

			down_se_req( pl_w ) ;
			pl_se_req( pl_w, 50 ) ;
			dm_voice_req( pl_w ) ;
			land_dust_set( &pl_w->pos, pl_w->pl_no ) ;
			shock_set( &pl_w->pos, 0 ) ;
			bounce_quake( pl_w ) ;
			pl_w->spd.x *= 0.4f ;
			pl_w->acc.x = 0.0f ;
			pl_w->spd.y = DOT_Y( rise_y ) ;
			pl_w->acc.y = -DOT_Y( 0.375f ) ;
			pl_w->pl_stance = STANCE_AIR ;
			st->mode = BOUNCE_MODE_GROUND_RISE ;
			st->char_switched = 0 ;
			bounce_spin_face_dir( pl_w ) ;
			pl_char_set( pl_w, Char_Damage, 28 ) ;
		}
		return 1 ;

	case BOUNCE_MODE_GROUND_RISE:
		bounce_arc_move( pl_w, st, 0, 29 ) ;
		return 1 ;

	case BOUNCE_MODE_WALL_FLY:
		bounce_wall_fly_move( pl_w, st, 1 ) ;
		return 1 ;

	case BOUNCE_MODE_WALL_NO_BOUNCE_FLY:
		bounce_wall_fly_move( pl_w, st, 0 ) ;
		return 1 ;

	case BOUNCE_MODE_WALL_REBOUND:
		bounce_arc_move( pl_w, st, 1, 22 ) ;
		return 1 ;

	case BOUNCE_MODE_WALL_NO_BOUNCE_FALL:
		bounce_arc_move( pl_w, st, 0, 22 ) ;
		return 1 ;

	default:
		bounce_clear_motion( pl_w->work_no ) ;
		return 0 ;
	}
}
