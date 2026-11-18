/* DM0C, the home game's main menu: the ring of mode lines, the cursor and the mode's title, and
   the wall paper behind the menus, its texture scrolled. */
#include "player.h"
#include "shinobi.h"
#include "dm.h"

/* the uv scroll of a model */

void dm0c_menu_move( PLAYER_WORK *w ) ;
void dm0c_line_move( PLAYER_WORK *w ) ;
void dm0c_line_sub_move( PLAYER_WORK *w ) ;
void dm0c_cursor_move( PLAYER_WORK *w ) ;
void dm0c_title_move( PLAYER_WORK *w ) ;
void wall_paper_move( PLAYER_WORK *w ) ;
void dm0c_mark_move( PLAYER_WORK *w ) ;

static VEC3 dm0c_zero = { 0.0f, 0.0f, 0.0f } ;
static VEC3 dm0c_one = { 1.0f, 1.0f, 1.0f } ;
static FADE_COL dm0c_fade = { 1.0f, 1.0f, 1.0f, 1.0f } ;
static VEC3 menu_pos_tbl[9] = {	/* the mode lines on their ring */
	{ -9.2f, 5.3f, -12.9f }, { -3.4f, 6.0f, -12.9f }, { 2.0f, 5.8f, -12.9f },
	{ 6.6f, 4.8f, -12.9f }, { 10.5f, 3.0f, -12.9f }, { 8.7f, -5.5f, -12.9f },
	{ 2.0f, -6.0f, -12.9f }, { -4.0f, -5.3f, -12.9f }, { -9.2f, -3.8f, -12.9f }
} ;
static VEC3 menu_scale_tbl[18] = {	/* a line's scale, and then its two highlights' */
	{ 1.0f, 1.0f, 1.0f }, { 0.9f, 0.9f, 1.0f }, { 0.9f, 0.9f, 1.0f },
	{ 0.9f, 0.9f, 1.0f }, { 0.9f, 0.9f, 1.0f }, { 1.0f, 1.0f, 1.0f },
	{ 0.9f, 0.9f, 1.0f }, { 0.9f, 0.9f, 1.0f }, { 0.9f, 0.9f, 1.0f },
	{ 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f }, { 0.90000004f, 0.8888889f, 1.0f },
	{ 0.81111115f, 0.7777778f, 1.0f }, { 0.73333335f, 0.7777778f, 1.0f }, { 1.0f, 1.0f, 1.0f },
	{ 1.0f, 1.0f, 1.0f }, { 0.90000004f, 0.8888889f, 1.0f }, { 0.81111115f, 0.7777778f, 1.0f }
} ;
static VEC3 menu_cursor_tbl[2][9] = {	/* the cursor's scale on each line */
	{	{ 1.0f, 1.0f, 1.0f }, { 0.9f, 0.9f, 1.0f }, { 0.81f, 0.8f, 1.0f },
		{ 0.73f, 0.7f, 1.0f }, { 0.66f, 0.7f, 1.0f }, { 1.0f, 1.0f, 1.0f },
		{ 0.9f, 0.9f, 1.0f }, { 0.81f, 0.8f, 1.0f }, { 0.73f, 0.7f, 1.0f } },
	{	{ 1.25f, -0.5f, -13.0f }, { 5.5f, 0.0f, -13.0f }, { 3.5f, 0.0f, -13.0f },
		{ 2.5f, 0.0f, -13.0f }, { 4.0f, 0.0f, -13.0f }, { 1.25f, 0.0f, -13.0f },
		{ 1.75f, 0.0f, -13.0f }, { 4.0f, 0.0f, -13.0f }, { 1.75f, 0.0f, -13.0f } }
} ;
static VEC3 menu_title_pos[3] = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, -13.0f }, { 0.0f, 0.0f, -13.2f } } ;
static VEC3 wall_col_tbl[10] = {	/* the wall paper's colour, by its number */
	{ 1.0f, 1.0f, 1.0f }, { 0.97f, 0.57f, 0.67f }, { 0.97f, 0.57f, 0.67f }, { 0.37f, 0.13f, 1.0f },
	{ 1.0f, 0.6f, 0.0f }, { 1.0f, 0.6f, 0.0f }, { 1.0f, 0.6f, 0.0f }, { 1.0f, 0.2f, 0.2f },
	{ 1.0f, 1.0f, 1.0f }, { 0.95f, 0.55f, 0.65f }
} ;
static Sint8 menu_title_tbl[9] = { 23, 24, 25, 26, 27, 28, 31, 29, 30 } ;	/* the mode's title model */
static Float wall_uv_tbl[4] = { 0.001f, 0.001f, 0.0005f, 0.0005f } ;	/* the scroll each frame, u and v, of each layer */
static Sint32 wall_time_tbl[4] = { 1000, 1000, 2000, 2000 } ;	/* and the frames it takes */
static void ( *dm0c_menu_jmp[10] )( PLAYER_WORK *w ) = {
	dm0c_line_move, dm0c_line_sub_move, dm0c_line_sub_move, dm0c_cursor_move,
	dm0c_title_move, wall_paper_move, dm0c_mark_move
} ;

