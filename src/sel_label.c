/* The labels on the select screen's assist pillars: in place of the type's name, the name of the
   move the assist type does, asked of the character's own assist routine, with a symbol of the type
   on the side towards the screen's centre. The six labels (two sides, three pillars) are drawn into a
   texture of their own, which the label model is pointed at as it is drawn. */
#include "player.h"
#include "sel_label.h"

extern const char *sel_move_name( Sint32 type, Sint32 sp );
extern int obj_uv_begin( int *src, int *obj );
extern int obj_uv_next( void );
extern int obj_uv_get( Float *x, Float *y );
extern int obj_uv_put( Float *x, Float *y );

#define SEL_LABEL_TEX	0xfe0		/* the labels' common texture */
#define SEL_LABEL_W		256
#define SEL_LABEL_H		512
#define SEL_CELL_H		80			/* each label's rows: the cells run down the texture, a side's three pillars each */
#define SEL_CELLS		6
#define SEL_LABEL_FMT	0x901		/* untwiddled RGB565 */

#define SEL_ABS( n )	( ( n ) < 0 ? -( n ) : ( n ) )
#define RGB565( r, g, b )	( ( Uint16 )( ( ( r ) >> 3 ) << 11 | ( ( g ) >> 2 ) << 5 | ( b ) >> 3 ) )

static Uint16 sel_label_texels[SEL_LABEL_W * SEL_LABEL_H] ;
static NLthd sel_label_thd[2] = {
	{ SEL_LABEL_W, SEL_LABEL_H, SEL_LABEL_FMT, sel_label_texels, 0 },
	{ 0, 0, -1, 0, 0 },
} ;
static Sint16 sel_label_key[SEL_CELLS] ;	/* what each cell shows: its character and step, plus one; 0 for nothing */
static Uint8 sel_label_loaded ;

/* each type's colour, by sel_glow_step_tbl's label */
static const Uint8 sel_type_col[SEL_ICON_TYPES][3] = {
	{ 255, 120,   0 },	/* projectile */
	{ 190, 120,  30 },	/* ground */
	{ 160,  70, 235 },	/* expansion */
	{   0, 190, 160 },	/* dash */
	{  40, 160, 255 },	/* anti-air */
	{ 235,  50, 170 },	/* variety */
	{  80,  60, 210 },	/* throw */
	{ 110, 100, 255 },	/* air throw */
	{  60, 200,  60 },	/* heal */
	{ 250, 200,   0 },	/* enhance */
	{ 220,  30,  90 },	/* capture */
	{ 230,  30,  30 },	/* launcher */
	{ 130, 140, 165 },	/* balance */
} ;

/* the type's name, when the move has none */
static const char *const sel_type_name[SEL_ICON_TYPES] = {
	"Projectile Type", "Ground Type", "Expansion Type", "Dash Type", "Anti-air Type", "Variety Type",
	"Throw Type", "Air Throw Type", "Heal Type", "Enhance Type", "Capture Type", "Launcher Type", "Balance Type",
} ;

