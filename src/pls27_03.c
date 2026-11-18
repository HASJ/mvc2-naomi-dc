/* Ken's player effect 03: the flame of his air dash. The fire of his Shinryuuken (Char_Effect 4,
   drawn from his own sprites), turned to point opposite the way he moves, like an engine being
   revved; it goes out when the dash ends. */
#include "player.h"
#include "pl_char.h"
#include "nl.h"

static void pls27_03_main( PLAYER_WORK *ew );
static void pls27_03_pos_set( PLAYER_WORK *ew );

/****************************************************************/
PLAYER_WORK *pls27_03_set( PLAYER_WORK *pl_w )
{
	PLAYER_WORK *ew ;

	if( ( ew = work_get( 0, W_PLS, LIST_TOP ) ) ){
		ew->work_func = pls27_03_main ;
		ew->master_w = pl_w ;
		ew->ef_no = 0x2703 ;
		ew->routine_0 = 0 ;
	}
	return ew ;
}

/*==============================================================*/
/*			The flame's sprite points up; rot[2] (a 16-bit		*/
/*			angle, screen y down) turns it to point against		*/
/*			his speed. Its base sits at his middle, a little	*/
/*			behind him											*/
/*==============================================================*/
static void pls27_03_pos_set( PLAYER_WORK *ew )
{
	PLAYER_WORK *pl_w = ew->master_w ;
	Uint16 ang ;
	Float fx, fy ;

	if( pl_w->spd.x != 0.0f || pl_w->spd.y != 0.0f ){
		ang = ( Uint16 )( nlAtan256( pl_w->spd.x, -pl_w->spd.y ) << 8 ) ;
		ew->rot[2] = ang ;
	}
	ang = ( Uint16 )ew->rot[2] ;
	fx = -nlSin( ang ) ;	/* the way the flame points, in the world (y up) */
	fy = nlCos( ang ) ;
	ew->disp_dir = 0 ;
	ew->pos.x = pl_w->pos.x + fx * DOT_X( 12.0f ) ;
	ew->pos.y = pl_w->pos.y + DOT_Y( 45.0f ) + fy * DOT_Y( 12.0f ) ;
	ew->pos.z = pl_w->pos.z ;
}

/*==============================================================*/
static void pls27_03_main( PLAYER_WORK *ew )
{
	PLAYER_WORK *pl_w = ew->master_w ;

	if( pl_w->routine_1 != PL_NORM || pl_w->act_st != N_AIR_DASH || pl_w->pl_type != PL_KEN ){
		ew->disp_flag = 0 ;
		work_free( ew ) ;
		return ;
	}
	if( !ew->routine_0 ){
		ew->routine_0 ++ ;
		CHAR_PART_COPY( ew, pl_w ) ;
		ew->pl_no = pl_w->pl_no ;
		ew->pl_type = pl_w->pl_type ;
		ew->atck_level = pl_w->atck_level ;
		ew->work_no = pl_w->work_no ;
		ew->col_cnt = pl_w->col_cnt ;
		ew->scale = pl_w->scale ;
		ew->scale.x *= 0.55f ;
		ew->scale.y *= 0.55f ;
		ew->prio = pl_w->prio ;
		ew->rot_ofs_x = 0 ;
		ew->rot_ofs_y = 0 ;
		ew->rot[2] = 0 ;
		pl_char_set( ew, Char_Effect, 4 ) ;
		pls27_03_pos_set( ew ) ;
		ew->disp_flag = 1 ;
		return ;
	}
	ew->disp_flag = 1 ;
	pls27_03_pos_set( ew ) ;
	pl_char_move( ew ) ;
}

/*==============================================================*/
/*	End of File.												*/
/*==============================================================*/
/* "pls27_03.c" End of File	*/