#ifdef HOST_PORT
#include "dm0c_netplay_art.h"

#define DM0C_NETPLAY_TEX	0xfe1		/* common texture slot (sel_label.c uses 0xfe0) */
#define DM0C_NETPLAY_W		256
#define DM0C_NETPLAY_H		128
#define DM0C_NETPLAY_FMT	0x902		/* KM_RECTANGLE | RD_ARGB4444 */
#define DM0C_NETPLAY_TCW	( ( 2u << 27 ) | ( 1u << 26 ) | DM0C_NETPLAY_TEX )

static Uint16 dm0c_netplay_texels[DM0C_NETPLAY_W * DM0C_NETPLAY_H] ;
static NLthd dm0c_netplay_thd[2] = {
	{ DM0C_NETPLAY_W, DM0C_NETPLAY_H, DM0C_NETPLAY_FMT, dm0c_netplay_texels, 0 },
	{ 0, 0, -1, 0, 0 },
} ;
static Uint32 dm0c_netplay_line_obj[61] ;
static Uint32 dm0c_netplay_title_obj[95] ;

static Uint32 dm0c_f32_bits( Float f )
{
	union { Float f ; Uint32 u ; } u ;

	u.f = f ;
	return u.u ;
}

static Uint32 *dm0c_put_vtx( Uint32 *p, Float x, Float y, Float z, Uint32 w3, Uint32 argb, Uint32 w5, Float u, Float v )
{
	p[0] = dm0c_f32_bits( x ) | 1u ;	/* bit 0 tells nl_obj record() this is a 32-byte vertex */
	p[1] = dm0c_f32_bits( y ) ;
	p[2] = dm0c_f32_bits( z ) ;
	p[3] = w3 ;
	p[4] = argb ;
	p[5] = w5 ;
	p[6] = dm0c_f32_bits( u ) ;
	p[7] = dm0c_f32_bits( v ) ;
	return p + 8 ;
}

static Uint32 *dm0c_put_quad( Uint32 *p, Uint32 hdr, Float x0, Float x1, Float y0, Float y1, Float z,
                              Uint32 w3, Uint32 argb0, Uint32 w5_0, Uint32 argb1, Uint32 w5_1,
                              Float u0, Float u1, Float v_top, Float v_bot )
{
	p[0] = hdr ;
	p[1] = 4u ;
	p += 2 ;
	p = dm0c_put_vtx( p, x0, y0, z, w3, argb0, w5_0, u0, v_bot ) ;
	p = dm0c_put_vtx( p, x0, y1, z, w3, argb0, w5_0, u0, v_top ) ;
	p = dm0c_put_vtx( p, x1, y0, z, w3, argb1, w5_1, u1, v_bot ) ;
	p = dm0c_put_vtx( p, x1, y1, z, w3, argb1, w5_1, u1, v_top ) ;
	return p ;
}

static Uint16 dm0c_argb4444( Uint8 b )
{
	return ( Uint16 )( ( ( b >> 4 ) << 12 ) | ( ( b & 15 ) * 0x0111u ) ) ;
}