/****************************************************************/
/*			The move the character's assist type personal		*/
/*			does (its pl_sp_type), or -1 when it can't be		*/
/*			asked												*/
/****************************************************************/
#if !defined( TARGET_DC ) || defined( HOST_PORT )	/* the home program loads a character's routines only for its players */
#define TS( n )	extern void pl##n##_tbl_set( PLAYER_WORK *pl_w ) ;
TS( 00 ) TS( 01 ) TS( 02 ) TS( 03 ) TS( 04 ) TS( 05 ) TS( 06 ) TS( 07 ) TS( 08 ) TS( 09 ) TS( 0a ) TS( 0b )
TS( 0c ) TS( 0d ) TS( 0e ) TS( 0f ) TS( 10 ) TS( 11 ) TS( 12 ) TS( 13 ) TS( 14 ) TS( 15 ) TS( 16 ) TS( 17 )
TS( 18 ) TS( 19 ) TS( 1a ) TS( 1b ) TS( 1c ) TS( 1d ) TS( 1e ) TS( 1f ) TS( 20 ) TS( 21 ) TS( 22 ) TS( 23 )
TS( 24 ) TS( 25 ) TS( 26 ) TS( 27 ) TS( 28 ) TS( 29 ) TS( 2a ) TS( 2b ) TS( 2c ) TS( 2d ) TS( 2e ) TS( 2f )
TS( 30 ) TS( 31 ) TS( 32 ) TS( 33 ) TS( 34 ) TS( 35 ) TS( 36 ) TS( 37 ) TS( 38 ) TS( 39 ) TS( 3a )
#undef TS
#define TS( n )	pl##n##_tbl_set,
static void ( *const sel_tbl_set[59] )( PLAYER_WORK *pl_w ) = {
	TS( 00 ) TS( 01 ) TS( 02 ) TS( 03 ) TS( 04 ) TS( 05 ) TS( 06 ) TS( 07 ) TS( 08 ) TS( 09 ) TS( 0a ) TS( 0b )
	TS( 0c ) TS( 0d ) TS( 0e ) TS( 0f ) TS( 10 ) TS( 11 ) TS( 12 ) TS( 13 ) TS( 14 ) TS( 15 ) TS( 16 ) TS( 17 )
	TS( 18 ) TS( 19 ) TS( 1a ) TS( 1b ) TS( 1c ) TS( 1d ) TS( 1e ) TS( 1f ) TS( 20 ) TS( 21 ) TS( 22 ) TS( 23 )
	TS( 24 ) TS( 25 ) TS( 26 ) TS( 27 ) TS( 28 ) TS( 29 ) TS( 2a ) TS( 2b ) TS( 2c ) TS( 2d ) TS( 2e ) TS( 2f )
	TS( 30 ) TS( 31 ) TS( 32 ) TS( 33 ) TS( 34 ) TS( 35 ) TS( 36 ) TS( 37 ) TS( 38 ) TS( 39 ) TS( 3a )
} ;
#undef TS

/* a work of its own: the assist routines only set the move, its level and the act, all in the work */
static PLAYER_WORK sel_ask_w ;
static void ( *sel_ask_tbl[28] )( PLAYER_WORK *pl_w ) ;

static Sint32 sel_assist_move( Sint32 type, Sint32 personal )
{
	PLAYER_WORK *w = &sel_ask_w ;

	if( type < 0 || type >= 59 ) return -1 ;
	memset( w, 0, sizeof( PLAYER_WORK ) ) ;
	memset( sel_ask_tbl, 0, sizeof( sel_ask_tbl ) ) ;
	w->pl_tbl = sel_ask_tbl ;
	w->pl_type = type ;
	w->load_type = type ;
	sel_tbl_set[type]( w ) ;
	if( !w->pl_tbl[TBL_SP_ASSIST] ) return -1 ;
	w->pl_personal = personal ;
	w->pl_sp_type = 0xff ;
	w->pl_tbl[TBL_SP_ASSIST]( w ) ;
	return w->pl_sp_type == 0xff ? -1 : w->pl_sp_type ;
}
#else
static Sint32 sel_assist_move( Sint32 type, Sint32 personal )
{
	return -1 ;
}
#endif

/*==============================================================*/
/*			A texel of the label, its rows upside down as the	*/
/*			model reads them									*/
/*==============================================================*/
static Uint16 *sel_texel( Sint32 cell, Sint32 x, Sint32 y )
{
	return &sel_label_texels[( cell * SEL_CELL_H + SEL_CELL_H - 1 - y ) * SEL_LABEL_W + x] ;
}

/* a texel blended towards r, g, b by a, 0 to 255 */
static void sel_blend( Uint16 *t, Sint32 r, Sint32 g, Sint32 b, Sint32 a )
{
	Sint32 r0 = ( *t >> 11 ) << 3, g0 = ( *t >> 5 & 63 ) << 2, b0 = ( *t & 31 ) << 3 ;

	if( a <= 0 ) return ;
	if( a > 255 ) a = 255 ;
	r0 += ( r - r0 ) * a / 255 ;
	g0 += ( g - g0 ) * a / 255 ;
	b0 += ( b - b0 ) * a / 255 ;
	*t = RGB565( r0, g0, b0 ) ;
}

