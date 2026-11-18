/* fmac_int.c: host_fmac for a host with no double FPU (the PSP), where libgcc's soft doubles cost about ten
   times as much. The same result as ( float )( ( double )a * b + c ): the product exactly, the sum rounded to 53
   bits, then to 24. Most sums are settled in floats (the product's rounding error found by Dekker's split, the
   sum's by Knuth's), when what is left is clearly short of half an ulp; the rest in integers. Denormals,
   infinities, NaNs and results out of the float's normal range take the double's way. */
#include <stdint.h>
#include <string.h>

#if defined( __psp__ )

static float slow( float a, float b, float c ) { return ( float )( ( double )a * b + c ) ; }

static int clz64( uint64_t x ) { return __builtin_clzll( x ) ; }

static float exact( float a, float b, float c )
{
	uint32_t ua, ub, uc, ea, eb, ec, s, sx, sy ;
	uint64_t x, y, r ;
	int ex, ey, d, n ;
	uint32_t rest ;

	memcpy( &ua, &a, 4 ) ; memcpy( &ub, &b, 4 ) ; memcpy( &uc, &c, 4 ) ;
	ea = ua >> 23 & 255 ; eb = ub >> 23 & 255 ; ec = uc >> 23 & 255 ;
	if( ea - 1 > 253 || eb - 1 > 253 || ec - 1 > 253 ) return slow( a, b, c ) ;
	/* the product exactly, its top bit at 61 */
	x = ( uint64_t )( ( ua & 0x7fffff ) | 0x800000 ) * ( ( ub & 0x7fffff ) | 0x800000 ) ;
	ex = ( int )( ea + eb ) - 300 ;
	if( x >> 47 ) { x <<= 14 ; ex -= 14 ; } else { x <<= 15 ; ex -= 15 ; }
	sx = ( ua ^ ub ) >> 31 ;
	y = ( uint64_t )( ( uc & 0x7fffff ) | 0x800000 ) << 38 ;
	ey = ( int )ec - 188 ;
	sy = uc >> 31 ;
	/* x the larger */
	if( ey > ex || ( ey == ex && y > x ) ) {
		uint64_t t = x ; x = y ; y = t ;
		d = ex ; ex = ey ; ey = d ;
		s = sx ; sx = sy ; sy = s ;
	}
	d = ex - ey ;
	if( d >= 63 ) y = y != 0 ;
	else if( d ) y = y >> d | ( ( y << ( 64 - d ) ) != 0 ) ;
	if( sx == sy ) r = x + y ;
	else {
		r = x - y ;
		if( !r ) return 0.0f ;
	}
	/* to 53 bits, nearest even */
	n = clz64( r ) ;
	r <<= n ;
	ex -= n ;
	rest = ( uint32_t )r & 0x7ff ;
	r >>= 11 ;
	ex += 11 ;
	if( rest > 0x400 || ( rest == 0x400 && ( r & 1 ) ) ) {
		r ++ ;
		if( r >> 53 ) { r >>= 1 ; ex ++ ; }
	}
	/* then to 24 bits, nearest even */
	rest = ( uint32_t )r & 0x1fffffff ;
	r >>= 29 ;
	ex += 29 ;
	if( rest > 0x10000000 || ( rest == 0x10000000 && ( r & 1 ) ) ) {
		r ++ ;
		if( r >> 24 ) { r >>= 1 ; ex ++ ; }
	}
	/* value r * 2^ex, r in [2^23, 2^24): biased exponent ex + 150 */
	ex += 150 ;
	if( ex < 1 || ex > 254 ) return slow( a, b, c ) ;
	ua = sx << 31 | ( uint32_t )ex << 23 | ( ( uint32_t )r & 0x7fffff ) ;
	memcpy( &a, &ua, 4 ) ;
	return a ;
}

static uint32_t bits( float f ) { uint32_t u ; memcpy( &u, &f, 4 ) ; return u ; }

static float of_bits( uint32_t u ) { float f ; memcpy( &f, &u, 4 ) ; return f ; }

/* half x's ulp, or a quarter where x is a power of two (the ulp below it being half the one above) */
static float half_ulp( float x )
{
	uint32_t u = bits( x ) & 0x7fffffff ;
	return of_bits( ( ( u >> 23 ) - 24 - ( ( u & 0x7fffff ) == 0 ) ) << 23 ) ;
}

/* exponents where the splits and sums below can't overflow or leave the normal floats */
static int in_range( float x ) { return ( bits( x ) >> 23 & 255 ) - 40 <= 174 ; }

float host_fmac( float a, float b, float c )
{
	const float margin = 1.0f - 1.0f / 1048576 ;
	uint32_t ua = bits( a ), ub = bits( b ), uc = bits( c ) ;
	uint32_t ea = ua >> 23 & 255, eb = ub >> 23 & 255, ec = uc >> 23 & 255 ;
	int ep = ( int )( ea + eb ) - 127 ;
	float p, s, t, e, h, ah, al, bh, bl, err, u, v, r, w ;

	if( !( ua & 0x7fffffff ) || !( ub & 0x7fffffff ) ) {
		/* a zero product: c, or a zero signed as the double's sum */
		if( ea == 255 || eb == 255 || ec == 255 || ( ec == 0 && ( uc & 0x7fffff ) ) ) return exact( a, b, c ) ;
		if( uc & 0x7fffffff ) return c ;
		return of_bits( ( ua ^ ub ) & uc & 0x80000000 ) ;
	}
	if( ea - 27 > 200 || eb - 27 > 200 || ep < 67 || ep > 187 ) return exact( a, b, c ) ;
	p = a * b ;
	if( !( uc & 0x7fffffff ) ) return p ;		/* the product alone, rounded once */
	if( ec - 40 > 174 ) return exact( a, b, c ) ;
	/* s + e = p + c exactly; the product's error is at most half p's ulp */
	s = p + c ;
	t = s - p ;
	e = ( p - ( s - t ) ) + ( c - t ) ;
	if( !in_range( s ) ) return exact( a, b, c ) ;
	h = half_ulp( s ) ;
	err = of_bits( ( bits( p ) & 0x7f800000 ) - ( 24u << 23 ) ) ;
	if( err < h && __builtin_fabsf( e ) <= ( h - err ) * margin ) return s ;
	/* the product's error exactly: a * b = p + err */
	t = 4097.0f * a ; ah = t - ( t - a ) ; al = a - ah ;
	t = 4097.0f * b ; bh = t - ( t - b ) ; bl = b - bh ;
	err = ( ( ah * bh - p ) + ah * bl + al * bh ) + al * bl ;
	/* a * b + c = r + w + v exactly */
	u = e + err ; t = u - e ; v = ( e - ( u - t ) ) + ( err - t ) ;
	r = s + u ; t = r - s ; w = ( s - ( r - t ) ) + ( u - t ) ;
	if( r != 0 && in_range( r ) ) {
		h = half_ulp( r ) ;
		if( __builtin_fabsf( w ) <= h * margin && __builtin_fabsf( v ) <= h * ( 1.0f / 2097152 ) ) return r ;
	}
	return exact( a, b, c ) ;
}

#endif