static void dm0c_netplay_init( void )
{
	Uint32 *p ;
	Float u0, u1, vt, vb ;
	int i, x, y ;

	/* top half transparent black outside ring glyph outlines; lower rows transparent white around banner text */
	for( y = 0 ; y < 32 ; y ++ ){
		for( x = 0 ; x < DM0C_NETPLAY_W ; x ++ ) dm0c_netplay_texels[y * DM0C_NETPLAY_W + x] = 0x0000u ;
	}
	for( y = 32 ; y < DM0C_NETPLAY_H ; y ++ ){
		for( x = 0 ; x < DM0C_NETPLAY_W ; x ++ ) dm0c_netplay_texels[y * DM0C_NETPLAY_W + x] = 0x0fffu ;
	}
	for( y = 0 ; y < DM0C_NP_RING_H ; y ++ ){
		for( x = 0 ; x < DM0C_NP_RING_W ; x ++ ){
			dm0c_netplay_texels[( 4 + y ) * DM0C_NETPLAY_W + 4 + x] = dm0c_argb4444( dm0c_netplay_ring[y * DM0C_NP_RING_W + x] ) ;
		}
	}
	for( y = 0 ; y < DM0C_NP_DESC_H ; y ++ ){
		for( x = 0 ; x < DM0C_NP_DESC_W ; x ++ ){
			dm0c_netplay_texels[( 36 + y ) * DM0C_NETPLAY_W + 4 + x] = dm0c_argb4444( dm0c_netplay_desc1[y * DM0C_NP_DESC_W + x] ) ;
			dm0c_netplay_texels[( 64 + y ) * DM0C_NETPLAY_W + 4 + x] = dm0c_argb4444( dm0c_netplay_desc2[y * DM0C_NP_DESC_W + x] ) ;
		}
	}

	/* ring label model (replaces obj_dm0c->obj[15]) */
	p = dm0c_netplay_line_obj ;
	p[0] = 1u ;
	p[1] = 3u ;
	p[2] = 0x3d75c380u ;
	p[3] = 0x3df5c320u ;
	p[4] = 0u ;
	p[5] = 0x3fffedf8u ;
	p += 6 ;
	p[0] = 0x8200001cu ;
	p[1] = 0x83000000u ;
	p[2] = 0x9401a46du ;
	p[3] = DM0C_NETPLAY_TCW ;
	p[4] = 0x3d75c380u ;
	p[5] = 0x3df5c320u ;
	p[6] = 0u ;
	p[7] = 0x3fffedf8u ;
	p[8] = 0u ;
	p[9] = 0xfffffffdu ;
	p[10] = 0x3f400000u ;
	for( i = 11 ; i < 19 ; i ++ ) p[i] = ( i == 15 ) ? 0u : 0x3f800000u ;
	p[19] = 136u ;
	p += 20 ;
	u0 = ( 4.0f + 0.5f ) / DM0C_NETPLAY_W ;
	u1 = ( 4.0f + DM0C_NP_RING_W - 0.5f ) / DM0C_NETPLAY_W ;
	vt = ( 4.0f + 0.5f ) / DM0C_NETPLAY_H ;
	vb = ( 4.0f + DM0C_NP_RING_H - 0.5f ) / DM0C_NETPLAY_H ;
	p = dm0c_put_quad( p, 0x56u, -1.90f, 1.90f, -0.45f, 0.45f, 0.0f,
	                   0x7fu, 0xffff6400u, 0xffff6400u, 0xffff9600u, 0xffff9600u,
	                   u0, u1, vt, vb ) ;
	*p = 0u ;

	/* center banner description model (replaces obj_dm0c->obj[29]) */
	p = dm0c_netplay_title_obj ;
	p[0] = 1u ;
	p[1] = 3u ;
	p[2] = 0x3eb7af80u ;
	p[3] = 0xb5000000u ;
	p[4] = 0xc1500000u ;
	p[5] = 0x40a1be98u ;
	p += 6 ;
	p[0] = 0x8200002cu ;
	p[1] = 0x83000000u ;
	p[2] = 0x9401a46du ;
	p[3] = DM0C_NETPLAY_TCW ;
	p[4] = 0x3eb7af80u ;
	p[5] = 0xb5000000u ;
	p[6] = 0xc1500000u ;
	p[7] = 0x40a1be98u ;
	p[8] = 0u ;
	p[9] = 0xffffffffu ;
	p[10] = 0x3f400000u ;
	for( i = 11 ; i < 19 ; i ++ ) p[i] = ( i == 15 ) ? 0u : 0x3f800000u ;
	p[19] = 272u ;
	p += 20 ;
	u0 = ( 4.0f + 0.5f ) / DM0C_NETPLAY_W ;
	u1 = ( 4.0f + DM0C_NP_DESC_W - 0.5f ) / DM0C_NETPLAY_W ;
	vt = ( 36.0f + 0.5f ) / DM0C_NETPLAY_H ;
	vb = ( 36.0f + DM0C_NP_DESC_H - 0.5f ) / DM0C_NETPLAY_H ;
	p = dm0c_put_quad( p, 0x16u, -4.57f, 6.23f, 0.17f, 1.13f, -13.00f,
	                   0u, 0x80000000u, 0x3f800000u, 0x80000000u, 0x3f800000u,
	                   u0, u1, vt, vb ) ;
	vt = ( 64.0f + 0.5f ) / DM0C_NETPLAY_H ;
	vb = ( 64.0f + DM0C_NP_DESC_H - 0.5f ) / DM0C_NETPLAY_H ;
	p = dm0c_put_quad( p, 0x96u, -4.57f, 6.23f, -1.13f, -0.17f, -13.00f,
	                   0u, 0x80000000u, 0x3f800000u, 0x80000000u, 0x3f800000u,
	                   u0, u1, vt, vb ) ;
	*p = 0u ;

	nlSetCommonTextureOffset( DM0C_NETPLAY_TEX ) ;
	nlSpriteLoadTextureOnce( dm0c_netplay_thd, 0, 0 ) ;
	nlSetCommonTextureOffset( 0 ) ;
}
#endif