/*==============================================================*/
/*			The plate: white, a lilac sheen along the bottom,	*/
/*			and a thin rim										*/
/*==============================================================*/
static void sel_plate_draw( Sint32 cell )
{
	Sint32 x, y, r, g, b, f ;

	for( y = 0 ; y < SEL_CELL_H ; y ++ ){
		for( x = 0 ; x < SEL_LABEL_W ; x ++ ){
			f = y * 256 / SEL_CELL_H ;				/* down the plate */
			f += ( x - SEL_LABEL_W / 2 ) * 40 / SEL_LABEL_W ;	/* and a slant */
			if( f < 0 ) f = 0 ;
			if( f > 255 ) f = 255 ;
			f = f * f / 255 ;
			r = 255 - f * 60 / 255 ;
			g = 255 - f * 55 / 255 ;
			b = 255 - f * 25 / 255 ;
			if( x < 3 || y < 3 || x >= SEL_LABEL_W - 3 || y >= SEL_CELL_H - 3 ){
				r = 150 ; g = 150 ; b = 185 ;
			}
			*sel_texel( cell, x, y ) = RGB565( r, g, b ) ;
		}
	}
}

/*==============================================================*/
/*			The type's symbol, its shades tinted with the		*/
/*			type's colour										*/
/*==============================================================*/
static void sel_icon_draw( Sint32 cell, Sint32 step, Sint32 x0, Sint32 y0 )
{
	const Uint8 *src = sel_icon_bits[step] ;
	const Uint8 *col = sel_type_col[step] ;
	Sint32 x, y, a, l, r, g, b ;

	for( y = 0 ; y < SEL_ICON_SIZE ; y ++ ){
		for( x = 0 ; x < SEL_ICON_SIZE ; x ++, src ++ ){
			a = ( *src >> 4 ) * 17 ;
			l = *src & 15 ;
			if( !a ) continue ;
			if( l <= 8 ){
				r = col[0] * l / 8 ; g = col[1] * l / 8 ; b = col[2] * l / 8 ;
			} else {
				r = col[0] + ( 255 - col[0] ) * ( l - 8 ) / 7 ;
				g = col[1] + ( 255 - col[1] ) * ( l - 8 ) / 7 ;
				b = col[2] + ( 255 - col[2] ) * ( l - 8 ) / 7 ;
			}
			sel_blend( sel_texel( cell, x0 + x, y0 + y ), r, g, b, a ) ;
		}
	}
}

/*==============================================================*/
/*			Text in the font									*/
/*==============================================================*/
static const SEL_GLYPH *sel_glyph( char c )
{
	if( c < SEL_FONT_FIRST || c >= SEL_FONT_FIRST + SEL_FONT_CHARS ) c = '?' ;
	return &sel_font_glyph[c - SEL_FONT_FIRST] ;
}

/* the width of n characters of s, in quarter texels at full size */
static Sint32 sel_text_w( const char *s, Sint32 n )
{
	Sint32 w = 0 ;

	while( n -- > 0 && *s ) w += sel_glyph( *s ++ )->advance ;
	return w ;
}

/* a glyph's coverage, 0 to 15, at column x and row y of it; 0 outside */
static Sint32 sel_glyph_at( const SEL_GLYPH *gl, Sint32 x, Sint32 y )
{
	Sint32 i ;

	if( x < 0 || y < 0 || x >= gl->width || y >= SEL_FONT_H ) return 0 ;
	i = y * gl->width + x ;
	return sel_font_bits[gl->ofs + ( i >> 1 )] >> ( ( i & 1 ) * 4 ) & 15 ;
}