/****************************************************************/
/*			A piece of the menu: its kind, number and colour	*/
/****************************************************************/
PLAYER_WORK *dm0c_menu_set( Uint8 kind, Uint8 no, Uint8 n )
{
	PLAYER_WORK *w ;

	if( ( w = work_get( 0, W_SYS, LIST_TOP ) ) ){
		w->disp_flag = 0 ;
		w->work_func = dm0c_menu_move ;
		w->sub_no = kind ;
		w->type = no ;
		w->sub_type = n ;
		w->scale = dm0c_one ;
		w->fade = dm0c_fade ;
		w->obj_flag = OBJ_POS | OBJ_SCALE | OBJ_TRNSL | OBJ_FADE ;
	}
	return w ;
}

/****************************************************************/
/*			A piece that rides on another, drawn with its		*/
/*			matrix												*/
/****************************************************************/
PLAYER_WORK *dm0c_menu_sub_set( PLAYER_WORK *mw, Sint8 kind, int no )
{
	PLAYER_WORK *w ;

	if( ( w = work_get( 0, W_SYS, LIST_TOP ) ) ){
		w->disp_flag = 1 ;
		w->work_func = dm0c_menu_move ;
		w->sub_no = kind ;
		w->type = mw->type ;
		w->base_mtx = &mw->obj_mtx ;
		w->obj = obj_dm0c->obj[no] ;
		w->fade = dm0c_fade ;
		w->obj_flag = OBJ_POS | OBJ_SCALE | OBJ_TRNSL | OBJ_FADE ;
	}
	return w ;
}

/****************************************************************/
/*			A mode's line on the ring, with its two highlights	*/
/****************************************************************/
void dm0c_line_move( PLAYER_WORK *w )
{
	PLAYER_WORK *sw ;

	if( !w->routine_0 ){
		if( w->type == HOME_NETWORK ){
			work_free( w ) ;
			return ;
		}
		w->routine_0 ++ ;
		w->disp_flag = 1 ;
#ifdef HOST_PORT
		if( w->type == HOME_SCORE ) w->obj = ( int * )dm0c_netplay_line_obj ;
		else
#endif
		w->obj = obj_dm0c->obj[w->type + 8] ;
		w->pos = menu_pos_tbl[w->type] ;
		w->scale = menu_scale_tbl[w->type] ;
		sw = dm0c_menu_sub_set( w, 1, 6 ) ;
		sw->scale = menu_scale_tbl[w->type + 9] ;
		sw->pos.z = -0.1f ;
		sw = dm0c_menu_sub_set( w, 1, 7 ) ;
		sw->scale = menu_scale_tbl[w->type + 9] ;
		sw->sub_type = 1 ;
		sw->pos.z = -0.12f ;
	}
	if( w->type == HOME_VERSUS ){	/* dim without two pads */
		if( !( port_peri[0]->info->type & PDD_DEVTYPE_CONTROLLER )
			|| !( port_peri[1]->info->type & PDD_DEVTYPE_CONTROLLER ) ) w->fade.r = w->fade.g = w->fade.b = 0.6f ;
		else w->fade.r = w->fade.g = w->fade.b = 1.0f ;
	}
}

/****************************************************************/
/*			A line's highlight: the second is shown on all		*/
/*			lines but the chosen one							*/
/****************************************************************/
void dm0c_line_sub_move( PLAYER_WORK *w )
{
	if( w->sub_type ){
		if( w->type == Sys->home_mode ) w->disp_flag = 0 ;
		else w->disp_flag = 1 ;
	}
	if( w->type == HOME_VERSUS ){
		if( !( port_peri[0]->info->type & PDD_DEVTYPE_CONTROLLER )
			|| !( port_peri[1]->info->type & PDD_DEVTYPE_CONTROLLER ) ) w->fade.r = w->fade.g = w->fade.b = 0.6f ;
		else w->fade.r = w->fade.g = w->fade.b = 1.0f ;
	}
}

/****************************************************************/
/*			The cursor on the chosen line, six frames each		*/
/****************************************************************/
void dm0c_cursor_move( PLAYER_WORK *w )
{
	if( !w->routine_0 ){
		w->routine_0 ++ ;
		w->disp_flag = 1 ;
		w->timer0 = 6 ;
		w->timer1 = 0 ;
	}
	if( -- w->timer0 < 0 ){
		w->timer0 = 6 ;
		w->timer1 = ( w->timer1 + 1 ) % 6 ;
	}
	w->obj = obj_dm0c->obj[w->timer1 + 17] ;
	w->pos = menu_pos_tbl[Sys->home_mode] ;
	w->pos.z = -13.01f ;
	w->scale = menu_cursor_tbl[0][Sys->home_mode] ;
}

/****************************************************************/
/*			The chosen mode's title, and the menu's two			*/
/*			headings											*/
/****************************************************************/
void dm0c_title_move( PLAYER_WORK *w )
{
	if( !w->type ){
		if( !w->routine_0 ){
			w->routine_0 ++ ;
			w->disp_flag = 1 ;
		}
#ifdef HOST_PORT
		if( Sys->home_mode == HOME_SCORE ) w->obj = ( int * )dm0c_netplay_title_obj ;
		else
#endif
		w->obj = obj_dm0c->obj[menu_title_tbl[Sys->home_mode]] ;
		w->pos = dm0c_zero ;
	} else if( !w->routine_0 ){
		w->routine_0 ++ ;
		w->disp_flag = 1 ;
		w->obj = obj_dm0c->obj[4 + ( w->type - 1 )] ;
		w->pos = menu_title_pos[w->type] ;
	}
}

/****************************************************************/
/*			The wall paper's start: its layer's model and		*/
/*			colour												*/
/****************************************************************/
void wall_paper_init( PLAYER_WORK *w, OBJ_FILE *file, int no )
{
	w->routine_0 ++ ;
	w->disp_flag = 1 ;
	w->obj = file->obj[no] ;
	w->timer0 = 0 ;
	w->timer1 = 0 ;
	w->obj_flag = w->obj_flag | OBJ_FADE ;
	obj_uv_clear( w->obj ) ;
	w->work1 = 0 ;
	w->fade.r = wall_col_tbl[w->sub_type].x ;
	w->fade.g = wall_col_tbl[w->sub_type].y ;
	w->fade.b = wall_col_tbl[w->sub_type].z ;
}