/* n characters of s with the pen's left at x (texels) and the glyphs' top at y, scaled by sx and sy (in 256ths) */
static void sel_text_draw( Sint32 cell, const char *s, Sint32 n, Sint32 x, Sint32 y, Sint32 sx, Sint32 sy, Sint32 x_min, Sint32 x_max )
{
	Sint32 pen = x * 4 * 256 ;	/* in quarter texels, by 256 */

	for( ; n > 0 && *s ; n --, s ++ ){
		const SEL_GLYPH *gl = sel_glyph( *s ) ;
		Sint32 gx0 = ( pen / 4 + gl->left * sx ) >> 8 ;
		Sint32 gx1 = ( pen / 4 + ( gl->left + gl->width ) * sx + 255 ) >> 8 ;
		Sint32 gy1 = y + ( ( SEL_FONT_H * sy + 255 ) >> 8 ) ;
		Sint32 dx, dy ;

		for( dy = y ; dy < gy1 ; dy ++ ){
			Sint32 v = ( ( dy - y ) * 2 + 1 ) * 128 * 256 / sy - 128 ;	/* the glyph's row, in 256ths, at the texel's centre */
			Sint32 v0 = v >> 8, fv = v & 255 ;

			if( dy < 3 || dy >= SEL_CELL_H - 3 ) continue ;
			for( dx = gx0 ; dx < gx1 ; dx ++ ){
				Sint32 u = ( ( dx * 256 + 128 ) * 4 - pen ) / 4 * 256 / sx - gl->left * 256 - 128 ;
				Sint32 u0 = u >> 8, fu = u & 255, a ;

				if( dx < x_min || dx >= x_max ) continue ;
				a = ( sel_glyph_at( gl, u0, v0 ) * ( 256 - fu ) + sel_glyph_at( gl, u0 + 1, v0 ) * fu ) * ( 256 - fv )
				  + ( sel_glyph_at( gl, u0, v0 + 1 ) * ( 256 - fu ) + sel_glyph_at( gl, u0 + 1, v0 + 1 ) * fu ) * fv ;
				a = a * 17 >> 16 ;
				sel_blend( sel_texel( cell, dx, dy ), 20, 18, 36, a ) ;
			}
		}
		pen += gl->advance * sx ;
	}
}

/*==============================================================*/
/*			The move's name in the room left by the symbol:		*/
/*			one line if it fits, else two, narrowed to fit		*/
/*==============================================================*/
static void sel_name_draw( Sint32 cell, const char *s, Sint32 x0, Sint32 x1 )
{
	Sint32 len = 0, brk = -1, i, lines, w0, w1, wmax, sx, sy, room_w, room_h, line_h, y ;

	while( s[len] ) len ++ ;
	room_w = ( x1 - x0 ) * 4 ;
	room_h = SEL_CELL_H - 12 ;
	w0 = sel_text_w( s, len ) ;
	lines = 1 ;
	if( w0 * 4 > room_w * 5 ){	/* one line while it needs narrowing by no more than a fifth */
		/* the space nearest the middle */
		for( i = 0 ; i < len ; i ++ ){
			if( s[i] != ' ' ) continue ;
			if( brk < 0 || SEL_ABS( sel_text_w( s, i ) * 2 - w0 ) < SEL_ABS( sel_text_w( s, brk ) * 2 - w0 ) ) brk = i ;
		}
		if( brk > 0 ) lines = 2 ;
	}
	if( lines == 2 ){
		w0 = sel_text_w( s, brk ) ;
		w1 = sel_text_w( s + brk + 1, len - brk - 1 ) ;
		wmax = w0 > w1 ? w0 : w1 ;
		line_h = SEL_FONT_H * 7 / 8 ;
	} else {
		w1 = 0 ;
		wmax = w0 ;
		line_h = SEL_FONT_H ;
	}
	sy = room_h * 256 / ( line_h * lines ) ;
	if( sy > 256 ) sy = 256 ;
	sx = wmax > 0 ? room_w * 256 / wmax : 256 ;
	if( sx > sy ) sx = sy ;
	if( sx < sy * 3 / 5 ) sy = sx * 5 / 3 ;	/* narrowed no further than three fifths: smaller instead */
	y = ( SEL_CELL_H - ( ( line_h * lines - ( line_h - SEL_FONT_H ) ) * sy >> 8 ) ) / 2 ;
	if( lines == 2 ){
		sel_text_draw( cell, s, brk, x0 + ( room_w - ( w0 * sx >> 8 ) ) / 8, y, sx, sy, x0 - 4, x1 + 4 ) ;
		sel_text_draw( cell, s + brk + 1, len - brk - 1, x0 + ( room_w - ( w1 * sx >> 8 ) ) / 8, y + ( line_h * sy >> 8 ), sx, sy, x0 - 4, x1 + 4 ) ;
	} else {
		sel_text_draw( cell, s, len, x0 + ( room_w - ( w0 * sx >> 8 ) ) / 8, y, sx, sy, x0 - 4, x1 + 4 ) ;
	}
}