/****************************************************************/
/*			The wall paper's texture scrolled, the two layers	*/
/*			opposite ways; the colour changes on the secret		*/
/*			factor's shop											*/
/****************************************************************/
void wall_paper_scroll( PLAYER_WORK *w, OBJ_FILE *file, Uint32 no )
{
	Float u, v ;

	w->timer0 ++ ;
	if( w->timer0 >= wall_time_tbl[no] ) w->timer0 = 0 ;
	w->timer1 ++ ;
	if( w->timer1 >= wall_time_tbl[no + 1] ) w->timer1 = 0 ;
	obj_uv_begin( file->obj[no + 1], w->obj ) ;
	while( !obj_uv_next() ){
		obj_uv_get( &u, &v ) ;
		if( !w->type ){
			u -= 1.0f - wall_uv_tbl[no] * w->timer0 ;
			v -= 1.0f - wall_uv_tbl[no + 1] * w->timer1 ;
		} else {
			u += 1.0f - wall_uv_tbl[no] * w->timer0 ;
			v += 1.0f - wall_uv_tbl[no + 1] * w->timer1 ;
		}
		obj_uv_put( &u, &v ) ;
	}
	if( w->type ) return ;
	if( w->sub_type != 1 ) return ;
	switch( w->routine_1 ){
	case 0 :
		if( 0 ) if( Sys->routine_0 ) break ;	/* left out of the build */
		if( Sys->routine_0 == 3 && Sys->routine_1 == 1 ){
			w->routine_1 ++ ;
			w->work2 = 30 ;
			w->spd.x = ( wall_col_tbl[2].x - w->fade.r ) / 30.0f ;
			w->spd.y = ( wall_col_tbl[2].y - w->fade.g ) / 30.0f ;
			w->spd.z = ( wall_col_tbl[2].z - w->fade.b ) / 30.0f ;
		}
		break ;
	case 1 :
	case 3 :
		if( ( w->work2 = w->work2 - 1 ) < 0 ){
			if( w->routine_1 == 3 ) w->routine_1 = 0 ;
			else w->routine_1 ++ ;
		} else {
			w->fade.r += w->spd.x ;
			w->fade.g += w->spd.y ;
			w->fade.b += w->spd.z ;
		}
		break ;
	case 2 :
		if( Sys->routine_0 != 3 && Sys->routine_1 != 1 ){
			w->routine_1 ++ ;
			w->work2 = 30 ;
			w->spd.x = ( wall_col_tbl[1].x - w->fade.r ) / 30.0f ;
			w->spd.y = ( wall_col_tbl[1].y - w->fade.g ) / 30.0f ;
			w->spd.z = ( wall_col_tbl[1].z - w->fade.b ) / 30.0f ;
		}
		break ;
	}
}

/****************************************************************/
/*			The wall paper: its models from dm0c, dm10 or dm0f	*/
/****************************************************************/
void wall_paper_move( PLAYER_WORK *w )
{
	OBJ_FILE *file ;
	int no ;

	if( w->sub_type < 4 ) file = obj_dm0c ;
	else if( w->sub_type == 8 ) file = obj_dm10 ;
	else if( w->sub_type == 9 ) file = obj_dm0c ;
	else file = obj_dm0f ;
	no = w->type * 2 ;
	if( !w->routine_0 ) wall_paper_init( w, file, no ) ;
	else if( w->routine_0 == 1 ) wall_paper_scroll( w, file, no ) ;
	else work_free( w ) ;
}

/****************************************************************/
/*			The menu's mark										*/
/****************************************************************/
void dm0c_mark_move( PLAYER_WORK *w )
{
	if( !w->routine_0 ){
		w->routine_0 ++ ;
		w->disp_flag = 1 ;
		w->pos.x = -1.052f ;
		w->pos.y = 2.172f ;
		w->obj = obj_dm0c->obj[83] ;
	}
}

/****************************************************************/
/*			A piece of the menu, each frame, by its kind		*/
/****************************************************************/
void dm0c_menu_move( PLAYER_WORK *w )
{
	dm0c_menu_jmp[w->sub_no]( w ) ;
}

/****************************************************************/
/*			The main menu's pieces								*/
/****************************************************************/
void home_menu_set( void )
{
	int i ;

#ifdef HOST_PORT
	dm0c_netplay_init() ;
#endif
	for( i = 0 ; i < 9 ; i ++ ) dm0c_menu_set( 0, i, 0 ) ;
	dm0c_menu_set( 3, 0, 0 ) ;
	dm0c_menu_set( 4, 0, 0 ) ;
	dm0c_menu_set( 4, 1, 0 ) ;
	dm0c_menu_set( 4, 2, 0 ) ;
	dm0c_menu_set( 6, 0, 0 ) ;
}

/****************************************************************/
/*			The wall paper, by its number: the first has two	*/
/*			layers												*/
/****************************************************************/
void dm0c_wall_paper( int n )
{
	dm0c_menu_set( 5, 0, n ) ;
	if( !n ) dm0c_menu_set( 5, 1, n ) ;
}