/*==============================================================*/
/*			A cell drawn for side's pillar slot of character	*/
/*			type, whose type is step							*/
/*==============================================================*/
static void sel_label_make( Sint32 cell, Sint32 side, Sint32 slot, Sint32 type, Sint32 step )
{
	const char *name = 0 ;
	Sint32 sp, ix ;

	if( step < 0 || step >= SEL_ICON_TYPES ) step = 0 ;
	sp = sel_assist_move( type, slot ) ;
	if( sp >= 0 ) name = sel_move_name( type, sp ) ;
	if( !name ) name = sel_type_name[step] ;
	sel_plate_draw( cell ) ;
	/* the symbol on the side towards the screen's centre: the right for the first side, the left for the second */
	ix = side ? 7 : SEL_LABEL_W - SEL_ICON_SIZE - 7 ;
	sel_icon_draw( cell, step, ix, ( SEL_CELL_H - SEL_ICON_SIZE ) / 2 ) ;
	if( side ) sel_name_draw( cell, name, ix + SEL_ICON_SIZE + 6, SEL_LABEL_W - 10 ) ;
	else sel_name_draw( cell, name, 10, ix - 6 ) ;
}

/****************************************************************/
/*			Before a pillar's label is drawn: its cell made		*/
/*			for the character, the model's texture coordinates	*/
/*			set from the template model tmpl to the cell, and	*/
/*			its texture the labels'								*/
/****************************************************************/
void sel_label_uv( PLAYER_WORK *w, Sint16 tmpl, Sint32 step )
{
	Sint32 cell = ( w->type & 1 ) * 3 + w->sub_type % 3 ;
	Sint16 key = ( w->pl_type << 4 | step ) + 1 ;
	Float u, v, u0 = 1.0f, u1 = 0.0f, v0 = 1.0f, v1 = 0.0f ;
	Uint8 *p ;

	if( sel_label_key[cell] != key || !sel_label_loaded ){
		sel_label_make( cell, w->type & 1, w->sub_type % 3, w->pl_type, step ) ;
		sel_label_key[cell] = key ;
		nlSetCommonTextureOffset( SEL_LABEL_TEX ) ;
		nlSpriteLoadTextureOnce( sel_label_thd, 0, 0 ) ;
		nlSetCommonTextureOffset( 0 ) ;
		sel_label_loaded = 1 ;
	}
	/* the template's corners, then each point moved into the cell */
	obj_uv_begin( obj_dm01->obj[tmpl], w->obj ) ;
	while( obj_uv_next() == 0 ){
		obj_uv_get( &u, &v ) ;
		if( u < u0 ) u0 = u ;
		if( u > u1 ) u1 = u ;
		if( v < v0 ) v0 = v ;
		if( v > v1 ) v1 = v ;
	}
	if( u1 <= u0 || v1 <= v0 ) return ;
	obj_uv_begin( obj_dm01->obj[tmpl], w->obj ) ;
	while( obj_uv_next() == 0 ){
		obj_uv_get( &u, &v ) ;
		u = ( 0.5f + ( u - u0 ) / ( u1 - u0 ) * ( SEL_LABEL_W - 1 ) ) / SEL_LABEL_W ;
		v = ( cell * SEL_CELL_H + 0.5f + ( v - v0 ) / ( v1 - v0 ) * ( SEL_CELL_H - 1 ) ) / SEL_LABEL_H ;
		obj_uv_put( &u, &v ) ;
	}
	/* each material's texture word: the labels' texture, untwiddled RGB565 */
	for( p = ( Uint8 * )w->obj + 24 ; *( Uint32 * )p != 0 ; p += 80 + *( Sint32 * )( p + 76 ) ){
		Uint32 tcw = *( Uint32 * )( p + 12 ) ;

		tcw = ( tcw & 0x02000000 ) | 1 << 27 | 1 << 26 | SEL_LABEL_TEX ;
		*( Uint32 * )( p + 12 ) = tcw ;
	}
}

/****************************************************************/
/*			The labels drawn again for whoever comes next		*/
/****************************************************************/
void sel_label_reset( void )
{
	Sint32 i ;

	for( i = 0 ; i < SEL_CELLS ; i ++ ) sel_label_key[i] = 0 ;
	sel_label_loaded = 0 ;
}

/*==============================================================*/
/*	End of File.												*/
/*==============================================================*/
/* "sel_label.c" End of File	*/
